// SPDX-License-Identifier: Apache-2.0
#include <zephyr/ztest.h>
#include "mySoc.hpp"

using namespace firefly::core;

namespace {
SocConfig cfg()
{
    SocConfig c;
    c.capacity_mah      = 1000.0f;
    c.supercap_full_mv  = 3600.0f;
    c.supercap_empty_mv = 2200.0f;
    c.derate_below_c    = 0.0f;
    c.derate_pct_per_c  = 1.0f;
    c.derate_floor_pct  = 50.0f;
    return c;
}
constexpr ms_t kHour = 3'600'000;
} // namespace

ZTEST(firefly_soc, test_StartsFullAfterReset)
{
    Soc s(cfg());
    s.reset(100.0f, 0);

    zassert_within(s.used_mah(), 0.0f, 1e-5f);
    zassert_within(s.battery_pct(), 100.0f, 1e-5f);
}

ZTEST(firefly_soc, test_CoulombCountsOneHourAtOneHundredMilliamps)
{
    Soc s(cfg());
    s.reset(100.0f, 0);

    s.sample(100'000, 25.0f, kHour);   // 100 mA for 1 h = 100 mAh

    zassert_within(s.used_mah(), 100.0f, 0.01f);
    zassert_within(s.battery_pct(), 90.0f, 0.01f);
}

ZTEST(firefly_soc, test_AccumulatesAcrossSamples)
{
    Soc s(cfg());
    s.reset(100.0f, 0);

    for (int i = 1; i <= 4; ++i) {
        s.sample(50'000, 25.0f, kHour * static_cast<ms_t>(i));  // 50 mA
    }

    zassert_within(s.used_mah(), 200.0f, 0.01f);
    zassert_within(s.battery_pct(), 80.0f, 0.01f);
}

ZTEST(firefly_soc, test_ChargeCurrentCannotPushAboveFull)
{
    Soc s(cfg());
    s.reset(100.0f, 0);

    s.sample(-500'000, 25.0f, kHour);   // charging

    zassert_within(s.used_mah(), 0.0f, 1e-5f);
    zassert_within(s.battery_pct(), 100.0f, 1e-5f);
}

ZTEST(firefly_soc, test_IgnoresNonMonotonicTime)
{
    Soc s(cfg());
    s.reset(100.0f, kHour);

    s.sample(100'000, 25.0f, 0);        // clock went backwards
    s.sample(100'000, 25.0f, kHour);    // duplicate timestamp

    zassert_within(s.used_mah(), 0.0f, 1e-5f);
}

ZTEST(firefly_soc, test_ColdDeratesUsableCapacity)
{
    Soc s(cfg());
    s.reset(100.0f, 0);

    s.sample(0, -20.0f, 1);             // 20 C below, 1 %/C -> 80 % capacity

    zassert_within(s.derated_capacity_mah(), 800.0f, 0.01f);
}

ZTEST(firefly_soc, test_DeratingHasAFloor)
{
    Soc s(cfg());
    s.reset(100.0f, 0);

    s.sample(0, -90.0f, 1);             // would be -90 % without the floor

    zassert_within(s.derated_capacity_mah(), 500.0f, 0.01f);
}

ZTEST(firefly_soc, test_ColdReportsLowerPercentForSameChargeDrawn)
{
    Soc warm(cfg());
    warm.reset(100.0f, 0);
    warm.sample(200'000, 25.0f, kHour);   // 200 mAh used

    Soc cold(cfg());
    cold.reset(100.0f, 0);
    cold.sample(200'000, -20.0f, kHour);  // same charge, 800 mAh capacity

    zassert_within(warm.battery_pct(), 80.0f, 0.01f);
    zassert_within(cold.battery_pct(), 75.0f, 0.01f);
    zassert_true((cold.battery_pct()) < (warm.battery_pct()));
}

ZTEST(firefly_soc, test_PercentClampsAtZero)
{
    Soc s(cfg());
    s.reset(100.0f, 0);

    s.sample(2'000'000, 25.0f, kHour);   // 2000 mAh from a 1000 mAh cell

    zassert_within(s.battery_pct(), 0.0f, 1e-5f);
}

ZTEST(firefly_soc, test_SupercapScalesBetweenEmptyAndFull)
{
    Soc s(cfg());

    zassert_within(s.supercap_pct(2200.0f), 0.0f, 1e-5f);
    zassert_within(s.supercap_pct(3600.0f), 100.0f, 1e-5f);
    zassert_within(s.supercap_pct(2900.0f), 50.0f, 0.01f);
    zassert_within(s.supercap_pct(1000.0f), 0.0f, 1e-5f);     // clamped
    zassert_within(s.supercap_pct(9999.0f), 100.0f, 1e-5f);   // clamped
}

ZTEST_SUITE(firefly_soc, NULL, NULL, NULL, NULL, NULL);
