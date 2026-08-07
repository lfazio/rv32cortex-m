/* SPDX-License-Identifier: Apache-2.0 */
/*
 * rv_ir.c - RV32 to IR.
 *
 * The frontend half of frontend -> IR -> optimisation -> backend for
 * RISC-V. It names no host.
 *
 * RISC-V has no condition flags, and that shows up here as an absence:
 * this file never emits EMU_IR_SETF, so the shared dead-flag pass finds
 * nothing to do and costs nothing. `SLT` and `SLTU` become EMU_IR_SETCC,
 * which compares two values directly rather than setting flags and
 * reading them back -- inventing a flag word this architecture does not
 * have would defeat the pass for the one frontend that should pay
 * nothing for it.
 *
 * What is *not* here, and why
 * ---------------------------
 * Loads, stores and the FP groups go through EMU_IR_HELPER_TRAP to the
 * same helpers the direct translator used, rather than being open-coded.
 * That is the choice this project has already measured twice: inlining a
 * memory access on the ARM backend cost it the LR/SC reservation, then
 * PMP, then address translation, each found on hardware rather than by a
 * suite. Everything rv_hart_load and rv_hart_store do beyond the access
 * itself is had for free by calling them, and on a host that predicts
 * calls well the cost is small.
 *
 * SYSTEM, the atomics and anything else end the block and land on the
 * interpreter, which is where the state they touch can be observed.
 */

#include "rv32/rv_hart.h"
#include "rv32/rv_decode.h"
#include "rv32/rv_ir.h"
#include "rv32/rv_jit.h"

#include <stddef.h>

/*
 * Guest instructions folded into one block. Interrupts are delivered
 * between blocks, so this bounds interrupt latency as much as code size.
 */
#define RV_IR_MAX_BLOCK_INSNS 64u

/*
 * Code cache on a host. Generous where the framework's microcontroller
 * default is not: on a target those bytes are the guest's, but here
 * constant retranslation is what would mask a translator bug behind a
 * fresh translation -- at 12 KB CoreMark flushed nineteen times a run.
 */
#define RV_JIT_HOST_CODE_BYTES (4u * 1024u * 1024u)

extern const emu_backend_t rv_backend_interp;

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

/*
 * The generic accessors the shared lowering calls.
 *
 * The guest pc already points at the faulting instruction when either is
 * reached -- the frontend emits EMU_IR_SETPC ahead of every memory
 * operation -- so rv_hart_trap records the right address.
 */
static uint32_t rv_ir_load(emu_cpu_t *cpu, uint32_t addr, uint32_t spec,
                           uint32_t *out)
{
    rv_hart_t *const h = (rv_hart_t *)cpu;
    const rv_exc_t exc = rv_hart_load(h, addr, EMU_IR_MEM_SIZE(spec),
                                      (spec & EMU_IR_MEM_SIGNED) != 0u, out);

    if (EMU_UNLIKELY(exc != RV_EXC_NONE)) {
        rv_hart_trap(h, exc, addr);
        return 1u;
    }
    return 0u;
}

static uint32_t rv_ir_store(emu_cpu_t *cpu, uint32_t addr, uint32_t spec,
                            uint32_t val)
{
    rv_hart_t *const h = (rv_hart_t *)cpu;
    const rv_exc_t exc = rv_hart_store(h, addr, EMU_IR_MEM_SIZE(spec), val);

    if (EMU_UNLIKELY(exc != RV_EXC_NONE)) {
        rv_hart_trap(h, exc, addr);
        return 1u;
    }
    return 0u;
}

/* ------------------------------------------------------------------ */
/* The target descriptor                                               */
/* ------------------------------------------------------------------ */

static uint32_t rv_reg_offset(uint32_t n)
{
    return (uint32_t)offsetof(rv_hart_t, x) + n * 4u;
}

/* x0 reads as zero and discards writes. */
static bool rv_reg_zero(uint32_t n)
{
    return n == 0u;
}

const emu_ir_target_t rv_ir_target = {
    .reg_offset   = rv_reg_offset,
    /*
     * No condition flags at all. flags_offset is unused while every
     * flag_bit is zero, and nothing this file emits reads or writes
     * them -- stated here rather than left implicit, because a backend
     * that assumed a flag word existed would otherwise write into
     * whatever sits at offset zero of the hart.
     */
    .flags_offset = 0u,
    .flag_bit     = { 0u, 0u, 0u, 0u },
    .reg_is_zero  = rv_reg_zero,
    .pc_offset    = (uint32_t)offsetof(rv_hart_t, pc),
    .helpers      = NULL,
    .helper_count = 0u,
    .load         = rv_ir_load,
    .store        = rv_ir_store,
};

