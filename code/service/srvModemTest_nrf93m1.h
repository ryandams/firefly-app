/* SPDX-License-Identifier: Apache-2.0 */
#ifndef FIREFLY_SRV_MODEM_TEST_NRF93M1_H
#define FIREFLY_SRV_MODEM_TEST_NRF93M1_H

#include <stdbool.h>
#include <stdint.h>

struct zbus_observer;

#ifdef __cplusplus
extern "C" {
#endif

/* Owns the nRF93M1 UART directly. Each button-0 press executes one timed
 * AT-command UDP round trip equivalent to NordicSemi_nRF93M1_timed.py. */
int srvModemTest_nrf93m1_start(const struct zbus_observer *subscriber);
int srvModemTest_nrf93m1_stop(void);
bool srvModemTest_nrf93m1_is_running(void);
int srvModemTest_nrf93m1_last_result(void);
uint32_t srvModemTest_nrf93m1_processed_count(void);

#ifdef __cplusplus
}
#endif
#endif
