/* SPDX-License-Identifier: Apache-2.0 */
/*
 * rv_jit_x86_64.c - x86-64 JIT backend.
 *
 * Same shape as the Thumb-2 backend and the same rv_backend_t interface:
 * basic blocks of guest code are translated into host machine code held in
 * a buffer, looked up by guest pc, and entered as ordinary C functions.
 *
 * Its reason for existing is not speed on a workstation. It is that the
 * Thumb-2 JIT cannot be exercised by any host test suite -- every JIT bug
 * this project has found was found by flashing a board and reading a UART,
 * and three of them were live for months first. An x86-64 JIT puts a
 * *translated* backend under the 274-test architecture suite and the 77
 * Berkeley tests, on every build, which is the coverage the ARM one can
 * never have.
 *
 * Encoding follows Intel SDM Vol 2A/2B (docs/x86_64/). Two things differ
 * from the ARM backend and both simplify it:
 *
 *   - x86 caches are coherent with respect to instruction fetch, so there
 *     is no clean-to-PoU and no I-cache invalidate. A jump is enough.
 *   - The immediate forms are unconstrained, so there is no equivalent of
 *     the Thumb-2 modified-immediate encoder and nothing has to be
 *     materialised through a constant pool.
 *
 * KNOWN DEFECT: rv32mi/scall and rv32ui/ma_data do not complete under this
 * backend -- see the README section of the same name. Both reach the
 * correct place with the correct result and then fail to make progress at
 * a rate anything like the interpreter's, and it has not been root-caused.
 * The architecture suite passes 274/274 here, so this is not a general
 * correctness problem, but it is unexplained and riscv-tests is
 * interpreter-only until it is understood.
 *
 * What is translated is the integer core: LUI, AUIPC, the OP-IMM and OP
 * groups, and loads and stores through a helper. Everything else ends the
 * block and is executed by the interpreter, which is the same policy the
 * ARM backend started from -- and, as recorded in CLAUDE.md, declining is
 * the expensive choice, so the set is meant to grow.
 */

#include "rv32/rv_jit.h"

#if RV_ENABLE_JIT && defined(RV_JIT_X86_64)

#include "rv32/rv_backend.h"
#include "rv32/rv_decode.h"
#include "rv32/rv_hart.h"

#include <stddef.h>
#include <string.h>
#include <sys/mman.h>

_Static_assert(offsetof(rv_hart_t, x) == 0,
               "translated code assumes hart->x is at offset 0");
_Static_assert(sizeof(((rv_hart_t *)0)->x) == 32u * 4u,
               "translated code assumes 32 32-bit guest registers");

#define HART_PC_OFF ((uint32_t)offsetof(rv_hart_t, pc))

/* ------------------------------------------------------------------ */
/* Host registers                                                      */
/* ------------------------------------------------------------------ */

/*
 * Only the low eight are used, so no instruction here needs a REX.R or
 * REX.B bit and every ModRM byte is one byte. rbx and rbp are callee-saved
 * in the System V ABI, which is what lets them survive a helper call.
 */
enum {
    EAX = 0, ECX = 1, EDX = 2, EBX = 3,
    ESP = 4, EBP = 5, ESI = 6, EDI = 7
};

#define REG_HART EBX   /* rv_hart_t *, live for the whole block */
#define REG_CNT  EBP   /* instructions retired so far           */

/* ------------------------------------------------------------------ */
/* Code buffer                                                         */
/* ------------------------------------------------------------------ */

typedef struct {
    uint32_t guest_pc;
    uint8_t *code;
    uint32_t hits;
} jit_block_t;

/*
 * Block and hash capacity, sized for a host rather than reusing
 * RV_JIT_MAX_BLOCKS / RV_JIT_HASH_SIZE. Those are 256 each because on a
 * microcontroller the tables compete with guest RAM; here they cost 160 KB
 * of a machine with gigabytes, and at 256 CoreMark flushed nineteen times
 * in a single run -- which retranslates constantly and is exactly the
 * churn that would mask a translator bug behind a fresh translation.
 */
#define X86_MAX_BLOCKS  8192u
#define X86_HASH_SIZE   8192u

static uint8_t     *g_code;
static uint32_t     g_code_size;
static uint32_t     g_code_used;
static jit_block_t  g_blocks[X86_MAX_BLOCKS];
static uint32_t     g_block_count;
static int32_t      g_hash[X86_HASH_SIZE];

