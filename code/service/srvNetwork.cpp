// SPDX-License-Identifier: Apache-2.0
//
// srvNetwork - fully power-cycled per round. srvNetwork_start()/stop()
// (network_port::power_on/power_off) do not touch the modem or its power
// domain at all - the modem stays completely unpowered from service start
// until the first button0 press. Each button0 press then runs one full
// round: energize the modem's power domain/rail where one exists, resume
// the modem device (this is what actually starts the power pulse/AT
// traffic - see network_port::connect), bring the PPP link up, send one
// UDP message, bring the PPP link back down, suspend the modem device, and
// release the rail again (network_port::disconnect).
//
// The AT traffic is deliberately gated behind the button press rather than
// starting automatically at boot, to support power measurement setups that
// need a precise, on-demand starting point and zero idle current between
// rounds (an energized-but-idle rail was observed to draw ~3 mA on
// nrf93m1dk). Each round fully power-cycles the modem rather than leaving
// it dialed between rounds: on real hardware an idle PPP call gets dropped
// by the network within a few seconds regardless, and reconnecting to an
// already-dead call causes CMUX buffer overruns and failed renegotiation -
// a full cycle per round is slower but is what has proven reliable. Runs
// on its own persistent thread since waiting for cellular registration can
// legitimately take up to CONFIG_FIREFLY_NETWORK_CONNECT_TIMEOUT_S seconds.
//
// network_port mirrors myLeds.h's firefly_led_port: the real port drives
// the actual modem/PPP/socket stack (only compiled in when
// CONFIG_FIREFLY_NETWORK is set - see below), and CONFIG_FIREFLY_TEST_HOOKS
// lets tests substitute a fake one to exercise srvNetwork's own control
// flow without any real hardware.
#include "srvNetwork.h"
#include "event_bus.h"
#include "thread.hpp"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/printk.h>
#include <zephyr/zbus/zbus.h>
#include <errno.h>

#ifdef CONFIG_SEGGER_SYSTEMVIEW
#include <SEGGER_SYSVIEW.h>
#endif

#ifdef CONFIG_FIREFLY_NETWORK
#include <zephyr/device.h>
#include <zephyr/drivers/cellular.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_event.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/ppp.h>
#include <zephyr/pm/device_runtime.h>
#include "modemEg800zPpp.h"

// Plain POSIX names (socket/sendto/close) rather than <zephyr/net/socket.h>'s
// zsock_*-only API - matches zephyr/samples/net/cellular_modem, and unlike
// that header these need CONFIG_POSIX_API=y plus the dedicated POSIX headers
// on every target, not just ones where the host libc happens to already
// declare them.
#include <zephyr/posix/netinet/in.h>
#include <zephyr/posix/sys/socket.h>
#include <zephyr/posix/sys/time.h>
#include <zephyr/posix/unistd.h>
#endif

LOG_MODULE_REGISTER(srvNetwork, CONFIG_FIREFLY_LOG_LEVEL);

namespace
{

int64_t g_round_start_ms;

// SystemView view of a round (CONFIG_SEGGER_SYSTEMVIEW; no-ops otherwise).
// Every trace_point() also lands in SystemView's message log, and each phase
// below is a named marker span on its timeline, next to the thread and ISR
// activity it caused. The spans match report()'s PHASE BREAKDOWN.
enum Span : unsigned
{
    SPAN_ROUND,
    SPAN_MODEM_RESUME,
    SPAN_AWAIT_L4,
    SPAN_SEND,
    SPAN_ECHO_WAIT,
    SPAN_IF_DOWN,
    SPAN_MODEM_SUSPEND,
};

#ifdef CONFIG_SEGGER_SYSTEMVIEW
const char *const kSpanNames[] = {
    "network round", "modem resume", "await L4 (register/PDP/PPP)", "send", "echo wait", "iface down", "modem suspend",
};
#endif

// Opens a span until end() or the end of its scope, so an early error
// return still closes it.
class SysviewSpan
{
  public:
    explicit SysviewSpan(Span span) : span_(span)
    {
#ifdef CONFIG_SEGGER_SYSTEMVIEW
        // Named every time: a name sent before recording started is lost.
        SEGGER_SYSVIEW_NameMarker(span_, kSpanNames[span_]);
        SEGGER_SYSVIEW_MarkStart(span_);
#endif
    }
    ~SysviewSpan()
    {
        end();
    }
    SysviewSpan(const SysviewSpan &) = delete;
    SysviewSpan &operator=(const SysviewSpan &) = delete;

