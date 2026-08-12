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

/*
 * mov reg, [rsp + disp32] and its inverse -- the temp frame.
 *
 * rsp as a base needs the SIB byte, and a register above r7 needs REX.B,
 * which is why these are here rather than open-coded at the call site:
 * they were, and `mov r12d, [rsp+n]` came out as `mov esp, [rsp+n]`. The
 * ModRM reg field is three bits, so the fourth silently went nowhere and
 * the block wrote its stack pointer with a guest value. It did not fault
 * until the return.
 */
void x86_ld_rsp(int reg, uint32_t disp)
{
    emit_rex(0u, (unsigned)reg, 0u);
    emu_jit_emit8(0x8B);
    emu_jit_emit8((uint8_t)(0x84u | (((unsigned)reg & 7u) << 3)));
    emu_jit_emit8(0x24);
    emu_jit_emit32(disp);
}

void x86_st_rsp(int reg, uint32_t disp)
{
    emit_rex(0u, (unsigned)reg, 0u);
    emu_jit_emit8(0x89);
    emu_jit_emit8((uint8_t)(0x84u | (((unsigned)reg & 7u) << 3)));
    emu_jit_emit8(0x24);
    emu_jit_emit32(disp);
}

void x86_alu_rr(uint8_t op, int dst, int src)
{
    emit_rex(0u, (unsigned)src, (unsigned)dst);
    emu_jit_emit8(op);
    modrm_rr(src, dst);
}

/* The F7 group: /2 not, /3 neg, /4 mul, /5 imul. */
void x86_unary(unsigned ext, int reg)
{
    emit_rex(0u, 0u, (unsigned)reg);
    emu_jit_emit8(0xF7);
    modrm_rr((int)ext, reg);
}