static uint8_t     *g_emit;
static uint8_t     *g_emit_end;
static bool         g_emit_overflow;

static rv_jit_stats_t g_stats;

static uint32_t pc_hash(uint32_t pc)
{
    return (pc >> 1) & (X86_HASH_SIZE - 1u);
}

static void emit8(uint8_t b)
{
    if (RV_UNLIKELY(g_emit >= g_emit_end)) {
        g_emit_overflow = true;
        return;
    }
    *g_emit++ = b;
}

static void emit32(uint32_t v)
{
    emit8((uint8_t)v);
    emit8((uint8_t)(v >> 8));
    emit8((uint8_t)(v >> 16));
    emit8((uint8_t)(v >> 24));
}

static void emit64(uint64_t v)
{
    emit32((uint32_t)v);
    emit32((uint32_t)(v >> 32));
}

/* ------------------------------------------------------------------ */
/* Emitters                                                            */
/* ------------------------------------------------------------------ */

/*
 * ModRM for [rbx + disp32]. disp32 rather than the shorter disp8 form
 * uniformly: the guest register file reaches 124 bytes, which would fit,
 * but pc and everything after it do not, and one encoding for both is
 * worth four bytes a memory reference in a buffer this size.
 */
static void emit_modrm_hart(int reg, uint32_t disp)
{
    emit8((uint8_t)(0x80u | ((unsigned)reg << 3) | REG_HART));
    emit32(disp);
}

/* mod=11: register-direct. */
static void emit_modrm_rr(int reg, int rm)
{
    emit8((uint8_t)(0xC0u | ((unsigned)reg << 3) | (unsigned)rm));
}

/* mov r32, [hart + disp] */
static void emit_ld_hart(int dst, uint32_t disp)
{
    emit8(0x8B);
    emit_modrm_hart(dst, disp);
}

/* mov [hart + disp], r32 */
static void emit_st_hart(int src, uint32_t disp)
{
    emit8(0x89);
    emit_modrm_hart(src, disp);
}

/*
 * Read guest register `r` into a host register.
 *
 * x0 is not stored as a zero that happens to be there -- it is materialised
 * as an immediate, so nothing depends on the register file's slot 0 having
 * been kept clear.
 */
static void emit_rd_gpr(int dst, uint32_t r)
{
    if (r == 0u) {
        emit8((uint8_t)(0xB8u + dst));   /* mov r32, imm32 */
        emit32(0u);
        return;
    }
    emit_ld_hart(dst, r * 4u);
}

/* Write a host register back to guest register `r`; writes to x0 vanish. */
static void emit_wr_gpr(uint32_t r, int src)
{
    if (r == 0u) {
        return;
    }
    emit_st_hart(src, r * 4u);
}

static void emit_mov_imm32(int dst, uint32_t imm)
{
    emit8((uint8_t)(0xB8u + dst));
    emit32(imm);
}

/* mov r64, imm64 -- only ever used for helper addresses. */
static void emit_mov_imm64(int dst, uint64_t imm)
{
    emit8(0x48);                          /* REX.W */
    emit8((uint8_t)(0xB8u + dst));
    emit64(imm);
}

/* op r/m32, r32 with mod=11, i.e. dst op= src. */
static void emit_alu_rr(uint8_t op, int dst, int src)
{
    emit8(op);
    emit_modrm_rr(src, dst);
}

#define ALU_ADD 0x01u
#define ALU_OR  0x09u
#define ALU_AND 0x21u
#define ALU_SUB 0x29u
#define ALU_XOR 0x31u
#define ALU_CMP 0x39u

/* Shift r32 by cl. ext selects the operation: 4 shl, 5 shr, 7 sar. */
static void emit_shift_cl(int dst, unsigned ext)
{
    emit8(0xD3);
    emit8((uint8_t)(0xC0u | (ext << 3) | (unsigned)dst));
}

/* Shift r32 by an immediate. */
static void emit_shift_imm(int dst, unsigned ext, uint32_t amount)
{
    emit8(0xC1);
    emit8((uint8_t)(0xC0u | (ext << 3) | (unsigned)dst));
    emit8((uint8_t)(amount & 31u));
}

/*
 * setcc al; movzx eax, al.
 *
 * SLT and SLTU are the only guest instructions that need a flag turned
 * back into a value, and this is the shortest way to do it that does not
 * branch.
 */