    void end()
    {
        if (open_)
        {
#ifdef CONFIG_SEGGER_SYSTEMVIEW
            SEGGER_SYSVIEW_MarkStop(span_);
#endif
            open_ = false;
        }
    }

  private:
    [[maybe_unused]] Span span_;
    bool open_ = true;
};

// Shown in SystemView's log as an error, for a round that failed.
void sysview_error(const char *text)
{
#ifdef CONFIG_SEGGER_SYSTEMVIEW
    SEGGER_SYSVIEW_Error(text);
#else
    ARG_UNUSED(text);
#endif
}

void trace_point(const char *stage)
{
    const int64_t now = k_uptime_get();
    LOG_INF("NETTRACE t=%lld ms elapsed=%lld ms | %s", static_cast<long long>(now),
            static_cast<long long>(now - g_round_start_ms), stage);
#ifdef CONFIG_SEGGER_SYSTEMVIEW
    SEGGER_SYSVIEW_Print(stage);
#endif
}

#ifdef CONFIG_FIREFLY_NETWORK

constexpr uint32_t kL4Connected = 1;

K_EVENT_DEFINE(g_l4_event);

// Mirrors srvModemTest_nrf93m1's RunState/report() so the two implementations'
// timings can be compared line-for-line. srvNetwork can't see the individual
// AT phases (registration, PDP attach, PPP LCP/IPCP) the raw-AT test does -
// Zephyr only surfaces the single NET_EVENT_L4_CONNECTED once all of that is
// done - so those are collapsed into one "iface up -> L4 connected" span.
enum Mark : size_t
{
    MODEM_RESUME_DONE,
    IF_UP_DONE,
    L4_CONNECTED,
    SOCK_CREATED,
    SENT,
    DISCONNECT_BEGIN,
    IF_DOWN_DONE,
    MODEM_SUSPEND_DONE,
    kMarkCount
};

struct RoundMarks
{
    int64_t t[kMarkCount];
    bool valid[kMarkCount];
};

RoundMarks g_marks;

void mark(Mark m)
{
    g_marks.t[m] = k_uptime_get();
    g_marks.valid[m] = true;
}

// Formats a millisecond duration as "%lld.%03lld" without pulling in
// CBPRINTF_FP_SUPPORT, which isn't guaranteed to be enabled for every board
// srvNetwork runs on (srvModemTest_nrf93m1 can afford it - it only builds
// for the one board that already turns it on).
void format_seconds(char *buf, size_t len, int64_t ms)
{
    snprintk(buf, len, "%lld.%03lld", static_cast<long long>(ms / 1000), static_cast<long long>(ms % 1000));
}

void report_phase(const char *label, Mark a, Mark b)
{
    if (g_marks.valid[a] && g_marks.valid[b])
    {
        char buf[32];
        format_seconds(buf, sizeof(buf), g_marks.t[b] - g_marks.t[a]);
        LOG_INF("PHASE %-34s %8s s", label, buf);
    }
    else
    {
        LOG_INF("PHASE %-34s %8s", label, "n/a");
    }
}

void report(int64_t round_start_ms)
{
    LOG_INF("====================================================================");
    LOG_INF("PHASE BREAKDOWN");
    LOG_INF("====================================================================");
    report_phase("iface up -> L4 connected", IF_UP_DONE, L4_CONNECTED);
    report_phase("L4 connected -> socket ready", L4_CONNECTED, SOCK_CREATED);
    report_phase("socket ready -> payload sent", SOCK_CREATED, SENT);
    report_phase("--- uplink path total ---", MODEM_RESUME_DONE, SENT);
    report_phase("sent -> teardown start (settle)", SENT, DISCONNECT_BEGIN);
    report_phase("teardown: iface down", DISCONNECT_BEGIN, IF_DOWN_DONE);
    report_phase("iface down -> modem suspended", IF_DOWN_DONE, MODEM_SUSPEND_DONE);
    report_phase("--- teardown total ---", DISCONNECT_BEGIN, MODEM_SUSPEND_DONE);
    LOG_INF("--------------------------------------------------------------------");
    if (g_marks.valid[MODEM_SUSPEND_DONE])
    {
        char buf[32];
        format_seconds(buf, sizeof(buf), g_marks.t[MODEM_SUSPEND_DONE] - round_start_ms);
        LOG_INF("%-38s %8s s", "CONNECTION WINDOW (button->suspended)", buf);
        if (g_marks.valid[SENT] && g_marks.valid[DISCONNECT_BEGIN])
        {
            const int64_t settle = g_marks.t[DISCONNECT_BEGIN] - g_marks.t[SENT];
            format_seconds(buf, sizeof(buf), g_marks.t[MODEM_SUSPEND_DONE] - round_start_ms - settle);
            LOG_INF("  minus send settle (artificial delay) %8s s   <== use this for comparison", buf);
        }
    }
    LOG_INF("====================================================================");
}

void l4_event_handler(uint64_t event, struct net_if *iface, void *info, size_t info_length, void *user_data)
{
    ARG_UNUSED(info);
    ARG_UNUSED(info_length);
    ARG_UNUSED(user_data);

    if (iface != net_if_get_first_by_type(&NET_L2_GET_NAME(PPP)))
    {
        return;
    }

    if (event == NET_EVENT_L4_CONNECTED)
    {
        trace_point("NET_EVENT_L4_CONNECTED received (PPP/IPv4 usable)");
        mark(L4_CONNECTED);
        k_event_post(&g_l4_event, kL4Connected);
    }
    else if (event == NET_EVENT_L4_DISCONNECTED)
    {
        trace_point("NET_EVENT_L4_DISCONNECTED received");
        k_event_set(&g_l4_event, 0);
    }
}

NET_MGMT_REGISTER_EVENT_HANDLER(srv_network_l4, NET_EVENT_L4_CONNECTED | NET_EVENT_L4_DISCONNECTED, l4_event_handler,
                                NULL);

/* The modem sits behind a GPIO power domain on boards that have one (see
 * modem_dcdc in the board's own devicetree). That rail, and the modem
 * device's own resume/suspend, are both claimed/released from
 * modem_connect()/modem_disconnect() - not from srvNetwork_start()/stop()
 * at boot - so idle current between rounds is true zero (rail off), not
 * "rail on, chip in reset". On nrf93m1dk that idle-rail draw measured
 * ~3 mA with the rail claimed at boot, which is what motivated moving it
 * here: AT traffic and RF only draw current during an explicit
 * power-up-through-shutdown round, gated behind the button0 press.
 *
 * modem_connect() powers the rail (where one exists) and then resumes the
 * modem device (pm_device_runtime_get on the modem device) - this is the
 * call that starts the power-pulse/RDY-wait/AT-command sequence.
 * modem_disconnect() suspends the device first, then releases the rail: on
 * real hardware the cellular network drops an idle PPP call within a few
 * seconds regardless of what we do, and reconnecting to an already-dead
 * call causes CMUX buffer overruns and failed renegotiation (observed on
 * nrf93m1dk) - a full power-cycle per round is slower but is the only
 * combination that has proven reliable.
 *
 * power_on()/power_off() (the network_port hooks called from
 * srvNetwork_start()/stop()) are therefore no-ops for the real port: there
 * is nothing left to do at service start/stop once the rail moved into the
 * per-round path. */
#if DT_NODE_EXISTS(DT_NODELABEL(modem_dcdc))
int modem_domain_on(void)
{
    const int err = pm_device_runtime_get(DEVICE_DT_GET(DT_NODELABEL(modem_dcdc)));
    if (err != 0)
    {
        LOG_ERR("modem power domain on failed (%d)", err);
    }
    return err;
}

int modem_domain_off(void)
{
    const int err = pm_device_runtime_put(DEVICE_DT_GET(DT_NODELABEL(modem_dcdc)));
    return (err == -EALREADY) ? 0 : err;
}
#else
/* No separate power domain on this board's devicetree (e.g. an externally
 * wired modem with only mdm-power-gpios) - nothing to pre-claim, the
 * modem's own resume in modem_connect() is the first thing that touches it
 * either way. */
int modem_domain_on(void)
{
    return 0;
}
int modem_domain_off(void)
{
    return 0;
}
#endif

int modem_power_on(void *)
{
    return 0;
}
int modem_power_off(void *)
{
    return 0;
}

int modem_connect(void *)
{
    struct net_if *iface = net_if_get_first_by_type(&NET_L2_GET_NAME(PPP));

    if (iface == nullptr)
    {
        LOG_ERR("no PPP network interface - is &modem enabled in the devicetree?");
        return -ENODEV;
    }

#ifndef CONFIG_FIREFLY_NETWORK_TRANSPORT_EG800Z_PPP
    // Only meaningful for the modem_cellular transport (nRF93M1, Fibocom
    // LE270): DEVICE_DT_GET on the modem node only resolves to a real
    // device object when MODEM_CELLULAR has bound a driver to it, and the
    // cellular_get_modem_info() API belongs to that driver. Under the
    // EG800Z transport there is no such device - referencing it here is an
    // undefined __device_dts_ord_* at link time.
    const struct device *modem = DEVICE_DT_GET(DT_ALIAS(modem));

    // Cached from the init chat script's AT+CGMR (see e.g.
    // cellular_nordic_nrf93m1.c / cellular_fibocom_le270.c) - the modem is
    // still powered off at this point, so this reports whatever the
    // previous round queried. Empty on the very first round ever;
    // populated from round 2 onward.
    char fw_version[64] = {};
    if (cellular_get_modem_info(modem, CELLULAR_MODEM_INFO_FW_VERSION, fw_version, sizeof(fw_version)) == 0 &&
        fw_version[0] != '\0')
    {
        LOG_INF("modem firmware version: %s", fw_version);
    }
    else
    {
        LOG_INF("modem firmware version: not yet queried (available from next round)");
    }
#endif

    k_event_set(&g_l4_event, 0);
    g_marks = {};

    SysviewSpan resume_span(SPAN_MODEM_RESUME);
    trace_point("modem power domain on begin");
    int err = modem_domain_on();
    trace_point(err == 0 ? "modem power domain on complete" : "modem power domain on failed");
    if (err != 0)
    {
        LOG_ERR("modem domain resume failed (%d)", err);
        return err;
    }

#ifdef CONFIG_FIREFLY_NETWORK_TRANSPORT_EG800Z_PPP
    // The EG800Z has no CMUX, so there is no modem_cellular device to resume -
    // modemEg800zPpp owns the UART and runs the PPP application note's
    // sequence itself, stopping at CONNECT so net_if_up() below still starts
    // LCP/AUTH/IPCP inside this round's phase timing.
    trace_point("EG800Z link_up begin (AT setup/SIM/registration/ATD*99#)");
    err = modemEg800zPpp_link_up();
    trace_point(err == 0 ? "EG800Z link_up complete (data mode)" : "EG800Z link_up failed");
    if (err != 0)
    {
        LOG_ERR("EG800Z link_up failed (%d)", err);
        return err;
    }
#else
    trace_point("pm_device_runtime_get(modem) begin (power pulse/modem init/AT setup)");
    err = pm_device_runtime_get(modem);
    trace_point(err == 0 ? "pm_device_runtime_get(modem) complete" : "pm_device_runtime_get(modem) failed");
    if (err != 0)
    {
        LOG_ERR("modem resume failed (%d)", err);
        return err;
    }
#endif
    mark(MODEM_RESUME_DONE);
    resume_span.end();
    SysviewSpan l4_span(SPAN_AWAIT_L4);

    trace_point("net_if_up(PPP) begin (registration/PDP/PPP may occur hereafter)");
    err = net_if_up(iface);
    trace_point((err == 0 || err == -EALREADY) ? "net_if_up(PPP) returned" : "net_if_up(PPP) failed");
    if (err != 0 && err != -EALREADY)
    {
        LOG_ERR("net_if_up failed (%d)", err);
        return err;
    }
    mark(IF_UP_DONE);

    trace_point("waiting for NET_EVENT_L4_CONNECTED");
    const uint32_t events =
        k_event_wait(&g_l4_event, kL4Connected, false, K_SECONDS(CONFIG_FIREFLY_NETWORK_CONNECT_TIMEOUT_S));
    if (events != kL4Connected)
    {
        trace_point("L4/PPP connectivity wait timed out");
        LOG_ERR("timed out waiting for PPP connectivity");
        return -ETIMEDOUT;
    }

    trace_point("connect complete: Zephyr L4 is ready");
    return 0;
}

int modem_send(void *, const void *data, size_t len)
{
    struct sockaddr_in dest = {};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(CONFIG_FIREFLY_NETWORK_DEST_PORT);
    if (net_addr_pton(AF_INET, CONFIG_FIREFLY_NETWORK_DEST_IP, &dest.sin_addr) != 0)
    {
        LOG_ERR("bad CONFIG_FIREFLY_NETWORK_DEST_IP %s", CONFIG_FIREFLY_NETWORK_DEST_IP);
        return -EINVAL;
    }

    SysviewSpan send_span(SPAN_SEND);
    trace_point("socket(AF_INET, SOCK_DGRAM) begin");
    const int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0)
    {
        LOG_ERR("socket() failed (%d)", errno);
        return -errno;
    }
    trace_point("UDP socket created");
    mark(SOCK_CREATED);

