/* SPDX-License-Identifier: Apache-2.0 */
/*
 * armv7m_decode.h - Thumb-2 and FPv5 encodings, decoded.
 *
 * Two things live here, and they answer to different masters.
 *
 * **The field rules** -- the instruction length, ThumbExpandImm,
 * DecodeImmShift, Shift_C, VFPExpandImm. One copy of each, used by the
 * interpreter, the disassembler and the IR translator alike. G4MH spelled
 * its length rule twice and the copies came apart on one slot; the
 * modified-immediate expansion is the most error-prone decode in this
 * instruction set and is not something to have three of.
 *
 * **`armv7m_decode`**, which turns an encoding into an operation and its
 * operands for the consumers that need to *talk about* an instruction
 * rather than run it: the disassembler, the pair statistics and the IR
 * translator. The interpreter does not go through it -- its decode is the
 * one checked instruction by instruction against a Cortex-M7 -- so this
 * is a second description of the encoding space, and is tested as one:
 * the disassembler against the assembler's own listing, the translator
 * against the board through the JIT, and both against the interpreter on
 * which encodings are UNDEFINED.
 */
#ifndef ARMV7M_DECODE_H
#define ARMV7M_DECODE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Length                                                              */
/* ------------------------------------------------------------------ */

/*
 * Is this halfword the first of a 32-bit instruction?
 *
 * ARMv7-M encodes the answer in bits 15:11 of the first halfword: the
 * three values 0b11101, 0b11110 and 0b11111 introduce a 32-bit
 * encoding, everything else is a complete 16-bit one.
 *
 * **One copy of this rule, and a property test over all 65536
 * halfwords.**
 */
static inline bool armv7m_is_32bit(uint16_t hw)
{
    const uint16_t top = (uint16_t)(hw >> 11);

    return top == 0x1Du || top == 0x1Eu || top == 0x1Fu;
}

static inline uint32_t armv7m_insn_len(uint16_t hw)
{
    return armv7m_is_32bit(hw) ? 4u : 2u;
}

/* ------------------------------------------------------------------ */
/* Shifts and immediates                                               */
/* ------------------------------------------------------------------ */

enum {
    ARMV7M_SH_LSL,
    ARMV7M_SH_LSR,
    ARMV7M_SH_ASR,
    ARMV7M_SH_ROR,
    ARMV7M_SH_RRX
};

/*
 * The four rules below are `static inline` so that the interpreter, which
 * calls Shift_C on most data-processing instructions, pays no call for
 * sharing them.
 *
 * DecodeImmShift: a two-bit type and a five-bit amount to an operation
 * and a count. LSR #0 and ASR #0 mean 32, and ROR #0 is RRX.
 *
 * Shift_C takes the amount already decoded: a register shift passes the
 * low byte of Rm, which may be anything up to 255. **The carry is the
 * point of it** -- every logical instruction with S set takes its C from
 * there.
 *
 * ThumbExpandImm_C: with bits 11:10 clear the value is one of four byte
 * patterns and the carry is unchanged; otherwise it is `1:imm7` rotated
 * right by the top five bits, and the carry is bit 31 of the result.
 *
 * VFPExpandImm, single precision: aBbbbbbc defgh000 ...
 */
static inline void armv7m_decode_imm_shift(uint32_t type, uint32_t imm5, uint32_t *t,
                             uint32_t *n)
{
    switch (type) {
    case 0u:
        *t = ARMV7M_SH_LSL;
        *n = imm5;
        break;
    case 1u:
        *t = ARMV7M_SH_LSR;
        *n = imm5 ? imm5 : 32u;
        break;
    case 2u:
        *t = ARMV7M_SH_ASR;
        *n = imm5 ? imm5 : 32u;
        break;
    default:
        if (imm5 == 0u) {
            *t = ARMV7M_SH_RRX;
            *n = 1u;
        } else {
            *t = ARMV7M_SH_ROR;
            *n = imm5;
        }
        break;
    }
}

