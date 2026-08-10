// SPDX-License-Identifier: Apache-2.0
#include "myReportFactory.hpp"

namespace firefly::core {

Report ReportFactory::build(ReportKind kind, const ReportInputs &in)
{
    Report r;
    r.kind         = kind;
    r.timestamp_ms = in.timestamp_ms;
    r.battery      = in.battery;
    r.motion       = in.motion;
    r.board_temp_c = in.board_temp_c;

    if (in.position != nullptr && in.position->valid) {
        r.position       = *in.position;
        r.position_valid = true;
    }

    if (in.tags != nullptr) {
        for (std::size_t i = 0; i < in.tag_count && r.tag_count < Report::kMaxTags; ++i) {
            if (in.tags[i].valid) {
                r.tags[r.tag_count++] = in.tags[i];
            }
        }
    }

    return r;
}

} // namespace firefly::core
