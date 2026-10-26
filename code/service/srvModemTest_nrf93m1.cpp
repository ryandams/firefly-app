// SPDX-License-Identifier: Apache-2.0
#include "srvModemTest_nrf93m1.h"
#include "event_bus.h"
#include "thread.hpp"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
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

LOG_MODULE_REGISTER(srvModemTest, CONFIG_FIREFLY_LOG_LEVEL);

namespace
{

#ifdef CONFIG_FIREFLY_MODEM_TEST_NRF93M1

#define MODEM_NODE DT_ALIAS(modem)
#define MODEM_UART_NODE DT_PARENT(MODEM_NODE)
const struct device *const kUart = DEVICE_DT_GET(MODEM_UART_NODE);
const struct gpio_dt_spec kPower = GPIO_DT_SPEC_GET(MODEM_NODE, mdm_power_gpios);
// Never referenced by this file until now - left the RESET pin floating/
// undriven since boot, unlike Nordic's own cellular_nordic_nrf93m1.c
// driver for this same module, which explicitly deasserts reset (with a
// documented CONFIG_MODEM_CELLULAR_RESET_POWER_ON_DELAY_MS gap) before
// ever pulsing power. A floating or stuck-asserted reset line would hold
// the modem in reset regardless of how many times POWERKEY gets pulsed -
// consistent with a stray garbage byte on one attempt but literally no
// startup current draw on the next.
const struct gpio_dt_spec kReset = GPIO_DT_SPEC_GET(MODEM_NODE, mdm_reset_gpios);
#if DT_NODE_EXISTS(DT_NODELABEL(modem_dcdc))
const struct device *const kDomain = DEVICE_DT_GET(DT_NODELABEL(modem_dcdc));
#endif
// Only defined on boards whose overlay wires DTR - see the board overlay's
// top comment for why (kept asserted so the modem doesn't commit to full
// System OFF between PSM cycles in the first place).
#if DT_NODE_HAS_PROP(MODEM_NODE, mdm_dtr_gpios)
const struct gpio_dt_spec kDtr = GPIO_DT_SPEC_GET(MODEM_NODE, mdm_dtr_gpios);
#endif

constexpr int64_t kPollMs = 2;
constexpr size_t kRxSize = 2048;
constexpr size_t kUartRxBufferSize = 256;
constexpr size_t kUartRxBufferCount = 4;
constexpr size_t kMarkCount = 11;
enum Mark : size_t
{
    MODEM_READY,
    RF_ON,
    REGISTERED,
    ATTACHED,
    SOCK_READY,
    SENT,
    REPLIED,
    RX_DONE,
    DETACHED,
    DEREG,
    POWERED_DOWN
};
const char *const kMarkNames[kMarkCount] = {"modem_ready", "rf_on",   "registered", "attached", "sock_ready",  "sent",
                                            "replied",     "rx_done", "detached",   "dereg",    "powered_down"};

// This service's own belief about modem power state - mdm-power-gpios is a
// momentary power-key pulse, not a level-controlled enable line, so there
// is no pin state to read back and confirm against. Set true once
// power_up_modem()'s pulse completes, false again once a round's shutdown
// (AT%POWD, or the GPIO-pulse fallback) completes.
bool g_modem_powered = false;

// Serialises modem access between the service thread's test rounds and the
// "modem" shell commands below. Both drive the same UART, the same
// g_uart_rx_ring and the same printk transcript, so running a shell AT
// command in the middle of a round would corrupt both sides' reads and
// interleave their output. Rounds hold this for their whole duration - which
// for the PSM cycle is several minutes - and the shell reports "busy" rather
// than blocking a shell thread for that long.
K_MUTEX_DEFINE(g_modem_lock);

struct RunState
{
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
    switch (event->type)
    {
    case UART_RX_RDY:
        ring_buf_put(&g_uart_rx_ring, event->data.rx.buf + event->data.rx.offset, event->data.rx.len);
        break;
    case UART_RX_BUF_REQUEST:
    {
        uint8_t *next = g_uart_rx_buffers[g_next_uart_rx_buffer];
        g_next_uart_rx_buffer = (g_next_uart_rx_buffer + 1) % kUartRxBufferCount;
        (void)uart_rx_buf_rsp(dev, next, kUartRxBufferSize);
        break;
    }
    default:
        break;
    }
}

double seconds(int64_t ms)
{
    return static_cast<double>(ms) / 1000.0;
}

void set_mark(RunState &s, Mark mark)
{
    s.mark[mark] = k_uptime_get();
    s.valid[mark] = true;
}

void uart_write(const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; ++i)
    {
        uart_poll_out(kUart, data[i]);
    }
}

void uart_write(const char *text)
{
    uart_write(reinterpret_cast<const uint8_t *>(text), strlen(text));
}

void flush_rx()
{
    ring_buf_reset(&g_uart_rx_ring);
}

bool contains(const char *haystack, const char *needle)
{
    return strstr(haystack, needle) != nullptr;
}

bool registered_urc(const char *text)
{
    const char *p = text;
    while ((p = strstr(p, "+CEREG:")) != nullptr)
    {
        p += strlen("+CEREG:");
        while (*p == ' ' || *p == '\t')
        {
            ++p;
        }
        if ((*p == '1' || *p == '5') && (p[1] == '\r' || p[1] == '\n' || p[1] == ','))
        {
            return true;
        }
    }
    return false;
}

bool read_until(RunState &s, const char *expected, int32_t timeout_ms, bool cereg_pattern = false)
{
    char buffer[kRxSize]{};
    char line[256]{};
    size_t used = 0;
    size_t line_used = 0;
    int64_t first = -1;
    const int64_t start = k_uptime_get();
    const int64_t deadline = start + timeout_ms;

    while (k_uptime_get() < deadline)
    {
        uint8_t byte;
        if (ring_buf_get(&g_uart_rx_ring, &byte, 1) != 1)
        {
            k_sleep(K_MSEC(kPollMs));
            continue;
        }
        if (first < 0 && byte != '\r' && byte != '\n')
        {
            first = k_uptime_get();
        }
        if (used + 1 < sizeof(buffer))
        {
            buffer[used++] = static_cast<char>(byte);
            buffer[used] = '\0';
        }
        if (byte == '\r' || byte == '\n')
        {
            if (line_used != 0)
            {
                line[line_used] = '\0';
                printk("    RX [%8.4f] %s\n", seconds(k_uptime_get() - s.t0), line);
                line_used = 0;
            }
        }
        else
        {
            if (line_used + 1 < sizeof(line))
            {
                line[line_used++] = static_cast<char>(byte);
            }
        }
        if ((cereg_pattern && registered_urc(buffer)) ||
            (!cereg_pattern && expected != nullptr && contains(buffer, expected)))
        {
            if (line_used != 0)
            {
                line[line_used] = '\0';
                printk("    RX [%8.4f] %s\n", seconds(k_uptime_get() - s.t0), line);
            }
            return true;
        }
    }
    if (line_used != 0)
    {
        line[line_used] = '\0';
        printk("    RX [%8.4f] %s\n", seconds(k_uptime_get() - s.t0), line);
    }
    ARG_UNUSED(first);
    return false;
}