/*
 * **The first version of this interpreter computed shifted operands
 * without a carry**, so `lsls r0, r0, #1; bcs` branched on a carry the
 * shift never produced.
 */
static inline uint32_t armv7m_shift_c(uint32_t v, uint32_t type, uint32_t amount,
                        uint32_t cin, uint32_t *cout)
{
    if (type == ARMV7M_SH_RRX) {
        *cout = v & 1u;
        return (cin << 31) | (v >> 1);
    }
    if (amount == 0u) {
        *cout = cin;
        return v;
    }
    switch (type) {
    case ARMV7M_SH_LSL:
        if (amount < 32u) {
            *cout = (v >> (32u - amount)) & 1u;
            return v << amount;
        }
        *cout = (amount == 32u) ? (v & 1u) : 0u;
        return 0u;
    case ARMV7M_SH_LSR:
        if (amount < 32u) {
            *cout = (v >> (amount - 1u)) & 1u;
            return v >> amount;
        }
        *cout = (amount == 32u) ? (v >> 31) : 0u;
        return 0u;
    case ARMV7M_SH_ASR:
        if (amount < 32u) {
            *cout = (v >> (amount - 1u)) & 1u;
            return (uint32_t)((int32_t)v >> amount);
        }
        *cout = v >> 31;
        return (uint32_t)((int32_t)v >> 31);
    default: { /* ROR */
        const uint32_t m = amount & 31u;
        const uint32_t res = (m == 0u) ? v : ((v >> m) | (v << (32u - m)));

        *cout = res >> 31;
        return res;
    }
    }
}

static inline uint32_t armv7m_expand_imm_c(uint32_t imm12, uint32_t cin, uint32_t *cout)
{
    const uint32_t imm8 = imm12 & 0xFFu;

    if ((imm12 & 0xC00u) == 0u) {
        *cout = cin;
        switch ((imm12 >> 8) & 3u) {
        case 0u:
            return imm8;
        case 1u:
            return (imm8 << 16) | imm8;
        case 2u:
            return (imm8 << 24) | (imm8 << 8);
        default:
            return imm8 * 0x01010101u;
        }
    }
    {
        const uint32_t v = 0x80u | (imm12 & 0x7Fu);
        const uint32_t rot = (imm12 >> 7) & 31u;
        const uint32_t res = (v >> rot) | (v << (32u - rot));

        *cout = res >> 31;
        return res;
    }
}

static inline uint32_t armv7m_vfp_expand_imm(uint32_t imm8)
{
    const uint32_t b = (imm8 >> 6) & 1u;
    const uint32_t exp = ((b ^ 1u) << 7) | (b ? 0x7Cu : 0u) | ((imm8 >> 4) & 3u);

    return ((imm8 >> 7) << 31) | (exp << 23) | ((imm8 & 0xFu) << 19);
}

/*
 * Is this a special register MRS and MSR can name? xPSR and its views,
 * the two stack pointers, and the four mask and control registers.
 * **Anything else is UNDEFINED on a Cortex-M7** -- not a read of zero
 * and a write of nothing, which is what this frontend did until the
 * board was asked, register by register.
 */
static inline bool armv7m_sysm_valid(uint32_t sysm)
{
    return sysm <= 3u || (sysm >= 5u && sysm <= 9u) ||
           (sysm >= 16u && sysm <= 20u);
}

/* ------------------------------------------------------------------ */
/* Operations                                                          */
/* ------------------------------------------------------------------ */

/*
 * X-macro, so that the name of an operation is generated from the same
 * list as its number. Most are the mnemonic; the ones that are not --
 * VCVT_TOS, VMOV_CORE2 -- name one of several things a mnemonic means.
 */
