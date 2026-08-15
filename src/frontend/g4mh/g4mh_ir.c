/* SPDX-License-Identifier: Apache-2.0 */
/*
 * g4mh_ir.c - RH850 G4MH to IR.
 *
 * The frontend half of frontend -> IR -> optimisation -> backend. It
 * knows the guest and nothing about any host: no register names, no
 * encodings, no calling convention. Everything it emits goes through
 * emu_ir.h, and what happens to it afterwards -- compiled by
 * src/backend/<host>/jit.c or evaluated by that host's interp.c -- is
 * not this file's business.
 *
 * What that buys, concretely, over the direct translator it replaces:
 * the hand-written one materialised the four PSW flags after *every*
 * arithmetic instruction, because it could not see whether anything
 * would read them. Here the flags are a separate EMU_IR_SETF and the
 * shared optimiser deletes the ones nothing reads. On this guest that is
 * the single largest saving available, since G4MH sets flags on almost
 * everything and x86-64 needs a seto/lahf pair plus masking to
 * materialise them.
 *
 * On flag masks
 * -------------
 * G4MH's arithmetic and logical instructions do *not* define the same
 * set, and the difference is easy to miss because both "set the flags":
 *
 *   ADD/SUB/CMP   Z, S, OV, CY
 *   OR/XOR/AND    Z, S, OV(=0)      -- CY is left exactly as it was
 *
 * Declaring CY as defined by a logical operation would let the optimiser
 * delete an earlier definition of CY that a later conditional still
 * reads, which is a wrong branch rather than a wrong value. The masks
 * below are therefore per-instruction, not per-category.
 */

#include "g4mh/g4mh_cpu.h"
#include "g4mh/g4mh_decode.h"

#include "emu/emu_ir.h"
#include "g4mh/g4mh_intc.h"

#include <stddef.h>

/*
 * Guest instructions folded into one block. The same 64 the direct
 * translator used: interrupts are delivered between blocks, so this is
 * an interrupt-latency bound as much as a code-size one.
 */
#define G4MH_IR_MAX_BLOCK_INSNS 64u

/*
 * Code cache on a host. Generous where the framework's microcontroller
 * default is not: on a target those bytes are the guest's, but here
 * constant retranslation is what would mask a translator bug behind a
 * fresh translation.
 */
#define G4MH_JIT_CODE_BYTES (4u * 1024u * 1024u)

/* Flags each group defines; see the note above. */
#define F_ARITH (EMU_IR_F_Z | EMU_IR_F_S | EMU_IR_F_V | EMU_IR_F_C)
#define F_LOGIC (EMU_IR_F_Z | EMU_IR_F_S | EMU_IR_F_V)

/* ------------------------------------------------------------------ */
/* Memory                                                              */
/* ------------------------------------------------------------------ */

/*
 * The generic accessors the shared lowering calls, going to the same
 * g4mh_load/g4mh_store the interpreter uses so alignment checks and the
 * bus policy are had for free.
 *
 * g4mh_cpu_exception takes the return address, and for a data abort that
 * is the faulting instruction itself. pc already holds it: the frontend
 * emits EMU_IR_SETPC before every memory operation for exactly this.
 */
static uint32_t g4mh_ir_load(emu_cpu_t *cpu, uint32_t addr, uint32_t spec,
                             uint32_t *out)
{
    g4mh_cpu_t *const c = (g4mh_cpu_t *)cpu;
    const g4mh_exc_t e = g4mh_load(c, addr, EMU_IR_MEM_SIZE(spec),
                                   (spec & EMU_IR_MEM_SIGNED) != 0u, out);

    if (EMU_UNLIKELY(e != G4MH_EXC_NONE)) {
        g4mh_cpu_exception(c, e, c->pc);
        return 1u;
    }
    return 0u;
}

static uint32_t g4mh_ir_store(emu_cpu_t *cpu, uint32_t addr, uint32_t spec,
                              uint32_t val)
{
    g4mh_cpu_t *const c = (g4mh_cpu_t *)cpu;
    const g4mh_exc_t e = g4mh_store(c, addr, EMU_IR_MEM_SIZE(spec), val);

    if (EMU_UNLIKELY(e != G4MH_EXC_NONE)) {
        g4mh_cpu_exception(c, e, c->pc);
        return 1u;
    }
    return 0u;
}

