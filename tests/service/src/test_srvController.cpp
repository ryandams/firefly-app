// SPDX-License-Identifier: Apache-2.0
#include <zephyr/dt-bindings/input/input-event-codes.h>
#include <zephyr/zbus/zbus.h>
#include <zephyr/ztest.h>
#include <errno.h>
#include "myButtonListener.h"
#include "myLeds.h"
#include "srvController.h"

extern "C"
{
    ZBUS_MSG_SUBSCRIBER_DEFINE(test_sub);
    /* test_network_sub is srvNetwork's observer of this same channel - see
     * test_srvNetwork.cpp, which references it via an extern declaration. */
    ZBUS_MSG_SUBSCRIBER_DEFINE(test_network_sub);
    ZBUS_CHAN_DEFINE(bus_chan, struct bus_msg, NULL, NULL, ZBUS_OBSERVERS(test_sub, test_network_sub),
                      ZBUS_MSG_INIT(0));
    ZBUS_CHAN_DEFINE(unexpected_chan, struct bus_msg, NULL, NULL, ZBUS_OBSERVERS(test_sub), ZBUS_MSG_INIT(0));
}

namespace
{
struct FakeLeds
{
    bool states[4]{};
    int configure_error{};
    int set_error{};
    uint32_t writes{};
};
FakeLeds leds;

int configure(void *ctx)
{
    return static_cast<FakeLeds *>(ctx)->configure_error;
}
int set(void *ctx, uint8_t index, bool on)
{
    auto *fake = static_cast<FakeLeds *>(ctx);
    if (fake->set_error != 0)
    {
        return fake->set_error;
    }
    if (index >= 4)
    {
        return -EINVAL;
    }
    fake->states[index] = on;
    fake->writes++;
    return 0;
}
const firefly_led_port port{&leds, configure, set};

void drain()
{
    const struct zbus_channel *channel;
    struct bus_msg message;
    /* test_network_sub is the other observer of bus_chan (srvNetwork's, in
     * test_srvNetwork.cpp) - drained here too so this suite's publishes
     * never pile up in its queue between tests. */
    while (zbus_sub_wait_msg(&test_sub, &channel, &message, K_NO_WAIT) == 0)
    {
    }
    while (zbus_sub_wait_msg(&test_network_sub, &channel, &message, K_NO_WAIT) == 0)
    {
    }
}

void before(void *)
{
    (void)srvController_stop();
    myButtonListener_reset();
    drain();
    leds = {};
    zassert_ok(srvController_reset());
    zassert_ok(myLeds_set_port(&port));
}

void after(void *)
{
    zassert_ok(srvController_stop());
    myButtonListener_reset();
    drain();
}

void publish_and_wait(const event_msg &event)
{
    const uint32_t count = srvController_processed_count();
    zassert_ok(bus_publish_event(&bus_chan, &event));
    zassert_ok(srvController_wait_processed(count, 500));
}
} // namespace

ZTEST(firefly_service, test_lifecycle_and_restart)
{
    zassert_ok(srvController_stop());
    zassert_equal(srvController_start(nullptr), -EINVAL);
    zassert_ok(srvController_start(&test_sub));
    zassert_true(srvController_is_running());
    zassert_equal(srvController_reset(), -EBUSY);
    zassert_equal(srvController_start(&test_sub), -EALREADY);
    zassert_ok(srvController_stop());
    zassert_ok(srvController_stop());
    zassert_ok(srvController_start(&test_sub));
}

ZTEST(firefly_service, test_all_events_and_payload_are_preserved)
{
    zassert_ok(srvController_start(&test_sub));
    for (uint8_t kind = 0; kind < EVT_COUNT; ++kind)
    {
        event_msg sent{kind, static_cast<uint8_t>(kind == EVT_MOTION ? 2 : 0), 100U + kind};
        publish_and_wait(sent);
        event_msg received{};
        zassert_ok(srvController_last_event(&received));
        zassert_mem_equal(&received, &sent, sizeof(sent));
    }
}