#define ARMV7M_OPS(X)                                                          \
    X(UNDEF) X(UNPRED) X(NOCP)                                                 \
    /* data processing */                                                      \
    X(AND) X(BIC) X(ORR) X(ORN) X(EOR) X(ADD) X(ADC) X(SBC) X(SUB) X(RSB)      \
    X(MOV) X(MVN) X(TST) X(TEQ) X(CMN) X(CMP)                                  \
    X(LSL) X(LSR) X(ASR) X(ROR) X(RRX)                                         \
    X(MOVW) X(MOVT) X(ADR)                                                     \
    /* multiplies and divide */                                                \
    X(MUL) X(MLA) X(MLS) X(SMULL) X(UMULL) X(SMLAL) X(UMLAL) X(UMAAL)          \
    X(SDIV) X(UDIV)                                                            \
    X(SMLABB) X(SMLABT) X(SMLATB) X(SMLATT)                                    \
    X(SMULBB) X(SMULBT) X(SMULTB) X(SMULTT)                                    \
    X(SMLAD) X(SMLADX) X(SMUAD) X(SMUADX)                                      \
    X(SMLAWB) X(SMLAWT) X(SMULWB) X(SMULWT)                                    \
    X(SMLSD) X(SMLSDX) X(SMUSD) X(SMUSDX)                                      \
    X(SMMLA) X(SMMLAR) X(SMMUL) X(SMMULR) X(SMMLS) X(SMMLSR)                   \
    X(USAD8) X(USADA8)                                                         \
    X(SMLALBB) X(SMLALBT) X(SMLALTB) X(SMLALTT)                                \
    X(SMLALD) X(SMLALDX) X(SMLSLD) X(SMLSLDX)                                  \
    /* saturation, bit fields, extension, bytes */                             \
    X(SSAT) X(USAT) X(SSAT16) X(USAT16) X(QADD) X(QDADD) X(QSUB) X(QDSUB)      \
    X(SBFX) X(UBFX) X(BFI) X(BFC)                                              \
    X(SXTB) X(SXTH) X(UXTB) X(UXTH) X(SXTB16) X(UXTB16)                        \
    X(SXTAB) X(SXTAH) X(UXTAB) X(UXTAH) X(SXTAB16) X(UXTAB16)                  \
    X(REV) X(REV16) X(REVSH) X(RBIT) X(CLZ) X(SEL) X(PKHBT) X(PKHTB)           \
    X(PARALLEL)                                                                \
    /* loads and stores */                                                     \
    X(LDR) X(LDRB) X(LDRH) X(LDRSB) X(LDRSH) X(STR) X(STRB) X(STRH)            \
    X(LDRT) X(LDRBT) X(LDRHT) X(LDRSBT) X(LDRSHT) X(STRT) X(STRBT) X(STRHT)    \
    X(LDRD) X(STRD) X(LDREX) X(LDREXB) X(LDREXH) X(STREX) X(STREXB) X(STREXH)  \
    X(CLREX) X(LDM) X(LDMDB) X(STM) X(STMDB) X(PUSH) X(POP)                    \
    X(TBB) X(TBH) X(PLD) X(PLI)                                                \
    /* branches */                                                             \
    X(B) X(BCC) X(BL) X(BX) X(BLX) X(CBZ) X(CBNZ)                              \
    /* system */                                                               \
    X(SVC) X(BKPT) X(UDF) X(IT) X(NOP) X(YIELD) X(WFE) X(WFI) X(SEV)           \
    X(CPSIE) X(CPSID) X(MSR) X(MRS) X(DSB) X(DMB) X(ISB)                       \
    /* floating point */                                                       \
    X(VADD) X(VSUB) X(VMUL) X(VDIV) X(VNMUL) X(VMLA) X(VMLS) X(VNMLA)          \
    X(VNMLS) X(VFMA) X(VFMS) X(VFNMA) X(VFNMS) X(VSQRT) X(VABS) X(VNEG)        \
    X(VMOV) X(VMOVI) X(VCMP) X(VCMPE) X(VCMPZ) X(VCMPEZ)                       \
    X(VCVT_TOS) X(VCVT_TOU) X(VCVTR_TOS) X(VCVTR_TOU) X(VCVT_FROMS)            \
    X(VCVT_FROMU) X(VCVT_FIX) X(VCVTB) X(VCVTT)                                \
    X(VCVTA) X(VCVTN) X(VCVTP) X(VCVTM)                                        \
    X(VRINTA) X(VRINTN) X(VRINTP) X(VRINTM) X(VRINTR) X(VRINTZ) X(VRINTX)      \
    X(VMAXNM) X(VMINNM) X(VSEL)                                                \
    X(VMOV_CORE) X(VMOV_CORE2) X(VMOV_SCALAR) X(VMRS) X(VMSR)                  \
    X(VLDR) X(VSTR) X(VLDM) X(VSTM) X(VLDMDB) X(VSTMDB) X(VPUSH) X(VPOP)

