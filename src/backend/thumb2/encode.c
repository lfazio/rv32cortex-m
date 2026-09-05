/* SPDX-License-Identifier: Apache-2.0 */
/*
 * src/backend/thumb2/encode.c - Thumb-2 encoding. See emu_thumb2.h.
 */

#include "emu/emu_thumb2.h"
#include "emu/emu_ir.h"

#if defined(EMU_HOST_JIT_THUMB2)

void t2_emit16(uint16_t h)
{
    emu_jit_emit16(h);
}

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
    t2_emit32((uint16_t)(0xF8D0u | rn),
              (uint16_t)((rt << 12) | (off & 0xFFFu)));
}

void t2_str_imm(uint32_t rt, uint32_t rn, uint32_t off)
{
    t2_emit32((uint16_t)(0xF8C0u | rn),
              (uint16_t)((rt << 12) | (off & 0xFFFu)));
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

/*
 * ARM's "modified immediate": a byte, rotated, or one of three repeating
 * patterns. Returns false for a constant that has no such form.
 *
 * Worth writing out rather than approximating, because a wrong answer
 * here does not fail to assemble -- it assembles as a *different
 * constant*, which is the same class of defect as the 16-bit CMP that
 * became `CMP r0, r1` and cost this project an interrupt-latency bound.
 * The four cases are the architecture's, in its order:
 *
 *   0000_0000 0000_0000 0000_0000 XYXY_XYXY   imm12 = 0000 XY
 *   0000_0000 XYXY_XYXY 0000_0000 XYXY_XYXY   imm12 = 0001 XY
 *   XYXY_XYXY 0000_0000 XYXY_XYXY 0000_0000   imm12 = 0010 XY
 *   XYXY_XYXY XYXY_XYXY XYXY_XYXY XYXY_XYXY   imm12 = 0011 XY
 *
 * and otherwise an 8-bit value with its top bit set, rotated right by
 * 8..31 -- so the test is that rotating *left* by that amount lands the
 * whole value inside 0x80..0xFF.
 */
bool t2_expand_imm(uint32_t v, uint16_t *out)
{
    const uint32_t b = v & 0xFFu;
    const uint32_t hb = (v >> 8) & 0xFFu;

    if (v < 0x100u) {
        *out = (uint16_t)v;
        return true;
    }
    if (b != 0u && v == ((b << 16) | b)) {
        *out = (uint16_t)(0x100u | b);
        return true;
    }
    if (hb != 0u && v == ((hb << 24) | (hb << 8))) {
        *out = (uint16_t)(0x200u | hb);
        return true;
    }
    if (b != 0u && v == (b * 0x01010101u)) {
        *out = (uint16_t)(0x300u | b);
        return true;
    }
    for (uint32_t rot = 8u; rot < 32u; rot++) {
        const uint32_t r = (v << rot) | (v >> (32u - rot));

        if (r >= 0x80u && r <= 0xFFu) {
            *out = (uint16_t)((rot << 7) | (r & 0x7Fu));
            return true;
        }
    }
    return false;
}

/* The T3 modified-immediate data-processing form: rd = rn <op> #imm. */
static void t2_dp_imm(uint16_t hw1_base, uint32_t rd, uint32_t rn,
                      uint16_t imm12)
{
    const uint32_t i = (imm12 >> 11) & 1u;
    const uint32_t imm3 = (imm12 >> 8) & 7u;
    const uint32_t imm8 = imm12 & 0xFFu;

    t2_emit32((uint16_t)(hw1_base | (i << 10) | rn),
              (uint16_t)((imm3 << 12) | (rd << 8) | imm8));
}

void t2_add_imm(uint32_t rd, uint32_t rn, uint16_t imm12)
{
    t2_dp_imm(0xF100u, rd, rn, imm12);
}
void t2_sub_imm(uint32_t rd, uint32_t rn, uint16_t imm12)
{
    t2_dp_imm(0xF1A0u, rd, rn, imm12);
}
void t2_and_imm(uint32_t rd, uint32_t rn, uint16_t imm12)
{
    t2_dp_imm(0xF000u, rd, rn, imm12);
}
void t2_orr_imm(uint32_t rd, uint32_t rn, uint16_t imm12)
{
    t2_dp_imm(0xF040u, rd, rn, imm12);
}
void t2_eor_imm(uint32_t rd, uint32_t rn, uint16_t imm12)
{
    t2_dp_imm(0xF080u, rd, rn, imm12);
}

/*
 * ADDW/SUBW: a plain unsigned 12-bit immediate with no rotation, which
 * reaches every value 0..4095 and so covers most of what a guest adds --
 * displacements, small constants, frame offsets. Separate from the T3
 * forms above because it is a different encoding, not a different
 * immediate: T3 can express 0x00FF0000 and cannot express 4000.
 */
void t2_addw(uint32_t rd, uint32_t rn, uint16_t imm12)
{
    const uint32_t i = (imm12 >> 11) & 1u;
    const uint32_t imm3 = (imm12 >> 8) & 7u;
    const uint32_t imm8 = imm12 & 0xFFu;

    t2_emit32((uint16_t)(0xF200u | (i << 10) | rn),
              (uint16_t)((imm3 << 12) | (rd << 8) | imm8));
}

void t2_subw(uint32_t rd, uint32_t rn, uint16_t imm12)
{
    const uint32_t i = (imm12 >> 11) & 1u;
    const uint32_t imm3 = (imm12 >> 8) & 7u;
    const uint32_t imm8 = imm12 & 0xFFu;

    t2_emit32((uint16_t)(0xF2A0u | (i << 10) | rn),
              (uint16_t)((imm3 << 12) | (rd << 8) | imm8));
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
void t2_shift_reg(uint32_t kind, uint32_t rd, uint32_t rn, uint32_t rm)
{
    t2_emit32((uint16_t)(0xFA00u | (kind << 5) | rn),
              (uint16_t)(0xF000u | (rd << 8) | rm));
}
#define T2_LSL 0u
#define T2_LSR 1u
#define T2_ASR 2u
#define T2_ROR 3u

/* Immediate shifts, as the MOV.W shifted-register form. */
void t2_shift_imm(uint32_t type, uint32_t rd, uint32_t rm, uint32_t amount)
{
    const uint32_t n = amount & 31u;

    /*
     * imm5 == 0 does not mean "no shift" for every type, and only LSL
     * reads it that way. ARM spends the otherwise-useless encoding on
     * the amount imm5 cannot hold: LSR #0 *is* LSR #32, ASR #0 is
     * ASR #32, and ROR #0 is RRX. So a shift by zero -- which RISC-V
     * spells `srli`/`srai rd, rs, 0` and means as a move -- assembled
     * as a shift by 32 and produced zero or a sign extension.
     *
     * Rewriting the type to LSL is what makes a zero shift a move for
     * all four. Do not "fix" this by skipping the emit instead: rd and
     * rm differ, so the move has to happen.
     *
     * Invisible to the x86-64 backend, whose `shr r32, 0` is a genuine
     * no-op, and to every host suite for the same reason. The board
     * found it: I-srli-00 and I-srai-00, which shift by zero 17 and 16
     * times.
     */
    if (n == 0u) {
        type = T2_LSL;
    }

    /*
     * The amount is split imm3:imm2 -- imm3 is the *high* three bits at
     * 14:12 and imm2 the low two at 7:6. Splitting it the other way
     * round encodes a different amount entirely and never faults: a
     * shift by 31 came out as a shift by 0, and only the guest's own
     * slli/srli/srai self-tests noticed.
     */
    t2_emit32(0xEA4Fu, (uint16_t)(((n >> 2) << 12) | (rd << 8) |
                                  ((n & 3u) << 6) | (type << 4) | rm));
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
/* VFP, single precision                                               */
/* ------------------------------------------------------------------ */

/*
 * Only S0-S2 are used, so the D/N/M bits below are always the low bit of
 * the register number and the V fields the rest. Spelling that out
 * rather than hiding it: every VFP encoding splits a register across two
 * non-adjacent fields, and putting both halves in the wrong place is the
 * failure this file has already had once with a Thumb-2 shift amount --
 * it assembles, and it names a different register.
 */
#define VFP_VD(n) (((n) >> 1) & 0xFu)
#define VFP_D(n) ((n) & 1u)

/* VMOV Sn, Rt (op 0) and VMOV Rt, Sn (op 1). */
void t2_vmov_core(uint32_t sn, uint32_t rt, bool to_core)
{
    t2_emit32((uint16_t)(0xEE00u | (to_core ? 0x10u : 0u) | VFP_VD(sn)),
              (uint16_t)((rt << 12) | 0x0A10u | (VFP_D(sn) << 7)));
}

/*
 * The three-register arithmetic group: VADD, VSUB, VMUL, VDIV .F32.
 * `hi` selects the opcode block and `sub` the bit 6 that separates VADD
 * from VSUB inside it.
 */
/*
 * **The N bit is bit 7 of the *second* halfword.**
 *
 * It carries sn's low bit, and this put it at bit 7 of the *first* --
 * where the encoding has nothing, so the instruction assembled with
 * N clear regardless. Harmless for as long as every caller passed an
 * even `sn`, which both of the original two did (`vadd s0, s0, s1` and
 * friends), and wrong the moment one did not: the fused multiply-add
 * lowering passes `sn = s1` and every FMA read s0 as its multiplicand.
 *
 * Checked against the assembler rather than the manual --
 * `vfma.f32 s0, s1, s2` is `eea0 0a81` and this emitted `eea0 0a01`.
 * That is the whole difference, and it cost a wrong answer on fptest's
 * mixed kernel with four other kernels agreeing.
 *
 * The lesson this file already carries twice, in a third place: an
 * encoder whose wrong answers are other valid instructions has to be
 * tested against the assembler at its *boundary* values, and for a
 * register field that means an odd one.
 */
void t2_vfp3(uint16_t hi, bool sub, uint32_t sd, uint32_t sn, uint32_t sm)
{
    t2_emit32((uint16_t)(hi | VFP_VD(sn)),
              (uint16_t)((VFP_VD(sd) << 12) | 0x0A00u | (VFP_D(sn) << 7) |
                         (VFP_D(sd) << 6) | (sub ? 0x40u : 0u) |
                         (VFP_D(sm) << 5) | VFP_VD(sm)));
}

void t2_vsqrt(uint32_t sd, uint32_t sm)
{
    t2_emit32((uint16_t)(0xEEB1u | (VFP_D(sd) << 6)),
              (uint16_t)((VFP_VD(sd) << 12) | 0x0AC0u | (VFP_D(sm) << 5) |
                         VFP_VD(sm)));
}

/* VMRS Rt, FPSCR and VMSR FPSCR, Rt. */
void t2_vmrs(uint32_t rt)
{
    t2_emit32(0xEEF1u, (uint16_t)((rt << 12) | 0x0A10u));
}

void t2_vmsr(uint32_t rt)
{
    t2_emit32(0xEEE1u, (uint16_t)((rt << 12) | 0x0A10u));
}

/* RBIT Rd, Rm -- reverses all 32 bits; see the flag note in ir_lower.c. */

/* ------------------------------------------------------------------ */
/* Control flow                                                        */
/* ------------------------------------------------------------------ */

/* Condition codes, as the cond field of a Thumb-2 conditional branch. */
uint32_t t2_cond(uint8_t c)
{
    switch ((emu_ir_cond_t)c) {
    case EMU_IR_C_EQ:
        return 0x0u;
    case EMU_IR_C_NE:
        return 0x1u;
    case EMU_IR_C_LTU:
        return 0x3u; /* CC/LO */
    case EMU_IR_C_GEU:
        return 0x2u; /* CS/HS */
    case EMU_IR_C_LT:
        return 0xBu;
    case EMU_IR_C_GE:
        return 0xAu;
    case EMU_IR_C_LE:
        return 0xDu;
    case EMU_IR_C_GT:
        return 0xCu;
    case EMU_IR_C_LEU:
        return 0x9u; /* LS */
    case EMU_IR_C_GTU:
        return 0x8u; /* HI */
    default:
        return 0xEu; /* AL */
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
    t2_emit16((uint16_t)(0x4780u | (T2_R12 << 3))); /* BLX r12 */
}

/*
 * Forward branches, with the displacement filled in by t2_patch_branch
 * once the target is known.
 */
uint8_t *t2_b_forward(void)
{
    uint8_t *const at = emu_jit_here();

    t2_emit32(0xF000u, 0xB800u); /* B.W  <label>  (T4) */
    return at;
}

uint8_t *t2_bcond_forward(uint32_t cond)
{
    uint8_t *const at = emu_jit_here();

    t2_emit32((uint16_t)(0xF000u | (cond << 6)), 0x8000u); /* B<c>.W (T3) */
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

        hw[0] =
            (uint16_t)((hw[0] & 0xFBC0u) | (s << 10) | ((imm >> 11) & 0x3Fu));
        hw[1] = (uint16_t)(0x8000u | (((imm >> 18) & 1u) << 11) |
                           (((imm >> 17) & 1u) << 13) | (imm & 0x7FFu));
    } else {
        /* T4: J1 and J2 are stored inverted against S. */
        const uint32_t s = (imm >> 23) & 1u;
        const uint32_t j1 = (~((imm >> 22) ^ s)) & 1u;
        const uint32_t j2 = (~((imm >> 21) ^ s)) & 1u;

        hw[0] = (uint16_t)(0xF000u | (s << 10) | ((imm >> 11) & 0x3FFu));
        hw[1] = (uint16_t)(0x9000u | (j1 << 13) | (j2 << 11) | (imm & 0x7FFu));
    }
}

/*
 * The registers a block may hand to the allocator.
 *
 * r4 and r5 are the cpu pointer and the retired count, r0-r3 and r12 are
 * scratch and the AAPCS argument registers, and sp is the frame -- so
 * these six are the whole of what is callee-saved and free. Low ones
 * first, because a list confined to r0-r7 fits the 16-bit PUSH.
 */
const uint32_t t2_alloc_regs[T2_ALLOC_REGS] = {6u, 7u, 8u, 9u, 10u, 11u};

/*
 * PUSH and POP of an explicit register list, taking the 16-bit form when
 * the list fits it.
 *
 * The 16-bit encodings reach r0-r7 plus LR (or PC), which covers the
 * common case here -- three fixed registers and at most two allocated --
 * and saves four bytes a block on the host where bytes are the currency.
 * Anything higher needs the 32-bit form, and handing a high register to
 * the narrow one would not fault: it would push a different register.
 */
void t2_push(uint32_t list)
{
    if ((list & ~0x40FFu) == 0u) {
        t2_emit16(
            (uint16_t)(0xB400u | ((list >> 6) & 0x100u) | (list & 0xFFu)));
        return;
    }
    t2_emit32(0xE92Du, (uint16_t)(list & 0x5FFFu));
}

void t2_pop(uint32_t list)
{
    if ((list & ~0x80FFu) == 0u) {
        t2_emit16(
            (uint16_t)(0xBC00u | ((list >> 7) & 0x100u) | (list & 0xFFu)));
        return;
    }
    t2_emit32(0xE8BDu, (uint16_t)(list & 0xDFFFu));
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
void t2_mull(bool sign, uint32_t rdlo, uint32_t rdhi, uint32_t rn, uint32_t rm)
{
    t2_emit32((uint16_t)((sign ? 0xFB80u : 0xFBA0u) | rn),
              (uint16_t)((rdlo << 12) | (rdhi << 8) | rm));
}

/* ------------------------------------------------------------------ */
/* Making emitted code fetchable                                        */
/* ------------------------------------------------------------------ */

/*
 * The platform's half, and the *only* `board_*` symbol emucore names.
 *
 * The contract and the documentation are in
 * src/platform/common/board_api.h; the prototype is repeated here because
 * src/emu/ is portable C11 with no platform dependency and so cannot
 * include that header. Two prototypes for one function is a drift risk,
 * and it is bounded: the signature is two arguments and a void return,
 * and a mismatch is a compile error at the definition site rather than
 * anything silent.
 *
 * **It is not weak any more, and that is the point of the move.** A weak
 * no-op here meant a new platform got cache maintenance that silently did
 * nothing -- which on a part with caches is not a wrong answer but
 * arbitrary code, on reuse of the buffer rather than on first write. The
 * Cortex-M55 port was written with no override and linked perfectly.
 * Every board defines this now, answering with nothing where there is
 * nothing to do, which is how board_api.h says a platform declines
 * everything else.
 */
void board_sync_icache(const void *addr, uint32_t len);

void t2_sync_code(const void *addr, uint32_t len)
{
    board_sync_icache(addr, len);

    /*
     * DSB before ISB, and both after the maintenance: the DSB makes the
     * writes (and any cache operations above) complete, the ISB flushes
     * the pipeline so nothing already fetched from these addresses is
     * executed. On a part without caches this pair alone is sufficient
     * and is what the M4 relies on.
     */
    __asm__ volatile("dsb 0xF" ::: "memory");
    __asm__ volatile("isb 0xF" ::: "memory");
}

#endif /* EMU_HOST_JIT_THUMB2 */