void x86_bswap(int reg)
{
    emit_rex(0u, 0u, (unsigned)reg);
    emu_jit_emit8(0x0F);
    emu_jit_emit8((uint8_t)(0xC8u + ((unsigned)reg & 7u)));
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

/*
 * The registers a block may hand to the allocator.
 *
 * All four are callee-saved in System V, which is the property that
 * matters: a value in one of them survives a helper call with nothing
 * emitted around it. rbx and rbp are the other two and are already the
 * cpu pointer and the retired count, and rsp is the frame -- so this is
 * the whole of what is available, and why the allocator had to wait for
 * REX support before it could exist at all.
 */
const int x86_alloc_regs[X86_ALLOC_REGS] = {
    X86_R12, X86_R13, X86_R14, X86_R15
};

/* push/pop of a 64-bit register; the operand size is already 64. */
static void x86_push(int reg)
{
    if (reg >= 8) {
        emu_jit_emit8(0x41);                              /* REX.B        */
    }
    emu_jit_emit8((uint8_t)(0x50u + ((unsigned)reg & 7u)));
}

static void x86_pop(int reg)
{
    if (reg >= 8) {
        emu_jit_emit8(0x41);
    }
    emu_jit_emit8((uint8_t)(0x58u + ((unsigned)reg & 7u)));
}

/*
 * `nsaved` of x86_alloc_regs are pushed, lowest index first, and popped
 * in the mirror order.
 *
 * They land *after* the sub rsp, 8, which matters for alignment: an odd
 * number of them moves rsp 8 out of step with what a call needs, and it
 * is the frame reservation in emu_ir_lower that compensates. Splitting
 * that reasoning across two files is unfortunate; putting the pushes
 * before the pad instead would mean this function had to know the frame
 * size, which is worse.
 */
/* ------------------------------------------------------------------ */
/* SSE, for the floating-point class                                   */
/* ------------------------------------------------------------------ */

/*
 * Only xmm0 and xmm1 are used, so the SSE half never needs REX -- but
 * the *other* operand of a movd is a general register and may be r12 or
 * above, which does. The 0x66/0xF3 mandatory prefix comes before REX,
 * which is the one ordering rule these encodings can get wrong without
 * assembling into nothing: 66 REX 0F is a movd, REX 66 0F is a REX
 * prefix followed by an instruction that ignores it.
 */
static void sse_rr(uint8_t pfx, uint8_t op2, unsigned reg, unsigned rm)
{
    if (pfx != 0u) {
        emu_jit_emit8(pfx);
    }
    emit_rex(0u, reg, rm);
    emu_jit_emit8(0x0F);
    emu_jit_emit8(op2);
    emu_jit_emit8((uint8_t)(0xC0u | ((reg & 7u) << 3) | (rm & 7u)));
}

void x86_movd_to_xmm(int xmm, int gpr)
{
    sse_rr(0x66u, 0x6Eu, (unsigned)xmm, (unsigned)gpr);
}

void x86_movd_from_xmm(int gpr, int xmm)
{
    sse_rr(0x66u, 0x7Eu, (unsigned)xmm, (unsigned)gpr);
}

/* The scalar-single arithmetic group: F3 0F xx, xmm to xmm. */
void x86_ss_op(uint8_t op2, int dst, int src)
{
    sse_rr(0xF3u, op2, (unsigned)dst, (unsigned)src);
}

/* ucomiss: no prefix, and it sets ZF/PF/CF rather than a result. */
void x86_ucomiss(int a, int b)
{
    sse_rr(0u, 0x2Eu, (unsigned)a, (unsigned)b);
}

void x86_cvt_to_i(int gpr, int xmm, bool truncate)
{
    sse_rr(0xF3u, truncate ? 0x2Cu : 0x2Du, (unsigned)gpr, (unsigned)xmm);
}

void x86_cvt_from_i(int xmm, int gpr)
{
    sse_rr(0xF3u, 0x2Au, (unsigned)xmm, (unsigned)gpr);
}

/* stmxcsr / ldmxcsr [rsp + disp32] -- 0F AE /3 and /2. */
static void mxcsr_rsp(unsigned ext, uint32_t disp)
{
    emu_jit_emit8(0x0F);
    emu_jit_emit8(0xAE);
    emu_jit_emit8((uint8_t)(0x84u | (ext << 3)));
    emu_jit_emit8(0x24);
    emu_jit_emit32(disp);
}

void x86_stmxcsr(uint32_t disp) { mxcsr_rsp(3u, disp); }
void x86_ldmxcsr(uint32_t disp) { mxcsr_rsp(2u, disp); }

/* movzx r32, byte [base + index] -- the flag-map lookup. */
void x86_movzx8_idx(int dst, int base, int index)
{
    emit_rex(0u, (unsigned)dst, (unsigned)base);
    emu_jit_emit8(0x0F);
    emu_jit_emit8(0xB6);
    emu_jit_emit8((uint8_t)(((unsigned)dst & 7u) << 3 | 0x04u));
    emu_jit_emit8((uint8_t)((((unsigned)index & 7u) << 3) |
                            ((unsigned)base & 7u)));
}

void x86_prologue(uint32_t nsaved)
{
    emu_jit_emit8(0x53);                                  /* push rbx     */
    emu_jit_emit8(0x55);                                  /* push rbp     */
    emu_jit_emit8(0x48); emu_jit_emit8(0x83);
    emu_jit_emit8(0xEC); emu_jit_emit8(0x08);             /* sub rsp, 8   */
    emu_jit_emit8(0x48); emu_jit_emit8(0x89);
    emu_jit_emit8(0xFB);                                  /* mov rbx, rdi */
    emu_jit_emit8(0x31); emu_jit_emit8(0xED);             /* xor ebp, ebp */

    for (uint32_t i = 0; i < nsaved && i < X86_ALLOC_REGS; i++) {
        x86_push(x86_alloc_regs[i]);
    }
}

void x86_epilogue(uint32_t nsaved)
{
    for (uint32_t i = (nsaved < X86_ALLOC_REGS) ? nsaved : X86_ALLOC_REGS;
         i-- > 0;) {
        x86_pop(x86_alloc_regs[i]);
    }
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
