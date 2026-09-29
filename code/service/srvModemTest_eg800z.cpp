// SPDX-License-Identifier: Apache-2.0
//
// Raw-AT counterpart to srvModemTest_nrf93m1.cpp, built to be directly
// comparable to it: same power-up/ready-poll/round/power-down shape, same
// report() layout, same button1-vs-button2 split. See srvModemTest_eg800z.h
// for the button mapping and why button2's round exercises DTR-gated sleep
// instead of PSM (this module's documented AT interface has no AT+CPSMS).
//
// UNVERIFIED AREA: the TCP/IP socket commands below (AT+QIACT/QIOPEN/
// QISEND/QICLOSE) follow Quectel's common "AT+QI*" convention used across
// most of its LTE module line, but the only AT command manual available
// for this exact module family (see cellular_quectel_eg800z.c) does not
// document a TCP/IP command set at all - Quectel ships that as a separate
// "TCP(IP) AT Commands Manual" per module family, not present in the
// project's reference docs. Everything else here (power/CFUN/CEREG/CGATT/
// COPS/CPIN/QPOWD/QSCLK) is confirmed against that manual. Socket steps are
// sent as non-fatal (the round continues and reports what happened rather
// than aborting) specifically because of this uncertainty - check the
// transcript against real hardware and fix the exact syntax here first if
// the socket steps MISS.
#include "srvModemTest_eg800z.h"
#include "event_bus.h"
#include "thread.hpp"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/printk.h>
#include <zephyr/zbus/zbus.h>
#include <errno.h>

#ifdef CONFIG_FIREFLY_MODEM_TEST_EG800Z
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/pm/device_runtime.h>
#include <zephyr/sys/ring_buffer.h>
#include <string.h>
#endif

LOG_MODULE_REGISTER(srvModemTestEg800z, CONFIG_FIREFLY_LOG_LEVEL);

