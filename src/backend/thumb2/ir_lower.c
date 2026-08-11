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
 * Every temp has a home in the frame, and r6-r11 -- everything AAPCS
 * makes callee-saved that is not already spoken for -- are handed out on
 * top of that by emu_ir_regalloc. A temp with a register never touches
 * its slot; a temp without one behaves exactly as everything did before
 * there was an allocator.
 *
 * Six registers where x86-64 has four, and they cost less to save: a
 * whole list goes in one PUSH, so the second and later ones are free
 * where on x86-64 each is its own instruction. That does not mean six
 * are worth having. On x86-64 the same allocator measured 3.6% of
 * CoreMark's emitted code for the first register and 0.6% for the
 * fourth, which says the live sets in these blocks are small -- as they
 * would be at 4.12 guest instructions a block.
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

/*
 * Where the allocator put each temp, and how many registers that took.
 *
 * A temp with a register never touches its frame slot: the slot still
 * exists and is simply unused, which is what stops the two homes from
 * ever disagreeing. Reaching one is a MOV.W against an LDR.W -- four
 * bytes either way here, unlike x86-64 -- so what this saves on this
 * host is the *pair*, the store and the reload, not the width of one
 * access.
 */
static uint8_t  g_reg[EMU_IR_MAX_TEMPS];
static uint32_t g_nsaved;

/*
 * The register holding temp `n`, or -1 for one that lives in the frame.
 * The bound is not paranoia: SCRATCH_ADDR and SCRATCH_VAL are slots past
 * the end of the block's temps, so they index past what the allocator
 * filled in and must always answer "in the frame".
 */
static int phys(uint16_t n)
{
    if (n >= g_ntemps || n >= EMU_IR_MAX_TEMPS) {
        return -1;
    }
    return (g_reg[n] == EMU_IR_NO_REG) ? -1 : (int)t2_alloc_regs[g_reg[n]];
}

void ld_slot(uint32_t rt, uint16_t n)
{
    t2_ldr_imm(rt, 13u, slot(n));         /* [sp, #off] */
}

/*
 * What r0 still holds from the previous instruction, if anything.
 *
 * A dependent pair otherwise emits a store and then an immediate reload
 * of the same slot, and a quarter to a third of adjacent instruction
 * pairs in this project's guests are dependent. This declines the
 * reload when the value is already there.
 *
 * Not a register allocator: the store still happens, so every slot stays
 * valid and no liveness analysis is needed. It costs nothing -- no setup
 * instruction, no extra register held across a call -- unlike the
 * guest-register cache tried on this host before, which measured 15.5%
 * slower.
 *
 * It matters more here than on x86-64. This is the host where code size
 * sets performance: the cache is 12 KB, CoreMark's translated working
 * set does not fit, and bytes removed are compactions avoided.
 *
 * The window is one instruction wide and closes at the first load of the
 * next. Anything reached by a branch, and anything after a call, starts
 * holding nothing -- which falls out of clearing at the top of every
 * instruction and only re-establishing it at a final store.
 */
/*
 * A store that need not be emitted: the value has exactly one reader,
 * that reader is the next live instruction, and it takes its left
 * operand from r0 -- which the reload elision hands over directly. Both
 * halves matter; `uses == 1` alone would allow a reader further down
 * the block with r0 clobbered in between.
 */
static uint16_t g_dead_store;

/* Ops whose first act is ld_operand(r0, in->a), so the elision applies. */
static bool reads_a_in_r0(uint8_t op)
{
    switch ((emu_ir_op_t)op) {
    case EMU_IR_MOV:
    case EMU_IR_ADD: case EMU_IR_SUB: case EMU_IR_AND:
    case EMU_IR_OR:  case EMU_IR_XOR:
    case EMU_IR_SHL: case EMU_IR_SHR: case EMU_IR_SAR:
    case EMU_IR_SHLI: case EMU_IR_SHRI: case EMU_IR_SARI:
    case EMU_IR_NOT: case EMU_IR_NEG:
    case EMU_IR_BSWAP32: case EMU_IR_BSWAP16: case EMU_IR_HSWAP:
    case EMU_IR_CLZ: case EMU_IR_CTZ:
    case EMU_IR_SEXT8: case EMU_IR_SEXT16:
    case EMU_IR_ZEXT8: case EMU_IR_ZEXT16:
    case EMU_IR_PUT:
    case EMU_IR_SETPC: case EMU_IR_EXIT:
        return true;
    default:
        return false;
    }
}

