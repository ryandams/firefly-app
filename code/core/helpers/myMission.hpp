// SPDX-License-Identifier: Apache-2.0
//
// myMission - the policy half of srvController.
//
// Deliberately PURE: it owns no ports and performs no I/O. It consumes Events
// and returns Actions for the service layer to execute. That is what makes the
// whole mission policy testable on the host in microseconds, and it is why the
// Zephyr SMF/zbus wiring stays in services/ where it belongs.
//
//   idle -> fix -> build -> session -> sleep      (matches the IBD)
//
#ifndef _MYMISSION_HPP_
#define _MYMISSION_HPP_

#include "common_types.hpp"

namespace firefly::core {

enum class State : std::uint8_t { Boot, Idle, Fix, Build, Session, Sleep };

struct Event {
    enum class Kind : std::uint8_t {
        RtcAlarm,
        Motion,
        FixOk,
        FixTimeout,
        ReportStored,
        SessionOk,
        SessionTransientFail,
        SessionPermanentFail,
        PowerLow,
        PowerOk,
    };

    Kind        kind        = Kind::RtcAlarm;
    Position    pos         = {};
    Motion      motion      = Motion::Still;
    std::size_t queue_depth = 0;
};

struct Action {
    enum class Kind : std::uint8_t {
        None, StartFix, StopFix, BuildReport, RunSession, Sleep, SetRadio,
    };

    Kind        kind        = Kind::None;
    ms_t        duration_ms = 0;      // Sleep, StartFix timeout
    Radio       radio       = Radio::Gnss;
    bool        on          = false;  // SetRadio
    std::size_t batch       = 0;      // RunSession
};

// Fixed capacity - no heap in core.
class ActionList {
public:
    static constexpr std::size_t kMax = 4;

    void add(const Action &a) { if (n_ < kMax) items_[n_++] = a; }
    std::size_t         size() const  { return n_; }
    bool                empty() const { return n_ == 0; }
    const Action       &operator[](std::size_t i) const { return items_[i]; }
    const Action       *begin() const { return items_; }
    const Action       *end()   const { return items_ + n_; }

    // Convenience for tests and for the executor.
    bool has(Action::Kind k) const {
        for (std::size_t i = 0; i < n_; ++i) { if (items_[i].kind == k) return true; }
        return false;
    }
    const Action *find(Action::Kind k) const {
        for (std::size_t i = 0; i < n_; ++i) { if (items_[i].kind == k) return &items_[i]; }
        return nullptr;
    }

private:
    Action      items_[kMax] = {};
    std::size_t n_           = 0;
};

struct Config {
    ms_t        fix_timeout_ms       = 120'000;      // 2 min
    ms_t        report_interval_ms   = 3'600'000;    // 1 h
    std::size_t batch_size           = 6;            // send once this many queued
    ms_t        max_batch_latency_ms = 21'600'000;   // ...or after 6 h regardless
    ms_t        backoff_initial_ms   = 60'000;       // 1 min
    ms_t        backoff_max_ms       = 3'600'000;    // 1 h ceiling
    ms_t        low_power_sleep_ms   = 43'200'000;   // 12 h when battery is low
    float       low_battery_pct      = 5.0f;
};

class Mission {
public:
    // Default-constructible and constant-initialisable on purpose: it lets the
    // service layer declare a file-scope Mission with no dynamic initialiser,
    // no placement new and no <new> header - none of which are available in a
    // -nostdinc++ Zephyr build with the minimal C++ library.
    constexpr Mission() = default;

    explicit Mission(const Config &cfg) : cfg_(cfg) {}

    void configure(const Config &cfg) { cfg_ = cfg; }

    ActionList on(const Event &ev, ms_t now_ms);

    State state()          const { return state_; }
    ms_t  backoff_ms()     const { return backoff_ms_; }
    bool  low_power()      const { return low_power_; }
    ms_t  last_session_ms() const { return last_session_ms_; }

private:
    ActionList begin_fix(ms_t now);
    ActionList after_report(ms_t now, std::size_t depth);
    ActionList go_sleep(ms_t now, ms_t duration);
    bool       should_send(std::size_t depth, ms_t now) const;
    void       grow_backoff();

    Config cfg_             = {};
    State  state_           = State::Boot;
    ms_t   backoff_ms_      = 0;
    ms_t   last_session_ms_ = 0;
    bool   low_power_       = false;
};

} // namespace firefly::core

#endif /* _MYMISSION_HPP_ */
