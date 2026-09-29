/* SPDX-License-Identifier: Apache-2.0 */
/*
 * modemEg800zPpp - direct PPP transport for the Quectel EG800Z, with no
 * CMUX anywhere in it.
 *
 * Why this exists instead of drivers/modem/modem_cellular.c: "AT+CMUX"
 * does not appear anywhere in the EG800Z&EG915Z Series (UniRTOS) AT
 * Commands Manual V1.0 - the module has no CMUX. modem_cellular is built
 * on it unconditionally (RUN_INIT_SCRIPT always advances to CONNECT_CMUX,
 * and both its AT pipe and its PPP pipe are CMUX DLCIs, with no vendor
 * flag to opt out), so it cannot drive this module at all. What the module
 * does provide, per the EG800Z&EG915Z Series PPP Application Note V1.1, is
 * a single port that switches between command mode and data mode. That is
 * what this file implements, on modem_backend_uart + modem_chat +
 * modem_ppp directly.
 *
 * Document references throughout are to those two documents, cited as
 * "PPP note" and "AT manual".
 *
 * Ownership: link_up() takes the modem UART for the whole session. Nothing
 * else may touch that UART - in particular srvModemTest_eg800z is mutually
 * exclusive with this (Kconfig choice FIREFLY_MODEM_SERVICE).
 */
#ifndef FIREFLY_MODEM_EG800Z_PPP_H
#define FIREFLY_MODEM_EG800Z_PPP_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Brings the module from "powered, command mode" to "PPP data mode, ready
 * for net_if_up()". Runs the PPP note section 3.2 sequence: UART sync,
 * ATE0/CMEE/IFC, AT&C1/AT&D1, RI URC configuration, AT+CFUN=1, AT+CPIN?
 * until the SIM is ready, AT+CREG?/AT+CGREG? until registered,
 * AT+CGDCONT, then ATD*99# and modem_ppp_attach() on CONNECT.
 *
 * Deliberately does NOT call net_if_up() - the caller does that next, so
 * LCP/AUTH/IPCP negotiation stays inside srvNetwork's own phase timing.
 * Deliberately does NOT pulse PWRKEY: this module self-starts with the
 * board (autostarts in the board overlay), and a >=650 ms PWRKEY pulse on
 * a running module powers it down. */
int modemEg800zPpp_link_up(void);

/* Reverse of link_up(). Call after net_if_down(), which is what emits the
 * LCP Terminate-Request the PPP note section 5 recommends; this then waits
 * for the module's own NO CARRIER, detaches PPP and closes the UART.
 * Leaves the module powered and in command mode - it is not shut down,
 * because waking it again would need the PWRKEY pulse that this board
 * cannot issue safely. */
int modemEg800zPpp_link_down(void);

/* Runs one AT command while the PPP link stays up, for debugging and for
 * whatever the CMUX AT shell would otherwise have provided.
 *
 * Uses the "+++" escape (PPP note section 4.1.1.2) to reach command mode
 * and ATO (section 4.1.2.1) to return. That escape has strict guard
 * timing - no traffic for 1 s before, "+++" within 1 s, nothing for 1 s
 * after - so each call costs at least ~2 s and briefly stops PPP traffic.
 * The note also warns it must only be used after PPP negotiation has
 * completed; calling this before the link is up will tear the link down.
 *
 * response, when non-NULL, receives the first response line. Returns 0 on
 * success, -EAGAIN if the link is not in data mode, or a negative errno
 * from the underlying chat script. */
int modemEg800zPpp_at(const char *command, char *response, size_t response_size);

/* Number of RI-triggered URC drains completed since link_up(). Lets a test
 * or the shell observe that the section 4.2 path actually ran. */
unsigned int modemEg800zPpp_urc_count(void);

#ifdef __cplusplus
}
#endif
#endif