/* ------------------------------------------------------------------ */
/* The target descriptor                                               */
/* ------------------------------------------------------------------ */

static uint32_t g4mh_reg_offset(uint32_t n)
{
    return (uint32_t)offsetof(g4mh_cpu_t, r) + n * 4u;
}

/* r0 reads as zero and discards writes. */
static bool g4mh_reg_zero(uint32_t n)
{
    return n == 0u;
}

const emu_ir_target_t g4mh_ir_target = {
    .reg_offset   = g4mh_reg_offset,
    .flags_offset = (uint32_t)offsetof(g4mh_cpu_t, psw),
    /*
     * The IR's neutral flags mapped onto PSW. Order is Z, S, V, C, which
     * is the order of EMU_IR_F_* and *not* the order of the bits in PSW
     * -- the array is indexed by the IR's bit position, not the guest's.
     */
    .flag_bit     = { G4MH_PSW_Z, G4MH_PSW_S, G4MH_PSW_OV, G4MH_PSW_CY },
    .reg_is_zero  = g4mh_reg_zero,
    .pc_offset    = (uint32_t)offsetof(g4mh_cpu_t, pc),
    .helpers      = NULL,
    .helper_count = 0u,
    .load         = g4mh_ir_load,
    .store        = g4mh_ir_store,
};

/* ------------------------------------------------------------------ */
/* Translation                                                         */
/* ------------------------------------------------------------------ */

/*
 * Lower one 32-bit instruction, given both halfwords.
 *
 * Only the disp16 load and store group so far. The width of the halfword
 * and word forms is carried in *bit 0 of the displacement* rather than
 * in the opcode -- a halfword access is 2-byte aligned and a word access
 * 4-byte, so that bit is free and the encoding spends it -- which means
 * the displacement has to be masked before it is used, and a lowering
 * that forgot would be off by one on every odd-looking displacement.
 */
static bool lower_one32(emu_ir_block_t *b, uint16_t w0, uint16_t w1,
                        uint32_t pc)
{
    const uint32_t r1 = g4mh_reg1(w0);
    const uint32_t r2 = g4mh_reg2(w0);
    const uint32_t op = g4mh_op6(w0);

    /*
     * Address formation. Both share their slot with a 48-bit encoding
     * selected by reg2 == 0 -- MOVEA with MOV imm32, MOVHI with DISPOSE
     * -- and this frontend has already been caught once by an ISA that
     * reuses a register field as an opcode extension: decoding on the
     * opcode alone retired six unimplemented instructions as writes into
     * r0. Neither touches the flags.
     */
    if (op == 0x31u || op == 0x32u) {
        if (r2 == 0u) {
            return false;
        }
        const uint32_t imm = (op == 0x31u)
                                 ? (uint32_t)emu_sext(w1, 16)
                                 : ((uint32_t)w1 << 16);

        emu_ir_put(b, r2, emu_ir_alu(b, EMU_IR_ADD, emu_ir_get(b, r1),
                                     emu_ir_const(b, imm)));
        return true;
    }

    /*
     * Format VIII: the bit-manipulation group on memory. The operation
     * selector is bits[15:14] -- the *top* of the field every other
     * 32-bit format uses for reg2 -- with the 3-bit bit number below it,
     * so reading a register out of [15:11] here gets nonsense.
     */
    if (op == 0x3Eu) {
        static const uint8_t k_bitop[4] = {
            EMU_IR_BITOP_SET, EMU_IR_BITOP_INV,
            EMU_IR_BITOP_CLR, EMU_IR_BITOP_TST
        };
        const uint32_t sel = (w0 >> 14) & 3u;
        const uint32_t bit = (w0 >> 11) & 7u;

        (void)emu_ir_emit(b, EMU_IR_SETPC, 0u, EMU_IR_NO_TEMP,
                          EMU_IR_NO_TEMP, pc, 0u);
        emu_ir_bitop(b, (emu_ir_op_t)k_bitop[sel], emu_ir_get(b, r1),
                     emu_ir_const(b, bit),
                     (uint32_t)emu_sext(w1, 16), 1u);
        return true;
    }

    if (op < 0x38u || op > 0x3Bu) {
        return false;
    }

    /*
     * pc first: a data abort records it, and the store that normally
     * follows an instruction has not run yet.
     */
    (void)emu_ir_emit(b, EMU_IR_SETPC, 0u, EMU_IR_NO_TEMP, EMU_IR_NO_TEMP,
                      pc, 0u);

    const bool byte = (op == 0x38u) || (op == 0x3Au);
    const uint32_t size = byte ? 1u : ((w1 & 1u) ? 4u : 2u);
    const uint32_t disp = byte ? (uint32_t)emu_sext(w1, 16)
                               : (uint32_t)emu_sext(w1 & 0xFFFEu, 16);
    const uint16_t base = emu_ir_get(b, r1);

    if (op <= 0x39u) {                              /* LD.B / LD.H / LD.W */
        /* Byte and halfword loads sign-extend; the word form has
         * nothing to extend. */
        const uint8_t spec = EMU_IR_MEM_AUX(size, size != 4u);

        emu_ir_put(b, r2, emu_ir_emit(b, EMU_IR_LOAD, spec, base,
                                      EMU_IR_NO_TEMP, disp, 0u));
        return true;
    }

    (void)emu_ir_emit(b, EMU_IR_STORE, EMU_IR_MEM_AUX(size, 0u), base,
                      emu_ir_get(b, r2), disp, 0u);
    return true;
}

