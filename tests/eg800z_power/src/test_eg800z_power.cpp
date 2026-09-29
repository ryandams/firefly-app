// SPDX-License-Identifier: Apache-2.0
//
// Hardware-in-the-loop: the Quectel EG800Z modem must start fully
// unpowered, then one triggered round must power it up, connect to the
// cellular network, send data, and power it back down again - verified
// against the REAL modem, not a fake port. Mirrors
// tests/nrf93m1_power/src/test_nrf93m1_power.cpp's raw-AT scenario;
// srvModemTest_eg800z is structured to be directly comparable to
// srvModemTest_nrf93m1 (see srvModemTest_eg800z.h).
#include <zephyr/kernel.h>
#include <zephyr/zbus/zbus.h>
#include <zephyr/ztest.h>
#include "event_bus.h"

extern "C" {
ZBUS_MSG_SUBSCRIBER_DEFINE(test_sub);
ZBUS_CHAN_DEFINE(bus_chan, struct bus_msg, NULL, NULL, ZBUS_OBSERVERS(test_sub), ZBUS_MSG_INIT(0));
}

#ifdef CONFIG_FIREFLY_MODEM_TEST_EG800Z

#include "srvModemTest_eg800z.h"

namespace {

int press_button0_and_wait(int timeout_ms)
{
    const uint32_t count = srvModemTest_eg800z_processed_count();
    const button_event press{0, true};
    const int publish_error = bus_publish_button(&bus_chan, &press);
    if (publish_error != 0) { return publish_error; }
    return srvModemTest_eg800z_wait_processed(count, timeout_ms);
}

} // namespace

ZTEST(eg800z_power_raw_at, test_not_powered_then_power_up_connect_send_shutdown)
{
    zassert_false(srvModemTest_eg800z_modem_is_powered(), "modem must start unpowered");

    zassert_ok(srvModemTest_eg800z_start(&test_sub));
    zassert_false(srvModemTest_eg800z_modem_is_powered(),
                  "srvModemTest_eg800z_start() must not power the modem - only a round does");

    zassert_ok(press_button0_and_wait((CONFIG_FIREFLY_MODEM_TEST_REG_TIMEOUT_S + 60) * 1000));
    zassert_equal(srvModemTest_eg800z_last_result(), 0,
                  "power up/connect/send/shutdown round failed");

    zassert_false(srvModemTest_eg800z_modem_is_powered(), "modem must be back off after the round");

    zassert_ok(srvModemTest_eg800z_stop());
}
ZTEST_SUITE(eg800z_power_raw_at, NULL, NULL, NULL, NULL, NULL);

#endif // CONFIG_FIREFLY_MODEM_TEST_EG800Z
