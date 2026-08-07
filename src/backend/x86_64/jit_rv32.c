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
 * What is translated is the integer core: LUI, AUIPC, the OP-IMM and OP
 * groups, and loads and stores through a helper. Everything else ends the
 * block and is executed by the interpreter, which is the same policy the
 * ARM backend started from -- and, as recorded in CLAUDE.md, declining is
 * the expensive choice, so the set is meant to grow.
 */

#include "rv32/rv_jit.h"
#include "emu/emu_jit.h"
#include "emu/emu_x86_64.h"

#if RV_ENABLE_JIT && defined(RV_JIT_X86_64)

#include "rv32/rv_backend.h"
#include "rv32/rv_decode.h"
#include "rv32/rv_ir.h"
#include "rv32/rv_hart.h"

#include <stddef.h>
#include <string.h>
#include <sys/mman.h>

_Static_assert(offsetof(rv_hart_t, x) == 0,
               "translated code assumes hart->x is at offset 0");
_Static_assert(sizeof(((rv_hart_t *)0)->x) == 32u * 4u,
               "translated code assumes 32 32-bit guest registers");

#define HART_PC_OFF ((uint32_t)offsetof(rv_hart_t, pc))

/*
 * Not RV_JIT_CODE_SIZE. That knob is tuned for a microcontroller where
 * every byte of code cache is a byte the guest does not get; a host has no
 * such tension, and a buffer small enough to flush repeatedly would hide
 * translator bugs behind constant retranslation.
 */
#define RV_JIT_HOST_CODE_BYTES (4u * 1024u * 1024u)

/* ------------------------------------------------------------------ */
/* Host registers                                                      */
/* ------------------------------------------------------------------ */

/*
 * Only the low eight are used, so no instruction here needs a REX.R or
 * REX.B bit and every ModRM byte is one byte. rbx and rbp are callee-saved
 * in the System V ABI, which is what lets them survive a helper call.
 */

/* ------------------------------------------------------------------ */
/* Guest registers                                                     */
/* ------------------------------------------------------------------ */

/*
 * These stay here rather than in emu_x86_64.c because they are the one
 * place the host encoder meets the guest: x[] at offset 0 is a fact about
 * rv_hart_t, not about x86.
 *
 * x0 is materialised as an immediate rather than loaded, so nothing
 * depends on the register file's slot 0 having been kept clear.
 */
static void emit_rd_gpr(int dst, uint32_t r)
{
    if (r == 0u) {
        x86_mov_imm32(dst, 0u);
        return;
    }
    x86_ld_cpu(dst, r * 4u);
}