/*
 * Lower one 48-bit instruction: MOV imm32, and Format XIV's disp23
 * loads and stores.
 *
 * Those are the ones worth lowering. What is left at this width is
 * JR/JMP disp32, which ends a block anyway, and PREPARE's wide forms,
 * which write sp, ep and a register list; both stay on the interpreter.
 *
 * Field split and sub-opcode table: see the interpreter, which carries
 * the note about where they came from. Anything this declines lands
 * there, which is what makes declining safe rather than merely quiet:
 * the reserved-bit case must raise RIE, and the one implementation of
 * that is the one being fallen back to.
 */
static bool lower_one48(emu_ir_block_t *b, uint16_t w0, uint16_t w1,
                        uint16_t w2, uint32_t pc)
{
    const uint32_t op  = g4mh_op6(w0);
    const uint32_t r1  = g4mh_reg1(w0);
    const uint32_t r3  = (w1 >> 11) & 0x1Fu;
    const uint32_t sub = w1 & 0x0Fu;
    const bool     is_b = (sub == 0x5u) || (sub == 0xDu && op == 0x3Cu);
    const uint32_t d0  = (w1 >> 4) & 1u;
    uint32_t disp;
    uint16_t base;

    if (g4mh_reg2(w0) != 0u) {
        return false;
    }

    /*
     * MOV imm32, reg1 -- in the MOVEA slot, told apart by reg2 == 0.
     *
     * Worth having on its own account: it is how a G4MH guest loads any
     * constant wider than 16 bits, so leaving it to the interpreter
     * ends a block at every address and every large literal. Four of
     * them in one test program cost four fallbacks and made the disp23
     * lowering below look as though it had not fired.
     */
    if (op == 0x31u) {
        emu_ir_put(b, r1, emu_ir_const(b, (uint32_t)w1 |
                                          ((uint32_t)w2 << 16)));
        return true;
    }

    if (op != 0x3Cu && op != 0x3Du) {
        return false;
    }
    if (!is_b && d0 != 0u) {
        return false;                   /* reserved: RIE, on the interp */
    }

    disp = (uint32_t)emu_sext(((uint32_t)w2 << 7) |
                              ((uint32_t)(w1 >> 4) & 0x7Fu), 23);

    /* pc first: a data abort records it. Same reason as lower_one32. */
    (void)emu_ir_emit(b, EMU_IR_SETPC, 0u, EMU_IR_NO_TEMP, EMU_IR_NO_TEMP,
                      pc, 0u);
    base = emu_ir_get(b, r1);

    switch ((sub << 1) | (op & 1u)) {
    case (0x5u << 1) | 0u:              /* LD.B  */
    case (0x5u << 1) | 1u:              /* LD.BU */
    case (0x7u << 1) | 0u:              /* LD.H  */
    case (0x7u << 1) | 1u:              /* LD.HU */
    case (0x9u << 1) | 0u: {            /* LD.W  */
        const uint32_t size = (sub == 0x5u) ? 1u : (sub == 0x7u) ? 2u : 4u;
        /* op6's low bit is the unsigned form for the byte and halfword
         * loads, and 0x3C/LD.W has nothing to extend. */
        const bool sx = (size != 4u) && ((op & 1u) == 0u);

        emu_ir_put(b, r3, emu_ir_emit(b, EMU_IR_LOAD,
                                      EMU_IR_MEM_AUX(size, sx), base,
                                      EMU_IR_NO_TEMP, disp, 0u));
        return true;
    }

    case (0x9u << 1) | 1u: {            /* LD.DW */
        /*
         * Both loads, then both writes -- not load/write/load/write.
         * The interpreter leaves the first register untouched when the
         * second access faults, and a JIT that wrote as it went would
         * hand the handler a different register file. Nothing computes
         * a wrong answer either way, which is what makes it the kind of
         * divergence that survives.
         */
        const uint32_t rd = r3 & ~1u;
        const uint16_t lo = emu_ir_emit(b, EMU_IR_LOAD,
                                        EMU_IR_MEM_AUX(4u, 0u), base,
                                        EMU_IR_NO_TEMP, disp, 0u);
        const uint16_t hi = emu_ir_emit(b, EMU_IR_LOAD,
                                        EMU_IR_MEM_AUX(4u, 0u), base,
                                        EMU_IR_NO_TEMP, disp + 4u, 0u);
        emu_ir_put(b, rd, lo);
        emu_ir_put(b, rd + 1u, hi);
        return true;
    }

    case (0xDu << 1) | 0u:              /* ST.B */
    case (0xDu << 1) | 1u:              /* ST.H */
    case (0xFu << 1) | 0u: {            /* ST.W */
        const uint32_t size = (sub == 0xDu) ? ((op & 1u) ? 2u : 1u) : 4u;

        (void)emu_ir_emit(b, EMU_IR_STORE, EMU_IR_MEM_AUX(size, 0u),
                          base, emu_ir_get(b, r3), disp, 0u);
        return true;
    }

    case (0xFu << 1) | 1u: {            /* ST.DW */
        const uint32_t rs = r3 & ~1u;

        (void)emu_ir_emit(b, EMU_IR_STORE, EMU_IR_MEM_AUX(4u, 0u), base,
                          emu_ir_get(b, rs), disp, 0u);
        (void)emu_ir_emit(b, EMU_IR_STORE, EMU_IR_MEM_AUX(4u, 0u), base,
                          emu_ir_get(b, rs + 1u), disp + 4u, 0u);
        return true;
    }

    default:
        return false;
    }
}

