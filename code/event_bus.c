/* SPDX-License-Identifier: Apache-2.0 */
#include "event_bus.h"
#include <zephyr/kernel.h>
#include <zephyr/zbus/zbus.h>
#include <errno.h>

bool bus_event_is_valid(const struct event_msg *event)
{
    if (event == NULL || event->evt >= EVT_COUNT)
    {
        return false;
    }
    return event->evt != EVT_MOTION || event->motion <= 2U;
}

bool bus_button_is_valid(const struct button_event *button)
{
    return button != NULL && button->button < 4U;
}

int bus_publish_event(const struct zbus_channel *chan, const struct event_msg *event)
{
    if (chan == NULL || !bus_event_is_valid(event))
    {
        return -EINVAL;
    }
    struct bus_msg message = {.kind = BUS_MSG_EVENT, .data.event = *event};
    return zbus_chan_pub(chan, &message, K_MSEC(CONFIG_FIREFLY_BUS_TIMEOUT_MS));
}

int bus_publish_button(const struct zbus_channel *chan, const struct button_event *button)
{
    if (chan == NULL || !bus_button_is_valid(button))
    {
        return -EINVAL;
    }
    struct bus_msg message = {.kind = BUS_MSG_BUTTON, .data.button = *button};
    return zbus_chan_pub(chan, &message, K_MSEC(CONFIG_FIREFLY_BUS_TIMEOUT_MS));
}
