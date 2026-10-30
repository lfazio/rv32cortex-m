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

#define EMU_FAULT_NONE 0u
#define EMU_FAULT_FETCH 1u
#define EMU_FAULT_LOAD 2u
#define EMU_FAULT_STORE 3u

/* ------------------------------------------------------------------ */
/* Execution state                                                     */
/* ------------------------------------------------------------------ */

typedef enum {
    EMU_STATE_RUNNING = 0,
    EMU_STATE_WFI, /* parked waiting for an interrupt            */
    EMU_STATE_HALTED, /* stopped by the debugger or a fatal fault   */
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
    EMU_RUN_BUDGET = 0, /* instruction budget exhausted, call again   */
    EMU_RUN_WFI, /* core entered its wait-for-interrupt state  */
    EMU_RUN_HALTED, /* core halted                                */
    EMU_RUN_BREAKPOINT, /* breakpoint with a debugger attached        */
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

/*
 * Emit a string. The target has no stdio, so this is the whole interface.
 * Here rather than in emu_cpu.h because a backend reports its statistics
 * through one too, and emu_backend.h must not depend on the CPU contract.
 */
typedef void (*emu_print_fn)(void *ctx, const char *s);

#if defined(__GNUC__)
#define EMU_LIKELY(x) __builtin_expect(!!(x), 1)
#define EMU_UNLIKELY(x) __builtin_expect(!!(x), 0)
#define EMU_HOT __attribute__((hot))
#define EMU_ALWAYS_INLINE inline __attribute__((always_inline))
/*
 * Out of line, and placed with the code that is not expected to run.
 *
 * For the rare half of a hot function, when "rare" is not enough: a
 * static helper with one caller is inlined whatever the branch
 * prediction says, and its locals become the hot function's frame. A
 * `uint64_t` whose address is taken, in a helper reached once in a
 * million calls, cost rv_hart_load a stack adjustment on every one of
 * them -- 8% more instructions in that function, measured.
 */
#define EMU_COLD_NOINLINE __attribute__((noinline, cold))
/*
 * Out of line and *not* cold: the half of a hot function that one class
 * of guest runs all the time and another never does -- every access
 * under an operating system, none in M-mode. Kept out of the caller for
 * the same reason as above, without telling the compiler it is rare.
 */
#define EMU_NOINLINE __attribute__((noinline))
/*
 * Out of line and opaque: no clone specialised for a caller's
 * constants, nothing the caller learns from the body. For a function
 * whose only purpose is to keep something out of a hot loop that LTO
 * would otherwise reshape -- see rv_hart_trap_unretire. GCC's noipa;
 * clang has no equivalent and gets noinline.
 */
#if defined(__clang__)
#define EMU_NOIPA __attribute__((noinline))
#else
#define EMU_NOIPA __attribute__((noipa))
#endif
#else
#define EMU_LIKELY(x) (x)
#define EMU_UNLIKELY(x) (x)
#define EMU_HOT
#define EMU_ALWAYS_INLINE inline
#define EMU_COLD_NOINLINE
#define EMU_NOINLINE
#define EMU_NOIPA
#endif

/*
 * EMU_HOT_TEXT -- hot, *and* placed in whatever memory the platform says
 * is fastest to fetch from.
 *
 * A microcontroller with a tightly-coupled instruction memory can put
 * the per-dispatch path there; a host, and a part without one, get plain
 * EMU_HOT and nothing changes. The platform names the section, this file
 * does not know what a TCM is, and neither does any caller.
 *
 * **noinline is load-bearing, not a hint.** A section attribute places
 * the function that carries it; if the compiler inlines the body into a
 * caller that lives somewhere else, the placement silently does nothing
 * and the build still links. That failure is invisible -- same shape as
 * the define that reached the wrong CMake target -- so the check is
 * `nm` on the symbol's address, not the absence of a warning.
 */
#if defined(__GNUC__) && defined(EMU_HOT_TEXT_SECTION)
#define EMU_HOT_TEXT \
    EMU_HOT __attribute__((section(EMU_HOT_TEXT_SECTION), noinline))
#else
#define EMU_HOT_TEXT EMU_HOT
#endif

/*
 * EMU_HOT_DATA -- a frontend's core state, in the memory the platform
 * names for data that every guest instruction touches.
 *
 * Translated code loads and stores guest registers in that struct, so
 * where it lands is a cost on every block. On the F746 the platform
 * names its DTCM: one cycle, never behind the 4 KB D-cache. Left to the
 * linker, the RV32 hart slid into the cached SRAM1 in a build whose
 * `.bss` was ordered differently, and Dhrystone under the JIT read 19.3
 * host cycles an instruction against 16.4, moving with the start of
 * `.bss` because the hart then shared D-cache sets with whatever else
 * was hot; pinned, it read 16.2 at every start (docs/performance.md).
 *
 * For a zero-initialised static only: the platform's section is NOBITS
 * (the F746's is `.bss.dtcm`, which GCC makes NOBITS by its name), and
 * an initialised object there would be silently zeroed by the start-up.
 */
#if defined(__GNUC__) && defined(EMU_HOT_DATA_SECTION)
#define EMU_HOT_DATA __attribute__((section(EMU_HOT_DATA_SECTION)))
#else
#define EMU_HOT_DATA
#endif

/*
 * EMU_INTERP_TEXT -- an interpreter's run loop, in the memory the
 * platform names for it, which is not always the dispatch path's: on the
 * F746 ITCM made ARMv7-M's interpreter 11-17% faster and on the N6 4.8%
 * slower. Nothing at all where no section is named, so a platform that
 * does not opt in builds exactly what it did before. noinline for the
 * reason EMU_HOT_TEXT gives.
 */
#if defined(__GNUC__) && defined(EMU_INTERP_TEXT_SECTION)
#define EMU_INTERP_TEXT __attribute__((section(EMU_INTERP_TEXT_SECTION), noinline))
#else
#define EMU_INTERP_TEXT
#endif

/*
 * EMU_PER_CORE -- state belonging to whichever core is running, kept per
 * host thread.
 *
 * Under EMU_SMP (an SMP host, cmake/freertos.cmake) every emulated core
 * runs on a thread of its own for the whole run -- one FreeRTOS task
 * each -- so per thread *is* per core: the JIT's code cache, block table
 * and translator scratch become each core's own, with no lock anywhere
 * on the dispatch path. G4MH's instruction caches and SYNCI are per PE,
 * so a cache per core is the architecture's model rather than a
 * compromise. Everywhere else this is nothing and the state is the one
 * global it always was.
 *
 * A run that puts several cores on one thread -- --serial, or a system
 * with more cores than this build has tasks for -- shares that thread's
 * state between them, which is exactly the single-threaded arrangement.
 */
#ifndef EMU_SMP
#define EMU_SMP 0
#endif

#if EMU_SMP
#define EMU_PER_CORE _Thread_local
#else
#define EMU_PER_CORE
#endif

#endif /* EMU_TYPES_H */
