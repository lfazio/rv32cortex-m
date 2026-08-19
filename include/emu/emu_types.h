/* SPDX-License-Identifier: Apache-2.0 */
/*
 * emu_types.h - Base types and bit helpers shared by every frontend.
 *
 * Nothing here knows about an instruction set. It is the vocabulary the
 * ISA-agnostic runtime (bus, devices, backends, platforms) and every ISA
 * frontend are both written in, so it compiles unchanged for ARMv6-M
 * through ARMv8.1-M and for a native host build.
 */
#ifndef EMU_TYPES_H
#define EMU_TYPES_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* ------------------------------------------------------------------ */
/* Bus access results                                                  */
/* ------------------------------------------------------------------ */

/*
 * What went wrong with a memory access, in terms the bus can express.
 *
 * The bus deals in regions, permissions and access widths; it has no idea
 * what an architecture calls the resulting fault. So it reports the kind of
 * access that failed and the frontend maps that onto its own trap cause --
 * RISC-V's load/store/instruction access fault, RH850's MDP or MIP
 * exception, or whatever the next frontend needs.
 *
 * EMU_FAULT_NONE is 0 so the hot path tests against zero, which on Thumb-2
 * is CBZ rather than a compare against a constant that needs materialising.
 */
typedef uint32_t emu_fault_t;

#define EMU_FAULT_NONE      0u
#define EMU_FAULT_FETCH     1u
#define EMU_FAULT_LOAD      2u
#define EMU_FAULT_STORE     3u

/* ------------------------------------------------------------------ */
/* Execution state                                                     */
/* ------------------------------------------------------------------ */

typedef enum {
    EMU_STATE_RUNNING = 0,
    EMU_STATE_WFI,       /* parked waiting for an interrupt            */
    EMU_STATE_HALTED,    /* stopped by the debugger or a fatal fault   */
    /*
     * Held at reset, never yet started.
     *
     * Distinct from HALTED because the two differ in what may revive
     * them: a halted core is done, and one held at reset is waiting for
     * a *sibling* to release it -- the RH850's BOOTCTRL, where only PE0
     * runs at reset release and asserts a bit per PE to start the
     * others. Overloading HALTED would work today and would make "the
     * debugger stopped this core" and "this core has not booted"
     * indistinguishable in the status line, which is the first thing
     * anyone looks at when a multicore guest prints nothing.
     */
    EMU_STATE_HELD,
} emu_state_t;

/* Reason a backend's run function returned. */
typedef enum {
    EMU_RUN_BUDGET = 0,  /* instruction budget exhausted, call again   */
    EMU_RUN_WFI,         /* core entered its wait-for-interrupt state  */
    EMU_RUN_HALTED,      /* core halted                                */
    EMU_RUN_BREAKPOINT,  /* breakpoint with a debugger attached        */
    /*
     * Made no progress waiting on another core: a store-conditional that
     * failed, a compare-and-exchange that compared unequal, an explicit
     * SNOOZE. There is nothing to do until some other core runs, so a
     * scheduler should move on rather than let the budget drain.
     *
     * Without this a round-robin scheduler livelocks under any spin-wait:
     * the waiting core burns its whole quantum spinning on a value only
     * another core can change.
     */
    EMU_RUN_YIELD,
} emu_run_reason_t;

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

/* Sign-extend the low `bits` bits of v. */
static inline int32_t emu_sext(uint32_t v, unsigned bits)
{
    const unsigned sh = 32u - bits;
    return (int32_t)(v << sh) >> sh;
}

static inline uint32_t emu_bit(uint32_t v, unsigned n)
{
    return (v >> n) & 1u;
}

/* Extract bits [hi:lo] of v (inclusive). */
static inline uint32_t emu_bits(uint32_t v, unsigned hi, unsigned lo)
{
    return (v >> lo) & (0xFFFFFFFFu >> (31u - (hi - lo)));
}

#if defined(__GNUC__)
#  define EMU_LIKELY(x)    __builtin_expect(!!(x), 1)
#  define EMU_UNLIKELY(x)  __builtin_expect(!!(x), 0)
#  define EMU_HOT          __attribute__((hot))
#  define EMU_ALWAYS_INLINE inline __attribute__((always_inline))
#else
#  define EMU_LIKELY(x)    (x)
#  define EMU_UNLIKELY(x)  (x)
#  define EMU_HOT
#  define EMU_ALWAYS_INLINE inline
#endif

#endif /* EMU_TYPES_H */
