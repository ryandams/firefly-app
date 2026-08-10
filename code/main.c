/* SPDX-License-Identifier: Apache-2.0
 *
 * Boot only. Per the IBD, main() is trivial: bring up services in order and
 * get out of the way. All sequencing lives in srvController, all board power
 * behaviour in srvBoard.
 */
#include "srvController.h"
#include "firefly.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/zbus/zbus.h>

LOG_MODULE_REGISTER(main, CONFIG_FIREFLY_LOG_LEVEL);

ZBUS_CHAN_DEFINE(firefly_chan,
		 struct firefly_msg,
		 NULL, NULL,
		 ZBUS_OBSERVERS(srvController_sub),
		 ZBUS_MSG_INIT(0));

ZBUS_SUBSCRIBER_DEFINE(srvController_sub, 8);

static void srvController_thread(void *a, void *b, void *c)
{
	ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);

	const struct zbus_channel *chan;

	srvController_init();

	while (zbus_sub_wait(&srvController_sub, &chan, K_FOREVER) == 0) {
		struct firefly_msg msg;

		if (zbus_chan_read(chan, &msg, K_MSEC(100)) == 0) {
			srvController_dispatch(&msg);
		}
	}
}

K_THREAD_DEFINE(srvController_tid, 4096, srvController_thread,
		NULL, NULL, NULL, 5, 0, 0);

int main(void)
{
	LOG_INF("firefly %s starting (%s)", FIREFLY_SW_APP_VERSION,
		IS_ENABLED(CONFIG_FIREFLY_PORT_HOST) ? "host ports" : "MCU ports");

	/* On native_sim there is no RTC, so kick the cycle by hand to give the
	 * host run something to do. On target this comes from srvRtc.
	 */
	if (IS_ENABLED(CONFIG_FIREFLY_PORT_HOST)) {
		while (1) {
			struct firefly_msg m = {
				.evt = FIREFLY_EVT_RTC_ALARM,
				.motion = 0,
				.queue_depth = 0,
			};

			zbus_chan_pub(&firefly_chan, &m, K_MSEC(100));
			k_sleep(K_SECONDS(10));
		}
	}

	return 0;
}
