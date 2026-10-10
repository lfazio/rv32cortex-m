/* SPDX-License-Identifier: Apache-2.0 */
/*
 * ppc_decode.h - field extraction and instruction length for VLE.
 *
 * PowerPC numbers bits from the most significant, so every shift here is
 * `31 - manual_bit`. The conversion happens once, in this file, and
 * nothing downstream deals in the manual's convention.
 */
#ifndef PPC_DECODE_H
#define PPC_DECODE_H

#include "ppc/ppc_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Primary opcode: bits 0:5 of a 32-bit instruction. */
static EMU_ALWAYS_INLINE uint32_t ppc_op6(uint32_t w)
{
    return (w >> 26) & 0x3Fu;
}

/* The three register fields of the classic X/D forms. */
static EMU_ALWAYS_INLINE uint32_t ppc_rd(uint32_t w)
{
    return (w >> 21) & 0x1Fu;
}
static EMU_ALWAYS_INLINE uint32_t ppc_ra(uint32_t w)
{
    return (w >> 16) & 0x1Fu;
}
static EMU_ALWAYS_INLINE uint32_t ppc_rb(uint32_t w)
{
    return (w >> 11) & 0x1Fu;
}

/* Extended opcode, bits 21:30, and the Rc bit that asks for CR0. */
static EMU_ALWAYS_INLINE uint32_t ppc_xo10(uint32_t w)
{
    return (w >> 1) & 0x3FFu;
}
static EMU_ALWAYS_INLINE bool ppc_rc(uint32_t w)
{
    return (w & 1u) != 0u;
}
static EMU_ALWAYS_INLINE bool ppc_oe(uint32_t w)
{
    return (w & (1u << 10)) != 0u;
}

/* D-form's signed 16-bit displacement. */
static EMU_ALWAYS_INLINE int32_t ppc_d16(uint32_t w)
{
    return (int32_t)(int16_t)(uint16_t)(w & 0xFFFFu);
}

/*
 * Instruction length, from the first halfword -- **in VLE only**.
 *
 * Classic Book E is fixed 32-bit and this function must not be applied
 * to it. The two are different encodings of the same bytes rather than a
 * superset and a subset, and mixing them is not a subtle error: `bl`
 * (0x48000009) has top4 = 0x4, so the VLE rule calls it 16-bit and the
 * stream desynchronises from there. That mistake was made here first
 * time round and caught by test_branch.
 *
 * **Derived from the assembler, not from a diagram.** Tabulated with
 * scripts/ppc-check-encodings.sh across thirteen values of bits 0:3:
 *
 *     0 2 4 6 8 9 A B C D E  ->  16-bit
 *     1 3 5 7                ->  32-bit
 *
 * so 32-bit is exactly "bit 3 clear and bit 0 set". Note 0x9, 0xB and
 * 0xD: they have bit 0 set and are 16-bit, so the test is not `top4 & 1`
 * -- which is the guess the layout invites, and which would
 * desynchronise on se_lwz (0xC0..) and se_stw (0xD0..) among others.
 *
 * A wrong length here is not a wrong answer. It is a desynchronised
 * instruction stream, and every instruction after it is garbage.
 */
static EMU_ALWAYS_INLINE unsigned ppc_vle_len(uint16_t w0)
{
    return (((w0 >> 12) & 0x9u) == 0x1u) ? 4u : 2u;
}

/* ------------------------------------------------------------------ */
/* VLE 16-bit forms                                                    */
/* ------------------------------------------------------------------ */
/*
 * The compressed register field is four bits and does *not* mean r0-r15.
 * It maps 0-7 to r0-r7 and 8-15 to r24-r31 -- the two ends of the file,
 * which is where an EABI keeps its scratch and its callee-saved
 * registers. Read as a plain index it silently addresses r8-r15, which
 * are live registers, so nothing faults and the wrong value is used.
 *
 * Confirmed against the assembler rather than assumed: `se_mr 3,8`
 * through `se_mr 3,23` are *rejected* as invalid registers, and
 * `se_mr r3,r24` encodes as 0x0183.
 */
static EMU_ALWAYS_INLINE uint32_t ppc_se_reg(uint32_t f)
{
    return (f < 8u) ? f : (f + 16u);
}

/* SD4-form's displacement is scaled by the access size, so the same
 * nibble means 15 for a byte and 60 for a word. */
static EMU_ALWAYS_INLINE uint32_t ppc_se_sd4(uint16_t w, uint32_t size)
{
    return ((uint32_t)(w >> 8) & 0xFu) * size;
}

/* ------------------------------------------------------------------ */
/* The decoded form                                                    */
/* ------------------------------------------------------------------ */

/*
 * What an instruction *does*, as the interpreter and the translator see
 * it -- independent of which of the e200's two encodings it came from.
 * `e_add16i`, `addi` and `se_addi` are all PPC_S_ADDI with different
 * fields; the decoder is the only place that knows they differ.
 */
