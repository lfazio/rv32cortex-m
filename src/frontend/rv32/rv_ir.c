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
#if defined(EMU_JIT_THUMB2)
/* On a target these bytes are the guest's; see CLAUDE.md. */
#  define RV_JIT_HOST_CODE_BYTES RV_JIT_CODE_SIZE
#else
#  define RV_JIT_HOST_CODE_BYTES (4u * 1024u * 1024u)
#endif

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

#if RV_EXT_F
static uint32_t rv_freg_offset(uint32_t n)
{
    /*
     * sizeof rather than 4, because FLEN follows D: the file is 32 bits
     * wide without it and 64 with. It was written as 4 and stayed that
     * way when D widened the file, which was invisible only because
     * nothing emitted an FGET or an FPUT from that moment on -- every FP
     * instruction went to the helper. A stride that is right by not
     * being used is not right.
     */
    return (uint32_t)(offsetof(rv_hart_t, f) +
                      n * sizeof(((rv_hart_t *)0)->f[0]));
}

/*
 * The box, on the FGET and FPUT of a *single-precision* value.
 *
 * Zero without D, where FLEN is 32 and there is no upper half to fill.
 * Not every FP move wants it even with D -- FSW and FMV.X.W move bits
 * and take the low half raw -- so it is applied per instruction rather
 * than folded into rv_freg_offset. See rv_fpu.c, which makes the same
 * distinction in the same four places through fr32/fw32.
 */
#if RV_EXT_D
#  define RV_IR_BOX  EMU_IR_FP_BOX
#else
#  define RV_IR_BOX  0u
#endif

/*
 * One block's worth of floating-point side effects.
 *
 * The IR hands over its own neutral flag encoding, which is deliberately
 * RISC-V's fflags order -- so the accumulate is a mask and an OR, and
 * the two backends that had to reorder theirs did it once each rather
 * than the frontend doing it per host.
 *
 * FS is set to Dirty here too, which is the other thing an FP operation
 * owes the guest: a context switch reads FS to decide whether the
 * register file needs saving, and a block that computed floats without
 * marking them dirty loses them. Dirty is *not* part of the generation
 * key -- the key tracks off-ness -- which is what stops this write from
 * flushing the code cache every time a block uses a float.
 */
static void rv_ir_fp_flags(emu_cpu_t *cpu, uint32_t flags)
{
    rv_hart_t *const h = (rv_hart_t *)cpu;

    h->fcsr |= flags & 0x1Fu;
    h->mstatus |= MSTATUS_FS_MASK;
}

/*
 * The escape hatch, for everything a backend declined: FMIN/FMAX, the
 * classifications, the conversions a host has no encoding for, the fused
 * multiply-adds, and any rounding mode this host cannot spell.
 *
 * It is the same rv_hart_fp the interpreter uses, so nothing about
 * rounding, NaN handling or the flag rules exists twice -- which is what
 * makes routing to it cheaper than getting a second implementation
 * subtly wrong. Non-zero means it entered a trap and the block must
 * stop; the pc it records was written by the SETPC the caller emits.
 */
static uint32_t rv_ir_fp_helper(emu_cpu_t *cpu, uint32_t insn, uint32_t unused)
{
    rv_hart_t *const h = (rv_hart_t *)cpu;
    uint32_t tval = 0u;
    const rv_exc_t e = rv_hart_fp(h, insn, &tval);

    (void)unused;
    if (e == RV_EXC_NONE) {
        return 0u;
    }
    rv_hart_trap(h, e, tval);
    return 1u;
}

static const void *const rv_ir_helpers[] = { (const void *)rv_ir_fp_helper };
#define RV_IR_HELPER_FP 0u
#endif /* RV_EXT_F */

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
#if RV_EXT_F
    .helpers      = rv_ir_helpers,
    .helper_count = 1u,
    .freg_offset  = rv_freg_offset,
    .fp_flags     = rv_ir_fp_flags,
#else
    .helpers      = NULL,
    .helper_count = 0u,
#endif
    .load         = rv_ir_load,
    .store        = rv_ir_store,
};

/* ------------------------------------------------------------------ */
/* Translation                                                         */
/* ------------------------------------------------------------------ */

#if RV_EXT_F
/* Is the FP unit off? Every FP instruction is illegal while it is. */
static bool h_fs_off(const emu_cpu_t *cpu)
{
    return (((const rv_hart_t *)cpu)->mstatus & MSTATUS_FS_MASK) == 0u;
}

/* fcsr's frm, for resolving a "dynamic" rounding mode at translation. */
#define RV_IR_FRM(cpu)  ((((const rv_hart_t *)(cpu))->fcsr >> 5) & 7u)

