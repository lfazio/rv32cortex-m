/* SPDX-License-Identifier: Apache-2.0 */
/*
 * src/backend/thumb2/ir_lower.c - Lowering the IR to Thumb-2.
 *
 * The ARMv7E-M half of frontend -> IR -> optimisation -> backend. It
 * knows the host and nothing about any guest; everything guest-specific
 * arrives through emu_ir_target_t.
 *
 * Register convention
 * -------------------
 *   r4   the emu_cpu_t pointer, callee-saved so it survives a helper
 *   r5   the retired count this block returns
 *   sp   the temp frame; every IR value has exactly one home in it
 *   r0-r3, r12   scratch, and the AAPCS argument registers
 *
 * Temps live in the frame rather than in registers, as on x86-64 and for
 * the same reason: a register allocator is the largest and most
 * error-prone part of a backend, and this host has no way to test one
 * except by flashing a board. Everything live being in memory also means
 * a helper call clobbers nothing.
 *
 * That cost is real and is larger here than on x86-64, because this is
 * the host where code size sets performance: the code cache is 12 KB and
 * CoreMark's translated working set is around 48 KB. Do not expect this
 * to beat the hand-written translator beside it until values stay in
 * registers. What it buys today is that the Thumb-2 backend stops being
 * a second implementation of semantics the IR already owns.
 *
 * Encodings are from ARM DDI 0403E (docs/arm/). Where a 16-bit form
 * exists it is *not* used: the register numbers here are frequently
 * above r7, and this project has already been bitten by a 16-bit
 * data-processing encoding silently becoming a different instruction
 * when handed a high register.
 */

#include "emu/emu_ir.h"
#include "emu/emu_thumb2.h"

#if defined(EMU_JIT_THUMB2)


static uint16_t g_ntemps;

#define SCRATCH_ADDR ((uint16_t)(g_ntemps))
#define SCRATCH_VAL  ((uint16_t)(g_ntemps + 1u))

static uint32_t slot(uint16_t n) { return (uint32_t)n * 4u; }

void ld_slot(uint32_t rt, uint16_t n)
{
    t2_ldr_imm(rt, 13u, slot(n));         /* [sp, #off] */
}

void st_slot(uint32_t rt, uint16_t n)
{
    t2_str_imm(rt, 13u, slot(n));
}

/* Load an operand, tolerating EMU_IR_NO_TEMP so callers need not check. */
void ld_operand(uint32_t rt, uint16_t n)
{
    if (n == EMU_IR_NO_TEMP) {
        t2_imm32(rt, 0u);
        return;
    }
    ld_slot(rt, n);
}


/*
 * Sites to be patched to the block epilogue. Every early exit -- a
 * trapping access, a taken EXIT_IF, an EXIT -- lands there rather than
 * returning directly, because all paths must undo the same frame
 * adjustment.
 */
#define IR_MAX_EXITS 64u
typedef struct { uint8_t *at; bool conditional; } ir_exit_t;

/*
 * The kind is recorded, not recovered from the emitted halfword.
 * Reading it back means deciding conditional-versus-unconditional
 * from an encoding already written, and a misread patches a B<c>.W
 * with B.W's field layout -- landing somewhere plausible instead of
 * somewhere right.
 */
static ir_exit_t g_exits[IR_MAX_EXITS];
static uint32_t g_nexits;



/*
 * Patch a forward branch emitted above to reach `target`.
 *
 * Guarded on overflow because `at` may be past the end of a buffer that
 * stopped accepting emissions -- the block is discarded either way, and
 * writing through the pointer would corrupt whatever follows.
 */