/*
 * Lower one 16-bit instruction. Returns false for anything not modelled,
 * which ends the block -- the caller has emitted nothing for it.
 */
static bool lower_one(emu_ir_block_t *b, uint16_t w0, uint32_t pc)
{
    const uint32_t r1 = g4mh_reg1(w0);
    const uint32_t r2 = g4mh_reg2(w0);
    const uint32_t op = g4mh_op6(w0);

    /*
     * Format IV: the short load and store forms, addressed off EP.
     *
     * These overlay the 6-bit opcode space with a 4-bit one, so they are
     * decoded on bits[10:7] before the wider switch below gets a chance
     * -- 0x06..0x09 as a 6-bit opcode is SATSUBR and friends, and
     * reaching them by the wrong field would translate the wrong
     * instruction rather than declining.
     *
     * The displacement is unsigned and pre-scaled by the access width:
     * a halfword form spends the bit the alignment leaves free, exactly
     * as the disp16 forms do. It is *not* sign-extended.
     */
    switch (g4mh_op4(w0)) {
    case 0x06:                                  /* SLD.B disp7      */
    case 0x07:                                  /* SST.B disp7      */
    case 0x08:                                  /* SLD.H disp8      */
    case 0x09: {                                /* SST.H disp8      */
        const uint32_t op4 = g4mh_op4(w0);
        const bool byte = (op4 < 0x08u);
        const uint32_t size = byte ? 1u : 2u;
        const uint32_t disp = byte ? (w0 & 0x7Fu) : ((w0 & 0x7Fu) << 1);

        /* pc first: a data abort records it. */
        (void)emu_ir_emit(b, EMU_IR_SETPC, 0u, EMU_IR_NO_TEMP,
                          EMU_IR_NO_TEMP, pc, 0u);
        const uint16_t ep = emu_ir_get(b, G4MH_REG_EP);

        if ((op4 & 1u) == 0u) {                 /* the loads sign-extend */
            emu_ir_put(b, r2, emu_ir_emit(b, EMU_IR_LOAD,
                                          EMU_IR_MEM_AUX(size, 1u), ep,
                                          EMU_IR_NO_TEMP, disp, 0u));
        } else {
            (void)emu_ir_emit(b, EMU_IR_STORE, EMU_IR_MEM_AUX(size, 0u),
                              ep, emu_ir_get(b, r2), disp, 0u);
        }
        return true;
    }
    default:
        break;
    }

    switch (op) {
    case 0x00:                                  /* MOV reg1, reg2   */
        /*
         * reg2 == 0 is NOP and reg1 == 0 shares the slot with other
         * encodings. Declining both is what the direct translator did
         * and is still right: reg2 == 0 as an opcode extension has
         * already caught this frontend out once, and the failure mode is
         * a silent write into r0 rather than an exception.
         */
        if (r1 == 0u || r2 == 0u) {
            return false;
        }
        emu_ir_put(b, r2, emu_ir_get(b, r1));
        return true;

    case 0x08:                                  /* OR  reg1, reg2   */
    case 0x09:                                  /* XOR reg1, reg2   */
    case 0x0A: {                                /* AND reg1, reg2   */
        static const uint8_t k_alu[3] = {
            EMU_IR_OR, EMU_IR_XOR, EMU_IR_AND
        };
        const uint16_t x = emu_ir_get(b, r2);
        const uint16_t y = emu_ir_get(b, r1);
        const uint16_t s = emu_ir_alu(b, (emu_ir_op_t)k_alu[op - 0x08u], x, y);

        emu_ir_put(b, r2, s);
        (void)emu_ir_emit(b, EMU_IR_SETF, EMU_IR_FS_LOGIC, s,
                          EMU_IR_NO_TEMP, 0u, F_LOGIC);
        return true;
    }

    case 0x0B: {                                /* TST reg1, reg2   */
        const uint16_t x = emu_ir_get(b, r2);
        const uint16_t y = emu_ir_get(b, r1);
        const uint16_t s = emu_ir_alu(b, EMU_IR_AND, x, y);

        /* No register write: the AND exists only for its flags. The
         * dead-value pass keeps it because the SETF reads it. */
        (void)emu_ir_emit(b, EMU_IR_SETF, EMU_IR_FS_LOGIC, s,
                          EMU_IR_NO_TEMP, 0u, F_LOGIC);
        return true;
    }

    case 0x0D:                                  /* SUB reg1, reg2   */
    case 0x0E: {                                /* ADD reg1, reg2   */
        const bool sub = (op == 0x0Du);
        const uint16_t x = emu_ir_get(b, r2);
        const uint16_t y = emu_ir_get(b, r1);
        const uint16_t s = emu_ir_alu(b, sub ? EMU_IR_SUB : EMU_IR_ADD, x, y);

        emu_ir_put(b, r2, s);
        /*
         * The flag source gets the *operands*, not the result: overflow
         * and carry cannot be recovered from the result alone. `a` is
         * the left operand and `b` the right, which is the order
         * EMU_IR_FS_SUB subtracts in.
         */
        (void)emu_ir_emit(b, EMU_IR_SETF,
                          sub ? EMU_IR_FS_SUB : EMU_IR_FS_ADD, x, y, 0u,
                          F_ARITH);
        return true;
    }

    case 0x0F: {                                /* CMP reg1, reg2   */
        const uint16_t x = emu_ir_get(b, r2);
        const uint16_t y = emu_ir_get(b, r1);

        (void)emu_ir_emit(b, EMU_IR_SETF, EMU_IR_FS_SUB, x, y, 0u, F_ARITH);
        return true;
    }

    case 0x10:                                  /* MOV imm5, reg2   */
        if (r2 == 0u) {
            return false;                       /* CALLT shares the slot */
        }
        emu_ir_put(b, r2, emu_ir_const(b, (uint32_t)g4mh_imm5(w0)));
        return true;

    case 0x12: {                                /* ADD imm5, reg2   */
        const uint16_t x = emu_ir_get(b, r2);
        const uint16_t y = emu_ir_const(b, (uint32_t)g4mh_imm5(w0));
        const uint16_t s = emu_ir_alu(b, EMU_IR_ADD, x, y);

        emu_ir_put(b, r2, s);
        (void)emu_ir_emit(b, EMU_IR_SETF, EMU_IR_FS_ADD, x, y, 0u, F_ARITH);
        return true;
    }

    case 0x13: {                                /* CMP imm5, reg2   */
        const uint16_t x = emu_ir_get(b, r2);
        const uint16_t y = emu_ir_const(b, (uint32_t)g4mh_imm5(w0));

        (void)emu_ir_emit(b, EMU_IR_SETF, EMU_IR_FS_SUB, x, y, 0u, F_ARITH);
        return true;
    }

    default:
        return false;
    }
}