/*
 * Route one FP instruction to rv_hart_fp. HELPER_TRAP rather than
 * HELPER: the helper can enter a trap, and then pc already points at
 * the handler and the rest of the block must not run.
 */
static bool rv_ir_fp_fallback(emu_ir_block_t *b, uint32_t pc, uint32_t insn)
{
    (void)emu_ir_emit(b, EMU_IR_SETPC, 0u, EMU_IR_NO_TEMP, EMU_IR_NO_TEMP,
                      pc, 0u);
    (void)emu_ir_emit(b, EMU_IR_HELPER_TRAP, 0u, emu_ir_const(b, insn),
                      EMU_IR_NO_TEMP, RV_IR_HELPER_FP, 0u);
    return true;
}
#endif

/*
 * Lower one instruction. Returns false for anything not modelled, which
 * ends the block with nothing emitted for it.
 *
 * `ends_block` is set when the instruction wrote pc itself, so the
 * caller stops rather than continuing at pc + 4.
 */
/*
 * `cpu` is read only for the state the block is *specialised* on -- the
 * rounding mode and whether the FP unit is on -- and both are carried in
 * rv_ir_gen_key, so a block outliving either gets flushed. Reading
 * anything else here would be a staleness bug; that is the rule, and
 * this file has one place it applies.
 */