int send_at(RunState &s, const char *command, const char *expected = "OK", int32_t timeout_ms = 10000,
            bool required = true)
{
    const int64_t tx = k_uptime_get();
    printk("  TX [%8.4f] %s\n", seconds(tx - s.t0), command);
    uart_write(command);
    uart_write("\r");
    const bool matched = read_until(s, expected, timeout_ms);
    const int64_t end = k_uptime_get();
    printk("     -> %s | complete %7.1f ms\n", matched ? "match" : "MISS ", static_cast<double>(end - tx));
    if (required && !matched)
    {
        printk("Expected '%s' not received.\n", expected);
        return -ETIMEDOUT;
    }
    return matched ? 0 : -ETIMEDOUT;
}

#ifdef CONFIG_FIREFLY_MODEM_FW_UPDATE_NRF93M1
// Sends AT+CGMR and extracts the version suffix from the module's
// "mfw_nrf93m1_<version>" response line (confirmed exact format via
// nordicsemi_search_sources - the nRF93M1 "Updating the modem firmware"
// guide's own worked example: "AT+CGMR" -> "mfw_nrf93m1_1.5.0" -> "OK").
// send_at()/read_until() only report whether one fixed expected substring
// appeared, not the text actually received - this is the one place that
// needs the real value, not just a match/no-match, so it duplicates
// read_until()'s byte-at-a-time ring-buffer loop rather than extending
// that shared helper's signature for a single caller.
bool read_cgmr_version(RunState &s, char *out, size_t out_size, int32_t timeout_ms = 5000)
{
    static const char kPrefix[] = "mfw_nrf93m1_";
    char buffer[kRxSize]{};
    size_t used = 0;
    const int64_t tx = k_uptime_get();
    printk("  TX [%8.4f] AT+CGMR\n", seconds(tx - s.t0));
    uart_write("AT+CGMR");
    uart_write("\r");
    const int64_t deadline = k_uptime_get() + timeout_ms;
    while (k_uptime_get() < deadline)
    {
        uint8_t byte;
        if (ring_buf_get(&g_uart_rx_ring, &byte, 1) != 1)
        {
            k_sleep(K_MSEC(kPollMs));
            continue;
        }
        if (used + 1 < sizeof(buffer))
        {
            buffer[used++] = static_cast<char>(byte);
            buffer[used] = '\0';
        }
        // Wait for the whole response (prefix line + "OK") before scanning,
        // rather than extracting the instant the prefix match completes -
        // byte-at-a-time, the version digits that follow the prefix have
        // not arrived in the buffer yet at that exact moment, which is
        // exactly what made the first version of this function always
        // capture an empty string (confirmed on real hardware).
        if (contains(buffer, "OK"))
        {
            break;
        }
    }
    out[0] = '\0';
    bool found = false;
    const char *p = strstr(buffer, kPrefix);
    if (p != nullptr)
    {
        p += strlen(kPrefix);
        size_t i = 0;
        while (p[i] != '\0' && p[i] != '\r' && p[i] != '\n' && i + 1 < out_size)
        {
            out[i] = p[i];
            ++i;
        }
        out[i] = '\0';
        found = (i > 0);
    }
    printk("  RX [%8.4f] AT+CGMR -> %s | complete %7.1f ms\n", seconds(k_uptime_get() - s.t0),
           found ? out : "(no match)", static_cast<double>(k_uptime_get() - tx));
    return found;
}
#endif // CONFIG_FIREFLY_MODEM_FW_UPDATE_NRF93M1

int wait_for_sim_ready(RunState &s)
{
    constexpr int kAttempts = 20;
    printk("\n=== Waiting for SIM ===\n");
    for (int attempt = 1; attempt <= kAttempts; ++attempt)
    {
        const int err = send_at(s, "AT+CPIN?", "READY", 500, false);
        if (err == 0)
        {
            return 0;
        }
        printk("  SIM not ready (attempt %d/%d), retrying...\n", attempt, kAttempts);
        k_sleep(K_MSEC(500));
    }
    printk("SIM did not become ready\n");
    return -ETIMEDOUT;
}

void report(const RunState &s)
{
    struct Row
    {
        const char *label;
        Mark a;
        Mark b;
    };
    static const Row rows[] = {
        {"modem ready -> RF on (setup)", MODEM_READY, RF_ON},  {"RF on -> registered", RF_ON, REGISTERED},
        {"registered -> PDP attached", REGISTERED, ATTACHED},  {"attached -> socket ready", ATTACHED, SOCK_READY},
        {"socket ready -> payload sent", SOCK_READY, SENT},    {"--- uplink path total ---", RF_ON, SENT},
        {"sent -> server reply (RTT)", SENT, REPLIED},         {"rx wait window", SENT, RX_DONE},
        {"rx wait end -> detached", RX_DONE, DETACHED},        {"detached -> deregistered", DETACHED, DEREG},
        {"deregistered -> powered down", DEREG, POWERED_DOWN}, {"--- teardown total ---", RX_DONE, POWERED_DOWN},
    };
    printk("\n====================================================================\n");
    printk("PHASE BREAKDOWN\n");
    printk("====================================================================\n");
    for (const auto &row : rows)
    {
        if (s.valid[row.a] && s.valid[row.b])
        {
            printk("%-34s %8.3f s\n", row.label, seconds(s.mark[row.b] - s.mark[row.a]));
        }
        else
        {
            printk("%-34s %8s%s\n", row.label, "n/a",
                   row.b == REPLIED && !s.reply ? "   <- no %SKTRECV, nothing to measure" : "");
        }
    }
    printk("--------------------------------------------------------------------\n");
    if (s.valid[POWERED_DOWN])
    {
        const double total = seconds(s.mark[POWERED_DOWN] - s.t0);
        printk("%-34s %8.3f s\n", "CONNECTION WINDOW (power up->powered down)", total);
        if (s.valid[SENT] && s.valid[RX_DONE])
        {
            const double wait = seconds(s.mark[RX_DONE] - s.mark[SENT]);
            printk("%-34s %8.3f s   <== use this for comparison\n",
                   s.reply ? "  minus rx wait (real RTT)" : "  minus rx wait (idle timeout)", total - wait);
        }
    }
    printk("====================================================================\n");
    if (s.send_attempted && !s.sent_ok)
    {
        printk("WARNING: modem never returned OK after the payload - send unconfirmed.\n");
    }
    if (s.reply_attempted && !s.reply)
    {
        printk("WARNING: no server reply within %.1f s. The 'rx wait window'\n",
               seconds(CONFIG_FIREFLY_MODEM_TEST_RX_WAIT_MS));
        printk("         above is the timeout constant, NOT a round-trip time.\n");
        printk("         It is excluded from the comparison figure.\n");
    }
    printk("CSV,");
    for (size_t i = 0; i < kMarkCount; ++i)
    {
        if (s.valid[i])
        {
            printk("%.4f", seconds(s.mark[i] - s.t0));
        }
        if (i + 1 < kMarkCount)
        {
            printk(",");
        }
    }
    printk("\n");
}

