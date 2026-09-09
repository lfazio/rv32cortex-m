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
#include "g4mh/g4mh_backend.h"
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
#define G4MH_JIT_CODE_BYTES EMU_HOST_JIT_CODE_BYTES

/*
 * G4MH's sixteen branch conditions onto the IR's ten.
 *
 * At file scope because both the branches and CMOV select on it, and a
 * second copy is a second thing to get wrong. **The table is indexed by
 * the architecture's encoding, so a hole must be a hole**: 0xFF marks
 * the six with no IR spelling -- BV, BNV, BN, BP, BSA -- and is tested
 * before use, because mapping an unrepresentable condition onto a near
 * neighbour is a silently wrong branch.
 */
static const uint8_t k_g4mh_cond[16] = {
            0xFFu, /* 0 BV   overflow      -- no IR spelling  */
            EMU_IR_C_LTU, /* 1 BL   carry set                        */
            EMU_IR_C_EQ, /* 2 BE   zero                             */
            EMU_IR_C_LEU, /* 3 BNH  carry or zero                    */
            0xFFu, /* 4 BN   negative      -- no IR spelling  */
            EMU_IR_C_ALWAYS, /* 5 BR   unconditional                */
            EMU_IR_C_LT, /* 6 BLT  signed less                      */
            EMU_IR_C_LE, /* 7 BLE  signed less or equal             */
            0xFFu, /* 8 BNV                -- no IR spelling  */
            EMU_IR_C_GEU, /* 9 BNL  carry clear                      */
            EMU_IR_C_NE, /* A BNE  not zero                         */
            EMU_IR_C_GTU, /* B BH   higher                           */
            0xFFu, /* C BP   positive      -- no IR spelling  */
            0xFFu, /* D BSA  saturated     -- no IR spelling  */
            EMU_IR_C_GE, /* E BGE  signed greater or equal          */
            EMU_IR_C_GT /* F BGT  signed greater                   */
        };

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
    .reg_offset = g4mh_reg_offset,
    .flags_offset = (uint32_t)offsetof(g4mh_cpu_t, psw),
    /*
     * The IR's neutral flags mapped onto PSW. Order is Z, S, V, C, which
     * is the order of EMU_IR_F_* and *not* the order of the bits in PSW
     * -- the array is indexed by the IR's bit position, not the guest's.
     */
    .flag_bit = {G4MH_PSW_Z, G4MH_PSW_S, G4MH_PSW_OV, G4MH_PSW_CY},
    .reg_is_zero = g4mh_reg_zero,
    .pc_offset = (uint32_t)offsetof(g4mh_cpu_t, pc),
    .helpers = NULL,
    .helper_count = 0u,
    .load = g4mh_ir_load,
    .store = g4mh_ir_store,
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
/*
 * `ends` is set by a lowering that left the block through EMU_IR_EXIT.
 * Everything after an unconditional jump is unreachable, and translating
 * it is not merely wasted buffer: the bytes after a call are frequently
 * not instructions at all.
 */
