// SPDX-License-Identifier: Apache-2.0
#include "srvModemTest_nrf93m1.h"
#include "event_bus.h"
#include "thread.hpp"

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/printk.h>
#include <zephyr/zbus/zbus.h>
#include <errno.h>

#ifdef CONFIG_FIREFLY_MODEM_TEST_NRF93M1
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/pm/device_runtime.h>
#include <zephyr/sys/ring_buffer.h>
#include <string.h>
#endif

namespace {

#ifdef CONFIG_FIREFLY_MODEM_TEST_NRF93M1

#define MODEM_NODE DT_ALIAS(modem)
#define MODEM_UART_NODE DT_PARENT(MODEM_NODE)
const struct device *const kUart = DEVICE_DT_GET(MODEM_UART_NODE);
const struct gpio_dt_spec kPower = GPIO_DT_SPEC_GET(MODEM_NODE, mdm_power_gpios);
#if DT_NODE_EXISTS(DT_NODELABEL(modem_dcdc))
const struct device *const kDomain = DEVICE_DT_GET(DT_NODELABEL(modem_dcdc));
#endif

constexpr int64_t kPollMs = 2;
constexpr size_t kRxSize = 2048;
constexpr size_t kUartRxBufferSize = 256;
constexpr size_t kUartRxBufferCount = 4;
constexpr size_t kMarkCount = 9;
enum Mark : size_t { RF_ON, REGISTERED, ATTACHED, SOCK_READY, SENT, REPLIED, RX_DONE, DETACHED, DEREG };
const char *const kMarkNames[kMarkCount] = {
    "rf_on", "registered", "attached", "sock_ready", "sent", "replied", "rx_done", "detached", "dereg"
};

struct RunState {
    int64_t t0;
    int64_t mark[kMarkCount];
    bool valid[kMarkCount];
    bool sent_ok;
    bool reply;
    bool send_attempted;
    bool reply_attempted;
};

RING_BUF_DECLARE(g_uart_rx_ring, 4096);
uint8_t g_uart_rx_buffers[kUartRxBufferCount][kUartRxBufferSize];
size_t g_next_uart_rx_buffer = 1;

void uart_event_handler(const struct device *dev, struct uart_event *event, void *)
{
    switch (event->type) {
    case UART_RX_RDY:
        ring_buf_put(&g_uart_rx_ring, event->data.rx.buf + event->data.rx.offset,
                     event->data.rx.len);
        break;
    case UART_RX_BUF_REQUEST: {
        uint8_t *next = g_uart_rx_buffers[g_next_uart_rx_buffer];
        g_next_uart_rx_buffer = (g_next_uart_rx_buffer + 1) % kUartRxBufferCount;
        (void)uart_rx_buf_rsp(dev, next, kUartRxBufferSize);
        break;
    }
    default:
        break;
    }
}

double seconds(int64_t ms) { return static_cast<double>(ms) / 1000.0; }

void set_mark(RunState &s, Mark mark)
{
    s.mark[mark] = k_uptime_get();
    s.valid[mark] = true;
}

void uart_write(const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; ++i) { uart_poll_out(kUart, data[i]); }
}

void uart_write(const char *text) { uart_write(reinterpret_cast<const uint8_t *>(text), strlen(text)); }

void flush_rx()
{
    ring_buf_reset(&g_uart_rx_ring);
}

bool contains(const char *haystack, const char *needle) { return strstr(haystack, needle) != nullptr; }

bool registered_urc(const char *text)
{
    const char *p = text;
    while ((p = strstr(p, "+CEREG:")) != nullptr) {
        p += strlen("+CEREG:");
        while (*p == ' ' || *p == '\t') { ++p; }
        if ((*p == '1' || *p == '5') && (p[1] == '\r' || p[1] == '\n' || p[1] == ',')) {
            return true;
        }
    }
    return false;
}

