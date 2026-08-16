/* SPDX-License-Identifier: Apache-2.0 */
/*
 * g4mh_decode.h - RH850 instruction formats.
 *
 * RH850 is a variable-length encoding: 16, 32 or 48 bits. Everything else
 * in the frontend depends on getting the length right, because the length
 * is what advances the pc -- a wrong answer does not produce a wrong
 * result, it desynchronises the instruction stream and every instruction
 * after it is garbage. So length classification is separated out here,
 * where it can be tested on its own, rather than falling out of the
 * execute switch.
 *
 * The first halfword decides:
 *
 *   bits[10:5] < 0x30      16-bit   formats I, II, III, IV
 *   bits[10:5] in 0x30..0x3F, except the two cases below
 *                          32-bit   formats V, VI, VII, VIII, IX, X, XI
 *   bits[15:5] == 0x0617 or bits[15:5] == 0x061F
 *                          48-bit   MOV imm32 / JMP disp32 / JR disp32
 *
 * The 16-bit range is not simply "low opcode": formats III and IV live
 * inside it at bits[10:7] rather than bits[10:5], which is why the test
 * below is written on the 6-bit field and the sub-formats are separated
 * afterwards.
 */
#ifndef G4MH_G4MH_DECODE_H
#define G4MH_G4MH_DECODE_H

#include "emu/emu_types.h"

#include "g4mh_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Field accessors                                                     */
/* ------------------------------------------------------------------ */

/*
 * The first halfword of every encoding is laid out the same way:
 *
 *   15..11  reg2   destination, or the second source
 *   10..5   opcode
 *    4..0   reg1   first source, or a 5-bit immediate
 *
 * Formats III and IV overlay this: their opcode is bits[10:7] and the
 * remaining bits carry a displacement.
 */
static EMU_ALWAYS_INLINE uint32_t g4mh_reg1(uint32_t w) { return w & 0x1Fu; }
static EMU_ALWAYS_INLINE uint32_t g4mh_reg2(uint32_t w) { return (w >> 11) & 0x1Fu; }
static EMU_ALWAYS_INLINE uint32_t g4mh_op6(uint32_t w)  { return (w >> 5) & 0x3Fu; }
static EMU_ALWAYS_INLINE uint32_t g4mh_op4(uint32_t w)  { return (w >> 7) & 0x0Fu; }

/* Format II's 5-bit immediate is sign-extended; the shift forms' is not. */
static EMU_ALWAYS_INLINE int32_t g4mh_imm5(uint32_t w)
{
    return emu_sext(w & 0x1Fu, 5);
}

/* ------------------------------------------------------------------ */
/* Instruction length                                                  */
/* ------------------------------------------------------------------ */

/*
 * Length, in two stages, because RH850 does not always put the answer in
 * the first halfword.
 *
 * `g4mh_insn_len` reports 2 or 4 from the first halfword alone: how much
 * has to be fetched before the question can be settled. When it says 4,
 * `g4mh_insn_is_48` then decides whether a third halfword follows. Doing
 * it in one step would mean speculatively fetching a halfword past a
 * 16-bit instruction, which can fault at the end of a mapped region.
 *
 * The 48-bit forms all have reg2 == 0 -- the architecture reuses that
 * field as an opcode extension throughout, which is also why `MOV imm32`
 * shares the MOVEA slot and `JR` shares JARL's. Getting this wrong does
 * not produce a wrong result, it desynchronises the instruction stream.
 */
unsigned g4mh_insn_len(uint16_t w);
bool g4mh_insn_is_48(uint16_t w0, uint16_t w1);

/*
 * And whether a *fourth* follows, which only PREPARE list12, imm5,
 * imm32 does -- the ISA's one 64-bit encoding. Asked only after
 * g4mh_insn_is_48 has said yes, because every 64-bit form is also a
 * 48-bit one under that function's question ("is there a third").
 */
bool g4mh_insn_is_64(uint16_t w0, uint16_t w1);

/* True if the first halfword starts a 16-bit instruction. */
static EMU_ALWAYS_INLINE bool g4mh_is_16bit(uint16_t w)
{
    return g4mh_op6(w) < 0x30u;
}

/* ------------------------------------------------------------------ */
/* Condition codes                                                     */
/* ------------------------------------------------------------------ */

/*
 * The 4-bit condition field shared by Bcond, SETF and CMOV. Evaluated
 * against PSW; returns true when the branch is taken.
 */
bool g4mh_cond(uint32_t cond, uint32_t psw);

/* Mnemonic for a condition code ("v", "l", "z", ... "sa"). */
const char *g4mh_cond_name(uint32_t cond);

/* ABI register name ("zero", "sp", "gp", "ep", "lp", "r7", ...). */
const char *g4mh_reg_name(unsigned r);

/* Name of a system register, or NULL if this implementation has no name
 * for that (bank, reg) pair. */
const char *g4mh_sr_name(unsigned bank, unsigned reg);

#ifdef __cplusplus
}
#endif

#endif /* G4MH_G4MH_DECODE_H */
