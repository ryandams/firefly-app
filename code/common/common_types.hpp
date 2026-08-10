// SPDX-License-Identifier: Apache-2.0
// Plain value types shared across the application. No Zephyr, no heap.
#ifndef _COMMON_TYPES_HPP_
#define _COMMON_TYPES_HPP_

#include <cstdint>
#include <cstddef>

namespace firefly::core {

using ms_t = std::uint64_t;   // monotonic milliseconds
using ua_t = std::int32_t;    // microamps, signed (charge = negative)

struct Position {
    double lat         = 0.0;
    double lon         = 0.0;
    float  hdop        = 0.0f;
    ms_t   fix_time_ms = 0;
    bool   valid       = false;
};

struct BatteryState {
    float soc_pct      = 0.0f;
    float supercap_pct = 0.0f;
    ua_t  current_ua   = 0;
    float temp_c       = 0.0f;
};

struct TagReading {
    std::uint8_t addr[6] = {};
    float        temp_c  = 0.0f;
    std::int8_t  rssi    = 0;
    bool         valid   = false;
};

enum class Motion : std::uint8_t { Still, Moving, Crash };
enum class Radio  : std::uint8_t { Gnss, Modem, Ble, Sub868 };

} // namespace firefly::core

#endif /* _COMMON_TYPES_HPP_ */