int prepare_hardware()
{
    if (!device_is_ready(kUart) || !gpio_is_ready_dt(&kPower) || !gpio_is_ready_dt(&kReset))
    {
        return -ENODEV;
    }
    int err = gpio_pin_configure_dt(&kPower, GPIO_OUTPUT_INACTIVE);
    if (err != 0)
    {
        return err;
    }
    // Explicitly released (not left floating) before the first POWERKEY
    // pulse ever happens - see kReset's declaration comment for why.
    err = gpio_pin_configure_dt(&kReset, GPIO_OUTPUT_INACTIVE);
    if (err != 0)
    {
        return err;
    }
#if DT_NODE_HAS_PROP(MODEM_NODE, mdm_dtr_gpios)
    if (!gpio_is_ready_dt(&kDtr))
    {
        return -ENODEV;
    }
    // Asserted once here and never released - see the board overlay's top
    // comment for why (this is a mitigation to discourage full System OFF
    // between PSM cycles, not a substitute for the POWERKEY-based wake in
    // psm_socket_round(), which stays as the documented, guaranteed
    // recovery path regardless of what this does or doesn't prevent).
    err = gpio_pin_configure_dt(&kDtr, GPIO_OUTPUT_ACTIVE);
    if (err != 0)
    {
        return err;
    }
#endif
    // Starts with flow control off - wait_for_modem_ready() below is the
    // one place that decides when RTS/CTS gets turned on, since it also
    // has to be able to turn it back off on demand (see that function's
    // comment for why).
    struct uart_config config = {
        .baudrate = 115200,
        .parity = UART_CFG_PARITY_NONE,
        .stop_bits = UART_CFG_STOP_BITS_1,
        .data_bits = UART_CFG_DATA_BITS_8,
        .flow_ctrl = UART_CFG_FLOW_CTRL_NONE,
    };
    err = uart_configure(kUart, &config);
    if (err != 0)
    {
        return err;
    }
    err = pm_device_runtime_get(kUart);
    if (err != 0 && err != -EALREADY)
    {
        return err;
    }
    err = uart_callback_set(kUart, uart_event_handler, nullptr);
    if (err != 0)
    {
        return err;
    }
    ring_buf_reset(&g_uart_rx_ring);
    g_next_uart_rx_buffer = 1;
    err = uart_rx_enable(kUart, g_uart_rx_buffers[0], kUartRxBufferSize, 1000);
    if (err != 0)
    {
        return err;
    }
    return 0;
}

// Modem-off -> modem-on power-key pulse, plus the modem_dcdc rail where the
// board has one. Deliberately NOT run from prepare_hardware() (which only
// runs once, at service start/boot) - it runs at the top of each round
// instead, so the modem (and its rail) stay fully unpowered from
// srvModemTest_nrf93m1_start() until the first button0 press. No fixed
// settle delay here - wait_for_modem_ready() below polls until the modem
// actually answers instead of blindly sleeping a worst-case boot time.
int power_up_modem()
{
#if DT_NODE_EXISTS(DT_NODELABEL(modem_dcdc))
    const int domain_err = pm_device_runtime_get(kDomain);
    if (domain_err != 0 && domain_err != -EALREADY)
    {
        printk("  modem power domain on failed (%d)\n", domain_err);
        return domain_err;
    }
#endif
    gpio_pin_set_dt(&kPower, 1);
    k_sleep(K_MSEC(100));
    gpio_pin_set_dt(&kPower, 0);
    g_modem_powered = true;
    flush_rx();
    return 0;
}

// Polls "AT" until the modem answers "OK" instead of a fixed post-pulse
// sleep - the module's boot time varies (SIM/firmware/temperature), and a
// fixed delay is either a wasted wait on the common case or a race on the
// slow one. Mirrors wait_for_sim_ready()'s retry-loop shape. Also used to
// wake the modem from PSM/idle sleep between rounds (mark_ready = false
// there, so a mid-test wake doesn't clobber the original MODEM_READY mark
// used for the "modem ready -> RF on" report row).
//
// Temporarily drops hardware flow control while polling. Per the nRF91-
// series UARTE hardware docs' "Transmission" section, TX is suspended
// indefinitely - no timeout - while CTS is deasserted, and this modem's
// %MODULECFG/%SIMCFG power-saving settings persist in NVRAM across power
// cycles: once they've been sent, even a *fresh cold boot* can leave CTS
// deasserted for longer than before (or until the modem sees UART
// activity), not just a wake from a PSM cycle within the same session. A
// plain uart_poll_out() under HWFC would then block forever with no way
// to ever send the byte that would clear it - which is what left both the
// PSM-cycle wake and, after those settings started persisting, even a
// fresh boot hanging indefinitely instead of timing out. Flow control is
// restored once the modem actually answers "AT", or unconditionally on
// timeout so a failed wake doesn't leave the UART stuck without it either.
int wait_for_modem_ready(RunState &s, bool mark_ready = true,
                         int32_t timeout_ms = CONFIG_FIREFLY_MODEM_TEST_READY_TIMEOUT_MS)
{
    printk("\n=== Waiting for modem ready ===\n");
    struct uart_config config = {
        .baudrate = 115200,
        .parity = UART_CFG_PARITY_NONE,
        .stop_bits = UART_CFG_STOP_BITS_1,
        .data_bits = UART_CFG_DATA_BITS_8,
        .flow_ctrl = UART_CFG_FLOW_CTRL_NONE,
    };
    int err = uart_configure(kUart, &config);
    if (err != 0)
    {
        return err;
    }

    bool ready = false;
    const int64_t deadline = k_uptime_get() + timeout_ms;
    while (k_uptime_get() < deadline)
    {
        if (send_at(s, "AT", "OK", 300, false) == 0)
        {
            ready = true;
            break;
        }
        k_sleep(K_MSEC(200));
    }

    config.flow_ctrl = UART_CFG_FLOW_CTRL_RTS_CTS;
    const int fc_err = uart_configure(kUart, &config);

    if (!ready)
    {
        printk("Modem did not respond to AT within timeout\n");
        return -ETIMEDOUT;
    }
    if (mark_ready)
    {
        set_mark(s, MODEM_READY);
    }
    return fc_err;
}

