// SPDX-License-Identifier: Apache-2.0
// Mission FSM tests. No Zephyr, no target, no simulator - pure logic.
#include <zephyr/ztest.h>
#include "myMission.hpp"

using namespace firefly::core;

namespace {

Config fast_cfg()
{
    Config c;
    c.fix_timeout_ms       = 60'000;
    c.report_interval_ms   = 3'600'000;
    c.batch_size           = 3;
    c.max_batch_latency_ms = 10'000'000;
    c.backoff_initial_ms   = 60'000;
    c.backoff_max_ms       = 240'000;
    return c;
}

Event alarm()                    { Event e; e.kind = Event::Kind::RtcAlarm;   return e; }
Event fix_ok()                   { Event e; e.kind = Event::Kind::FixOk;      return e; }
Event fix_timeout()              { Event e; e.kind = Event::Kind::FixTimeout; return e; }
Event stored(std::size_t depth)  { Event e; e.kind = Event::Kind::ReportStored; e.queue_depth = depth; return e; }
Event session(Event::Kind k)     { Event e; e.kind = k; return e; }
Event crash()                    { Event e; e.kind = Event::Kind::Motion; e.motion = Motion::Crash; return e; }

// Drive the machine from Sleep/Boot up to the Build state.
void to_build(Mission &m, ms_t t)
{
    m.on(alarm(), t);
    m.on(fix_ok(), t);
}

} // namespace

