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

#if EMU_HAVE_JIT

/* Bytes of RAM for translated code. */
#ifndef RV_JIT_CODE_SIZE
#define RV_JIT_CODE_SIZE (12u * 1024u)
#endif

/* Maximum number of translated blocks tracked at once. */
#ifndef RV_JIT_MAX_BLOCKS
#define RV_JIT_MAX_BLOCKS 256u
#endif

/* Power-of-two hash table size for guest pc -> block lookup. */
#ifndef RV_JIT_HASH_SIZE
#define RV_JIT_HASH_SIZE 256u
#endif

/* Most guest instructions translated into a single block. */
#ifndef RV_JIT_MAX_BLOCK_INSNS
#define RV_JIT_MAX_BLOCK_INSNS 64u
#endif

/*
 * The code cache must live in memory the core can execute. On Cortex-M
 * that is ordinary SRAM; a platform that places it elsewhere passes the
 * buffer in. Must be 4-byte aligned.
 */
void rv_jit_set_code_buffer(void *buf, uint32_t size);

/* Discard every translation. Cheap; called on reset and on invalidate. */
void rv_jit_flush(void);

/*
 * The RV32 JIT's own statistics used to live here, duplicating the
 * framework's and adding nine counters of its own -- helper calls by
 * class, elided loads and stores, passthrough arming, and reads per block
 * of the four registers a per-block cache would hold.
 *
 * **Every one of those nine had zero writers.** They belonged to the
 * hand-written Thumb-2 backend and were left behind when the shared IR
 * framework replaced it: rv_jit_get_stats memset the struct and filled
 * only the fields emu_jit_get_stats already reports, so the firmware
 * printed `helpers muldiv 0 clmul 0 bit 0`, `pt hits 0 armed 0` and
 * `reads/blk sp=- in 0` on every run, for ever. Nine numbers that could
 * not be anything but zero, in a report whose entire purpose is to say
 * whether translation happened -- which is this tree's own rule about
 * checking that an instrument can represent the difference, printed
 * every run and read by nobody.
 *
 * emu_jit_stats_t is the whole of it now. A backend that grows counters
 * of its own should add them there if the dispatch loop can maintain
 * them, and otherwise earn a hook by having something to put in it.
 */

extern const emu_backend_t rv_backend_jit;

#endif /* EMU_HAVE_JIT */

#ifdef __cplusplus
}
#endif

#endif /* RV32_RV_JIT_H */
