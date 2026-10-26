// SPDX-License-Identifier: Apache-2.0
//
// Exercises srvNetwork's own control flow via a fake network_port - this
// suite never touches a real modem/PPP/socket stack (CONFIG_FIREFLY_NETWORK
// is off here), it verifies that power_on/connect/send/disconnect/power_off
// get called in the right order and only in response to button0 presses,
// that failures at each step are reported correctly, and that
// start/stop/isRunning behave. Real modem behaviour can only be verified on
// nrf93m1dk (or nrf54lm20dk with an nRF93M1 wired to uart30) hardware.
#include <zephyr/kernel.h>
#include <zephyr/zbus/zbus.h>
#include <zephyr/ztest.h>
#include <errno.h>
#include <string.h>
#include "event_bus.h"
#include "srvNetwork.h"

extern "C"
{
    /* Defined in test_srvController.cpp, alongside bus_chan - shared so
     * both srvController and srvNetwork can independently observe the same
     * button events, exactly like the real app's main.c. */
    extern const struct zbus_observer test_network_sub;
    extern const struct zbus_channel unexpected_chan;
}

namespace
{

struct FakeNetwork
{
    int power_on_error{};
    int connect_error{};
    int connect_delay_ms{};
    int send_error{};
    int disconnect_error{};
    int power_off_error{};
    uint32_t power_on_calls{};
    uint32_t connect_calls{};
    uint32_t disconnect_calls{};
    uint32_t power_off_calls{};
    bool connected{};
    bool sent{};
    bool disconnected{};
    size_t sent_len{};
    char sent_data[64]{};
};
FakeNetwork net;

int fake_power_on(void *ctx)
{
    auto *fake = static_cast<FakeNetwork *>(ctx);
    fake->power_on_calls++;
    return fake->power_on_error;
}

int fake_connect(void *ctx)
{
    auto *fake = static_cast<FakeNetwork *>(ctx);
    fake->connect_calls++;
    if (fake->connect_delay_ms > 0)
    {
        k_sleep(K_MSEC(fake->connect_delay_ms));
    }
    if (fake->connect_error != 0)
    {
        return fake->connect_error;
    }
    fake->connected = true;
    return 0;
}

int fake_send(void *ctx, const void *data, size_t len)
{
    auto *fake = static_cast<FakeNetwork *>(ctx);
    if (fake->send_error != 0)
    {
        return fake->send_error;
    }
    fake->sent = true;
    fake->sent_len = len;
    memcpy(fake->sent_data, data, len < sizeof(fake->sent_data) ? len : sizeof(fake->sent_data) - 1);
    return 0;
}

int fake_disconnect(void *ctx)
{
    auto *fake = static_cast<FakeNetwork *>(ctx);
    fake->disconnect_calls++;
    fake->disconnected = true;
    return fake->disconnect_error;
}

int fake_power_off(void *ctx)
{
    auto *fake = static_cast<FakeNetwork *>(ctx);
    fake->power_off_calls++;
    return fake->power_off_error;
}

const network_port port{&net, fake_power_on, fake_connect, fake_send, fake_disconnect, fake_power_off};

void drain()
{
    const struct zbus_channel *channel;
    struct bus_msg message;
    while (zbus_sub_wait_msg(&test_network_sub, &channel, &message, K_NO_WAIT) == 0)
    {
    }
}

/* Publishes a button0 press and waits for the resulting round to finish. */
int press_button0_and_wait(int timeout_ms)
{
    const uint32_t count = srvNetwork_processed_count();
    const button_event press{0, true};
    const int publish_error = bus_publish_button(&bus_chan, &press);
    if (publish_error != 0)
    {
        return publish_error;
    }
    return srvNetwork_wait_processed(count, timeout_ms);
}

void before(void *)
{
    (void)srvNetwork_stop();
    net = {};
    drain();
    srvNetwork_reset();
    zassert_ok(srvNetwork_set_port(&port));
}

void after(void *)
{
    (void)srvNetwork_stop();
    drain();
}

} // namespace