static void emit_setcc_eax(uint8_t cc)
{
    emit8(0x0F);
    emit8(cc);
    emit8(0xC0);                          /* setcc al */
    emit8(0x0F);
    emit8(0xB6);
    emit8(0xC0);                          /* movzx eax, al */
}

#define CC_L  0x9Cu   /* signed less than   */
#define CC_B  0x92u   /* unsigned less than */

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

/*
 * One load or store, performed exactly as the interpreter would.
 *
 * Going through rv_hart_load/rv_hart_store rather than inlining the access
 * is a deliberate difference from the ARM backend, which inlines and has
 * paid for it three times -- the LR/SC reservation, PMP, and then
 * translation. Everything those helpers do beyond the access itself is
 * had for free here, and the cost is a call on a host that predicts them
 * well.
 *
 * Returns non-zero if the access trapped, in which case the trap has
 * already been entered and the block must return.
 */
static uint32_t jit_helper_mem(rv_hart_t *h, uint32_t insn, uint32_t pc)
{
    const uint32_t op = insn & 0x7Fu;
    const uint32_t f3 = rv_funct3(insn);
    const uint32_t rs1 = rv_rs1(insn);
    rv_exc_t exc;

    if (op == 0x03u) {                    /* LOAD */
        const uint32_t addr = h->x[rs1] + (uint32_t)rv_imm_i(insn);
        const uint32_t size = 1u << (f3 & 3u);
        const bool sext = (f3 & 4u) == 0u;
        uint32_t v = 0u;

        exc = rv_hart_load(h, addr, size, sext, &v);
        if (RV_UNLIKELY(exc != RV_EXC_NONE)) {
            h->pc = pc;
            rv_hart_trap(h, exc, addr);
            return 1u;
        }
        const uint32_t rd = rv_rd(insn);
        h->x[rd] = v;
        h->x[0] = 0u;
        return 0u;
    }

    /* STORE */
    const uint32_t addr = h->x[rs1] + (uint32_t)rv_imm_s(insn);
    const uint32_t size = 1u << (f3 & 3u);

    exc = rv_hart_store(h, addr, size, h->x[rv_rs2(insn)]);
    if (RV_UNLIKELY(exc != RV_EXC_NONE)) {
        h->pc = pc;
        rv_hart_trap(h, exc, addr);
        return 1u;
    }
    return 0u;
}

#if RV_EXT_F
/*
 * One F-extension instruction, through the same rv_hart_fp the interpreter
 * uses.
 *
 * Not open-coded onto SSE, and that is a decision rather than a shortcut.
 * The scalar single-precision operations look like a direct match, but the
 * places where a host FPU and RISC-V disagree are exactly the places this
 * project has already been bitten: NaN-to-integer conversion (RISC-V gives
 * the maximum, x86 gives INT_MIN), the sign of zero out of FMIN/FMAX,
 * canonical-NaN propagation, and the exception flags, which live in MXCSR
 * in a different order with different sticky rules. The ARM backend needed
 * a seven-instruction fixup for the NaN case alone.
 *
 * CLAUDE.md's own conclusion applies unchanged: a helper call is a
 * translation, declining is not. Routing here keeps the block whole -- the
 * FP instruction no longer ends it and hand the rest to the interpreter --
 * without making a second copy of semantics the core already owns.
 *
 * Returns non-zero if the instruction trapped.
 */
static uint32_t jit_helper_fp(rv_hart_t *h, uint32_t insn, uint32_t pc)
{
    uint32_t tval = 0u;
    const rv_exc_t exc = rv_hart_fp(h, insn, &tval);

    if (RV_UNLIKELY(exc != RV_EXC_NONE)) {
        h->pc = pc;
        rv_hart_trap(h, exc, tval);
        return 1u;
    }
    return 0u;
}
#endif

/* ------------------------------------------------------------------ */
/* Block prologue and epilogue                                         */
/* ------------------------------------------------------------------ */

/*
 * A block is uint32_t (*)(rv_hart_t *) and returns the number of guest
 * instructions it retired -- which is not always the number it contains,
 * because a trapping access returns early.
 *
 * The stack adjustment is not padding. System V requires rsp to be 16-byte
 * aligned at a call, and entry leaves it 8 past that; two pushes bring it
 * back to 8, so one more 8 is needed before any helper call can be made.
 * Getting it wrong does not fault here -- it faults inside whatever libc
 * routine the helper eventually reaches and uses an aligned SSE store.
 */