// Modem-on -> modem-off. Tries the driver's own software shutdown command
// first (AT%POWD=1, matching cellular_nordic_nrf93m1.c's shutdown chat
// script); if that isn't confirmed within the timeout, falls back to the
// same power-key pulse used to power the modem up. Releases the modem_dcdc
// rail claimed in power_up_modem() either way, so a failed shutdown still
// leaves nothing energized.
int power_down_modem(RunState &s)
{
    int result;
    if (send_at(s, "AT%POWD=1", "POWERED DOWN", 5000, false) == 0)
    {
        set_mark(s, POWERED_DOWN);
        g_modem_powered = false;
        result = 0;
    }
    else
    {
        printk("  AT%%POWD=1 not confirmed - falling back to GPIO power-key pulse\n");
        const int err1 = gpio_pin_set_dt(&kPower, 1);
        k_sleep(K_MSEC(100));
        const int err2 = gpio_pin_set_dt(&kPower, 0);
        g_modem_powered = false;
        if (err1 != 0 || err2 != 0)
        {
            printk("  GPIO power-off pulse failed too - modem shutdown NOT confirmed\n");
            result = -EIO;
        }
        else
        {
            set_mark(s, POWERED_DOWN);
            result = 0;
        }
    }
#if DT_NODE_EXISTS(DT_NODELABEL(modem_dcdc))
    // const int domain_err = pm_device_runtime_put(kDomain);
    // if (domain_err != 0 && domain_err != -EALREADY) {
    //     printk("  modem power domain off failed (%d)\n", domain_err);
    //     if (result == 0) { result = domain_err; }
    // }
#endif
    return result;
}

#ifdef CONFIG_FIREFLY_MODEM_FW_UPDATE_NRF93M1
// Button 2 in this build: a one-time AT%HTTPFOTADL firmware check/update,
// entirely over this raw AT pipe. CONFIG_MODEM=n in this build mode means
// modem_cellular.c is not even compiled in, and the update itself needs no
// PPP/CMUX either - confirmed via nordicsemi_search_sources (the nRF93M1
// "Updating the modem firmware" and "Features" guides) that %HTTPFOTADL
// downloads, verifies, and applies the delta entirely inside the module's
// own IP/HTTP stack ("a host without its own IP stack can still use TCP and
// UDP sockets"), using whatever PDP context AT+CFUN=1 attaches with - no
// host-side PPP/socket involvement at all.
//
// Setup sequence (CGDCONT/COPS/CFUN=1/registration wait) mirrors run_test()
// above rather than inventing a new one. Per the AT Commands Reference
// confirmed via the same search: the module boots at CFUN=0 and does not
// attach on its own, so AT+CFUN=1 (after CGDCONT/COPS) is required before
// registration - or the HTTP download - can happen.
int run_firmware_update()
{
    RunState s{};
    s.t0 = k_uptime_get();
    int err = 0;
    const char *outcome = "FAILED";
#define REQUIRED(expr)                                                                                                 \
    do                                                                                                                 \
    {                                                                                                                  \
        err = (expr);                                                                                                  \
        if (err != 0)                                                                                                  \
        {                                                                                                              \
            goto done;                                                                                                 \
        }                                                                                                              \
    } while (0)

    printk("\n=== Power up ===\n");
    REQUIRED(power_up_modem());
    REQUIRED(wait_for_modem_ready(s));

    printk("\n=== Setup (untimed) ===\n");
    REQUIRED(send_at(s, "ATE0"));
    {
        char command[96];
        snprintk(command, sizeof(command), "AT+CGDCONT=1,\"IP\",\"%s\"", CONFIG_FIREFLY_MODEM_TEST_APN);
        REQUIRED(send_at(s, command));
    }
    REQUIRED(send_at(s, "AT+COPS=0"));
    REQUIRED(send_at(s, "AT+CEREG=2"));

    printk("\n=== RF on ===\n");
    REQUIRED(send_at(s, "AT+CFUN=1"));

    printk("\n=== Waiting for +CEREG URC ===\n");
    if (!read_until(s, nullptr, CONFIG_FIREFLY_MODEM_TEST_REG_TIMEOUT_S * 1000, true))
    {
        printk("Network registration timed out\n");
        err = -ETIMEDOUT;
        goto done;
    }

    printk("\n=== Checking current firmware version ===\n");
    {
        char current[32] = {0};
        if (!read_cgmr_version(s, current, sizeof(current)))
        {
            printk("Could not read AT+CGMR - aborting update\n");
            err = -EIO;
            goto done;
        }
        printk("  current modem firmware: mfw_nrf93m1_%s\n", current);

        // Delta images are relative to one specific starting version (per
        // the same Nordic guide), so a mismatch here means the configured
        // URL's package would not apply cleanly - skip rather than guess.
        if (strcmp(current, CONFIG_FIREFLY_MODEM_FW_UPDATE_FROM_VERSION) != 0)
        {
            printk("  expected mfw_nrf93m1_%s - not updating\n", CONFIG_FIREFLY_MODEM_FW_UPDATE_FROM_VERSION);
            outcome = "NO-OP (version mismatch)";
            err = 0;
            goto done;
        }

        printk("\n=== Downloading and verifying update (up to %d s) ===\n",
               CONFIG_FIREFLY_MODEM_FW_UPDATE_DOWNLOAD_TIMEOUT_S);
        {
            char command[300];
            snprintk(command, sizeof(command), "AT%%HTTPFOTADL=\"%s\",100", CONFIG_FIREFLY_MODEM_FW_UPDATE_URL);
            // "OK" here only means the command was accepted, not that the
            // download succeeded - the real outcome comes via the
            // %HTTPURC: "FOTA",... lines read_until() waits for next.
            REQUIRED(send_at(s, command, "OK", 10000));
        }
        if (!read_until(s, "VERIFIED", CONFIG_FIREFLY_MODEM_FW_UPDATE_DOWNLOAD_TIMEOUT_S * 1000))
        {
            printk("Did not see %%HTTPURC \"FOTA\",\"VERIFIED\" in time - see the RX\n"
                   "lines above for what the modem actually reported (a\n"
                   "DOWNLOAD ERROR or VERIFICATION FAILURE detail, if any,\n"
                   "was already printed live as it arrived).\n");
            err = -ETIMEDOUT;
            goto done;
        }

        printk("\n=== Verified - waiting for the module's own reboot (up to %d s) ===\n",
               CONFIG_FIREFLY_MODEM_FW_UPDATE_REBOOT_TIMEOUT_S);
        // The module reboots and applies the delta on its own here - this
        // is not a power-key wake, just re-polling "AT" until it answers
        // again, same as wait_for_modem_ready()'s normal job.
        flush_rx();
        REQUIRED(wait_for_modem_ready(s, false, CONFIG_FIREFLY_MODEM_FW_UPDATE_REBOOT_TIMEOUT_S * 1000));

        char updated[32] = {0};
        if (!read_cgmr_version(s, updated, sizeof(updated)))
        {
            printk("Modem came back but AT+CGMR did not respond as expected\n");
            err = -EIO;
            goto done;
        }
        printk("  modem firmware is now: mfw_nrf93m1_%s\n", updated);
        if (strcmp(updated, CONFIG_FIREFLY_MODEM_FW_UPDATE_TO_VERSION) == 0)
        {
            outcome = "SUCCESS";
        }
        else
        {
            outcome = "FAILED (unexpected version after update)";
            err = -EIO;
        }
    }

done:
    // Always attempted, even after an earlier failure - a firmware-update
    // round should not leave the modem sitting powered on indefinitely.
    printk("\n=== Power down ===\n");
    power_down_modem(s);
    printk("\n====================================================================\n");
    printk("FIRMWARE UPDATE RESULT: %s\n", (err == 0) ? outcome : "FAILED");
    printk("====================================================================\n");
    return err;
#undef REQUIRED
}
#endif // CONFIG_FIREFLY_MODEM_FW_UPDATE_NRF93M1

