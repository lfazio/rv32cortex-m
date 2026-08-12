/* SPDX-License-Identifier: Apache-2.0 */
/*
 * emu_thumb2.h - Thumb-2 instruction encoding, for any frontend.
 *
 * The counterpart of emu_x86_64.h: how to spell an instruction on an
 * ARMv7E-M host. Nothing here knows what is being translated.
 *
 * Where a 16-bit form exists it is deliberately *not* used. The register
 * numbers a lowering hands these are frequently above r7, and this
 * project has already been bitten by a 16-bit data-processing encoding
 * silently assembling as a different instruction when given a high
 * register -- nothing faulted, and a loop cap simply stopped applying.
 *
 * Encodings are from ARM DDI 0403E (docs/arm/).
 */
#ifndef EMU_THUMB2_H
#define EMU_THUMB2_H

#include "emu_jit.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Registers the block frame reserves; see src/backend/thumb2/ir_lower.c. */
#define T2_R0    0u
#define T2_R1    1u
#define T2_R2    2u
#define T2_R3    3u
#define T2_CPU   4u
#define T2_CNT   5u
#define T2_SP   13u
#define T2_R12  12u

/* Shift types, for the register and immediate forms below. */
#define T2_LSL 0u
#define T2_LSR 1u
#define T2_ASR 2u
#define T2_ROR 3u

void t2_emit16(uint16_t h);
void t2_emit32(uint16_t hw1, uint16_t hw2);

void t2_movw(uint32_t rd, uint16_t imm);
void t2_movt(uint32_t rd, uint16_t imm);
void t2_imm32(uint32_t rd, uint32_t v);
void t2_mov(uint32_t rd, uint32_t rm);

void t2_ldr_imm(uint32_t rt, uint32_t rn, uint32_t off);
void t2_str_imm(uint32_t rt, uint32_t rn, uint32_t off);

void t2_add(uint32_t rd, uint32_t rn, uint32_t rm);
void t2_sub(uint32_t rd, uint32_t rn, uint32_t rm);
void t2_and(uint32_t rd, uint32_t rn, uint32_t rm);
void t2_orr(uint32_t rd, uint32_t rn, uint32_t rm);
void t2_eor(uint32_t rd, uint32_t rn, uint32_t rm);
void t2_cmp(uint32_t rn, uint32_t rm);
void t2_mvn(uint32_t rd, uint32_t rm);
void t2_neg(uint32_t rd, uint32_t rn);

void t2_shift_reg(uint32_t kind, uint32_t rd, uint32_t rn, uint32_t rm);
void t2_shift_imm(uint32_t type, uint32_t rd, uint32_t rm, uint32_t amount);

void t2_mul(uint32_t rd, uint32_t rn, uint32_t rm);
void t2_mull(bool sign, uint32_t rdlo, uint32_t rdhi, uint32_t rn,
             uint32_t rm);

void t2_rev(uint32_t rd, uint32_t rm);
void t2_rev16(uint32_t rd, uint32_t rm);
void t2_rbit(uint32_t rd, uint32_t rm);
void t2_clz(uint32_t rd, uint32_t rm);

void t2_sxtb(uint32_t rd, uint32_t rm);
void t2_sxth(uint32_t rd, uint32_t rm);
void t2_uxtb(uint32_t rd, uint32_t rm);
void t2_uxth(uint32_t rd, uint32_t rm);

/*
 * Forward branches, with the displacement filled in later. Pair each
 * with t2_patch_branch once the target is known.
 */
uint8_t *t2_b_forward(void);
uint8_t *t2_bcond_forward(uint32_t cond);
void     t2_patch_branch(uint8_t *at, const uint8_t *target, bool conditional);

/*
 * Call an absolute address. BLX takes its target in a register, so the
 * address is materialised into r12 -- the one scratch register AAPCS
 * lets a callee clobber and that is not an argument.
 */
void t2_call(const void *fn);

/*
 * VFP, single precision. S0-S2 only: a floating-point value lives in an
 * integer temp as its bit pattern and enters the FP unit solely to be
 * operated on, which is what keeps the register allocator out of it.
 */
#define T2_S0 0u
#define T2_S1 1u
#define T2_S2 2u

/* First-halfword bases for the three-register group. */
#define T2_VADD 0xEE30u        /* with `sub` false */
#define T2_VSUB 0xEE30u        /* with `sub` true  */
#define T2_VMUL 0xEE20u
#define T2_VDIV 0xEE80u

void t2_vmov_core(uint32_t sn, uint32_t rt, bool to_core);
void t2_vfp3(uint16_t hi, bool sub, uint32_t sd, uint32_t sn, uint32_t sm);
void t2_vsqrt(uint32_t sd, uint32_t sm);
void t2_vmrs(uint32_t rt);
void t2_vmsr(uint32_t rt);

/* An IR condition as the cond field of a Thumb-2 conditional branch. */
uint32_t t2_cond(uint8_t c);

/*
 * The registers the allocator may use, in the order it takes them: all
 * callee-saved under AAPCS, so a value in one survives a helper call
 * with nothing emitted around it.
 */
#define T2_ALLOC_REGS 6u
extern const uint32_t t2_alloc_regs[T2_ALLOC_REGS];

/*
 * PUSH and POP of a register list. LR is bit 14 and PC bit 15; the
 * 16-bit encoding is taken whenever the list fits it.
 */
#define T2_LIST_LR (1u << 14)
#define T2_LIST_PC (1u << 15)
void t2_push(uint32_t list);
void t2_pop(uint32_t list);

#ifdef __cplusplus
}
#endif

#endif /* EMU_THUMB2_H */
