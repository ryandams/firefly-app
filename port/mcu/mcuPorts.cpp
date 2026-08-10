// SPDX-License-Identifier: Apache-2.0
//
// Real adapters. SKELETON - each method is the place where the corresponding
// driver/subsystem from the IBD gets wired in. Nothing above this file needs
// to change as these fill out.
//
//   IGnss        -> quectel_l96 via Zephyr GNSS API      (drivers/gnss)
//   IBle         -> BLE host: GATT reads + channel sounding
//   ISession     -> srvNetwork/myLwm2m over the nRF93M1 modem
//   IReportStore -> srvStorage -> myReportStore -> FCB on external QSPI NOR
//   IPowerGate   -> drvGpio + PCAL6416A rails
//
// Helpers from the IBD that drive silicon (myLwm2m over the modem,
// myCredentialStore over CRACEN/KMU) belong here, not in code/core/helpers.
#include "bundle.hpp"
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(firefly_port_mcu, CONFIG_FIREFLY_LOG_LEVEL);

namespace firefly::port {
namespace {

using namespace firefly::core;

class McuClock final : public IClock {
public:
    ms_t now_ms() const override { return static_cast<ms_t>(k_uptime_get()); }
};

class McuGnss final : public IGnss {
public:
    void start() override
    {
        // TODO: power the L96 rail, then pm_device_action_run(dev, RESUME).
        LOG_WRN("McuGnss::start not implemented");
    }
    void stop() override
    {
        // TODO: suspend device, drop the rail, keep EASY ephemeris warm.
        LOG_WRN("McuGnss::stop not implemented");
    }
    bool poll(Position &) override
    {
        // TODO: consume the gnss_data callback published by the driver.
        return false;
    }
};

class McuBle final : public IBle {
public:
    void start_scan(ms_t) override { LOG_WRN("McuBle::start_scan not implemented"); }
    void stop_scan() override      {}
    std::size_t read_tags(TagReading *, std::size_t) override
    {
        // TODO: bt_le_scan_start + GATT read of the tag temperature char.
        return 0;
    }
};

class McuSession final : public ISession {
public:
    SessionResult run(std::size_t) override
    {
        // TODO: bring up modem/PPP, run the LwM2M session, ack on success.
        LOG_WRN("McuSession::run not implemented");
        return SessionResult::TransientFail;
    }
};

class McuStore final : public IReportStore {
public:
    bool push(const void *, std::size_t) override { return false; }
    std::size_t depth() const override            { return 0; }
    void ack(std::size_t) override                {}
};

class McuPowerGate final : public IPowerGate {
public:
    void set(Radio, bool) override
    {
        // TODO: gpio_pin_set_dt() on the PCAL6416A rail for this radio.
    }
};

McuClock     g_clock;
McuGnss      g_gnss;
McuBle       g_ble;
McuSession   g_session;
McuStore     g_store;
McuPowerGate g_power;

} // namespace

Bundle &acquire()
{
    static Bundle b{ &g_clock, &g_gnss, &g_ble, &g_session, &g_store, &g_power };
    return b;
}

} // namespace firefly::port
