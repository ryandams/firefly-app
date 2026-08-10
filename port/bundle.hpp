// SPDX-License-Identifier: Apache-2.0
//
// The single seam between code/core and the outside world.
//
// Exactly one of port/mcu or port/host is compiled into any given build,
// selected by CONFIG_FIREFLY_PORT_HOST. code/ never changes.
//
// This is the HARDWARE seam. Zephyr is linked on both sides of it.
#ifndef _BUNDLE_HPP_
#define _BUNDLE_HPP_

#include "ports.hpp"

namespace firefly::port {

struct Bundle {
    core::IClock       *clock   = nullptr;
    core::IGnss        *gnss    = nullptr;
    core::IBle         *ble     = nullptr;
    core::ISession     *session = nullptr;
    core::IReportStore *store   = nullptr;
    core::IPowerGate   *power   = nullptr;
};

// Implemented once per port. Returns statically allocated adapters - no heap,
// safe to call before the kernel is fully up.
Bundle &acquire();

} // namespace firefly::port

#endif /* _BUNDLE_HPP_ */
