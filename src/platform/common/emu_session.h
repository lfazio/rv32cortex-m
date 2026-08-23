/* SPDX-License-Identifier: Apache-2.0 */
/*
 * emu_session.h - bringing a guest up, and saying what it did.
 *
 * The two runners were one sequence with different ends. In the middle --
 * open the cores, choose the backend, install the hooks, place the image,
 * reset, boot, then afterwards report what happened -- they were the same
 * code twice, and the drift had already started: the host carried a
 * second copy of the newlib syscall ABI, and emu_jit_diff_report existed
 * twice with different formats *and* different limits, so a divergence
 * reported from a board could not be diffed against one from a host.
 *
 * What genuinely differs is the two ends, and they stay per-platform
 * because the difference is real rather than incidental:
 *
 *   acquisition   a host parses argv and reads a file; a board has an
 *                 image linked into it and takes replacements over the
 *                 wire. Neither shape helps the other.
 *
 *   termination   a host exits with a status a test suite reads; a board
 *                 has nowhere to exit to and parks serving its link.
 *
 * Everything between them is here. A third platform writes its two ends
 * and gets the rest, which is the property to keep.
 */
#ifndef EMU_PLATFORM_SESSION_H
#define EMU_PLATFORM_SESSION_H

#include "emu/emu_cpu.h"
#include "emu/emu_dev.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct emu_session_cfg {
    const emu_cpu_ops_t *ops;       /* NULL: the first frontend built in */
    emu_bus_t  *buses;              /* one per core, already given their
                                     * shared regions by the platform    */
    unsigned    ncores;             /* 0: whatever the frontend reports  */

    /*
     * The guest's console UART, and the two ends of it. Initialised here
     * rather than by the caller because it is part of bringing a guest
     * up and was dropped once already by being one line in the middle of
     * a block that moved: the guest's output silently went nowhere and
     * two ctest cases failed with no message.
     */
    emu_uart_t *uart;
    void      (*uart_tx)(void *ctx, uint8_t c);
    int       (*uart_rx)(void *ctx);

    /* The guest image, and where it goes. */
    const uint8_t *image;
    uint32_t       image_size;

    /*
     * Placement, for a platform that lets a caller override it.
     *
     * `load_addr` 0 means EMU_GUEST_ROM_BASE, where the image is already
     * mapped read-only and there is nothing to copy. Anything else is
     * written into the bus, which is what a guest linked to run from RAM
     * needs -- riscv-tests are exactly that, and they also *write to
     * their own image*, so the read-only window would fault them.
     *
     * `entry` 0 means "from the ELF, or the load address".
     */
    uint32_t load_addr;
    uint32_t entry;

    uint32_t ram_base;
    uint32_t ram_size;

    /*
     * Guest RAM as *this* process sees it, so a fresh guest starts on a
     * cleared one. Without that, one test's leftovers become the next
     * test's initial state and a suite's results start depending on the
     * order it ran in -- and a guest whose .bss is garbage does not fail
     * loudly, it produces nothing.
     *
     * NULL leaves RAM alone, for a platform that has already cleared it.
     */
    uint8_t *ram_host;

    /* Installed on every core: any of them may make a system call. */
    emu_syscall_fn syscall_fn;
    void          *syscall_ctx;
    const struct emu_cache_ops *cache_ops;
    emu_unmask_fn  unmask_fn;
    void          *unmask_ctx;

    /* Which backend, for a frontend that has two. */
    bool want_jit;

    /*
     * Whether the report ends with the guest's registers.
     *
     * A board always does: there is no command line to ask on, and the
     * state after a guest stops is most of what a person reading a telnet
     * session came for. A runner does it under --dump, because 378
     * architecture tests do not each want a register dump.
     */
    bool dump_state;

    /*
     * Where a failure is reported. The platforms disagree about the sink
     * -- stderr and a telnet ring against a UART -- so they pass the
     * function rather than this file choosing one.
     */
    void (*fail)(const char *msg, const char *detail);
} emu_session_cfg_t;

/*
 * Open the cores, place the image, reset and boot. False on failure,
 * having already reported through cfg->fail.
 *
 * `sys` is the caller's, because the gdb stub, the interrupt bridge and
 * the run loop all need it and they are the platform's to wire.
 */
bool emu_session_start(emu_system_t *sys, const emu_session_cfg_t *cfg);

/*
 * Place the image and restart, without re-opening the cores.
 *
 * What a reload is: the guest changes, the machine does not. Splitting
 * this out is what stopped the two platforms from each having their own
 * idea of how much of the bring-up an upload repeats -- one of them
 * skipped the frontend's devices and the new guest retired nothing.
 */
bool emu_session_reload(emu_system_t *sys, const emu_session_cfg_t *cfg);

/*
 * What the run did: the counters every platform reports, in one order and
 * one format.
 *
 * `host_cycles` is 0 where a platform has no cycle counter worth quoting,
 * which suppresses the ratio rather than printing a meaningless one.
 * Everything else -- the framework's JIT statistics, the IR optimiser's,
 * the pair histogram -- is the same question wherever it is asked.
 *
 * Everything is here, including the guest's state and the link's
 * counters: they are the same question on every platform, and the one
 * thing that differed -- whether to dump at all -- is a bool rather than
 * a hook each platform implemented differently.
 */
void emu_session_report(emu_system_t *sys, uint64_t retired,
                        uint32_t host_cycles, bool capped, bool dump_state);

#ifdef __cplusplus
}
#endif

#endif /* EMU_PLATFORM_SESSION_H */