ZTEST(firefly_network, test_start_powers_on_modem_immediately)
{
    zassert_equal(net.power_on_calls, 0);
    zassert_ok(srvNetwork_start(&test_network_sub));
    zassert_equal(net.power_on_calls, 1);
    zassert_false(net.connected);
    zassert_false(net.sent);
}

ZTEST(firefly_network, test_button0_press_connects_sends_and_disconnects)
{
    zassert_ok(srvNetwork_start(&test_network_sub));
    zassert_ok(press_button0_and_wait(500));

    zassert_true(net.connected);
    zassert_true(net.sent);
    zassert_true(net.disconnected);
    zassert_equal(srvNetwork_last_result(), 0);
    zassert_mem_equal(net.sent_data, CONFIG_FIREFLY_NETWORK_MESSAGE, net.sent_len);
    /* modem power is untouched by a round - only power_on (start) and
     * power_off (stop) ever touch it. */
    zassert_equal(net.power_off_calls, 0);
}

ZTEST(firefly_network, test_repeated_presses_each_run_a_fresh_round_without_repowering)
{
    zassert_ok(srvNetwork_start(&test_network_sub));

    zassert_ok(press_button0_and_wait(500));
    zassert_equal(net.connect_calls, 1);
    zassert_equal(net.disconnect_calls, 1);

    net.connected = false;
    net.sent = false;
    net.disconnected = false;

    zassert_ok(press_button0_and_wait(500));
    zassert_equal(net.connect_calls, 2);
    zassert_equal(net.disconnect_calls, 2);
    zassert_true(net.connected);
    zassert_true(net.sent);
    zassert_true(net.disconnected);

    /* power_on only ever happens once, at start() - never repeated per round. */
    zassert_equal(net.power_on_calls, 1);
}

ZTEST(firefly_network, test_stop_powers_off_the_modem)
{
    zassert_ok(srvNetwork_start(&test_network_sub));
    zassert_ok(press_button0_and_wait(500));
    zassert_equal(net.power_off_calls, 0);

    zassert_ok(srvNetwork_stop());
    zassert_equal(net.power_off_calls, 1);
}

ZTEST(firefly_network, test_other_buttons_and_releases_are_ignored)
{
    zassert_ok(srvNetwork_start(&test_network_sub));

    const button_event other_button{1, true};
    const button_event release{0, false};
    zassert_ok(bus_publish_button(&bus_chan, &other_button));
    zassert_ok(bus_publish_button(&bus_chan, &release));
    k_sleep(K_MSEC(100));

    zassert_equal(srvNetwork_processed_count(), 0);
    zassert_false(net.connected);
}

ZTEST(firefly_network, test_connect_failure_still_disconnects_and_is_reported)
{
    net.connect_error = -ENETUNREACH;

    zassert_ok(srvNetwork_start(&test_network_sub));
    zassert_ok(press_button0_and_wait(500));

    zassert_false(net.sent);
    zassert_true(net.disconnected);
    zassert_equal(srvNetwork_last_result(), -ENETUNREACH);
}

ZTEST(firefly_network, test_send_failure_is_reported)
{
    net.send_error = -EIO;

    zassert_ok(srvNetwork_start(&test_network_sub));
    zassert_ok(press_button0_and_wait(500));

    zassert_true(net.connected);
    zassert_true(net.disconnected);
    zassert_equal(srvNetwork_last_result(), -EIO);
}

ZTEST(firefly_network, test_power_on_failure_prevents_start)
{
    net.power_on_error = -ENODEV;

    zassert_equal(srvNetwork_start(&test_network_sub), -ENODEV);
    zassert_false(srvNetwork_is_running());
}

ZTEST(firefly_network, test_lifecycle_and_duplicate_start)
{
    zassert_false(srvNetwork_is_running());
    zassert_equal(srvNetwork_last_result(), -ENODATA);

    zassert_equal(srvNetwork_start(nullptr), -EINVAL);
    zassert_ok(srvNetwork_start(&test_network_sub));
    zassert_equal(srvNetwork_start(&test_network_sub), -EALREADY);
    zassert_ok(srvNetwork_stop());
    zassert_false(srvNetwork_is_running());
}

