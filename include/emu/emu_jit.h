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
    uint32_t code_used;
    uint32_t code_size;
} emu_jit_stats_t;

/* ------------------------------------------------------------------ */
/* What a frontend supplies                                            */
/* ------------------------------------------------------------------ */

typedef struct emu_jit_ops {
    const char *name;

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
     * May a translated block run at all right now?
     *
     * False sends everything to the interpreter for as long as it stays
     * false. This is for the state a translator cannot express rather than
     * for state it bakes in: RV32 answers false whenever PMP, Sdtrig or
     * address translation is armed, because each of those needs a check
     * per fetch that emitted code does not perform.
     *
     * NULL means always.
     */
    bool (*may_run)(emu_cpu_t *cpu);

    /*
     * Everything the translator bakes into a block, as one value.
     *
     * The framework compares it on every dispatch and flushes when it
     * moves. This exists because the alternative -- each backend deciding
     * for itself when its blocks went stale -- is where this project has
     * found the most bugs: a rounding mode resolved at translation, an FPU
     * turned off after the block was built, a PMP entry locked afterwards,
     * a page table edited under a block keyed on virtual addresses. Return
     * something that changes whenever any of that does; returning a
     * constant is correct only for a translator that reads nothing.
     *
     * NULL means nothing is baked in.
     */
    uint32_t (*generation)(emu_cpu_t *cpu);

    /* Run one instruction when translation declined, and on the guard. */
    const emu_backend_t *interp;

    /* True when the hart is parked and only an interrupt can restart it. */
    bool (*is_idle)(emu_cpu_t *cpu);
    bool (*wake)(emu_cpu_t *cpu);

    /* Deliver a pending interrupt between blocks, if any. Returns true if
     * one was taken, which costs a retired instruction. */
    bool (*take_irq)(emu_cpu_t *cpu);

    /* Add `n` to whatever counts retired instructions and cycles. */
    void (*count)(emu_cpu_t *cpu, uint32_t n);

    /* Execution state, so the loop can stop. */
    uint8_t (*state)(emu_cpu_t *cpu);

    /*
     * Where the guest is. Asked for rather than read out of the struct:
     * both frontends happen to put pc straight after the register file,
     * and a framework that relied on that would break silently the day one
     * of them reordered a field.
     */
    uint32_t (*pc)(emu_cpu_t *cpu);
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

void emu_jit_emit8(uint8_t b);
void emu_jit_emit16(uint16_t h);
void emu_jit_emit32(uint32_t w);
void emu_jit_emit64(uint64_t d);

/* Where the next byte will land, for computing a branch displacement. */
uint8_t *emu_jit_here(void);

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

/* True once the buffer overflowed; the block will be discarded. */
bool emu_jit_overflowed(void);

/* --- the dispatch loop -------------------------------------------- */

emu_run_reason_t emu_jit_run(emu_cpu_t *cpu, uint32_t budget,
                             uint32_t *retired, const emu_jit_ops_t *ops);

#ifdef __cplusplus
}
#endif

#endif /* EMU_JIT_H */
