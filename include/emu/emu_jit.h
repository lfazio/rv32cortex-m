/* SPDX-License-Identifier: Apache-2.0 */
/*
 * emu_jit.h - What every JIT backend does the same way.
 *
 * Three JITs in this tree were written independently and converged on the
 * same skeleton: a code buffer, a table of translated blocks keyed on
 * guest pc, a hash to find them, a flush when something they baked in
 * changes, and a dispatch loop that looks up, translates on a miss, and
 * falls back to the interpreter for whatever the translator declines.
 * None of that is architecture-specific in either direction -- not in the
 * guest ISA, not in the host.
 *
 * What *is* specific stays with the frontend: deciding where a block ends,
 * emitting the instructions, and knowing what it baked in. That is the
 * emu_jit_ops_t below.
 *
 * The split is drawn where it is because of what the Thumb-2 backend cost
 * to get right. Its bugs were almost all in the common half -- a block
 * outliving the state it was specialised on, a budget the dispatch loop
 * could overshoot, a flush that watched a flag instead of the
 * configuration -- and every one of them would otherwise be rewritten from
 * scratch for each new frontend. Fixing them once is the point.
 */
#ifndef EMU_JIT_H
#define EMU_JIT_H

#include "emu_backend.h"
#include "emu_cpu.h"

/*
 * Which host this build can emit for.
 *
 * In the emu layer rather than in a frontend's config, because a JIT is
 * now a property of the pair: the same x86-64 emitter serves RV32 and
 * G4MH. Deriving it from rv32/rv_config.h -- which is where it used to
 * live -- meant a frontend that did not include that header compiled its
 * whole JIT away and silently ran interpreted, which is exactly what
 * happened to the first G4MH build.
 */
#if defined(__x86_64__) && defined(__linux__)
#  define EMU_JIT_X86_64 1
#endif
#if defined(__ARM_ARCH) && (__ARM_ARCH >= 7) && defined(__thumb2__)
#  define EMU_JIT_THUMB2 1
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Statistics                                                          */
/* ------------------------------------------------------------------ */

/*
 * Read this before believing a passing test.
 *
 * A backend that declines everything and falls back for every instruction
 * passes every suite while proving nothing about the translator, and this
 * has already happened once here: the first x86-64 run interpreted 92% of
 * the self-test and looked perfectly healthy. `interp` against the retired
 * count is the number that says whether translation is being exercised.
 */
typedef struct emu_jit_stats {
    uint32_t blocks;            /* blocks currently held               */
    uint32_t translations;      /* blocks translated since reset       */
    uint32_t block_entries;     /* times a translated block was run    */
    uint32_t interp_fallbacks;  /* instructions the interpreter ran    */
    uint32_t flushes;           /* whole-cache discards                */
    uint32_t compactions;       /* reclaims that kept the hot blocks   */
    uint32_t evictions;         /* blocks compaction discarded         */
    uint32_t code_used;
    uint32_t code_size;
    /*
     * Why a translation did not produce a block. These have to stay
     * apart: "the translator declined" is ordinary and must reclaim
     * nothing, while "the buffer overflowed" means retry after
     * compacting. Conflating them cost 65% of all host cycles on CoreMark
     * with every test still passing, which is why they are counted rather
     * than reasoned about.
     */
    uint32_t declined;
    uint32_t overflowed;
#ifdef EMU_JIT_DIFF
    /*
     * How much of the differential check actually ran. Without these,
     * "the compiled code agreed with the IR" and "the reference declined
     * every block" are the same silence -- and the reference declines any
     * block holding a store, which is most of an architecture test. A
     * checker that never fires reads as a clean bill of health, which is
     * the failure mode this whole file is annotated against.
     */
    uint32_t diff_checked;      /* blocks run against the reference    */
    uint32_t diff_declined;     /* blocks the reference would not run  */
#endif
#ifdef EMU_JIT_PROFILE
    /* Define EMU_JIT_PROFILE to split host cycles by phase. Needs a cycle
     * counter, so it is off by default and ARM-only for now. */
    uint32_t cyc_translate;
    uint32_t cyc_compact;
#endif
} emu_jit_stats_t;

/* ------------------------------------------------------------------ */
/* What a frontend supplies                                            */
/* ------------------------------------------------------------------ */