static void patch_branch(uint8_t *at, const uint8_t *target, bool conditional)
{
    if (at == NULL || emu_jit_overflowed()) {
        return;
    }

    uint16_t *const hw = (uint16_t *)(void *)at;
    /* The branch reads pc as its own address plus 4. */
    const int32_t off = (int32_t)(target - (at + 4));
    const uint32_t imm = (uint32_t)off >> 1;

    if (conditional) {
        /* imm11 | J2 | imm6 | J1 -- the T3 encoding's split fields. */
        const uint32_t s = (imm >> 19) & 1u;
        const uint32_t j2 = (imm >> 18) & 1u;
        const uint32_t j1 = (imm >> 17) & 1u;

        hw[0] = (uint16_t)((hw[0] & 0xFBC0u) | (s << 10) |
                           ((imm >> 11) & 0x3Fu));
        hw[1] = (uint16_t)(0x8000u | (j1 << 13) | (j2 << 11) |
                           (imm & 0x7FFu));
    } else {
        /* T4: S | imm10 | J1 | 1 | J2 | imm11, with J = !(I ^ S). */
        const uint32_t s = (imm >> 23) & 1u;
        const uint32_t i1 = (imm >> 22) & 1u;
        const uint32_t i2 = (imm >> 21) & 1u;
        const uint32_t j1 = (~(i1 ^ s)) & 1u;
        const uint32_t j2 = (~(i2 ^ s)) & 1u;

        hw[0] = (uint16_t)(0xF000u | (s << 10) | ((imm >> 11) & 0x3FFu));
        hw[1] = (uint16_t)(0x9000u | (j1 << 13) | (j2 << 11) |
                           (imm & 0x7FFu));
    }
}

static void note_exit(uint8_t *at, bool conditional)
{
    if (g_nexits < IR_MAX_EXITS) {
        g_exits[g_nexits].at = at;
        g_exits[g_nexits].conditional = conditional;
        g_nexits++;
    }
}

/* ------------------------------------------------------------------ */
/* Lowering                                                            */
/* ------------------------------------------------------------------ */

/*
 * Bisection gate.
 *
 * This host has no suite: the only way to find out which emitted
 * encoding is wrong is to allow one group at a time and flash. Level 0
 * emits nothing but the block frame and the pc and retire bookkeeping,
 * so the guest computes garbage but must still *run* -- if it faults
 * there, the frame is wrong and no instruction encoding is implicated.
 */
#ifndef T2_BISECT
#  define T2_BISECT 99
#endif

static bool bisect_allows(uint8_t op)
{
    switch ((emu_ir_op_t)op) {
    case EMU_IR_NOP: case EMU_IR_RETIRE: case EMU_IR_SETPC:
        return T2_BISECT >= 0;
    case EMU_IR_GET: case EMU_IR_PUT: case EMU_IR_CONST: case EMU_IR_MOV:
        return T2_BISECT >= 1;
    case EMU_IR_ADD: case EMU_IR_SUB: case EMU_IR_AND:
    case EMU_IR_OR:  case EMU_IR_XOR: case EMU_IR_NOT: case EMU_IR_NEG:
        return T2_BISECT >= 2;
    case EMU_IR_SHL: case EMU_IR_SHR: case EMU_IR_SAR:
    case EMU_IR_SHLI: case EMU_IR_SHRI: case EMU_IR_SARI:
        return T2_BISECT >= 3;
    case EMU_IR_BSWAP32: case EMU_IR_BSWAP16: case EMU_IR_HSWAP:
    case EMU_IR_CLZ: case EMU_IR_CTZ:
    case EMU_IR_SEXT8: case EMU_IR_SEXT16:
    case EMU_IR_ZEXT8: case EMU_IR_ZEXT16:
        return T2_BISECT >= 4;
    case EMU_IR_SETCC:
        return T2_BISECT >= 5;
    case EMU_IR_EXIT: case EMU_IR_EXIT_IF:
        return T2_BISECT >= 6;
    case EMU_IR_LOAD: case EMU_IR_STORE:
        return T2_BISECT >= 7;
    default:
        return false;
    }
}