/* Writes to x0 vanish. */
static void emit_wr_gpr(uint32_t r, int src)
{
    if (r == 0u) {
        return;
    }
    x86_st_cpu(src, r * 4u);
}

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
        if (EMU_UNLIKELY(exc != RV_EXC_NONE)) {
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
    if (EMU_UNLIKELY(exc != RV_EXC_NONE)) {
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

    if (EMU_UNLIKELY(exc != RV_EXC_NONE)) {
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
 * (moved to emu_x86_64.c)
 * instructions it retired -- which is not always the number it contains,
 * because a trapping access returns early.
 *
 * The stack adjustment is not padding. System V requires rsp to be 16-byte
 * aligned at a call, and entry leaves it 8 past that; two pushes bring it
 * back to 8, so one more 8 is needed before any helper call can be made.
 * Getting it wrong does not fault here -- it faults inside whatever libc
 * routine the helper eventually reaches and uses an aligned SSE store.
 */


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
    emu_jit_emit8(0x48); emu_jit_emit8(0x89); emu_jit_emit8(0xDF);   /* mov rdi, rbx */
    x86_mov_imm32(X86_ESI, insn);
    x86_mov_imm32(X86_EDX, pc);
    x86_mov_imm64(X86_EAX, (uint64_t)(uintptr_t)fn);
    emu_jit_emit8(0xFF); emu_jit_emit8(0xD0);                /* call rax */

    x86_alu_rr(0x85u, X86_EAX, X86_EAX);            /* test eax, eax */
    emu_jit_emit8(0x0F); emu_jit_emit8(0x85);                /* jne rel32 */
    if (*nexits < RV_JIT_MAX_BLOCK_INSNS) {
        exits[(*nexits)++] = emu_jit_here();
    }
    emu_jit_emit32(0u);                              /* patched to the epilogue */
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
        x86_mov_imm32(X86_EAX, insn & 0xFFFFF000u);
        emit_wr_gpr(rd, X86_EAX);
        return XLAT_OK;

    case 0x17u:                           /* AUIPC */
        x86_mov_imm32(X86_EAX, pc + (insn & 0xFFFFF000u));
        emit_wr_gpr(rd, X86_EAX);
        return XLAT_OK;

    case 0x13u: {                         /* OP-IMM */
        const uint32_t imm = (uint32_t)rv_imm_i(insn);

        emit_rd_gpr(X86_EAX, rs1);
        switch (f3) {
        case 0u:                          /* ADDI */
            x86_mov_imm32(X86_ECX, imm);
            x86_alu_rr(X86_ADD, X86_EAX, X86_ECX);
            break;
        case 4u:                          /* XORI */
            x86_mov_imm32(X86_ECX, imm);
            x86_alu_rr(X86_XOR, X86_EAX, X86_ECX);
            break;
        case 6u:                          /* ORI */
            x86_mov_imm32(X86_ECX, imm);
            x86_alu_rr(X86_OR, X86_EAX, X86_ECX);
            break;
        case 7u:                          /* ANDI */
            x86_mov_imm32(X86_ECX, imm);
            x86_alu_rr(X86_AND, X86_EAX, X86_ECX);
            break;
        case 2u:                          /* SLTI */
            x86_mov_imm32(X86_ECX, imm);
            x86_alu_rr(X86_CMP, X86_EAX, X86_ECX);
            x86_setcc_eax(X86_CC_L);
            break;
        case 3u:                          /* SLTIU */
            x86_mov_imm32(X86_ECX, imm);
            x86_alu_rr(X86_CMP, X86_EAX, X86_ECX);
            x86_setcc_eax(X86_CC_B);
            break;
        case 1u:                          /* SLLI */
            if (f7 != 0u) {
                return XLAT_NO;             /* a Zb* encoding; not here yet */
            }
            x86_shift_imm(X86_EAX, 4u, rs2);
            break;
        case 5u:                          /* SRLI / SRAI */
            if (f7 == 0u) {
                x86_shift_imm(X86_EAX, 5u, rs2);
            } else if (f7 == 0x20u) {
                x86_shift_imm(X86_EAX, 7u, rs2);
            } else {
                return XLAT_NO;
            }
            break;
        default:
            return XLAT_NO;
        }
        emit_wr_gpr(rd, X86_EAX);
        return XLAT_OK;
    }

    case 0x33u: {                         /* OP */
        if (f7 != 0u && f7 != 0x20u) {
            return XLAT_NO;                 /* M, Zb*: interpreter's job */
        }
        emit_rd_gpr(X86_EAX, rs1);
        emit_rd_gpr(X86_ECX, rs2);

        switch (f3) {
        case 0u:
            x86_alu_rr(f7 == 0x20u ? X86_SUB : X86_ADD, X86_EAX, X86_ECX);
            break;
        case 4u:
            if (f7 != 0u) { return XLAT_NO; }
            x86_alu_rr(X86_XOR, X86_EAX, X86_ECX);
            break;
        case 6u:
            if (f7 != 0u) { return XLAT_NO; }
            x86_alu_rr(X86_OR, X86_EAX, X86_ECX);
            break;
        case 7u:
            if (f7 != 0u) { return XLAT_NO; }
            x86_alu_rr(X86_AND, X86_EAX, X86_ECX);
            break;
        case 2u:
            if (f7 != 0u) { return XLAT_NO; }
            x86_alu_rr(X86_CMP, X86_EAX, X86_ECX);
            x86_setcc_eax(X86_CC_L);
            break;
        case 3u:
            if (f7 != 0u) { return XLAT_NO; }
            x86_alu_rr(X86_CMP, X86_EAX, X86_ECX);
            x86_setcc_eax(X86_CC_B);
            break;
        case 1u:
            if (f7 != 0u) { return XLAT_NO; }
            x86_shift_cl(X86_EAX, 4u);       /* cl already holds rs2 */
            break;
        case 5u:
            x86_shift_cl(X86_EAX, f7 == 0x20u ? 7u : 5u);
            break;
        default:
            return XLAT_NO;
        }
        emit_wr_gpr(rd, X86_EAX);
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
        emit_rd_gpr(X86_EAX, rs1);
        emit_rd_gpr(X86_ECX, rs2);
        x86_alu_rr(X86_CMP, X86_EAX, X86_ECX);

        uint8_t *const to_taken = x86_jcc32(branch_cc(f3));
        x86_mov_imm32(X86_EAX, pc + len);            /* not taken */
        uint8_t *const to_done = x86_jmp32();
        x86_patch_rel32(to_taken, emu_jit_here());
        x86_mov_imm32(X86_EAX, pc + (uint32_t)rv_imm_b(insn));
        x86_patch_rel32(to_done, emu_jit_here());
        x86_st_cpu(X86_EAX, HART_PC_OFF);
        return XLAT_END;
    }

    case 0x6Fu:                           /* JAL */
        x86_mov_imm32(X86_EAX, pc + len);
        emit_wr_gpr(rd, X86_EAX);
        x86_mov_imm32(X86_EAX, pc + (uint32_t)rv_imm_j(insn));
        x86_st_cpu(X86_EAX, HART_PC_OFF);
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
        emit_rd_gpr(X86_EAX, rs1);
        x86_mov_imm32(X86_ECX, (uint32_t)rv_imm_i(insn));
        x86_alu_rr(X86_ADD, X86_EAX, X86_ECX);
        emu_jit_emit8(0x83); emu_jit_emit8(0xE0); emu_jit_emit8(0xFE);    /* and eax, ~1 */
        x86_mov_imm32(X86_ECX, pc + len);
        emit_wr_gpr(rd, X86_ECX);
        x86_st_cpu(X86_EAX, HART_PC_OFF);
        return XLAT_END;

    default:
        return XLAT_NO;
    }
}

/* ------------------------------------------------------------------ */
/* The frontend's half of the framework                                */
/* ------------------------------------------------------------------ */

/*
 * Everything below is what emu_jit.h asks a frontend for. The buffer, the
 * block table, the hash, the flush and the dispatch loop are the
 * framework's now -- this file kept only the parts that know about RISC-V.
 */

/*
 * Guest -> IR -> optimise -> host.
 *
 * The per-opcode switch that used to be here moved to the frontend
 * (src/frontend/rv32/rv_ir.c, which names no host); the emitting moved
 * to ir_lower.c, which names no guest. What is left is the same glue
 * jit_g4mh.c has, which is the point -- the two become one jit.c for
 * this host.
 *
 * live_out is EMU_IR_F_ALL, but for this guest it is moot: RISC-V has no
 * condition flags, the frontend emits no EMU_IR_SETF, and the dead-flag
 * pass therefore finds nothing and costs nothing.
 */
static emu_ir_block_t g_ir;

static uint32_t rv_jit_translate(emu_cpu_t *cpu, uint32_t pc)
{
    const uint32_t n = rv_ir_translate(cpu, pc, &g_ir);

    if (n == 0u) {
        return 0u;
    }

    emu_ir_optimise(&g_ir, EMU_IR_F_ALL, NULL);

    x86_prologue();
    if (!emu_ir_lower(&g_ir, &rv_ir_target)) {
        return 0u;
    }
    x86_epilogue();
    return n;
}

/*
 * Anything that can refuse or redirect a fetch sends everything to the
 * interpreter. Translating under PMP, Sdtrig or address translation means
 * reproducing each of their rules in emitted code, and the ARM backend's
 * history is that every one of those was got wrong at least once. This
 * backend exists to find bugs, not to add a second copy of them.
 *
 * That is why `blocked` below is the whole fetch_guard, where the Thumb-2
 * backend uses trig_active alone: this translator declines what that one
 * bakes in and flushes for.
 */
static void rv_jit_bind(emu_cpu_t *cpu, emu_jit_hot_t *out)
{
    rv_hart_t *const h = (rv_hart_t *)cpu;

    out->pc = &h->pc;
    out->state = &h->state;
    out->blocked = &h->fetch_guard;
#if RV_LAZY_IRQ_CHECK
    out->irq_pending = &h->irq_dirty;
#endif
}

static bool rv_jit_is_idle(emu_cpu_t *cpu)
{
    return ((rv_hart_t *)cpu)->state == EMU_STATE_WFI;
}

static bool rv_jit_wake(emu_cpu_t *cpu)
{
    rv_hart_t *const h = (rv_hart_t *)cpu;

    if (!rv_hart_wfi_wake(h)) {
        return false;
    }
    h->state = EMU_STATE_RUNNING;
#if RV_LAZY_IRQ_CHECK
    h->irq_dirty = true;
#endif
    return true;
}

static bool rv_jit_take_irq(emu_cpu_t *cpu)
{
    rv_hart_t *const h = (rv_hart_t *)cpu;

#if RV_LAZY_IRQ_CHECK
    if (!h->irq_dirty) {
        return false;
    }
    h->irq_dirty = false;
#endif
    const rv_exc_t irq = rv_hart_pending_irq(h);
    if (irq == RV_EXC_NONE) {
        return false;
    }
    rv_hart_trap(h, RV_CAUSE_INTERRUPT | irq, 0u);
    return true;
}

static void rv_jit_count(emu_cpu_t *cpu, uint32_t n)
{
#if RV_EXT_ZICNTR
    rv_hart_t *const h = (rv_hart_t *)cpu;

    if ((h->mcountinhibit & 0x1u) == 0u) {
        h->mcycle += n;
    }
    if ((h->mcountinhibit & 0x4u) == 0u) {
        h->minstret += n;
    }
#else
    (void)cpu;
    (void)n;
#endif
}

static const emu_jit_ops_t g_jit_ops = {
    .name       = "jit-x86-64",
    .bind       = rv_jit_bind,
    .translate  = rv_jit_translate,
    /*
     * Relocatable: every branch inside a block is a rel32 whose ends move
     * together, and every address that is not -- helpers, guest pc -- is
     * an absolute immediate. x86 needs no sync, its caches being coherent
     * with instruction fetch.
     */
    .relocatable = true,
    .sync       = NULL,
    .interp     = &rv_backend_interp,
    .is_idle    = rv_jit_is_idle,
    .wake       = rv_jit_wake,
    .take_irq   = rv_jit_take_irq,
    .count      = rv_jit_count,
};

/*
 * generation is NULL, and that is a claim rather than an omission: this
 * translator reads nothing from mutable hart state. frm, mstatus.FS and
 * the PMP configuration -- everything the Thumb-2 backend specialises on
 * and must flush for -- are all behind fetch_guard or routed to a helper
 * here, so no block outlives anything it baked in. Adding any
 * translate-time decision to this file means giving it a generation.
 */

static bool jit_init(emu_cpu_t *cpu)
{
    (void)cpu;
    return emu_jit_init(RV_JIT_HOST_CODE_BYTES);
}

static void jit_reset(emu_cpu_t *cpu)
{
    (void)cpu;
    emu_jit_flush();
}

static void jit_invalidate(emu_cpu_t *cpu, uint32_t addr, uint32_t len)
{
    (void)cpu;
    (void)addr;
    (void)len;
    /* Blocks are not indexed by the guest memory they came from, so the
     * conservative answer is the only available one. FENCE.I is rare. */
    emu_jit_flush();
}

static emu_run_reason_t jit_run(emu_cpu_t *cpu, uint32_t budget,
                                uint32_t *retired)
{
    return emu_jit_run(cpu, budget, retired, &g_jit_ops);
}

void rv_jit_get_stats(rv_jit_stats_t *out)
{
    emu_jit_stats_t st;

    emu_jit_get_stats(&st);
    memset(out, 0, sizeof(*out));
    out->blocks = st.blocks;
    out->translations = st.translations;
    out->block_entries = st.block_entries;
    out->interp_fallbacks = st.interp_fallbacks;
    out->flushes = st.flushes;
    out->code_used = st.code_used;
    out->code_size = st.code_size;
}

void rv_jit_flush(void)
{
    emu_jit_flush();
}

const emu_backend_t rv_backend_jit = {
    .name       = "jit-x86-64",
    .init       = jit_init,
    .reset      = jit_reset,
    .run        = jit_run,
    .invalidate = jit_invalidate,
};

#endif /* RV_ENABLE_JIT && RV_JIT_X86_64 */
