/* SPDX-License-Identifier: Apache-2.0 */
#ifndef FIREFLY_MY_BUTTON_LISTENER_H
#define FIREFLY_MY_BUTTON_LISTENER_H
#include <stdint.h>
struct zbus_channel;
#ifdef __cplusplus
extern "C"
{
#endif
    int myButtonListener_init(const struct zbus_channel *channel);
    void myButtonListener_reset(void);
#ifdef CONFIG_FIREFLY_TEST_HOOKS
    int myButtonListener_inject(uint16_t code, int32_t value);
#endif
#ifdef __cplusplus
}
#endif
#endif
