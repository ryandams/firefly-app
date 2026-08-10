// SPDX-License-Identifier: Apache-2.0
//
// Host ports: the port interfaces satisfied without silicon.
//
// Note what is NOT here: no Bluetooth host, no HCI driver, no radio. srvBle's
// contract with the rest of the system is "tag readings and CS results", so
// that is what we fake. Compiling the real BLE stack to exercise the mission
// state machine would be testing the wrong layer.
//
// Scenarios are deliberately scriptable so a native_sim run is reproducible.
#include "bundle.hpp"
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(firefly_port_host, CONFIG_FIREFLY_LOG_LEVEL);

namespace firefly::port {
namespace {

using namespace firefly::core;

// ---------------------------------------------------------------- clock
class HostClock final : public IClock {
public:
    ms_t now_ms() const override { return static_cast<ms_t>(k_uptime_get()); }
};

// ----------------------------------------------------------------- gnss
// Replays a canned track. Every Nth acquisition fails outright so the fix
// timeout path gets exercised without waiting for bad sky.
class HostGnss final : public IGnss {
public:
    void start() override
    {
        running_    = true;
        started_ms_ = k_uptime_get();
        ++attempts_;
        LOG_INF("host gnss: acquiring (attempt %u)", attempts_);
    }

    void stop() override { running_ = false; }

    bool poll(Position &out) override
    {
        if (!running_) { return false; }

        const int64_t elapsed = k_uptime_get() - started_ms_;
        if (elapsed < CONFIG_FIREFLY_HOST_TTFF_MS) { return false; }

        if (CONFIG_FIREFLY_HOST_FIX_FAIL_EVERY > 0 &&
            (attempts_ % CONFIG_FIREFLY_HOST_FIX_FAIL_EVERY) == 0) {
            return false;   // never resolves -> service raises FixTimeout
        }

        out.lat         = 56.1629 + 0.0001 * static_cast<double>(attempts_);
        out.lon         = 10.2039 + 0.0001 * static_cast<double>(attempts_);
        out.hdop        = 1.1f;
        out.fix_time_ms = static_cast<ms_t>(elapsed);
        out.valid       = true;
        return true;
    }

private:
    bool          running_    = false;
    std::int64_t  started_ms_ = 0;
    unsigned      attempts_   = 0;
};

// ------------------------------------------------------------------ ble
class HostBle final : public IBle {
public:
    void start_scan(ms_t window_ms) override
    {
        scanning_ = true;
        LOG_INF("host ble: scan window %u ms", static_cast<unsigned>(window_ms));
    }

    void stop_scan() override { scanning_ = false; }

    std::size_t read_tags(TagReading *out, std::size_t max) override
    {
        if (!scanning_ || out == nullptr) { return 0; }

        const std::size_t n = (max < kTags) ? max : kTags;
        for (std::size_t i = 0; i < n; ++i) {
            out[i]         = {};
            out[i].addr[5] = static_cast<std::uint8_t>(i + 1);
            // Drifts around a freezer setpoint so alarm logic has something
            // to react to.
            out[i].temp_c  = -18.0f + 0.5f * static_cast<float>((cycle_ + i) % 8);
            out[i].rssi    = static_cast<std::int8_t>(-55 - static_cast<int>(i) * 6);
            out[i].valid   = true;
        }
        ++cycle_;
        return n;
    }

private:
    static constexpr std::size_t kTags = 3;
    bool                         scanning_ = false;
    unsigned                     cycle_    = 0;
};

// -------------------------------------------------------------- session
// Fails the first N attempts so the exponential backoff path is reachable
// in a native_sim run.
class HostSession final : public ISession {
public:
    SessionResult run(std::size_t max_reports) override
    {
        ++calls_;
        if (calls_ <= CONFIG_FIREFLY_HOST_SESSION_FAILS) {
            LOG_WRN("host session: transient failure %u", calls_);
            return SessionResult::TransientFail;
        }
        LOG_INF("host session: uploaded %u report(s)",
                static_cast<unsigned>(max_reports));
        return SessionResult::Ok;
    }

private:
    unsigned calls_ = 0;
};

// ---------------------------------------------------------------- store
// Backed by plain RAM here. The real ZMS/FCB path is exercised separately by
// pointing port/mcu at Zephyr's flash_simulator on native_sim.
class HostStore final : public IReportStore {
public:
    bool push(const void *, std::size_t) override
    {
        if (depth_ >= kCap) { return false; }
        ++depth_;
        return true;
    }
    std::size_t depth() const override { return depth_; }
    void ack(std::size_t count) override
    {
        depth_ = (count >= depth_) ? 0 : depth_ - count;
    }

private:
    static constexpr std::size_t kCap = 64;
    std::size_t                  depth_ = 0;
};

// ---------------------------------------------------------------- power
class HostPowerGate final : public IPowerGate {
public:
    void set(Radio r, bool on) override
    {
        static const char *name[] = {"gnss", "modem", "ble", "868"};
        LOG_INF("host power: %s -> %s", name[static_cast<int>(r)], on ? "on" : "off");
    }
};

// Static storage - no heap anywhere in the port layer.
HostClock     g_clock;
HostGnss      g_gnss;
HostBle       g_ble;
HostSession   g_session;
HostStore     g_store;
HostPowerGate g_power;

} // namespace

Bundle &acquire()
{
    static Bundle b{ &g_clock, &g_gnss, &g_ble, &g_session, &g_store, &g_power };
    return b;
}

} // namespace firefly::port