bool read_until(RunState &s, const char *expected, int32_t timeout_ms,
                bool cereg_pattern = false)
{
    char buffer[kRxSize]{};
    char line[256]{};
    size_t used = 0;
    size_t line_used = 0;
    int64_t first = -1;
    const int64_t start = k_uptime_get();
    const int64_t deadline = start + timeout_ms;

    while (k_uptime_get() < deadline) {
        uint8_t byte;
        if (ring_buf_get(&g_uart_rx_ring, &byte, 1) != 1) {
            k_sleep(K_MSEC(kPollMs));
            continue;
        }
        if (first < 0 && byte != '\r' && byte != '\n') { first = k_uptime_get(); }
        if (used + 1 < sizeof(buffer)) {
            buffer[used++] = static_cast<char>(byte);
            buffer[used] = '\0';
        }
        if (byte == '\r' || byte == '\n') {
            if (line_used != 0) {
                line[line_used] = '\0';
                printk("    RX [%8.4f] %s\n", seconds(k_uptime_get() - s.t0), line);
                line_used = 0;
            }
        } else {
            if (line_used + 1 < sizeof(line)) { line[line_used++] = static_cast<char>(byte); }
        }
        if ((cereg_pattern && registered_urc(buffer)) ||
            (!cereg_pattern && expected != nullptr && contains(buffer, expected))) {
            if (line_used != 0) {
                line[line_used] = '\0';
                printk("    RX [%8.4f] %s\n", seconds(k_uptime_get() - s.t0), line);
            }
            return true;
        }
    }
    if (line_used != 0) {
        line[line_used] = '\0';
        printk("    RX [%8.4f] %s\n", seconds(k_uptime_get() - s.t0), line);
    }
    ARG_UNUSED(first);
    return false;
}

int send_at(RunState &s, const char *command, const char *expected = "OK",
            int32_t timeout_ms = 10000, bool required = true)
{
    const int64_t tx = k_uptime_get();
    printk("  TX [%8.4f] %s\n", seconds(tx - s.t0), command);
    uart_write(command);
    uart_write("\r");
    const bool matched = read_until(s, expected, timeout_ms);
    const int64_t end = k_uptime_get();
    printk("     -> %s | complete %7.1f ms\n", matched ? "match" : "MISS ",
           static_cast<double>(end - tx));
    if (required && !matched) {
        printk("Expected '%s' not received.\n", expected);
        return -ETIMEDOUT;
    }
    return matched ? 0 : -ETIMEDOUT;
}

int wait_for_sim_ready(RunState &s)
{
    constexpr int kAttempts = 20;
    printk("\n=== Waiting for SIM ===\n");
    for (int attempt = 1; attempt <= kAttempts; ++attempt) {
        const int err = send_at(s, "AT+CPIN?", "READY", 500, false);
        if (err == 0) { return 0; }
        printk("  SIM not ready (attempt %d/%d), retrying...\n", attempt, kAttempts);
        k_sleep(K_MSEC(500));
    }
    printk("SIM did not become ready\n");
    return -ETIMEDOUT;
}

void report(const RunState &s)
{
    struct Row { const char *label; Mark a; Mark b; };
    static const Row rows[] = {
        {"RF on -> registered", RF_ON, REGISTERED},
        {"registered -> PDP attached", REGISTERED, ATTACHED},
        {"attached -> socket ready", ATTACHED, SOCK_READY},
        {"socket ready -> payload sent", SOCK_READY, SENT},
        {"--- uplink path total ---", RF_ON, SENT},
        {"sent -> server reply (RTT)", SENT, REPLIED},
        {"rx wait window", SENT, RX_DONE},
        {"rx wait end -> detached", RX_DONE, DETACHED},
        {"detached -> deregistered", DETACHED, DEREG},
        {"--- teardown total ---", RX_DONE, DEREG},
    };
    printk("\n====================================================================\n");
    printk("PHASE BREAKDOWN\n");
    printk("====================================================================\n");
    for (const auto &row : rows) {
        if (s.valid[row.a] && s.valid[row.b]) {
            printk("%-34s %8.3f s\n", row.label, seconds(s.mark[row.b] - s.mark[row.a]));
        } else {
            printk("%-34s %8s%s\n", row.label, "n/a",
                   row.b == REPLIED && !s.reply ? "   <- no %SKTRECV, nothing to measure" : "");
        }
    }
    printk("--------------------------------------------------------------------\n");
    if (s.valid[RF_ON] && s.valid[DEREG]) {
        const double total = seconds(s.mark[DEREG] - s.mark[RF_ON]);
        printk("%-34s %8.3f s\n", "CONNECTION WINDOW (RF on->dereg)", total);
        if (s.valid[SENT] && s.valid[RX_DONE]) {
            const double wait = seconds(s.mark[RX_DONE] - s.mark[SENT]);
            printk("%-34s %8.3f s   <== use this for comparison\n",
                   s.reply ? "  minus rx wait (real RTT)" : "  minus rx wait (idle timeout)",
                   total - wait);
        }
    }
    printk("====================================================================\n");
    if (s.send_attempted && !s.sent_ok) {
        printk("WARNING: modem never returned OK after the payload - send unconfirmed.\n");
    }
    if (s.reply_attempted && !s.reply) {
        printk("WARNING: no server reply within %.1f s. The 'rx wait window'\n",
               seconds(CONFIG_FIREFLY_MODEM_TEST_RX_WAIT_MS));
        printk("         above is the timeout constant, NOT a round-trip time.\n");
        printk("         It is excluded from the comparison figure.\n");
    }
    printk("CSV,");
    for (size_t i = 0; i < kMarkCount; ++i) {
        if (s.valid[i]) { printk("%.4f", seconds(s.mark[i] - s.t0)); }
        if (i + 1 < kMarkCount) { printk(","); }
    }
    printk("\n");
}

