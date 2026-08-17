/* SPDX-License-Identifier: Apache-2.0 */
#ifndef FIREFLY_SRV_CONTROLLER_H
#define FIREFLY_SRV_CONTROLLER_H
#include <stdbool.h>
#include <stdint.h>
#include "event_bus.h"

struct zbus_observer;
#ifdef __cplusplus
extern "C"
{
#endif
    int srvController_start(const struct zbus_observer *subscriber);
    int srvController_stop(void);
    bool srvController_is_running(void);
    int srvController_last_event(struct event_msg *event);
    uint32_t srvController_processed_count(void);
    int srvController_wait_processed(uint32_t previous_count, int32_t timeout_ms);
#ifdef CONFIG_FIREFLY_TEST_HOOKS
    int srvController_reset(void);
#endif
#ifdef __cplusplus
}
#endif
#endif
