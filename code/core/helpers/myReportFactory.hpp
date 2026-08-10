// SPDX-License-Identifier: Apache-2.0
//
// myReportFactory - composes a Report from the inputs the IBD feeds it:
// position, timestamp, battery+SoC+board temp, motion/orientation, tag temps.
// Pure composition; encoding to LwM2M/protobuf happens in port/.
#ifndef _MYREPORTFACTORY_HPP_
#define _MYREPORTFACTORY_HPP_

#include "common_types.hpp"

namespace firefly::core {

enum class ReportKind : std::uint8_t { Periodic, Event, Location, Config, Crash };

struct Report {
    static constexpr std::size_t kMaxTags = 8;

    ReportKind   kind         = ReportKind::Periodic;
    ms_t         timestamp_ms = 0;
    Position     position     = {};
    BatteryState battery      = {};
    Motion       motion       = Motion::Still;
    float        board_temp_c = 0.0f;

    TagReading   tags[kMaxTags] = {};
    std::size_t  tag_count      = 0;

    bool         position_valid = false;
};

struct ReportInputs {
    ms_t              timestamp_ms = 0;
    const Position   *position     = nullptr;   // null => no fix this cycle
    BatteryState      battery      = {};
    Motion            motion       = Motion::Still;
    float             board_temp_c = 0.0f;
    const TagReading *tags         = nullptr;
    std::size_t       tag_count    = 0;
};

class ReportFactory {
public:
    // Builds a report. Invalid tags are dropped; the list is truncated to
    // kMaxTags rather than overflowing.
    static Report build(ReportKind kind, const ReportInputs &in);
};

} // namespace firefly::core

#endif /* _MYREPORTFACTORY_HPP_ */
