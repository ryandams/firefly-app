// SPDX-License-Identifier: Apache-2.0
//
// Service-level contract of the two raw-AT modem test services,
// srvModemTest_nrf93m1 and srvModemTest_eg800z. Their AT/UART code is only
// compiled with CONFIG_FIREFLY_MODEM_TEST_{NRF93M1,EG800Z}, which need the
// real modem in devicetree - so on native_sim this covers everything around
// it: subscriber validation, start/stop, which bus messages trigger a round
// (button 0/1 presses only), result/count bookkeeping and the wait API. A
// triggered round reports -ENOSYS here, the "no modem compiled in" result.
// The AT sequences themselves are verified on hardware (tests/*_power).
//
// The services' subscribers are attached to bus_chan only for this suite
// (zbus runtime observers): as permanent observers they would queue every
// message the other suites publish while these services are not running,
// draining the shared ZBUS_MSG_SUBSCRIBER net_buf pool. Even attached only
// here, every subscriber whose service is not running keeps a buffer per
// publish, so drain_idle() empties them all before each service is tested.
#include <zephyr/kernel.h>
#include <zephyr/zbus/zbus.h>
#include <zephyr/ztest.h>
#include <errno.h>
#include "event_bus.h"
#include "modemEg800zPpp.h"
#include "srvModemTest_eg800z.h"
#include "srvModemTest_nrf93m1.h"

extern "C"
{
    /* Defined in test_srvController.cpp next to bus_chan, with the other
     * suites' subscribers. */
    extern const struct zbus_channel unexpected_chan;
    extern const struct zbus_observer test_sub;
    extern const struct zbus_observer test_network_sub;

    /* modemEg800zPppNet.c has no header of its own - modemEg800zPpp.cpp is
     * its only caller in the app. */
    struct modem_ppp;
    struct modem_ppp *modemEg800zPpp_ppp(void);

    ZBUS_MSG_SUBSCRIBER_DEFINE(test_nrf93m1_sub);
    ZBUS_MSG_SUBSCRIBER_DEFINE(test_eg800z_sub);

    /* A valid observer of the wrong kind: the services require a message
     * subscriber, since they read copies of each message from its queue. */
    static void ignore_message(const struct zbus_channel *)
    {
    }
    ZBUS_LISTENER_DEFINE(test_listener, ignore_message);
}

namespace
{

struct ModemTestService
{
    const char *name;
    const struct zbus_observer *subscriber;
    int (*start)(const struct zbus_observer *subscriber);
    int (*stop)(void);
    bool (*is_running)(void);
    int (*last_result)(void);
    uint32_t (*processed_count)(void);
    int (*wait_processed)(uint32_t previous_count, int32_t timeout_ms);
    bool (*modem_is_powered)(void);
};

const ModemTestService kServices[] = {
    {"nrf93m1", &test_nrf93m1_sub, srvModemTest_nrf93m1_start, srvModemTest_nrf93m1_stop,
     srvModemTest_nrf93m1_is_running, srvModemTest_nrf93m1_last_result, srvModemTest_nrf93m1_processed_count,
     srvModemTest_nrf93m1_wait_processed, srvModemTest_nrf93m1_modem_is_powered},
    {"eg800z", &test_eg800z_sub, srvModemTest_eg800z_start, srvModemTest_eg800z_stop, srvModemTest_eg800z_is_running,
     srvModemTest_eg800z_last_result, srvModemTest_eg800z_processed_count, srvModemTest_eg800z_wait_processed,
     srvModemTest_eg800z_modem_is_powered},
};

constexpr int32_t kRoundTimeoutMs = 500;

void drain(const struct zbus_observer *subscriber)
{
    const struct zbus_channel *channel;
    struct bus_msg message;
    while (zbus_sub_wait_msg(subscriber, &channel, &message, K_NO_WAIT) == 0)
    {
    }
}

/* Every msg subscriber of bus_chan holds one pool buffer per queued message
 * until read; only the service under test reads its own queue. */
void drain_idle()
{
    drain(&test_sub);
    drain(&test_network_sub);
    drain(&test_nrf93m1_sub);
    drain(&test_eg800z_sub);
}

int publish_button(uint8_t button, bool pressed)
{
    const button_event event{button, pressed};
    return bus_publish_button(&bus_chan, &event);
}

/* Presses a button and waits for the round it triggers to complete. */
void press_and_wait(const ModemTestService &service, uint8_t button)
{
    const uint32_t count = service.processed_count();
    zassert_ok(publish_button(button, true), "%s: publish", service.name);
    zassert_ok(service.wait_processed(count, kRoundTimeoutMs), "%s: button %u did not run a round", service.name,
               button);
}

void before(void *)
{
    for (const ModemTestService &service : kServices)
    {
        zassert_ok(service.stop());
        zassert_ok(zbus_chan_add_obs(&bus_chan, service.subscriber, K_MSEC(100)));
        zassert_ok(zbus_chan_add_obs(&unexpected_chan, service.subscriber, K_MSEC(100)));
    }
    drain_idle();
}

void after(void *)
{
    for (const ModemTestService &service : kServices)
    {
        zassert_ok(service.stop());
        zassert_ok(zbus_chan_rm_obs(&bus_chan, service.subscriber, K_MSEC(100)));
        zassert_ok(zbus_chan_rm_obs(&unexpected_chan, service.subscriber, K_MSEC(100)));
    }
    drain_idle();
}

} // namespace

