// SPDX-License-Identifier: Apache-2.0
//
// High-water mark of SystemView's RTT up-buffer
// (CONFIG_SEGGER_SYSVIEW_RTT_BUFFER_SIZE), to see how close a recording came
// to "*** overflow ***". RTT keeps no peak itself, so a timer samples the
// fill level every kSamplePeriodMs. The buffer fills over many milliseconds
// when the probe falls behind, so the sampled peak is close to the true one.
// The timer's own expiry shows up in SystemView as a small periodic event.
//
// Read it with the "sysview" shell command, or watch g_sysview_peak_bytes in
// a debugger. Built only with CONFIG_SEGGER_SYSTEMVIEW (CMakeLists.txt).
#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/sys/atomic.h>
#include <SEGGER_RTT.h>
#include <SEGGER_SYSVIEW.h>

namespace
{
constexpr uint32_t kSamplePeriodMs = 5;
constexpr unsigned kBufferSize = CONFIG_SEGGER_SYSVIEW_RTT_BUFFER_SIZE;

unsigned bytes_in_buffer()
{
    return SEGGER_RTT_GetBytesInBuffer(static_cast<unsigned>(SEGGER_SYSVIEW_GetChannelID()));
}

} // namespace

// Not static, so a debugger can find it by name. Only the sampling timer
// raises it; the shell's reset clears it.
extern "C" atomic_t g_sysview_peak_bytes;
atomic_t g_sysview_peak_bytes = ATOMIC_INIT(0);

namespace
{
void sample(struct k_timer *)
{
    const atomic_val_t used = static_cast<atomic_val_t>(bytes_in_buffer());
    if (used > atomic_get(&g_sysview_peak_bytes))
    {
        atomic_set(&g_sysview_peak_bytes, used);
    }
}

K_TIMER_DEFINE(g_sample_timer, sample, nullptr);

// APPLICATION level: SystemView (and its RTT channel) is set up earlier.
int start_sampling()
{
    k_timer_start(&g_sample_timer, K_MSEC(kSamplePeriodMs), K_MSEC(kSamplePeriodMs));
    return 0;
}

} // namespace

SYS_INIT(start_sampling, APPLICATION, 0);

#ifdef CONFIG_SHELL
#include <zephyr/shell/shell.h>

namespace
{
int cmd_sysview(const struct shell *sh, size_t, char **)
{
    const unsigned peak = static_cast<unsigned>(atomic_get(&g_sysview_peak_bytes));
    shell_print(sh, "SystemView RTT buffer: peak %u of %u bytes (%u%%), now %u", peak, kBufferSize,
                peak * 100U / kBufferSize, bytes_in_buffer());
    return 0;
}

int cmd_sysview_reset(const struct shell *sh, size_t, char **)
{
    atomic_set(&g_sysview_peak_bytes, 0);
    shell_print(sh, "SystemView RTT buffer peak cleared");
    return 0;
}

} // namespace

SHELL_STATIC_SUBCMD_SET_CREATE(sysview_subcmds, SHELL_CMD_ARG(reset, NULL, "Clear the peak", cmd_sysview_reset, 1, 0),
                               SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(sysview, &sysview_subcmds, "SystemView RTT buffer high-water mark", cmd_sysview);
#endif
