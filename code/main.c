/* SPDX-License-Identifier: Apache-2.0 */
#include "srvController.h"
#include "srvNetwork.h"
#include "srvModemTest_nrf93m1.h"
#include "srvModemTest_eg800z.h"
#include "myButtonListener.h"
#include "firefly.h"
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/zbus/zbus.h>

LOG_MODULE_REGISTER(main, CONFIG_FIREFLY_LOG_LEVEL);

ZBUS_MSG_SUBSCRIBER_DEFINE(srvController_sub);

/* A subscriber may only observe bus_chan when a service below actually
 * drains it (zbus_sub_wait_msg()). zbus clones one message per observer on
 * every publish regardless of whether anyone reads it. A never-drained
 * subscriber's clones pile up in its own fifo and are never freed, so the
 * shared CONFIG_ZBUS_MSG_SUBSCRIBER_NET_BUF_POOL_SIZE pool (16) silently runs
 * out - confirmed on real hardware as
 * "zbus: could not deliver notification to observer . Error code -12"
 * (net_buf_clone() returning -ENOMEM) starting on the button's 8th press and
 * on every press after that, permanently. So srvNetwork_sub exists only with
 * CONFIG_FIREFLY_NETWORK, and srvModemTest_sub only with a raw-AT test. */
#ifdef CONFIG_FIREFLY_NETWORK
ZBUS_MSG_SUBSCRIBER_DEFINE(srvNetwork_sub);
#define NETWORK_OBSERVER , srvNetwork_sub
#else
#define NETWORK_OBSERVER
#endif

#if defined(CONFIG_FIREFLY_MODEM_TEST_NRF93M1) || defined(CONFIG_FIREFLY_MODEM_TEST_EG800Z)
ZBUS_MSG_SUBSCRIBER_DEFINE(srvModemTest_sub);
#define MODEM_TEST_OBSERVER , srvModemTest_sub
#else
#define MODEM_TEST_OBSERVER
#endif

ZBUS_CHAN_DEFINE(bus_chan, struct bus_msg, NULL, NULL,
                 ZBUS_OBSERVERS(srvController_sub NETWORK_OBSERVER MODEM_TEST_OBSERVER), ZBUS_MSG_INIT(0));

int main(void)
{
    LOG_INF("firefly %s starting", FIREFLY_SW_APP_VERSION);

    int error = srvController_start(&srvController_sub);
    if (error != 0)
    {
        LOG_ERR("controller start failed (%d)", error);
        return error;
    }
    error = myButtonListener_init(&bus_chan);
    if (error != 0)
    {
        LOG_ERR("button listener init failed (%d)", error);
        (void)srvController_stop();
        return error;
    }

#ifdef CONFIG_FIREFLY_NETWORK
    /* Powers the modem on and starts srvNetwork's listener loop - it then
     * waits for button0 presses (see srvNetwork.h) and runs one connect/
     * send/disconnect round per press. Unlike the controller/button
     * listener above, a failed modem power-on (no SIM, modem not wired up
     * yet) is not fatal to the rest of the app - srvNetwork logs its own
     * outcome via srvNetwork_last_result(), it does not stop main() from
     * continuing.
     */
    error = srvNetwork_start(&srvNetwork_sub);
    if (error != 0)
    {
        LOG_ERR("network service failed to start (%d)", error);
    }
#endif

#ifdef CONFIG_FIREFLY_MODEM_TEST_NRF93M1
    error = srvModemTest_nrf93m1_start(&srvModemTest_sub);
    if (error != 0)
    {
        LOG_ERR("nRF93M1 modem test failed to start (%d)", error);
    }
#endif

#ifdef CONFIG_FIREFLY_MODEM_TEST_EG800Z
    error = srvModemTest_eg800z_start(&srvModemTest_sub);
    if (error != 0)
    {
        LOG_ERR("EG800Z modem test failed to start (%d)", error);
    }
#endif

    LOG_INF("firefly ready");
    return 0;
}