ZTEST(firefly_mission, test_AlarmStartsFixAndPowersGnss)
{
    Mission m(fast_cfg());

    ActionList a = m.on(alarm(), 1000);

    zassert_equal(m.state(), State::Fix);
    zassert_true(a.has(Action::Kind::StartFix));
    zassert_equal(a.find(Action::Kind::StartFix)->duration_ms, 60'000u);

    const Action *r = a.find(Action::Kind::SetRadio);
    zassert_not_null(r);
    zassert_equal(r->radio, Radio::Gnss);
    zassert_true(r->on);
}

ZTEST(firefly_mission, test_FixOkStopsGnssAndBuildsReport)
{
    Mission m(fast_cfg());
    m.on(alarm(), 0);

    ActionList a = m.on(fix_ok(), 5000);

    zassert_equal(m.state(), State::Build);
    zassert_true(a.has(Action::Kind::StopFix));
    zassert_true(a.has(Action::Kind::BuildReport));

    const Action *r = a.find(Action::Kind::SetRadio);
    zassert_not_null(r);
    zassert_false(r->on, "GNSS must be powered down before building the report");
}

ZTEST(firefly_mission, test_FixTimeoutStillProducesAReport)
{
    Mission m(fast_cfg());
    m.on(alarm(), 0);

    ActionList a = m.on(fix_timeout(), 60'000);

    zassert_equal(m.state(), State::Build);
    zassert_true(a.has(Action::Kind::BuildReport), "a failed fix is still worth reporting");
}

ZTEST(firefly_mission, test_BelowBatchSizeGoesToSleepInsteadOfSession)
{
    Mission m(fast_cfg());
    to_build(m, 0);

    ActionList a = m.on(stored(2), 1000);   // batch_size is 3

    zassert_equal(m.state(), State::Sleep);
    zassert_false(a.has(Action::Kind::RunSession));

    const Action *s = a.find(Action::Kind::Sleep);
    zassert_not_null(s);
    zassert_equal(s->duration_ms, 3'600'000u);
}

ZTEST(firefly_mission, test_ReachingBatchSizeRunsSessionAndPowersModem)
{
    Mission m(fast_cfg());
    to_build(m, 0);

    ActionList a = m.on(stored(3), 1000);

    zassert_equal(m.state(), State::Session);

    const Action *r = a.find(Action::Kind::RunSession);
    zassert_not_null(r);
    zassert_equal(r->batch, 3u);

    const Action *g = a.find(Action::Kind::SetRadio);
    zassert_not_null(g);
    zassert_equal(g->radio, Radio::Modem);
    zassert_true(g->on);
}

ZTEST(firefly_mission, test_MaxLatencyForcesSessionBelowBatchSize)
{
    Mission m(fast_cfg());
    to_build(m, 0);

    // One report queued, but we are past max_batch_latency_ms since boot.
    ActionList a = m.on(stored(1), 10'000'001);

    zassert_equal(m.state(), State::Session);
    zassert_true(a.has(Action::Kind::RunSession), "stale data must not sit in the queue forever");
}

ZTEST(firefly_mission, test_TransientFailureBacksOffExponentiallyAndCaps)
{
    Mission m(fast_cfg());

    const ms_t expected[] = {60'000, 120'000, 240'000, 240'000};

    for (ms_t want : expected) {
        to_build(m, 0);
        m.on(stored(3), 0);
        zassert_equal(m.state(), State::Session);

        ActionList a = m.on(session(Event::Kind::SessionTransientFail), 0);

        zassert_equal(m.backoff_ms(), want);
        const Action *s = a.find(Action::Kind::Sleep);
        zassert_not_null(s);
        zassert_equal(s->duration_ms, want);
    }
}

ZTEST(firefly_mission, test_SuccessResetsBackoffAndModemGoesOff)
{
    Mission m(fast_cfg());

    to_build(m, 0);
    m.on(stored(3), 0);
    m.on(session(Event::Kind::SessionTransientFail), 0);
    zassert_true((m.backoff_ms()) > (0u));

    to_build(m, 0);
    m.on(stored(3), 0);
    ActionList a = m.on(session(Event::Kind::SessionOk), 500'000);

    zassert_equal(m.backoff_ms(), 0u);
    zassert_equal(m.last_session_ms(), 500'000u);
    zassert_equal(m.state(), State::Sleep);

    const Action *r = a.find(Action::Kind::SetRadio);
    zassert_not_null(r);
    zassert_false(r->on);
}

ZTEST(firefly_mission, test_LowPowerSkipsRadiosEntirely)
{
    Mission m(fast_cfg());

    Event low; low.kind = Event::Kind::PowerLow;
    m.on(low, 0);
    zassert_true(m.low_power());

    ActionList a = m.on(alarm(), 1000);

    zassert_equal(m.state(), State::Sleep);
    zassert_false(a.has(Action::Kind::StartFix));

    const Action *s = a.find(Action::Kind::Sleep);
    zassert_not_null(s);
    zassert_equal(s->duration_ms, 43'200'000u);
}

ZTEST(firefly_mission, test_LowPowerHoldsReportsBackFromTheModem)
{
    Mission m(fast_cfg());
    to_build(m, 0);

    Event low; low.kind = Event::Kind::PowerLow;
    m.on(low, 0);

    ActionList a = m.on(stored(99), 0);   // far past batch size

    zassert_false(a.has(Action::Kind::RunSession), "a flat battery must not be spent on a modem session");
    zassert_equal(m.state(), State::Sleep);
}

ZTEST(firefly_mission, test_PowerOkClearsLowPower)
{
    Mission m(fast_cfg());

    Event low; low.kind = Event::Kind::PowerLow;
    Event ok;  ok.kind  = Event::Kind::PowerOk;

    m.on(low, 0);
    m.on(ok, 0);

    zassert_false(m.low_power());
    m.on(alarm(), 0);
    zassert_equal(m.state(), State::Fix);
}

ZTEST(firefly_mission, test_CrashPreemptsSleepAndForcesAFix)
{
    Mission m(fast_cfg());
    to_build(m, 0);
    m.on(stored(1), 0);
    zassert_equal(m.state(), State::Sleep);

    ActionList a = m.on(crash(), 12'345);

    zassert_equal(m.state(), State::Fix);
    zassert_true(a.has(Action::Kind::StartFix));
}

ZTEST(firefly_mission, test_CrashDuringFixDoesNotRestartTheFix)
{
    Mission m(fast_cfg());
    m.on(alarm(), 0);
    zassert_equal(m.state(), State::Fix);

    ActionList a = m.on(crash(), 100);

    zassert_equal(m.state(), State::Fix);
    zassert_true(a.empty());
}

ZTEST(firefly_mission, test_StrayEventsInWrongStateAreIgnored)
{
    Mission m(fast_cfg());

    ActionList a = m.on(session(Event::Kind::SessionOk), 0);

    zassert_true(a.empty());
    zassert_equal(m.state(), State::Boot);
}

ZTEST(firefly_mission, test_ActionListNeverOverflows)
{
    ActionList a;
    for (int i = 0; i < 20; ++i) { a.add(Action{}); }

    zassert_equal(a.size(), ActionList::kMax);
}

ZTEST_SUITE(firefly_mission, NULL, NULL, NULL, NULL, NULL);
