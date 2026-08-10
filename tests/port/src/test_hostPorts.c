/* SPDX-License-Identifier: Apache-2.0
 *
 * The host ports running under the real Zephyr kernel. Keep this file for
 * behaviour that genuinely needs threads, time and the scheduler - pure logic
 * belongs in tests/core.
 */
#include <zephyr/ztest.h>
#include <zephyr/kernel.h>

/* C wrappers over the C++ port bundle, defined in port_test_shim.cpp. */
extern void        host_gnss_start(void);
extern void        host_gnss_stop(void);
extern bool        host_gnss_poll(double *lat, double *lon);
extern size_t      host_ble_read(float *temps, size_t max);
extern void        host_ble_scan(uint32_t window_ms);

ZTEST(firefly_ports, test_gnss_needs_time_to_fix)
{
	double lat, lon;

	host_gnss_start();
	zassert_false(host_gnss_poll(&lat, &lon),
		      "fix must not be instant - TTFF has to be observable");

	k_sleep(K_MSEC(CONFIG_FIREFLY_HOST_TTFF_MS + 100));

	zassert_true(host_gnss_poll(&lat, &lon), "fix expected after TTFF");
	zassert_true(lat > 55.0 && lat < 57.0, "latitude out of scripted range");

	host_gnss_stop();
}

ZTEST(firefly_ports, test_ble_returns_tags_only_while_scanning)
{
	float temps[4];

	zassert_equal(host_ble_read(temps, 4), 0, "no tags before scanning");

	host_ble_scan(500);
	zassert_true(host_ble_read(temps, 4) > 0, "tags expected while scanning");
}

ZTEST_SUITE(firefly_ports, NULL, NULL, NULL, NULL, NULL);