static bool lower_one(const emu_ir_insn_t *in, const emu_ir_target_t *t)
{
    if (!bisect_allows(in->op)) {
        return false;
    }

    switch ((emu_ir_op_t)in->op) {
    case EMU_IR_NOP:
    case EMU_IR_RETIRE:
        if (in->op == (uint8_t)EMU_IR_RETIRE) {
            /* ADD.W r5, r5, #1 */
            t2_emit32((uint16_t)(0xF100u | T2_CNT), (uint16_t)((T2_CNT << 8) | 1u));
        }
        break;

    case EMU_IR_GET:
        if (t->reg_is_zero != NULL && t->reg_is_zero(in->imm)) {
            t2_imm32(T2_R0, 0u);
        } else {
            t2_ldr_imm(T2_R0, T2_CPU, t->reg_offset(in->imm));
        }
        st_slot(T2_R0, in->dst);
        break;

    case EMU_IR_PUT:
        if (t->reg_is_zero != NULL && t->reg_is_zero(in->imm)) {
            break;
        }
        ld_operand(T2_R0, in->a);
        t2_str_imm(T2_R0, T2_CPU, t->reg_offset(in->imm));
        break;

    case EMU_IR_CONST:
        t2_imm32(T2_R0, in->imm);
        st_slot(T2_R0, in->dst);
        break;

    case EMU_IR_MOV:
        ld_operand(T2_R0, in->a);
        st_slot(T2_R0, in->dst);
        break;

    case EMU_IR_ADD: case EMU_IR_SUB: case EMU_IR_AND:
    case EMU_IR_OR:  case EMU_IR_XOR:
        ld_operand(T2_R0, in->a);
        ld_operand(T2_R1, in->b);
        switch ((emu_ir_op_t)in->op) {
        case EMU_IR_ADD: t2_add(T2_R0, T2_R0, T2_R1); break;
        case EMU_IR_SUB: t2_sub(T2_R0, T2_R0, T2_R1); break;
        case EMU_IR_AND: t2_and(T2_R0, T2_R0, T2_R1); break;
        case EMU_IR_OR:  t2_orr(T2_R0, T2_R0, T2_R1); break;
        default:         t2_eor(T2_R0, T2_R0, T2_R1); break;
        }
        st_slot(T2_R0, in->dst);
        break;

    case EMU_IR_SHL: case EMU_IR_SHR: case EMU_IR_SAR:
        ld_operand(T2_R0, in->a);
        ld_operand(T2_R1, in->b);
        /*
         * ARM shifts use the low *byte* of the count and saturate past
         * 31, where both guests define only the low five bits and expect
         * a shift of 32 to be a shift of 0. Masking is therefore not
         * optional.
         */
        t2_imm32(T2_R2, 31u);
        t2_and(T2_R1, T2_R1, T2_R2);
        t2_shift_reg((in->op == (uint8_t)EMU_IR_SHL) ? T2_LSL
                       : (in->op == (uint8_t)EMU_IR_SHR) ? T2_LSR : T2_ASR,
                       T2_R0, T2_R0, T2_R1);
        st_slot(T2_R0, in->dst);
        break;

    case EMU_IR_SHLI: case EMU_IR_SHRI: case EMU_IR_SARI:
        ld_operand(T2_R0, in->a);
        t2_shift_imm((in->op == (uint8_t)EMU_IR_SHLI) ? T2_LSL
                       : (in->op == (uint8_t)EMU_IR_SHRI) ? T2_LSR : T2_ASR,
                       T2_R0, T2_R0, in->imm);
        st_slot(T2_R0, in->dst);
        break;

    case EMU_IR_NOT:
        ld_operand(T2_R0, in->a);
        t2_mvn(T2_R0, T2_R0);
        st_slot(T2_R0, in->dst);
        break;

    case EMU_IR_NEG:
        ld_operand(T2_R0, in->a);
        t2_neg(T2_R0, T2_R0);
        st_slot(T2_R0, in->dst);
        break;

    /*
     * The bit and byte group, one host instruction each -- which is the
     * whole reason these are IR operations rather than shift-and-mask
     * sequences the backend would have to pattern-match back.
     */
    case EMU_IR_BSWAP32:
        ld_operand(T2_R0, in->a);
        t2_rev(T2_R0, T2_R0);
        st_slot(T2_R0, in->dst);
        break;

    case EMU_IR_BSWAP16:
        ld_operand(T2_R0, in->a);
        t2_rev16(T2_R0, T2_R0);
        st_slot(T2_R0, in->dst);
        break;

    case EMU_IR_HSWAP:
        ld_operand(T2_R0, in->a);
        t2_shift_imm(T2_ROR, T2_R0, T2_R0, 16u);
        st_slot(T2_R0, in->dst);
        break;

    case EMU_IR_CLZ:
        ld_operand(T2_R0, in->a);
        t2_clz(T2_R0, T2_R0);
        st_slot(T2_R0, in->dst);
        break;

    case EMU_IR_CTZ:
        /*
         * RBIT then CLZ. Both are defined for a zero input -- CLZ of
         * zero is 32, which is what the IR specifies -- so unlike the
         * x86 lowering this needs no fixup for the case a bit search is
         * most often handed.
         */
        ld_operand(T2_R0, in->a);
        t2_rbit(T2_R0, T2_R0);
        t2_clz(T2_R0, T2_R0);
        st_slot(T2_R0, in->dst);
        break;

    case EMU_IR_SEXT8:  ld_operand(T2_R0, in->a); t2_sxtb(T2_R0, T2_R0);
                        st_slot(T2_R0, in->dst); break;
    case EMU_IR_SEXT16: ld_operand(T2_R0, in->a); t2_sxth(T2_R0, T2_R0);
                        st_slot(T2_R0, in->dst); break;
    case EMU_IR_ZEXT8:  ld_operand(T2_R0, in->a); t2_uxtb(T2_R0, T2_R0);
                        st_slot(T2_R0, in->dst); break;
    case EMU_IR_ZEXT16: ld_operand(T2_R0, in->a); t2_uxth(T2_R0, T2_R0);
                        st_slot(T2_R0, in->dst); break;

    case EMU_IR_SETCC: {
        /*
         * Compare and materialise a 0 or 1 without branching, using the
         * IT block ARM provides for exactly this.
         *
         * MOVS on a low register writes N and Z, so the false value has
         * to be set up *before* the compare -- doing it between the CMP
         * and the IT destroys the comparison, which is a mistake this
         * project has made and recorded.
         */
        if (in->aux == (uint8_t)EMU_IR_C_ALWAYS) {
            t2_imm32(T2_R0, 1u);
            st_slot(T2_R0, in->dst);
            break;
        }
        ld_operand(T2_R1, in->a);
        ld_operand(T2_R2, in->b);
        t2_imm32(T2_R0, 0u);
        t2_cmp(T2_R1, T2_R2);
        t2_emit16((uint16_t)(0xBF00u | (t2_cond(in->aux) << 4) | 0x8u)); /* IT */
        t2_imm32(T2_R0, 1u);
        st_slot(T2_R0, in->dst);
        break;
    }

    case EMU_IR_SETPC:
        if (in->a != EMU_IR_NO_TEMP) {
            ld_operand(T2_R0, in->a);
        } else {
            t2_imm32(T2_R0, in->imm);
        }
        t2_str_imm(T2_R0, T2_CPU, t->pc_offset);
        break;

    case EMU_IR_EXIT:
        if (in->a != EMU_IR_NO_TEMP) {
            ld_operand(T2_R0, in->a);
        } else {
            t2_imm32(T2_R0, in->imm);
        }
        t2_str_imm(T2_R0, T2_CPU, t->pc_offset);
        note_exit(t2_b_forward(), false);
        break;

    case EMU_IR_EXIT_IF: {
        ld_operand(T2_R0, in->a);
        ld_operand(T2_R1, in->b);
        t2_cmp(T2_R0, T2_R1);
        /* Branch *over* the exit on the inverse condition. */
        uint8_t *const skip =
            t2_bcond_forward(t2_cond(in->aux) ^ 1u);
        t2_imm32(T2_R0, in->imm);
        t2_str_imm(T2_R0, T2_CPU, t->pc_offset);
        note_exit(t2_b_forward(), false);
        patch_branch(skip, emu_jit_here(), true);
        break;
    }

    case EMU_IR_LOAD:
        if (t->load == NULL) {
            return false;
        }
        ld_operand(T2_R1, in->a);
        if (in->imm != 0u) {
            t2_imm32(T2_R2, in->imm);
            t2_add(T2_R1, T2_R1, T2_R2);
        }
        t2_mov(T2_R0, T2_CPU);
        t2_imm32(T2_R2, in->aux);
        /*
         * ADDW r3, sp, #slot -- the out-pointer, as the immediate form.
         *
         * Not "materialise the offset, then ADD.W r3, r3, sp": SP as the
         * register operand of a 32-bit data-processing instruction is
         * UNPREDICTABLE on ARMv7-M, and unpredictable on this part meant
         * it quietly did something that was not an add. The preceding
         * ADD.W with a zero immediate was dead alongside it.
         */
        {
            const uint32_t off = slot(in->dst);

            t2_emit32((uint16_t)(0xF20Du | (((off >> 11) & 1u) << 10)),
                      (uint16_t)((((off >> 8) & 7u) << 12) |
                                 (T2_R3 << 8) | (off & 0xFFu)));
        }
        t2_call((const void *)t->load);
        t2_imm32(T2_R1, 0u);
        t2_cmp(T2_R0, T2_R1);
        note_exit(t2_bcond_forward(t2_cond(EMU_IR_C_NE)), true);
        break;

    case EMU_IR_STORE:
        if (t->store == NULL) {
            return false;
        }
        ld_operand(T2_R1, in->a);
        if (in->imm != 0u) {
            t2_imm32(T2_R2, in->imm);
            t2_add(T2_R1, T2_R1, T2_R2);
        }
        ld_operand(T2_R3, in->b);
        t2_mov(T2_R0, T2_CPU);
        t2_imm32(T2_R2, in->aux);
        t2_call((const void *)t->store);
        t2_imm32(T2_R1, 0u);
        t2_cmp(T2_R0, T2_R1);
        note_exit(t2_bcond_forward(t2_cond(EMU_IR_C_NE)), true);
        break;

    /*
     * Not lowered yet. Each is real work rather than an oversight:
     * SETF and GETCOND need the guest's flag word rebuilt from APSR,
     * SELECT needs it read back, the memory bit ops need the
     * load-modify-store sequence, and the multiplies need UMULL/SMULL
     * with their register pairs. Returning false discards the block,
     * which the framework treats exactly as a declined translation --
     * so this is a coverage cost, not a correctness one.
     */
    case EMU_IR_SETF:
    case EMU_IR_GETCOND:
    case EMU_IR_SELECT:
    case EMU_IR_BITOP_SET:
    case EMU_IR_BITOP_CLR:
    case EMU_IR_BITOP_INV:
    case EMU_IR_BITOP_TST:
    case EMU_IR_HELPER:
    case EMU_IR_HELPER_TRAP:
    case EMU_IR_POPCNT:
    case EMU_IR_MUL: case EMU_IR_MULHS: case EMU_IR_MULHU:
    case EMU_IR_ROTL: case EMU_IR_ROTLI:
    case EMU_IR_ADDI: case EMU_IR_ANDI:
    case EMU_IR_ORI:  case EMU_IR_XORI:
    case EMU_IR_BEXT: case EMU_IR_BSET:
    case EMU_IR_BCLR: case EMU_IR_BINV:
    default:
        return false;
    }
    return true;
}