#define PPC_SEMANTICS(X)                                                       \
    X(ILLEGAL) X(PRIV_ILLEGAL)                                                 \
    /* arithmetic, register: rD = f(rA, rB), OE and Rc in the insn */          \
    X(ADD) X(ADDC) X(ADDE) X(ADDME) X(ADDZE)                                   \
    X(SUBF) X(SUBFC) X(SUBFE) X(SUBFME) X(SUBFZE) X(NEG)                       \
    X(MULLW) X(MULHW) X(MULHWU) X(DIVW) X(DIVWU)                               \
    /* arithmetic, immediate */                                                \
    X(LI) X(ADDI) X(ADDIC) X(SUBFIC) X(MULLI)                                  \
    /* logical: rA = rS op rB, or rS op imm */                                 \
    X(AND) X(ANDC) X(OR) X(ORC) X(XOR) X(NAND) X(NOR) X(EQV)                   \
    X(ANDI) X(ORI) X(XORI) X(MR)                                               \
    X(EXTSB) X(EXTSH) X(EXTZB) X(EXTZH) X(CNTLZW)                              \
    /* shifts and rotates */                                                   \
    X(SLW) X(SRW) X(SRAW) X(SRAWI) X(RLWINM) X(RLWIMI) X(RLWNM) X(BTST)        \
    /* compares: crf <- (rA or its halfword) against rB or imm */              \
    X(CMP) X(CMPL) X(CMPH) X(CMPHL)                                            \
    /* the condition register */                                               \
    X(CRAND) X(CRANDC) X(CREQV) X(CRNAND) X(CRNOR) X(CROR) X(CRORC) X(CRXOR)   \
    X(MCRF) X(MCRXR) X(MFCR) X(MTCRF) X(ISEL)                                  \
    /* memory */                                                               \
    X(LOAD) X(STORE) X(LMW) X(STMW) X(LARX) X(STCX) X(LMV) X(STMV) X(DCBZ)     \
    /* control */                                                              \
    X(B) X(BC) X(BCLR) X(BCCTR)                                                \
    X(SC) X(RFI) X(RFCI) X(RFDI) X(RFMCI) X(TW) X(WAIT)                        \
    X(MFMSR) X(MTMSR) X(WRTEE) X(MFSPR) X(MTSPR) X(NOP) X(PRIV_NOP)            \
    /* the embedded floating point unit, scalar */                             \
    X(EFSADD) X(EFSSUB) X(EFSMUL) X(EFSDIV) X(EFSMADD) X(EFSMSUB)              \
    X(EFSNMADD) X(EFSNMSUB) X(EFSABS) X(EFSNABS) X(EFSNEG) X(EFSSQRT)          \
    X(EFSMAX) X(EFSMIN) X(EFSCMPGT) X(EFSCMPLT) X(EFSCMPEQ)                    \
    X(EFSTSTGT) X(EFSTSTLT) X(EFSTSTEQ)                                        \
    X(EFSCFUI) X(EFSCFSI) X(EFSCFUF) X(EFSCFSF) X(EFSCFH)                      \
    X(EFSCTUI) X(EFSCTSI) X(EFSCTUF) X(EFSCTSF) X(EFSCTUIZ) X(EFSCTSIZ)        \
    X(EFSCTH)                                                                  \
    /* SPE and the vector half of the EFPU, which this model does not have */  \
    X(SPE)

typedef enum {
#define X(n) PPC_S_##n,
    PPC_SEMANTICS(X)
#undef X
    PPC_S_COUNT
} ppc_sem_t;

/*
 * Operand layouts, which is all the disassembler needs beyond the name.
 * The letters are the order the assembler takes them in.
 */
