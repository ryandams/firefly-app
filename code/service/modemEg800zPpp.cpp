// SPDX-License-Identifier: Apache-2.0
#include "modemEg800zPpp.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(modemEg800z, CONFIG_FIREFLY_LOG_LEVEL);

#ifdef CONFIG_FIREFLY_NETWORK_TRANSPORT_EG800Z_PPP

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/modem/backend/uart.h>
#include <zephyr/modem/chat.h>
#include <zephyr/modem/pipe.h>
#include <zephyr/modem/ppp.h>
#include <zephyr/sys/atomic.h>
#include <errno.h>
#include <string.h>

extern "C" struct modem_ppp *modemEg800zPpp_ppp(void);

namespace {

// The modem node and its parent UART, exactly as srvModemTest_eg800z.cpp
// resolves them - with MODEM_CELLULAR off, nothing binds a driver to the
// "quectel,eg800z" node, so its properties are ours to use directly.
#define MODEM_NODE DT_ALIAS(modem)
#define MODEM_UART_NODE DT_PARENT(MODEM_NODE)

const struct device *const kUart = DEVICE_DT_GET(MODEM_UART_NODE);
// PWRKEY and RESET_N. This transport never pulses either of them - the module
// self-starts with the board (autostarts) and a >=650 ms PWRKEY pulse on a
// running module is the documented power-OFF action. They still have to be
// driven: both are GPIO_ACTIVE_LOW, so leaving them as high-impedance inputs
// lets the lines sit at or drift to their asserted level, which holds the
// module in reset (RESET_N) or keeps the power key pressed (PWRKEY). Both
// working paths on this board configure them as inactive outputs before any
// AT traffic - modem_cellular_init() in drivers/modem/modem_cellular.c and
// prepare_hardware() in srvModemTest_eg800z.cpp - and a first version of this
// file that configured only DTR and RI got no response to "AT" at all.
const struct gpio_dt_spec kPower = GPIO_DT_SPEC_GET(MODEM_NODE, mdm_power_gpios);
const struct gpio_dt_spec kReset = GPIO_DT_SPEC_GET(MODEM_NODE, mdm_reset_gpios);
const struct gpio_dt_spec kDtr = GPIO_DT_SPEC_GET(MODEM_NODE, mdm_dtr_gpios);
#if DT_NODE_HAS_PROP(MODEM_NODE, mdm_ring_gpios)
const struct gpio_dt_spec kRing = GPIO_DT_SPEC_GET(MODEM_NODE, mdm_ring_gpios);
#endif

// ---------------------------------------------------------------------------
//  Pipe, chat and PPP
// ---------------------------------------------------------------------------
// One UART pipe for the whole session. modem_chat and modem_ppp take turns
// attaching to it - that turn-taking IS the command-mode/data-mode model the
// PPP note describes, and it is only possible because both modules attach to
// any modem pipe (see the "Modem chat"/"Modem PPP" sections of Zephyr's
// modem docs, which say so explicitly).
//
// dtr_gpio is deliberately NOT handed to the backend config. The backend
// asserts DTR on pipe open and releases it on close, which is right for a
// modem where DTR only means "UART is live"; here AT&D1 makes a DTR
// low->high edge mean "leave data mode", so this file has to own the pin.
uint8_t g_uart_rx_buf[1024] __aligned(4);
uint8_t g_uart_tx_buf[1024] __aligned(4);
struct modem_backend_uart g_uart_backend;
struct modem_pipe *g_pipe;

// The modem_ppp instance itself lives in modemEg800zPppNet.c - see that
// file's comment for why it cannot be declared from C++.

uint8_t g_chat_rx_buf[256];
uint8_t *g_chat_argv[32];

// Longest response line we hand back from modemEg800zPpp_at().
constexpr size_t kResponseMax = 128;
char g_last_response[kResponseMax];
size_t g_last_response_len;

atomic_t g_in_data_mode;
atomic_t g_urc_count;
bool g_pipe_open;

void capture_response(struct modem_chat *, char **argv, uint16_t argc, void *)
{
    // argv[0] is the matched prefix; the remainder is the payload. Only the
    // first line of a multi-line response is kept - enough to read back
    // "+CSQ: 20,99" style answers without a parser per command.
    g_last_response_len = 0;
    g_last_response[0] = '\0';
    for (uint16_t i = 1; i < argc; ++i) {
        const size_t part = strlen(argv[i]);
        if (g_last_response_len + part + 2 >= kResponseMax) { break; }
        if (g_last_response_len != 0) { g_last_response[g_last_response_len++] = ','; }
        memcpy(&g_last_response[g_last_response_len], argv[i], part);
        g_last_response_len += part;
        g_last_response[g_last_response_len] = '\0';
    }
}

// Registration state, filled in by the +CREG/+CGREG/+CEREG URC and read-command
// responses. PPP note section 3.2: "It indicates that the module has registered
// on network when AT+CREG? returns 1 or 5" - same rule for the PS domain via
// AT+CGREG?.
atomic_t g_registered_cs;
atomic_t g_registered_ps;

bool stat_is_registered(const char *value)
{
    // <stat> 1 = registered home, 5 = registered roaming.
    return value != nullptr && value[0] != '\0' && value[1] == '\0' &&
           (value[0] == '1' || value[0] == '5');
}

void on_cxreg(struct modem_chat *, char **argv, uint16_t argc, void *user_data)
{
    // "+CREG: <n>,<stat>" for the read command, "+CREG: <stat>" for the URC -
    // take the last field either way, which is <stat> in both forms.
    if (argc < 2) { return; }
    const bool registered = stat_is_registered(argv[argc - 1]);
    atomic_set(static_cast<atomic_t *>(user_data), registered ? 1 : 0);
}

void on_creg(struct modem_chat *chat, char **argv, uint16_t argc, void *)
{
    on_cxreg(chat, argv, argc, &g_registered_cs);
}

void on_cgreg(struct modem_chat *chat, char **argv, uint16_t argc, void *)
{
    on_cxreg(chat, argv, argc, &g_registered_ps);
}

void on_no_carrier(struct modem_chat *, char **, uint16_t, void *)
{
    // PPP note section 3.2: "The module will report NO CARRIER when PPP
    // connection is terminated." That is also how the module tells us it has
    // left data mode on its own.
    atomic_set(&g_in_data_mode, 0);
    LOG_INF("NO CARRIER - module left data mode");
}

MODEM_CHAT_MATCHES_DEFINE(g_unsol,
                          MODEM_CHAT_MATCH("+CREG: ", ",", on_creg),
                          MODEM_CHAT_MATCH("+CGREG: ", ",", on_cgreg),
                          MODEM_CHAT_MATCH("+CEREG: ", ",", on_cgreg),
                          MODEM_CHAT_MATCH("NO CARRIER", "", on_no_carrier),
                          MODEM_CHAT_MATCH("RDY", "", NULL),
                          MODEM_CHAT_MATCH("POWERED DOWN", "", NULL));

struct modem_chat g_chat;

// "\r" out, "\n" filtered on the way in - the same delimiter/filter pair the
// EG800Z entry in cellular_quectel_eg800z.c uses for this module.
const struct modem_chat_config kChatConfig = {
    .user_data = nullptr,
    .receive_buf = g_chat_rx_buf,
    .receive_buf_size = sizeof(g_chat_rx_buf),
    .delimiter = (uint8_t *)"\r",
    .delimiter_size = 1,
    .filter = (uint8_t *)"\n",
    .filter_size = 1,
    .argv = g_chat_argv,
    .argv_size = ARRAY_SIZE(g_chat_argv),
    .unsol_matches = g_unsol,
    .unsol_matches_size = ARRAY_SIZE(g_unsol),
};

MODEM_CHAT_MATCH_DEFINE(kOk, "OK", "", NULL);
MODEM_CHAT_MATCH_DEFINE(kConnect, "CONNECT", "", NULL);
MODEM_CHAT_MATCH_DEFINE(kCpinReady, "+CPIN: READY", "", NULL);
MODEM_CHAT_MATCH_DEFINE(kAny, "", "", capture_response);
MODEM_CHAT_MATCHES_DEFINE(kAbort, MODEM_CHAT_MATCH("ERROR", "", NULL));
MODEM_CHAT_MATCHES_DEFINE(kDialAbort,
                          MODEM_CHAT_MATCH("ERROR", "", NULL),
                          MODEM_CHAT_MATCH("BUSY", "", NULL),
                          MODEM_CHAT_MATCH("NO ANSWER", "", NULL),
                          MODEM_CHAT_MATCH("NO CARRIER", "", NULL),
                          MODEM_CHAT_MATCH("NO DIALTONE", "", NULL));

} // namespace