    trace_point("sendto() begin");
    const ssize_t sent = sendto(sock, data, len, 0, reinterpret_cast<struct sockaddr *>(&dest), sizeof(dest));
    const int send_errno = errno;
    trace_point(sent >= 0 ? "sendto() returned success" : "sendto() returned failure");
    if (sent >= 0)
    {
        mark(SENT);
    }
    send_span.end();

    if (sent >= 0)
    {
        // sendto() returning success only means the payload was queued by
        // the local UDP/IP stack - it says nothing about whether it has
        // actually drained out over the modem UART yet. The destination
        // test server echoes every datagram it receives right back
        // (confirmed on real hardware), so wait for that instead of a
        // blind fixed sleep: it's strong, immediate confirmation the
        // payload cleared the modem and reached the server, typically
        // arriving in ~0.3-0.4s - well under the old fixed 1s wait.
        // SO_RCVTIMEO (set below) bounds this by
        // CONFIG_FIREFLY_NETWORK_SEND_SETTLE_MS, so if nothing arrives
        // (server unreachable, reply lost, antenna removed for a coverage
        // test, etc.) recv() still returns once that elapses - this never
        // waits longer than the old behavior did, only ever the same or
        // less.
        // A zero SO_RCVTIMEO means "block forever" to Zephyr, so a setting
        // of 0, or a timeout that could not be set, skips the wait instead.
        struct timeval rcv_timeout = {};
        rcv_timeout.tv_sec = CONFIG_FIREFLY_NETWORK_SEND_SETTLE_MS / 1000;
        rcv_timeout.tv_usec = (CONFIG_FIREFLY_NETWORK_SEND_SETTLE_MS % 1000) * 1000;
        if (CONFIG_FIREFLY_NETWORK_SEND_SETTLE_MS == 0)
        {
            trace_point("SEND_SETTLE is 0 - not waiting for an echo");
        }
        else if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &rcv_timeout, sizeof(rcv_timeout)) != 0)
        {
            LOG_WRN("setsockopt(SO_RCVTIMEO) failed (%d) - not waiting for an echo", errno);
        }
        else
        {
            trace_point("waiting for echo (bounded by SEND_SETTLE as a timeout)");
            SysviewSpan echo_span(SPAN_ECHO_WAIT);
            char echo_buf[64];
            struct sockaddr_in from = {};
            socklen_t from_len = sizeof(from);
            const ssize_t echoed =
                recvfrom(sock, echo_buf, sizeof(echo_buf), 0, reinterpret_cast<struct sockaddr *>(&from), &from_len);
            trace_point(echoed > 0 ? "echo received" : "no echo within timeout - proceeding anyway");
        }
    }

    close(sock);
    trace_point("UDP socket closed");

    if (sent < 0 || static_cast<size_t>(sent) != len)
    {
        LOG_ERR("sendto() failed (%d)", send_errno);
        return -send_errno;
    }

    return 0;
}

