/* SPDX-License-Identifier: Apache-2.0 */
/*
 * rv_decode.h - Instruction field extraction and RVC expansion.
 */
#ifndef RV32_RV_DECODE_H
#define RV32_RV_DECODE_H

#include "rv_types.h"
#include "rv_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Major opcodes (inst[6:0])                                           */
/* ------------------------------------------------------------------ */

#define OP_LOAD       0x03u
#define OP_MISC_MEM   0x0Fu
#define OP_IMM        0x13u
#define OP_AUIPC      0x17u
#define OP_STORE      0x23u
#define OP_AMO        0x2Fu
#define OP_OP         0x33u
#define OP_LUI        0x37u
#define OP_BRANCH     0x63u
#define OP_JALR       0x67u
#define OP_JAL        0x6Fu
#define OP_SYSTEM     0x73u

/* ------------------------------------------------------------------ */
/* Field accessors                                                     */
/* ------------------------------------------------------------------ */

static RV_ALWAYS_INLINE uint32_t rv_opcode(uint32_t i) { return i & 0x7Fu; }
static RV_ALWAYS_INLINE uint32_t rv_rd(uint32_t i)     { return (i >> 7) & 0x1Fu; }
static RV_ALWAYS_INLINE uint32_t rv_rs1(uint32_t i)    { return (i >> 15) & 0x1Fu; }
static RV_ALWAYS_INLINE uint32_t rv_rs2(uint32_t i)    { return (i >> 20) & 0x1Fu; }
static RV_ALWAYS_INLINE uint32_t rv_funct3(uint32_t i) { return (i >> 12) & 0x7u; }
static RV_ALWAYS_INLINE uint32_t rv_funct7(uint32_t i) { return (i >> 25) & 0x7Fu; }

/* Immediates, already sign-extended where the format calls for it. */
static RV_ALWAYS_INLINE int32_t rv_imm_i(uint32_t i)
{
    return (int32_t)i >> 20;
}

static RV_ALWAYS_INLINE int32_t rv_imm_s(uint32_t i)
{
    /* imm[11:5] = i[31:25], imm[4:0] = i[11:7] */
    return ((int32_t)(i & 0xFE000000u) >> 20) | (int32_t)((i >> 7) & 0x1Fu);
}

static RV_ALWAYS_INLINE int32_t rv_imm_b(uint32_t i)
{
    uint32_t v = ((i >> 7) & 0x1Eu)          /* imm[4:1]  */
               | ((i >> 20) & 0x7E0u)        /* imm[10:5] */
               | ((i << 4) & 0x800u)         /* imm[11]   */
               | ((i >> 19) & 0x1000u);      /* imm[12]   */
    return rv_sext(v, 13);
}

static RV_ALWAYS_INLINE uint32_t rv_imm_u(uint32_t i)
{
    return i & 0xFFFFF000u;
}

static RV_ALWAYS_INLINE int32_t rv_imm_j(uint32_t i)
{
    uint32_t v = ((i >> 20) & 0x7FEu)        /* imm[10:1] */
               | ((i >> 9) & 0x800u)         /* imm[11]   */
               | (i & 0xFF000u)              /* imm[19:12]*/
               | ((i >> 11) & 0x100000u);    /* imm[20]   */
    return rv_sext(v, 21);
}

/* ------------------------------------------------------------------ */
/* Compressed instructions                                             */
/* ------------------------------------------------------------------ */

/* True if the 16-bit parcel starts a 32-bit (or longer) instruction. */
static RV_ALWAYS_INLINE bool rv_is_32bit(uint16_t parcel)
{
    return (parcel & 0x3u) == 0x3u;
}

/*
 * Expand a 16-bit RVC instruction into the equivalent 32-bit encoding.
 * Returns 0 for reserved, illegal, or unsupported (F/D) encodings; 0 is
 * itself a permanently-illegal RISC-V encoding, so callers can treat the
 * result uniformly and let the normal illegal-instruction path raise the
 * trap with the original 16-bit value in mtval.
 */
uint32_t rv_decode_expand_c(uint16_t c);

/* Length in bytes of the instruction starting with `parcel`. */
static RV_ALWAYS_INLINE unsigned rv_insn_len(uint16_t parcel)
{
    return rv_is_32bit(parcel) ? 4u : 2u;
}

#ifdef __cplusplus
}
#endif

#endif /* RV32_RV_DECODE_H */
