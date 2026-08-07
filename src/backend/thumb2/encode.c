/* SPDX-License-Identifier: Apache-2.0 */
/*
 * src/backend/thumb2/encode.c - Thumb-2 encoding. See emu_thumb2.h.
 */

#include "emu/emu_thumb2.h"
#include "emu/emu_ir.h"

#if defined(EMU_JIT_THUMB2)

void t2_emit16(uint16_t h) { emu_jit_emit16(h); }

/* A 32-bit Thumb-2 instruction is two halfwords, high first. */
void t2_emit32(uint16_t hw1, uint16_t hw2)
{
    t2_emit16(hw1);
    t2_emit16(hw2);
}

/* ------------------------------------------------------------------ */
/* Moves and immediates                                                */
/* ------------------------------------------------------------------ */

/* MOVW/MOVT: any 32-bit constant in two instructions, no literal pool. */
void t2_movw(uint32_t rd, uint16_t imm)
{
    const uint32_t i = (imm >> 11) & 1u;
    const uint32_t imm4 = (imm >> 12) & 0xFu;
    const uint32_t imm3 = (imm >> 8) & 7u;
    const uint32_t imm8 = imm & 0xFFu;

    t2_emit32((uint16_t)(0xF240u | (i << 10) | imm4),
           (uint16_t)((imm3 << 12) | (rd << 8) | imm8));
}

void t2_movt(uint32_t rd, uint16_t imm)
{
    const uint32_t i = (imm >> 11) & 1u;
    const uint32_t imm4 = (imm >> 12) & 0xFu;
    const uint32_t imm3 = (imm >> 8) & 7u;
    const uint32_t imm8 = imm & 0xFFu;

    t2_emit32((uint16_t)(0xF2C0u | (i << 10) | imm4),
           (uint16_t)((imm3 << 12) | (rd << 8) | imm8));
}

void t2_imm32(uint32_t rd, uint32_t v)
{
    t2_movw(rd, (uint16_t)v);
    if ((v >> 16) != 0u) {
        t2_movt(rd, (uint16_t)(v >> 16));
    }
}

/* MOV.W rd, rm -- the 32-bit form, so any register pair is encodable. */
void t2_mov(uint32_t rd, uint32_t rm)
{
    t2_emit32(0xEA4Fu, (uint16_t)((rd << 8) | rm));
}

/* ------------------------------------------------------------------ */
/* Memory: the frame and the cpu struct                                */
/* ------------------------------------------------------------------ */

/*
 * LDR.W / STR.W rt, [rn, #imm12]. The unsigned-offset form reaches 4 KB,
 * which covers both a temp frame and every offset inside the guest state
 * these frontends have.
 */
void t2_ldr_imm(uint32_t rt, uint32_t rn, uint32_t off)
{
    t2_emit32((uint16_t)(0xF8D0u | rn), (uint16_t)((rt << 12) | (off & 0xFFFu)));
}

void t2_str_imm(uint32_t rt, uint32_t rn, uint32_t off)
{
    t2_emit32((uint16_t)(0xF8C0u | rn), (uint16_t)((rt << 12) | (off & 0xFFFu)));
}

/* ------------------------------------------------------------------ */
/* Data processing                                                     */
/* ------------------------------------------------------------------ */

/* The 32-bit register data-processing forms: rd = rn <op> rm. */
void t2_dp3(uint16_t hw1_base, uint32_t rd, uint32_t rn, uint32_t rm)
{
    t2_emit32((uint16_t)(hw1_base | rn), (uint16_t)((rd << 8) | rm));
}

void t2_add(uint32_t rd, uint32_t rn, uint32_t rm)
{
    t2_dp3(0xEB00u, rd, rn, rm);
}
void t2_sub(uint32_t rd, uint32_t rn, uint32_t rm)
{
    t2_dp3(0xEBA0u, rd, rn, rm);
}
void t2_and(uint32_t rd, uint32_t rn, uint32_t rm)
{
    t2_dp3(0xEA00u, rd, rn, rm);
}
void t2_orr(uint32_t rd, uint32_t rn, uint32_t rm)
{
    t2_dp3(0xEA40u, rd, rn, rm);
}
void t2_eor(uint32_t rd, uint32_t rn, uint32_t rm)
{
    t2_dp3(0xEA80u, rd, rn, rm);
}

/* CMP.W rn, rm -- sets the flags and discards the result. */
void t2_cmp(uint32_t rn, uint32_t rm)
{
    t2_emit32((uint16_t)(0xEBB0u | rn), (uint16_t)(0x0F00u | rm));
}


