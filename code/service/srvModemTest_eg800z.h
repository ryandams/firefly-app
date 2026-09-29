/* SPDX-License-Identifier: Apache-2.0 */
#ifndef FIREFLY_SRV_MODEM_TEST_EG800Z_H
#define FIREFLY_SRV_MODEM_TEST_EG800Z_H

#include <stdbool.h>
#include <stdint.h>

struct zbus_observer;

#ifdef __cplusplus
extern "C" {
#endif

/* Owns the Quectel EG800Z UART directly - the raw-AT counterpart to
 * srvModemTest_nrf93m1, structured to be directly comparable to it. The
 * modem is not powered at srvModemTest_eg800z_start() - prepare_hardware()
 * only sets up the UART and configures PWRKEY/RESET/DTR as inactive
 * outputs. Two rounds, one per DK button:
 *
 *  - Button 1 (bus index 0): power up, connect, send, power back down
 *    (AT+QPOWD=1, falling back to a PWRKEY power-off pulse if that isn't
 *    confirmed).
 *  - Button 2 (bus index 1): power up, connect, then repeat send/sleep/
 *    wake several times before powering down. This module's documented AT
 *    interface has no 3GPP PSM (AT+CPSMS is not implemented - see
 *    cellular_quectel_eg800z.c) - the comparable low-power mechanism it
 *    does support is AT+QSCLK-gated UART sleep, controlled here via the
 *    MAIN_DTR pin, so that is what this round exercises instead of PSM.
 */
int srvModemTest_eg800z_start(const struct zbus_observer *subscriber);
int srvModemTest_eg800z_stop(void);
bool srvModemTest_eg800z_is_running(void);
int srvModemTest_eg800z_last_result(void);
uint32_t srvModemTest_eg800z_processed_count(void);
int srvModemTest_eg800z_wait_processed(uint32_t previous_count, int32_t timeout_ms);

#ifdef CONFIG_FIREFLY_TEST_HOOKS
/* mdm-power-gpios (PWRKEY) is a momentary pulse, not a level-controlled
 * enable line, so there is no pin state to read back. This instead reports
 * this service's own belief about modem power state: false until a
 * round's power-up pulse completes, true from then until that round's
 * power-down (AT+QPOWD, or the PWRKEY-pulse fallback) completes. Lets a
 * test assert "not powered" before/after a round without any real UART/AT
 * traffic. */
bool srvModemTest_eg800z_modem_is_powered(void);
#endif

#ifdef __cplusplus
}
#endif
#endif
