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

    /*
     * The amount is split imm3:imm2 -- imm3 is the *high* three bits at
     * 14:12 and imm2 the low two at 7:6. Splitting it the other way
     * round encodes a different amount entirely and never faults: a
     * shift by 31 came out as a shift by 0, and only the guest's own
     * slli/srli/srai self-tests noticed.
     */
    t2_emit32(0xEA4Fu,
              (uint16_t)(((n >> 2) << 12) | (rd << 8) | ((n & 3u) << 6) |
                         (type << 4) | rm));
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


/*
 * Forward branches, with the displacement filled in by t2_patch_branch
 * once the target is known.
 */
uint8_t *t2_b_forward(void)
{
    uint8_t *const at = emu_jit_here();

    t2_emit32(0xF000u, 0xB800u);            /* B.W  <label>  (T4) */
    return at;
}

uint8_t *t2_bcond_forward(uint32_t cond)
{
    uint8_t *const at = emu_jit_here();

    t2_emit32((uint16_t)(0xF000u | (cond << 6)), 0x8000u);  /* B<c>.W (T3) */
    return at;
}

/*
 * Guarded on overflow because `at` may be past the end of a buffer that
 * stopped accepting emissions -- the block is discarded either way, and
 * writing through the pointer would corrupt whatever follows.
 */
void t2_patch_branch(uint8_t *at, const uint8_t *target, bool conditional)
{
    if (at == NULL || emu_jit_overflowed()) {
        return;
    }

    uint16_t *const hw = (uint16_t *)(void *)at;
    /* A branch reads pc as its own address plus 4. */
    const uint32_t imm = (uint32_t)(int32_t)(target - (at + 4)) >> 1;

    if (conditional) {
        const uint32_t s = (imm >> 19) & 1u;

        hw[0] = (uint16_t)((hw[0] & 0xFBC0u) | (s << 10) |
                           ((imm >> 11) & 0x3Fu));
        hw[1] = (uint16_t)(0x8000u | (((imm >> 18) & 1u) << 11) |
                           (((imm >> 17) & 1u) << 13) | (imm & 0x7FFu));
    } else {
        /* T4: J1 and J2 are stored inverted against S. */
        const uint32_t s = (imm >> 23) & 1u;
        const uint32_t j1 = (~((imm >> 22) ^ s)) & 1u;
        const uint32_t j2 = (~((imm >> 21) ^ s)) & 1u;

        hw[0] = (uint16_t)(0xF000u | (s << 10) | ((imm >> 11) & 0x3FFu));
        hw[1] = (uint16_t)(0x9000u | (j1 << 13) | (j2 << 11) |
                           (imm & 0x7FFu));
    }
}

/* MUL.W rd, rn, rm -- the low 32 bits of the product. */
void t2_mul(uint32_t rd, uint32_t rn, uint32_t rm)
{
    t2_emit32((uint16_t)(0xFB00u | rn), (uint16_t)(0xF000u | (rd << 8) | rm));
}

/*
 * SMULL / UMULL rdlo, rdhi, rn, rm -- the full 64-bit product.
 *
 * The high-half opcodes need this; MUL.W gives only the low 32 bits.
 * rdlo and rdhi must differ, which is the caller's business.
 */
void t2_mull(bool sign, uint32_t rdlo, uint32_t rdhi, uint32_t rn,
             uint32_t rm)
{
    t2_emit32((uint16_t)((sign ? 0xFB80u : 0xFBA0u) | rn),
              (uint16_t)((rdlo << 12) | (rdhi << 8) | rm));
}

#endif /* EMU_JIT_THUMB2 */
