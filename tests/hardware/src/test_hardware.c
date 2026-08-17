// SPDX-License-Identifier: Apache-2.0
#include <zephyr/drivers/gpio.h>
#include <zephyr/ztest.h>

ZTEST(hardware_gpio, test_required_aliases_are_ready_and_leds_operate)
{
    const struct gpio_dt_spec leds[] = {
        GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios),
        GPIO_DT_SPEC_GET(DT_ALIAS(led1), gpios),
        GPIO_DT_SPEC_GET(DT_ALIAS(led2), gpios),
        GPIO_DT_SPEC_GET(DT_ALIAS(led3), gpios),
    };
    for (size_t i = 0; i < ARRAY_SIZE(leds); ++i)
    {
        zassert_true(gpio_is_ready_dt(&leds[i]));
        zassert_ok(gpio_pin_configure_dt(&leds[i], GPIO_OUTPUT_INACTIVE));
        zassert_ok(gpio_pin_set_dt(&leds[i], 1));
        zassert_ok(gpio_pin_set_dt(&leds[i], 0));
    }
}
ZTEST_SUITE(hardware_gpio, NULL, NULL, NULL, NULL, NULL);
