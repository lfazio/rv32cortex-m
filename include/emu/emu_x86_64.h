/* SPDX-License-Identifier: Apache-2.0 */
/*
 * emu_x86_64.h - x86-64 instruction encoding, for any frontend.
 *
 * The third axis of a JIT, after "which guest ISA" and "the framework":
 * how to spell an instruction on this host. Nothing here knows what is
 * being translated, which is what lets the RV32 and G4MH backends share
 * it -- they emit the same `mov`, `add` and `jcc`, and only differ in
 * which guest register those refer to.
 *
 * Encodings follow Intel SDM Vol 2A/2B (docs/x86_64/). All sixteen
 * registers are encodable: the emitters add a REX prefix when, and only
 * when, one is needed. A redundant 0x40 is legal but costs a byte on
 * every instruction, and on the target that shares its code cache with
 * the guest, bytes are the currency.
 *
 * The System V ABI is the other thing this file owns, because getting it
 * wrong does not fault in the emitted code -- it faults much later inside
 * whatever libc routine a helper eventually reaches and uses an aligned
 * SSE store.
 */
#ifndef EMU_X86_64_H
#define EMU_X86_64_H

#include "emu_jit.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    X86_EAX = 0, X86_ECX = 1, X86_EDX = 2, X86_EBX = 3,
    X86_ESP = 4, X86_EBP = 5, X86_ESI = 6, X86_EDI = 7,
    /*
     * The extended registers. Reaching these costs a REX prefix, which
     * the encoders emit only when one is actually required -- but they
     * are the only registers a future allocator can use here, since rbx,
     * rbp and rsp are all spoken for.
     */
    X86_R8  = 8, X86_R9  = 9, X86_R10 = 10, X86_R11 = 11,
    X86_R12 = 12, X86_R13 = 13, X86_R14 = 14, X86_R15 = 15
};

/*
 * Where a translated block keeps its two live values.
 *
 * rbx holds the emu_cpu_t pointer and rbp the retired count, and both are
 * callee-saved in System V -- which is the whole reason they were chosen:
 * they survive a call to a helper without the block saving them.
 */
#define X86_CPU  X86_EBX
#define X86_CNT  X86_EBP

/* ALU opcodes for the r/m32, r32 direction (dst op= src). */
#define X86_ADD 0x01u
#define X86_OR  0x09u
#define X86_AND 0x21u
#define X86_SUB 0x29u
#define X86_XOR 0x31u
#define X86_CMP 0x39u
#define X86_TEST 0x85u

/* Shift extensions for the /n field: 4 shl, 5 shr, 7 sar. */
#define X86_SHL 4u
#define X86_SHR 5u
#define X86_SAR 7u

/* Condition codes, as the second byte of a 0x0F-prefixed jcc/setcc. */
#define X86_CC_E   0x84u
#define X86_CC_NE  0x85u
#define X86_CC_L   0x8Cu
#define X86_CC_GE  0x8Du
#define X86_CC_B   0x82u
#define X86_CC_AE  0x83u
#define X86_CC_LE  0x8Eu
#define X86_CC_G   0x8Fu
#define X86_CC_BE  0x86u
#define X86_CC_A   0x87u
/*
 * setcc from the matching jcc: `jcc rel32` is 0F 80+cc and `setcc r/m8` is
 * 0F 90+cc, so the conversion *adds* 0x10. Subtracting instead turns setl
 * into 0F 7C, which is not a jump or a set but an SSE encoding -- the
 * emitted block dies on SIGILL rather than computing the wrong answer,
 * which at least fails loudly.
 */
#define X86_SET(cc) (uint8_t)((cc) + 0x10u)

/* --- memory, relative to the cpu pointer -------------------------- */

void x86_ld_cpu(int dst, uint32_t disp);    /* mov r32, [rbx + disp] */
void x86_st_cpu(int src, uint32_t disp);    /* mov [rbx + disp], r32 */

/* --- registers and immediates ------------------------------------- */

void x86_mov_rr(int dst, int src);
void x86_mov_imm32(int dst, uint32_t imm);
void x86_mov_imm64(int dst, uint64_t imm);  /* helper addresses only */

void x86_alu_rr(uint8_t op, int dst, int src);
void x86_add_imm8(int dst, int8_t imm);
void x86_and_imm8(int dst, int8_t imm);

void x86_shift_cl(int dst, unsigned ext);
void x86_shift_imm(int dst, unsigned ext, uint32_t amount);

/*
 * setcc al; movzx eax, al -- a flag turned back into a 0 or 1 in eax.
 * The shortest way to do it that does not branch.
 */
void x86_setcc_eax(uint8_t cc);

/* Sign- and zero-extend the low byte or halfword of a register. */
void x86_movsx8(int dst, int src);
void x86_movsx16(int dst, int src);
void x86_movzx8(int dst, int src);
void x86_movzx16(int dst, int src);

/* --- control flow -------------------------------------------------- */

/*
 * Emit a jump with a displacement to be filled in later, and return the
 * address of the four-byte slot. Pair with x86_patch_rel32 once the target
 * is known.
 */
uint8_t *x86_jcc32(uint8_t cc);
uint8_t *x86_jmp32(void);
void     x86_patch_rel32(uint8_t *slot, const uint8_t *target);

/* call rax, having loaded it with x86_mov_imm64. */
void x86_call_rax(void);

/* --- block frame --------------------------------------------------- */

/*
 * A block is uint32_t (*)(emu_cpu_t *) and returns the guest instructions
 * it retired -- not always the number it contains, since a trapping
 * helper returns early.
 *
 * The stack adjustment is not padding: System V wants rsp 16-byte aligned
 * at a call, entry leaves it 8 past, and two pushes bring it back to 8, so
 * one more 8 is needed before any helper call can be made.
 */
void x86_prologue(void);
void x86_epilogue(void);

/* Add one to the retired count. */
void x86_count_one(void);

#ifdef __cplusplus
}
#endif

#endif /* EMU_X86_64_H */
