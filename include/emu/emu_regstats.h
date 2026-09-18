/* SPDX-License-Identifier: Apache-2.0 */
/*
 * emu_regstats.h - guest-register traffic in translated blocks.
 *
 * **The question this exists to answer, and the trap in answering it.**
 * After inlining memory accesses and linking exits, what is left in a
 * translated block is the register file: every surviving GET is a load
 * from the cpu struct and every PUT a store, because the allocator
 * covers temps rather than guest registers. Pinning hot guest registers
 * to host registers is the obvious next lever.
 *
 * It has been tried. A three-register cache on Thumb-2 measured **15.5%
 * slower**, and the reason is a cost model rather than a frequency
 * count: a cached read is a MOV where an uncached one is an LDR -- one
 * instruction either way -- while write-through adds an instruction per
 * write, and the extra callee-saved registers are paid at every PUSH and
 * POP whether the block uses them or not. Reads per block said it should
 * win; it did not.
 *
 * So this deliberately does not report "which register is hottest". It
 * reports what a cost model needs:
 *
 *   - how many times one guest register is referenced *within a single
 *     block*, after the reload elision has already removed the easy
 *     ones. Pinning saves that many loads and stores and costs an entry
 *     load and an exit store, so a register referenced once or twice
 *     cannot pay;
 *   - the split between reads and writes, because they cost differently:
 *     a pinned read is free and a pinned write is not;
 *   - how many distinct registers a block touches at all, which is what
 *     sets the save/restore cost a pinned set imposes on *every* block,
 *     including the ones that never use it.
 *
 * **It is weighted per translation, not per block entry, and that is a
 * real limitation.** A block translated once and entered a million times
 * counts the same here as one entered twice. The counts therefore cannot
 * be read as "what the guest spends its time doing" -- only as "what a
 * block looks like". Where the two would disagree is loops, which are
 * exactly the blocks that matter, so a positive result here would need
 * confirming dynamically before anything is built on it. A negative one
 * does not: the benefit and the cost are both per entry and both scale
 * with it, so a register that cannot pay in a block cannot pay in a hot
 * block either.
 */

#ifndef EMU_REGSTATS_H
#define EMU_REGSTATS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct emu_ir_block;
struct emu_ir_target;

#if EMU_JIT_HOT_REG_STATS

/*
 * Record one block, after emu_ir_optimise and before lowering.
 *
 * After, because the reload elision is what decides whether a GET is
 * real: counting before it would measure the frontend's emission habits
 * rather than the traffic a backend has to emit.
 *
 * `t` is the guest description, and it is needed for one thing: which
 * registers are hardwired to zero. **The first version of this did not
 * take it, and reported RV32's x0 as the busiest register in every
 * guest** -- 19-24% of all references. A GET of x0 survives the passes
 * on purpose and every backend answers it with a constant at lowering,
 * so it is not register-file traffic at all; counting it put a quarter
 * of the histogram into a bucket that cannot be optimised, and would
 * have made a pinned x0 look like the obvious first choice.
 */
void emu_reg_note_block(const struct emu_ir_block *b,
                        const struct emu_ir_target *t);

/* Print what was collected, to the console. */
void emu_reg_report(void);

#endif /* EMU_JIT_HOT_REG_STATS */

#ifdef __cplusplus
}
#endif

#endif /* EMU_REGSTATS_H */