enum {
    PPC_F_NONE,
    PPC_F_D,          /* rD                         */
    PPC_F_S,          /* rS                         */
    PPC_F_D_A,        /* rD, rA                     */
    PPC_F_A_S,        /* rA, rS                     */
    PPC_F_D_B,        /* rD, rB                     */
    PPC_F_A_B,        /* rA, rB                     */
    PPC_F_D_A_B,      /* rD, rA, rB                 */
    PPC_F_A_S_B,      /* rA, rS, rB                 */
    PPC_F_D_A_SI,     /* rD, rA, simm               */
    PPC_F_D_A_UI,     /* rD, rA, uimm               */
    PPC_F_A_S_UI,     /* rA, rS, uimm               */
    PPC_F_A_S_SH,     /* rA, rS, sh                 */
    PPC_F_D_SI,       /* rD, simm                   */
    PPC_F_D_UI,       /* rD, uimm                   */
    PPC_F_A_SI,       /* rA, simm                   */
    PPC_F_A_UI,       /* rA, uimm                   */
    PPC_F_X_UI,       /* se_ rX, uimm               */
    PPC_F_D_MEM,      /* rD, d(rA)                  */
    PPC_F_MEM,        /* d(rA)                      */
    PPC_F_CRF_A_B,    /* crfD, rA, rB               */
    PPC_F_CRF_A_SI,   /* crfD, rA, simm             */
    PPC_F_CRF_A_UI,   /* crfD, rA, uimm             */
    PPC_F_CRB3,       /* crbD, crbA, crbB           */
    PPC_F_CRF_CRF,    /* crfD, crfS                 */
    PPC_F_CRF,        /* crfD                       */
    PPC_F_FXM_S,      /* fxm, rS                    */
    PPC_F_D_SPR,      /* rD, spr                    */
    PPC_F_SPR_S,      /* spr, rS                    */
    PPC_F_ISEL,       /* rD, rA, rB, crb            */
    PPC_F_RLWINM,     /* rA, rS, sh, mb, me         */
    PPC_F_RLWNM,      /* rA, rS, rB, mb, me         */
    PPC_F_TARGET,     /* target                     */
    PPC_F_BC,         /* BO, BI, target             */
    PPC_F_BO_BI,      /* BO, BI                     */
    PPC_F_E_BC,       /* e_bc BO32, BI32, target    */
    PPC_F_SE_BC,      /* se_bc BO16, BI16, target   */
    PPC_F_TO_A_B,     /* TO, rA, rB                 */
    PPC_F_TO_A_SI,    /* TO, rA, simm               */
    PPC_F_LEV,        /* the system call's LEV      */
    PPC_F_E,          /* wrteei E                   */
    PPC_F_MO,         /* mbar MO                    */
    PPC_F_CT_A_B,     /* CT, rA, rB                 */
    PPC_F_RAW         /* .long: the decoder does not know it */
};

/*
 * Every mnemonic this core has, with what it does and how it is
 * written. The name is the assembler's, so the disassembler's output
 * assembles back to the same bytes -- which is how both it and this
 * table are tested.
 */
