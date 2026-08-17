/* SPDX-License-Identifier: Apache-2.0 */
#ifndef FIREFLY_EVENT_BUS_H
#define FIREFLY_EVENT_BUS_H

#include <stdbool.h>
#include <stdint.h>

struct zbus_channel;

#ifdef __cplusplus
extern "C"
{
#endif

    enum event_kind
    {
        EVT_RTC_ALARM = 0,
        EVT_MOTION,
        EVT_FIX_OK,
        EVT_FIX_TIMEOUT,
        EVT_REPORT_STORED,
        EVT_SESSION_OK,
        EVT_SESSION_TRANSIENT_FAIL,
        EVT_SESSION_PERMANENT_FAIL,
        EVT_POWER_LOW,
        EVT_POWER_OK,
        EVT_COUNT
    };

    enum bus_msg_kind
    {
        BUS_MSG_EVENT = 0,
        BUS_MSG_BUTTON,
        BUS_MSG_COUNT
    };

    struct event_msg
    {
        uint8_t evt;
        uint8_t motion;
        uint32_t queue_depth;
    };

    struct button_event
    {
        uint8_t button;
        bool pressed;
    };

    /* A single copied zbus message type preserves ordering across event sources. */
    struct bus_msg
    {
        uint8_t kind;
        union
        {
            struct event_msg event;
            struct button_event button;
        } data;
    };

    extern const struct zbus_channel bus_chan;

    bool bus_event_is_valid(const struct event_msg *event);
    bool bus_button_is_valid(const struct button_event *button);
    int bus_publish_event(const struct zbus_channel *chan, const struct event_msg *event);
    int bus_publish_button(const struct zbus_channel *chan, const struct button_event *button);

#ifdef __cplusplus
}
#endif
#endif
