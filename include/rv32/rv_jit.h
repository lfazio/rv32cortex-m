/* SPDX-License-Identifier: Apache-2.0 */
/*
 * rv_jit.h - Thumb-2 just-in-time backend.
 *
 * Translates RV32 basic blocks into Thumb-2 machine code held in a RAM
 * code cache, eliminating the per-instruction costs the interpreter
 * cannot avoid: the bus call to fetch, RVC expansion, the dispatch switch,
 * the pc write and the counter update.
 *
 * Design choices, and why:
 *
 *   Register file stays in memory. Guest x1..x31 live in the hart struct
 *   and every operation loads and stores them. That sounds wasteful, but
 *   hart->x is at offset 0 so each access is a single 16-bit
 *   LDR/STR Rt,[r4,#n] -- and it means guest state is coherent at every
 *   instruction boundary, so a trap, an interrupt or a debugger read needs
 *   no unwinding. Register allocation across a block would be the next
 *   optimisation, not a prerequisite.
 *
 *   Blocks end at every control transfer. No block chaining or inline
 *   caching: each block writes h->pc and returns to the dispatcher. This
 *   keeps interrupt latency bounded by one block rather than by a chain.
 *
 *   Anything not translated ends the block early and is executed by the
 *   interpreter. The JIT is a fast path over the interpreter, not a
 *   replacement, so correctness never depends on covering every encoding.
 *
 * ARM register usage inside a translated block:
 *
 *   r4        hart pointer (callee-saved, so it survives helper calls)
 *   r0-r3     scratch, and the argument registers for helper calls
 *   lr        pushed in the prologue, popped into pc at the exit
 */
#ifndef RV32_RV_JIT_H
#define RV32_RV_JIT_H

#include "rv_types.h"
#include "rv_config.h"
#include "emu/emu_backend.h"

#ifdef __cplusplus
extern "C" {
#endif

#if RV_ENABLE_JIT

/* Bytes of RAM for translated code. */
#ifndef RV_JIT_CODE_SIZE
#  define RV_JIT_CODE_SIZE (12u * 1024u)
#endif

/* Maximum number of translated blocks tracked at once. */
#ifndef RV_JIT_MAX_BLOCKS
#  define RV_JIT_MAX_BLOCKS 256u
#endif

/* Power-of-two hash table size for guest pc -> block lookup. */
#ifndef RV_JIT_HASH_SIZE
#  define RV_JIT_HASH_SIZE 256u
#endif

/* Most guest instructions translated into a single block. */
#ifndef RV_JIT_MAX_BLOCK_INSNS
#  define RV_JIT_MAX_BLOCK_INSNS 64u
#endif

/*
 * The code cache must live in memory the core can execute. On Cortex-M
 * that is ordinary SRAM; a platform that places it elsewhere passes the
 * buffer in. Must be 4-byte aligned.
 */
void rv_jit_set_code_buffer(void *buf, uint32_t size);

/* Discard every translation. Cheap; called on reset and on invalidate. */
void rv_jit_flush(void);

/* Statistics, for reporting how well translation is going. */
typedef struct rv_jit_stats {
    uint32_t blocks;         /* blocks currently translated         */
    uint32_t code_used;      /* bytes of code cache in use          */
    uint32_t code_size;      /* bytes available                     */
    uint32_t flushes;        /* whole-cache resets                  */
    uint32_t compactions;    /* reclaims that kept the hot blocks   */
    uint32_t evictions;      /* blocks discarded by compaction      */
    uint32_t translations;   /* blocks translated since reset       */
    uint32_t interp_fallbacks; /* instructions run by the interpreter */
    /* Helper calls emitted for operations with no short Thumb-2 form,
     * split so a hot one can be identified rather than guessed at. */
    uint32_t alu_calls_muldiv;
    uint32_t alu_calls_clmul;
    uint32_t alu_calls_bit;
    /* Block entries. Divided into instructions retired this gives the
     * average block length, which is what per-block overhead is paid on. */
    uint32_t block_entries;
    /*
     * Guest-register loads not emitted because the value was already in
     * R1 from the previous instruction's store, and stores not emitted
     * because the next instruction overwrote the register without being
     * able to trap first. Two host instructions and four bytes of code
     * cache between them, per pair. See RV32_PAIR_STATS.
     */
    uint32_t ld_elided;
    uint32_t st_elided;
    uint32_t pt_hits;        /* passthrough accesses via the helper  */
    uint32_t pt_armed;       /* inlined peripheral window emitted?   */
    /*
     * Reads of the registers a per-block cache would hold, in translation
     * order: sp(x2), ra(x1), a0(x10), a1(x11). `hot_reads` totals the reads
     * and `hot_blocks` counts the blocks that read each at least once, so
     * hot_reads/hot_blocks is the average reads per block that uses it --
     * which is the number that decides whether caching it can pay for the
     * load that sets it up.
     */
    uint32_t hot_reads[4];
    uint32_t hot_blocks[4];
    /* See emu_jit_stats_t: declined and overflowed must stay apart. */
    uint32_t declined;
    uint32_t overflowed;
#ifdef EMU_JIT_PROFILE
    uint32_t cyc_translate;
    uint32_t cyc_compact;
#endif
} rv_jit_stats_t;

void rv_jit_get_stats(rv_jit_stats_t *out);

extern const emu_backend_t rv_backend_jit;

#endif /* RV_ENABLE_JIT */

#ifdef __cplusplus
}
#endif

#endif /* RV32_RV_JIT_H */