namespace {

#ifdef CONFIG_FIREFLY_MODEM_TEST_EG800Z

#define MODEM_NODE DT_ALIAS(modem)
#define MODEM_UART_NODE DT_PARENT(MODEM_NODE)
const struct device *const kUart = DEVICE_DT_GET(MODEM_UART_NODE);
const struct gpio_dt_spec kPower = GPIO_DT_SPEC_GET(MODEM_NODE, mdm_power_gpios);
const struct gpio_dt_spec kReset = GPIO_DT_SPEC_GET(MODEM_NODE, mdm_reset_gpios);
const struct gpio_dt_spec kDtr = GPIO_DT_SPEC_GET(MODEM_NODE, mdm_dtr_gpios);

constexpr int64_t kPollMs = 2;
constexpr int kSleepCycleCount = 2;
constexpr int kSleepIdleSeconds = 60;
constexpr size_t kRxSize = 2048;
constexpr size_t kUartRxBufferSize = 256;
constexpr size_t kUartRxBufferCount = 4;
constexpr size_t kMarkCount = 11;
enum Mark : size_t {
    MODEM_READY, RF_ON, REGISTERED, ATTACHED, SOCK_READY, SENT, REPLIED, RX_DONE, DETACHED, DEREG,
    POWERED_DOWN
};
const char *const kMarkNames[kMarkCount] = {
    "modem_ready", "rf_on", "registered", "attached", "sock_ready", "sent", "replied", "rx_done",
    "detached", "dereg", "powered_down"
};

// This service's own belief about modem power state - PWRKEY is a
// momentary pulse, not a level-controlled enable line, so there is no pin
// state to read back and confirm against. Set true once power_up_modem()'s
// pulse completes, false again once a round's shutdown (AT+QPOWD, or the
// PWRKEY-pulse fallback) completes.
bool g_modem_powered = false;

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

// Matches both the spontaneous-URC form ("+CEREG: <stat>") and the
// AT+CEREG? query-response form ("+CEREG: <n>,<stat>[,tac,ci,act]") - the
// query response has an extra leading <n> (URC reporting mode) field
// before <stat> that the URC form doesn't have. Needed because the
// reference AT_commands_send_UDP_test.ini test script for this module
// reads registration status by *polling* AT+CEREG? rather than waiting on
// a spontaneous URC - the URC-only pattern this used to be would silently
// never match a query response with stat=1/5 in the second field.
bool cereg_status_registered(const char *text)
{
    const char *p = text;
    while ((p = strstr(p, "+CEREG:")) != nullptr) {
        p += strlen("+CEREG:");
        while (*p == ' ' || *p == '\t') { ++p; }
        const char *field = p;
        while (*p >= '0' && *p <= '9') { ++p; }
        if (p == field) { continue; }
        if (*p == ',') {
            ++p;
            field = p;
            while (*p >= '0' && *p <= '9') { ++p; }
            if (p == field) { continue; }
        }
        if (p - field == 1 && (*field == '1' || *field == '5')) { return true; }
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
        if ((cereg_pattern && cereg_status_registered(buffer)) ||
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

// Actively polls AT+CEREG?/AT+CSQ rather than only waiting on a
// spontaneous +CEREG URC - the reference AT_commands_send_UDP_test.ini
// test script for this module reads registration status this way, and a
// silent multi-minute wait for a URC that may never come gives no signal
// as to *why* (no antenna/coverage vs. genuinely still searching vs.
// something else). Each poll's own response text still flows through the
// same read_until() call that's watching for stat 1/5, so a registration
// that happens to complete between polls is caught immediately rather
// than waiting for the next poll interval.
int wait_for_registration(RunState &s)
{
    printk("\n=== Waiting for registration ===\n");
    constexpr int32_t kPollWindowMs = 5000;
    const int64_t deadline = k_uptime_get() + CONFIG_FIREFLY_MODEM_TEST_REG_TIMEOUT_S * 1000;
    while (k_uptime_get() < deadline) {
        printk("  TX [%8.4f] AT+CEREG?\n", seconds(k_uptime_get() - s.t0));
        uart_write("AT+CEREG?\r");
        printk("  TX [%8.4f] AT+CSQ\n", seconds(k_uptime_get() - s.t0));
        uart_write("AT+CSQ\r");
        if (read_until(s, nullptr, kPollWindowMs, true)) { return 0; }
    }
    printk("Network registration timed out\n");
    return -ETIMEDOUT;
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
        {"modem ready -> RF on (setup)", MODEM_READY, RF_ON},
        {"RF on -> registered", RF_ON, REGISTERED},
        {"registered -> PDP attached", REGISTERED, ATTACHED},
        {"attached -> socket ready", ATTACHED, SOCK_READY},
        {"socket ready -> payload sent", SOCK_READY, SENT},
        {"--- uplink path total ---", RF_ON, SENT},
        {"sent -> server reply (RTT)", SENT, REPLIED},
        {"rx wait window", SENT, RX_DONE},
        {"rx wait end -> detached", RX_DONE, DETACHED},
        {"detached -> deregistered", DETACHED, DEREG},
        {"deregistered -> powered down", DEREG, POWERED_DOWN},
        {"--- teardown total ---", RX_DONE, POWERED_DOWN},
    };
    printk("\n====================================================================\n");
    printk("PHASE BREAKDOWN\n");
    printk("====================================================================\n");
    for (const auto &row : rows) {
        if (s.valid[row.a] && s.valid[row.b]) {
            printk("%-34s %8.3f s\n", row.label, seconds(s.mark[row.b] - s.mark[row.a]));
        } else {
            printk("%-34s %8s%s\n", row.label, "n/a",
                   row.b == REPLIED && !s.reply ? "   <- no reply, nothing to measure" : "");
        }
    }
    printk("--------------------------------------------------------------------\n");
    if (s.valid[POWERED_DOWN]) {
        const double total = seconds(s.mark[POWERED_DOWN] - s.t0);
        printk("%-34s %8.3f s\n", "CONNECTION WINDOW (power up->powered down)", total);
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
    if (!device_is_ready(kUart) || !gpio_is_ready_dt(&kPower) || !gpio_is_ready_dt(&kReset) ||
        !gpio_is_ready_dt(&kDtr)) {
        return -ENODEV;
    }
    int err = gpio_pin_configure_dt(&kPower, GPIO_OUTPUT_INACTIVE);
    if (err != 0) { return err; }
    // RESET_N is a last-resort recovery pin per the hardware design guide,
    // not part of normal power-on/off - configured here but never pulsed
    // by the normal test flow below.
    err = gpio_pin_configure_dt(&kReset, GPIO_OUTPUT_INACTIVE);
    if (err != 0) { return err; }
    // Start DTR asserted (physically low, given GPIO_ACTIVE_LOW in the
    // devicetree) - "stay awake" - so the module doesn't try to sleep
    // mid-round outside the sleep-cycle test's deliberate windows.
    err = gpio_pin_configure_dt(&kDtr, GPIO_OUTPUT_ACTIVE);
    if (err != 0) { return err; }
    struct uart_config config = {
        .baudrate = 115200,
        .parity = UART_CFG_PARITY_NONE,
        .stop_bits = UART_CFG_STOP_BITS_1,
        .data_bits = UART_CFG_DATA_BITS_8,
        // RTS/CTS are wired on this carrier board - the driver-equivalent
        // init script sends AT+IFC=2,2 to tell the module the same thing;
        // see run_test()/run_sleep_cycle_test().
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
    return 0;
}

// Modem-off -> modem-on PWRKEY pulse. Deliberately NOT run from
// prepare_hardware() (which only runs once, at service start/boot) - it
// runs at the top of each round instead, so the modem stays fully
// unpowered from srvModemTest_eg800z_start() until the first button press.
// PWRKEY is active-low (GPIO_ACTIVE_LOW in the devicetree); the hardware
// design guide documents >=500ms to power on using this same pin, and
// power_down_modem() below reuses the same pulse shape (>=650ms) for its
// GPIO fallback - 650ms safely clears both thresholds. No fixed settle
// delay here - wait_for_modem_ready() polls until the modem actually
// answers instead of blindly sleeping a worst-case boot time.
void power_up_modem()
{
    gpio_pin_set_dt(&kPower, 1);
    k_sleep(K_MSEC(650));
    gpio_pin_set_dt(&kPower, 0);
    g_modem_powered = true;
    flush_rx();
}

// Polls "AT" until the modem answers "OK" instead of a fixed post-pulse
// sleep - the EG800Z's own datasheets disagree on exact boot time (~10s in
// one timing diagram, ~17-27s in a sibling module's more detailed one), so
// a fixed delay would either waste time on the common case or race on the
// slow one. Mirrors wait_for_sim_ready()'s retry-loop shape.
int wait_for_modem_ready(RunState &s)
{
    printk("\n=== Waiting for modem ready ===\n");
    const int64_t deadline = k_uptime_get() + CONFIG_FIREFLY_MODEM_TEST_READY_TIMEOUT_MS;
    while (k_uptime_get() < deadline) {
        if (send_at(s, "AT", "OK", 300, false) == 0) {
            set_mark(s, MODEM_READY);
            return 0;
        }
        k_sleep(K_MSEC(200));
    }
    printk("Modem did not respond to AT within timeout\n");
    return -ETIMEDOUT;
}

// Modem-on -> modem-off. Tries the module's own software shutdown command
// first (AT+QPOWD=1, returns OK immediately, then outputs "POWERED DOWN"
// once deregistration completes - documented as taking up to 60s); if that
// isn't confirmed within the timeout, falls back to the same PWRKEY pulse
// used to power the modem up, held long enough (>=650ms) to trigger the
// power-off action instead of a power-on one.
int power_down_modem(RunState &s)
{
    int result;
    if (send_at(s, "AT+QPOWD=1", "POWERED DOWN", 65000, false) == 0) {
        set_mark(s, POWERED_DOWN);
        g_modem_powered = false;
        result = 0;
    } else {
        printk("  AT+QPOWD=1 not confirmed - falling back to PWRKEY power-off pulse\n");
        const int err1 = gpio_pin_set_dt(&kPower, 1);
        k_sleep(K_MSEC(650));
        const int err2 = gpio_pin_set_dt(&kPower, 0);
        g_modem_powered = false;
        if (err1 != 0 || err2 != 0) {
            printk("  PWRKEY power-off pulse failed too - modem shutdown NOT confirmed\n");
            result = -EIO;
        } else {
            set_mark(s, POWERED_DOWN);
            result = 0;
        }
    }
    return result;
}

// One round's socket work: open a UDP socket, send the test payload, wait
// for a reply, then close it. See the file header comment - the exact
// AT+QI* syntax here is a best-effort guess (no TCP/IP AT command manual
// was available for this module), sent non-fatally so the round completes
// and reports what actually happened either way.
void socket_round(RunState &s)
{
    printk("\n=== Socket ===\n");
    send_at(s, "AT+QIACT=1", "OK", 5000, false);
    {
        char command[128];
        snprintk(command, sizeof(command), "AT+QIOPEN=1,0,\"UDP\",\"%s\",%d,0,0",
                 CONFIG_FIREFLY_MODEM_TEST_SERVER_IP, CONFIG_FIREFLY_MODEM_TEST_SERVER_PORT);
        if (send_at(s, command, "+QIOPEN:", 10000, false) != 0) {
            printk("  WARNING: no +QIOPEN response - socket open unconfirmed\n");
        }
    }
    set_mark(s, SOCK_READY);

    printk("\n=== Send ===\n");
    s.send_attempted = true;
    static const char message[] = CONFIG_FIREFLY_MODEM_TEST_MESSAGE;
    {
        char command[32];
        snprintk(command, sizeof(command), "AT+QISEND=0,%u",
                 static_cast<unsigned>(sizeof(message) - 1));
        send_at(s, command, ">", 5000, false);
    }
    uart_write(message);
    uart_poll_out(kUart, 0x1a);
    s.sent_ok = read_until(s, "SEND OK", 10000);
    set_mark(s, SENT);
    if (!s.sent_ok) { printk("  WARNING: no SEND OK after payload - modem did not confirm the send\n"); }

    printk("\n=== Waiting up to %.0f s for a reply ===\n",
           seconds(CONFIG_FIREFLY_MODEM_TEST_RX_WAIT_MS));
    s.reply_attempted = true;
    s.reply = read_until(s, "+QIURC: \"recv\"", CONFIG_FIREFLY_MODEM_TEST_RX_WAIT_MS);
    set_mark(s, RX_DONE);
    if (s.reply) {
        set_mark(s, REPLIED);
        printk("  reply received, RTT %.1f ms\n", static_cast<double>(s.mark[RX_DONE] - s.mark[SENT]));
    } else {
        printk("  NO REPLY -- waited %.3f s, which is just RX_WAIT (%.1f s).\n",
               seconds(s.mark[RX_DONE] - s.mark[SENT]), seconds(CONFIG_FIREFLY_MODEM_TEST_RX_WAIT_MS));
        printk("  This is a timeout, not a measurement. Excluded from the total.\n");
    }
    send_at(s, "AT+QICLOSE=0", "OK", 10000, false);
}

int run_test()
{
    RunState s{};
    s.t0 = k_uptime_get();
    int err = 0;
#define REQUIRED(expr) do { err = (expr); if (err != 0) { goto done; } } while (0)

    LOG_INF("\n=== Power up ===\n");
    printk("\n=== Power up ===\n");
    power_up_modem();

    REQUIRED(wait_for_modem_ready(s));

    printk("\n=== Setup (untimed) ===\n");
    // Disables command echo (ATE1 is this module's power-on default) so the
    // transcript below shows only the modem's actual responses, not our
    // own command bytes echoed back first.
    REQUIRED(send_at(s, "ATE0"));
    REQUIRED(send_at(s, "AT+CMEE=1"));
    REQUIRED(send_at(s, "AT+IFC=2,2"));
    REQUIRED(send_at(s, "AT+CFUN=4"));
    REQUIRED(wait_for_sim_ready(s));
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

    REQUIRED(wait_for_registration(s));
    set_mark(s, REGISTERED);
    printk("  registered at %.4f s (+%.3f s after RF on)\n",
           seconds(s.mark[REGISTERED] - s.t0), seconds(s.mark[REGISTERED] - s.mark[RF_ON]));

    printk("\n=== Attach ===\n");
    REQUIRED(send_at(s, "AT+CGATT=1"));
    set_mark(s, ATTACHED);

    socket_round(s);

    printk("\n=== Teardown ===\n");
    REQUIRED(send_at(s, "AT+CGATT=0"));
    set_mark(s, DETACHED);
    REQUIRED(send_at(s, "AT+COPS=2"));
    set_mark(s, DEREG);

    printk("\n=== Verify (untimed) ===\n");
    REQUIRED(send_at(s, "AT+CEREG?"));
    REQUIRED(send_at(s, "AT+CGATT?"));

    printk("\n=== Power down ===\n");
    LOG_INF("\n=== Power down ===\n");
    REQUIRED(power_down_modem(s));
done:
    report(s);
    return err;
#undef REQUIRED
}

// Sleep cycling: power up once, connect/attach once, then repeat send/
// sleep/wake kSleepCycleCount times before a full shutdown at the end.
// Triggered by DK "Button 2" (bus index 1) - see run_test() above for the
// single-shot round on "Button 1" (bus index 0).
//
// This substitutes for the nRF93M1 test's PSM cycling: the EG800Z's
// documented AT interface has no AT+CPSMS at all (checked against the
// only AT command manual available for this module family - see
// cellular_quectel_eg800z.c). Its equivalent low-power mechanism is
// AT+QSCLK-gated UART sleep, controlled through MAIN_DTR: once
// AT+QSCLK=1 is set, releasing DTR (letting it go physically high) allows
// the module to sleep, and driving DTR low again wakes it - per the
// Quectel Low Power Mode Application Note for this module family. That is
// exactly what each cycle below does.
int run_sleep_cycle_test()
{
    RunState s{};
    s.t0 = k_uptime_get();
    int err = 0;
#define REQUIRED(expr) do { err = (expr); if (err != 0) { goto done; } } while (0)

    printk("\n=== Power up ===\n");
    power_up_modem();
    REQUIRED(wait_for_modem_ready(s));

    printk("\n=== Setup (untimed) ===\n");
    // Disables command echo (ATE1 is this module's power-on default) so the
    // transcript below shows only the modem's actual responses, not our
    // own command bytes echoed back first.
    REQUIRED(send_at(s, "ATE0"));
    REQUIRED(send_at(s, "AT+CMEE=1"));
    REQUIRED(send_at(s, "AT+IFC=2,2"));
    REQUIRED(send_at(s, "AT+CFUN=4"));
    REQUIRED(wait_for_sim_ready(s));
    {
        char command[96];
        snprintk(command, sizeof(command), "AT+CGDCONT=1,\"IP\",\"%s\"",
                 CONFIG_FIREFLY_MODEM_TEST_APN);
        REQUIRED(send_at(s, command));
    }
    REQUIRED(send_at(s, "AT+COPS=0"));
    REQUIRED(send_at(s, "AT+CEREG=2"));
    // Enables DTR-gated sleep for the rest of this round - see the
    // function comment above.
    REQUIRED(send_at(s, "AT+QSCLK=1"));

    printk("\n=== RF on ===\n");
    REQUIRED(send_at(s, "AT+CFUN=1"));
    REQUIRED(wait_for_registration(s));

    printk("\n=== Attach ===\n");
    REQUIRED(send_at(s, "AT+CGATT=1"));

    for (int cycle = 1; cycle <= kSleepCycleCount; ++cycle) {
        printk("\n=== Sleep cycle %d/%d: socket/send ===\n", cycle, kSleepCycleCount);
        socket_round(s);

        printk("\n=== Sleep cycle %d/%d: sleep %d s (DTR released) ===\n",
               cycle, kSleepCycleCount, kSleepIdleSeconds);
        gpio_pin_set_dt(&kDtr, 0); /* release DTR: allow sleep */
        k_sleep(K_SECONDS(kSleepIdleSeconds));

        printk("\n=== Sleep cycle %d/%d: wake (DTR asserted) ===\n", cycle, kSleepCycleCount);
        gpio_pin_set_dt(&kDtr, 1); /* assert DTR: force wake / stay awake */
        flush_rx();
    }

    printk("\n=== Teardown ===\n");
    REQUIRED(send_at(s, "AT+CGATT=0"));
    REQUIRED(send_at(s, "AT+COPS=2"));

    printk("\n=== Power down ===\n");
    REQUIRED(power_down_modem(s));
done:
    printk("\n=== Sleep cycle test finished (%d) ===\n", err);
    return err;
#undef REQUIRED
}

#endif

class SrvModemTestEg800z final : public firefly::Thread<CONFIG_FIREFLY_MODEM_TEST_STACK_SIZE> {
public:
    SrvModemTestEg800z() : Thread(5, "srvModemTestEg800z") { k_sem_init(&processed_sem_, 0, 1); }
    int startTest(const struct zbus_observer *subscriber)
    {
        if (subscriber == nullptr || subscriber->type != ZBUS_OBSERVER_MSG_SUBSCRIBER_TYPE) {
            return -EINVAL;
        }
        if (isRunning()) { return -EALREADY; }
#ifdef CONFIG_FIREFLY_MODEM_TEST_EG800Z
        const int err = prepare_hardware();
        if (err != 0) { return err; }
#endif
        subscriber_ = subscriber;
        return start();
    }
    int lastResult() const { return has_result_ ? last_result_ : -ENODATA; }
    uint32_t count() const { return static_cast<uint32_t>(atomic_get(&processed_)); }
    int waitProcessed(uint32_t previous, int32_t timeout_ms)
    {
        if (timeout_ms < 0) { return -EINVAL; }
        while (count() <= previous) {
            const int error = k_sem_take(&processed_sem_, K_MSEC(timeout_ms));
            if (error != 0) { return error; }
        }
        return 0;
    }
protected:
    void run() override
    {
        // Button numbering is Zephyr's bus index (0-based), not DK
        // silkscreen labels (1-based): silkscreen "Button 1" = index 0,
        // "Button 2" = index 1.
        printk("srvModemTest_eg800z ready - waiting for Button 1 (single round) / "
               "Button 2 (sleep cycle)\n");
        while (!shouldStop()) {
            const struct zbus_channel *channel = nullptr;
            struct bus_msg message{};
            const int rc = zbus_sub_wait_msg(subscriber_, &channel, &message, K_MSEC(50));
            if (rc == -ENOMSG || rc == -EAGAIN) { continue; }
            if (rc != 0 || channel != &bus_chan) { continue; }
            if (message.kind != BUS_MSG_BUTTON || !bus_button_is_valid(&message.data.button) ||
                !message.data.button.pressed) { continue; }
            const uint8_t button = message.data.button.button;
            if (button != 0 && button != 1) { continue; }
#ifdef CONFIG_FIREFLY_MODEM_TEST_EG800Z
            last_result_ = (button == 0) ? run_test() : run_sleep_cycle_test();
#else
            last_result_ = -ENOSYS;
#endif
            has_result_ = true;
            atomic_inc(&processed_);
            k_sem_give(&processed_sem_);
        }
    }
private:
    const struct zbus_observer *subscriber_ = nullptr;
    int last_result_ = 0;
    bool has_result_ = false;
    atomic_t processed_{0};
    struct k_sem processed_sem_{};
};

SrvModemTestEg800z g_test;
}

extern "C" int srvModemTest_eg800z_start(const struct zbus_observer *subscriber)
{
    return g_test.startTest(subscriber);
}
extern "C" int srvModemTest_eg800z_stop(void) { return g_test.stop(); }
extern "C" bool srvModemTest_eg800z_is_running(void) { return g_test.isRunning(); }
extern "C" int srvModemTest_eg800z_last_result(void) { return g_test.lastResult(); }
extern "C" uint32_t srvModemTest_eg800z_processed_count(void) { return g_test.count(); }
extern "C" int srvModemTest_eg800z_wait_processed(uint32_t previous_count, int32_t timeout_ms)
{
    return g_test.waitProcessed(previous_count, timeout_ms);
}

#ifdef CONFIG_FIREFLY_TEST_HOOKS
extern "C" bool srvModemTest_eg800z_modem_is_powered(void)
{
#ifdef CONFIG_FIREFLY_MODEM_TEST_EG800Z
    return g_modem_powered;
#else
    return false;
#endif
}
#endif