static uint16_t g_r0_holds;
static uint16_t g_r0_avail;
static bool     g_r0_first;

void st_slot(uint32_t rt, uint16_t n)
{
    const int p = phys(n);

    if (p >= 0) {
        /*
         * The register is this temp's home, so the write happens even
         * where a dead-store test would skip it -- a later instruction
         * reads the register, not the slot. In practice the two never
         * collide, because the allocator declines exactly the temps the
         * dead-store rule claims; the ordering here is what makes that a
         * coincidence rather than a dependency.
         */
        if (p != (int)rt) {
            t2_mov((uint32_t)p, rt);
        }
        if (rt == T2_R0) {
            g_r0_holds = n;
        }
        return;
    }
    if (rt == T2_R0 && n == g_dead_store) {
        g_r0_holds = n;             /* nothing reads the slot */
        return;
    }
    t2_str_imm(rt, 13u, slot(n));

    if (rt == T2_R0) {
        g_r0_holds = n;
    } else if (n == g_r0_holds) {
        g_r0_holds = EMU_IR_NO_TEMP;
    }
}

/*
 * Where to read an operand from, and where to write a result.
 *
 * These are what make allocation worth anything on this host. Replacing
 * a frame access with a register move saves nothing here -- LDR.W and
 * MOV.W are four bytes each, unlike x86-64 where the same substitution
 * is eight bytes against three -- and the first version of this backend
 * did exactly that and measured *worse*: 6040 translations against 5943
 * and 410 buffer overflows, from the wider PUSH and the load detour with
 * nothing on the other side of the ledger.
 *
 * What pays is using the register in place. Thumb-2 is three-address, so
 * an ADD whose two operands and destination are all allocated is one
 * instruction where the frame needs four -- two loads, the add and a
 * store, sixteen bytes down to four.
 *
 * use_reg emits the load only when the temp has no register; def_reg
 * names where to compute, and the st_slot that follows becomes a no-op
 * when that was the temp's own register.
 */
static uint32_t use_reg(uint16_t n, uint32_t scratch);
static uint32_t def_reg(uint16_t n, uint32_t scratch);

/* Load an operand, tolerating EMU_IR_NO_TEMP so callers need not check. */
void ld_operand(uint32_t rt, uint16_t n)
{
    /*
     * The elision is consulted before the allocation, because a value
     * both in a register and still in r0 is cheaper taken from r0 -- no
     * instruction at all against a MOV.W.
     */
    if (rt == T2_R0 && g_r0_first) {
        g_r0_first = false;
        if (n != EMU_IR_NO_TEMP && n == g_r0_avail) {
            return;                 /* already there */
        }
    }

    const int p = phys(n);

    if (p >= 0) {
        if (p != (int)rt) {
            t2_mov(rt, (uint32_t)p);
        }
        return;
    }
    if (n == EMU_IR_NO_TEMP) {
        t2_imm32(rt, 0u);
        return;
    }
    ld_slot(rt, n);
}


static uint32_t use_reg(uint16_t n, uint32_t scratch)
{
    const int p = phys(n);

    if (p >= 0) {
        return (uint32_t)p;
    }
    ld_operand(scratch, n);
    return scratch;
}

static uint32_t def_reg(uint16_t n, uint32_t scratch)
{
    const int p = phys(n);

    return (p >= 0) ? (uint32_t)p : scratch;
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
    case EMU_IR_MUL: case EMU_IR_MULHS: case EMU_IR_MULHU:
        return T2_BISECT >= 7;
    case EMU_IR_LOAD: case EMU_IR_STORE:
        return T2_BISECT >= 7;
    default:
        return false;
    }
}