namespace {

// ---------------------------------------------------------------------------
//  Chat scripts - PPP note section 3.2's recommended procedure, in order
// ---------------------------------------------------------------------------
// Synchronisation. Note section 3.2: "MCU sends AT<CR><LF> to the module
// every 100 ms until OK is received." modem_chat sends each request once per
// script run, so the 100 ms cadence becomes "re-run this one-second script
// until it succeeds" in sync_uart() below - the same effect at coarser
// granularity, since a script timeout is expressed in whole seconds.
MODEM_CHAT_SCRIPT_CMDS_DEFINE(kSyncCmds, MODEM_CHAT_SCRIPT_CMD_RESP("AT", kOk));
MODEM_CHAT_SCRIPT_DEFINE(kSyncScript, kSyncCmds, kAbort, NULL, 1);

// AT+IPR=<rate>;&W from the note is deliberately not sent: it writes the
// baud rate to the module's NVM, and this UART is fixed at 115200 on both
// sides by the board overlay's current-speed, so the only thing a &W would
// add is an NVM write on every link_up().
MODEM_CHAT_SCRIPT_CMDS_DEFINE(
    kInitCmds,
    MODEM_CHAT_SCRIPT_CMD_RESP("ATE0", kOk),
    MODEM_CHAT_SCRIPT_CMD_RESP("AT+CMEE=1", kOk),
    // AT manual, "AT+IFC Set UART Flow Control": 2,2 = RTS/CTS both ways,
    // matching hw-flow-control on this UART node.
    MODEM_CHAT_SCRIPT_CMD_RESP("AT+IFC=2,2", kOk),
    // AT&C1: DCD follows the data carrier (PPP note section 4.3).
    MODEM_CHAT_SCRIPT_CMD_RESP("AT&C1", kOk),
    // AT&D1: a DTR low->high edge keeps the call and switches to command
    // mode (AT manual, "AT&D Set DTR Function Mode", value 1). This is what
    // enter_command_mode_dtr() relies on. Not AT&D2, which would drop the
    // call on the same edge.
    MODEM_CHAT_SCRIPT_CMD_RESP("AT&D1", kOk),
    // Pin the RI URC pulse rather than inheriting it. AT manual sections
    // 4.4.6/4.4.7: <typeRI>,<pulse_duration>,<pulse_count>.
    MODEM_CHAT_SCRIPT_CMD_RESP(
        "AT+QCFG=\"urc/ri/other\",\"pulse\","
        STRINGIFY(CONFIG_FIREFLY_NETWORK_EG800Z_RI_PULSE_MS) ",1", kOk),
    MODEM_CHAT_SCRIPT_CMD_RESP(
        "AT+QCFG=\"urc/ri/smsincoming\",\"pulse\","
        STRINGIFY(CONFIG_FIREFLY_NETWORK_EG800Z_RI_PULSE_MS) ",1", kOk),
    // Registration URCs on, so on_creg/on_cgreg see changes without polling.
    MODEM_CHAT_SCRIPT_CMD_RESP("AT+CREG=1", kOk),
    MODEM_CHAT_SCRIPT_CMD_RESP("AT+CGREG=1", kOk),
    // Full function mode - RF must be on to register. The module was already
    // at CFUN=1 in every observed run; setting it again returns OK.
    MODEM_CHAT_SCRIPT_CMD_RESP("AT+CFUN=1", kOk));
MODEM_CHAT_SCRIPT_DEFINE(kInitScript, kInitCmds, kAbort, NULL, 30);

// Note section 3.2: the SIM must be checked with AT+CPIN? before dialling.
// A ready SIM answers "+CPIN: READY"; anything else (+CME ERROR: 13 for a
// missing card) matches nothing and the script times out, which is the
// retry signal wait_for_sim() wants.
MODEM_CHAT_SCRIPT_CMDS_DEFINE(kSimCmds, MODEM_CHAT_SCRIPT_CMD_RESP("AT+CPIN?", kCpinReady));
MODEM_CHAT_SCRIPT_DEFINE(kSimScript, kSimCmds, kAbort, NULL, 2);

// The +CREG:/+CGREG: response lines are picked up by the unsolicited
// matches (modem_chat evaluates those during a script too), so these only
// need to wait for the trailing OK.
MODEM_CHAT_SCRIPT_CMDS_DEFINE(kRegCmds, MODEM_CHAT_SCRIPT_CMD_RESP("AT+CREG?", kOk),
                              MODEM_CHAT_SCRIPT_CMD_RESP("AT+CGREG?", kOk));
MODEM_CHAT_SCRIPT_DEFINE(kRegScript, kRegCmds, kAbort, NULL, 5);

// Note section 3.1: set the APN with AT+CGDCONT, then ATD*99#. The short
// unmatched "AT" is the same guard cellular_quectel_eg800z.c uses - dialling
// immediately after CGDCONT draws NO CARRIER on some units.
MODEM_CHAT_SCRIPT_CMDS_DEFINE(
    kDialCmds,
    MODEM_CHAT_SCRIPT_CMD_RESP("AT+CGDCONT=1,\"IP\",\"" CONFIG_FIREFLY_NETWORK_APN "\"", kOk),
    MODEM_CHAT_SCRIPT_CMD_RESP_NONE("AT", 500),
    MODEM_CHAT_SCRIPT_CMD_RESP("ATD*99#", kConnect));
MODEM_CHAT_SCRIPT_DEFINE(kDialScript, kDialCmds, kDialAbort, NULL, 30);

// ATO returns to data mode and answers CONNECT (note section 4.1.2.1).
MODEM_CHAT_SCRIPT_CMDS_DEFINE(kResumeCmds, MODEM_CHAT_SCRIPT_CMD_RESP("ATO", kConnect));
MODEM_CHAT_SCRIPT_DEFINE(kResumeScript, kResumeCmds, kDialAbort, NULL, 10);

// Backing storage for the one-off command run by modemEg800zPpp_at().
char g_at_request[64];
struct modem_chat_script_chat g_at_chats[1];
struct modem_chat_script g_at_script;

// ---------------------------------------------------------------------------
//  Helpers
// ---------------------------------------------------------------------------
int run_script(const struct modem_chat_script *script)
{
    return modem_chat_run_script(&g_chat, script);
}

// Re-runs the one-command sync script until the module answers or the
// deadline passes. See kSyncScript's comment for why this is a loop.
int sync_uart(int32_t timeout_ms)
{
    const int64_t deadline = k_uptime_get() + timeout_ms;
    do {
        if (run_script(&kSyncScript) == 0) { return 0; }
        k_sleep(K_MSEC(100));
    } while (k_uptime_get() < deadline);
    LOG_ERR("no AT/OK from module within %d ms", timeout_ms);
    return -ETIMEDOUT;
}

int wait_for_sim(void)
{
    const int64_t deadline =
        k_uptime_get() + CONFIG_FIREFLY_NETWORK_EG800Z_SIM_TIMEOUT_S * MSEC_PER_SEC;
    do {
        if (run_script(&kSimScript) == 0) {
            LOG_INF("SIM ready");
            return 0;
        }
        k_sleep(K_MSEC(500));
    } while (k_uptime_get() < deadline);
    LOG_ERR("SIM not ready within %d s", CONFIG_FIREFLY_NETWORK_EG800Z_SIM_TIMEOUT_S);
    return -ENODEV;
}

// Note section 3.2 treats the two domains differently: it reboots the module
// if CS registration fails within 60 s, but for the PS domain says "It is
// able to go to next step without registering on PS in 60s". So a CS
// timeout is an error and a PS timeout is only a warning.
int wait_for_registration(void)
{
    const int64_t deadline =
        k_uptime_get() + CONFIG_FIREFLY_NETWORK_EG800Z_REG_TIMEOUT_S * MSEC_PER_SEC;
    atomic_set(&g_registered_cs, 0);
    atomic_set(&g_registered_ps, 0);
    do {
        (void)run_script(&kRegScript);
        if (atomic_get(&g_registered_cs) == 1 && atomic_get(&g_registered_ps) == 1) {
            LOG_INF("registered (CS and PS)");
            return 0;
        }
        k_sleep(K_SECONDS(1));
    } while (k_uptime_get() < deadline);

    if (atomic_get(&g_registered_cs) != 1) {
        LOG_ERR("no network registration within %d s",
                CONFIG_FIREFLY_NETWORK_EG800Z_REG_TIMEOUT_S);
        return -ENETUNREACH;
    }
    LOG_WRN("PS domain not registered within %d s - dialling anyway, as the PPP "
            "application note's flowchart allows",
            CONFIG_FIREFLY_NETWORK_EG800Z_REG_TIMEOUT_S);
    return 0;
}

} // namespace

