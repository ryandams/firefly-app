// SPDX-License-Identifier: Apache-2.0
//
// srvController - the mechanism half. All policy lives in myMission; this file
// only translates zbus messages into Events and Actions into port calls.
// If you find yourself adding an `if` here, it probably belongs in core/.
#include "srvController.h"

#include "myMission.hpp"
#include "bundle.hpp"
#include "firefly.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(srvController, CONFIG_FIREFLY_LOG_LEVEL);

using namespace firefly::core;

namespace {

Config default_config()
{
    Config c;
    c.fix_timeout_ms       = CONFIG_FIREFLY_FIX_TIMEOUT_MS;
    c.report_interval_ms   = CONFIG_FIREFLY_REPORT_INTERVAL_S * 1000ULL;
    c.batch_size           = CONFIG_FIREFLY_BATCH_SIZE;
    c.max_batch_latency_ms = CONFIG_FIREFLY_MAX_BATCH_LATENCY_S * 1000ULL;
    return c;
}

// Constant-initialised: no dynamic initialiser, no placement new, no heap.
// Zephyr builds C++ with -nostdinc++ and the minimal C++ library, where <new>
// is not something to rely on.
Mission                  g_mission;
firefly::port::Bundle   *g_ports;
bool                     g_ready;

Event to_event(const struct firefly_msg &m)
{
    Event e;
    e.kind        = static_cast<Event::Kind>(m.evt);
    e.motion      = static_cast<Motion>(m.motion);
    e.queue_depth = m.queue_depth;
    return e;
}

void execute(const Action &a)
{
    switch (a.kind) {
    case Action::Kind::StartFix:
        g_ports->gnss->start();
        break;

    case Action::Kind::StopFix:
        g_ports->gnss->stop();
        break;

    case Action::Kind::SetRadio:
        g_ports->power->set(a.radio, a.on);
        break;

    case Action::Kind::BuildReport:
        // TODO: gather inputs, myReportFactory::build(), push to store, then
        //       publish FIREFLY_EVT_REPORT_STORED with the new queue depth.
        LOG_INF("build report");
        break;

    case Action::Kind::RunSession:
        // TODO: hand off to srvNetwork; publish the SESSION_* event when it
        //       completes rather than blocking here.
        LOG_INF("run session, batch %u", static_cast<unsigned>(a.batch));
        break;

    case Action::Kind::Sleep:
        // TODO: srvBoard arms the RTC and executes the sleep.
        LOG_INF("sleep %u ms", static_cast<unsigned>(a.duration_ms));
        break;

    case Action::Kind::None:
    default:
        break;
    }
}

} // namespace

extern "C" void srvController_init(void)
{
    g_ports = &firefly::port::acquire();
    g_mission.configure(default_config());
    g_ready = true;

    LOG_INF("srvController ready");
}

extern "C" void srvController_dispatch(const struct firefly_msg *msg)
{
    if (msg == nullptr || !g_ready) { return; }

    const ms_t       now = g_ports->clock->now_ms();
    const ActionList as  = g_mission.on(to_event(*msg), now);

    for (const Action &a : as) { execute(a); }
}

extern "C" uint8_t srvController_state(void)
{
    return g_ready ? static_cast<uint8_t>(g_mission.state()) : 0U;
}