static void emit_prologue(void)
{
    emit8(0x53);                          /* push rbx        */
    emit8(0x55);                          /* push rbp        */
    emit8(0x48); emit8(0x83); emit8(0xEC); emit8(0x08);  /* sub rsp, 8 */
    emit8(0x48); emit8(0x89); emit8(0xFB);               /* mov rbx, rdi */
    emit8(0x31); emit8(0xED);                            /* xor ebp, ebp */
}

static void emit_epilogue(void)
{
    emit8(0x89); emit8(0xE8);             /* mov eax, ebp    */
    emit8(0x48); emit8(0x83); emit8(0xC4); emit8(0x08);  /* add rsp, 8 */
    emit8(0x5D);                          /* pop rbp         */
    emit8(0x5B);                          /* pop rbx         */
    emit8(0xC3);                          /* ret             */
}

/* ------------------------------------------------------------------ */
/* Translation                                                         */
/* ------------------------------------------------------------------ */

/*
 * The outcome of translating one instruction.
 *
 * XLAT_END exists because a control transfer writes h->pc itself, and the
 * caller's automatic "pc = the next instruction" must not then overwrite
 * it. Collapsing the two into a bool is how a translated branch quietly
 * becomes a fall-through.
 */
typedef enum {
    XLAT_NO = 0,     /* not translated; the interpreter takes it */
    XLAT_OK,         /* translated; caller advances pc           */
    XLAT_END         /* translated; it set pc, and ends the block */
} xlat_t;

/* Emit a jcc rel32 whose target is patched later; returns the disp slot. */
static uint8_t *emit_jcc32(uint8_t cc)
{
    emit8(0x0F);
    emit8(cc);
    uint8_t *const slot = g_emit;
    emit32(0u);
    return slot;
}

static uint8_t *emit_jmp32(void)
{
    emit8(0xE9);
    uint8_t *const slot = g_emit;
    emit32(0u);
    return slot;
}

static void patch_rel32(uint8_t *slot, const uint8_t *target)
{
    const int32_t rel = (int32_t)(target - (slot + 4));
    memcpy(slot, &rel, sizeof(rel));
}

/*
 * Call a helper of the shape (rv_hart_t *, insn, pc) -> trapped, and leave
 * the block if it says the instruction trapped.
 *
 * rdi, rsi, rdx are the first three System V argument registers; the hart
 * pointer lives in rbx precisely so it survives the call.
 */
static void emit_call_trapping(const void *fn, uint32_t insn, uint32_t pc,
                               uint8_t **exits, unsigned *nexits)
{
    emit8(0x48); emit8(0x89); emit8(0xDF);   /* mov rdi, rbx */
    emit_mov_imm32(ESI, insn);
    emit_mov_imm32(EDX, pc);
    emit_mov_imm64(EAX, (uint64_t)(uintptr_t)fn);
    emit8(0xFF); emit8(0xD0);                /* call rax */

    emit_alu_rr(0x85u, EAX, EAX);            /* test eax, eax */
    emit8(0x0F); emit8(0x85);                /* jne rel32 */
    if (*nexits < RV_JIT_MAX_BLOCK_INSNS) {
        exits[(*nexits)++] = g_emit;
    }
    emit32(0u);                              /* patched to the epilogue */
}

/* x86 condition codes for the six RISC-V branches, in funct3 order. */
static uint8_t branch_cc(uint32_t f3)
{
    switch (f3) {
    case 0u:  return 0x84u;   /* BEQ  -> je   */
    case 1u:  return 0x85u;   /* BNE  -> jne  */
    case 4u:  return 0x8Cu;   /* BLT  -> jl   */
    case 5u:  return 0x8Du;   /* BGE  -> jge  */
    case 6u:  return 0x82u;   /* BLTU -> jb   */
    default:  return 0x83u;   /* BGEU -> jae  */
    }
}

/*
 * Translate one instruction.
 *
 * `exits` collects the addresses of forward jumps that must land on the
 * epilogue; the trapping paths are the only things that use it.
 */
