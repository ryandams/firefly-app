// SPDX-License-Identifier: Apache-2.0
#include "myButtonListener.h"
#include "event_bus.h"
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/input/input.h>
#include <zephyr/logging/log.h>
#include <zephyr/pm/device_runtime.h>
#include <errno.h>

LOG_MODULE_REGISTER(myButtonListener, CONFIG_FIREFLY_LOG_LEVEL);
namespace
{
const struct zbus_channel *g_channel;
int button_index(uint16_t code)
{
    switch (code)
    {
    case INPUT_KEY_0:
        return 0;
    case INPUT_KEY_1:
        return 1;
    case INPUT_KEY_2:
        return 2;
    case INPUT_KEY_3:
        return 3;
    default:
        return -1;
    }
}
int publish_input(uint16_t code, int32_t value)
{
    if (g_channel == nullptr)
    {
        return -ENODEV;
    }
    const int index = button_index(code);
    if (index < 0)
    {
        return -ENOTSUP;
    }
    const struct button_event button = {static_cast<uint8_t>(index), value != 0};
    LOG_INF("button %d %s", index, value != 0 ? "pressed" : "released");
    const int error = bus_publish_button(g_channel, &button);
    if (error != 0)
    {
        LOG_ERR("button publish failed (%d)", error);
    }
    return error;
}
void on_input_event(struct input_event *event, void *)
{
    if (event != nullptr)
    {
        (void)publish_input(event->code, event->value);
    }
}
INPUT_CALLBACK_DEFINE(NULL, on_input_event, NULL);
} // namespace

extern "C" int myButtonListener_init(const struct zbus_channel *channel)
{
    if (channel == nullptr)
    {
        return -EINVAL;
    }
    /* INPUT_GPIO_KEYS calls pm_device_runtime_enable() on its own device in
     * gpio_keys_init(), which leaves it suspended (GPIO interrupts disabled,
     * pins disconnected) whenever CONFIG_PM_DEVICE_RUNTIME=y - as it is here,
     * for the modem's PM. Without this get(), no button ever fires an event.
     * Held for the process lifetime; never put back. */
    const int error = pm_device_runtime_get(DEVICE_DT_GET(DT_PATH(buttons)));
    if (error != 0)
    {
        LOG_ERR("failed to resume buttons device (%d)", error);
        return error;
    }
    g_channel = channel;
    LOG_INF("button listener ready");
    return 0;
}
extern "C" void myButtonListener_reset(void)
{
    g_channel = nullptr;
}
#ifdef CONFIG_FIREFLY_TEST_HOOKS
extern "C" int myButtonListener_inject(uint16_t code, int32_t value)
{
    return publish_input(code, value);
}
#endif