ZTEST(firefly_modem_test, test_start_rejects_missing_or_wrong_kind_of_subscriber)
{
    for (const ModemTestService &service : kServices)
    {
        zassert_equal(service.start(nullptr), -EINVAL, "%s", service.name);
        zassert_equal(service.start(&test_listener), -EINVAL, "%s", service.name);
        zassert_false(service.is_running(), "%s", service.name);
    }
}

ZTEST(firefly_modem_test, test_lifecycle_duplicate_start_and_restart)
{
    for (const ModemTestService &service : kServices)
    {
        zassert_ok(service.stop(), "%s: stop before start", service.name);
        zassert_ok(service.start(service.subscriber), "%s", service.name);
        zassert_true(service.is_running(), "%s", service.name);
        zassert_equal(service.start(service.subscriber), -EALREADY, "%s", service.name);
        zassert_ok(service.stop(), "%s", service.name);
        zassert_false(service.is_running(), "%s", service.name);
        zassert_ok(service.stop(), "%s: duplicate stop", service.name);
        zassert_ok(service.start(service.subscriber), "%s: restart", service.name);
        zassert_true(service.is_running(), "%s", service.name);
    }
}

ZTEST(firefly_modem_test, test_button0_and_button1_each_run_one_round)
{
    for (const ModemTestService &service : kServices)
    {
        /* Rounds only ever run in this test, so nothing has completed yet. */
        drain_idle();
        zassert_equal(service.last_result(), -ENODATA, "%s", service.name);
        zassert_ok(service.start(service.subscriber), "%s", service.name);

        const uint32_t before_rounds = service.processed_count();
        press_and_wait(service, 0);
        zassert_equal(service.last_result(), -ENOSYS, "%s: single round", service.name);
        press_and_wait(service, 1);
        zassert_equal(service.last_result(), -ENOSYS, "%s: second round type", service.name);
        zassert_equal(service.processed_count(), before_rounds + 2, "%s", service.name);

        /* The modem is only powered for the duration of a real round. */
        zassert_false(service.modem_is_powered(), "%s", service.name);
    }
}

ZTEST(firefly_modem_test, test_only_button0_and_button1_presses_trigger_a_round)
{
    for (const ModemTestService &service : kServices)
    {
        drain_idle();
        zassert_ok(service.start(service.subscriber), "%s", service.name);
        const uint32_t count = service.processed_count();

        /* Everything below must be skipped. Messages are handled in publish
         * order, so if the press published last completes the very next
         * round, none of these started one. */
        zassert_ok(publish_button(0, false), "%s: release", service.name);
        zassert_ok(publish_button(2, true), "%s: button 2", service.name);
        zassert_ok(publish_button(3, true), "%s: button 3", service.name);

        const event_msg event{EVT_RTC_ALARM, 0, 0};
        zassert_ok(bus_publish_event(&bus_chan, &event), "%s: non-button message", service.name);

        bus_msg invalid_button{};
        invalid_button.kind = BUS_MSG_BUTTON;
        invalid_button.data.button = {7, true};
        zassert_ok(zbus_chan_pub(&bus_chan, &invalid_button, K_MSEC(100)), "%s: invalid button", service.name);

        bus_msg on_other_channel{};
        on_other_channel.kind = BUS_MSG_BUTTON;
        on_other_channel.data.button = {0, true};
        zassert_ok(zbus_chan_pub(&unexpected_chan, &on_other_channel, K_MSEC(100)), "%s: other channel", service.name);

        press_and_wait(service, 0);
        zassert_equal(service.processed_count(), count + 1, "%s: an ignored message started a round", service.name);
    }
}

ZTEST(firefly_modem_test, test_wait_processed_arguments_and_timeout)
{
    for (const ModemTestService &service : kServices)
    {
        zassert_equal(service.wait_processed(0, -1), -EINVAL, "%s", service.name);
        zassert_equal(service.wait_processed(service.processed_count(), 1), -EAGAIN, "%s", service.name);
    }
}

ZTEST(firefly_modem_test, test_eg800z_ppp_transport_reports_unavailable_when_not_built)
{
    /* CONFIG_FIREFLY_NETWORK_TRANSPORT_EG800Z_PPP is off here: the stubs must
     * fail the link-up/AT paths rather than pretend to work, and treat
     * link-down as a no-op so teardown paths stay clean. */
    char response[16] = "untouched";
    zassert_equal(modemEg800zPpp_link_up(), -ENOSYS);
    zassert_ok(modemEg800zPpp_link_down());
    zassert_equal(modemEg800zPpp_at("AT", response, sizeof(response)), -ENOSYS);
    zassert_str_equal(response, "untouched");
    zassert_equal(modemEg800zPpp_urc_count(), 0U);
    zassert_is_null(modemEg800zPpp_ppp());
}

ZTEST_SUITE(firefly_modem_test, NULL, NULL, before, after, NULL);