namespace {

// ---------------------------------------------------------------------------
//  Data mode <-> command mode  (PPP note section 4.1)
// ---------------------------------------------------------------------------
// DTR is asserted (logical 1) for data mode. GPIO_ACTIVE_LOW in the board
// overlay makes that a physical low, which is what this module wants: the AT
// manual's AT+QSCLK entry says it can only sleep while MAIN_DTR is high, so
// low is the awake/attached state.
void dtr_set_data_mode(bool data_mode)
{
    (void)gpio_pin_set_dt(&kDtr, data_mode ? 1 : 0);
}

// Leaves data mode by driving DTR low->high, which AT&D1 turns into "keep the
// connected call and change to command mode". Cheaper and less fragile than
// the "+++" escape - no guard timing at all - so this is what the RI path
// uses. modem_ppp has to be released first: whoever is attached to the pipe
// owns its receive callback, and we need modem_chat to see the OK.
int enter_command_mode_dtr(void)
{
    modem_ppp_release(modemEg800zPpp_ppp());
    int err = modem_chat_attach(&g_chat, g_pipe);
    if (err != 0) {
        LOG_ERR("chat attach failed (%d)", err);
        return err;
    }
    dtr_set_data_mode(false);

    // The module answers OK to the edge itself; rather than matching that
    // asynchronous OK, confirm command mode positively by getting a fresh
    // AT/OK exchange. A module still in data mode would treat "AT" as PPP
    // payload and never answer, so this is a real check, not a formality.
    err = sync_uart(2000);
    if (err != 0) {
        LOG_ERR("DTR edge did not reach command mode (%d)", err);
        dtr_set_data_mode(true);
        modem_chat_release(&g_chat);
        (void)modem_ppp_attach(modemEg800zPpp_ppp(), g_pipe);
        return err;
    }
    return 0;
}

// The "+++" escape (note section 4.1.1.2). Used by modemEg800zPpp_at() so an
// on-demand AT command does not depend on DTR, which AT+QSCLK would also
// like to own.
//
// "+++" must go out with no trailing delimiter: the note requires no
// character at all for 1 s afterwards, and modem_chat appends the configured
// delimiter ("\r") to every script request. So it is written straight to the
// pipe with modem_pipe_transmit() while modem_chat stays attached for the
// receive side - transmitting on a pipe is independent of who is attached
// for receive.
int enter_command_mode_escape(void)
{
    modem_ppp_release(modemEg800zPpp_ppp());
    int err = modem_chat_attach(&g_chat, g_pipe);
    if (err != 0) {
        LOG_ERR("chat attach failed (%d)", err);
        return err;
    }

    static const uint8_t kEscape[] = {'+', '+', '+'};
    k_sleep(K_MSEC(1100));  // guard: no traffic for >1 s before
    const int sent = modem_pipe_transmit(g_pipe, kEscape, sizeof(kEscape));
    if (sent != (int)sizeof(kEscape)) {
        LOG_ERR("failed to transmit escape sequence (%d)", sent);
        modem_chat_release(&g_chat);
        (void)modem_ppp_attach(modemEg800zPpp_ppp(), g_pipe);
        return (sent < 0) ? sent : -EIO;
    }
    k_sleep(K_MSEC(1100));  // guard: no traffic for >1 s after

    err = sync_uart(2000);
    if (err != 0) {
        LOG_ERR("escape sequence did not reach command mode (%d)", err);
        modem_chat_release(&g_chat);
        (void)modem_ppp_attach(modemEg800zPpp_ppp(), g_pipe);
        return err;
    }
    return 0;
}

// ATO back to data mode, then hand the pipe back to modem_ppp. DTR returns to
// its asserted (data mode) level first, so the line is not left sitting in
// the "switch to command mode" state while PPP frames flow again.
int exit_command_mode(void)
{
    dtr_set_data_mode(true);
    const int err = run_script(&kResumeScript);
    modem_chat_release(&g_chat);

    const int attach_err = modem_ppp_attach(modemEg800zPpp_ppp(), g_pipe);
    if (attach_err != 0) {
        LOG_ERR("modem_ppp_attach failed (%d)", attach_err);
        return attach_err;
    }
    if (err != 0) {
        LOG_ERR("ATO did not return CONNECT (%d) - PPP may be dead", err);
        atomic_set(&g_in_data_mode, 0);
    }
    return err;
}

// ---------------------------------------------------------------------------
//  RI-driven URC handling  (PPP note section 4.2)
// ---------------------------------------------------------------------------
#if DT_NODE_HAS_PROP(MODEM_NODE, mdm_ring_gpios)

K_THREAD_STACK_DEFINE(g_ri_stack, CONFIG_FIREFLY_NETWORK_EG800Z_RI_WORKQ_STACK_SIZE);
struct k_work_q g_ri_workq;
struct k_work g_ri_work;
struct gpio_callback g_ri_cb;

// A URC arriving in data mode is not written to the port; the module pulses
// RI from high to low for <pulse_duration> ms instead (note section 4.2, and
// AT+QCFG="urc/ri/other" above pins that to
// CONFIG_FIREFLY_NETWORK_EG800Z_RI_PULSE_MS). Switching to command mode makes
// the module report the URC "if the incoming short message still exists".
void ri_work_handler(struct k_work *)
{
    // Confirm the pulse actually ended rather than acting on any falling
    // edge. Waiting slightly longer than the configured width and then
    // requiring the line back at its inactive level distinguishes the URC
    // pulse from AT+QCFG="urc/ri/ring"'s "auto" behaviour, which holds RI
    // down for the whole duration of an incoming call.
    k_sleep(K_MSEC(CONFIG_FIREFLY_NETWORK_EG800Z_RI_PULSE_MS + 40));
    if (gpio_pin_get_dt(&kRing) != 0) {
        LOG_DBG("RI still asserted after %d ms - not a URC pulse, ignoring",
                CONFIG_FIREFLY_NETWORK_EG800Z_RI_PULSE_MS);
        return;
    }

    if (atomic_get(&g_in_data_mode) != 1) {
        // In command mode the URC is delivered to the port directly and the
        // unsolicited matches have already seen it. Nothing to do.
        LOG_DBG("RI pulse while in command mode - URC already reported");
        return;
    }

    LOG_INF("RI pulse - switching to command mode to drain URC");
    if (enter_command_mode_dtr() != 0) { return; }

    // Give the module a moment to re-report the pending URC now that the
    // port can carry it. Anything it sends is matched by g_unsol, so the
    // registration/NO CARRIER callbacks above run here as a side effect.
    k_sleep(K_MSEC(500));

    if (exit_command_mode() == 0) {
        atomic_inc(&g_urc_count);
        LOG_INF("URC drained, back in data mode");
    }
}

void ri_gpio_callback(const struct device *, struct gpio_callback *, uint32_t)
{
    // Straight to a dedicated workqueue: the handler blocks in AT exchanges
    // for hundreds of milliseconds, which must not happen in an ISR nor on
    // the system workqueue.
    k_work_submit_to_queue(&g_ri_workq, &g_ri_work);
}

int ri_init(void)
{
    static bool initialised;
    if (initialised) { return 0; }

    if (!gpio_is_ready_dt(&kRing)) {
        LOG_ERR("RI GPIO not ready");
        return -ENODEV;
    }
    int err = gpio_pin_configure_dt(&kRing, GPIO_INPUT);
    if (err != 0) { return err; }

    k_work_queue_init(&g_ri_workq);
    k_work_queue_start(&g_ri_workq, g_ri_stack, K_THREAD_STACK_SIZEOF(g_ri_stack),
                       K_PRIO_PREEMPT(10), NULL);
    k_thread_name_set(k_work_queue_thread_get(&g_ri_workq), "eg800z_ri");
    k_work_init(&g_ri_work, ri_work_handler);

    gpio_init_callback(&g_ri_cb, ri_gpio_callback, BIT(kRing.pin));
    err = gpio_add_callback(kRing.port, &g_ri_cb);
    if (err != 0) { return err; }

    // Edge to active: with mdm-ring-gpios declared GPIO_ACTIVE_LOW this is
    // the high->low transition the note describes.
    err = gpio_pin_interrupt_configure_dt(&kRing, GPIO_INT_EDGE_TO_ACTIVE);
    if (err != 0) { return err; }

    initialised = true;
    return 0;
}

#else /* !DT_NODE_HAS_PROP(MODEM_NODE, mdm_ring_gpios) */

int ri_init(void)
{
    LOG_WRN("no mdm-ring-gpios in the devicetree - URCs arriving during PPP "
            "data mode cannot be detected (PPP note section 4.2)");
    return 0;
}

#endif

} // namespace

