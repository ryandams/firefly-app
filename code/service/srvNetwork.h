/* SPDX-License-Identifier: Apache-2.0 */
/*
 * srvNetwork - fully power-cycled per round. srvNetwork_start()/stop()
 * (network_port::power_on/power_off) do not touch the modem or its power
 * domain at all - the modem stays completely unpowered from service start
 * until the first button0 press. Each button0 press then runs one full
 * round: energize the modem's power domain/rail where one exists, resume
 * the modem device (this is what actually starts the power pulse/AT
 * traffic - see network_port::connect), bring the PPP link up, send one
 * UDP message, bring the PPP link back down, suspend the modem device, and
 * release the rail again (network_port::disconnect).
 *
 * The AT traffic is deliberately gated behind the button press rather than
 * starting automatically at boot, to support power measurement setups that
 * need a precise, on-demand starting point and zero idle current between
 * rounds (an energized-but-idle rail was observed to draw ~3 mA on
 * nrf93m1dk). Each round fully power-cycles the modem rather than leaving
 * it dialed between rounds: on real hardware an idle PPP call gets dropped
 * by the network within a few seconds regardless, and reconnecting to an
 * already-dead call causes CMUX buffer overruns and failed renegotiation -
 * a full cycle per round is slower but is what has proven reliable. Runs
 * on its own persistent thread since waiting for cellular registration can
 * legitimately take up to CONFIG_FIREFLY_NETWORK_CONNECT_TIMEOUT_S seconds.
 *
 * network_port mirrors myLeds.h's firefly_led_port: the real port drives
 * the actual modem/PPP/socket stack (only compiled in when
 * CONFIG_FIREFLY_NETWORK is set - see srvNetwork.cpp), and
 * CONFIG_FIREFLY_TEST_HOOKS lets tests substitute a fake one to exercise
 * srvNetwork's own control flow without any real hardware.
 */
#ifndef FIREFLY_SRV_NETWORK_H
#define FIREFLY_SRV_NETWORK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct zbus_observer;

struct network_port {
    void *context;
    int (*power_on)(void *context);
    int (*connect)(void *context);
    int (*send)(void *context, const void *data, size_t len);
    int (*disconnect)(void *context);
    int (*power_off)(void *context);
};

#ifdef __cplusplus
extern "C" {
#endif

/* subscriber must already be one of bus_chan's observers (see event_bus.h)
 * - srvNetwork listens on it for button0-press events and runs one round
 * per press. */
int srvNetwork_start(const struct zbus_observer *subscriber);
int srvNetwork_stop(void);
bool srvNetwork_is_running(void);

/* Result of the most recently completed round: 0 on success, otherwise
 * whichever of connect()/send()/disconnect() failed first. -ENODATA if no
 * round has completed yet. */
int srvNetwork_last_result(void);
uint32_t srvNetwork_processed_count(void);
int srvNetwork_wait_processed(uint32_t previous_count, int32_t timeout_ms);

#ifdef CONFIG_FIREFLY_TEST_HOOKS
int srvNetwork_set_port(const struct network_port *port);
void srvNetwork_reset(void);
#endif

#ifdef __cplusplus
}
#endif
#endif
