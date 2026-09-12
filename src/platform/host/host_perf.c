/* SPDX-License-Identifier: Apache-2.0 */
/*
 * host_perf.c - what the *host* did while the guest ran.
 *
 * Guest instructions per second says how fast the emulated machine is
 * going. It does not say how much work that cost, and the ratio is the
 * number that answers questions about the emulator itself: host
 * instructions per guest instruction is what a JIT improvement moves,
 * and cycles per host instruction is whether the host is stalling.
 *
 * Two sources, and they are not equivalent:
 *
 *   perf_event_open   real counters for this process -- instructions
 *                     retired and CPU cycles, excluding the kernel.
 *                     The only way to get an instruction count.
 *
 *   rdtsc             a time-stamp counter, available to anyone, and
 *                     **not a cycle counter**: it ticks at a fixed rate
 *                     regardless of what the core's clock is doing, so
 *                     it measures elapsed time in disguise. Used only
 *                     when perf is unavailable, and labelled so.
 *
 * **perf is commonly denied.** `kernel.perf_event_paranoid` is 3 on
 * Debian and derivatives, which refuses unprivileged use outright; 2 or
 * lower allows a process to count itself. So the failure is the normal
 * case rather than an error, and it is reported once with the sysctl to
 * change -- not per sample, and not as a crash.
 */

#include "host_perf.h"

#include <stdio.h>
#include <string.h>

#if defined(__linux__)
#include <asm/unistd.h>
#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <errno.h>
#endif

static int g_fd_insns = -1;
static int g_fd_cycles = -1;
static bool g_have_perf;
static bool g_have_tsc;

#if defined(__linux__)
static int perf_open_one(uint32_t config)
{
    struct perf_event_attr attr;

    memset(&attr, 0, sizeof(attr));
    attr.type = PERF_TYPE_HARDWARE;
    attr.size = sizeof(attr);
    attr.config = config;
    /*
     * The emulator's own work only. Counting the kernel would fold in
     * every write() the guest's console makes, which is the host's cost
     * of *printing* rather than of emulating.
     */
    attr.exclude_kernel = 1;
    attr.exclude_hv = 1;
    attr.disabled = 0;
    attr.inherit = 0;

    /* pid 0, cpu -1: this process, wherever it is scheduled. */
    return (int)syscall(__NR_perf_event_open, &attr, 0, -1, -1, 0);
}
#endif

static inline uint64_t read_tsc(void)
{
#if defined(__x86_64__)
    uint32_t lo, hi;

    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
#elif defined(__aarch64__)
    uint64_t v;

    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(v));
    return v;
#else
    return 0u;
#endif
}

void host_perf_init(void)
{
#if defined(__linux__)
    g_fd_insns = perf_open_one(PERF_COUNT_HW_INSTRUCTIONS);
    g_fd_cycles = perf_open_one(PERF_COUNT_HW_CPU_CYCLES);
    g_have_perf = (g_fd_insns >= 0 && g_fd_cycles >= 0);

    if (!g_have_perf) {
        /*
         * Once, and naming the knob. Repeating it per sample would bury
         * the thing it is interrupting, and saying only "unavailable"
         * leaves the reader with nothing to do about it.
         */
        static bool said;

        if (!said) {
            said = true;
            (void)fprintf(stderr,
                          "host counters unavailable (%s); "
                          "sysctl kernel.perf_event_paranoid=2 enables them\n",
                          strerror(errno));
        }
        if (g_fd_insns >= 0) {
            (void)close(g_fd_insns);
        }
        if (g_fd_cycles >= 0) {
            (void)close(g_fd_cycles);
        }
        g_fd_insns = -1;
        g_fd_cycles = -1;
    }
#endif

    g_have_tsc = (read_tsc() != 0u);
}

bool host_perf_have_insns(void)
{
    return g_have_perf;
}

bool host_perf_have_cycles(void)
{
    return g_have_perf || g_have_tsc;
}

bool host_perf_cycles_are_tsc(void)
{
    return !g_have_perf && g_have_tsc;
}

void host_perf_read(uint64_t *insns, uint64_t *cycles)
{
    *insns = 0u;
    *cycles = 0u;

#if defined(__linux__)
    if (g_have_perf) {
        uint64_t v;

        if (read(g_fd_insns, &v, sizeof(v)) == (ssize_t)sizeof(v)) {
            *insns = v;
        }
        if (read(g_fd_cycles, &v, sizeof(v)) == (ssize_t)sizeof(v)) {
            *cycles = v;
        }
        return;
    }
#endif

    if (g_have_tsc) {
        *cycles = read_tsc();
    }
}
