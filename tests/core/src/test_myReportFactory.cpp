// SPDX-License-Identifier: Apache-2.0
#include <zephyr/ztest.h>
#include "myReportFactory.hpp"

using namespace firefly::core;

ZTEST(firefly_report, test_CopiesScalarInputs)
{
    ReportInputs in;
    in.timestamp_ms   = 1234;
    in.battery.soc_pct = 77.5f;
    in.motion         = Motion::Moving;
    in.board_temp_c   = -11.25f;

    Report r = ReportFactory::build(ReportKind::Periodic, in);

    zassert_equal(r.kind, ReportKind::Periodic);
    zassert_equal(r.timestamp_ms, 1234u);
    zassert_within(r.battery.soc_pct, 77.5f, 1e-5f);
    zassert_equal(r.motion, Motion::Moving);
    zassert_within(r.board_temp_c, -11.25f, 1e-5f);
}

ZTEST(firefly_report, test_NoFixLeavesPositionInvalid)
{
    ReportInputs in;
    Report r = ReportFactory::build(ReportKind::Periodic, in);

    zassert_false(r.position_valid);
}

ZTEST(firefly_report, test_InvalidPositionIsNotCopied)
{
    Position p;
    p.lat = 56.0; p.lon = 10.0; p.valid = false;

    ReportInputs in;
    in.position = &p;

    Report r = ReportFactory::build(ReportKind::Location, in);

    zassert_false(r.position_valid);
    zassert_within(r.position.lat, 0.0, 1e-9, "stale coordinates must not leak into a report");
}

ZTEST(firefly_report, test_ValidPositionIsCopied)
{
    Position p;
    p.lat = 56.1629; p.lon = 10.2039; p.hdop = 1.2f; p.valid = true;

    ReportInputs in;
    in.position = &p;

    Report r = ReportFactory::build(ReportKind::Location, in);

    zassert_true(r.position_valid);
    zassert_within(r.position.lat, 56.1629, 1e-9);
    zassert_within(r.position.hdop, 1.2f, 1e-5f);
}

ZTEST(firefly_report, test_DropsInvalidTags)
{
    TagReading tags[3] = {};
    tags[0].valid = true;  tags[0].temp_c = 4.0f;
    tags[1].valid = false; tags[1].temp_c = 99.0f;
    tags[2].valid = true;  tags[2].temp_c = -18.0f;

    ReportInputs in;
    in.tags = tags; in.tag_count = 3;

    Report r = ReportFactory::build(ReportKind::Periodic, in);

    zassert_equal(r.tag_count, 2u);
    zassert_within(r.tags[0].temp_c, 4.0f, 1e-5f);
    zassert_within(r.tags[1].temp_c, -18.0f, 1e-5f);
}

ZTEST(firefly_report, test_TruncatesRatherThanOverflowing)
{
    TagReading tags[32] = {};
    for (auto &t : tags) { t.valid = true; }

    ReportInputs in;
    in.tags = tags; in.tag_count = 32;

    Report r = ReportFactory::build(ReportKind::Periodic, in);

    zassert_equal(r.tag_count, Report::kMaxTags);
}

ZTEST(firefly_report, test_HandlesNullTagPointer)
{
    ReportInputs in;
    in.tags = nullptr; in.tag_count = 5;   // count without data

    Report r = ReportFactory::build(ReportKind::Event, in);

    zassert_equal(r.tag_count, 0u);
}

ZTEST_SUITE(firefly_report, NULL, NULL, NULL, NULL, NULL);
