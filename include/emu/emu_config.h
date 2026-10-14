/* SPDX-License-Identifier: Apache-2.0 */
/*
 * emu_config.h - Compile-time configuration of the ISA-agnostic runtime.
 *
 * Everything here is a property of the emulator itself rather than of the
 * guest architecture: how big the bus region table is, how long a backend
 * runs before returning, and which diagnostics are compiled in. Per-ISA
 * knobs live in that frontend's own config header (include/rv32/rv_config.h,
 * include/g4mh/g4mh_config.h).
 *
 * Every knob may be overridden from the build system (-DEMU_xxx=...).
 */
#ifndef EMU_CONFIG_H
#define EMU_CONFIG_H

/* ------------------------------------------------------------------ */
/* Bus configuration                                                   */
/* ------------------------------------------------------------------ */

/*
 * Maximum number of regions in a bus region table. Passthrough platforms
 * need more than they first appear to: protecting individual peripheral
 * registers means splitting a window into several entries.
 */
#ifndef EMU_MAX_REGIONS
#define EMU_MAX_REGIONS 16
#endif

/* ------------------------------------------------------------------ */
/* Execution engine                                                    */
/* ------------------------------------------------------------------ */

/*
 * How many instructions a backend's run function executes before returning
 * to the host so it can service host-side work: the console, the guest's
 * clock, and on a multicore guest the other cores, which meet once per
 * budget. It is the host runner's --quantum default; the boards pass their
 * own (EMU_RUN_SLICE in src/platform/stm32/board.c).
 *
 * **The guest's clock moves once per budget**, so this is also its
 * resolution: two reads of `time` less than a budget apart can return the
 * same value. riscv-arch-test's Zicntr cases read it twice about 11,000
 * instructions apart and fail from a budget of 11,000 up.
 */
#ifndef EMU_DEFAULT_BUDGET
#define EMU_DEFAULT_BUDGET 4096u
#endif

/*
 * The default instead when an SMP host runs a multicore guest's cores in
 * parallel, where every round is a barrier across host threads: three
 * PEs of independent work went 1.85x at 4096 to 2.67x here translated,
 * and 65536 bought nothing more. The clock's resolution is the price, as
 * above; docs/os.md ("the quantum") has what was measured against it.
 */
#ifndef EMU_PARALLEL_BUDGET
#define EMU_PARALLEL_BUDGET 32768u
#endif

/* ------------------------------------------------------------------ */
/* Debug / diagnostics                                                 */
/* ------------------------------------------------------------------ */

/* Per-instruction trace hook. Slow; enable only when chasing a bug. */
#ifndef EMU_ENABLE_TRACE
#define EMU_ENABLE_TRACE 0
#endif

/* Count executed instructions / traps / bus faults for `stats`. */
#ifndef EMU_ENABLE_STATS
#define EMU_ENABLE_STATS 1
#endif

#endif /* EMU_CONFIG_H */
