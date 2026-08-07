/* SPDX-License-Identifier: Apache-2.0 */
/*
 * emu_x86_64.c - x86-64 encoding. See emu_x86_64.h.
 */

#include "emu/emu_x86_64.h"

#include <string.h>

#if defined(__x86_64__)

/*
 * REX, emitted only when it is needed.
 *
 * 0100WRXB: W widens the operation to 64 bits, R extends the ModRM reg
 * field and B the rm field. Omitting it when no bit is set matters --
 * a redundant 0x40 is legal but costs a byte on every instruction, and
 * on the target that shares a code cache with the guest, bytes are the
 * currency.
 *
 * One trap this opens: with a REX prefix present, encodings 4..7 of a
 * *byte* operand mean SPL/BPL/SIL/DIL rather than AH/CH/DH/BH. Nothing
 * here takes a byte operand above al, so the question does not arise --
 * but it is the reason the byte helpers below are written in terms of
 * eax specifically rather than a parameter.
 */
static void emit_rex(unsigned w, unsigned reg, unsigned rm)
{
    const unsigned bits = (w != 0u ? 8u : 0u) |
                          (((unsigned)reg >> 3) & 1u) << 2 |
                          (((unsigned)rm >> 3) & 1u);

    if (bits != 0u) {
        emu_jit_emit8((uint8_t)(0x40u | bits));
    }
}

/* ModRM for [rbx + disp32].
 *
 * disp32 uniformly rather than the shorter disp8 form: a guest register
 * file fits in disp8, but pc and everything past it does not, and one
 * encoding for both is worth three bytes a reference. */
static void modrm_cpu(int reg, uint32_t disp)
{
    emu_jit_emit8((uint8_t)(0x80u | (((unsigned)reg & 7u) << 3) |
                            (X86_CPU & 7u)));
    emu_jit_emit32(disp);
}

/* mod=11: register-direct. */
static void modrm_rr(int reg, int rm)
{
    emu_jit_emit8((uint8_t)(0xC0u | (((unsigned)reg & 7u) << 3) |
                            ((unsigned)rm & 7u)));
}

void x86_ld_cpu(int dst, uint32_t disp)
{
    emit_rex(0u, (unsigned)dst, X86_CPU);
    emu_jit_emit8(0x8B);
    modrm_cpu(dst, disp);
}

void x86_st_cpu(int src, uint32_t disp)
{
    emit_rex(0u, (unsigned)src, X86_CPU);
    emu_jit_emit8(0x89);
    modrm_cpu(src, disp);
}

void x86_mov_rr(int dst, int src)
{
    emit_rex(0u, (unsigned)src, (unsigned)dst);
    emu_jit_emit8(0x89);
    modrm_rr(src, dst);
}

void x86_mov_imm32(int dst, uint32_t imm)
{
    emit_rex(0u, 0u, (unsigned)dst);
    emu_jit_emit8((uint8_t)(0xB8u + ((unsigned)dst & 7u)));
    emu_jit_emit32(imm);
}

void x86_mov_imm64(int dst, uint64_t imm)
{
    emit_rex(1u, 0u, (unsigned)dst);          /* REX.W, and .B if needed */
    emu_jit_emit8((uint8_t)(0xB8u + ((unsigned)dst & 7u)));
    emu_jit_emit64(imm);
}

/*
 * dst <op>= [rsp + disp] -- an ALU instruction reading its right operand
 * straight out of a frame slot.
 *
 * Every IR value lives in a slot, so the alternative is a `mov` into a
 * scratch register followed by the operation: two instructions and four
 * more bytes on *every* binary op with two temps. On the target that
 * shares its code cache with the guest, that is the difference between
 * a working set that fits and one that compacts.
 *
 * The op constants name the `r/m32, r32` direction, where the
 * destination is the memory side. Setting bit 1 selects the `r32, r/m32`
 * form, which is the one that reads memory and writes the register --
 * so `add` 0x01 becomes 0x03, `sub` 0x29 becomes 0x2B, and so on
 * uniformly.
 */
void x86_alu_slot(uint8_t op, int dst, uint32_t disp)
{
    emit_rex(0u, (unsigned)dst, 0u);
    emu_jit_emit8((uint8_t)(op | 0x02u));
    emu_jit_emit8((uint8_t)(0x84u | (((unsigned)dst & 7u) << 3)));
    emu_jit_emit8(0x24);                       /* SIB: [rsp] */
    emu_jit_emit32(disp);
}

void x86_alu_rr(uint8_t op, int dst, int src)
{
    emit_rex(0u, (unsigned)src, (unsigned)dst);
    emu_jit_emit8(op);
    modrm_rr(src, dst);
}

