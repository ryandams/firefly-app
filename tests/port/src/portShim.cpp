// SPDX-License-Identifier: Apache-2.0
// C shim so the ztest file above can stay plain C.
#include "bundle.hpp"
#include <cstddef>

using namespace firefly::core;

extern "C" {

void host_gnss_start(void) { firefly::port::acquire().gnss->start(); }
void host_gnss_stop(void)  { firefly::port::acquire().gnss->stop(); }

bool host_gnss_poll(double *lat, double *lon)
{
    Position p;
    if (!firefly::port::acquire().gnss->poll(p)) { return false; }
    if (lat) { *lat = p.lat; }
    if (lon) { *lon = p.lon; }
    return true;
}

void host_ble_scan(std::uint32_t window_ms)
{
    firefly::port::acquire().ble->start_scan(window_ms);
}

std::size_t host_ble_read(float *temps, std::size_t max)
{
    TagReading buf[8];
    const std::size_t n = firefly::port::acquire().ble->read_tags(
        buf, (max < 8) ? max : 8);

    for (std::size_t i = 0; i < n && temps; ++i) { temps[i] = buf[i].temp_c; }
    return n;
}

} // extern "C"