static bool lower_one32(emu_ir_block_t *b, uint16_t w0, uint16_t w1,
                        uint32_t pc, bool *ends)
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
        const uint32_t imm =
            (op == 0x31u) ? (uint32_t)emu_sext(w1, 16) : ((uint32_t)w1 << 16);

        emu_ir_put(
            b, r2,
            emu_ir_alu(b, EMU_IR_ADD, emu_ir_get(b, r1), emu_ir_const(b, imm)));
        return true;
    }

    /*
     * MUL and MULU, which produce a 64-bit product across two registers:
     * the low half to reg2 and **the high half to reg3**.
     *
     * Both destinations are written, and reg2 is also a source, so the
     * operands are read into temps before either is stored -- and when
     * reg3 names reg2 the high half lands last, exactly as the
     * interpreter's two `wr` calls do it. An ISA that varies one bit to
     * mean "and also write reg3" has already caught this frontend out
     * once, in the pointer-update loads.
     *
     * One case for both, matched on `sub & 0x7FD`, which is the mask the
     * interpreter switches on: bit 1 is the signedness and is tested in
     * the body rather than doubling the case.
     *
     * No flags -- a multiply here defines none.
     */
    if ((op == 0x3Fu) && ((((uint32_t)w1 & 0x07FFu) & 0x7FDu) == 0x220u)) {
        const bool uns = (((uint32_t)w1 & 0x07FFu) & 0x2u) != 0u;
        const uint32_t r3 = ((uint32_t)w1 >> 11) & 0x1Fu;
        const uint16_t x = emu_ir_get(b, r2);
        const uint16_t y = emu_ir_get(b, r1);
        const uint16_t lo = emu_ir_alu(b, EMU_IR_MUL, x, y);
        const uint16_t hi =
            emu_ir_alu(b, uns ? EMU_IR_MULHU : EMU_IR_MULHS, x, y);

        emu_ir_put(b, r2, lo);
        emu_ir_put(b, r3, hi);
        return true;
    }

    /*
     * CMOV, in both its forms: reg3 = cond ? {reg1 | imm5} : reg2.
     *
     * Sub-opcode 0x300 with the condition in bits [4:1] and bit 5
     * choosing the register source over the imm5, so the group is
     * `sub & 0x7C0 == 0x300`. **The condition really is `(sub >> 1) &
     * 0xF` and not the three-bit reading**; CLAUDE.md records that only
     * cond 15 can settle it, because every smaller value is consistent
     * with both.
     *
     * **Branch-free, with no EMU_IR_SELECT.** The IR has one and Thumb-2
     * declines it, so using it would lower this on the host and leave
     * the board exactly where it was -- which is the trap the whole
     * SETF/GETCOND episode was: a lowering that cannot reach the backend
     * that needs it. `b ^ ((a ^ b) & -cond)` is four ordinary ALU
     * operations that every backend already has, and GETCOND yields the
     * 0 or 1 that NEG turns into the mask.
     *
     * No flags: CMOV moves a value and defines none.
     */
    if ((op == 0x3Fu) && (((uint32_t)w1 & 0x7C0u) == 0x300u)) {
        const uint32_t sub = (uint32_t)w1 & 0x07FFu;
        const uint32_t cond = k_g4mh_cond[(sub >> 1) & 0xFu];

        if (cond == 0xFFu) {
            return false;
        }

        const uint32_t dst = ((uint32_t)w1 >> 11) & 0x1Fu;
        const uint16_t a = ((sub & 0x20u) != 0u)
                               ? emu_ir_get(b, r1)
                               : emu_ir_const(b, (uint32_t)g4mh_imm5(w0));
        const uint16_t f = emu_ir_get(b, r2);
        const uint16_t c0 = emu_ir_emit(b, EMU_IR_GETCOND, (uint8_t)cond,
                                        EMU_IR_NO_TEMP, EMU_IR_NO_TEMP, 0u, 0u);
        const uint16_t m = emu_ir_emit(b, EMU_IR_NEG, 0u, c0, EMU_IR_NO_TEMP,
                                       0u, 0u);
        const uint16_t d = emu_ir_alu(b, EMU_IR_AND,
                                      emu_ir_alu(b, EMU_IR_XOR, a, f), m);

        emu_ir_put(b, dst, emu_ir_alu(b, EMU_IR_XOR, f, d));
        return true;
    }

    /*
     * The imm16 logical group: ORI, XORI, ANDI.
     *
     * **They zero-extend where the arithmetic forms sign-extend**, which
     * is the only thing about them that can be got wrong quietly -- a
     * sign-extended ANDI mask clears the top half of the register
     * instead of preserving it, and every operand with bit 15 clear
     * behaves identically either way.
     *
     * Flags are the logical set: Z and S from the result, OV cleared,
     * **CY left alone**. That last is why the mask is F_LOGIC and not
     * F_ARITH -- writing a zero to CY here would destroy a carry the
     * guest is still carrying between an add and the branch that reads
     * it.
     */
    if (op >= 0x34u && op <= 0x36u) {
        static const uint8_t k_log[3] = {EMU_IR_OR, EMU_IR_XOR, EMU_IR_AND};
        const uint16_t v =
            emu_ir_alu(b, (emu_ir_op_t)k_log[op - 0x34u], emu_ir_get(b, r1),
                       emu_ir_const(b, (uint32_t)w1));

        emu_ir_put(b, r2, v);
        (void)emu_ir_emit(b, EMU_IR_SETF, EMU_IR_FS_LOGIC, v, EMU_IR_NO_TEMP,
                          0u, F_LOGIC);
        return true;
    }

    /*
     * Format V: JARL and JR, disp22.
     *
     * One encoding serves both -- reg2 == 0 makes it a JR, because the
     * return address is then written to r0 and discarded -- so there is
     * nothing here to tell them apart and nothing that needs to.
     *
     * **The displacement has its high bits in the first halfword**:
     * disp[21:16] in w0[5:0] and disp[15:1] in w1[15:1], with disp[0]
     * hardwired zero, relative to the address of *this* instruction
     * rather than the next. The natural assumption is the other order,
     * low bits first as RISC-V does it, and the interpreter's comment
     * records that being implemented first and giving plausible-looking
     * displacements for small forward jumps and garbage for the rest.
     * The two must agree; this is a copy of that expression, deliberately.
     *
     * Bit 0 of the second halfword is what separates this from
     * everything else sharing the slot -- the displacement is even, so
     * the bit is free to be an opcode.
     */
    if ((op == 0x3Cu || op == 0x3Du) && (w1 & 1u) == 0u) {
        const uint32_t d =
            ((uint32_t)(w0 & 0x3Fu) << 16) | ((uint32_t)w1 & 0xFFFEu);

        /*
         * The link first, because it is the address after this
         * instruction and every 32-bit form here is four bytes; then the
         * retire, before the exit rather than after it, since a count
         * placed past the exit charges the guest for an instruction it
         * did run and never records it.
         */
        emu_ir_put(b, r2, emu_ir_const(b, pc + 4u));
        (void)emu_ir_emit(b, EMU_IR_RETIRE, 0u, EMU_IR_NO_TEMP, EMU_IR_NO_TEMP,
                          0u, 0u);
        (void)emu_ir_emit(b, EMU_IR_EXIT, 0u, EMU_IR_NO_TEMP, EMU_IR_NO_TEMP,
                          pc + (uint32_t)emu_sext(d, 22), 0u);
        *ends = true;
        return true;
    }

    /*
     * Format VIII: the bit-manipulation group on memory. The operation
     * selector is bits[15:14] -- the *top* of the field every other
     * 32-bit format uses for reg2 -- with the 3-bit bit number below it,
     * so reading a register out of [15:11] here gets nonsense.
     */
    if (op == 0x3Eu) {
        static const uint8_t k_bitop[4] = {EMU_IR_BITOP_SET, EMU_IR_BITOP_INV,
                                           EMU_IR_BITOP_CLR, EMU_IR_BITOP_TST};
        const uint32_t sel = (w0 >> 14) & 3u;
        const uint32_t bit = (w0 >> 11) & 7u;

        (void)emu_ir_emit(b, EMU_IR_SETPC, 0u, EMU_IR_NO_TEMP, EMU_IR_NO_TEMP,
                          pc, 0u);
        emu_ir_bitop(b, (emu_ir_op_t)k_bitop[sel], emu_ir_get(b, r1),
                     emu_ir_const(b, bit), (uint32_t)emu_sext(w1, 16), 1u);
        return true;
    }

    /*
     * The register-register group, whose operation is a sub-opcode in the
     * second halfword. It is the largest thing this translator declined:
     * instrumenting the decline path and disassembling what came out put
     * it at 93 of 253 on the g4mh guest, ahead of the branches.
     *
     * Only the shifts so far, in both their forms. The sub-opcode's low
     * bit chooses them: even writes reg2, odd writes **reg3** and leaves
     * reg2 alone. Getting that backwards is not a wrong answer in one
     * register, it is a wrong answer in two -- and this frontend has
     * already been caught by a sub-opcode bit that meant "and also write
     * reg3", which ran every `ld.w [rN]+` as an LDL.W that never advanced
     * the pointer.
     */
    if (op == 0x3Fu) {
        const uint32_t sub = (uint32_t)w1 & 0x07FFu;
        uint8_t shop;
        bool left;

        switch (sub & ~0x2u) {
        case 0x080u:
            shop = EMU_IR_SHR;
            left = false;
            break;
        case 0x0A0u:
            shop = EMU_IR_SAR;
            left = false;
            break;
        case 0x0C0u:
            shop = EMU_IR_SHL;
            left = true;
            break;
        default:
            return false;
        }

        const uint32_t dst = ((sub & 2u) != 0u) ? ((uint32_t)w1 >> 11) & 0x1Fu
                                                : r2;
        const uint16_t v = emu_ir_get(b, r2);
        const uint16_t n = emu_ir_emit(b, EMU_IR_ANDI, 0u, emu_ir_get(b, r1),
                                       EMU_IR_NO_TEMP, 31u, 0u);
        const uint16_t res = emu_ir_alu(b, (emu_ir_op_t)shop, v, n);

        /*
         * The carry, which is the bit that left: n - 1 from the bottom
         * for a right shift, 32 - n for a left one -- and **zero when n
         * is zero**, which no bit position can express, so it is masked
         * off by whether n was zero at all.
         *
         * At n == 0 the position computed here is 31 or 0, either of
         * which reads a real bit of the operand; only the `n != 0` term
         * stops it reaching the flag. The interpreter's do_shl and
         * friends open with `cy = 0` before testing n for the same
         * reason, and the immediate forms in lower_one settle it at
         * translation time instead.
         */
        const uint16_t pos =
            left ? emu_ir_alu(b, EMU_IR_SUB, emu_ir_const(b, 32u), n)
                 : emu_ir_emit(b, EMU_IR_ADDI, 0u, n, EMU_IR_NO_TEMP,
                               0xFFFFFFFFu, 0u);
        const uint16_t posm = emu_ir_emit(b, EMU_IR_ANDI, 0u, pos,
                                          EMU_IR_NO_TEMP, 31u, 0u);
        const uint16_t bit =
            emu_ir_emit(b, EMU_IR_ANDI, 0u, emu_ir_alu(b, EMU_IR_SHR, v, posm),
                        EMU_IR_NO_TEMP, 1u, 0u);
        const uint16_t nz = emu_ir_emit(b, EMU_IR_SETCC, EMU_IR_C_NE, n,
                                        emu_ir_const(b, 0u), 0u, 0u);
        const uint16_t cy = emu_ir_alu(b, EMU_IR_AND, bit, nz);

        emu_ir_put(b, dst, res);
        (void)emu_ir_emit(b, EMU_IR_SETF, EMU_IR_FS_SHIFT, res, cy, 0u,
                          F_ARITH);
        return true;
    }

    if (op < 0x38u || op > 0x3Bu) {
        return false;
    }

    /*
     * pc first: a data abort records it, and the store that normally
     * follows an instruction has not run yet.
     */
    (void)emu_ir_emit(b, EMU_IR_SETPC, 0u, EMU_IR_NO_TEMP, EMU_IR_NO_TEMP, pc,
                      0u);

    const bool byte = (op == 0x38u) || (op == 0x3Au);
    const uint32_t size = byte ? 1u : ((w1 & 1u) ? 4u : 2u);
    const uint32_t disp = byte ? (uint32_t)emu_sext(w1, 16)
                               : (uint32_t)emu_sext(w1 & 0xFFFEu, 16);
    const uint16_t base = emu_ir_get(b, r1);

    if (op <= 0x39u) { /* LD.B / LD.H / LD.W */
        /* Byte and halfword loads sign-extend; the word form has
         * nothing to extend. */
        const uint8_t spec = EMU_IR_MEM_AUX(size, size != 4u);

        emu_ir_put(
            b, r2,
            emu_ir_emit(b, EMU_IR_LOAD, spec, base, EMU_IR_NO_TEMP, disp, 0u));
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
    const uint32_t op = g4mh_op6(w0);
    const uint32_t r1 = g4mh_reg1(w0);
    const uint32_t r3 = (w1 >> 11) & 0x1Fu;
    const uint32_t sub = w1 & 0x0Fu;
    const bool is_b = (sub == 0x5u) || (sub == 0xDu && op == 0x3Cu);
    const uint32_t d0 = (w1 >> 4) & 1u;
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
        emu_ir_put(b, r1, emu_ir_const(b, (uint32_t)w1 | ((uint32_t)w2 << 16)));
        return true;
    }

    if (op != 0x3Cu && op != 0x3Du) {
        return false;
    }
    if (!is_b && d0 != 0u) {
        return false; /* reserved: RIE, on the interp */
    }

    disp = (uint32_t)emu_sext(
        ((uint32_t)w2 << 7) | ((uint32_t)(w1 >> 4) & 0x7Fu), 23);

    /* pc first: a data abort records it. Same reason as lower_one32. */
    (void)emu_ir_emit(b, EMU_IR_SETPC, 0u, EMU_IR_NO_TEMP, EMU_IR_NO_TEMP, pc,
                      0u);
    base = emu_ir_get(b, r1);

    switch ((sub << 1) | (op & 1u)) {
    case (0x5u << 1) | 0u: /* LD.B  */
    case (0x5u << 1) | 1u: /* LD.BU */
    case (0x7u << 1) | 0u: /* LD.H  */
    case (0x7u << 1) | 1u: /* LD.HU */
    case (0x9u << 1) | 0u: { /* LD.W  */
        const uint32_t size = (sub == 0x5u) ? 1u : (sub == 0x7u) ? 2u : 4u;
        /* op6's low bit is the unsigned form for the byte and halfword
         * loads, and 0x3C/LD.W has nothing to extend. */
        const bool sx = (size != 4u) && ((op & 1u) == 0u);

        emu_ir_put(b, r3,
                   emu_ir_emit(b, EMU_IR_LOAD, EMU_IR_MEM_AUX(size, sx), base,
                               EMU_IR_NO_TEMP, disp, 0u));
        return true;
    }

    case (0x9u << 1) | 1u: { /* LD.DW */
        /*
         * Both loads, then both writes -- not load/write/load/write.
         * The interpreter leaves the first register untouched when the
         * second access faults, and a JIT that wrote as it went would
         * hand the handler a different register file. Nothing computes
         * a wrong answer either way, which is what makes it the kind of
         * divergence that survives.
         */
        const uint32_t rd = r3 & ~1u;
        const uint16_t lo = emu_ir_emit(b, EMU_IR_LOAD, EMU_IR_MEM_AUX(4u, 0u),
                                        base, EMU_IR_NO_TEMP, disp, 0u);
        const uint16_t hi = emu_ir_emit(b, EMU_IR_LOAD, EMU_IR_MEM_AUX(4u, 0u),
                                        base, EMU_IR_NO_TEMP, disp + 4u, 0u);
        emu_ir_put(b, rd, lo);
        emu_ir_put(b, rd + 1u, hi);
        return true;
    }

    case (0xDu << 1) | 0u: /* ST.B */
    case (0xDu << 1) | 1u: /* ST.H */
    case (0xFu << 1) | 0u: { /* ST.W */
        const uint32_t size = (sub == 0xDu) ? ((op & 1u) ? 2u : 1u) : 4u;

        (void)emu_ir_emit(b, EMU_IR_STORE, EMU_IR_MEM_AUX(size, 0u), base,
                          emu_ir_get(b, r3), disp, 0u);
        return true;
    }

    case (0xFu << 1) | 1u: { /* ST.DW */
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
/*
 * `counted` is set by a lowering that emitted its own EMU_IR_RETIRE.
 *
 * A conditional branch has to: the block leaves through EXIT_IF on the
 * taken path, and a RETIRE placed after it by the caller is skipped
 * exactly then -- which undercounts every loop back edge in the guest
 * while computing entirely correct answers.
 */
static bool lower_one(emu_ir_block_t *b, uint16_t w0, uint32_t pc,
                      bool *counted)
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
    case 0x06: /* SLD.B disp7      */
    case 0x07: /* SST.B disp7      */
    case 0x08: /* SLD.H disp8      */
    case 0x09: { /* SST.H disp8      */
        const uint32_t op4 = g4mh_op4(w0);
        const bool byte = (op4 < 0x08u);
        const uint32_t size = byte ? 1u : 2u;
        const uint32_t disp = byte ? (w0 & 0x7Fu) : ((w0 & 0x7Fu) << 1);

        /* pc first: a data abort records it. */
        (void)emu_ir_emit(b, EMU_IR_SETPC, 0u, EMU_IR_NO_TEMP, EMU_IR_NO_TEMP,
                          pc, 0u);
        const uint16_t ep = emu_ir_get(b, G4MH_REG_EP);

        if ((op4 & 1u) == 0u) { /* the loads sign-extend */
            emu_ir_put(b, r2,
                       emu_ir_emit(b, EMU_IR_LOAD, EMU_IR_MEM_AUX(size, 1u), ep,
                                   EMU_IR_NO_TEMP, disp, 0u));
        } else {
            (void)emu_ir_emit(b, EMU_IR_STORE, EMU_IR_MEM_AUX(size, 0u), ep,
                              emu_ir_get(b, r2), disp, 0u);
        }
        return true;
    }
    case 0x0B: { /* Bcond disp9      */
        /*
         * The branches, which are what make a loop body a block. Without
         * them every back edge ended a block, so nothing amortised: the
         * translator paid a full translation for a handful of
         * instructions and handed the branch back to the interpreter.
         *
         * G4MH names sixteen conditions and the IR names ten, and the
         * ten are exactly the ones a compiler emits -- equality, both
         * signed orderings, both unsigned. The six left out (BV, BNV,
         * BN, BP, BSA, and the saturation flag) have no IR spelling
         * because no other frontend has them; they decline, which is
         * correct and costs a block each time a guest uses one.
         *
         * **The condition table is indexed by the architecture's
         * encoding, so a hole in it must be a hole and not a wrong
         * answer.** 0xFF marks the six, and is tested before use --
         * mapping an unrepresentable condition onto a near neighbour
         * would be a silently wrong branch, which is the worst thing
         * available here.
         */
        /* k_g4mh_cond, at file scope: CMOV needs the same map. */
        const uint32_t cond = k_g4mh_cond[w0 & 0xFu];

        if (cond == 0xFFu) {
            return false;
        }

        /*
         * disp[8:4] in bits[15:11], disp[3:1] in bits[6:4], bit 0 always
         * zero, sign-extended from 9 and added to the address of the
         * *branch* rather than of the next instruction.
         */
        const uint32_t d = ((((uint32_t)w0 >> 11) & 0x1Fu) << 4) |
                           ((((uint32_t)w0 >> 4) & 0x7u) << 1);
        const uint32_t target = pc + (uint32_t)emu_sext(d, 9);

        /*
         * The condition is read from PSW into a value, then the exit
         * compares that value against zero. EXIT_IF compares two
         * operands -- it is a frontend with no flags that shaped it --
         * so a flag-testing branch needs the GETCOND in front.
         *
         * RETIRE before the exit, and `counted` so the caller does not
         * add a second one after it.
         */
        const uint16_t t = emu_ir_emit(b, EMU_IR_GETCOND, (uint8_t)cond,
                                       EMU_IR_NO_TEMP, EMU_IR_NO_TEMP, 0u, 0u);

        (void)emu_ir_emit(b, EMU_IR_RETIRE, 0u, EMU_IR_NO_TEMP, EMU_IR_NO_TEMP,
                          0u, 0u);
        (void)emu_ir_emit(b, EMU_IR_EXIT_IF, EMU_IR_C_NE, t,
                          emu_ir_const(b, 0u), target, 0u);
        *counted = true;
        return true;
    }

    default:
        break;
    }

    switch (op) {
    case 0x00: /* MOV reg1, reg2   */
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

    case 0x08: /* OR  reg1, reg2   */
    case 0x09: /* XOR reg1, reg2   */
    case 0x0A: { /* AND reg1, reg2   */
        static const uint8_t k_alu[3] = {EMU_IR_OR, EMU_IR_XOR, EMU_IR_AND};
        const uint16_t x = emu_ir_get(b, r2);
        const uint16_t y = emu_ir_get(b, r1);
        const uint16_t s = emu_ir_alu(b, (emu_ir_op_t)k_alu[op - 0x08u], x, y);

        emu_ir_put(b, r2, s);
        (void)emu_ir_emit(b, EMU_IR_SETF, EMU_IR_FS_LOGIC, s, EMU_IR_NO_TEMP,
                          0u, F_LOGIC);
        return true;
    }

    case 0x0B: { /* TST reg1, reg2   */
        const uint16_t x = emu_ir_get(b, r2);
        const uint16_t y = emu_ir_get(b, r1);
        const uint16_t s = emu_ir_alu(b, EMU_IR_AND, x, y);

        /* No register write: the AND exists only for its flags. The
         * dead-value pass keeps it because the SETF reads it. */
        (void)emu_ir_emit(b, EMU_IR_SETF, EMU_IR_FS_LOGIC, s, EMU_IR_NO_TEMP,
                          0u, F_LOGIC);
        return true;
    }

    case 0x0D: /* SUB reg1, reg2   */
    case 0x0E: { /* ADD reg1, reg2   */
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
        (void)emu_ir_emit(b, EMU_IR_SETF, sub ? EMU_IR_FS_SUB : EMU_IR_FS_ADD,
                          x, y, 0u, F_ARITH);
        return true;
    }

    case 0x0F: { /* CMP reg1, reg2   */
        const uint16_t x = emu_ir_get(b, r2);
        const uint16_t y = emu_ir_get(b, r1);

        (void)emu_ir_emit(b, EMU_IR_SETF, EMU_IR_FS_SUB, x, y, 0u, F_ARITH);
        return true;
    }

    case 0x10: /* MOV imm5, reg2   */
        if (r2 == 0u) {
            return false; /* CALLT shares the slot */
        }
        emu_ir_put(b, r2, emu_ir_const(b, (uint32_t)g4mh_imm5(w0)));
        return true;

    case 0x12: { /* ADD imm5, reg2   */
        const uint16_t x = emu_ir_get(b, r2);
        const uint16_t y = emu_ir_const(b, (uint32_t)g4mh_imm5(w0));
        const uint16_t s = emu_ir_alu(b, EMU_IR_ADD, x, y);

        emu_ir_put(b, r2, s);
        (void)emu_ir_emit(b, EMU_IR_SETF, EMU_IR_FS_ADD, x, y, 0u, F_ARITH);
        return true;
    }

    case 0x14: /* SHR imm5, reg2   */
    case 0x15: /* SAR imm5, reg2   */
    case 0x16: { /* SHL imm5, reg2   */
        /*
         * The amount is a *constant here*, which is what makes these
         * lowerable at all: the carry a shift defines is the last bit
         * shifted out, and knowing n at translation turns that into one
         * more shift of the source rather than a run-time select on
         * whether n is zero.
         *
         * **Shift by zero is a different instruction, not a degenerate
         * one.** G4MH leaves the value alone, clears OV *and* CY, and
         * still writes Z and S -- so it is a move with FS_LOGIC, which
         * defines exactly those four that way. Reaching it through the
         * general path would compute the carry as bit 31 of the source
         * (n - 1 wrapping to 31) and set CY from it. The interpreter's
         * do_shl/do_shr/do_sar all begin `cy = 0` before testing n for
         * this reason; the same case has to exist here.
         */
        static const uint8_t k_sh[3] = {EMU_IR_SHRI, EMU_IR_SARI,
                                        EMU_IR_SHLI};
        const uint32_t n = (uint32_t)(w0 & 0x1Fu);
        const uint16_t v = emu_ir_get(b, r2);

        if (n == 0u) {
            emu_ir_put(b, r2, v);
            (void)emu_ir_emit(b, EMU_IR_SETF, EMU_IR_FS_LOGIC, v,
                              EMU_IR_NO_TEMP, 0u, F_ARITH);
            return true;
        }

        const uint16_t res =
            emu_ir_emit(b, (emu_ir_op_t)k_sh[op - 0x14u], 0u, v,
                        EMU_IR_NO_TEMP, n, 0u);
        /*
         * The bit that leaves: n - 1 from the bottom for the right
         * shifts, 32 - n from the bottom for the left one. Both are
         * reads of the *source*, so the shift above and this are
         * independent and the optimiser is free to order them.
         */
        const uint32_t cbit = (op == 0x16u) ? (32u - n) : (n - 1u);
        const uint16_t cy = emu_ir_emit(b, EMU_IR_SHRI, 0u, v,
                                        EMU_IR_NO_TEMP, cbit, 0u);

        emu_ir_put(b, r2, res);
        (void)emu_ir_emit(b, EMU_IR_SETF, EMU_IR_FS_SHIFT, res, cy, 0u,
                          F_ARITH);
        return true;
    }

    case 0x13: { /* CMP imm5, reg2   */
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
        bool counted = false;
        bool ends = false;
        bool ok;

        if (g4mh_is_16bit(w0)) {
            ok = lower_one(b, w0, cur, &counted);
        } else {
            uint16_t w1;

            if (g4mh_insn_len(w0) != 4u ||
                emu_bus_fetch16(c->bus, cur + 2u, &w1) != EMU_FAULT_NONE) {
                break;
            }
            if (g4mh_insn_is_48(w0, w1)) {
                uint16_t w2;

                /*
                 * The one 64-bit encoding stays on the interpreter. Not
                 * because it is hard -- it is a PREPARE, which this
                 * backend declines at every width -- but because
                 * lowering it would mean this loop knowing the length in
                 * two places.
                 */
                if (g4mh_insn_is_64(w0, w1)) {
                    break;
                }
                if (emu_bus_fetch16(c->bus, cur + 4u, &w2) != EMU_FAULT_NONE) {
                    break;
                }
                len = 6u;
                ok = lower_one48(b, w0, w1, w2, cur);
            } else {
                len = 4u;
                ok = lower_one32(b, w0, w1, cur, &ends);
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
        if (!counted) {
            (void)emu_ir_emit(b, EMU_IR_RETIRE, 0u, EMU_IR_NO_TEMP,
                              EMU_IR_NO_TEMP, 0u, 0u);
        }
        (void)emu_ir_emit(b, EMU_IR_SETPC, 0u, EMU_IR_NO_TEMP, EMU_IR_NO_TEMP,
                          cur, 0u);
        if (ends) {
            break;
        }
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

    /*
     * The priority ceiling is raised here as well as in the interpreter,
     * and this is the second time that has had to be said: this file is
     * a *separate copy* of the run loop's interrupt check, so anything
     * added there is absent here until someone adds it. FE-level
     * delivery was the first (see the note above); the ISPR ceiling is
     * the second. Both were found by a test asserting a register rather
     * than that a handler ran.
     */
    unsigned ipri = 64u;
    const int ch = g4mh_cpu_pending_irq_pri(c, &ipri);
    if (ch < 0) {
        return false;
    }
    c->state = EMU_STATE_RUNNING;

    /*
     * Vector first, acknowledge second -- the third thing this separate
     * copy has needed. A table-reference channel resolves its handler by
     * reading memory, which can take an MDP, and the architecture then
     * cancels acceptance and leaves the request pending; acknowledging
     * before the read would discard an interrupt meant to be retried.
     */
    uint32_t vec;
    if (!g4mh_cpu_irq_vector(c, (uint32_t)ch, &vec)) {
        g4mh_cpu_exception(c, G4MH_EXC_MDP, c->pc);
        return true;
    }
    g4mh_intc_ack(c->intc, (uint32_t)ch);
    g4mh_cpu_exception_at(c, G4MH_EXC_EIINT_BASE + (uint32_t)ch, c->pc, vec);
    g4mh_cpu_ack_priority(c, ipri);
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
    .name = "g4mh",
    .translate = g4mh_ir_translate,
    .target = &g4mh_ir_target,
    .bind = g4mh_jit_bind,
    .interp = &g4mh_backend_interp,
    .is_idle = g4mh_jit_is_idle,
    .wake = g4mh_jit_wake,
    .take_irq = g4mh_jit_take_irq,
    .count = g4mh_jit_count,
    .after_interp = NULL,
    .code_bytes = G4MH_JIT_CODE_BYTES,
    /* r[0..31], pc and psw. */
    .diff_state_bytes = (uint32_t)offsetof(g4mh_cpu_t, psw) + 4u,
};
