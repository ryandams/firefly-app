// SPDX-License-Identifier: Apache-2.0
//
// mySoc - battery / supercap state of charge by coulomb counting.
// Pure arithmetic over samples fed in by the ADC port. No Zephyr.
#ifndef _MYSOC_HPP_
#define _MYSOC_HPP_

#include "common_types.hpp"

namespace firefly::core {

struct SocConfig {
    float capacity_mah      = 19000.0f;  // LiSOCl2 nominal
    float supercap_full_mv  = 3600.0f;
    float supercap_empty_mv = 2200.0f;
    // Usable capacity falls off at low temperature; simple linear derating.
    float derate_below_c    = 0.0f;
    float derate_pct_per_c  = 1.2f;      // % capacity lost per degree below
    float derate_floor_pct  = 45.0f;     // never derate below this fraction
};

class Soc {
public:
    explicit Soc(const SocConfig &cfg) : cfg_(cfg) {}

    // Seed from a known starting point (e.g. restored from storage).
    void reset(float soc_pct, ms_t now_ms)
    {
        used_mah_  = cfg_.capacity_mah * (1.0f - soc_pct / 100.0f);
        last_ms_   = now_ms;
        seeded_    = true;
    }

    // Integrate one current sample. Positive current_ua = discharge.
    void sample(ua_t current_ua, float temp_c, ms_t now_ms);

    float battery_pct() const;
    float supercap_pct(float mv) const;
    float used_mah()    const { return used_mah_; }
    float derated_capacity_mah() const;

private:
    SocConfig cfg_;
    float     used_mah_ = 0.0f;
    float     temp_c_   = 25.0f;
    ms_t      last_ms_  = 0;
    bool      seeded_   = false;
};

} // namespace firefly::core

#endif /* _MYSOC_HPP_ */