static xlat_t translate_one(uint32_t insn, uint32_t pc, uint32_t len,
                            uint8_t **exits, unsigned *nexits)
{
    const uint32_t op = insn & 0x7Fu;
    const uint32_t rd = rv_rd(insn);
    const uint32_t rs1 = rv_rs1(insn);
    const uint32_t rs2 = rv_rs2(insn);
    const uint32_t f3 = rv_funct3(insn);
    const uint32_t f7 = insn >> 25;

    switch (op) {
    case 0x37u:                           /* LUI */
        emit_mov_imm32(EAX, insn & 0xFFFFF000u);
        emit_wr_gpr(rd, EAX);
        return XLAT_OK;

    case 0x17u:                           /* AUIPC */
        emit_mov_imm32(EAX, pc + (insn & 0xFFFFF000u));
        emit_wr_gpr(rd, EAX);
        return XLAT_OK;

    case 0x13u: {                         /* OP-IMM */
        const uint32_t imm = (uint32_t)rv_imm_i(insn);

        emit_rd_gpr(EAX, rs1);
        switch (f3) {
        case 0u:                          /* ADDI */
            emit_mov_imm32(ECX, imm);
            emit_alu_rr(ALU_ADD, EAX, ECX);
            break;
        case 4u:                          /* XORI */
            emit_mov_imm32(ECX, imm);
            emit_alu_rr(ALU_XOR, EAX, ECX);
            break;
        case 6u:                          /* ORI */
            emit_mov_imm32(ECX, imm);
            emit_alu_rr(ALU_OR, EAX, ECX);
            break;
        case 7u:                          /* ANDI */
            emit_mov_imm32(ECX, imm);
            emit_alu_rr(ALU_AND, EAX, ECX);
            break;
        case 2u:                          /* SLTI */
            emit_mov_imm32(ECX, imm);
            emit_alu_rr(ALU_CMP, EAX, ECX);
            emit_setcc_eax(CC_L);
            break;
        case 3u:                          /* SLTIU */
            emit_mov_imm32(ECX, imm);
            emit_alu_rr(ALU_CMP, EAX, ECX);
            emit_setcc_eax(CC_B);
            break;
        case 1u:                          /* SLLI */
            if (f7 != 0u) {
                return XLAT_NO;             /* a Zb* encoding; not here yet */
            }
            emit_shift_imm(EAX, 4u, rs2);
            break;
        case 5u:                          /* SRLI / SRAI */
            if (f7 == 0u) {
                emit_shift_imm(EAX, 5u, rs2);
            } else if (f7 == 0x20u) {
                emit_shift_imm(EAX, 7u, rs2);
            } else {
                return XLAT_NO;
            }
            break;
        default:
            return XLAT_NO;
        }
        emit_wr_gpr(rd, EAX);
        return XLAT_OK;
    }

    case 0x33u: {                         /* OP */
        if (f7 != 0u && f7 != 0x20u) {
            return XLAT_NO;                 /* M, Zb*: interpreter's job */
        }
        emit_rd_gpr(EAX, rs1);
        emit_rd_gpr(ECX, rs2);

        switch (f3) {
        case 0u:
            emit_alu_rr(f7 == 0x20u ? ALU_SUB : ALU_ADD, EAX, ECX);
            break;
        case 4u:
            if (f7 != 0u) { return XLAT_NO; }
            emit_alu_rr(ALU_XOR, EAX, ECX);
            break;
        case 6u:
            if (f7 != 0u) { return XLAT_NO; }
            emit_alu_rr(ALU_OR, EAX, ECX);
            break;
        case 7u:
            if (f7 != 0u) { return XLAT_NO; }
            emit_alu_rr(ALU_AND, EAX, ECX);
            break;
        case 2u:
            if (f7 != 0u) { return XLAT_NO; }
            emit_alu_rr(ALU_CMP, EAX, ECX);
            emit_setcc_eax(CC_L);
            break;
        case 3u:
            if (f7 != 0u) { return XLAT_NO; }
            emit_alu_rr(ALU_CMP, EAX, ECX);
            emit_setcc_eax(CC_B);
            break;
        case 1u:
            if (f7 != 0u) { return XLAT_NO; }
            emit_shift_cl(EAX, 4u);       /* cl already holds rs2 */
            break;
        case 5u:
            emit_shift_cl(EAX, f7 == 0x20u ? 7u : 5u);
            break;
        default:
            return XLAT_NO;
        }
        emit_wr_gpr(rd, EAX);
        return XLAT_OK;
    }

    case 0x03u:                           /* LOAD  */
    case 0x23u: {                         /* STORE */
        if (op == 0x03u && f3 == 3u) {
            return XLAT_NO;                 /* no LD on RV32 */
        }
        if (op == 0x23u && f3 > 2u) {
            return XLAT_NO;
        }
        emit_call_trapping(&jit_helper_mem, insn, pc, exits, nexits);
        return XLAT_OK;
    }

#if RV_EXT_F
    case 0x07u:                           /* FLW  */
    case 0x27u:                           /* FSW  */
    case 0x53u:                           /* OP-FP */
    case 0x43u:                           /* FMADD  */
    case 0x47u:                           /* FMSUB  */
    case 0x4Bu:                           /* FNMSUB */
    case 0x4Fu:                           /* FNMADD */
        emit_call_trapping(&jit_helper_fp, insn, pc, exits, nexits);
        return XLAT_OK;
#endif

    case 0x63u: {                         /* BRANCH */
        if (f3 == 2u || f3 == 3u) {
            return XLAT_NO;               /* no such encoding */
        }
        emit_rd_gpr(EAX, rs1);
        emit_rd_gpr(ECX, rs2);
        emit_alu_rr(ALU_CMP, EAX, ECX);

        uint8_t *const to_taken = emit_jcc32(branch_cc(f3));
        emit_mov_imm32(EAX, pc + len);            /* not taken */
        uint8_t *const to_done = emit_jmp32();
        patch_rel32(to_taken, g_emit);
        emit_mov_imm32(EAX, pc + (uint32_t)rv_imm_b(insn));
        patch_rel32(to_done, g_emit);
        emit_st_hart(EAX, HART_PC_OFF);
        return XLAT_END;
    }

    case 0x6Fu:                           /* JAL */
        emit_mov_imm32(EAX, pc + len);
        emit_wr_gpr(rd, EAX);
        emit_mov_imm32(EAX, pc + (uint32_t)rv_imm_j(insn));
        emit_st_hart(EAX, HART_PC_OFF);
        return XLAT_END;

    case 0x67u:                           /* JALR */
        if (f3 != 0u) {
            return XLAT_NO;
        }
        /*
         * The target is computed before the link value is written, because
         * rd and rs1 are allowed to be the same register -- `jalr ra, ra`
         * is what a compiler emits for an indirect call through a saved
         * pointer, and writing the link first would jump to it.
         */
        emit_rd_gpr(EAX, rs1);
        emit_mov_imm32(ECX, (uint32_t)rv_imm_i(insn));
        emit_alu_rr(ALU_ADD, EAX, ECX);
        emit8(0x83); emit8(0xE0); emit8(0xFE);    /* and eax, ~1 */
        emit_mov_imm32(ECX, pc + len);
        emit_wr_gpr(rd, ECX);
        emit_st_hart(EAX, HART_PC_OFF);
        return XLAT_END;

    default:
        return XLAT_NO;
    }
}