/* ------------------------------------------------------------------ */
/* Translation                                                         */
/* ------------------------------------------------------------------ */

/*
 * Lower one instruction. Returns false for anything not modelled, which
 * ends the block with nothing emitted for it.
 *
 * `ends_block` is set when the instruction wrote pc itself, so the
 * caller stops rather than continuing at pc + 4.
 */
static bool lower_one(emu_ir_block_t *b, uint32_t insn, uint32_t pc,
                      uint32_t len, bool *ends_block, bool *counted)
{
    const uint32_t op = insn & 0x7Fu;
    const uint32_t rd = rv_rd(insn);
    const uint32_t rs1 = rv_rs1(insn);
    const uint32_t rs2 = rv_rs2(insn);
    const uint32_t f3 = rv_funct3(insn);

    *ends_block = false;
    *counted = false;

    switch (op) {
    case 0x37u:                                 /* LUI   */
        emu_ir_put(b, rd, emu_ir_const(b, (uint32_t)rv_imm_u(insn)));
        return true;

    case 0x17u:                                 /* AUIPC */
        emu_ir_put(b, rd, emu_ir_const(b, pc + (uint32_t)rv_imm_u(insn)));
        return true;

    case 0x13u: {                               /* OP-IMM */
        const uint16_t x = emu_ir_get(b, rs1);
        const uint32_t imm = (uint32_t)rv_imm_i(insn);
        const uint32_t sh = insn >> 25;

        switch (f3) {
        case 0u:
            emu_ir_put(b, rd, emu_ir_alu(b, EMU_IR_ADD, x,
                                         emu_ir_const(b, imm)));
            return true;
        case 4u:
            emu_ir_put(b, rd, emu_ir_alu(b, EMU_IR_XOR, x,
                                         emu_ir_const(b, imm)));
            return true;
        case 6u:
            emu_ir_put(b, rd, emu_ir_alu(b, EMU_IR_OR, x,
                                         emu_ir_const(b, imm)));
            return true;
        case 7u:
            emu_ir_put(b, rd, emu_ir_alu(b, EMU_IR_AND, x,
                                         emu_ir_const(b, imm)));
            return true;
        case 2u:
            emu_ir_put(b, rd, emu_ir_emit(b, EMU_IR_SETCC, EMU_IR_C_LT, x,
                                          emu_ir_const(b, imm), 0u, 0u));
            return true;
        case 3u:
            emu_ir_put(b, rd, emu_ir_emit(b, EMU_IR_SETCC, EMU_IR_C_LTU, x,
                                          emu_ir_const(b, imm), 0u, 0u));
            return true;
        case 1u:
            /*
             * Only shift-left lives here; every other funct7 in this
             * slot belongs to Zba/Zbb/Zbs, which share it. Declining on
             * anything but zero is the rule this project learned the
             * hard way: an extension sharing an opcode slot that is
             * decoded in one place, or it is decoded wrongly.
             */
            if (sh != 0u) {
                return false;
            }
            emu_ir_put(b, rd, emu_ir_emit(b, EMU_IR_SHLI, 0u, x,
                                          EMU_IR_NO_TEMP,
                                          insn >> 20 & 31u, 0u));
            return true;
        case 5u:
            if (sh == 0u) {
                emu_ir_put(b, rd, emu_ir_emit(b, EMU_IR_SHRI, 0u, x,
                                              EMU_IR_NO_TEMP,
                                              insn >> 20 & 31u, 0u));
                return true;
            }
            if (sh == 0x20u) {
                emu_ir_put(b, rd, emu_ir_emit(b, EMU_IR_SARI, 0u, x,
                                              EMU_IR_NO_TEMP,
                                              insn >> 20 & 31u, 0u));
                return true;
            }
            return false;
        default:
            return false;
        }
    }

    case 0x33u: {                               /* OP */
        const uint32_t f7 = insn >> 25;
        if (f7 != 0u && f7 != 0x20u) {
            return false;                       /* M, Zba/Zbb/Zbs, Zacas */
        }
        const uint16_t x = emu_ir_get(b, rs1);
        const uint16_t y = emu_ir_get(b, rs2);
        const bool alt = (f7 == 0x20u);

        switch (f3) {
        case 0u:
            emu_ir_put(b, rd, emu_ir_alu(b, alt ? EMU_IR_SUB : EMU_IR_ADD,
                                         x, y));
            return true;
        case 1u:
            if (alt) { return false; }
            emu_ir_put(b, rd, emu_ir_alu(b, EMU_IR_SHL, x, y));
            return true;
        case 2u:
            if (alt) { return false; }
            emu_ir_put(b, rd, emu_ir_emit(b, EMU_IR_SETCC, EMU_IR_C_LT,
                                          x, y, 0u, 0u));
            return true;
        case 3u:
            if (alt) { return false; }
            emu_ir_put(b, rd, emu_ir_emit(b, EMU_IR_SETCC, EMU_IR_C_LTU,
                                          x, y, 0u, 0u));
            return true;
        case 4u:
            if (alt) { return false; }
            emu_ir_put(b, rd, emu_ir_alu(b, EMU_IR_XOR, x, y));
            return true;
        case 5u:
            emu_ir_put(b, rd, emu_ir_alu(b, alt ? EMU_IR_SAR : EMU_IR_SHR,
                                         x, y));
            return true;
        case 6u:
            if (alt) { return false; }
            emu_ir_put(b, rd, emu_ir_alu(b, EMU_IR_OR, x, y));
            return true;
        case 7u:
            if (alt) { return false; }
            emu_ir_put(b, rd, emu_ir_alu(b, EMU_IR_AND, x, y));
            return true;
        default:
            return false;
        }
    }

    case 0x03u: {                               /* LOAD */
        /* pc first: a fault records it. */
        (void)emu_ir_emit(b, EMU_IR_SETPC, 0u, EMU_IR_NO_TEMP,
                          EMU_IR_NO_TEMP, pc, 0u);
        const uint16_t base = emu_ir_get(b, rs1);
        const uint8_t spec = EMU_IR_MEM_AUX(1u << (f3 & 3u),
                                            (f3 & 4u) == 0u);

        emu_ir_put(b, rd, emu_ir_emit(b, EMU_IR_LOAD, spec, base,
                                      EMU_IR_NO_TEMP,
                                      (uint32_t)rv_imm_i(insn), 0u));
        return true;
    }

    case 0x23u: {                               /* STORE */
        (void)emu_ir_emit(b, EMU_IR_SETPC, 0u, EMU_IR_NO_TEMP,
                          EMU_IR_NO_TEMP, pc, 0u);
        const uint16_t base = emu_ir_get(b, rs1);
        const uint16_t val = emu_ir_get(b, rs2);
        const uint8_t spec = EMU_IR_MEM_AUX(1u << (f3 & 3u), 0u);

        (void)emu_ir_emit(b, EMU_IR_STORE, spec, base, val,
                          (uint32_t)rv_imm_s(insn), 0u);
        return true;
    }

    case 0x63u: {                               /* BRANCH */
        static const uint8_t k_cond[8] = {
            EMU_IR_C_EQ, EMU_IR_C_NE, 0u, 0u,
            EMU_IR_C_LT, EMU_IR_C_GE, EMU_IR_C_LTU, EMU_IR_C_GEU
        };
        if (f3 == 2u || f3 == 3u) {
            return false;                       /* reserved */
        }
        const uint16_t x = emu_ir_get(b, rs1);
        const uint16_t y = emu_ir_get(b, rs2);

        /*
         * Retire before the exit, not after. A taken branch *does*
         * retire -- it is the trapping helper above that does not -- and
         * a RETIRE placed after EXIT_IF is skipped on exactly the taken
         * path, which undercounts every loop back edge in the guest.
         */
        (void)emu_ir_emit(b, EMU_IR_RETIRE, 0u, EMU_IR_NO_TEMP,
                          EMU_IR_NO_TEMP, 0u, 0u);
        (void)emu_ir_emit(b, EMU_IR_EXIT_IF, k_cond[f3], x, y,
                          pc + (uint32_t)rv_imm_b(insn), 0u);
        *counted = true;
        return true;
    }

    case 0x6Fu:                                 /* JAL */
        /*
         * The link is the address after *this* instruction, which for an
         * expanded C.JAL is two bytes on, not four. Passed in rather
         * than assumed, because the expansion produces a 32-bit encoding
         * that looks four bytes long.
         */
        emu_ir_put(b, rd, emu_ir_const(b, pc + len));
        (void)emu_ir_emit(b, EMU_IR_RETIRE, 0u, EMU_IR_NO_TEMP,
                          EMU_IR_NO_TEMP, 0u, 0u);
        (void)emu_ir_emit(b, EMU_IR_EXIT, 0u, EMU_IR_NO_TEMP,
                          EMU_IR_NO_TEMP, pc + (uint32_t)rv_imm_j(insn), 0u);
        *ends_block = true;
        *counted = true;
        return true;

    case 0x67u: {                               /* JALR */
        /*
         * The target is computed *before* the link register is written,
         * because `jalr ra, ra` -- an indirect call through a saved
         * pointer -- names the same register twice and would otherwise
         * jump to the return address it just wrote.
         */
        const uint16_t base = emu_ir_get(b, rs1);
        const uint16_t sum = emu_ir_alu(b, EMU_IR_ADD, base,
                                        emu_ir_const(b,
                                                     (uint32_t)rv_imm_i(insn)));
        const uint16_t tgt = emu_ir_alu(b, EMU_IR_AND, sum,
                                        emu_ir_const(b, ~1u));

        emu_ir_put(b, rd, emu_ir_const(b, pc + len));
        (void)emu_ir_emit(b, EMU_IR_RETIRE, 0u, EMU_IR_NO_TEMP,
                          EMU_IR_NO_TEMP, 0u, 0u);
        (void)emu_ir_emit(b, EMU_IR_EXIT, 0u, tgt, EMU_IR_NO_TEMP, 0u, 0u);
        *ends_block = true;
        *counted = true;
        return true;
    }

    default:
        return false;
    }
}