void x86_add_imm8(int dst, int8_t imm)
{
    emit_rex(0u, 0u, (unsigned)dst);
    emu_jit_emit8(0x83);
    modrm_rr(0, dst);
    emu_jit_emit8((uint8_t)imm);
}

void x86_and_imm8(int dst, int8_t imm)
{
    emit_rex(0u, 0u, (unsigned)dst);
    emu_jit_emit8(0x83);
    modrm_rr(4, dst);
    emu_jit_emit8((uint8_t)imm);
}

void x86_shift_cl(int dst, unsigned ext)
{
    emit_rex(0u, 0u, (unsigned)dst);
    emu_jit_emit8(0xD3);
    emu_jit_emit8((uint8_t)(0xC0u | (ext << 3) | ((unsigned)dst & 7u)));
}

void x86_shift_imm(int dst, unsigned ext, uint32_t amount)
{
    emit_rex(0u, 0u, (unsigned)dst);
    emu_jit_emit8(0xC1);
    emu_jit_emit8((uint8_t)(0xC0u | (ext << 3) | ((unsigned)dst & 7u)));
    emu_jit_emit8((uint8_t)(amount & 31u));
}

void x86_setcc_eax(uint8_t cc)
{
    emu_jit_emit8(0x0F);
    emu_jit_emit8(X86_SET(cc));
    emu_jit_emit8(0xC0);                      /* setcc al       */
    emu_jit_emit8(0x0F);
    emu_jit_emit8(0xB6);
    emu_jit_emit8(0xC0);                      /* movzx eax, al  */
}

static void ext_rr(uint8_t op2, int dst, int src)
{
    emit_rex(0u, (unsigned)dst, (unsigned)src);
    emu_jit_emit8(0x0F);
    emu_jit_emit8(op2);
    modrm_rr(dst, src);
}

void x86_movsx8(int dst, int src)  { ext_rr(0xBE, dst, src); }
void x86_movsx16(int dst, int src) { ext_rr(0xBF, dst, src); }
void x86_movzx8(int dst, int src)  { ext_rr(0xB6, dst, src); }
void x86_movzx16(int dst, int src) { ext_rr(0xB7, dst, src); }

uint8_t *x86_jcc32(uint8_t cc)
{
    emu_jit_emit8(0x0F);
    emu_jit_emit8(cc);
    uint8_t *const slot = emu_jit_here();
    emu_jit_emit32(0u);
    return slot;
}

uint8_t *x86_jmp32(void)
{
    emu_jit_emit8(0xE9);
    uint8_t *const slot = emu_jit_here();
    emu_jit_emit32(0u);
    return slot;
}

void x86_patch_rel32(uint8_t *slot, const uint8_t *target)
{
    /*
     * Guarded because the slot may be past the end of an overflowed
     * buffer: emu_jit_here() keeps returning the end pointer once emitting
     * has stopped, and writing through it would corrupt whatever follows.
     * The block is discarded either way, so the patch simply does not
     * happen.
     */
    if (slot == NULL || emu_jit_overflowed()) {
        return;
    }
    const int32_t rel = (int32_t)(target - (slot + 4));
    memcpy(slot, &rel, sizeof(rel));
}

void x86_call_rax(void)
{
    emu_jit_emit8(0xFF);
    emu_jit_emit8(0xD0);
}

void x86_prologue(void)
{
    emu_jit_emit8(0x53);                                  /* push rbx     */
    emu_jit_emit8(0x55);                                  /* push rbp     */
    emu_jit_emit8(0x48); emu_jit_emit8(0x83);
    emu_jit_emit8(0xEC); emu_jit_emit8(0x08);             /* sub rsp, 8   */
    emu_jit_emit8(0x48); emu_jit_emit8(0x89);
    emu_jit_emit8(0xFB);                                  /* mov rbx, rdi */
    emu_jit_emit8(0x31); emu_jit_emit8(0xED);             /* xor ebp, ebp */
}

void x86_epilogue(void)
{
    emu_jit_emit8(0x89); emu_jit_emit8(0xE8);             /* mov eax, ebp */
    emu_jit_emit8(0x48); emu_jit_emit8(0x83);
    emu_jit_emit8(0xC4); emu_jit_emit8(0x08);             /* add rsp, 8   */
    emu_jit_emit8(0x5D);                                  /* pop rbp      */
    emu_jit_emit8(0x5B);                                  /* pop rbx      */
    emu_jit_emit8(0xC3);                                  /* ret          */
}

void x86_count_one(void)
{
    x86_add_imm8(X86_CNT, 1);
}

#endif /* __x86_64__ */
