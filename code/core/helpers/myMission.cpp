// SPDX-License-Identifier: Apache-2.0
#include "myMission.hpp"

namespace firefly::core {

namespace {
Action radio(Radio r, bool on)
{
    Action a; a.kind = Action::Kind::SetRadio; a.radio = r; a.on = on; return a;
}
Action simple(Action::Kind k)
{
    Action a; a.kind = k; return a;
}
} // namespace

bool Mission::should_send(std::size_t depth, ms_t now) const
{
    if (low_power_)          { return false; }
    if (depth == 0)          { return false; }
    if (depth >= cfg_.batch_size) { return true; }
    return (now - last_session_ms_) >= cfg_.max_batch_latency_ms;
}

void Mission::grow_backoff()
{
    backoff_ms_ = (backoff_ms_ == 0) ? cfg_.backoff_initial_ms : backoff_ms_ * 2;
    if (backoff_ms_ > cfg_.backoff_max_ms) { backoff_ms_ = cfg_.backoff_max_ms; }
}

ActionList Mission::begin_fix(ms_t)
{
    ActionList out;
    state_ = State::Fix;
    out.add(radio(Radio::Gnss, true));

    Action fix; fix.kind = Action::Kind::StartFix; fix.duration_ms = cfg_.fix_timeout_ms;
    out.add(fix);
    return out;
}

ActionList Mission::go_sleep(ms_t, ms_t duration)
{
    ActionList out;
    state_ = State::Sleep;

    Action s; s.kind = Action::Kind::Sleep; s.duration_ms = duration;
    out.add(s);
    return out;
}

ActionList Mission::after_report(ms_t now, std::size_t depth)
{
    ActionList out;

    if (should_send(depth, now)) {
        state_ = State::Session;
        out.add(radio(Radio::Modem, true));

        Action r; r.kind = Action::Kind::RunSession; r.batch = depth;
        out.add(r);
        return out;
    }

    state_ = State::Sleep;
    Action s; s.kind = Action::Kind::Sleep;
    s.duration_ms = low_power_ ? cfg_.low_power_sleep_ms : cfg_.report_interval_ms;
    out.add(s);
    return out;
}

ActionList Mission::on(const Event &ev, ms_t now_ms)
{
    ActionList out;

    // ---- events valid in any state -------------------------------------
    switch (ev.kind) {
    case Event::Kind::PowerLow:
        low_power_ = true;
        return out;

    case Event::Kind::PowerOk:
        low_power_ = false;
        return out;

    case Event::Kind::Motion:
        // A crash pre-empts whatever we were doing: get a fix immediately.
        if (ev.motion == Motion::Crash && state_ != State::Fix) {
            return begin_fix(now_ms);
        }
        return out;

    default:
        break;
    }

    // ---- state-specific -------------------------------------------------
    switch (state_) {
    case State::Boot:
    case State::Idle:
    case State::Sleep:
        if (ev.kind == Event::Kind::RtcAlarm) {
            if (low_power_) {
                // Skip the radios entirely and go straight back to sleep.
                return go_sleep(now_ms, cfg_.low_power_sleep_ms);
            }
            return begin_fix(now_ms);
        }
        break;

    case State::Fix:
        if (ev.kind == Event::Kind::FixOk || ev.kind == Event::Kind::FixTimeout) {
            state_ = State::Build;
            out.add(simple(Action::Kind::StopFix));
            out.add(radio(Radio::Gnss, false));
            out.add(simple(Action::Kind::BuildReport));
            return out;
        }
        break;

    case State::Build:
        if (ev.kind == Event::Kind::ReportStored) {
            return after_report(now_ms, ev.queue_depth);
        }
        break;

    case State::Session:
        switch (ev.kind) {
        case Event::Kind::SessionOk:
            backoff_ms_      = 0;
            last_session_ms_ = now_ms;
            out.add(radio(Radio::Modem, false));
            {
                ActionList s = go_sleep(now_ms, cfg_.report_interval_ms);
                for (const Action &a : s) { out.add(a); }
            }
            return out;

        case Event::Kind::SessionTransientFail:
            grow_backoff();
            out.add(radio(Radio::Modem, false));
            {
                ActionList s = go_sleep(now_ms, backoff_ms_);
                for (const Action &a : s) { out.add(a); }
            }
            return out;

        case Event::Kind::SessionPermanentFail:
            // Do not retry on a schedule the server told us is hopeless.
            last_session_ms_ = now_ms;
            out.add(radio(Radio::Modem, false));
            {
                ActionList s = go_sleep(now_ms, cfg_.max_batch_latency_ms);
                for (const Action &a : s) { out.add(a); }
            }
            return out;

        default:
            break;
        }
        break;
    }

    return out; // unhandled event in this state -> no action
}

} // namespace firefly::core
