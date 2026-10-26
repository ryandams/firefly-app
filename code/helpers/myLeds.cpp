// SPDX-License-Identifier: Apache-2.0
#include "myLeds.h"
#include <zephyr/drivers/gpio.h>
#include <errno.h>

namespace
{
BUILD_ASSERT(DT_NODE_HAS_PROP(DT_ALIAS(led0), gpios), "firefly requires at least a led0 alias with gpios");

// Boards provide different numbers of LEDs (nRF54LM20 DK: led0..led3,
// nrf93m1dk: led0..led2) - build the array out of whichever led0..led3
// aliases the target board's devicetree actually defines, following the
// same IF_ENABLED(DT_NODE_EXISTS(...), (...,)) idiom Zephyr's own samples
// use for optional numbered aliases (see samples/sensor/dht_polling).
#define LED_ENTRY(idx) IF_ENABLED(DT_NODE_EXISTS(DT_ALIAS(led##idx)), (GPIO_DT_SPEC_GET(DT_ALIAS(led##idx), gpios), ))

struct gpio_dt_spec g_leds[] = {LED_ENTRY(0) LED_ENTRY(1) LED_ENTRY(2) LED_ENTRY(3)};

constexpr size_t kLedCount = ARRAY_SIZE(g_leds);

int configure_leds(void *)
{
    for (struct gpio_dt_spec &led : g_leds)
    {
        if (!gpio_is_ready_dt(&led))
        {
            return -ENODEV;
        }
        const int error = gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);
        if (error != 0)
        {
            return error;
        }
    }
    return 0;
}

int set_led(void *, uint8_t index, bool on)
{
    if (index >= kLedCount)
    {
        return -EINVAL;
    }
    return gpio_pin_set_dt(&g_leds[index], on ? 1 : 0);
}

const firefly_led_port g_default_led_port = {nullptr, configure_leds, set_led};
const firefly_led_port *g_led_port = &g_default_led_port;
} // namespace

extern "C" int myLeds_init(void)
{
    return g_led_port->configure(g_led_port->context);
}
extern "C" int myLeds_set(uint8_t index, bool on)
{
    if (index >= kLedCount)
    {
        return -EINVAL;
    }
    return g_led_port->set(g_led_port->context, index, on);
}
#ifdef CONFIG_FIREFLY_TEST_HOOKS
extern "C" int myLeds_set_port(const struct firefly_led_port *port)
{
    if (port == nullptr || port->configure == nullptr || port->set == nullptr)
    {
        return -EINVAL;
    }
    g_led_port = port;
    return 0;
}
extern "C" void myLeds_reset(void)
{
    g_led_port = &g_default_led_port;
}
#endif