/* ------------------------------------------------------------------ */
/* Block management                                                    */
/* ------------------------------------------------------------------ */

static void rebuild_hash(void)
{
    for (uint32_t i = 0; i < X86_HASH_SIZE; i++) {
        g_hash[i] = -1;
    }
    for (uint32_t i = 0; i < g_block_count; i++) {
        g_hash[pc_hash(g_blocks[i].guest_pc)] = (int32_t)i;
    }
}

void rv_jit_flush(void)
{
    g_block_count = 0u;
    g_code_used = 0u;
    rebuild_hash();
    g_stats.flushes++;
}

static jit_block_t *lookup(uint32_t pc)
{
    const int32_t i = g_hash[pc_hash(pc)];

    if (i >= 0 && g_blocks[i].guest_pc == pc) {
        g_blocks[i].hits++;
        return &g_blocks[i];
    }
    return NULL;
}

static jit_block_t *translate(rv_hart_t *h, uint32_t pc)
{
    if (g_block_count >= X86_MAX_BLOCKS ||
        g_code_used + 4096u > g_code_size) {
        /*
         * No compaction here, unlike the ARM backend. On a host the buffer
         * is megabytes and a flush is rare; on a microcontroller it was
         * kilobytes and flushing threw away everything hot, which is the
         * whole reason that backend grew a compactor.
         */
        rv_jit_flush();
    }

    g_emit = g_code + g_code_used;
    g_emit_end = g_code + g_code_size;
    g_emit_overflow = false;

    uint8_t *const start = g_emit;
    uint8_t *exits[RV_JIT_MAX_BLOCK_INSNS];
    unsigned nexits = 0u;
    uint32_t cur = pc;
    uint32_t count = 0u;

    emit_prologue();

    while (count < RV_JIT_MAX_BLOCK_INSNS && !g_emit_overflow) {
        uint16_t lo;
        if (rv_bus_fetch16(h->bus, cur, &lo) != RV_EXC_NONE) {
            break;
        }
        uint32_t insn;
        uint32_t len;

        if (rv_is_32bit(lo)) {
            uint16_t hi;
            if (rv_bus_fetch16(h->bus, cur + 2u, &hi) != RV_EXC_NONE) {
                break;
            }
            insn = (uint32_t)lo | ((uint32_t)hi << 16);
            len = 4u;
        } else {
#if RV_EXT_C
            /*
             * Expanded and then translated as its 32-bit equivalent, which
             * is the same thing the interpreter does. Skipping compressed
             * encodings instead would leave 92% of a C-compiled guest
             * interpreted -- measured, on the self-test -- and a backend
             * that only ever sees the instructions a compiler did not
             * choose is not covering much.
             */
            insn = rv_decode_expand_c(lo);
            len = 2u;
            if (insn == 0u) {
                break;                    /* illegal: the interpreter reports it */
            }
#else
            break;
#endif
        }

        uint8_t *const before = g_emit;
        const xlat_t r = translate_one(insn, cur, len, exits, &nexits);
        if (r == XLAT_NO) {
            g_emit = before;
            break;
        }

        /* One more guest instruction retired. */
        emit8(0x83); emit8(0xC5); emit8(0x01);        /* add ebp, 1 */
        cur += len;
        count++;

        if (r == XLAT_END) {
            break;                        /* it wrote pc itself */
        }
        emit_mov_imm32(EAX, cur);
        emit_st_hart(EAX, HART_PC_OFF);
    }

    if (count == 0u || g_emit_overflow) {
        return NULL;
    }

    uint8_t *const exit_at = g_emit;
    emit_epilogue();

    /* Patch every early exit to land on the epilogue. */
    for (unsigned i = 0; i < nexits; i++) {
        const int32_t rel = (int32_t)(exit_at - (exits[i] + 4));
        memcpy(exits[i], &rel, sizeof(rel));
    }

    if (g_emit_overflow) {
        return NULL;
    }

    jit_block_t *const b = &g_blocks[g_block_count++];
    b->guest_pc = pc;
    b->code = start;
    b->hits = 0u;
    g_code_used = (uint32_t)(g_emit - g_code);
    g_hash[pc_hash(pc)] = (int32_t)(g_block_count - 1u);

    g_stats.translations++;
    return b;
}