// ---------------------------------------------------------------------------
//  Public API
// ---------------------------------------------------------------------------
int modemEg800zPpp_link_up(void)
{
    if (!device_is_ready(kUart)) {
        LOG_ERR("modem UART not ready");
        return -ENODEV;
    }
    if (!gpio_is_ready_dt(&kDtr) || !gpio_is_ready_dt(&kPower) || !gpio_is_ready_dt(&kReset)) {
        LOG_ERR("modem control GPIOs not ready");
        return -ENODEV;
    }

    // Deassert PWRKEY and RESET_N before anything else. Never pulsed here,
    // but they must be actively held inactive rather than floating - see the
    // kPower/kReset declaration comment.
    int err = gpio_pin_configure_dt(&kPower, GPIO_OUTPUT_INACTIVE);
    if (err != 0) { return err; }
    err = gpio_pin_configure_dt(&kReset, GPIO_OUTPUT_INACTIVE);
    if (err != 0) { return err; }

    // DTR asserted from the outset: the module must be awake and in the
    // "stay in data mode" state before any AT traffic.
    err = gpio_pin_configure_dt(&kDtr, GPIO_OUTPUT_ACTIVE);
    if (err != 0) { return err; }

    err = ri_init();
    if (err != 0) { return err; }

    if (!g_pipe_open) {
        const struct modem_backend_uart_config backend_config = {
            .uart = kUart,
            // Not backend-owned: this file drives DTR for mode switching.
            .dtr_gpio = nullptr,
            .receive_buf = g_uart_rx_buf,
            .receive_buf_size = sizeof(g_uart_rx_buf),
            .transmit_buf = g_uart_tx_buf,
            .transmit_buf_size = sizeof(g_uart_tx_buf),
        };
        g_pipe = modem_backend_uart_init(&g_uart_backend, &backend_config);
        if (g_pipe == nullptr) {
            LOG_ERR("modem_backend_uart_init failed");
            return -EIO;
        }
        err = modem_chat_init(&g_chat, &kChatConfig);
        if (err != 0) {
            LOG_ERR("modem_chat_init failed (%d)", err);
            return err;
        }
        err = modem_pipe_open(g_pipe, K_SECONDS(5));
        if (err != 0) {
            LOG_ERR("modem_pipe_open failed (%d)", err);
            return err;
        }
        g_pipe_open = true;
    }

    err = modem_chat_attach(&g_chat, g_pipe);
    if (err != 0) {
        LOG_ERR("modem_chat_attach failed (%d)", err);
        return err;
    }

    // PPP note section 3.2, in order. No PWRKEY pulse anywhere: the module
    // self-starts with the board, and a >=650 ms pulse on a running module is
    // the documented power-OFF action.
    err = sync_uart(10000);
    if (err != 0) { goto detach; }

    err = run_script(&kInitScript);
    if (err != 0) {
        LOG_ERR("init script failed (%d)", err);
        goto detach;
    }

    err = wait_for_sim();
    if (err != 0) { goto detach; }

    err = wait_for_registration();
    if (err != 0) { goto detach; }

    err = run_script(&kDialScript);
    if (err != 0) {
        LOG_ERR("ATD*99# did not return CONNECT (%d)", err);
        goto detach;
    }

    // CONNECT means the port is now in data mode and every byte is a PPP
    // frame, so modem_chat must let go before modem_ppp takes over.
    modem_chat_release(&g_chat);
    err = modem_ppp_attach(modemEg800zPpp_ppp(), g_pipe);
    if (err != 0) {
        LOG_ERR("modem_ppp_attach failed (%d)", err);
        return err;
    }
    atomic_set(&g_in_data_mode, 1);
    atomic_set(&g_urc_count, 0);
    LOG_INF("CONNECT - PPP data mode, ready for net_if_up()");
    return 0;

detach:
    modem_chat_release(&g_chat);
    return err;
}