/*
 * The state the dispatch loop consults on every iteration, as pointers
 * rather than accessors.
 *
 * Callbacks read better and were how this started, but the loop runs once
 * per block entry -- 38,696 times in a two-iteration CoreMark, 2.9 million
 * in a full one -- and on a Cortex-M7 an indirect call it cannot predict
 * costs a pipeline refill. Six of them measured **+129 cycles per
 * dispatch**, which was 4.99M of the 7.03M host cycles the framework added
 * to the Thumb-2 backend. As loads they cost four instructions.
 *
 * This is not the layout guess emu_jit_ops_t's `pc` callback was written
 * to avoid: the frontend states where each value lives, once, in `bind`.
 * Nothing here infers an offset.
 */
typedef struct emu_jit_hot {
    const uint32_t *pc;
    const uint8_t  *state;

    /*
     * Everything the translator bakes into a block, as one value; see the
     * long note on staleness below. NULL means nothing is baked in.
     */
    const uint32_t *generation;

    /*
     * True while no translated block may run -- state the emitted code
     * cannot check for itself. NULL means never.
     */
    const bool *blocked;

    /*
     * Cleared by the frontend when it knows no interrupt can be pending,
     * so take_irq is called only when there is something to find. NULL
     * means ask every time.
     */
    const volatile bool *irq_pending;
} emu_jit_hot_t;

typedef struct emu_jit_ops {
    const char *name;

    /*
     * Point the framework at the hot state above. Called once per
     * emu_jit_run, not per dispatch, so it may be as slow as it likes.
     */
    void (*bind)(emu_cpu_t *cpu, emu_jit_hot_t *out);

    /*
     * Translate a basic block starting at `pc`, emitting through
     * emu_jit_emit*(). Returns the number of guest instructions the block
     * retires, or 0 if nothing at `pc` could be translated -- which is
     * ordinary and frequent, not an error: every interpreted instruction
     * lands here.
     *
     * The framework has already reserved space and will commit or discard
     * what is emitted; the translator only emits.
     */
    uint32_t (*translate)(emu_cpu_t *cpu, uint32_t pc);

    /*
     * A note on emu_jit_hot_t.generation, which is where this project has
     * found the most bugs: a rounding mode resolved at translation, an FPU
     * turned off after the block was built, a PMP entry locked afterwards,
     * a page table edited under a block keyed on virtual addresses. Point
     * it at something that changes whenever any of that does. A constant
     * is correct only for a translator that reads nothing.
     */

    /* Run one instruction when translation declined, and on the guard. */
    const emu_backend_t *interp;

    /*
     * Called after the interpreter has run one instruction on the
     * framework's behalf, or NULL.
     *
     * This is where a frontend can afford to re-derive what `generation`
     * reports. The distinction is a measured one: everything the RV32
     * translator specialises on -- frm, mstatus.FS, the PMP configuration
     * -- moves only through a CSR write, and the translator declines
     * SYSTEM, so every such write lands exactly here. Re-deriving it in
     * `generation` instead would put a sixteen-entry PMP walk on every
     * block entry, and CoreMark enters blocks 2.9 million times a run.
     *
     * `generation` should then be a cached value: one load and a compare.
     */
    void (*after_interp)(emu_cpu_t *cpu);

    /* True when the hart is parked and only an interrupt can restart it. */
    bool (*is_idle)(emu_cpu_t *cpu);
    bool (*wake)(emu_cpu_t *cpu);

    /* Deliver a pending interrupt between blocks, if any. Returns true if
     * one was taken, which costs a retired instruction. */
    bool (*take_irq)(emu_cpu_t *cpu);

    /* Add `n` to whatever counts retired instructions and cycles. */
    void (*count)(emu_cpu_t *cpu, uint32_t n);

    /*
     * Make freshly written code fetchable, or NULL on a host whose caches
     * are coherent with instruction fetch.
     *
     * x86 needs nothing here. A Cortex-M7 needs a clean to the point of
     * unification and an invalidate of the stale instruction lines, in
     * that order, and getting it wrong does not corrupt a result -- it
     * executes whatever those addresses held before.
     *
     * Called after every translation and after compaction moves blocks.
     */
    void (*sync)(const void *addr, uint32_t len);

    /*
     * Differential checking, for a host whose emitted code cannot be
     * tested any other way.
     *
     * Run the block that is about to execute on an independent
     * reference -- the IR interpreter -- and compare the guest state it
     * produces against the compiled code's. Both come from the *same*
     * IR, so a disagreement is a lowering bug and nothing else.
     *
     * Returns false to decline this block, which the framework takes as
     * "run it normally, do not compare". A block that writes guest
     * memory or calls a helper must decline: the reference and the
     * compiled code would each perform those effects, and a store
     * cannot be undone by restoring registers.
     *
     * state_bytes is how much of emu_cpu_t to snapshot and compare.
     */
    bool (*diff_ref)(emu_cpu_t *cpu);
    uint32_t state_bytes;

    /*
     * True if a translated block may be *moved*.
     *
     * Compaction slides survivors down to reclaim space, which is only
     * sound for code that does not depend on where it sits: every absolute
     * address materialised as an immediate, every branch within the block
     * relative. Both current backends satisfy that, but it is a property
     * of the emitted code rather than of the host, so the translator has
     * to assert it. False means the framework flushes instead, which is
     * correct but throws away the hot blocks with the cold.
     */
    bool relocatable;
} emu_jit_ops_t;

