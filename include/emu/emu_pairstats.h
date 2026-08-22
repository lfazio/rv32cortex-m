/* SPDX-License-Identifier: Apache-2.0 */
/*
 * emu_pairstats.h - adjacent-instruction-pair histogram, for any frontend.
 *
 * Measurement scaffolding for the question every fusion or peephole idea
 * has to answer first: which pairs actually *execute*, as opposed to
 * which ones look fusible in a listing. This tree has twice reached the
 * opposite conclusion from reading code -- the textbook RISC-V fusions
 * are 0.2% and 0.00% of CoreMark's pairs, and an ARM shifted-operand
 * fusion was built, measured, found never to fire, and reverted. Run this
 * before writing an encoder, not after.
 *
 * **Nothing here is RISC-V**, which is the change from what it replaced.
 * The histogram, the keying, the dependence analysis and the report are
 * the same for any instruction set; what differs is how to get an
 * operation's identity and its registers out of an encoding, and that is
 * the seven functions below. A frontend supplies them and gets the whole
 * thing -- which is the point, because the two frontends *without* a
 * reference model are the ones where "what does this guest actually
 * execute" is hardest to answer any other way.
 *
 * Counted on the interpreter, because a pair is only a pair when the two
 * instructions are consecutive in the executed stream *and* contiguous in
 * memory -- a branch between them means a translator would never see them
 * as a pair either.
 *
 * **Measured here, acted on in the IR.** The two halves belong at
 * different levels and it is worth being explicit about which:
 *
 *   - the *measurement* is a frontend question. It is about what a guest
 *     executes, it has to be dynamic to mean anything, and the answer
 *     differs per instruction set. Hence the interpreter and this table.
 *
 *   - the *optimisation* is not. A frontend decodes and emits IR; passes
 *     over that IR fuse and eliminate; backends lower what survives. So a
 *     pair worth acting on becomes a pass in emu_ir_optimise -- where it
 *     is written once and every host gets it -- and not a peephole in an
 *     emitter, where it would be written per host and match on encodings
 *     the IR has already abstracted away.
 *
 * That is not a preference. The ARM shifted-operand fusion was built as a
 * backend peephole, was correct on hardware, never fired, and cost 10% --
 * while the thing this histogram actually points at, the 29.6% of pairs
 * whose second instruction consumes the first's result, is answered by
 * pass_reg_traffic in the IR and is answered for x86-64 and Thumb-2 at
 * the same time.
 */
#ifndef EMU_PAIRSTATS_H
#define EMU_PAIRSTATS_H

#include "emu_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* An operand slot the instruction does not read. Zero cannot serve: on
 * RISC-V x0 is a real encoding, and on an ISA with no zero register it
 * would be an ordinary operand. */
#define EMU_PAIR_NO_REG 0xFFFFFFFFu

typedef struct emu_pair_ops {
    /*
     * A compact identity for the *operation*, with the registers
     * discarded -- what a fusion rule matches on. Two encodings that a
     * rule would treat alike must map to the same kind, and two it would
     * not must not: an `add` and a `sub` differing only in one funct7 bit
     * are different kinds, while `add x1,x2,x3` and `add x4,x5,x6` are
     * one.
     */
    uint32_t (*kind)(uint64_t insn);

    /*
     * The destination register, or 0 for an instruction that writes
     * none. Zero is right here rather than EMU_PAIR_NO_REG because a
     * write to a hardwired-zero register is also "writes nothing", and
     * the two cases want the same answer.
     */
    uint32_t (*rd)(uint64_t insn);

    /* Source registers the instruction actually reads, or
     * EMU_PAIR_NO_REG. "Actually" matters: LUI has an rs1 *field* and
     * does not read it, and counting that as a dependence would report
     * pairs that no translator could fuse. */
    uint32_t (*rs1)(uint64_t insn);
    uint32_t (*rs2)(uint64_t insn);

    /* The kind's mnemonic, for the report. */
    void (*kind_name)(uint32_t kind, char *buf, unsigned n);

    /*
     * Classification, for the one aggregate that needs it:
     * address-generation feeding a memory access is the pair a
     * scaled-index addressing mode would serve, and naming it needs to
     * know which kinds compute and which access.
     */
    bool (*kind_is_mem)(uint32_t kind);
    bool (*kind_is_alu)(uint32_t kind);
} emu_pair_ops_t;

#if EMU_PAIR_STATS

/* Record one executed instruction. */
void emu_pair_note(const emu_pair_ops_t *ops, uint32_t pc, uint64_t insn,
                   unsigned len);

/* Print the histogram, most frequent first, to stderr. */
void emu_pair_report(unsigned top_n);

#endif

#ifdef __cplusplus
}
#endif

#endif /* EMU_PAIRSTATS_H */