uint32_t rv_ir_translate(emu_cpu_t *cpu, uint32_t pc, emu_ir_block_t *b)
{
    rv_hart_t *const h = (rv_hart_t *)cpu;
    uint32_t cur = pc;
    uint32_t count = 0u;

    emu_ir_reset(b);

    while (count < RV_IR_MAX_BLOCK_INSNS && !b->overflow) {
        uint32_t insn;

        uint16_t lo, hi;
        uint32_t len;

        if (emu_bus_fetch16(h->bus, cur, &lo) != EMU_FAULT_NONE) {
            break;
        }
        if (rv_is_32bit(lo)) {
            if (emu_bus_fetch16(h->bus, cur + 2u, &hi) != EMU_FAULT_NONE) {
                break;
            }
            insn = (uint32_t)lo | ((uint32_t)hi << 16);
            len = 4u;
        } else {
            /*
             * Compressed encodings are expanded to their 32-bit
             * equivalent and lowered by the same code, which is what the
             * expander exists for. Skipping them instead is not a
             * correctness problem -- the interpreter picks them up --
             * but it is a coverage one: this guest is built with C, so
             * ending the block at every compressed instruction left 57%
             * of CoreMark interpreted against the direct translator's
             * 8%, and a backend that mostly falls back proves mostly
             * nothing.
             *
             * A zero expansion is a permanently-illegal encoding, so it
             * falls out through lower_one's default and ends the block.
             */
            insn = rv_decode_expand_c(lo);
            if (insn == 0u) {
                break;
            }
            len = 2u;
        }

        const uint32_t mark = b->count;
        bool ends = false;
        bool counted = false;

        if (!lower_one(b, insn, cur, len, &ends, &counted)) {
            /*
             * Discard whatever the attempt emitted: a lowering can emit
             * operands before reaching the funct7 that tells it to
             * decline, and a half-instruction left behind does not
             * fault -- it quietly computes something else.
             */
            b->count = mark;
            break;
        }

        cur += len;
        count++;
        if (!counted) {
            (void)emu_ir_emit(b, EMU_IR_RETIRE, 0u, EMU_IR_NO_TEMP,
                              EMU_IR_NO_TEMP, 0u, 0u);
        }

        /*
         * pc after every instruction, so a trap, an interrupt or the
         * interpreter resuming sees the right address. A block whose pc
         * only became correct at the end would send all three to the
         * wrong place.
         */
        if (ends) {
            /*
             * The instruction wrote pc itself. Emitting the usual
             * pc-after-this-instruction store here would overwrite a
             * jump target with the fall-through address.
             */
            break;
        }
        (void)emu_ir_emit(b, EMU_IR_SETPC, 0u, EMU_IR_NO_TEMP,
                          EMU_IR_NO_TEMP, cur, 0u);
    }

    if (b->overflow) {
        return 0u;
    }
    b->guest_insns = count;
    return count;
}

/* ------------------------------------------------------------------ */
/* What the host's jit.c needs from this frontend                      */
/* ------------------------------------------------------------------ */

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

const emu_ir_frontend_t rv_ir_frontend = {
    .name         = "rv32",
    .translate    = rv_ir_translate,
    .target       = &rv_ir_target,
    .bind         = rv_jit_bind,
    .interp       = &rv_backend_interp,
    .is_idle      = rv_jit_is_idle,
    .wake         = rv_jit_wake,
    .take_irq     = rv_jit_take_irq,
    .count        = rv_jit_count,
    .after_interp = NULL,
    .code_bytes   = RV_JIT_HOST_CODE_BYTES,
};