/* ------------------------------------------------------------------ */
/* The framework                                                       */
/* ------------------------------------------------------------------ */

/*
 * Bring up the code buffer. `bytes` is a request; a host that maps memory
 * may round it up. Safe to call repeatedly.
 *
 * A platform with its own buffer -- a microcontroller, where those bytes
 * are the guest's -- passes it in with emu_jit_set_buffer instead, before
 * the first init.
 */
bool emu_jit_init(uint32_t bytes);
void emu_jit_set_buffer(void *mem, uint32_t bytes);

/* Discard every translated block. */
void emu_jit_flush(void);

void emu_jit_get_stats(emu_jit_stats_t *out);

/* --- emitting, for use inside ops->translate ---------------------- */

/*
 * The emit cursor, exposed so these can be inlined.
 *
 * Encapsulating it behind calls is tidier and measurably worse: emitting
 * is the inner loop of translation, the Thumb-2 translator emits a
 * halfword per instruction and nothing else, and translation is 54% of
 * all host cycles on CoreMark at the 12 KB code cache a microcontroller
 * gets. A call per halfword showed up as a 7.5% translation cost.
 *
 * Written only by this header's inlines and by emu_jit.c.
 */
extern uint8_t *emu_jit_cursor;
extern uint8_t *emu_jit_limit;
extern bool     emu_jit_overflow;

static inline bool emu_jit_room(uint32_t n)
{
    if ((uint32_t)(emu_jit_limit - emu_jit_cursor) < n) {
        emu_jit_overflow = true;
        return false;
    }
    return true;
}

static inline void emu_jit_emit8(uint8_t b)
{
    if (emu_jit_room(1u)) {
        *emu_jit_cursor++ = b;
    }
}

static inline void emu_jit_emit16(uint16_t h)
{
    if (emu_jit_room(2u)) {
        __builtin_memcpy(emu_jit_cursor, &h, 2u);
        emu_jit_cursor += 2u;
    }
}

static inline void emu_jit_emit32(uint32_t w)
{
    if (emu_jit_room(4u)) {
        __builtin_memcpy(emu_jit_cursor, &w, 4u);
        emu_jit_cursor += 4u;
    }
}

static inline void emu_jit_emit64(uint64_t d)
{
    if (emu_jit_room(8u)) {
        __builtin_memcpy(emu_jit_cursor, &d, 8u);
        emu_jit_cursor += 8u;
    }
}

/* Where the next byte will land, for computing a branch displacement. */
static inline uint8_t *emu_jit_here(void) { return emu_jit_cursor; }

/*
 * Abandon everything emitted since `to`, which must have come from
 * emu_jit_here().
 *
 * Every translator needs this, because deciding an instruction cannot be
 * translated is not always possible before emitting part of it: a
 * three-operand form may load its operands and only then reach a funct7 it
 * does not know. Without a rewind that half-instruction stays in the
 * block and runs, which does not fault -- it quietly does something else.
 */
void emu_jit_rewind(uint8_t *to);

/*
 * Point the emitters at a region and start a fresh block.
 *
 * The dispatch loop does this itself before calling ops->translate; it is
 * public so that a backend's lowering can be driven -- and *executed* --
 * by a test without a frontend, a guest or a cache behind it. Emitted
 * code that no test has ever run is the failure mode this exists to
 * avoid.
 */
void emu_jit_emit_begin(void *buf, uint32_t bytes);

/* True once the buffer overflowed; the block will be discarded. */
static inline bool emu_jit_overflowed(void) { return emu_jit_overflow; }

/* --- the dispatch loop -------------------------------------------- */

emu_run_reason_t emu_jit_run(emu_cpu_t *cpu, uint32_t budget,
                             uint32_t *retired, const emu_jit_ops_t *ops);

#ifdef __cplusplus
}
#endif

#endif /* EMU_JIT_H */