static bool lower_one(const emu_ir_insn_t *in, const emu_ir_target_t *t)
{
    g_r0_avail = g_r0_holds;
    g_r0_holds = EMU_IR_NO_TEMP;
    g_r0_first = true;

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

    case EMU_IR_GET: {
        const uint32_t rd = def_reg(in->dst, T2_R0);

        if (t->reg_is_zero != NULL && t->reg_is_zero(in->imm)) {
            t2_imm32(rd, 0u);
        } else {
            t2_ldr_imm(rd, T2_CPU, t->reg_offset(in->imm));
        }
        st_slot(rd, in->dst);
        break;
    }

    case EMU_IR_PUT:
        if (t->reg_is_zero != NULL && t->reg_is_zero(in->imm)) {
            break;
        }
        t2_str_imm(use_reg(in->a, T2_R0), T2_CPU, t->reg_offset(in->imm));
        break;

    case EMU_IR_CONST: {
        const uint32_t rd = def_reg(in->dst, T2_R0);

        t2_imm32(rd, in->imm);
        st_slot(rd, in->dst);
        break;
    }

    case EMU_IR_MOV: {
        const uint32_t ra = use_reg(in->a, T2_R0);
        const uint32_t rd = def_reg(in->dst, T2_R0);

        if (rd != ra) {
            t2_mov(rd, ra);
        }
        st_slot(rd, in->dst);
        break;
    }

    case EMU_IR_ADD: case EMU_IR_SUB: case EMU_IR_AND:
    case EMU_IR_OR:  case EMU_IR_XOR: {
        const uint32_t ra = use_reg(in->a, T2_R0);
        const uint32_t rb = use_reg(in->b, T2_R1);
        const uint32_t rd = def_reg(in->dst, T2_R0);

        switch ((emu_ir_op_t)in->op) {
        case EMU_IR_ADD: t2_add(rd, ra, rb); break;
        case EMU_IR_SUB: t2_sub(rd, ra, rb); break;
        case EMU_IR_AND: t2_and(rd, ra, rb); break;
        case EMU_IR_OR:  t2_orr(rd, ra, rb); break;
        default:         t2_eor(rd, ra, rb); break;
        }
        st_slot(rd, in->dst);
        break;
    }

    case EMU_IR_SHL: case EMU_IR_SHR: case EMU_IR_SAR: {
        const uint32_t ra = use_reg(in->a, T2_R0);
        const uint32_t rb = use_reg(in->b, T2_R1);
        const uint32_t rd = def_reg(in->dst, T2_R0);

        /*
         * ARM shifts use the low *byte* of the count and saturate past
         * 31, where both guests define only the low five bits and expect
         * a shift of 32 to be a shift of 0. Masking is therefore not
         * optional.
         *
         * The mask lands in r2 rather than back in `rb`: with allocation
         * `rb` may be the count's own register, and narrowing it in
         * place would corrupt a value later instructions still read.
         */
        t2_imm32(T2_R2, 31u);
        t2_and(T2_R2, rb, T2_R2);
        t2_shift_reg((in->op == (uint8_t)EMU_IR_SHL) ? T2_LSL
                       : (in->op == (uint8_t)EMU_IR_SHR) ? T2_LSR : T2_ASR,
                       rd, ra, T2_R2);
        st_slot(rd, in->dst);
        break;
    }

    case EMU_IR_SHLI: case EMU_IR_SHRI: case EMU_IR_SARI: {
        const uint32_t ra = use_reg(in->a, T2_R0);
        const uint32_t rd = def_reg(in->dst, T2_R0);

        t2_shift_imm((in->op == (uint8_t)EMU_IR_SHLI) ? T2_LSL
                       : (in->op == (uint8_t)EMU_IR_SHRI) ? T2_LSR : T2_ASR,
                       rd, ra, in->imm);
        st_slot(rd, in->dst);
        break;
    }

    case EMU_IR_NOT: {
        const uint32_t ra = use_reg(in->a, T2_R0);
        const uint32_t rd = def_reg(in->dst, T2_R0);

        t2_mvn(rd, ra);
        st_slot(rd, in->dst);
        break;
    }

    case EMU_IR_NEG: {
        const uint32_t ra = use_reg(in->a, T2_R0);
        const uint32_t rd = def_reg(in->dst, T2_R0);

        t2_neg(rd, ra);
        st_slot(rd, in->dst);
        break;
    }

    /*
     * The bit and byte group, one host instruction each -- which is the
     * whole reason these are IR operations rather than shift-and-mask
     * sequences the backend would have to pattern-match back.
     */
    case EMU_IR_BSWAP32: case EMU_IR_BSWAP16: case EMU_IR_HSWAP:
    case EMU_IR_CLZ: case EMU_IR_CTZ:
    case EMU_IR_SEXT8: case EMU_IR_SEXT16:
    case EMU_IR_ZEXT8: case EMU_IR_ZEXT16: {
        const uint32_t ra = use_reg(in->a, T2_R0);
        const uint32_t rd = def_reg(in->dst, T2_R0);

        switch ((emu_ir_op_t)in->op) {
        case EMU_IR_BSWAP32: t2_rev(rd, ra); break;
        case EMU_IR_BSWAP16: t2_rev16(rd, ra); break;
        case EMU_IR_HSWAP:   t2_shift_imm(T2_ROR, rd, ra, 16u); break;
        case EMU_IR_CLZ:     t2_clz(rd, ra); break;
        /*
         * RBIT then CLZ. Both are defined for a zero input -- CLZ of
         * zero is 32, which is what the IR specifies -- so unlike the
         * x86 lowering this needs no fixup for the case a bit search is
         * most often handed.
         */
        case EMU_IR_CTZ:     t2_rbit(rd, ra); t2_clz(rd, rd); break;
        case EMU_IR_SEXT8:   t2_sxtb(rd, ra); break;
        case EMU_IR_SEXT16:  t2_sxth(rd, ra); break;
        case EMU_IR_ZEXT8:   t2_uxtb(rd, ra); break;
        default:             t2_uxth(rd, ra); break;
        }
        st_slot(rd, in->dst);
        break;
    }

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
        const uint32_t rd = def_reg(in->dst, T2_R0);

        if (in->aux == (uint8_t)EMU_IR_C_ALWAYS) {
            t2_imm32(rd, 1u);
            st_slot(rd, in->dst);
            break;
        }

        const uint32_t ra = use_reg(in->a, T2_R1);
        const uint32_t rb = use_reg(in->b, T2_R2);

        t2_imm32(rd, 0u);
        t2_cmp(ra, rb);
        t2_emit16((uint16_t)(0xBF00u | (t2_cond(in->aux) << 4) | 0x8u)); /* IT */
        t2_imm32(rd, 1u);
        st_slot(rd, in->dst);
        break;
    }

    case EMU_IR_SETPC:
    case EMU_IR_EXIT: {
        uint32_t rp = T2_R0;

        if (in->a != EMU_IR_NO_TEMP) {
            rp = use_reg(in->a, T2_R0);
        } else {
            t2_imm32(T2_R0, in->imm);
        }
        t2_str_imm(rp, T2_CPU, t->pc_offset);
        if (in->op == (uint8_t)EMU_IR_EXIT) {
            note_exit(t2_b_forward(), false);
        }
        break;
    }

    case EMU_IR_EXIT_IF: {
        const uint32_t ra = use_reg(in->a, T2_R0);
        const uint32_t rb = use_reg(in->b, T2_R1);

        t2_cmp(ra, rb);
        /* Branch *over* the exit on the inverse condition. */
        uint8_t *const skip =
            t2_bcond_forward(t2_cond(in->aux) ^ 1u);
        t2_imm32(T2_R0, in->imm);
        t2_str_imm(T2_R0, T2_CPU, t->pc_offset);
        note_exit(t2_b_forward(), false);
        patch_branch(skip, emu_jit_here(), true);
        break;
    }

    case EMU_IR_MUL: {
        const uint32_t ra = use_reg(in->a, T2_R0);
        const uint32_t rb = use_reg(in->b, T2_R1);
        const uint32_t rd = def_reg(in->dst, T2_R0);

        t2_mul(rd, ra, rb);
        st_slot(rd, in->dst);
        break;
    }

    case EMU_IR_MULHS:
    case EMU_IR_MULHU: {
        /*
         * SMULL/UMULL write both halves and their two destinations must
         * differ, so the low half lands in a register that is then
         * discarded. Only the high half is wanted.
         *
         * r1 takes the discard because the high half is either an
         * allocated register -- r6 and above -- or r0, so the two can
         * never name the same register whatever the allocator did.
         */
        const uint32_t ra = use_reg(in->a, T2_R2);
        const uint32_t rb = use_reg(in->b, T2_R3);
        const uint32_t rd = def_reg(in->dst, T2_R0);

        t2_mull(in->op == (uint8_t)EMU_IR_MULHS, T2_R1, rd, ra, rb);
        st_slot(rd, in->dst);
        break;
    }

    case EMU_IR_LOAD: {
        if (t->load == NULL) {
            return false;
        }
        /*
         * The helper writes through a pointer, and a register has no
         * address -- so an allocated destination lands in the scratch
         * slot and is fetched from there. That is the one place
         * allocation can lose: a load result read once pays an extra
         * LDR.W here to save a MOV.W at the use. Left to the allocator's
         * own rule rather than special-cased, because a load result read
         * twice already wins.
         */
        const int pd = phys(in->dst);
        const uint32_t ra = use_reg(in->a, T2_R1);

        /*
         * The address has to end up in r1, and `ra` may be an allocated
         * register a later instruction still reads -- so the sum is
         * built into r1 rather than back into `ra`.
         */
        if (in->imm != 0u) {
            t2_imm32(T2_R2, in->imm);
            t2_add(T2_R1, ra, T2_R2);
        } else if (ra != T2_R1) {
            t2_mov(T2_R1, ra);
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
            const uint32_t off = slot((pd >= 0) ? SCRATCH_VAL : in->dst);

            t2_emit32((uint16_t)(0xF20Du | (((off >> 11) & 1u) << 10)),
                      (uint16_t)((((off >> 8) & 7u) << 12) |
                                 (T2_R3 << 8) | (off & 0xFFu)));
        }
        t2_call((const void *)t->load);
        t2_imm32(T2_R1, 0u);
        t2_cmp(T2_R0, T2_R1);
        note_exit(t2_bcond_forward(t2_cond(EMU_IR_C_NE)), true);
        if (pd >= 0) {
            ld_slot((uint32_t)pd, SCRATCH_VAL);
        }
        break;
    }

    case EMU_IR_STORE: {
        if (t->store == NULL) {
            return false;
        }
        const uint32_t ra = use_reg(in->a, T2_R1);

        if (in->imm != 0u) {
            t2_imm32(T2_R2, in->imm);
            t2_add(T2_R1, ra, T2_R2);
        } else if (ra != T2_R1) {
            t2_mov(T2_R1, ra);
        }

        const uint32_t rv = use_reg(in->b, T2_R3);

        if (rv != T2_R3) {
            t2_mov(T2_R3, rv);
        }
        t2_mov(T2_R0, T2_CPU);
        t2_imm32(T2_R2, in->aux);
        t2_call((const void *)t->store);
        t2_imm32(T2_R1, 0u);
        t2_cmp(T2_R0, T2_R1);
        note_exit(t2_bcond_forward(t2_cond(EMU_IR_C_NE)), true);
        break;
    }

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
    g_r0_holds = EMU_IR_NO_TEMP;
    g_dead_store = EMU_IR_NO_TEMP;
    g_nexits = 0u;

    g_nsaved = emu_ir_regalloc(b, T2_ALLOC_REGS, g_reg);

    uint32_t list = (1u << T2_CPU) | (1u << T2_CNT);

    for (uint32_t i = 0; i < g_nsaved; i++) {
        list |= 1u << t2_alloc_regs[i];
    }

    /*
     * The frame, padded so that sp is 8-byte aligned at a helper call.
     *
     * AAPCS requires that at a public interface and a helper is one.
     * What decides it is the *total* below the caller's sp, which is the
     * saved registers plus the frame -- and three saved registers is 12
     * bytes, so before there was anything to allocate this was already
     * four out. That was benign here only because nothing a helper
     * reaches uses LDRD or a double.
     */
    uint32_t frame = (((b->next_temp + 2u) * 4u) + 7u) & ~7u;

    if ((((g_nsaved + 3u) * 4u) + frame) % 8u != 0u) {
        frame += 4u;
    }

    t2_push(list | T2_LIST_LR);
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
        g_dead_store = EMU_IR_NO_TEMP;
        if (b->insn[i].dst != EMU_IR_NO_TEMP && b->insn[i].uses == 1u) {
            uint32_t j = i + 1u;

            while (j < b->count && b->insn[j].dead) {
                j++;
            }
            if (j < b->count && b->insn[j].a == b->insn[i].dst &&
                reads_a_in_r0(b->insn[j].op)) {
                g_dead_store = b->insn[i].dst;
            }
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
    t2_pop(list | T2_LIST_PC);

    return !emu_jit_overflowed();
}

#endif /* EMU_JIT_THUMB2 */
