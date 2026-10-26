/* SPDX-License-Identifier: Apache-2.0 */
#ifndef FIREFLY_SRV_MODEM_TEST_NRF93M1_H
#define FIREFLY_SRV_MODEM_TEST_NRF93M1_H

#include <stdbool.h>
#include <stdint.h>

struct zbus_observer;

#ifdef __cplusplus
extern "C"
{
#endif

    /* Owns the nRF93M1 UART directly. The modem is not powered at
     * srvModemTest_nrf93m1_start() - prepare_hardware() only sets up the UART
     * and configures the power GPIO as an inactive output. Each button-0 press
     * then runs one full round: power up the modem, run the timed AT-command
     * UDP round trip equivalent to NordicSemi_nRF93M1_timed.py, and power the
     * modem back down (AT%POWD=1, falling back to a GPIO power-pulse if that
     * doesn't get confirmed). */
    int srvModemTest_nrf93m1_start(const struct zbus_observer *subscriber);
    int srvModemTest_nrf93m1_stop(void);
    bool srvModemTest_nrf93m1_is_running(void);
    int srvModemTest_nrf93m1_last_result(void);
    uint32_t srvModemTest_nrf93m1_processed_count(void);
    int srvModemTest_nrf93m1_wait_processed(uint32_t previous_count, int32_t timeout_ms);

#ifdef CONFIG_FIREFLY_TEST_HOOKS
    /* mdm-power-gpios is a momentary power-key pulse, not a level-controlled
     * enable line, so there is no pin state to read back. This instead reports
     * this service's own belief about modem power state: false until a round's
     * power-up pulse completes, true from then until that round's power-down
     * (AT%POWD, or the GPIO-pulse fallback) completes. Lets a test assert
     * "not powered" before/after a round without any real UART/AT traffic. */
    bool srvModemTest_nrf93m1_modem_is_powered(void);
#endif

#ifdef __cplusplus
}
#endif
#endif