ZTEST(firefly_service, test_messages_are_processed_in_publish_order)
{
    zassert_ok(srvController_start(&test_sub));
    constexpr uint32_t total = 12;
    for (uint32_t i = 0; i < total; ++i)
    {
        event_msg event{static_cast<uint8_t>(i % EVT_COUNT), 0, i};
        const uint32_t count = srvController_processed_count();
        zassert_ok(bus_publish_event(&bus_chan, &event));
        zassert_ok(srvController_wait_processed(count, 500));
        event_msg observed{};
        zassert_ok(srvController_last_event(&observed));
        zassert_equal(observed.queue_depth, i);
    }
    event_msg last{};
    zassert_ok(srvController_last_event(&last));
    zassert_equal(last.queue_depth, total - 1);
}

ZTEST(firefly_service, test_validation_and_uninitialized_state)
{
    event_msg output{};
    event_msg invalid_kind{EVT_COUNT, 0, 0};
    event_msg invalid_motion{EVT_MOTION, 3, 0};
    button_event invalid_button{4, true};
    zassert_equal(srvController_last_event(nullptr), -EINVAL);
    zassert_equal(srvController_last_event(&output), -ENODATA);
    zassert_equal(bus_publish_event(nullptr, &output), -EINVAL);
    zassert_equal(bus_publish_event(&bus_chan, nullptr), -EINVAL);
    zassert_equal(bus_publish_event(&bus_chan, &invalid_kind), -EINVAL);
    zassert_equal(bus_publish_event(&bus_chan, &invalid_motion), -EINVAL);
    zassert_equal(bus_publish_button(&bus_chan, &invalid_button), -EINVAL);
    zassert_equal(myButtonListener_init(nullptr), -EINVAL);
    zassert_equal(myButtonListener_inject(INPUT_KEY_0, 1), -ENODEV);
}

ZTEST(firefly_service, test_invalid_and_unexpected_messages_are_ignored)
{
    zassert_ok(srvController_start(&test_sub));
    const uint32_t count = srvController_processed_count();
    bus_msg invalid{};
    invalid.kind = static_cast<bus_msg_kind>(UINT8_MAX);
    zassert_ok(zbus_chan_pub(&bus_chan, &invalid, K_MSEC(100)));
    zassert_ok(zbus_chan_pub(&unexpected_chan, &invalid, K_MSEC(100)));
    k_sleep(K_MSEC(100));
    zassert_equal(srvController_processed_count(), count);
}

ZTEST(firefly_service, test_every_button_press_and_release)
{
    const uint16_t codes[]{INPUT_KEY_0, INPUT_KEY_1, INPUT_KEY_2, INPUT_KEY_3};
    zassert_ok(myButtonListener_init(&bus_chan));
    zassert_ok(srvController_start(&test_sub));
    for (uint8_t i = 0; i < 4; ++i)
    {
        uint32_t count = srvController_processed_count();
        zassert_ok(myButtonListener_inject(codes[i], 1));
        zassert_ok(srvController_wait_processed(count, 500));
        zassert_true(leds.states[i]);
        count = srvController_processed_count();
        zassert_ok(myButtonListener_inject(codes[i], 0));
        zassert_ok(srvController_wait_processed(count, 500));
        zassert_false(leds.states[i]);
    }
    zassert_equal(myButtonListener_inject(INPUT_KEY_A, 1), -ENOTSUP);
}

ZTEST(firefly_service, test_led_helper_validation_and_write_failures)
{
    zassert_equal(myLeds_set_port(nullptr), -EINVAL);
    const firefly_led_port invalid_port{&leds, nullptr, set};
    zassert_equal(myLeds_set_port(&invalid_port), -EINVAL);
    zassert_equal(myLeds_set(4, true), -EINVAL);
    leds.configure_error = -ENODEV;
    zassert_equal(srvController_start(&test_sub), -ENODEV);
    leds.configure_error = 0;
    leds.set_error = -EIO;
    zassert_ok(srvController_start(&test_sub));
    zassert_ok(myLeds_set_port(&port));
    const uint32_t count = srvController_processed_count();
    button_event button{0, true};
    zassert_ok(bus_publish_button(&bus_chan, &button));
    zassert_ok(srvController_wait_processed(count, 500));
    zassert_equal(leds.writes, 0);
}

ZTEST(firefly_service, test_wait_timeout_and_arguments)
{
    zassert_equal(srvController_wait_processed(0, -1), -EINVAL);
    zassert_equal(srvController_wait_processed(0, 1), -EAGAIN);
}

ZTEST_SUITE(firefly_service, NULL, NULL, before, after, NULL);