typedef enum {
#define X(n) ARMV7M_OP_##n,
    ARMV7M_OPS(X)
#undef X
    ARMV7M_OP_COUNT
} armv7m_op_t;

/* What the second operand of a data-processing instruction is. */
enum {
    ARMV7M_OPND_NONE,
    ARMV7M_OPND_IMM,     /* imm, with imm_carry the expansion's carry     */
    ARMV7M_OPND_REG,     /* rm, shifted by shift_t/shift_n                */
    ARMV7M_OPND_REGSHIFT /* rn shifted by the low byte of rm              */
};

/* How a load or store finds its address. */
enum {
    ARMV7M_ADDR_NONE,
    ARMV7M_ADDR_IMM,    /* [rn, #imm], with index/add/wback            */
    ARMV7M_ADDR_REG,    /* [rn, rm, lsl #shift_n]                      */
    ARMV7M_ADDR_LITERAL /* [pc, #imm], Align(pc, 4)                    */
};

#define ARMV7M_NOREG 0xFFu

/*
 * setflags: the 16-bit data-processing forms set flags *outside* an IT
 * block and not inside one, so the encoding alone cannot say.
 */
enum { ARMV7M_SF_NO, ARMV7M_SF_YES, ARMV7M_SF_NOT_IN_IT };

typedef struct armv7m_insn {
    uint16_t op;      /* armv7m_op_t                                   */
    uint8_t len;      /* 2 or 4                                        */
    uint8_t rd;       /* destination, or Rt for a load or store        */
    uint8_t rn;       /* first operand, or the base register           */
    uint8_t rm;       /* second operand, or the index                  */
    uint8_t ra;       /* accumulator; Rt2; RdHi                        */
    uint8_t cond;     /* B<cond>, IT's first condition, VSEL's         */
    uint8_t setflags; /* ARMV7M_SF_*                                   */
    uint8_t opnd;     /* ARMV7M_OPND_*                                 */
    uint8_t addr;     /* ARMV7M_ADDR_*                                 */
    uint8_t shift_t;  /* ARMV7M_SH_*                                   */
    uint8_t shift_n;
    uint8_t index;    /* P: the offset applies to the access           */
    uint8_t add;      /* U: the offset is added                        */
    uint8_t wback;    /* W: the base is written back                   */
    uint8_t size;     /* access width in bytes                         */
    uint8_t sext;     /* a narrow load sign-extends                    */
    uint8_t wide;     /* a 32-bit encoding of something with a narrow  */
    uint8_t fp_dbl;   /* an FP load/store/move names D registers       */
    uint8_t imm_carry; /* 0, 1, or 2 for "unchanged"                   */
    uint32_t imm;     /* immediate, offset, branch offset, reg list    */
    uint32_t imm2;    /* a second one: lsb/width, sat position, sysm   */
} armv7m_insn_t;

/*
 * Decode one instruction. `w1` is ignored for a 16-bit encoding. Never
 * fails: an encoding with no meaning comes back as ARMV7M_OP_UNDEF.
 */
void armv7m_decode(uint16_t w0, uint16_t w1, armv7m_insn_t *out);

/* An operation's name as spelled in ARMV7M_OPS; "?" out of range. */
const char *armv7m_op_name(uint32_t op);

#ifdef __cplusplus
}
#endif

#endif /* ARMV7M_DECODE_H */
