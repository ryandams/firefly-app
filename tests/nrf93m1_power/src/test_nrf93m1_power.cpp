// SPDX-License-Identifier: Apache-2.0
//
// Hardware-in-the-loop: the nRF93M1 modem must start fully unpowered, then
// one triggered round must power it up, connect to the cellular network,
// send data, and power it back down again - verified against the REAL
// modem, not a fake port (unlike tests/service's srvNetwork suite). Only
// one of the two ZTEST suites below is compiled per build -
// FIREFLY_MODEM_SERVICE is a mutually exclusive Kconfig choice (see
// Kconfig.firefly) - selected via testcase.yaml's two scenarios.
#include <zephyr/kernel.h>
#include <zephyr/zbus/zbus.h>
#include <zephyr/ztest.h>
#include "event_bus.h"

extern "C"
{
    ZBUS_MSG_SUBSCRIBER_DEFINE(test_sub);
    ZBUS_CHAN_DEFINE(bus_chan, struct bus_msg, NULL, NULL, ZBUS_OBSERVERS(test_sub), ZBUS_MSG_INIT(0));
}

#ifdef CONFIG_FIREFLY_NETWORK

#include <zephyr/device.h>
#include <zephyr/pm/device.h>
#include "srvNetwork.h"

namespace
{

enum pm_device_state modem_pm_state()
{
    const struct device *modem = DEVICE_DT_GET(DT_ALIAS(modem));
    enum pm_device_state state = PM_DEVICE_STATE_ACTIVE;
    zassert_ok(pm_device_state_get(modem, &state));
    return state;
}

int press_button0_and_wait(int timeout_ms)
{
    const uint32_t count = srvNetwork_processed_count();
    const button_event press{0, true};
    const int publish_error = bus_publish_button(&bus_chan, &press);
    if (publish_error != 0)
    {
        return publish_error;
    }
    return srvNetwork_wait_processed(count, timeout_ms);
}

} // namespace

ZTEST(nrf93m1_power_ppp, test_not_powered_then_power_up_connect_send_shutdown)
{
    zassert_equal(modem_pm_state(), PM_DEVICE_STATE_SUSPENDED, "modem must start suspended (unpowered)");

    zassert_ok(srvNetwork_start(&test_sub));
    zassert_equal(modem_pm_state(), PM_DEVICE_STATE_SUSPENDED,
                  "srvNetwork_start() must not power the modem - only a round does");

    zassert_ok(press_button0_and_wait((CONFIG_FIREFLY_NETWORK_CONNECT_TIMEOUT_S + 30) * 1000));
    zassert_equal(srvNetwork_last_result(), 0, "power up/connect/send/shutdown round failed");

    zassert_equal(modem_pm_state(), PM_DEVICE_STATE_SUSPENDED, "modem must be back off (suspended) after the round");

    zassert_ok(srvNetwork_stop());
}
ZTEST_SUITE(nrf93m1_power_ppp, NULL, NULL, NULL, NULL, NULL);

#endif // CONFIG_FIREFLY_NETWORK

#ifdef CONFIG_FIREFLY_MODEM_TEST_NRF93M1

#include "srvModemTest_nrf93m1.h"

namespace
{

int press_button0_and_wait(int timeout_ms)
{
    const uint32_t count = srvModemTest_nrf93m1_processed_count();
    const button_event press{0, true};
    const int publish_error = bus_publish_button(&bus_chan, &press);
    if (publish_error != 0)
    {
        return publish_error;
    }
    return srvModemTest_nrf93m1_wait_processed(count, timeout_ms);
}

} // namespace

ZTEST(nrf93m1_power_raw_at, test_not_powered_then_power_up_connect_send_shutdown)
{
    zassert_false(srvModemTest_nrf93m1_modem_is_powered(), "modem must start unpowered");

    zassert_ok(srvModemTest_nrf93m1_start(&test_sub));
    zassert_false(srvModemTest_nrf93m1_modem_is_powered(),
                  "srvModemTest_nrf93m1_start() must not power the modem - only a round does");

    zassert_ok(press_button0_and_wait((CONFIG_FIREFLY_MODEM_TEST_REG_TIMEOUT_S + 60) * 1000));
    zassert_equal(srvModemTest_nrf93m1_last_result(), 0, "power up/connect/send/shutdown round failed");

    zassert_false(srvModemTest_nrf93m1_modem_is_powered(), "modem must be back off after the round");

    zassert_ok(srvModemTest_nrf93m1_stop());
}
ZTEST_SUITE(nrf93m1_power_raw_at, NULL, NULL, NULL, NULL, NULL);

#endif // CONFIG_FIREFLY_MODEM_TEST_NRF93M1