int modemEg800zPpp_link_down(void)
{
    // The caller has already run net_if_down(), which makes Zephyr's PPP L2
    // send the LCP Terminate-Request that note section 5 recommends. The
    // module answers NO CARRIER and returns to command mode by itself; the
    // on_no_carrier() unsolicited match clears g_in_data_mode when it does.
    if (!g_pipe_open) { return 0; }

    modem_ppp_release(modemEg800zPpp_ppp());

    int err = modem_chat_attach(&g_chat, g_pipe);
    if (err == 0) {
        // Note section 5 gives the MCU 3 s to see the Terminate-Ack. If the
        // module has not dropped the carrier by then, force the issue with
        // the DTR edge rather than leaving it in data mode.
        for (int i = 0; i < 6 && atomic_get(&g_in_data_mode) == 1; ++i) {
            k_sleep(K_MSEC(500));
        }
        if (atomic_get(&g_in_data_mode) == 1) {
            LOG_WRN("no NO CARRIER after 3 s - forcing command mode via DTR");
            dtr_set_data_mode(false);
            k_sleep(K_MSEC(100));
            dtr_set_data_mode(true);
            atomic_set(&g_in_data_mode, 0);
        }
        modem_chat_release(&g_chat);
    }

    // Left powered and in command mode on purpose. Powering it down with
    // AT+QPOWD would need a PWRKEY pulse to bring it back, and on this board
    // that same pulse width is the power-OFF action - see the autostarts
    // comment in the board overlay.
    err = modem_pipe_close(g_pipe, K_SECONDS(5));
    if (err != 0) { LOG_WRN("modem_pipe_close failed (%d)", err); }
    g_pipe_open = false;
    return 0;
}