/* ------------------------------------------------------------------ */
/* Backend interface                                                   */
/* ------------------------------------------------------------------ */

typedef uint32_t (*block_fn_t)(rv_hart_t *);

static bool jit_init(rv_hart_t *h)
{
    (void)h;
    if (g_code != NULL) {
        return true;
    }

    /*
     * Not RV_JIT_CODE_SIZE. That knob is tuned for a microcontroller where
     * every byte of code cache is a byte the guest does not get, and where
     * 12 KB against a 48 KB working set was measured to make the JIT
     * *slower* than the interpreter. A host has no such tension, and a
     * buffer small enough to flush repeatedly would hide translator bugs
     * behind constant retranslation -- which is the opposite of why this
     * backend exists.
     */
    g_code_size = 4u * 1024u * 1024u;
    /*
     * Mapped writable *and* executable, which a hardened host may refuse.
     * The alternative is mprotect between translating and running, and the
     * honest note is that this backend exists for testing rather than
     * deployment: it never runs guest-controlled data as code that the
     * interpreter would not also have executed.
     */
    void *p = mmap(NULL, g_code_size, PROT_READ | PROT_WRITE | PROT_EXEC,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        return false;
    }
    g_code = (uint8_t *)p;
    rv_jit_flush();
    return true;
}

static void jit_reset(rv_hart_t *h)
{
    (void)h;
    rv_jit_flush();
}

