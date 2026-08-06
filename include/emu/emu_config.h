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
#  define EMU_MAX_REGIONS 16
#endif

/* ------------------------------------------------------------------ */
/* Execution engine                                                    */
/* ------------------------------------------------------------------ */

/*
 * How many instructions a backend's run function executes before returning
 * to the host so it can service ARM-side work (timers, USB, RTOS ticks).
 */
#ifndef EMU_DEFAULT_BUDGET
#  define EMU_DEFAULT_BUDGET 4096u
#endif

/* ------------------------------------------------------------------ */
/* Debug / diagnostics                                                 */
/* ------------------------------------------------------------------ */

/* Per-instruction trace hook. Slow; enable only when chasing a bug. */
#ifndef EMU_ENABLE_TRACE
#  define EMU_ENABLE_TRACE 0
#endif

/* Count executed instructions / traps / bus faults for `stats`. */
#ifndef EMU_ENABLE_STATS
#  define EMU_ENABLE_STATS 1
#endif

#endif /* EMU_CONFIG_H */
