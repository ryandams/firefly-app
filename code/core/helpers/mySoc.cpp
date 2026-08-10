// SPDX-License-Identifier: Apache-2.0
#include "mySoc.hpp"

namespace firefly::core {

namespace {
constexpr float clampf(float v, float lo, float hi)
{
    return (v < lo) ? lo : (v > hi) ? hi : v;
}
} // namespace

void Soc::sample(ua_t current_ua, float temp_c, ms_t now_ms)
{
    temp_c_ = temp_c;

    if (!seeded_) { last_ms_ = now_ms; seeded_ = true; return; }
    if (now_ms <= last_ms_) { return; }   // non-monotonic or duplicate sample

    const ms_t  dt_ms = now_ms - last_ms_;
    const float hours = static_cast<float>(dt_ms) / 3'600'000.0f;
    const float ma    = static_cast<float>(current_ua) / 1000.0f;

    used_mah_ += ma * hours;
    if (used_mah_ < 0.0f) { used_mah_ = 0.0f; }   // charge cannot exceed full

    last_ms_ = now_ms;
}

float Soc::derated_capacity_mah() const
{
    if (temp_c_ >= cfg_.derate_below_c) { return cfg_.capacity_mah; }

    const float below   = cfg_.derate_below_c - temp_c_;
    const float lost    = below * cfg_.derate_pct_per_c;
    const float pct     = clampf(100.0f - lost, cfg_.derate_floor_pct, 100.0f);

    return cfg_.capacity_mah * (pct / 100.0f);
}

float Soc::battery_pct() const
{
    const float cap = derated_capacity_mah();
    if (cap <= 0.0f) { return 0.0f; }

    return clampf(100.0f * (1.0f - used_mah_ / cap), 0.0f, 100.0f);
}

float Soc::supercap_pct(float mv) const
{
    const float span = cfg_.supercap_full_mv - cfg_.supercap_empty_mv;
    if (span <= 0.0f) { return 0.0f; }

    return clampf(100.0f * (mv - cfg_.supercap_empty_mv) / span, 0.0f, 100.0f);
}

} // namespace firefly::core
