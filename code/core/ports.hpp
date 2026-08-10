// SPDX-License-Identifier: Apache-2.0
//
// Port interfaces: the contract core/ places on the outside world.
// Implemented by port/mcu (nRF54LM20 silicon) and port/host (no silicon).
// Abstract classes only - no exceptions, no RTTI, no heap.
#ifndef _PORTS_HPP_
#define _PORTS_HPP_

#include "common_types.hpp"

namespace firefly::core {

struct IClock {
    virtual ms_t now_ms() const = 0;
protected:
    ~IClock() = default;
};

struct IGnss {
    virtual void start()             = 0;
    virtual void stop()              = 0;
    virtual bool poll(Position &out) = 0;
protected:
    ~IGnss() = default;
};

struct IBle {
    virtual void        start_scan(ms_t window_ms)                  = 0;
    virtual void        stop_scan()                                 = 0;
    virtual std::size_t read_tags(TagReading *out, std::size_t max) = 0;
protected:
    ~IBle() = default;
};

enum class SessionResult : std::uint8_t { Ok, TransientFail, PermanentFail };

struct ISession {
    virtual SessionResult run(std::size_t max_reports) = 0;
protected:
    ~ISession() = default;
};

struct IReportStore {
    virtual bool        push(const void *rec, std::size_t len) = 0;
    virtual std::size_t depth() const                          = 0;
    virtual void        ack(std::size_t count)                 = 0;
protected:
    ~IReportStore() = default;
};

struct IPowerGate {
    virtual void set(Radio r, bool on) = 0;
protected:
    ~IPowerGate() = default;
};

} // namespace firefly::core

#endif /* _PORTS_HPP_ */