int prepare_hardware()
{
    if (!device_is_ready(kUart) || !gpio_is_ready_dt(&kPower)) { return -ENODEV; }
    int err = 0;
#if DT_NODE_EXISTS(DT_NODELABEL(modem_dcdc))
    err = pm_device_runtime_get(kDomain);
    if (err != 0 && err != -EALREADY) { return err; }
#endif
    err = gpio_pin_configure_dt(&kPower, GPIO_OUTPUT_INACTIVE);
    if (err != 0) { return err; }
    struct uart_config config = {
        .baudrate = 115200,
        .parity = UART_CFG_PARITY_NONE,
        .stop_bits = UART_CFG_STOP_BITS_1,
        .data_bits = UART_CFG_DATA_BITS_8,
        .flow_ctrl = UART_CFG_FLOW_CTRL_RTS_CTS,
    };
    err = uart_configure(kUart, &config);
    if (err != 0) { return err; }
    err = pm_device_runtime_get(kUart);
    if (err != 0 && err != -EALREADY) { return err; }
    err = uart_callback_set(kUart, uart_event_handler, nullptr);
    if (err != 0) { return err; }
    ring_buf_reset(&g_uart_rx_ring);
    g_next_uart_rx_buffer = 1;
    err = uart_rx_enable(kUart, g_uart_rx_buffers[0], kUartRxBufferSize, 1000);
    if (err != 0) { return err; }
    gpio_pin_set_dt(&kPower, 1);
    k_sleep(K_MSEC(100));
    gpio_pin_set_dt(&kPower, 0);
    k_sleep(K_SECONDS(5));
    flush_rx();
    return 0;
}

int run_test()
{
    RunState s{};
    s.t0 = k_uptime_get();
    int err = 0;
#define REQUIRED(expr) do { err = (expr); if (err != 0) { goto done; } } while (0)
    printk("\n=== Setup (untimed) ===\n");
    REQUIRED(send_at(s, "AT"));
    REQUIRED(send_at(s, "AT+CMEE=1"));
    REQUIRED(send_at(s, "AT+CFUN=4"));
    REQUIRED(wait_for_sim_ready(s));
    REQUIRED(send_at(s, "AT%SKTDELETE=all"));
    {
        char command[96];
        snprintk(command, sizeof(command), "AT+CGDCONT=1,\"IP\",\"%s\"",
                 CONFIG_FIREFLY_MODEM_TEST_APN);
        REQUIRED(send_at(s, command));
    }
    REQUIRED(send_at(s, "AT+COPS=0"));
    REQUIRED(send_at(s, "AT+CEREG=2"));

    printk("\n=== RF on ===\n");
    set_mark(s, RF_ON);
    REQUIRED(send_at(s, "AT+CFUN=1"));

    printk("\n=== Waiting for +CEREG URC ===\n");
    if (!read_until(s, nullptr, CONFIG_FIREFLY_MODEM_TEST_REG_TIMEOUT_S * 1000, true)) {
        printk("Network registration timed out\n");
        err = -ETIMEDOUT;
        goto done;
    }
    set_mark(s, REGISTERED);
    printk("  registered at %.4f s (+%.3f s after RF on)\n",
           seconds(s.mark[REGISTERED] - s.t0), seconds(s.mark[REGISTERED] - s.mark[RF_ON]));

    printk("\n=== Attach ===\n");
    REQUIRED(send_at(s, "AT+CGATT=1"));
    set_mark(s, ATTACHED);

    printk("\n=== Socket ===\n");
    REQUIRED(send_at(s, "AT%SKTCREATE=1,2,17"));
    {
        char command[96];
        snprintk(command, sizeof(command), "AT%%SKTCONNECT=0,\"%s\",%d",
                 CONFIG_FIREFLY_MODEM_TEST_SERVER_IP, CONFIG_FIREFLY_MODEM_TEST_SERVER_PORT);
        REQUIRED(send_at(s, command));
    }
    set_mark(s, SOCK_READY);

    printk("\n=== Send ===\n");
    s.send_attempted = true;
    REQUIRED(send_at(s, "AT%SKTSENDT=0", ">"));
    {
        static const char message[] = CONFIG_FIREFLY_MODEM_TEST_MESSAGE;
        static const char hex[] = "0123456789abcdef";
        printk("  TX(DATA) %u bytes -> %u hex chars\n", static_cast<unsigned>(sizeof(message) - 1),
               static_cast<unsigned>(2 * (sizeof(message) - 1)));
        for (size_t i = 0; i < sizeof(message) - 1; ++i) {
            const uint8_t c = static_cast<uint8_t>(message[i]);
            uart_poll_out(kUart, hex[c >> 4]);
            uart_poll_out(kUart, hex[c & 0xf]);
        }
        uart_poll_out(kUart, 0x1a);
    }
    s.sent_ok = read_until(s, "OK", 10000);
    set_mark(s, SENT);
    if (!s.sent_ok) { printk("  WARNING: no OK after payload - modem did not confirm the send\n"); }

    printk("\n=== Waiting up to %.0f s for %%SKTRECV ===\n",
           seconds(CONFIG_FIREFLY_MODEM_TEST_RX_WAIT_MS));
    s.reply_attempted = true;
    s.reply = read_until(s, "SKTRECV", CONFIG_FIREFLY_MODEM_TEST_RX_WAIT_MS);
    set_mark(s, RX_DONE);
    if (s.reply) {
        set_mark(s, REPLIED);
        printk("  reply received, RTT %.1f ms\n", static_cast<double>(s.mark[RX_DONE] - s.mark[SENT]));
    } else {
        printk("  NO REPLY -- waited %.3f s, which is just RX_WAIT (%.1f s).\n",
               seconds(s.mark[RX_DONE] - s.mark[SENT]), seconds(CONFIG_FIREFLY_MODEM_TEST_RX_WAIT_MS));
        printk("  This is a timeout, not a measurement. Excluded from the total.\n");
    }

    printk("\n=== Teardown ===\n");
    REQUIRED(send_at(s, "AT+CGATT=0"));
    set_mark(s, DETACHED);
    REQUIRED(send_at(s, "AT+COPS=2"));
    set_mark(s, DEREG);

    printk("\n=== Verify (untimed) ===\n");
    REQUIRED(send_at(s, "AT+CEREG?"));
    REQUIRED(send_at(s, "AT+CGATT?"));
done:
    report(s);
    return err;
#undef REQUIRED
}