#define PPC_MNEMONICS(X)                                                       \
    X(UNKNOWN, ".long", ILLEGAL, RAW)                                          \
    /* VLE, 16-bit */                                                          \
    X(SE_ILLEGAL, "se_illegal", ILLEGAL, NONE)                                 \
    X(SE_ISYNC, "se_isync", NOP, NONE)                                         \
    X(SE_SC, "se_sc", SC, NONE)                                                \
    X(SE_BLR, "se_blr", BCLR, NONE)                                            \
    X(SE_BLRL, "se_blrl", BCLR, NONE)                                          \
    X(SE_BCTR, "se_bctr", BCCTR, NONE)                                         \
    X(SE_BCTRL, "se_bctrl", BCCTR, NONE)                                       \
    X(SE_RFI, "se_rfi", RFI, NONE)                                             \
    X(SE_RFCI, "se_rfci", RFCI, NONE)                                          \
    X(SE_RFDI, "se_rfdi", RFDI, NONE)                                          \
    X(SE_RFMCI, "se_rfmci", RFMCI, NONE)                                       \
    X(SE_NOT, "se_not", NOR, D)                                                \
    X(SE_NEG, "se_neg", NEG, D)                                                \
    X(SE_MFLR, "se_mflr", MFSPR, D)                                            \
    X(SE_MTLR, "se_mtlr", MTSPR, S)                                            \
    X(SE_MFCTR, "se_mfctr", MFSPR, D)                                          \
    X(SE_MTCTR, "se_mtctr", MTSPR, S)                                          \
    X(SE_EXTZB, "se_extzb", EXTZB, D)                                          \
    X(SE_EXTSB, "se_extsb", EXTSB, D)                                          \
    X(SE_EXTZH, "se_extzh", EXTZH, D)                                          \
    X(SE_EXTSH, "se_extsh", EXTSH, D)                                          \
    X(SE_MR, "se_mr", MR, D_A)                                                 \
    X(SE_MTAR, "se_mtar", MR, D_A)                                             \
    X(SE_MFAR, "se_mfar", MR, D_A)                                             \
    X(SE_ADD, "se_add", ADD, D_B)                                              \
    X(SE_MULLW, "se_mullw", MULLW, D_B)                                        \
    X(SE_SUB, "se_sub", SUBF, D_A)                                             \
    X(SE_SUBF, "se_subf", SUBF, D_B)                                           \
    X(SE_CMP, "se_cmp", CMP, A_B)                                              \
    X(SE_CMPL, "se_cmpl", CMPL, A_B)                                           \
    X(SE_CMPH, "se_cmph", CMPH, A_B)                                           \
    X(SE_CMPHL, "se_cmphl", CMPHL, A_B)                                        \
    X(SE_ADDI, "se_addi", ADDI, X_UI)                                          \
    X(SE_CMPLI, "se_cmpli", CMPL, X_UI)                                        \
    X(SE_SUBI, "se_subi", ADDI, X_UI)                                          \
    X(SE_SUBI_, "se_subi.", ADDI, X_UI)                                        \
    X(SE_CMPI, "se_cmpi", CMP, X_UI)                                           \
    X(SE_BMASKI, "se_bmaski", LI, X_UI)                                        \
    X(SE_ANDI, "se_andi", ANDI, X_UI)                                          \
    X(SE_SRW, "se_srw", SRW, A_B)                                              \
    X(SE_SRAW, "se_sraw", SRAW, A_B)                                           \
    X(SE_SLW, "se_slw", SLW, A_B)                                              \
    X(SE_OR, "se_or", OR, A_B)                                                 \
    X(SE_ANDC, "se_andc", ANDC, A_B)                                           \
    X(SE_AND, "se_and", AND, A_B)                                              \
    X(SE_AND_, "se_and.", AND, A_B)                                            \
    X(SE_LI, "se_li", LI, X_UI)                                                \
    X(SE_BCLRI, "se_bclri", ANDI, X_UI)                                        \
    X(SE_BGENI, "se_bgeni", LI, X_UI)                                          \
    X(SE_BSETI, "se_bseti", ORI, X_UI)                                         \
    X(SE_BTSTI, "se_btsti", BTST, X_UI)                                        \
    X(SE_SRWI, "se_srwi", RLWINM, X_UI)                                        \
    X(SE_SRAWI, "se_srawi", SRAWI, X_UI)                                       \
    X(SE_SLWI, "se_slwi", RLWINM, X_UI)                                        \
    X(SE_LBZ, "se_lbz", LOAD, D_MEM)                                           \
    X(SE_STB, "se_stb", STORE, D_MEM)                                          \
    X(SE_LHZ, "se_lhz", LOAD, D_MEM)                                           \
    X(SE_STH, "se_sth", STORE, D_MEM)                                          \
    X(SE_LWZ, "se_lwz", LOAD, D_MEM)                                           \
    X(SE_STW, "se_stw", STORE, D_MEM)                                          \
    X(SE_BC, "se_bc", BC, SE_BC)                                               \
    X(SE_B, "se_b", B, TARGET)                                                 \
    X(SE_BL, "se_bl", B, TARGET)                                               \
    /* VLE, 32-bit */                                                          \
    X(E_ADD16I, "e_add16i", ADDI, D_A_SI)                                      \
    X(E_LBZ, "e_lbz", LOAD, D_MEM)                                             \
    X(E_STB, "e_stb", STORE, D_MEM)                                            \
    X(E_LHA, "e_lha", LOAD, D_MEM)                                             \
    X(E_LHZ, "e_lhz", LOAD, D_MEM)                                             \
    X(E_STH, "e_sth", STORE, D_MEM)                                            \
    X(E_LWZ, "e_lwz", LOAD, D_MEM)                                             \
    X(E_STW, "e_stw", STORE, D_MEM)                                            \
    X(E_LBZU, "e_lbzu", LOAD, D_MEM)                                           \
    X(E_LHZU, "e_lhzu", LOAD, D_MEM)                                           \
    X(E_LWZU, "e_lwzu", LOAD, D_MEM)                                           \
    X(E_LHAU, "e_lhau", LOAD, D_MEM)                                           \
    X(E_STBU, "e_stbu", STORE, D_MEM)                                          \
    X(E_STHU, "e_sthu", STORE, D_MEM)                                          \
    X(E_STWU, "e_stwu", STORE, D_MEM)                                          \
    X(E_LMW, "e_lmw", LMW, D_MEM)                                              \
    X(E_STMW, "e_stmw", STMW, D_MEM)                                           \
    X(E_LMVGPRW, "e_lmvgprw", LMV, MEM)                                        \
    X(E_STMVGPRW, "e_stmvgprw", STMV, MEM)                                     \
    X(E_LMVSPRW, "e_lmvsprw", LMV, MEM)                                        \
    X(E_STMVSPRW, "e_stmvsprw", STMV, MEM)                                     \
    X(E_LMVSRRW, "e_lmvsrrw", LMV, MEM)                                        \
    X(E_STMVSRRW, "e_stmvsrrw", STMV, MEM)                                     \
    X(E_LMVCSRRW, "e_lmvcsrrw", LMV, MEM)                                      \
    X(E_STMVCSRRW, "e_stmvcsrrw", STMV, MEM)                                   \
    X(E_LMVDSRRW, "e_lmvdsrrw", LMV, MEM)                                      \
    X(E_STMVDSRRW, "e_stmvdsrrw", STMV, MEM)                                   \
    X(E_LMVMCSRRW, "e_lmvmcsrrw", LMV, MEM)                                    \
    X(E_STMVMCSRRW, "e_stmvmcsrrw", STMV, MEM)                                 \
    X(E_ADDI, "e_addi", ADDI, D_A_SI)                                          \
    X(E_ADDI_, "e_addi.", ADDI, D_A_SI)                                        \
    X(E_ADDIC, "e_addic", ADDIC, D_A_SI)                                       \
    X(E_ADDIC_, "e_addic.", ADDIC, D_A_SI)                                     \
    X(E_MULLI, "e_mulli", MULLI, D_A_SI)                                       \
    X(E_CMPI, "e_cmpi", CMP, CRF_A_SI)                                         \
    X(E_CMPLI, "e_cmpli", CMPL, CRF_A_UI)                                      \
    X(E_SUBFIC, "e_subfic", SUBFIC, D_A_SI)                                    \
    X(E_SUBFIC_, "e_subfic.", SUBFIC, D_A_SI)                                  \
    X(E_ANDI, "e_andi", ANDI, A_S_UI)                                          \
    X(E_ANDI_, "e_andi.", ANDI, A_S_UI)                                        \
    X(E_ORI, "e_ori", ORI, A_S_UI)                                             \
    X(E_ORI_, "e_ori.", ORI, A_S_UI)                                           \
    X(E_XORI, "e_xori", XORI, A_S_UI)                                          \
    X(E_XORI_, "e_xori.", XORI, A_S_UI)                                        \
    X(E_LI, "e_li", LI, D_SI)                                                  \
    X(E_ADD2I_, "e_add2i.", ADDI, A_SI)                                        \
    X(E_ADD2IS, "e_add2is", ADDI, A_SI)                                        \
    X(E_CMP16I, "e_cmp16i", CMP, A_SI)                                         \
    X(E_MULL2I, "e_mull2i", MULLI, A_SI)                                       \
    X(E_CMPL16I, "e_cmpl16i", CMPL, A_UI)                                      \
    X(E_CMPH16I, "e_cmph16i", CMPH, A_SI)                                      \
    X(E_CMPHL16I, "e_cmphl16i", CMPHL, A_UI)                                   \
    X(E_OR2I, "e_or2i", ORI, D_UI)                                             \
    X(E_AND2I_, "e_and2i.", ANDI, D_UI)                                        \
    X(E_OR2IS, "e_or2is", ORI, D_UI)                                           \
    X(E_LIS, "e_lis", LI, D_UI)                                                \
    X(E_AND2IS_, "e_and2is.", ANDI, D_UI)                                      \
    X(E_RLWIMI, "e_rlwimi", RLWIMI, RLWINM)                                    \
    X(E_RLWINM, "e_rlwinm", RLWINM, RLWINM)                                    \
    X(E_B, "e_b", B, TARGET)                                                   \
    X(E_BL, "e_bl", B, TARGET)                                                 \
    X(E_BC, "e_bc", BC, E_BC)                                                  \
    X(E_BCL, "e_bcl", BC, E_BC)                                                \
    X(E_CMPH, "e_cmph", CMPH, CRF_A_B)                                         \
    X(E_CMPHL, "e_cmphl", CMPHL, CRF_A_B)                                      \
    X(E_MCRF, "e_mcrf", MCRF, CRF_CRF)                                         \
    X(E_CRAND, "e_crand", CRAND, CRB3)                                         \
    X(E_CRANDC, "e_crandc", CRANDC, CRB3)                                      \
    X(E_CREQV, "e_creqv", CREQV, CRB3)                                         \
    X(E_CRNAND, "e_crnand", CRNAND, CRB3)                                      \
    X(E_CRNOR, "e_crnor", CRNOR, CRB3)                                         \
    X(E_CROR, "e_cror", CROR, CRB3)                                            \
    X(E_CRORC, "e_crorc", CRORC, CRB3)                                         \
    X(E_CRXOR, "e_crxor", CRXOR, CRB3)                                         \
    X(E_RLW, "e_rlw", RLWNM, A_S_B)                                            \
    X(E_RLW_, "e_rlw.", RLWNM, A_S_B)                                          \
    X(E_RLWI, "e_rlwi", RLWINM, A_S_SH)                                        \
    X(E_RLWI_, "e_rlwi.", RLWINM, A_S_SH)                                      \
    X(E_SLWI, "e_slwi", RLWINM, A_S_SH)                                        \
    X(E_SLWI_, "e_slwi.", RLWINM, A_S_SH)                                      \
    X(E_SRWI, "e_srwi", RLWINM, A_S_SH)                                        \
    X(E_SRWI_, "e_srwi.", RLWINM, A_S_SH)                                      \
    X(E_SC, "e_sc", SC, LEV)                                                   \
    /* Book E, D/I/B/SC forms (non-VLE pages only) */                          \
    X(TWI, "twi", TW, TO_A_SI)                                                 \
    X(MULLI, "mulli", MULLI, D_A_SI)                                           \
    X(SUBFIC, "subfic", SUBFIC, D_A_SI)                                        \
    X(CMPLI, "cmpli", CMPL, CRF_A_UI)                                          \
    X(CMPI, "cmpi", CMP, CRF_A_SI)                                             \
    X(ADDIC, "addic", ADDIC, D_A_SI)                                           \
    X(ADDIC_, "addic.", ADDIC, D_A_SI)                                         \
    X(ADDI, "addi", ADDI, D_A_SI)                                              \
    X(ADDIS, "addis", ADDI, D_A_SI)                                            \
    X(BC, "bc", BC, BC)                                                        \
    X(SC, "sc", SC, LEV)                                                       \
    X(B, "b", B, TARGET)                                                       \
    X(RLWIMI, "rlwimi", RLWIMI, RLWINM)                                        \
    X(RLWINM, "rlwinm", RLWINM, RLWINM)                                        \
    X(RLWNM, "rlwnm", RLWNM, RLWNM)                                            \
    X(ORI, "ori", ORI, A_S_UI)                                                 \
    X(ORIS, "oris", ORI, A_S_UI)                                               \
    X(XORI, "xori", XORI, A_S_UI)                                              \
    X(XORIS, "xoris", XORI, A_S_UI)                                            \
    X(ANDI_, "andi.", ANDI, A_S_UI)                                            \
    X(ANDIS_, "andis.", ANDI, A_S_UI)                                          \
    X(LWZ, "lwz", LOAD, D_MEM)                                                 \
    X(LWZU, "lwzu", LOAD, D_MEM)                                               \
    X(LBZ, "lbz", LOAD, D_MEM)                                                 \
    X(LBZU, "lbzu", LOAD, D_MEM)                                               \
    X(STW, "stw", STORE, D_MEM)                                                \
    X(STWU, "stwu", STORE, D_MEM)                                              \
    X(STB, "stb", STORE, D_MEM)                                                \
    X(STBU, "stbu", STORE, D_MEM)                                              \
    X(LHZ, "lhz", LOAD, D_MEM)                                                 \
    X(LHZU, "lhzu", LOAD, D_MEM)                                               \
    X(LHA, "lha", LOAD, D_MEM)                                                 \
    X(LHAU, "lhau", LOAD, D_MEM)                                               \
    X(STH, "sth", STORE, D_MEM)                                                \
    X(STHU, "sthu", STORE, D_MEM)                                              \
    X(LMW, "lmw", LMW, D_MEM)                                                  \
    X(STMW, "stmw", STMW, D_MEM)                                               \
    /* Book E, opcode 19 (non-VLE pages only) */                               \
    X(MCRF, "mcrf", MCRF, CRF_CRF)                                             \
    X(BCLR, "bclr", BCLR, BO_BI)                                               \
    X(BCCTR, "bcctr", BCCTR, BO_BI)                                            \
    X(CRAND, "crand", CRAND, CRB3)                                             \
    X(CRANDC, "crandc", CRANDC, CRB3)                                          \
    X(CREQV, "creqv", CREQV, CRB3)                                             \
    X(CRNAND, "crnand", CRNAND, CRB3)                                          \
    X(CRNOR, "crnor", CRNOR, CRB3)                                             \
    X(CROR, "cror", CROR, CRB3)                                                \
    X(CRORC, "crorc", CRORC, CRB3)                                             \
    X(CRXOR, "crxor", CRXOR, CRB3)                                             \
    X(RFI, "rfi", RFI, NONE)                                                   \
    X(RFCI, "rfci", RFCI, NONE)                                                \
    X(RFDI, "rfdi", RFDI, NONE)                                                \
    X(RFMCI, "rfmci", RFMCI, NONE)                                             \
    X(ISYNC, "isync", NOP, NONE)                                               \
    /* Book E, opcode 31: both kinds of page */                                \
    X(CMP, "cmp", CMP, CRF_A_B)                                                \
    X(CMPL, "cmpl", CMPL, CRF_A_B)                                             \
    X(TW, "tw", TW, TO_A_B)                                                    \
    X(SUBFC, "subfc", SUBFC, D_A_B)                                            \
    X(ADDC, "addc", ADDC, D_A_B)                                               \
    X(MULHWU, "mulhwu", MULHWU, D_A_B)                                         \
    X(MFCR, "mfcr", MFCR, D)                                                   \
    X(LWARX, "lwarx", LARX, D_A_B)                                             \
    X(LBARX, "lbarx", LARX, D_A_B)                                             \
    X(LHARX, "lharx", LARX, D_A_B)                                             \
    X(LWZX, "lwzx", LOAD, D_A_B)                                               \
    X(SLW, "slw", SLW, A_S_B)                                                  \
    X(CNTLZW, "cntlzw", CNTLZW, A_S)                                           \
    X(AND, "and", AND, A_S_B)                                                  \
    X(SUBF, "subf", SUBF, D_A_B)                                               \
    X(LWZUX, "lwzux", LOAD, D_A_B)                                             \
    X(ANDC, "andc", ANDC, A_S_B)                                               \
    X(MULHW, "mulhw", MULHW, D_A_B)                                            \
    X(MFMSR, "mfmsr", MFMSR, D)                                                \
    X(LBZX, "lbzx", LOAD, D_A_B)                                               \
    X(NEG, "neg", NEG, D_A)                                                    \
    X(LBZUX, "lbzux", LOAD, D_A_B)                                             \
    X(NOR, "nor", NOR, A_S_B)                                                  \
    X(WRTEE, "wrtee", WRTEE, S)                                                \
    X(SUBFE, "subfe", SUBFE, D_A_B)                                            \
    X(ADDE, "adde", ADDE, D_A_B)                                               \
    X(MTCRF, "mtcrf", MTCRF, FXM_S)                                            \
    X(MTMSR, "mtmsr", MTMSR, S)                                                \
    X(STWCX_, "stwcx.", STCX, D_A_B)                                           \
    X(STBCX_, "stbcx.", STCX, D_A_B)                                           \
    X(STHCX_, "sthcx.", STCX, D_A_B)                                           \
    X(STWX, "stwx", STORE, D_A_B)                                              \
    X(WRTEEI, "wrteei", WRTEE, E)                                              \
    X(STWUX, "stwux", STORE, D_A_B)                                            \
    X(SUBFZE, "subfze", SUBFZE, D_A)                                           \
    X(ADDZE, "addze", ADDZE, D_A)                                              \
    X(STBX, "stbx", STORE, D_A_B)                                              \
    X(SUBFME, "subfme", SUBFME, D_A)                                           \
    X(ADDME, "addme", ADDME, D_A)                                              \
    X(MULLW, "mullw", MULLW, D_A_B)                                            \
    X(STBUX, "stbux", STORE, D_A_B)                                            \
    X(ADD, "add", ADD, D_A_B)                                                  \
    X(LHZX, "lhzx", LOAD, D_A_B)                                               \
    X(EQV, "eqv", EQV, A_S_B)                                                  \
    X(LHZUX, "lhzux", LOAD, D_A_B)                                             \
    X(XOR, "xor", XOR, A_S_B)                                                  \
    X(MFSPR, "mfspr", MFSPR, D_SPR)                                            \
    X(LHAX, "lhax", LOAD, D_A_B)                                               \
    X(LHAUX, "lhaux", LOAD, D_A_B)                                             \
    X(STHX, "sthx", STORE, D_A_B)                                              \
    X(ORC, "orc", ORC, A_S_B)                                                  \
    X(STHUX, "sthux", STORE, D_A_B)                                            \
    X(OR, "or", OR, A_S_B)                                                     \
    X(DIVWU, "divwu", DIVWU, D_A_B)                                            \
    X(MTSPR, "mtspr", MTSPR, SPR_S)                                            \
    X(NAND, "nand", NAND, A_S_B)                                               \
    X(DIVW, "divw", DIVW, D_A_B)                                               \
    X(MCRXR, "mcrxr", MCRXR, CRF)                                              \
    X(LWBRX, "lwbrx", LOAD, D_A_B)                                             \
    X(SRW, "srw", SRW, A_S_B)                                                  \
    X(LHBRX, "lhbrx", LOAD, D_A_B)                                             \
    X(SRAW, "sraw", SRAW, A_S_B)                                               \
    X(SRAWI, "srawi", SRAWI, A_S_SH)                                           \
    X(MBAR, "mbar", NOP, MO)                                                   \
    X(STHBRX, "sthbrx", STORE, D_A_B)                                          \
    X(EXTSH, "extsh", EXTSH, A_S)                                              \
    X(EXTSB, "extsb", EXTSB, A_S)                                              \
    X(STWBRX, "stwbrx", STORE, D_A_B)                                          \
    X(MSYNC, "msync", NOP, NONE)                                               \
    X(ISEL, "isel", ISEL, ISEL)                                                \
    X(WAIT, "wait", WAIT, NONE)                                                \
    X(DCBF, "dcbf", NOP, A_B)                                                  \
    X(DCBST, "dcbst", NOP, A_B)                                                \
    X(DCBT, "dcbt", NOP, CT_A_B)                                               \
    X(DCBTST, "dcbtst", NOP, CT_A_B)                                           \
    X(DCBZ, "dcbz", DCBZ, A_B)                                                 \
    X(DCBI, "dcbi", PRIV_NOP, A_B)                                             \
    X(ICBI, "icbi", NOP, A_B)                                                  \
    X(DCBA, "dcba", NOP, A_B)                                                  \
    X(ICBT, "icbt", NOP, CT_A_B)                                               \
    /* the embedded floating point unit */                                     \
    X(EFSADD, "efsadd", EFSADD, D_A_B)                                         \
    X(EFSSUB, "efssub", EFSSUB, D_A_B)                                         \
    X(EFSMUL, "efsmul", EFSMUL, D_A_B)                                         \
    X(EFSDIV, "efsdiv", EFSDIV, D_A_B)                                         \
    X(EFSMADD, "efsmadd", EFSMADD, D_A_B)                                      \
    X(EFSMSUB, "efsmsub", EFSMSUB, D_A_B)                                      \
    X(EFSNMADD, "efsnmadd", EFSNMADD, D_A_B)                                   \
    X(EFSNMSUB, "efsnmsub", EFSNMSUB, D_A_B)                                   \
    X(EFSABS, "efsabs", EFSABS, D_A)                                           \
    X(EFSNABS, "efsnabs", EFSNABS, D_A)                                        \
    X(EFSNEG, "efsneg", EFSNEG, D_A)                                           \
    X(EFSSQRT, "efssqrt", EFSSQRT, D_A)                                        \
    X(EFSMAX, "efsmax", EFSMAX, D_A_B)                                         \
    X(EFSMIN, "efsmin", EFSMIN, D_A_B)                                         \
    X(EFSCMPGT, "efscmpgt", EFSCMPGT, CRF_A_B)                                 \
    X(EFSCMPLT, "efscmplt", EFSCMPLT, CRF_A_B)                                 \
    X(EFSCMPEQ, "efscmpeq", EFSCMPEQ, CRF_A_B)                                 \
    X(EFSTSTGT, "efststgt", EFSTSTGT, CRF_A_B)                                 \
    X(EFSTSTLT, "efststlt", EFSTSTLT, CRF_A_B)                                 \
    X(EFSTSTEQ, "efststeq", EFSTSTEQ, CRF_A_B)                                 \
    X(EFSCFUI, "efscfui", EFSCFUI, D_B)                                        \
    X(EFSCFSI, "efscfsi", EFSCFSI, D_B)                                        \
    X(EFSCFUF, "efscfuf", EFSCFUF, D_B)                                        \
    X(EFSCFSF, "efscfsf", EFSCFSF, D_B)                                        \
    X(EFSCFH, "efscfh", EFSCFH, D_B)                                           \
    X(EFSCTUI, "efsctui", EFSCTUI, D_B)                                        \
    X(EFSCTSI, "efsctsi", EFSCTSI, D_B)                                        \
    X(EFSCTUF, "efsctuf", EFSCTUF, D_B)                                        \
    X(EFSCTSF, "efsctsf", EFSCTSF, D_B)                                        \
    X(EFSCTUIZ, "efsctuiz", EFSCTUIZ, D_B)                                     \
    X(EFSCTSIZ, "efsctsiz", EFSCTSIZ, D_B)                                     \
    X(EFSCTH, "efscth", EFSCTH, D_B)                                           \
    X(SPE, "spe", SPE, RAW)

