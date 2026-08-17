/* SPDX-License-Identifier: Apache-2.0 */
#ifndef FIREFLY_MY_LEDS_H
#define FIREFLY_MY_LEDS_H

#include <stdbool.h>
#include <stdint.h>

struct firefly_led_port
{
    void *context;
    int (*configure)(void *context);
    int (*set)(void *context, uint8_t index, bool on);
};

#ifdef __cplusplus
extern "C"
{
#endif
    int myLeds_init(void);
    int myLeds_set(uint8_t index, bool on);
#ifdef CONFIG_FIREFLY_TEST_HOOKS
    int myLeds_set_port(const struct firefly_led_port *port);
    void myLeds_reset(void);
#endif
#ifdef __cplusplus
}
#endif
#endif