#endif

class SrvModemTest final : public firefly::Thread<CONFIG_FIREFLY_MODEM_TEST_STACK_SIZE> {
public:
    SrvModemTest() : Thread(5, "srvModemTest") {}
    int startTest(const struct zbus_observer *subscriber)
    {
        if (subscriber == nullptr || subscriber->type != ZBUS_OBSERVER_MSG_SUBSCRIBER_TYPE) {
            return -EINVAL;
        }
        if (isRunning()) { return -EALREADY; }
#ifdef CONFIG_FIREFLY_MODEM_TEST_NRF93M1
        const int err = prepare_hardware();
        if (err != 0) { return err; }
#endif
        subscriber_ = subscriber;
        return start();
    }
    int lastResult() const { return has_result_ ? last_result_ : -ENODATA; }
    uint32_t count() const { return static_cast<uint32_t>(atomic_get(&processed_)); }
protected:
    void run() override
    {
        printk("srvModemTest_nrf93m1 ready - waiting for button0\n");
        while (!shouldStop()) {
            const struct zbus_channel *channel = nullptr;
            struct bus_msg message{};
            const int rc = zbus_sub_wait_msg(subscriber_, &channel, &message, K_MSEC(50));
            if (rc == -ENOMSG || rc == -EAGAIN) { continue; }
            if (rc != 0 || channel != &bus_chan) { continue; }
            if (message.kind != BUS_MSG_BUTTON || !bus_button_is_valid(&message.data.button) ||
                message.data.button.button != 0 || !message.data.button.pressed) { continue; }
#ifdef CONFIG_FIREFLY_MODEM_TEST_NRF93M1
            last_result_ = run_test();
#else
            last_result_ = -ENOSYS;
#endif
            has_result_ = true;
            atomic_inc(&processed_);
        }
    }
private:
    const struct zbus_observer *subscriber_ = nullptr;
    int last_result_ = 0;
    bool has_result_ = false;
    atomic_t processed_{0};
};

SrvModemTest g_test;
}

extern "C" int srvModemTest_nrf93m1_start(const struct zbus_observer *subscriber)
{
    return g_test.startTest(subscriber);
}
extern "C" int srvModemTest_nrf93m1_stop(void) { return g_test.stop(); }
extern "C" bool srvModemTest_nrf93m1_is_running(void) { return g_test.isRunning(); }
extern "C" int srvModemTest_nrf93m1_last_result(void) { return g_test.lastResult(); }
extern "C" uint32_t srvModemTest_nrf93m1_processed_count(void) { return g_test.count(); }
