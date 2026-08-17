// SPDX-License-Identifier: Apache-2.0
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/gpio/gpio_emul.h>
#include <zephyr/zbus/zbus.h>
#include <zephyr/ztest.h>
#include "myButtonListener.h"
#include "srvController.h"

extern "C"
{
    ZBUS_MSG_SUBSCRIBER_DEFINE(gpio_sub);
    ZBUS_CHAN_DEFINE(bus_chan, struct bus_msg, NULL, NULL, ZBUS_OBSERVERS(gpio_sub), ZBUS_MSG_INIT(0));
}

ZTEST(gpio_pipeline, test_all_physical_button_edges_control_matching_leds)
{
    const gpio_dt_spec buttons[] = {
        GPIO_DT_SPEC_GET(DT_ALIAS(sw0), gpios),
        GPIO_DT_SPEC_GET(DT_ALIAS(sw1), gpios),
        GPIO_DT_SPEC_GET(DT_ALIAS(sw2), gpios),
        GPIO_DT_SPEC_GET(DT_ALIAS(sw3), gpios),
    };
    const gpio_dt_spec leds[] = {
        GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios),
        GPIO_DT_SPEC_GET(DT_ALIAS(led1), gpios),
        GPIO_DT_SPEC_GET(DT_ALIAS(led2), gpios),
        GPIO_DT_SPEC_GET(DT_ALIAS(led3), gpios),
    };
    zassert_ok(myButtonListener_init(&bus_chan));
    zassert_ok(srvController_start(&gpio_sub));
    for (uint8_t i = 0; i < 4; ++i)
    {
        uint32_t count = srvController_processed_count();
        zassert_ok(gpio_emul_input_set(buttons[i].port, buttons[i].pin, 0));
        zassert_ok(srvController_wait_processed(count, 500));
        zassert_equal(gpio_emul_output_get_dt(&leds[i]), 1);
        count = srvController_processed_count();
        zassert_ok(gpio_emul_input_set(buttons[i].port, buttons[i].pin, 1));
        zassert_ok(srvController_wait_processed(count, 500));
        zassert_equal(gpio_emul_output_get_dt(&leds[i]), 0);
    }
    zassert_ok(srvController_stop());
}
ZTEST_SUITE(gpio_pipeline, NULL, NULL, NULL, NULL, NULL);