/*
 * Build a block of IR starting at `pc`. Returns the number of guest
 * instructions folded in, 0 if nothing at `pc` could be lowered.
 *
 * pc is written after every instruction, as the direct translator did,
 * so that whatever runs next -- a trap, an interrupt, the interpreter
 * picking up where this left off -- sees the right address.
 */
uint32_t g4mh_ir_translate(emu_cpu_t *cpu, uint32_t pc, emu_ir_block_t *b)
{
    g4mh_cpu_t *const c = (g4mh_cpu_t *)cpu;
    uint32_t cur = pc;
    uint32_t count = 0u;

    emu_ir_reset(b);

    while (count < G4MH_IR_MAX_BLOCK_INSNS && !b->overflow) {
        uint16_t w0;

        if (emu_bus_fetch16(c->bus, cur, &w0) != EMU_FAULT_NONE) {
            break;
        }
        /*
         * The width is settled before the opcode is, and by the same
         * two functions the interpreter uses. Lowering a 48-bit form as
         * a 32-bit one would not compute a wrong answer, it would
         * desynchronise the instruction stream -- so the test is on the
         * form, and only then on whether the opcode is recognised.
         */
        const uint32_t mark = b->count;
        uint32_t len = 2u;
        bool ok;

        if (g4mh_is_16bit(w0)) {
            ok = lower_one(b, w0, cur);
        } else {
            uint16_t w1;

            if (g4mh_insn_len(w0) != 4u ||
                emu_bus_fetch16(c->bus, cur + 2u, &w1) != EMU_FAULT_NONE) {
                break;
            }
            if (g4mh_insn_is_48(w0, w1)) {
                uint16_t w2;

                if (emu_bus_fetch16(c->bus, cur + 4u, &w2) !=
                    EMU_FAULT_NONE) {
                    break;
                }
                len = 6u;
                ok = lower_one48(b, w0, w1, w2, cur);
            } else {
                len = 4u;
                ok = lower_one32(b, w0, w1, cur);
            }
        }

        if (!ok) {
            /*
             * Discard whatever the attempt emitted. A lowering can emit
             * part of an instruction before reaching the operand that
             * tells it to decline -- the reg2 == 0 cases above do
             * exactly that -- and a half-instruction left in the block
             * does not fail, it quietly computes something else.
             */
            b->count = mark;
            break;
        }

        cur += len;
        count++;
        (void)emu_ir_emit(b, EMU_IR_RETIRE, 0u, EMU_IR_NO_TEMP,
                          EMU_IR_NO_TEMP, 0u, 0u);
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

static bool g4mh_jit_is_idle(emu_cpu_t *cpu)
{
    return ((g4mh_cpu_t *)cpu)->state == EMU_STATE_WFI;
}

/*
 * Waking is the interpreter's business: a parked G4MH core restarts only
 * when a channel is pending, and deciding that means walking the INTC.
 * Returning false here leaves the framework to report WFI, and the
 * interrupt path below is what actually restarts it.
 */
static bool g4mh_jit_wake(emu_cpu_t *cpu)
{
    g4mh_cpu_t *const c = (g4mh_cpu_t *)cpu;

    if (!g4mh_cpu_pending_fe(c) && g4mh_cpu_pending_irq(c) < 0) {
        return false;
    }
    c->state = EMU_STATE_RUNNING;
    c->irq_dirty = true;
    return true;
}

static bool g4mh_jit_take_irq(emu_cpu_t *cpu)
{
    g4mh_cpu_t *const c = (g4mh_cpu_t *)cpu;

    if (!c->irq_dirty) {
        return false;
    }
    /* Cleared before the evaluation, not after; see the interpreter. */
    c->irq_dirty = false;

    /*
     * FE level first, exactly as the interpreter does it -- and the
     * duplication is the point rather than an accident: this backend has
     * its own interrupt path, so anything added to the interpreter's is
     * simply absent here. That is the shape of the performance-counter
     * bug (#38) and it recurred immediately: the FEINT delivery was
     * added to the interpreter alone and the JIT, which is the default
     * backend, silently never took one. The test caught it only because
     * it asserts the *cause register* and not merely that a handler ran.
     */
    if (g4mh_cpu_pending_fe(c)) {
        c->state = EMU_STATE_RUNNING;
        g4mh_intc_ack_fe(c->intc, c->coreid);
        g4mh_cpu_exception(c, G4MH_EXC_FEINT, c->pc);
        return true;
    }

    const int ch = g4mh_cpu_pending_irq(c);
    if (ch < 0) {
        return false;
    }
    c->state = EMU_STATE_RUNNING;
    g4mh_intc_ack(c->intc, (uint32_t)ch);
    g4mh_cpu_exception(c, G4MH_EXC_EIINT_BASE + (uint32_t)ch, c->pc);
    return true;
}

static void g4mh_jit_count(emu_cpu_t *cpu, uint32_t n)
{
    g4mh_cpu_t *const c = (g4mh_cpu_t *)cpu;

    c->cycles += n;
    c->retired += n;
}

extern const emu_backend_t g4mh_backend_interp;

/*
 * generation stays NULL: nothing is baked into a block. Every instruction
 * translated here reads only its own operands, and anything that could
 * change the meaning of a later one -- a system register write, a mode
 * change -- is not translated at all. The day one is, it needs a
 * generation.
 */
static void g4mh_jit_bind(emu_cpu_t *cpu, emu_jit_hot_t *out)
{
    g4mh_cpu_t *const c = (g4mh_cpu_t *)cpu;

    out->pc = &c->pc;
    out->state = &c->state;
}

const emu_ir_frontend_t g4mh_ir_frontend = {
    .name         = "g4mh",
    .translate    = g4mh_ir_translate,
    .target       = &g4mh_ir_target,
    .bind         = g4mh_jit_bind,
    .interp       = &g4mh_backend_interp,
    .is_idle      = g4mh_jit_is_idle,
    .wake         = g4mh_jit_wake,
    .take_irq     = g4mh_jit_take_irq,
    .count        = g4mh_jit_count,
    .after_interp = NULL,
    .code_bytes   = G4MH_JIT_CODE_BYTES,
    /* r[0..31], pc and psw. */
    .diff_state_bytes = (uint32_t)offsetof(g4mh_cpu_t, psw) + 4u,
};