int modem_disconnect(void *)
{
#ifndef CONFIG_FIREFLY_NETWORK_TRANSPORT_EG800Z_PPP
    const struct device *modem = DEVICE_DT_GET(DT_ALIAS(modem));
#endif
    struct net_if *iface = net_if_get_first_by_type(&NET_L2_GET_NAME(PPP));
    trace_point("disconnect begin");
    mark(DISCONNECT_BEGIN);
    SysviewSpan down_span(SPAN_IF_DOWN);
    if (iface != nullptr)
    {
        trace_point("net_if_down(PPP) begin");
        net_if_down(iface);
        trace_point("net_if_down(PPP) returned");
    }
    mark(IF_DOWN_DONE);
    down_span.end();
    SysviewSpan suspend_span(SPAN_MODEM_SUSPEND);

#ifdef CONFIG_FIREFLY_NETWORK_TRANSPORT_EG800Z_PPP
    trace_point("EG800Z link_down begin");
    int err = modemEg800zPpp_link_down();
    trace_point(err == 0 ? "EG800Z link_down complete" : "EG800Z link_down failed");
#else
    trace_point("pm_device_runtime_put(modem) begin");
    int err = pm_device_runtime_put(modem);
    trace_point((err == 0 || err == -EALREADY) ? "pm_device_runtime_put(modem) complete"
                                               : "pm_device_runtime_put(modem) failed");
    if (err == -EALREADY)
    {
        err = 0;
    }
#endif
    mark(MODEM_SUSPEND_DONE);

    trace_point("modem power domain off begin");
    const int domain_err = modem_domain_off();
    trace_point(domain_err == 0 ? "modem power domain off complete" : "modem power domain off failed");
    if (domain_err != 0)
    {
        LOG_ERR("modem domain suspend failed (%d)", domain_err);
    }

    return (err != 0) ? err : domain_err;
}