bool emu_ir_lower(const emu_ir_block_t *b, const emu_ir_target_t *t)
{
    if (b->overflow) {
        return false;
    }

    g_ntemps = (uint16_t)b->next_temp;
    g_nexits = 0u;

    /*
     * The frame, 8-byte aligned because AAPCS requires sp to be so at a
     * public interface and a helper call is one.
     */
    const uint32_t frame = (((b->next_temp + 2u) * 4u) + 7u) & ~7u;

    /* PUSH {r4, r5, lr} */
    t2_emit16(0xB530u);
    t2_mov(T2_CPU, T2_R0);
    t2_imm32(T2_CNT, 0u);
    if (frame != 0u) {
        /* SUB.W sp, sp, #frame */
        t2_imm32(T2_R12, frame);
        t2_emit32(0xEBADu, (uint16_t)((13u << 8) | T2_R12));
    }

    for (uint32_t i = 0; i < b->count; i++) {
        if (b->insn[i].dead) {
            continue;
        }
        if (!lower_one(&b->insn[i], t)) {
            return false;
        }
        if (emu_jit_overflowed()) {
            return false;
        }
    }

    /*
     * Every early exit lands here, before the frame is given back, so
     * all paths undo the same adjustment. Branching straight to the
     * return instead would leave sp low and pop the wrong words.
     */
    for (uint32_t i = 0; i < g_nexits; i++) {
        patch_branch(g_exits[i].at, emu_jit_here(),
                     g_exits[i].conditional);
    }

    if (frame != 0u) {
        t2_imm32(T2_R12, frame);
        t2_emit32(0xEB0Du, (uint16_t)((13u << 8) | T2_R12));   /* ADD.W sp, sp */
    }
    t2_mov(T2_R0, T2_CNT);
    t2_emit16(0xBD30u);                                     /* POP {r4,r5,pc} */

    return !emu_jit_overflowed();
}

#endif /* EMU_JIT_THUMB2 */