typedef enum {
#define X(id, name, sem, fmt) PPC_M_##id,
    PPC_MNEMONICS(X)
#undef X
    PPC_M_COUNT
} ppc_mn_t;

/*
 * One decoded instruction. Field names are the architecture's: `rd` is
 * rD, rS or rT -- the field at bits 6:10 -- whatever the instruction
 * calls it, and likewise `ra` and `rb`. The VLE register mappings (se_
 * forms' four-bit fields, the alternate registers) are already applied:
 * these are real register numbers.
 */
typedef struct ppc_insn {
    uint16_t id;      /* ppc_mn_t                                       */
    uint8_t sem;      /* ppc_sem_t                                      */
    uint8_t len;      /* 2 or 4                                         */
    uint8_t rd, ra, rb;
    uint8_t rc;       /* record CR0                                     */
    uint8_t oe;       /* record XER[OV]                                 */
    uint8_t crf;      /* a CR field: compare target, mcrf/mcrxr target  */
    uint8_t crs;      /* mcrf's source field                            */
    uint8_t bo, bi;   /* branch condition, in Book E's encoding         */
    uint8_t aa, lk;
    uint8_t sh, mb, me;
    uint8_t size;     /* access width in bytes                          */
    uint8_t sext;     /* a halfword load that sign-extends              */
    uint8_t rev;      /* byte-reversed access                           */
    uint8_t upd;      /* the base register is written back              */
    uint8_t idx;      /* indexed: rA|0 + rB                             */
    uint8_t ra0;      /* rA == 0 means the value 0, not r0              */
    uint32_t imm;     /* immediate, displacement, offset, SPR, FXM, TO  */
    /*
     * The immediate as the assembler writes it, where that is not `imm`:
     * se_subi's 1..32 against the -1..-32 it adds, e_lis's 16 bits
     * against the word it loads, se_bclri's bit number against its mask.
     */
    uint32_t ui;
} ppc_insn_t;

/*
 * Decode one instruction. `insn` is the first halfword in bits 31:16 and
 * the second in 15:0 -- the order it is in memory. `vle` says which of
 * the two encodings the page holds; in a VLE page `len` comes from
 * ppc_vle_len, in a Book E page it is always 4. Never fails: an
 * encoding this core does not have comes back as PPC_S_ILLEGAL.
 */
void ppc_decode(uint32_t insn, unsigned len, bool vle, ppc_insn_t *out);

const char *ppc_mn_name(uint32_t id);
uint32_t ppc_mn_format(uint32_t id);
uint32_t ppc_mn_sem(uint32_t id);

#ifdef __cplusplus
}
#endif

#endif /* PPC_DECODE_H */