static void jit_invalidate(rv_hart_t *h, uint32_t addr, uint32_t len)
{
    (void)h;
    (void)addr;
    (void)len;
    /* Blocks are not tracked by the guest memory they came from, so the
     * conservative answer is the only available one. FENCE.I is rare. */
    rv_jit_flush();
}

static rv_run_reason_t jit_run(rv_hart_t *h, uint32_t budget, uint32_t *retired)
{
    uint32_t done = 0u;
    rv_run_reason_t reason = RV_RUN_BUDGET;

    if (RV_UNLIKELY(g_code == NULL)) {
        return rv_backend_interp.run(h, budget, retired);
    }

    if (RV_UNLIKELY(h->state == RV_STATE_WFI)) {
        if (!rv_hart_wfi_wake(h)) {
            if (retired != NULL) {
                *retired = 0u;
            }
            return RV_RUN_WFI;
        }
        h->state = RV_STATE_RUNNING;
#if RV_LAZY_IRQ_CHECK
        h->irq_dirty = true;
#endif
    }

    while (done < budget) {
        if (RV_UNLIKELY(h->state != RV_STATE_RUNNING)) {
            reason = (h->state == RV_STATE_HALTED) ? RV_RUN_HALTED : RV_RUN_WFI;
            break;
        }

#if RV_LAZY_IRQ_CHECK
        if (RV_UNLIKELY(h->irq_dirty))
#endif
        {
#if RV_LAZY_IRQ_CHECK
            h->irq_dirty = false;
#endif
            const rv_exc_t irq = rv_hart_pending_irq(h);
            if (RV_UNLIKELY(irq != RV_EXC_NONE)) {
                rv_hart_trap(h, RV_CAUSE_INTERRUPT | irq, 0u);
                done++;
                continue;
            }
        }

        /*
         * Anything that can refuse or redirect a fetch is left to the
         * interpreter wholesale. Translating under PMP, Sdtrig or address
         * translation means reproducing each of their rules in the emitted
         * code, and the ARM backend's history is that every one of those
         * was got wrong at least once. This backend exists to find bugs,
         * not to add a second copy of them.
         */
        if (RV_UNLIKELY(h->fetch_guard)) {
            uint32_t n = 0u;
            const rv_run_reason_t r = rv_backend_interp.run(h, 1u, &n);
            done += n;
            g_stats.interp_fallbacks += n;
            if (r == RV_RUN_HALTED || r == RV_RUN_WFI) {
                reason = r;
                break;
            }
            continue;
        }

        jit_block_t *b = lookup(h->pc);
        if (b == NULL) {
            b = translate(h, h->pc);
            if (b == NULL) {
                uint32_t n = 0u;
                const rv_run_reason_t r = rv_backend_interp.run(h, 1u, &n);
                done += n;
                g_stats.interp_fallbacks += n;
                if (r == RV_RUN_HALTED || r == RV_RUN_WFI) {
                    reason = r;
                    break;
                }
                continue;
            }
        }

        const uint32_t n = ((block_fn_t)(void *)b->code)(h);
        g_stats.block_entries++;

        /*
         * A block that trapped on its first instruction retires nothing --
         * the early exit is taken before the retire counter is bumped --
         * but it has still made progress: the trap moved pc into a handler.
         * Charging the budget nothing for that spins this loop forever
         * while the guest runs on happily underneath, which is a hang that
         * --max-insn cannot break because that is checked by the caller.
         *
         * The budget is charged; the architectural counters below are not,
         * because no instruction retired and minstret must not say one did.
         */
        done += (n != 0u) ? n : 1u;


#if RV_EXT_ZICNTR
        if (RV_LIKELY((h->mcountinhibit & 0x1u) == 0u)) {
            h->mcycle += n;
        }
        if (RV_LIKELY((h->mcountinhibit & 0x4u) == 0u)) {
            h->minstret += n;
        }
#endif
    }

    if (retired != NULL) {
        *retired = done;
    }
    return reason;
}

void rv_jit_get_stats(rv_jit_stats_t *out)
{
    g_stats.code_used = g_code_used;
    g_stats.code_size = g_code_size;
    g_stats.blocks = g_block_count;
    *out = g_stats;
}

const rv_backend_t rv_backend_jit = {
    .name = "jit-x86-64",
    .init = jit_init,
    .reset = jit_reset,
    .run = jit_run,
    .invalidate = jit_invalidate,
};

#endif /* RV_ENABLE_JIT && RV_JIT_X86_64 */