const struct network_port g_default_port = {nullptr,    modem_power_on,   modem_connect,
                                            modem_send, modem_disconnect, modem_power_off};

#else // !CONFIG_FIREFLY_NETWORK

int stub_power_on(void *)
{
    return 0;
}
int stub_connect(void *)
{
    return -ENOSYS;
}
int stub_send(void *, const void *, size_t)
{
    return -ENOSYS;
}
int stub_disconnect(void *)
{
    return 0;
}
int stub_power_off(void *)
{
    return 0;
}

const struct network_port g_default_port = {nullptr,   stub_power_on,   stub_connect,
                                            stub_send, stub_disconnect, stub_power_off};

#endif

const struct network_port *g_port = &g_default_port;

constexpr k_timeout_t kWait = K_MSEC(50);

class SrvNetwork final : public firefly::Thread<CONFIG_FIREFLY_NETWORK_STACK_SIZE>
{
  public:
    SrvNetwork() : Thread(5, "srvNetwork")
    {
        k_sem_init(&processed_sem_, 0, 1);
    }

    int startNetwork(const struct zbus_observer *subscriber)
    {
        if (subscriber == nullptr || subscriber->type != ZBUS_OBSERVER_MSG_SUBSCRIBER_TYPE)
        {
            return -EINVAL;
        }
        if (isRunning())
        {
            return -EALREADY;
        }

        const int power_error = g_port->power_on(g_port->context);
        if (power_error != 0)
        {
            return power_error;
        }

        subscriber_ = subscriber;
        has_result_ = false;
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

#ifdef CONFIG_FIREFLY_TEST_HOOKS
    void reset()
    {
        subscriber_ = nullptr;
        has_result_ = false;
        last_result_ = 0;
        atomic_clear(&processed_);
        k_sem_reset(&processed_sem_);
    }
#endif

  protected:
    void run() override
    {
        LOG_INF("srvNetwork ready - waiting for button0");
        while (!shouldStop())
        {
            const struct zbus_channel *channel = nullptr;
            struct bus_msg message{};
            const int error = zbus_sub_wait_msg(subscriber_, &channel, &message, kWait);
            if (error == -ENOMSG || error == -EAGAIN)
            {
                continue;
            }
            if (error != 0)
            {
                LOG_ERR("message wait failed (%d)", error);
                continue;
            }
            if (channel != &bus_chan)
            {
                LOG_WRN("unexpected channel");
                continue;
            }
            if (message.kind != BUS_MSG_BUTTON || !bus_button_is_valid(&message.data.button))
            {
                continue;
            }
            if (message.data.button.button != 0 || !message.data.button.pressed)
            {
                continue;
            }
            runRound();
        }

        const int power_off_error = g_port->power_off(g_port->context);
        if (power_off_error != 0)
        {
            LOG_ERR("modem power-off failed (%d)", power_off_error);
        }
    }

  private:
    void runRound()
    {
        SysviewSpan round_span(SPAN_ROUND);
        g_round_start_ms = k_uptime_get();
        LOG_INF("============================================================");
        trace_point("button0 accepted: network round starts");
        LOG_INF("NETTRACE destination=%s:%d payload=%u bytes connect_timeout=%d s", CONFIG_FIREFLY_NETWORK_DEST_IP,
                CONFIG_FIREFLY_NETWORK_DEST_PORT, static_cast<unsigned>(sizeof(CONFIG_FIREFLY_NETWORK_MESSAGE) - 1),
                CONFIG_FIREFLY_NETWORK_CONNECT_TIMEOUT_S);
        int err = g_port->connect(g_port->context);
        trace_point(err == 0 ? "connect() returned success" : "connect() returned failure");

        if (err == 0 && shouldStop())
        {
            LOG_WRN("stop requested before send - skipping it");
            err = -ECANCELED;
        }
        else if (err == 0)
        {
            static const char message[] = CONFIG_FIREFLY_NETWORK_MESSAGE;

            LOG_INF("connected - sending %zu bytes", sizeof(message) - 1);
            // g_port->send() (modem_send() for the real port) already traces
            // its own "sendto()"/echo-wait sequence and waits for the
            // destination's echo reply, bounded by
            // CONFIG_FIREFLY_NETWORK_SEND_SETTLE_MS, before returning - see
            // that function for why. Nothing left to trace here but the
            // result: by the time this call returns, everything it would
            // have said already happened.
            err = g_port->send(g_port->context, message, sizeof(message) - 1);
            if (err != 0)
            {
                LOG_ERR("send failed (%d)", err);
            }
        }
        else
        {
            LOG_ERR("connect failed (%d)", err);
        }

        const int disconnect_err = g_port->disconnect(g_port->context);
        trace_point(disconnect_err == 0 ? "disconnect() returned success" : "disconnect() returned failure");
        if (disconnect_err != 0)
        {
            LOG_ERR("disconnect failed (%d)", disconnect_err);
        }

        last_result_ = (err != 0) ? err : disconnect_err;
        has_result_ = true;
        if (last_result_ != 0)
        {
            sysview_error("network round failed");
        }
        LOG_INF("NETTRACE SUMMARY total_button_to_finish=%lld ms result=%d",
                static_cast<long long>(k_uptime_get() - g_round_start_ms), last_result_);
#ifdef CONFIG_FIREFLY_NETWORK
        report(g_round_start_ms);
#endif
        LOG_INF("network round finished (%d)", last_result_);
        LOG_INF("============================================================");

        atomic_inc(&processed_);
        k_sem_give(&processed_sem_);
    }

    const struct zbus_observer *subscriber_ = nullptr;
    int last_result_ = 0;
    bool has_result_ = false;
    atomic_t processed_{0};
    struct k_sem processed_sem_{};
};

SrvNetwork g_network;

} // namespace

extern "C" int srvNetwork_start(const struct zbus_observer *subscriber)
{
    return g_network.startNetwork(subscriber);
}
extern "C" int srvNetwork_stop(void)
{
    return g_network.stop();
}
extern "C" bool srvNetwork_is_running(void)
{
    return g_network.isRunning();
}
extern "C" int srvNetwork_last_result(void)
{
    return g_network.lastResult();
}
extern "C" uint32_t srvNetwork_processed_count(void)
{
    return g_network.count();
}
extern "C" int srvNetwork_wait_processed(uint32_t count, int32_t timeout_ms)
{
    return g_network.waitProcessed(count, timeout_ms);
}

#ifdef CONFIG_FIREFLY_TEST_HOOKS
extern "C" int srvNetwork_set_port(const struct network_port *port)
{
    if (port == nullptr || port->power_on == nullptr || port->connect == nullptr || port->send == nullptr ||
        port->disconnect == nullptr || port->power_off == nullptr)
    {
        return -EINVAL;
    }
    g_port = port;
    return 0;
}

extern "C" void srvNetwork_reset(void)
{
    g_port = &g_default_port;
    g_network.reset();
}
#endif