int run_test()
{
    RunState s{};
    s.t0 = k_uptime_get();
    int err = 0;
#define REQUIRED(expr)                                                                                                 \
    do                                                                                                                 \
    {                                                                                                                  \
        err = (expr);                                                                                                  \
        if (err != 0)                                                                                                  \
        {                                                                                                              \
            goto done;                                                                                                 \
        }                                                                                                              \
    } while (0)

    LOG_INF("\n=== Power up ===\n");
    printk("\n=== Power up ===\n");
    REQUIRED(power_up_modem());

    REQUIRED(wait_for_modem_ready(s));

    printk("\n=== Setup (untimed) ===\n");
    // Disables command echo (ATE1 is this modem's power-on default) so the
    // transcript below shows only the modem's actual responses, not our
    // own command bytes echoed back first.
    //
    // AT%PMUCFG removed 2026-10-07: confirmed via nordicsemi_search_sources
    // (the nRF93M1 AT Commands Reference) that this is not a documented
    // nRF93M1 command at all - the real power/module-config command is
    // %MODULECFG, already used correctly elsewhere in this file (see
    // run_psm_cycle_test()'s "ledMode"/"usbCtrl"/etc. calls). Firmware 1.4.1
    // silently tolerated AT%PMUCFG=1,1 as a no-op; after updating to 1.5.0
    // it started correctly answering ERROR, aborting this REQUIRED() step on
    // real hardware (confirmed - this was never a valid command on either
    // version, just inconsistently enforced).
    REQUIRED(send_at(s, "ATE0"));
    REQUIRED(send_at(s, "AT+CMEE=1"));
    REQUIRED(send_at(s, "AT+CFUN=4"));
    REQUIRED(wait_for_sim_ready(s));
    REQUIRED(send_at(s, "AT%SKTDELETE=all"));
    {
        char command[96];
        snprintk(command, sizeof(command), "AT+CGDCONT=1,\"IP\",\"%s\"", CONFIG_FIREFLY_MODEM_TEST_APN);
        REQUIRED(send_at(s, command));
    }
    REQUIRED(send_at(s, "AT+COPS=0"));
    REQUIRED(send_at(s, "AT+CEREG=2"));

    printk("\n=== RF on ===\n");
    set_mark(s, RF_ON);
    REQUIRED(send_at(s, "AT+CFUN=1"));

    printk("\n=== Waiting for +CEREG URC ===\n");
    if (!read_until(s, nullptr, CONFIG_FIREFLY_MODEM_TEST_REG_TIMEOUT_S * 1000, true))
    {
        printk("Network registration timed out\n");
        err = -ETIMEDOUT;
        goto done;
    }
    set_mark(s, REGISTERED);
    printk("  registered at %.4f s (+%.3f s after RF on)\n", seconds(s.mark[REGISTERED] - s.t0),
           seconds(s.mark[REGISTERED] - s.mark[RF_ON]));

    printk("\n=== Attach ===\n");
    REQUIRED(send_at(s, "AT+CGATT=1"));
    set_mark(s, ATTACHED);

    printk("\n=== Socket ===\n");
    REQUIRED(send_at(s, "AT%SKTCREATE=1,2,17"));
    {
        char command[96];
        snprintk(command, sizeof(command), "AT%%SKTCONNECT=0,\"%s\",%d", CONFIG_FIREFLY_MODEM_TEST_SERVER_IP,
                 CONFIG_FIREFLY_MODEM_TEST_SERVER_PORT);
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
        for (size_t i = 0; i < sizeof(message) - 1; ++i)
        {
            const uint8_t c = static_cast<uint8_t>(message[i]);
            uart_poll_out(kUart, hex[c >> 4]);
            uart_poll_out(kUart, hex[c & 0xf]);
        }
        uart_poll_out(kUart, 0x1a);
    }
    s.sent_ok = read_until(s, "OK", 10000);
    set_mark(s, SENT);
    if (!s.sent_ok)
    {
        printk("  WARNING: no OK after payload - modem did not confirm the send\n");
    }

    printk("\n=== Waiting up to %.0f s for %%SKTRECV ===\n", seconds(CONFIG_FIREFLY_MODEM_TEST_RX_WAIT_MS));
    s.reply_attempted = true;
    s.reply = read_until(s, "SKTRECV", CONFIG_FIREFLY_MODEM_TEST_RX_WAIT_MS);
    set_mark(s, RX_DONE);
    if (s.reply)
    {
        set_mark(s, REPLIED);
        printk("  reply received, RTT %.1f ms\n", static_cast<double>(s.mark[RX_DONE] - s.mark[SENT]));
    }
    else
    {
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

    printk("\n=== Power down ===\n");
    LOG_INF("\n=== Power down ===\n");
    REQUIRED(power_down_modem(s));
done:
    report(s);
    return err;
#undef REQUIRED
}

#ifndef CONFIG_FIREFLY_MODEM_FW_UPDATE_NRF93M1
// Button 2 runs run_firmware_update() instead when that Kconfig is set (see
// its own definition above and the dispatch in SrvModemTest::run() below) -
// guarded out entirely rather than just unreachable, so it (and these two
// constants, used only here) do not trigger unused-symbol warnings in that
// build.
constexpr int kPsmCycleCount = 5;
constexpr int kPsmIdleSeconds = 60;

// One PSM cycle's socket work: wake the modem, fresh socket, connect,
// send, wait for a reply, then delete the socket so the next cycle
// starts clean. Socket/send AT sequence matches run_test()'s "Socket"/
// "Send" sections, factored out so run_psm_cycle_test() can repeat it.
//
// The wake step reuses power_up_modem()'s POWERKEY pulse rather than
// anything UART-based. Per the module's own getting-started guide: UART-
// based wake (RXD activity, and this covers DTR too - both are UART1
// pins) from deep sleep/System OFF only works if AT+IPR set the baud rate
// to <=9600 before sleep; at higher baud rates - this UART runs at
// 115200 - "the POWERKEY or power cycle must be used" instead, and
// firmware support for higher-baud wake is only planned for a future
// modem firmware release, not available now. power_up_modem() is safe to
// call again here: the modem_dcdc rail is already claimed from the
// original power-up and pm_device_runtime_get() on it is idempotent, and
// flush_rx() clearing the ring buffer before the wake response arrives is
// exactly what's wanted anyway.
// wait_for_modem_ready() (mark_ready = false, reused rather than a
// separate poll loop) then confirms the modem is back and responding to
// AT before anything else gets sent, dropping flow control while it does
// so since CTS can still be deasserted for a while after the POWERKEY
// wake too.
int psm_socket_round(RunState &s, int cycle)
{
    printk("\n=== PSM cycle %d/%d: wake ===\n", cycle, kPsmCycleCount);
    int err = power_up_modem();
    if (err != 0)
    {
        return err;
    }
    err = wait_for_modem_ready(s, false);
    if (err != 0)
    {
        return err;
    }

    printk("\n=== PSM cycle %d/%d: socket ===\n", cycle, kPsmCycleCount);
    err = send_at(s, "AT%SKTCREATE=1,2,17");
    if (err != 0)
    {
        return err;
    }
    {
        char command[96];
        snprintk(command, sizeof(command), "AT%%SKTCONNECT=0,\"%s\",%d", CONFIG_FIREFLY_MODEM_TEST_SERVER_IP,
                 CONFIG_FIREFLY_MODEM_TEST_SERVER_PORT);
        err = send_at(s, command);
        if (err != 0)
        {
            return err;
        }
    }

    printk("\n=== PSM cycle %d/%d: send ===\n", cycle, kPsmCycleCount);
    err = send_at(s, "AT%SKTSENDT=0", ">");
    if (err != 0)
    {
        return err;
    }
    {
        static const char message[] = CONFIG_FIREFLY_MODEM_TEST_MESSAGE;
        static const char hex[] = "0123456789abcdef";
        for (size_t i = 0; i < sizeof(message) - 1; ++i)
        {
            const uint8_t c = static_cast<uint8_t>(message[i]);
            uart_poll_out(kUart, hex[c >> 4]);
            uart_poll_out(kUart, hex[c & 0xf]);
        }
        uart_poll_out(kUart, 0x1a);
    }
    if (!read_until(s, "OK", 10000))
    {
        printk("  WARNING: no OK after payload - modem did not confirm the send\n");
    }
    if (!read_until(s, "SKTRECV", CONFIG_FIREFLY_MODEM_TEST_RX_WAIT_MS))
    {
        printk("  NO REPLY within %.1f s (timeout, not measured)\n", seconds(CONFIG_FIREFLY_MODEM_TEST_RX_WAIT_MS));
    }
    return send_at(s, "AT%SKTDELETE=all");
}

// PSM cycling: power up once, request PSM and attach once, then repeat
// connect->send->idle kPsmCycleCount times without touching AT+CPSMS or
// re-attaching between cycles (that signaling avoidance is the entire
// point of PSM), then a full shutdown at the end. Triggered by DK
// "Button 2" (bus index 1) - see run_test() above for the single-shot
// round on "Button 1" (bus index 0).
//
// AT+CPSMS's timers are negotiated with the network during Attach/TAU
// signaling, not the instant the command is sent, and this modem has no
// PSM-entry confirmation URC - so the idle window below is a best-effort
// wait (per the nRF93M1 AT command reference: Active-Time "00000001" =
// 1 x 2s unit), not a verified sleep duration. Nothing is sent during the
// idle window itself, letting the modem settle into PSM - psm_socket_round()
// runs an explicit wake step (wait_for_modem_ready()) at the start of each
// cycle instead of assuming the first AT command of the next cycle will
// get through.
int run_psm_cycle_test()
{
    RunState s{};
    s.t0 = k_uptime_get();
    int err = 0;
#define REQUIRED(expr)                                                                                                 \
    do                                                                                                                 \
    {                                                                                                                  \
        err = (expr);                                                                                                  \
        if (err != 0)                                                                                                  \
        {                                                                                                              \
            goto done;                                                                                                 \
        }                                                                                                              \
    } while (0)

    printk("\n=== Power up ===\n");
    REQUIRED(power_up_modem());
    REQUIRED(wait_for_modem_ready(s));

    printk("\n=== Setup (untimed) ===\n");
    // Disables command echo (ATE1 is this modem's power-on default) so the
    // transcript below shows only the modem's actual responses, not our
    // own command bytes echoed back first.
    REQUIRED(send_at(s, "ATE0"));
    REQUIRED(send_at(s, "AT+CGMR"));
    REQUIRED(send_at(s, "AT+CMEE=1"));
    REQUIRED(send_at(s, "AT+CFUN=4"));
    REQUIRED(wait_for_sim_ready(s));
    REQUIRED(send_at(s, "AT%SKTDELETE=all"));
    {
        char command[96];
        snprintk(command, sizeof(command), "AT+CGDCONT=1,\"IP\",\"%s\"", CONFIG_FIREFLY_MODEM_TEST_APN);
        REQUIRED(send_at(s, command));
    }
    REQUIRED(send_at(s, "AT+COPS=0"));
    // Mode 5 (vs. run_test()'s mode 2): per 3GPP TS 27.007, this adds the
    // network-GRANTED Active-Time/Periodic-TAU to the +CEREG URC, not just
    // what we requested - AT+CPSMS? only ever echoes the request back, so
    // this is the only way to see whether the network actually honored the
    // short Active-Time or overrode it with something longer.
    REQUIRED(send_at(s, "AT+CEREG=5"));

    // PSM parameters are negotiated with the network during Attach/TAU
    // signaling, not the instant this command is sent - requesting them
    // here, before AT+CFUN=1/attach, gets them included in that initial
    // Attach so the network can grant PSM right away. Requesting them
    // mid-session (as this used to, once per cycle) only stores them
    // locally; the network has no reason to act on them until the next
    // periodic TAU, which is minutes away here - almost certainly why real
    // PSM was never reached regardless of Active-Time tuning. Active-Time
    // itself stays short (2s = "00000001"): that's how long the modem
    // stays in higher-current RRC-idle after RRC release before the uA-
    // level PSM floor begins.
    printk("\n=== Enable PSM (Active-Time 2s, requested before attach) ===\n");
    REQUIRED(send_at(s, "AT%MODULECFG=\"ledMode\",0"));
    REQUIRED(send_at(s, "AT%MODULECFG=\"logCtrl\",0"));
    REQUIRED(send_at(s, "AT%MODULECFG=\"usbCtrl\",2"));
    REQUIRED(send_at(s, "AT%MODULECFG=\"usbSlpMask\",1"));
    REQUIRED(send_at(s, "AT%SIMCFG=\"SimPowerSave\",1"));
    REQUIRED(send_at(s, "AT%SIMCFG=\"SimPresenceDetect\",0"));
    REQUIRED(send_at(s, "AT+CPSMS=1,,,\"00000000\",\"00000000\""));

    printk("\n=== RF on ===\n");
    REQUIRED(send_at(s, "AT+CFUN=1"));
    if (!read_until(s, nullptr, CONFIG_FIREFLY_MODEM_TEST_REG_TIMEOUT_S * 1000, true))
    {
        printk("Network registration timed out\n");
        err = -ETIMEDOUT;
        goto done;
    }

    printk("\n=== Attach ===\n");
    REQUIRED(send_at(s, "AT+CGATT=1"));

    for (int cycle = 1; cycle <= kPsmCycleCount; ++cycle)
    {
        REQUIRED(psm_socket_round(s, cycle));

        // No AT+CPSMS toggling here on purpose: disabling PSM would
        // immediately un-grant it, leaving the next cycle to renegotiate
        // from scratch (the same problem this whole redesign exists to
        // avoid). PSM stays continuously granted from the one Attach
        // above; going idle (sending nothing) lets RRC release and
        // Active-Time run their course, and the next cycle's own AT
        // traffic (in psm_socket_round) is what wakes it back up.
        //
        // AT%PMUCFG=1,4 removed 2026-10-07 along with this file's other
        // %PMUCFG calls - not a documented nRF93M1 command (confirmed via
        // nordicsemi_search_sources), so it was never actually doing
        // anything here; the RRC-release/Active-Time/PSM transition this
        // comment describes already happens from the module's own natural
        // behavior once nothing is sent during the idle window below.
        printk("\n=== PSM cycle %d/%d: idle %d s (RRC release + Active-Time, then PSM) ===\n", cycle, kPsmCycleCount,
               kPsmIdleSeconds);
        k_sleep(K_SECONDS(kPsmIdleSeconds));
        flush_rx();
    }

    REQUIRED(psm_socket_round(s, kPsmCycleCount + 1));

    printk("\n=== PSM cycle %d/%d: idle %d s (RRC release + Active-Time, then PSM) ===\n", kPsmCycleCount + 1,
           kPsmCycleCount, kPsmIdleSeconds);
    flush_rx();

    printk("\n=== Teardown ===\n");
    REQUIRED(send_at(s, "AT+CGATT=0"));
    REQUIRED(send_at(s, "AT+COPS=2"));

    printk("\n=== Power down ===\n");
    REQUIRED(power_down_modem(s));
done:
    printk("\n=== PSM cycle test finished (%d) ===\n", err);
    return err;
#undef REQUIRED
}
#endif // !CONFIG_FIREFLY_MODEM_FW_UPDATE_NRF93M1

#endif

class SrvModemTest final : public firefly::Thread<CONFIG_FIREFLY_MODEM_TEST_STACK_SIZE>
{
  public:
    SrvModemTest() : Thread(5, "srvModemTest")
    {
        k_sem_init(&processed_sem_, 0, 1);
    }
    int startTest(const struct zbus_observer *subscriber)
    {
        if (subscriber == nullptr || subscriber->type != ZBUS_OBSERVER_MSG_SUBSCRIBER_TYPE)
        {
            return -EINVAL;
        }
        if (isRunning())
        {
            return -EALREADY;
        }
#ifdef CONFIG_FIREFLY_MODEM_TEST_NRF93M1
        const int err = prepare_hardware();
        if (err != 0)
        {
            return err;
        }
#endif
        subscriber_ = subscriber;
        return start();
    }
    int lastResult() const
    {
        return has_result_ ? last_result_ : -ENODATA;
    }
    uint32_t count() const
    {
        return static_cast<uint32_t>(atomic_get(&processed_));
    }
    int waitProcessed(uint32_t previous, int32_t timeout_ms)
    {
        if (timeout_ms < 0)
        {
            return -EINVAL;
        }
        while (count() <= previous)
        {
            const int error = k_sem_take(&processed_sem_, K_MSEC(timeout_ms));
            if (error != 0)
            {
                return error;
            }
        }
        return 0;
    }

  protected:
    void run() override
    {
        // Button numbering is Zephyr's bus index (0-based), not DK silkscreen
        // labels (1-based): silkscreen "Button 1" = button0 = index 0,
        // "Button 2" = button1 = index 1. This board has no third button.
        printk("srvModemTest_nrf93m1 ready - waiting for Button 1 (single round) / "
               "Button 2 (%s)\n",
               IS_ENABLED(CONFIG_FIREFLY_MODEM_FW_UPDATE_NRF93M1) ? "firmware update" : "PSM cycle");
        while (!shouldStop())
        {
            const struct zbus_channel *channel = nullptr;
            struct bus_msg message{};
            const int rc = zbus_sub_wait_msg(subscriber_, &channel, &message, K_MSEC(50));
            if (rc == -ENOMSG || rc == -EAGAIN)
            {
                continue;
            }
            if (rc != 0 || channel != &bus_chan)
            {
                continue;
            }
            if (message.kind != BUS_MSG_BUTTON || !bus_button_is_valid(&message.data.button) ||
                !message.data.button.pressed)
            {
                continue;
            }
            const uint8_t button = message.data.button.button;
            if (button != 0 && button != 1)
            {
                continue;
            }
#ifdef CONFIG_FIREFLY_MODEM_TEST_NRF93M1
            k_mutex_lock(&g_modem_lock, K_FOREVER);
#ifdef CONFIG_FIREFLY_MODEM_FW_UPDATE_NRF93M1
            last_result_ = (button == 0) ? run_test() : run_firmware_update();
#else
            last_result_ = (button == 0) ? run_test() : run_psm_cycle_test();
#endif
            k_mutex_unlock(&g_modem_lock);
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

SrvModemTest g_test;

// ---------------------------------------------------------------------------
//  "modem" shell commands - AT access to the modem from the console
// ---------------------------------------------------------------------------
// Zephyr's own CONFIG_MODEM_AT_SHELL is not usable in this mode. It needs
// MODEM_AT_USER_PIPE, which attaches to a modem_pipelink published by
// drivers/modem/modem_cellular.c's MODEM_CELLULAR_DEFINE_AND_INIT_USER_PIPES.
// This board's raw-AT configuration sets CONFIG_MODEM=n, so that driver does
// not exist and there is no pipe to attach to - and this service owns the
// modem UART outright (its own uart_rx_enable() and ring buffer), so a second
// consumer would fight it for received bytes. These commands therefore reuse
// the service's existing send_at() rather than adding a parallel AT path,
// which also means every exchange lands in the same TX/RX transcript the test
// rounds print.
// Guarded on both: these handlers reach straight into this file's raw-AT
// statics (g_modem_lock, g_modem_powered, send_at, power_up_modem), none of
// which exist unless FIREFLY_MODEM_TEST_NRF93M1 is selected. Other boards in
// this tree enable CONFIG_SHELL while running srvNetwork instead.
#if defined(CONFIG_SHELL) && defined(CONFIG_FIREFLY_MODEM_TEST_NRF93M1)

#include <zephyr/shell/shell.h>
#include <stdlib.h>

// Rounds only power the modem for their own duration, so between rounds there
// is nothing to talk to. "modem on"/"modem off" exist so the AT command has a
// powered modem to reach - without them the shell would only ever be usable
// during the seconds a round holds g_modem_lock, i.e. never.
int claim_modem(const struct shell *sh)
{
    if (!srvModemTest_nrf93m1_is_running())
    {
        shell_error(sh, "srvModemTest is not running - the modem UART has not "
                        "been set up (prepare_hardware() runs at service start)");
        return -ENODEV;
    }
    if (k_mutex_lock(&g_modem_lock, K_MSEC(100)) != 0)
    {
        shell_error(sh, "modem busy - a test round is in progress");
        return -EBUSY;
    }
    return 0;
}

int cmd_modem_on(const struct shell *sh, size_t, char **)
{
    const int claim = claim_modem(sh);
    if (claim != 0)
    {
        return claim;
    }

    RunState state{};
    state.t0 = k_uptime_get();
    int err = 0;
    if (g_modem_powered)
    {
        shell_print(sh, "modem already powered - re-syncing");
    }
    else
    {
        err = power_up_modem();
    }
    // Plain wait_for_modem_ready(). The uart9600 branch calls its
    // baud-recovery helper here instead, but that helper exists only to
    // rescue a modem left at 9600 by a PSM round interrupted midway. This
    // branch keeps the POWERKEY-based PSM wake and never sets a baud rate on
    // the modem, so nothing can leave it at a stale one - it is always
    // expected to answer at 115200.
    if (err == 0)
    {
        err = wait_for_modem_ready(state);
    }
    k_mutex_unlock(&g_modem_lock);

    if (err != 0)
    {
        shell_error(sh, "modem did not become ready (%d)", err);
        return err;
    }
    shell_print(sh, "modem powered and answering AT");
    return 0;
}

int cmd_modem_off(const struct shell *sh, size_t, char **)
{
    const int claim = claim_modem(sh);
    if (claim != 0)
    {
        return claim;
    }

    RunState state{};
    state.t0 = k_uptime_get();
    const int err = power_down_modem(state);
    k_mutex_unlock(&g_modem_lock);

    if (err != 0)
    {
        shell_error(sh, "power down failed (%d)", err);
        return err;
    }
    shell_print(sh, "modem powered down");
    return 0;
}

int cmd_modem_at(const struct shell *sh, size_t argc, char **argv)
{
    // Quote commands containing spaces: modem at "AT+CGDCONT=1,\"IP\",\"apn\""
    int32_t timeout_ms = 5000;
    if (argc >= 3)
    {
        timeout_ms = static_cast<int32_t>(strtol(argv[2], nullptr, 10));
        if (timeout_ms < 1 || timeout_ms > 120000)
        {
            shell_error(sh, "timeout_ms must be 1..120000");
            return -EINVAL;
        }
    }

    const int claim = claim_modem(sh);
    if (claim != 0)
    {
        return claim;
    }

    if (!g_modem_powered)
    {
        k_mutex_unlock(&g_modem_lock);
        shell_error(sh, "modem is not powered - run \"modem on\" first");
        return -EACCES;
    }

    // required=false so a command answering ERROR, or one whose reply simply
    // is not "OK", still returns here with the full exchange already printed
    // by send_at() instead of being reported as a hard failure.
    RunState state{};
    state.t0 = k_uptime_get();
    flush_rx();
    const int err = send_at(state, argv[1], "OK", timeout_ms, false);
    k_mutex_unlock(&g_modem_lock);

    if (err != 0)
    {
        shell_warn(sh,
                   "no \"OK\" within %d ms - see the RX lines above for what "
                   "the modem actually returned",
                   timeout_ms);
    }
    return 0;
}

#endif // CONFIG_SHELL && CONFIG_FIREFLY_MODEM_TEST_NRF93M1
} // namespace

#if defined(CONFIG_SHELL) && defined(CONFIG_FIREFLY_MODEM_TEST_NRF93M1)
SHELL_STATIC_SUBCMD_SET_CREATE(
    modem_subcmds, SHELL_CMD_ARG(on, NULL, "Power the modem up and wait until it answers AT", cmd_modem_on, 1, 0),
    SHELL_CMD_ARG(off, NULL, "Power the modem down (AT%POWD=1, GPIO pulse fallback)", cmd_modem_off, 1, 0),
    SHELL_CMD_ARG(at, NULL,
                  "Send one AT command: modem at <command> [timeout_ms]\n"
                  "Quote commands containing spaces or commas with embedded quotes.",
                  cmd_modem_at, 2, 1),
    SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(modem, &modem_subcmds, "Raw-AT access to the nRF93M1 modem", NULL);
#endif

extern "C" int srvModemTest_nrf93m1_start(const struct zbus_observer *subscriber)
{
    return g_test.startTest(subscriber);
}
extern "C" int srvModemTest_nrf93m1_stop(void)
{
    return g_test.stop();
}
extern "C" bool srvModemTest_nrf93m1_is_running(void)
{
    return g_test.isRunning();
}
extern "C" int srvModemTest_nrf93m1_last_result(void)
{
    return g_test.lastResult();
}
extern "C" uint32_t srvModemTest_nrf93m1_processed_count(void)
{
    return g_test.count();
}
extern "C" int srvModemTest_nrf93m1_wait_processed(uint32_t previous_count, int32_t timeout_ms)
{
    return g_test.waitProcessed(previous_count, timeout_ms);
}

#ifdef CONFIG_FIREFLY_TEST_HOOKS
extern "C" bool srvModemTest_nrf93m1_modem_is_powered(void)
{
#ifdef CONFIG_FIREFLY_MODEM_TEST_NRF93M1
    return g_modem_powered;
#else
    return false;
#endif
}
#endif