/* MVN.W rd, rm and RSB.W rd, rn, #0 (negate). */
void t2_mvn(uint32_t rd, uint32_t rm)
{
    t2_emit32(0xEA6Fu, (uint16_t)((rd << 8) | rm));
}

void t2_neg(uint32_t rd, uint32_t rn)
{
    t2_emit32((uint16_t)(0xF1C0u | rn), (uint16_t)(rd << 8));
}

/* Register-controlled shifts: LSL/LSR/ASR/ROR .W rd, rn, rm. */
void t2_shift_reg(uint32_t kind, uint32_t rd, uint32_t rn,
                           uint32_t rm)
{
    t2_emit32((uint16_t)(0xFA00u | (kind << 5) | rn),
           (uint16_t)(0xF000u | (rd << 8) | rm));
}
#define T2_LSL 0u
#define T2_LSR 1u
#define T2_ASR 2u
#define T2_ROR 3u

/* Immediate shifts, as the MOV.W shifted-register form. */
void t2_shift_imm(uint32_t type, uint32_t rd, uint32_t rm,
                           uint32_t amount)
{
    const uint32_t n = amount & 31u;

    t2_emit32(0xEA4Fu,
           (uint16_t)(((n & 7u) << 6) | ((n >> 3) << 12) | (type << 4) |
                      (rd << 8) | rm));
}
#define T2_LSL 0u
#define T2_LSR 1u
#define T2_ASR 2u
#define T2_ROR 3u

/* REV, REV16 and CLZ, which is why the bit group is in the IR at all. */
void t2_rev(uint32_t rd, uint32_t rm)
{
    t2_emit32((uint16_t)(0xFA90u | rm), (uint16_t)(0xF080u | (rd << 8) | rm));
}

void t2_rev16(uint32_t rd, uint32_t rm)
{
    t2_emit32((uint16_t)(0xFA90u | rm), (uint16_t)(0xF090u | (rd << 8) | rm));
}

void t2_rbit(uint32_t rd, uint32_t rm)
{
    t2_emit32((uint16_t)(0xFA90u | rm), (uint16_t)(0xF0A0u | (rd << 8) | rm));
}

void t2_clz(uint32_t rd, uint32_t rm)
{
    t2_emit32((uint16_t)(0xFAB0u | rm), (uint16_t)(0xF080u | (rd << 8) | rm));
}

/* Sign and zero extension. */
void t2_sxtb(uint32_t rd, uint32_t rm)
{
    t2_emit32(0xFA4Fu, (uint16_t)(0xF080u | (rd << 8) | rm));
}
void t2_sxth(uint32_t rd, uint32_t rm)
{
    t2_emit32(0xFA0Fu, (uint16_t)(0xF080u | (rd << 8) | rm));
}
void t2_uxtb(uint32_t rd, uint32_t rm)
{
    t2_emit32(0xFA5Fu, (uint16_t)(0xF080u | (rd << 8) | rm));
}
void t2_uxth(uint32_t rd, uint32_t rm)
{
    t2_emit32(0xFA1Fu, (uint16_t)(0xF080u | (rd << 8) | rm));
}

/* ------------------------------------------------------------------ */
/* Control flow                                                        */
/* ------------------------------------------------------------------ */

/* Condition codes, as the cond field of a Thumb-2 conditional branch. */
uint32_t t2_cond(uint8_t c)
{
    switch ((emu_ir_cond_t)c) {
    case EMU_IR_C_EQ:  return 0x0u;
    case EMU_IR_C_NE:  return 0x1u;
    case EMU_IR_C_LTU: return 0x3u;   /* CC/LO */
    case EMU_IR_C_GEU: return 0x2u;   /* CS/HS */
    case EMU_IR_C_LT:  return 0xBu;
    case EMU_IR_C_GE:  return 0xAu;
    case EMU_IR_C_LE:  return 0xDu;
    case EMU_IR_C_GT:  return 0xCu;
    case EMU_IR_C_LEU: return 0x9u;   /* LS */
    case EMU_IR_C_GTU: return 0x8u;   /* HI */
    default:           return 0xEu;   /* AL */
    }
}

/*
 * Call an absolute address. BLX takes the target in a register, so the
 * address is materialised into r12 -- the one scratch register AAPCS
 * lets a callee clobber and that is not an argument.
 */
void t2_call(const void *fn)
{
    t2_imm32(T2_R12, (uint32_t)(uintptr_t)fn);
    t2_emit16((uint16_t)(0x4780u | (T2_R12 << 3)));      /* BLX r12 */
}


#endif /* EMU_JIT_THUMB2 */