static bool lower_one(emu_cpu_t *cpu, emu_ir_block_t *b, uint32_t insn,
                      uint32_t pc, uint32_t len, bool *ends_block,
                      bool *counted)
{
#if !RV_EXT_F
    (void)cpu;
#endif
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

        if (f7 == 1u) {                         /* M extension */
            /*
             * The multiplies only. Divide and remainder stay on the
             * interpreter: they need the guest's divide-by-zero and
             * overflow results, which RISC-V defines and x86 traps on,
             * so open-coding them means reproducing two special cases
             * per host rather than one helper.
             *
             * MULHSU is skipped for the same reason -- it is the one
             * form with no single host instruction behind it.
             */
            static const uint8_t k_mul[4] = {
                EMU_IR_MUL, EMU_IR_MULHS, 0u, EMU_IR_MULHU
            };
            if (f3 == 2u || f3 > 3u) {
                return false;
            }
            emu_ir_put(b, rd, emu_ir_alu(b, (emu_ir_op_t)k_mul[f3],
                                         emu_ir_get(b, rs1),
                                         emu_ir_get(b, rs2)));
            return true;
        }
        if (f7 != 0u && f7 != 0x20u) {
            return false;                       /* Zba/Zbb/Zbs, Zacas */
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

#if RV_EXT_F
    /*
     * The floating-point group.
     *
     * Everything here is gated on mstatus.FS being on *at translation*,
     * and that is only half a guard on its own -- a block outlives the
     * check. The other half is rv_ir_gen_key, which carries FS
     * off-ness, so the cache is flushed if the guest turns the unit off
     * under a block that assumed it was on.
     *
     * The rounding mode is resolved here rather than left dynamic, for
     * the reason the IR states: a host without an encoding for one --
     * neither x86-64 nor ARM has ties-away -- must be able to decline,
     * and it cannot decline something it only discovers at run time.
     * frm is in the generation key for the same reason FS is.
     */
    case 0x07u:                                 /* LOAD-FP  */
    case 0x27u: {                               /* STORE-FP */
        if (f3 != 2u || (h_fs_off(cpu))) {
            return false;                       /* not FLW/FSW, or FS off */
        }
        /*
         * **With D, FLEN is 64 and a float in an f register carries a
         * box.** These were sent to the helper for a while because
         * EMU_IR_FGET/FPUT moved 32 bits and could not say so: lowering
         * FLW wrote an unboxed register and lowering FSW read one, and
         * neither could tell. The whole point of the boxing is that the
         * *next* reader of that register sees a canonical NaN, so
         * nothing about the instruction's own result looks wrong; it
         * fails somewhere else. 93 of 378 tests, and F-fsub.s among
         * them.
         *
         * EMU_IR_FP_BOX is what lets them come back. Note the asymmetry,
         * which is the architecture's and not an oversight: FLW *writes*
         * a single and boxes it, FSW stores the low half **raw**. A
         * store is bits, and putting it through the unboxing read would
         * turn an unboxed register into a canonical NaN in memory.
         */
        (void)emu_ir_emit(b, EMU_IR_SETPC, 0u, EMU_IR_NO_TEMP,
                          EMU_IR_NO_TEMP, pc, 0u);
        const uint16_t base = emu_ir_get(b, rs1);

        if (op == 0x07u) {
            const uint16_t v = emu_ir_emit(b, EMU_IR_LOAD,
                                           EMU_IR_MEM_AUX(4u, 0u), base,
                                           EMU_IR_NO_TEMP,
                                           (uint32_t)rv_imm_i(insn), 0u);
            (void)emu_ir_emit(b, EMU_IR_FPUT, RV_IR_BOX, v, EMU_IR_NO_TEMP,
                              rd, 0u);
        } else {
            const uint16_t v = emu_ir_emit(b, EMU_IR_FGET, 0u, EMU_IR_NO_TEMP,
                                           EMU_IR_NO_TEMP, rs2, 0u);
            (void)emu_ir_emit(b, EMU_IR_STORE, EMU_IR_MEM_AUX(4u, 0u),
                              base, v, (uint32_t)rv_imm_s(insn), 0u);
        }
        return true;
    }

    case 0x43u: case 0x47u: case 0x4Bu: case 0x4Fu:
        /*
         * The fused multiply-adds, which the IR does not model: it has
         * two operand fields and a third would hide from the use
         * counter and the register allocator. They go to the helper,
         * which keeps the block whole and gets the single rounding
         * right by reusing the code that already does it.
         */
        if (h_fs_off(cpu)) {
            return false;
        }
        return rv_ir_fp_fallback(b, pc, insn);

    case 0x53u: {                               /* OP-FP */
        if (h_fs_off(cpu)) {
            return false;
        }
        const uint32_t f7 = insn >> 25;

        /*
         * Everything that rounds, classifies or reports a flag goes to
         * rv_hart_fp, which is Berkeley SoftFloat.
         *
         * SoftFloat *is* the FP unit here -- see the note in
         * CMakeLists.txt -- so lowering an FP operation to the host's
         * own instructions makes a second implementation of semantics
         * the core already owns. The two then disagree exactly where the
         * architecture is fussiest: NaN propagation, subnormals, and
         * which of fflags an operation may raise. Measured on the
         * official suite, rv32i/F: the interpreter passes 78/78 on
         * SoftFloat, and the same binary with --jit lowering natively
         * passed 55/78. Twenty-three tests, every one of them the host
         * FPU being asked to be a RISC-V FPU.
         *
         * This is the rule the fused multiply-adds above already follow,
         * and the one CLAUDE.md states for FMIN/FMAX and FCLASS on
         * Thumb-2: a helper call is a translation, declining is not, and
         * open-coding is a second copy of the semantics. The block stays
         * whole either way -- only the arithmetic moves.
         *
         * What is left here is what cannot round. FMV.X.W and FMV.W.X
         * move bits between the register files and involve no
         * arithmetic at all, so they stay lowered; FP loads and stores
         * are memory and are lowered in their own cases above.
         *
         * To make this faster, bring operations back one at a time, each
         * measured against the F suite -- not the whole table on the
         * argument that the host has an FPU. FMUL.S is the first, and
         * the reason it can be is that the objection above was never
         * about *arithmetic*: add, subtract, multiply and divide are the
         * four operations IEEE 754 specifies exactly, so a compliant
         * host computes the same bits SoftFloat does. What differed was
         * everything around them -- which NaN comes out, and which
         * fflags get raised -- and both are now the backend's job:
         * canonicalisation after the operation, MXCSR framed across the
         * block. The rest of the table stays on the helper because the
         * rest of the table is where hosts genuinely disagree.
         */
        if (f7 == 0x70u && f3 == 0u && rs2 == 0u) {     /* FMV.X.W */
            /*
             * No box on the read. FMV.X.W moves bits, and putting it
             * through the unboxing form would turn an unboxed register
             * into the canonical NaN -- "no interpretation" is the whole
             * instruction. rv_fpu.c says the same thing beside its own
             * copy of this.
             */
            emu_ir_put(b, rd,
                       emu_ir_emit(b, EMU_IR_FGET, 0u, EMU_IR_NO_TEMP,
                                   EMU_IR_NO_TEMP, rs1, 0u));
            return true;
        }
        if (f7 == 0x78u && f3 == 0u && rs2 == 0u) {     /* FMV.W.X */
            (void)emu_ir_emit(b, EMU_IR_FPUT, RV_IR_BOX, emu_ir_get(b, rs1),
                              EMU_IR_NO_TEMP, rd, 0u);
            return true;
        }
        if (f7 == 0x08u) {                              /* FMUL.S */
            /*
             * "dyn" is resolved here, at translation, which is the
             * deliberate arrangement: it lets a backend decline a mode
             * it has no encoding for -- RMM, which neither host has --
             * instead of silently rounding some other way. frm is in
             * rv_ir_gen_key, so a block specialised on it is flushed if
             * the guest changes it. Modes 5 and 6 are reserved and 7 is
             * only meaningful in the instruction, so anything that is
             * not one of the five real ones falls through to the helper,
             * which is where illegal-instruction is decided.
             */
            const uint32_t rm = (f3 == 7u) ? RV_IR_FRM(cpu) : f3;

            if (rm <= EMU_IR_FRM_RMM &&
                emu_ir_can_lower(EMU_IR_FMUL, (uint8_t)rm)) {
                const uint16_t x = emu_ir_emit(b, EMU_IR_FGET, RV_IR_BOX,
                                               EMU_IR_NO_TEMP,
                                               EMU_IR_NO_TEMP, rs1, 0u);
                const uint16_t y = emu_ir_emit(b, EMU_IR_FGET, RV_IR_BOX,
                                               EMU_IR_NO_TEMP,
                                               EMU_IR_NO_TEMP, rs2, 0u);
                const uint16_t r = emu_ir_emit(b, EMU_IR_FMUL, (uint8_t)rm,
                                               x, y, 0u, 0u);

                (void)emu_ir_emit(b, EMU_IR_FPUT, RV_IR_BOX, r,
                                  EMU_IR_NO_TEMP, rd, 0u);
                return true;
            }
        }
        return rv_ir_fp_fallback(b, pc, insn);
    }
#endif /* RV_EXT_F */

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

        if (!lower_one(cpu, b, insn, cur, len, &ends, &counted)) {
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

/*
 * Everything a translated block is specialised on, as one word.
 *
 * The framework compares this on each block entry and flushes the cache
 * when it moves, so it has to change whenever anything baked in at
 * translation does -- and must *not* change for anything else, because
 * every spurious change throws away the whole cache.
 *
 * Three things go in, and each is here for a recorded reason:
 *
 *   frm    the IR resolves a "dynamic" rounding mode at translation, on
 *          purpose, so that a backend without an encoding for one --
 *          neither x86 nor ARM has ties-away -- can decline the block
 *          rather than round differently. That specialisation is only
 *          sound if changing frm invalidates it.
 *   FS     an FP instruction is legal only while the unit is on. A
 *          translate-time check is half a guard: a block built while FS
 *          was on keeps executing after the guest turns the FPU off, and
 *          three instructions that must raise illegal-instruction run
 *          silently. That has happened here before.
 *   vm_gen mappings change under blocks keyed on virtual addresses.
 *          Not strictly needed while the IR path declines to run at all
 *          under paging, but free, and the alternative is remembering to
 *          add it at the moment that stops being true.
 *
 * FS is reduced to *off or not*, not carried as the two-bit field. An FP
 * operation moves it Initial or Clean to Dirty as a side effect, so
 * keeping the field would flush the cache on the first float of every
 * block -- which is a correctness-preserving way to have no JIT at all.
 */
static uint32_t rv_ir_gen_key(const rv_hart_t *h)
{
#if RV_EXT_F
    const uint32_t fs_off = ((h->mstatus & MSTATUS_FS_MASK) == 0u)
                                ? 1u : 0u;
    const uint32_t frm = (h->fcsr >> 5) & 7u;

    return (h->vm_gen << 8) | (fs_off << 4) | frm;
#else
    return h->vm_gen;
#endif
}

/*
 * Re-derived here rather than in `generation` itself, which is read on
 * every block entry -- CoreMark enters blocks 2.9 million times a run.
 *
 * Once per interpreted instruction is enough, and is exactly the right
 * place: frm and mstatus.FS move only through a CSR write, a SYSTEM
 * instruction this translator declines, so every such write lands on the
 * interpreter fallback. Nothing else alters either -- a trap does not
 * touch FS, and mret restores privilege rather than extension state.
 */
static void rv_jit_after_interp(emu_cpu_t *cpu)
{
    rv_hart_t *const h = (rv_hart_t *)cpu;

    h->jit_gen = rv_ir_gen_key(h);
}

static void rv_jit_bind(emu_cpu_t *cpu, emu_jit_hot_t *out)
{
    rv_hart_t *const h = (rv_hart_t *)cpu;

    /*
     * Seeded here as well as maintained by after_interp, so the very
     * first block entry compares against the state it was translated
     * under rather than against zero.
     */
    h->jit_gen = rv_ir_gen_key(h);

    out->pc = &h->pc;
    out->state = &h->state;
    out->blocked = &h->fetch_guard;
    out->generation = &h->jit_gen;
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
    .after_interp = rv_jit_after_interp,
    .code_bytes   = RV_JIT_HOST_CODE_BYTES,
    /* x[0..31] and pc; everything past it is bus pointers and counters. */
    .diff_state_bytes = (uint32_t)offsetof(rv_hart_t, pc) + 4u,
};