ZTEST(firefly_network, test_stop_joins_a_slow_connect)
{
    net.connect_delay_ms = 100;

    zassert_ok(srvNetwork_start(&test_network_sub));
    const uint32_t count = srvNetwork_processed_count();
    const button_event press{0, true};
    zassert_ok(bus_publish_button(&bus_chan, &press));
    k_sleep(K_MSEC(10)); /* make sure the round has actually entered connect() */
    zassert_ok(srvNetwork_stop());
    zassert_false(srvNetwork_is_running());
    zassert_equal(srvNetwork_last_result(), -ECANCELED);
    zassert_true(srvNetwork_processed_count() > count);
}

ZTEST(firefly_network, test_port_validation)
{
    zassert_equal(srvNetwork_set_port(nullptr), -EINVAL);
    const network_port invalid{&net, nullptr, fake_connect, fake_send, fake_disconnect, fake_power_off};
    zassert_equal(srvNetwork_set_port(&invalid), -EINVAL);
}

ZTEST(firefly_network, test_disconnect_failure_is_reported_after_a_good_send)
{
    net.disconnect_error = -EIO;

    zassert_ok(srvNetwork_start(&test_network_sub));
    zassert_ok(press_button0_and_wait(500));

    zassert_true(net.sent);
    zassert_equal(srvNetwork_last_result(), -EIO);
}

ZTEST(firefly_network, test_connect_failure_takes_precedence_over_disconnect_failure)
{
    net.connect_error = -ENETUNREACH;
    net.disconnect_error = -EIO;

    zassert_ok(srvNetwork_start(&test_network_sub));
    zassert_ok(press_button0_and_wait(500));

    zassert_equal(srvNetwork_last_result(), -ENETUNREACH);
}

ZTEST(firefly_network, test_power_off_failure_at_stop_still_stops)
{
    net.power_off_error = -EIO;

    zassert_ok(srvNetwork_start(&test_network_sub));
    zassert_ok(srvNetwork_stop());
    zassert_equal(net.power_off_calls, 1);
    zassert_false(srvNetwork_is_running());
}

ZTEST(firefly_network, test_non_button_and_other_channel_messages_are_ignored)
{
    zassert_ok(zbus_chan_add_obs(&unexpected_chan, &test_network_sub, K_MSEC(100)));
    zassert_ok(srvNetwork_start(&test_network_sub));

    const event_msg event{EVT_RTC_ALARM, 0, 0};
    zassert_ok(bus_publish_event(&bus_chan, &event));
    bus_msg press{};
    press.kind = BUS_MSG_BUTTON;
    press.data.button = {0, true};
    zassert_ok(zbus_chan_pub(&unexpected_chan, &press, K_MSEC(100)));

    /* Handled in publish order: the button0 press published last runs the
     * first round only if both messages above were skipped. */
    zassert_ok(press_button0_and_wait(500));
    zassert_equal(srvNetwork_processed_count(), 1);
    zassert_equal(net.connect_calls, 1);

    zassert_ok(srvNetwork_stop());
    zassert_ok(zbus_chan_rm_obs(&unexpected_chan, &test_network_sub, K_MSEC(100)));
}

ZTEST(firefly_network, test_default_port_without_a_modem_fails_the_round_cleanly)
{
    /* No fake port: srvNetwork's own built-in port, which is all stubs while
     * CONFIG_FIREFLY_NETWORK is off. Power on/off and disconnect succeed, so
     * start/stop work, but there is no modem to connect to. */
    srvNetwork_reset();

    zassert_ok(srvNetwork_start(&test_network_sub));
    zassert_ok(press_button0_and_wait(500));
    zassert_equal(srvNetwork_last_result(), -ENOSYS);
    zassert_ok(srvNetwork_stop());
    zassert_false(net.connected);
}

ZTEST(firefly_network, test_wait_processed_arguments_and_timeout)
{
    zassert_equal(srvNetwork_wait_processed(0, -1), -EINVAL);
    zassert_equal(srvNetwork_wait_processed(srvNetwork_processed_count(), 1), -EAGAIN);
}

ZTEST_SUITE(firefly_network, NULL, NULL, before, after, NULL);
