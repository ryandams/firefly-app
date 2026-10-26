/* SPDX-License-Identifier: Apache-2.0 */
/*
 * The PPP L2 net_device for the EG800Z transport, and nothing else.
 *
 * This is a separate C file purely because MODEM_PPP_DEFINE expands to
 * NET_DEVICE_INIT, whose initialisers are C-only: they assign designators
 * out of declaration order and use compound literals that C++ rejects
 * outright ("designator order for field 'net_if_dev::flags' does not match
 * declaration order"). modemEg800zPpp.cpp cannot host it, so the definition
 * lives here and is reached through modemEg800zPpp_ppp().
 *
 * MTU 1500 with 1500-byte framing buffers, matching the PPP instance
 * cellular_quectel_eg800z.c used to declare for this module. init_iface is
 * NULL and there is no backing device, so this binds a bare PPP interface
 * that net_if_get_first_by_type(&NET_L2_GET_NAME(PPP)) finds - which is how
 * srvNetwork.cpp already locates the interface it brings up.
 */
/*
 * Guarded because firefly_service is a plain static library linked into the
 * test applications under tests/ as well as into the app. Those builds have
 * no modem configuration at all, and an unguarded MODEM_PPP_DEFINE there
 * fails to compile inside Zephyr's own headers
 * (CONFIG_MODEM_STATS_BUFFER_NAME_SIZE undeclared). The accessor still exists
 * in every build so modemEg800zPpp.cpp links either way.
 */
#include <zephyr/kernel.h> /* pulls in autoconf.h so the guard below is real */

#ifdef CONFIG_FIREFLY_NETWORK_TRANSPORT_EG800Z_PPP

#include <zephyr/modem/ppp.h>

/* 2026-10-07 upstream pin advance (sdk-nrf main -> 2026-10-07): MODEM_PPP_DEFINE
 * dropped its separate RX/TX buffer-size params for a single shared _buf_size.
 * This instance's RX and TX were already both 1500, so collapsing to one
 * argument is a pure mechanical re-arity - mtu=98 and buf_size=1500 are
 * unchanged from before. */
MODEM_PPP_DEFINE(modem_eg800z_ppp, NULL, 98, 1500);

struct modem_ppp *modemEg800zPpp_ppp(void)
{
    return &modem_eg800z_ppp;
}

#else

struct modem_ppp;

struct modem_ppp *modemEg800zPpp_ppp(void)
{
    return NULL;
}

#endif
