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
#include "rv_backend.h"

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
    uint32_t blocks;         /* blocks currently translated       */
    uint32_t code_used;      /* bytes of code cache in use        */
    uint32_t code_size;      /* bytes available                   */
    uint32_t flushes;        /* times the cache filled and reset  */
    uint32_t translations;   /* blocks translated since reset     */
    uint32_t interp_fallbacks; /* instructions run by the interpreter */
} rv_jit_stats_t;

void rv_jit_get_stats(rv_jit_stats_t *out);

extern const rv_backend_t rv_backend_jit;

#endif /* RV_ENABLE_JIT */

#ifdef __cplusplus
}
#endif

#endif /* RV32_RV_JIT_H */