int modemEg800zPpp_at(const char *command, char *response, size_t response_size)
{
    if (command == nullptr) { return -EINVAL; }
    if (atomic_get(&g_in_data_mode) != 1) {
        // Refused rather than attempted: note section 4.1.1.2 warns the
        // escape sequence tears down an in-progress negotiation, so running
        // it before the link is up would break the very thing it is for.
        LOG_WRN("modemEg800zPpp_at() needs an established PPP link");
        return -EAGAIN;
    }
    if (strlen(command) >= sizeof(g_at_request)) { return -E2BIG; }

    int err = enter_command_mode_escape();
    if (err != 0) { return err; }

    strncpy(g_at_request, command, sizeof(g_at_request) - 1);
    g_at_request[sizeof(g_at_request) - 1] = '\0';
    g_last_response[0] = '\0';
    g_last_response_len = 0;

    modem_chat_script_chat_init(&g_at_chats[0]);
    (void)modem_chat_script_chat_set_request(&g_at_chats[0], g_at_request);
    (void)modem_chat_script_chat_set_response_matches(&g_at_chats[0], &kAny, 1);

    modem_chat_script_init(&g_at_script);
    modem_chat_script_set_name(&g_at_script, "eg800z_at");
    (void)modem_chat_script_set_script_chats(&g_at_script, g_at_chats, ARRAY_SIZE(g_at_chats));
    (void)modem_chat_script_set_abort_matches(&g_at_script, kAbort, ARRAY_SIZE(kAbort));
    modem_chat_script_set_timeout(&g_at_script, 5);

    err = run_script(&g_at_script);
    if (err == 0 && response != nullptr && response_size > 0) {
        strncpy(response, g_last_response, response_size - 1);
        response[response_size - 1] = '\0';
    }

    const int resume_err = exit_command_mode();
    return (err != 0) ? err : resume_err;
}

unsigned int modemEg800zPpp_urc_count(void)
{
    return static_cast<unsigned int>(atomic_get(&g_urc_count));
}

#else /* !CONFIG_FIREFLY_NETWORK_TRANSPORT_EG800Z_PPP */

#include <errno.h>

int modemEg800zPpp_link_up(void) { return -ENOSYS; }
int modemEg800zPpp_link_down(void) { return 0; }
int modemEg800zPpp_at(const char *, char *, size_t) { return -ENOSYS; }
unsigned int modemEg800zPpp_urc_count(void) { return 0; }

#endif
