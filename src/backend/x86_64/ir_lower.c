/* SPDX-License-Identifier: Apache-2.0 */
/*
 * emu_ir_x86_64.c - Lowering the IR to x86-64.
 *
 * The backend half of frontend -> IR -> optimisation -> backend. It knows
 * the host and nothing about any guest: which registers exist, how to
 * spell an add, what the calling convention wants. Everything guest-
 * specific arrives through emu_ir_target_t, filled in once by the
 * frontend.
 *
 * Every temp has a home in a stack frame, and the four callee-saved
 * registers System V leaves free are handed out on top of that by
 * emu_ir_regalloc. A temp with a register never touches its slot; a temp
 * without one behaves exactly as everything did before there was an
 * allocator, which is what keeps the two homes from ever disagreeing.
 *
 * Four is not many, and the frame is the reason that is survivable: a
 * value that misses out is still correct and merely costs eight bytes an
 * access instead of three. Three mechanisms now sit on top of each other
 * and their domains are deliberately disjoint --
 *
 *   allocation   a value read more than once, or read far from its
 *                definition, lives in a register for its whole range
 *   reload       a value still in T0 from the previous instruction is
 *   elision      not reloaded at all, register or not
 *   dead store   a value whose single reader is the next instruction is
 *                never written anywhere
 *
 * -- and the allocator declines precisely the temps the dead-store rule
 * claims, so the two cannot both fire on one value.
 */

#include "emu/emu_ir.h"
#include "emu/emu_x86_64.h"

#if defined(EMU_JIT_X86_64)

/*
 * Scratch registers. rbx and rbp are taken by the framework (cpu pointer
 * and retired count) and rsp is the frame, so these three are what is
 * left that no helper call needs preserved across it -- which is the
 * point: everything live is in the frame, so a call clobbers nothing.
 */
#define T0 X86_EAX
#define T1 X86_ECX
#define T2 X86_EDX

/* Where temp `n` lives, as a displacement from rsp. */
static uint32_t slot(uint16_t n)
{
    return (uint32_t)n * 4u;
}

/*
 * Two slots past the block's temps, for the memory operations: a call
 * clobbers every scratch register, so an address that has to survive one
 * needs somewhere the call cannot reach.
 */
static uint16_t g_ntemps;
static uint32_t g_stats_elided;

/*
 * A store that need not be emitted.
 *
 * The frame round trip is a store *and* a load. The reload elision above
 * takes the load; this takes the store, in the one case where it is
 * provably never read: the value has exactly one reader, that reader is
 * the very next instruction, and that instruction takes its left operand
 * from T0 -- which the elision will hand over directly.
 *
 * Both halves of the condition are needed. `uses == 1` alone is not
 * enough, because the single reader might be further down the block with
 * other instructions clobbering T0 in between; "the next instruction"
 * alone is not enough, because a second reader later would find an
 * empty slot.
 *
 * uses is counted after every pass that deletes code, so a reader that
 * is itself about to be swept does not keep a store alive.
 */
static uint16_t g_dead_store;

/* Ops whose first operand load is `a`, so the elision reaches it. */
static bool reads_a_in_t0(uint8_t op)
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
        return true;
    default:
        return false;
    }
}

#define SCRATCH_ADDR ((uint16_t)(g_ntemps))
#define SCRATCH_VAL  ((uint16_t)(g_ntemps + 1u))
/* Two more for MXCSR: the caller's, and the one this block runs under. */
#define SCRATCH_MXOLD ((uint16_t)(g_ntemps + 2u))
#define SCRATCH_MXCUR ((uint16_t)(g_ntemps + 3u))

/*
 * MXCSR's sticky exception bits, mapped to EMU_IR_FE_*.
 *
 * A table because the two orders share no shift: x86 is
 * IE,DE,ZE,OE,UE,PE from bit 0 and the IR is NX,UF,OF,DZ,NV, so the
 * permutation is 0->4, 2->3, 3->2, 4->1, 5->0. Emitting that inline is
 * about twenty instructions on a path taken once per block; a lookup is
 * five. DE, the denormal flag, has no counterpart and is dropped -- both
 * guests define subnormals as ordinary values.
 *
 * Filled once rather than written out, because sixty-four hand-computed
 * constants is exactly the sort of table that is wrong in one entry.
 */
static uint8_t g_fe_map[64];
static bool    g_fe_map_ready;
static bool    g_has_fp;

static void fe_map_init(void)
{
    if (g_fe_map_ready) {
        return;
    }
    for (unsigned i = 0; i < 64u; i++) {
        uint8_t v = 0u;

        if ((i & (1u << 0)) != 0u) { v |= EMU_IR_FE_INVALID; }
        if ((i & (1u << 2)) != 0u) { v |= EMU_IR_FE_DIVBYZERO; }
        if ((i & (1u << 3)) != 0u) { v |= EMU_IR_FE_OVERFLOW; }
        if ((i & (1u << 4)) != 0u) { v |= EMU_IR_FE_UNDERFLOW; }
        if ((i & (1u << 5)) != 0u) { v |= EMU_IR_FE_INEXACT; }
        g_fe_map[i] = v;
    }
    g_fe_map_ready = true;
}

/*
 * Where the allocator put each temp, and how many registers that took.
 *
 * A temp with a register never touches its frame slot at all: the slot
 * still exists and is simply unused, which is what keeps the two homes
 * from ever disagreeing. Reaching one costs three bytes against the
 * eight a `[rsp + disp32]` access needs, and on this backend code size
 * is what the IR path has been losing on.
 */
static uint8_t  g_reg[EMU_IR_MAX_TEMPS];
static uint32_t g_nsaved;

/*
 * The physical register holding temp `n`, or -1 for one that lives in
 * the frame. The bound is not paranoia: SCRATCH_ADDR and SCRATCH_VAL are
 * slots past the end of the block's temps, so they index past what the
 * allocator filled in and must always answer "in the frame".
 */
static int phys(uint16_t n)
{
    if (n >= g_ntemps || n >= EMU_IR_MAX_TEMPS) {
        return -1;
    }
    return (g_reg[n] == EMU_IR_NO_REG) ? -1 : x86_alloc_regs[g_reg[n]];
}

static void ld_slot(int reg, uint16_t n)
{
    x86_ld_rsp(reg, slot(n));
}

/*
 * What T0 still holds from the previous instruction, if anything.
 *
 * With every value in a frame slot, a dependent pair emits a store and
 * then an immediate reload of the same slot -- and a quarter to a third
 * of adjacent instruction pairs in this project's guests are dependent.
 * This declines to emit that reload when the value is already in the
 * register.
 *
 * It is not a register allocator and does not pretend to be one. The
 * store still happens, so every slot stays valid and no liveness
 * analysis is needed; only the load is saved. What it costs is nothing:
 * there is no setup instruction and no extra register held across a
 * call, which is what sank the guest-register cache this project tried
 * on ARM and measured 15.5% slower.
 *
 * The window is one instruction and closes at the first emission of the
 * next, because after that T0's contents are whatever that instruction
 * put there. Anything reached by a branch, and anything after a call,
 * starts with nothing held -- both fall out of clearing at the top of
 * every instruction and only re-establishing it at a final store.
 */
static uint16_t g_t0_holds;    /* set by a store, read by the next load */
static uint16_t g_t0_avail;    /* what was held when this insn started  */
static bool     g_t0_first;    /* still before this insn's first load?  */

static void st_slot(int reg, uint16_t n)
{
    const int p = phys(n);

    if (p >= 0) {
        /*
         * The register is this temp's home, so the write has to happen
         * even when a dead-store test would otherwise skip it -- a later
         * instruction reads the register, not the slot. In practice the
         * two never collide, because the allocator declines exactly the
         * temps the dead-store rule claims; the ordering here is what
         * makes that a coincidence rather than a dependency.
         */
        if (p != reg) {
            x86_mov_rr(p, reg);
        }
        if (reg == T0) {
            g_t0_holds = n;
        }
        return;
    }
    if (reg == T0 && n == g_dead_store) {
        /* Nothing will read the slot; the value stays in T0. */
        g_t0_holds = n;
        g_stats_elided++;
        return;
    }
    x86_st_rsp(reg, slot(n));

    if (reg == T0) {
        g_t0_holds = n;
    } else if (n == g_t0_holds) {
        /* The slot was rewritten from elsewhere; T0 is stale for it. */
        g_t0_holds = EMU_IR_NO_TEMP;
    }
}

/*
 * Where to compute a result: its own register when it has one, else the
 * scratch. The st_slot that follows becomes a no-op in the first case,
 * which is the whole saving -- three bytes an instruction, against the
 * five that reaching the register rather than the slot already saved.
 *
 * Only for lowerings whose emitters take a register. Several here are
 * written in terms of eax specifically -- the setcc and movzx pair, the
 * bit-search sequence, the one-operand multiply -- and stay that way.
 */
static int dst_reg(uint16_t n)
{
    const int p = phys(n);

    return (p >= 0) ? p : T0;
}

/* Load an operand, tolerating EMU_IR_NO_TEMP so callers need not check. */
static void ld_operand(int reg, uint16_t n)
{
    /*
     * The elision is consulted before the allocation, because a value
     * that is both in a register and still in T0 is cheaper taken from
     * T0 -- nothing at all, or a three-byte mov, against a slot load.
     *
     * It fires only on an instruction's *first* operand load, whatever
     * register that goes to. The restriction is what makes delivering
     * into a register other than T0 safe: several lowerings write T0
     * partway through -- emit_mem_call materialises the callee there --
     * so a window still open at the second load would hand over a
     * function pointer. Being first means nothing in this instruction
     * has run yet.
     */
    if (g_t0_first) {
        g_t0_first = false;
        if (n != EMU_IR_NO_TEMP && n == g_t0_avail) {
            g_stats_elided++;
            if (reg != T0) {
                x86_mov_rr(reg, T0);
            }
            return;
        }
    }

    const int p = phys(n);

    if (p >= 0) {
        if (p != reg) {
            x86_mov_rr(reg, p);
        }
        return;
    }
    if (n == EMU_IR_NO_TEMP) {
        x86_mov_imm32(reg, 0u);
        return;
    }
    ld_slot(reg, n);
}

/* ------------------------------------------------------------------ */
/* Flags                                                               */
/* ------------------------------------------------------------------ */

/*
 * Materialise the guest's flag word from an x86 result.
 *
 * Only the flags in `live` are computed. That is the payoff of the
 * dead-flag pass: on a guest that sets four flags per arithmetic
 * instruction, most SETFs are gone entirely and the survivors often want
 * only Z.
 *
 * Confined to eax/ecx/edx. An earlier hand-written version of this
 * sequence in the G4MH backend clobbered esi and the failure was a wrong
 * value several instructions later, which is why the register set here
 * is stated rather than assumed.
 */
static void emit_setf(const emu_ir_insn_t *in, const emu_ir_target_t *t)
{
    const uint8_t live = in->live;

    if (live == 0u) {
        return;
    }

    /* The result, and for ADD/SUB the operands that decide V and C. */
    ld_operand(T0, in->a);

    /*
     * Recompute the operation for its flags rather than trying to have
     * kept them from the original: the value-producing instruction is a
     * separate IR instruction and anything could sit between them, so the
     * host flags from it are long gone.
     */
    switch ((emu_ir_flagsrc_t)in->aux) {
    case EMU_IR_FS_ADD:
    case EMU_IR_FS_SUB:
        ld_operand(T1, in->b);
        /* cmp/add sets CF and OF as the guest defines them. */
        x86_alu_rr(in->aux == (uint8_t)EMU_IR_FS_ADD ? X86_ADD : X86_SUB,
                   T0, T1);
        break;
    case EMU_IR_FS_LOGIC:
    case EMU_IR_FS_ZS:
    default:
        /* test sets ZF and SF and clears CF and OF, which is exactly the
         * logical-operation definition. */
        x86_alu_rr(X86_TEST, T0, T0);
        break;
    }

    /*
     * Read the flag word, clear the bits being redefined, OR in the new
     * ones. Read-modify-write because a guest's flag word holds more than
     * these four bits -- G4MH's PSW carries the interrupt-disable and
     * privilege state in the same register, and clobbering those would
     * be a control-flow bug rather than a wrong arithmetic result.
     */
    static const uint8_t k_cc[4] = {
        X86_CC_E,    /* Z */
        X86_CC_L,    /* S: sign, taken as "less than zero"  */
        0u,          /* V: overflow, handled below          */
        X86_CC_B     /* C: carry/borrow                     */
    };

    uint32_t clear = 0u;
    for (unsigned f = 0; f < 4u; f++) {
        if ((live & (1u << f)) != 0u) {
            clear |= t->flag_bit[f];
        }
    }

    /*
     * edx accumulates the new flag bits. Built before the flag word is
     * touched so that the setcc sequence still sees the x86 flags the
     * operation above left.
     */
    x86_mov_imm32(T2, 0u);
    for (unsigned f = 0; f < 4u; f++) {
        if ((live & (1u << f)) == 0u || t->flag_bit[f] == 0u) {
            continue;
        }
        /*
         * setcc into al, then fold. Overflow has no jcc alias in the
         * table above because its condition code is 0x80, which is not
         * one of the named ones.
         */
        emu_jit_emit8(0x0F);
        emu_jit_emit8((f == 2u) ? 0x90u : X86_SET(k_cc[f]));  /* seto/setcc al */
        emu_jit_emit8(0xC0);
        x86_movzx8(T0, T0);
        x86_shift_imm(T0, X86_SHL, (uint32_t)__builtin_ctz(t->flag_bit[f]));
        x86_alu_rr(X86_OR, T2, T0);
    }

    x86_ld_cpu(T1, t->flags_offset);
    x86_mov_imm32(T0, ~clear);
    x86_alu_rr(X86_AND, T1, T0);
    x86_alu_rr(X86_OR, T1, T2);
    x86_st_cpu(T1, t->flags_offset);
}

/* ------------------------------------------------------------------ */
/* Lowering                                                            */
/* ------------------------------------------------------------------ */

/*
 * Sites that need patching to the block epilogue.
 *
 * A trapping helper and a taken EXIT_IF both leave the block early, and
 * neither knows where the end is when it is emitted. Collected here and
 * patched once the frame teardown has been placed -- which it must be,
 * because every path out of the block has to undo the same rsp
 * adjustment or the return address is read from the wrong place.
 */
#define IR_MAX_EXITS 64u
static uint8_t *g_exits[IR_MAX_EXITS];
static uint32_t g_nexits;

static void note_exit(uint8_t *slot)
{
    if (g_nexits < IR_MAX_EXITS) {
        g_exits[g_nexits++] = slot;
    }
}

/* lea rcx, [rsp + disp] -- the out-pointer a load writes through. */
static void lea_rcx_slot(uint16_t n)
{
    emu_jit_emit8(0x48); emu_jit_emit8(0x8D);
    emu_jit_emit8(0x8C); emu_jit_emit8(0x24);
    emu_jit_emit32(slot(n));
}

/* Address of a memory operation: operand `a` plus the displacement. */
static void emit_addr(const emu_ir_insn_t *in, int dst)
{
    ld_operand(dst, in->a);
    if (in->imm != 0u) {
        x86_mov_imm32(X86_EDX, in->imm);
        x86_alu_rr(X86_ADD, dst, X86_EDX);
    }
}

/*
 * Call the target's load or store and leave the block if it trapped.
 * System V: (cpu, addr, spec, out-or-value); esi already holds the
 * address and ecx the fourth argument.
 */
static void emit_mem_call(const void *fn, uint32_t spec)
{
    emu_jit_emit8(0x48); emu_jit_emit8(0x89);
    emu_jit_emit8(0xDF);                      /* mov rdi, rbx (the cpu) */
    x86_mov_imm32(X86_EDX, spec);
    x86_mov_imm64(X86_EAX, (uint64_t)(uintptr_t)fn);
    x86_call_rax();
    x86_alu_rr(X86_TEST, X86_EAX, X86_EAX);
    note_exit(x86_jcc32(X86_CC_NE));
}

/*
 * What this backend will emit for the floating-point class.
 *
 * The rounding mode is half the question. Arithmetic runs under the
 * MXCSR this block sets, which is round-to-nearest -- so any other mode
 * would need the control word changed around the operation and is
 * declined instead. Conversion to an integer has two encodings, a
 * truncating one that ignores MXCSR and one that obeys it, which covers
 * RTZ and RNE and nothing else.
 *
 * FMIN, FMAX and FCLASS are declined for a different reason: minss and
 * maxss return their *second* operand for a NaN where both guests return
 * the other operand, and getting that right is fifteen instructions each
 * against a helper call. The unsigned conversions are declined because
 * x86-64 without AVX-512 has no unsigned form at all and the workaround
 * is a range test and a bias.
 */
bool emu_ir_can_lower(emu_ir_op_t op, uint8_t aux)
{
    switch (op) {
    case EMU_IR_FGET: case EMU_IR_FPUT:
    case EMU_IR_FSGNJ: case EMU_IR_FCMP:
        return true;

    case EMU_IR_FADD: case EMU_IR_FSUB:
    case EMU_IR_FMUL: case EMU_IR_FDIV:
    case EMU_IR_FSQRT:
        return EMU_IR_FRM(aux) == EMU_IR_FRM_RNE;

    case EMU_IR_FCVT_FROM_I:
        return (aux & EMU_IR_F_UNSIGNED) == 0u &&
               EMU_IR_FRM(aux) == EMU_IR_FRM_RNE;

    case EMU_IR_FCVT_TO_I:
        return (aux & EMU_IR_F_UNSIGNED) == 0u &&
               (EMU_IR_FRM(aux) == EMU_IR_FRM_RTZ ||
                EMU_IR_FRM(aux) == EMU_IR_FRM_RNE);

    case EMU_IR_FMIN: case EMU_IR_FMAX: case EMU_IR_FCLASS:
    default:
        return false;
    }
}

static bool lower_one(const emu_ir_insn_t *in, const emu_ir_target_t *t)
{
    /*
     * Open the window on what the previous instruction left in T0, and
     * close the record of it: only a store below re-establishes it.
     */
    g_t0_avail = g_t0_holds;
    g_t0_holds = EMU_IR_NO_TEMP;
    g_t0_first = true;

    switch ((emu_ir_op_t)in->op) {
    case EMU_IR_NOP:
        break;

    case EMU_IR_GET: {
        const int rd = dst_reg(in->dst);

        if (t->reg_is_zero != NULL && t->reg_is_zero(in->imm)) {
            x86_mov_imm32(rd, 0u);
        } else {
            x86_ld_cpu(rd, t->reg_offset(in->imm));
        }
        st_slot(rd, in->dst);
        break;
    }

    case EMU_IR_PUT:
        if (t->reg_is_zero != NULL && t->reg_is_zero(in->imm)) {
            break;              /* writes discarded */
        }
        ld_operand(T0, in->a);
        x86_st_cpu(T0, t->reg_offset(in->imm));
        break;

    case EMU_IR_CONST: {
        const int rd = dst_reg(in->dst);

        x86_mov_imm32(rd, in->imm);
        st_slot(rd, in->dst);
        break;
    }

    case EMU_IR_MOV: {
        const int rd = dst_reg(in->dst);

        ld_operand(rd, in->a);
        st_slot(rd, in->dst);
        break;
    }

    case EMU_IR_ADD: case EMU_IR_SUB: case EMU_IR_AND:
    case EMU_IR_OR:  case EMU_IR_XOR: {
        static const uint8_t k_alu[] = {
            [EMU_IR_ADD - EMU_IR_ADD] = X86_ADD,
            [EMU_IR_SUB - EMU_IR_ADD] = X86_SUB,
            [EMU_IR_AND - EMU_IR_ADD] = X86_AND,
            [EMU_IR_OR  - EMU_IR_ADD] = X86_OR,
            [EMU_IR_XOR - EMU_IR_ADD] = X86_XOR,
        };
        const uint8_t alu = k_alu[in->op - (uint8_t)EMU_IR_ADD];
        const int pb = phys(in->b);
        const int rd = dst_reg(in->dst);

        /*
         * x86 is two-address, so the destination has to be loaded with
         * the left operand first -- which makes computing in place worth
         * one mov rather than the three instructions it saves on the
         * three-address host.
         *
         * `rd` cannot collide with `pb`: b is read here, so the
         * allocator's strictly-before expiry cannot have handed its
         * register to this instruction's destination.
         */
        ld_operand(rd, in->a);
        /*
         * The right operand is taken where it already is -- its register
         * if it has one, otherwise straight out of its frame slot rather
         * than loaded first. Either way one instruction instead of two.
         * TEST is not in this table, so the direction-bit form the slot
         * encoding relies on always applies.
         */
        if (pb >= 0) {
            x86_alu_rr(alu, rd, pb);
        } else if (in->b != EMU_IR_NO_TEMP) {
            x86_alu_slot(alu, rd, slot(in->b));
        } else {
            ld_operand(T1, in->b);
            x86_alu_rr(alu, rd, T1);
        }
        st_slot(rd, in->dst);
        break;
    }

    case EMU_IR_SHL: case EMU_IR_SHR: case EMU_IR_SAR: {
        static const uint8_t k_sh[] = {
            [EMU_IR_SHL - EMU_IR_SHL] = X86_SHL,
            [EMU_IR_SHR - EMU_IR_SHL] = X86_SHR,
            [EMU_IR_SAR - EMU_IR_SHL] = X86_SAR,
        };
        const int rd = dst_reg(in->dst);

        ld_operand(rd, in->a);
        ld_operand(T1, in->b);      /* count must be in cl */
        x86_shift_cl(rd, k_sh[in->op - (uint8_t)EMU_IR_SHL]);
        st_slot(rd, in->dst);
        break;
    }

    case EMU_IR_SHLI: case EMU_IR_SHRI: case EMU_IR_SARI: {
        static const uint8_t k_sh[] = {
            [EMU_IR_SHLI - EMU_IR_SHLI] = X86_SHL,
            [EMU_IR_SHRI - EMU_IR_SHLI] = X86_SHR,
            [EMU_IR_SARI - EMU_IR_SHLI] = X86_SAR,
        };
        const int rd = dst_reg(in->dst);

        ld_operand(rd, in->a);
        x86_shift_imm(rd, k_sh[in->op - (uint8_t)EMU_IR_SHLI], in->imm);
        st_slot(rd, in->dst);
        break;
    }

    case EMU_IR_NOT:
    case EMU_IR_NEG: {
        const int rd = dst_reg(in->dst);

        ld_operand(rd, in->a);
        x86_unary((in->op == (uint8_t)EMU_IR_NOT) ? X86_NOT : X86_NEG, rd);
        st_slot(rd, in->dst);
        break;
    }

    /*
     * The bit and byte group, which is why these are IR operations
     * rather than open-coded shift sequences: each is one host
     * instruction here, and would be four to six emitted bytes of shift
     * and mask if the IR had made the frontend spell it out.
     */
    case EMU_IR_BSWAP32: {
        const int rd = dst_reg(in->dst);

        ld_operand(rd, in->a);
        x86_bswap(rd);
        st_slot(rd, in->dst);
        break;
    }

    case EMU_IR_BSWAP16:
        /* Swap bytes within each halfword: rotate each 16-bit half by 8. */
        ld_operand(T0, in->a);
        emu_jit_emit8(0x66);
        emu_jit_emit8(0xC1); emu_jit_emit8(0xC0); emu_jit_emit8(0x08);
        emu_jit_emit8(0x0F); emu_jit_emit8(0xC8);  /* bswap eax   */
        emu_jit_emit8(0x66);
        emu_jit_emit8(0xC1); emu_jit_emit8(0xC0); emu_jit_emit8(0x08);
        emu_jit_emit8(0x0F); emu_jit_emit8(0xC8);
        st_slot(T0, in->dst);
        break;

    case EMU_IR_HSWAP: {
        const int rd = dst_reg(in->dst);

        ld_operand(rd, in->a);
        x86_shift_imm(rd, X86_ROR, 16u);
        st_slot(rd, in->dst);
        break;
    }

    case EMU_IR_CLZ:
    case EMU_IR_CTZ:
        /*
         * bsr/bsf plus a cmov, not lzcnt/tzcnt.
         *
         * lzcnt is the obvious encoding and it is a trap: it is
         * `F3 0F BD`, and on a CPU without ABM the F3 is *ignored* and
         * the instruction executes as bsr. It does not fault, it returns
         * a different answer -- the index of the set bit instead of the
         * count of leading zeros, and for a zero input it leaves the
         * destination alone where lzcnt would give 32. Caught here by
         * running it: CLZ(0x00100000) came back 20 instead of 11, and
         * CLZ(0) came back 0 instead of 32.
         *
         * bsr and bsf are baseline x86-64. Both set ZF for a zero input
         * and leave the destination undefined, so the zero case is
         * supplied by a cmov -- placed immediately after, because
         * anything else in between would clobber the flags it reads.
         *
         *   clz:  edx = -1; bsr ecx, eax; cmovz ecx, edx
         *         eax = 31; sub eax, ecx      -> 32 for a zero input
         *   ctz:  edx = 32; bsf ecx, eax; cmovz ecx, edx; eax = ecx
         */
        ld_operand(T0, in->a);
        x86_mov_imm32(T2, (in->op == (uint8_t)EMU_IR_CLZ) ? 0xFFFFFFFFu : 32u);
        emu_jit_emit8(0x0F);
        emu_jit_emit8((in->op == (uint8_t)EMU_IR_CLZ) ? 0xBDu : 0xBCu);
        emu_jit_emit8(0xC8);                       /* bsr/bsf ecx, eax  */
        emu_jit_emit8(0x0F); emu_jit_emit8(0x44);
        emu_jit_emit8(0xCA);                       /* cmovz ecx, edx    */
        if (in->op == (uint8_t)EMU_IR_CLZ) {
            x86_mov_imm32(T0, 31u);
            x86_alu_rr(X86_SUB, T0, T1);
        } else {
            x86_mov_rr(T0, T1);
        }
        st_slot(T0, in->dst);
        break;

    case EMU_IR_BEXT: case EMU_IR_BSET:
    case EMU_IR_BCLR: case EMU_IR_BINV: {
        /*
         * x86 has bt/bts/btr/btc with a register bit index, which is
         * exactly this and handles the index modulo 32 the same way the
         * guests do.
         */
        static const uint8_t k_bt[] = {
            [EMU_IR_BEXT - EMU_IR_BEXT] = 0xA3,    /* bt  */
            [EMU_IR_BSET - EMU_IR_BEXT] = 0xAB,    /* bts */
            [EMU_IR_BCLR - EMU_IR_BEXT] = 0xB3,    /* btr */
            [EMU_IR_BINV - EMU_IR_BEXT] = 0xBB,    /* btc */
        };
        ld_operand(T0, in->a);
        ld_operand(T1, in->b);
        emu_jit_emit8(0x0F);
        emu_jit_emit8(k_bt[in->op - (uint8_t)EMU_IR_BEXT]);
        emu_jit_emit8(0xC8);                       /* ..., eax, ecx */
        if (in->op == (uint8_t)EMU_IR_BEXT) {
            /* The bit went to CF; turn it back into a 0 or 1. */
            x86_setcc_eax(X86_CC_B);
        }
        st_slot(T0, in->dst);
        break;
    }

    /*
     * The source stays eax: with a REX prefix present, byte-operand
     * encodings 4..7 mean SPL/BPL/SIL/DIL rather than AH/CH/DH/BH, and
     * al is encoding 0 under any prefix. Only the destination varies.
     */
    case EMU_IR_SEXT8: case EMU_IR_SEXT16:
    case EMU_IR_ZEXT8: case EMU_IR_ZEXT16: {
        const int rd = dst_reg(in->dst);

        ld_operand(T0, in->a);
        switch ((emu_ir_op_t)in->op) {
        case EMU_IR_SEXT8:  x86_movsx8(rd, T0);  break;
        case EMU_IR_SEXT16: x86_movsx16(rd, T0); break;
        case EMU_IR_ZEXT8:  x86_movzx8(rd, T0);  break;
        default:            x86_movzx16(rd, T0); break;
        }
        st_slot(rd, in->dst);
        break;
    }

    /* ---- floating point ----------------------------------------- */
    case EMU_IR_FGET: {
        if (t->freg_offset == NULL) { return false; }
        const int rd = dst_reg(in->dst);

        x86_ld_cpu(rd, t->freg_offset(in->imm));
        st_slot(rd, in->dst);
        break;
    }

    case EMU_IR_FPUT:
        if (t->freg_offset == NULL) { return false; }
        ld_operand(T0, in->a);
        x86_st_cpu(T0, t->freg_offset(in->imm));
        break;

    case EMU_IR_FADD: case EMU_IR_FSUB:
    case EMU_IR_FMUL: case EMU_IR_FDIV: {
        static const uint8_t k_ss[] = {
            [EMU_IR_FADD - EMU_IR_FADD] = X86_ADDSS,
            [EMU_IR_FSUB - EMU_IR_FADD] = X86_SUBSS,
            [EMU_IR_FMUL - EMU_IR_FADD] = X86_MULSS,
            [EMU_IR_FDIV - EMU_IR_FADD] = X86_DIVSS,
        };
        if (!emu_ir_can_lower((emu_ir_op_t)in->op, in->aux)) { return false; }
        ld_operand(T0, in->a);
        ld_operand(T1, in->b);
        x86_movd_to_xmm(X86_XMM0, T0);
        x86_movd_to_xmm(X86_XMM1, T1);
        x86_ss_op(k_ss[in->op - (uint8_t)EMU_IR_FADD], X86_XMM0, X86_XMM1);
        x86_movd_from_xmm(T0, X86_XMM0);
        st_slot(T0, in->dst);
        break;
    }

    case EMU_IR_FSQRT:
        if (!emu_ir_can_lower((emu_ir_op_t)in->op, in->aux)) { return false; }
        ld_operand(T0, in->a);
        x86_movd_to_xmm(X86_XMM0, T0);
        x86_ss_op(X86_SQRTSS, X86_XMM0, X86_XMM0);
        x86_movd_from_xmm(T0, X86_XMM0);
        st_slot(T0, in->dst);
        break;

    case EMU_IR_FSGNJ:
        /*
         * Bit manipulation, so it never reaches the FP unit -- which is
         * also why it is exact for a NaN operand, where anything routed
         * through xmm would risk quietening one.
         */
        ld_operand(T0, in->a);
        ld_operand(T1, in->b);
        if (in->aux == EMU_IR_FSGNJ_X) {
            x86_alu_rr(X86_XOR, T1, T0);
        }
        x86_mov_imm32(T2, 0x80000000u);
        x86_alu_rr(X86_AND, T1, T2);
        if (in->aux == EMU_IR_FSGNJ_N) {
            x86_alu_rr(X86_XOR, T1, T2);
        }
        x86_mov_imm32(T2, 0x7FFFFFFFu);
        x86_alu_rr(X86_AND, T0, T2);
        x86_alu_rr(X86_OR, T0, T1);
        st_slot(T0, in->dst);
        break;

    case EMU_IR_FCMP:
        /*
         * Unordered is false for all three, which the operand order
         * does most of the work for: `ucomiss b, a` sets CF and ZF for
         * unordered, and `seta`/`setae` both want CF clear.
         *
         * Equality cannot be had that way -- unordered sets ZF, so
         * `sete` alone is true for a NaN -- so it takes the parity flag
         * as well. That is the case a test using ordinary numbers never
         * reaches.
         */
        ld_operand(T0, in->a);
        ld_operand(T1, in->b);
        x86_movd_to_xmm(X86_XMM0, T0);
        x86_movd_to_xmm(X86_XMM1, T1);
        if (in->aux == (uint8_t)EMU_IR_C_EQ) {
            x86_ucomiss(X86_XMM0, X86_XMM1);
            emu_jit_emit8(0x0F); emu_jit_emit8(0x9B);
            emu_jit_emit8(0xC0);                       /* setnp al  */
            emu_jit_emit8(0x0F); emu_jit_emit8(0x94);
            emu_jit_emit8(0xC1);                       /* sete  cl  */
            emu_jit_emit8(0x20); emu_jit_emit8(0xC8);  /* and al,cl */
            x86_movzx8(T0, T0);
        } else {
            x86_ucomiss(X86_XMM1, X86_XMM0);
            x86_setcc_eax((in->aux == (uint8_t)EMU_IR_C_LT) ? X86_CC_A
                                                            : X86_CC_AE);
        }
        st_slot(T0, in->dst);
        break;

    case EMU_IR_FCVT_FROM_I:
        if (!emu_ir_can_lower((emu_ir_op_t)in->op, in->aux)) { return false; }
        ld_operand(T0, in->a);
        x86_cvt_from_i(X86_XMM0, T0);
        x86_movd_from_xmm(T0, X86_XMM0);
        st_slot(T0, in->dst);
        break;

    case EMU_IR_FCVT_TO_I: {
        if (!emu_ir_can_lower((emu_ir_op_t)in->op, in->aux)) { return false; }
        /*
         * x86 reports every input it cannot represent -- NaN, either
         * infinity, out of range -- as the single value 0x80000000, the
         * "integer indefinite". The guests distinguish three outcomes
         * there: a NaN gives the *maximum*, a large positive gives the
         * maximum, and a large negative gives the minimum, which is the
         * one case x86 already has right.
         *
         * So the fixup is only on that one value, and it is the input
         * that decides. Testing the result and then re-examining the
         * operand is the only way round: nothing in the flags survives
         * the conversion to say which case it was.
         */
        ld_operand(T0, in->a);
        x86_movd_to_xmm(X86_XMM0, T0);
        x86_mov_rr(T2, T0);                    /* keep the bit pattern */
        x86_cvt_to_i(T0, X86_XMM0,
                     EMU_IR_FRM(in->aux) == EMU_IR_FRM_RTZ);
        x86_mov_imm32(T1, 0x80000000u);
        x86_alu_rr(X86_CMP, T0, T1);
        uint8_t *const ok = x86_jcc32(X86_CC_NE);

        /* Indefinite. A NaN, or a positive out of range, gives INT_MAX. */
        x86_ucomiss(X86_XMM0, X86_XMM0);
        uint8_t *const nan = x86_jcc32(0x8Au);  /* jp */
        x86_alu_rr(X86_TEST, T2, T1);          /* sign bit of the input */
        uint8_t *const neg = x86_jcc32(X86_CC_NE);
        x86_patch_rel32(nan, emu_jit_here());
        x86_mov_imm32(T0, 0x7FFFFFFFu);
        x86_patch_rel32(neg, emu_jit_here());
        x86_patch_rel32(ok, emu_jit_here());
        st_slot(T0, in->dst);
        break;
    }

    case EMU_IR_SETF:
        emit_setf(in, t);
        break;

    case EMU_IR_GETCOND: {
        static const uint8_t k_cc[] = {
            [EMU_IR_C_EQ]  = X86_CC_E,  [EMU_IR_C_NE]  = X86_CC_NE,
            [EMU_IR_C_LT]  = X86_CC_L,  [EMU_IR_C_GE]  = X86_CC_GE,
            [EMU_IR_C_LTU] = X86_CC_B,  [EMU_IR_C_GEU] = X86_CC_AE,
            [EMU_IR_C_LE]  = X86_CC_LE, [EMU_IR_C_GT]  = X86_CC_G,
            [EMU_IR_C_LEU] = X86_CC_BE, [EMU_IR_C_GTU] = X86_CC_A,
        };
        if (in->aux == (uint8_t)EMU_IR_C_ALWAYS) {
            x86_mov_imm32(T0, 1u);
        } else {
            x86_setcc_eax(k_cc[in->aux]);
        }
        st_slot(T0, in->dst);
        break;
    }

    case EMU_IR_SETCC: {
        static const uint8_t k_cc[] = {
            [EMU_IR_C_EQ]  = X86_CC_E,  [EMU_IR_C_NE]  = X86_CC_NE,
            [EMU_IR_C_LT]  = X86_CC_L,  [EMU_IR_C_GE]  = X86_CC_GE,
            [EMU_IR_C_LTU] = X86_CC_B,  [EMU_IR_C_GEU] = X86_CC_AE,
            [EMU_IR_C_LE]  = X86_CC_LE, [EMU_IR_C_GT]  = X86_CC_G,
            [EMU_IR_C_LEU] = X86_CC_BE, [EMU_IR_C_GTU] = X86_CC_A,
        };
        if (in->aux == (uint8_t)EMU_IR_C_ALWAYS) {
            x86_mov_imm32(T0, 1u);
        } else {
            ld_operand(T0, in->a);
            ld_operand(T1, in->b);
            x86_alu_rr(X86_CMP, T0, T1);
            x86_setcc_eax(k_cc[in->aux]);
        }
        st_slot(T0, in->dst);
        break;
    }

    case EMU_IR_HELPER:
    case EMU_IR_HELPER_TRAP: {
        /*
         * System V: the cpu pointer in rdi and two arguments after it.
         * rbx holds the cpu pointer and is callee-saved, so it survives
         * the call; every live value is in the frame, so nothing else
         * needs preserving.
         */
        if (t->helpers == NULL || in->imm >= t->helper_count) {
            return false;
        }
        ld_operand(X86_EDX, in->b);           /* arg2 */
        ld_operand(X86_ESI, in->a);           /* arg1 */
        emu_jit_emit8(0x48); emu_jit_emit8(0x89);
        emu_jit_emit8(0xDF);                  /* mov rdi, rbx */
        x86_mov_imm64(X86_EAX, (uint64_t)(uintptr_t)t->helpers[in->imm]);
        x86_call_rax();

        if (in->op == (uint8_t)EMU_IR_HELPER_TRAP) {
            /*
             * Non-zero means the helper entered a trap: pc is already in
             * the handler, so the rest of the block must not run.
             */
            x86_alu_rr(X86_TEST, X86_EAX, X86_EAX);
            note_exit(x86_jcc32(X86_CC_NE));
        }
        if (in->dst != EMU_IR_NO_TEMP) {
            st_slot(X86_EAX, in->dst);
        }
        break;
    }

    case EMU_IR_EXIT_IF: {
        static const uint8_t k_cc[] = {
            [EMU_IR_C_EQ]  = X86_CC_E,  [EMU_IR_C_NE]  = X86_CC_NE,
            [EMU_IR_C_LT]  = X86_CC_L,  [EMU_IR_C_GE]  = X86_CC_GE,
            [EMU_IR_C_LTU] = X86_CC_B,  [EMU_IR_C_GEU] = X86_CC_AE,
            [EMU_IR_C_LE]  = X86_CC_LE, [EMU_IR_C_GT]  = X86_CC_G,
            [EMU_IR_C_LEU] = X86_CC_BE, [EMU_IR_C_GTU] = X86_CC_A,
        };
        /*
         * `a` and `b` are compared directly rather than through the
         * guest's flags: a branch on a flagless guest has no flags to
         * consult, and one on a flag machine can use EMU_IR_GETCOND into
         * a temp first.
         */
        ld_operand(T0, in->a);
        ld_operand(T1, in->b);
        x86_alu_rr(X86_CMP, T0, T1);
        uint8_t *const not_taken = x86_jcc32((uint8_t)(k_cc[in->aux] ^ 1u));
        x86_mov_imm32(T0, in->imm);
        x86_st_cpu(T0, t->pc_offset);
        note_exit(x86_jmp32());
        x86_patch_rel32(not_taken, emu_jit_here());
        break;
    }

    case EMU_IR_RETIRE:
        x86_count_one();
        break;

    case EMU_IR_SETPC:
        if (in->a != EMU_IR_NO_TEMP) {
            ld_operand(T0, in->a);
        } else {
            x86_mov_imm32(T0, in->imm);
        }
        x86_st_cpu(T0, t->pc_offset);
        break;

    case EMU_IR_EXIT:
        /*
         * Writes pc and *leaves*. Sharing a case with SETPC above -- on
         * the reasoning that both just write pc -- meant a JAL wrote its
         * target and then fell into the pc-after-this-instruction store
         * that follows every instruction, so every jump went to pc + 4.
         * The guest ran, retired instructions and printed nothing.
         */
        if (in->a != EMU_IR_NO_TEMP) {
            ld_operand(T0, in->a);
        } else {
            x86_mov_imm32(T0, in->imm);
        }
        x86_st_cpu(T0, t->pc_offset);
        note_exit(x86_jmp32());
        break;

    /*
     * Not lowered here. Each needs either a helper call with the host
     * calling convention set up, or a branch whose target is only known
     * once the rest of the block is emitted -- both real work, and both
     * better added with the frontend that first needs them than guessed
     * at now. Returning false discards the block, which the framework
     * already treats exactly as it treats a declined translation.
     */
    /*
     * The memory bit ops need the guest's address translated, which on
     * this emulator means a call to the frontend's load/store helpers --
     * a guest address is not a host address. x86 could otherwise do each
     * of these in a single bts/btr/btc with a memory operand.
     */
    case EMU_IR_LOAD: {
        if (t->load == NULL) {
            return false;
        }
        const int pd = phys(in->dst);

        emit_addr(in, X86_ESI);
        /*
         * The helper writes through a pointer, and a register has no
         * address -- so an allocated destination lands in the scratch
         * slot and is fetched from there. That costs a load the frame
         * home would not have paid, which is the one place allocation
         * can lose: a value read once, three bytes saved at the use
         * against eight spent here. It is left to the allocator's own
         * rule rather than special-cased, because a load result read
         * twice already wins.
         */
        lea_rcx_slot((pd >= 0) ? SCRATCH_VAL : in->dst);
        emit_mem_call((const void *)t->load, in->aux);
        if (pd >= 0) {
            ld_slot(pd, SCRATCH_VAL);
        }
        break;
    }

    case EMU_IR_STORE:
        if (t->store == NULL) {
            return false;
        }
        emit_addr(in, X86_ESI);
        ld_operand(X86_ECX, in->b);      /* the value */
        emit_mem_call((const void *)t->store, in->aux);
        break;

    /*
     * The memory bit ops: one IR instruction, three host steps. Read the
     * byte, report the bit *as it was* in Z, write back unless this is
     * the test-only form.
     *
     * The address is spilled because the load call clobbers every
     * scratch register. Z alone moves -- CY, OV and S keep whatever the
     * previous instruction left, which is unlike almost everything else
     * a guest does -- so the flag word is read-modify-written rather
     * than rebuilt.
     */
    case EMU_IR_BITOP_SET:
    case EMU_IR_BITOP_CLR:
    case EMU_IR_BITOP_INV:
    case EMU_IR_BITOP_TST: {
        if (t->load == NULL || t->store == NULL || t->flag_bit[0] == 0u) {
            return false;
        }
        const uint32_t spec = EMU_IR_MEM_AUX(1u, 0u);

        emit_addr(in, X86_ESI);
        st_slot(X86_ESI, SCRATCH_ADDR);
        lea_rcx_slot(SCRATCH_VAL);
        emit_mem_call((const void *)t->load, spec);

        /* edx = 1 << (bit & 7) */
        ld_operand(T1, in->b);
        x86_and_imm8(T1, 7);
        x86_mov_imm32(T2, 1u);
        emu_jit_emit8(0xD3); emu_jit_emit8(0xE2);      /* shl edx, cl */

        ld_slot(T0, SCRATCH_VAL);
        x86_alu_rr(X86_TEST, T0, T2);
        /*
         * setz, not setnz: Z reports the bit having been *clear*.
         * Computing it from the result instead would make SET1 always
         * clear Z and CLR1 always set it, with the right byte in memory
         * either way.
         */
        emu_jit_emit8(0x0F); emu_jit_emit8(0x94);
        emu_jit_emit8(0xC1);                           /* setz cl */
        x86_movzx8(T1, T1);
        x86_shift_imm(T1, X86_SHL, (uint32_t)__builtin_ctz(t->flag_bit[0]));

        x86_ld_cpu(X86_ESI, t->flags_offset);
        x86_mov_imm32(X86_EDI, ~t->flag_bit[0]);
        x86_alu_rr(X86_AND, X86_ESI, X86_EDI);
        x86_alu_rr(X86_OR, X86_ESI, T1);
        x86_st_cpu(X86_ESI, t->flags_offset);

        if (in->op == (uint8_t)EMU_IR_BITOP_TST) {
            break;                        /* no write-back, no store fault */
        }

        ld_slot(T0, SCRATCH_VAL);
        if (in->op == (uint8_t)EMU_IR_BITOP_SET) {
            x86_alu_rr(X86_OR, T0, T2);
        } else if (in->op == (uint8_t)EMU_IR_BITOP_INV) {
            x86_alu_rr(X86_XOR, T0, T2);
        } else {
            emu_jit_emit8(0xF7); emu_jit_emit8(0xD2);  /* not edx */
            x86_alu_rr(X86_AND, T0, T2);
        }
        x86_mov_rr(X86_ECX, T0);
        ld_slot(X86_ESI, SCRATCH_ADDR);
        emit_mem_call((const void *)t->store, spec);
        break;
    }

    case EMU_IR_SELECT: {
        /*
         * Reads the guest's flags, so they have to be brought back into
         * the host's. Only Z is reconstructed: a guest without flags has
         * none to consult and should use EMU_IR_SETCC instead, which is
         * what the flagless frontend does.
         */
        if (in->aux == (uint8_t)EMU_IR_C_ALWAYS) {
            ld_operand(T0, in->a);
            st_slot(T0, in->dst);
            break;
        }
        if (t->flag_bit[0] == 0u ||
            (in->aux != (uint8_t)EMU_IR_C_EQ &&
             in->aux != (uint8_t)EMU_IR_C_NE)) {
            return false;
        }
        ld_operand(T0, in->b);            /* the false value */
        ld_operand(T1, in->a);            /* the true value  */
        x86_ld_cpu(T2, t->flags_offset);
        x86_mov_imm32(X86_ESI, t->flag_bit[0]);
        x86_alu_rr(X86_TEST, T2, X86_ESI);
        emu_jit_emit8(0x0F);
        emu_jit_emit8(in->aux == (uint8_t)EMU_IR_C_EQ ? 0x45u : 0x44u);
        emu_jit_emit8(0xC1);              /* cmovnz/cmovz eax, ecx */
        st_slot(T0, in->dst);
        break;
    }

    /*
     * popcnt is SSE4.2 and, unlike lzcnt, has no benign degradation:
     * without the feature `F3 0F B8` is a different encoding, not a
     * slower spelling of the same thing. Declining is correct until
     * something needs it enough to justify a CPUID check or the SWAR
     * sequence.
     */
    case EMU_IR_MUL:
    case EMU_IR_MULHS:
    case EMU_IR_MULHU: {
        /*
         * The one-operand form: it multiplies eax by the operand and
         * writes the full 64-bit product to edx:eax, which is what the
         * high-half opcodes need. The two-operand imul gives only the
         * low half.
         *
         * Signedness matters only for the high half -- the low 32 bits
         * of a product are the same either way -- so MUL uses the signed
         * form and takes eax.
         */
        ld_operand(T0, in->a);
        ld_operand(T1, in->b);
        emu_jit_emit8(0xF7);
        emu_jit_emit8((uint8_t)((in->op == (uint8_t)EMU_IR_MULHU)
                                ? 0xE1u    /* mul  ecx */
                                : 0xE9u)); /* imul ecx */
        if (in->op != (uint8_t)EMU_IR_MUL) {
            x86_mov_rr(T0, T2);            /* the high half */
        }
        st_slot(T0, in->dst);
        break;
    }

    case EMU_IR_FMIN: case EMU_IR_FMAX: case EMU_IR_FCLASS:
    case EMU_IR_POPCNT:
    case EMU_IR_ROTL: case EMU_IR_ROTLI:
    case EMU_IR_ADDI: case EMU_IR_ANDI:
    case EMU_IR_ORI:  case EMU_IR_XORI:
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

    /*
     * Reserve the frame. System V wants rsp 16-byte aligned at a call,
     * and x86_prologue has already brought it there, so the reservation
     * has to keep it aligned -- getting this wrong does not fault in the
     * emitted code, it faults inside whatever libc routine a helper
     * eventually reaches that uses an aligned SSE store.
     */
    g_ntemps = (uint16_t)b->next_temp;
    g_t0_holds = EMU_IR_NO_TEMP;
    g_dead_store = EMU_IR_NO_TEMP;

    g_nsaved = emu_ir_regalloc(b, X86_ALLOC_REGS, g_reg);

    /*
     * Does this block touch the FP unit? Asked once, because everything
     * MXCSR costs -- saving it, clearing the sticky bits, reading them
     * back and handing them to the frontend -- is paid per block and
     * must not be paid by the blocks that never look at a float.
     */
    g_has_fp = false;
    for (uint32_t i = 0; i < b->count && !g_has_fp; i++) {
        if (!b->insn[i].dead &&
            b->insn[i].op >= (uint8_t)EMU_IR_FADD &&
            b->insn[i].op <= (uint8_t)EMU_IR_FCLASS) {
            g_has_fp = true;
        }
    }
    if (g_has_fp && (t->fp_flags == NULL || t->freg_offset == NULL)) {
        return false;
    }
    if (g_has_fp) {
        fe_map_init();
    }

    /*
     * The frame, with the pad an odd number of saved registers needs.
     *
     * x86_prologue pushes them after its own alignment adjustment, so
     * each one moves rsp by 8 and an odd count leaves it 8 out of step
     * with what System V wants at a call. Getting this wrong does not
     * fault in the emitted code -- it faults inside whatever libc
     * routine a helper eventually reaches that uses an aligned SSE
     * store, which is a long way from the cause.
     */
    uint32_t frame = (((b->next_temp + 4u) * 4u) + 15u) & ~15u;

    if ((g_nsaved & 1u) != 0u) {
        frame += 8u;
    }

    g_nexits = 0u;

    /*
     * The block prologue belongs here rather than in the caller, so that
     * jit.c names no host at all. It was the last thing in the pipeline
     * that did.
     */
    x86_prologue(g_nsaved);

    if (frame != 0u) {
        emu_jit_emit8(0x48); emu_jit_emit8(0x81);
        emu_jit_emit8(0xEC); emu_jit_emit32(frame);       /* sub rsp, imm32 */
    }

    if (g_has_fp) {
        /*
         * Take ownership of MXCSR for the length of the block: keep the
         * caller's, and run under one with the sticky flags clear and
         * rounding fixed at nearest.
         *
         * Clearing matters and is not obvious. The guest's flags are
         * sticky too, so re-reporting one is idempotent -- but a guest
         * that *clears* its flags and then runs an operation raising
         * none would still see the host's leftovers, and there is no
         * other point at which they get cleared.
         *
         * Fixing the rounding rather than inheriting it is what makes
         * emu_ir_can_lower's answer true: it says this backend emits
         * arithmetic only for round-to-nearest, and that is only a
         * promise if the block sets it rather than hoping.
         */
        x86_stmxcsr(slot(SCRATCH_MXOLD));
        ld_slot(T0, SCRATCH_MXOLD);
        x86_mov_imm32(T1, ~0x603Fu);        /* sticky bits and RC */
        x86_alu_rr(X86_AND, T0, T1);
        x86_st_rsp(T0, slot(SCRATCH_MXCUR));
        x86_ldmxcsr(slot(SCRATCH_MXCUR));
    }

    for (uint32_t i = 0; i < b->count; i++) {
        if (b->insn[i].dead) {
            continue;
        }
        /*
         * Decide before lowering whether this instruction's store can be
         * skipped: its value must have exactly one reader, and that
         * reader must be the next live instruction taking it in T0.
         */
        g_dead_store = EMU_IR_NO_TEMP;
        if (b->insn[i].dst != EMU_IR_NO_TEMP && b->insn[i].uses == 1u) {
            uint32_t j = i + 1u;

            while (j < b->count && b->insn[j].dead) {
                j++;
            }
            if (j < b->count && b->insn[j].a == b->insn[i].dst &&
                reads_a_in_t0(b->insn[j].op)) {
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
     * Every early exit lands here, *before* the frame teardown, so all
     * paths undo the same rsp adjustment. Jumping straight to the
     * caller's epilogue instead would leave rsp low and return to
     * whatever the frame happened to contain.
     */
    for (uint32_t i = 0; i < g_nexits; i++) {
        x86_patch_rel32(g_exits[i], emu_jit_here());
    }

    if (g_has_fp) {
        /*
         * One accumulation of the block's flags, on the single path
         * every exit takes -- which is why the early exits are patched
         * to land above this rather than at the return.
         *
         * ebp holds the retired count and is callee-saved, so it
         * survives the call; eax is loaded from it afterwards by
         * x86_epilogue.
         */
        x86_stmxcsr(slot(SCRATCH_MXCUR));
        ld_slot(T0, SCRATCH_MXCUR);
        x86_and_imm8(T0, 0x3F);
        x86_mov_imm64(X86_ECX, (uint64_t)(uintptr_t)g_fe_map);
        x86_movzx8_idx(X86_ESI, X86_ECX, T0);
        emu_jit_emit8(0x48); emu_jit_emit8(0x89);
        emu_jit_emit8(0xDF);                       /* mov rdi, rbx */
        x86_mov_imm64(X86_EAX, (uint64_t)(uintptr_t)t->fp_flags);
        x86_call_rax();
        x86_ldmxcsr(slot(SCRATCH_MXOLD));
    }

    if (frame != 0u) {
        emu_jit_emit8(0x48); emu_jit_emit8(0x81);
        emu_jit_emit8(0xC4); emu_jit_emit32(frame);       /* add rsp, imm32 */
    }
    x86_epilogue(g_nsaved);
    return !emu_jit_overflowed();
}

#endif /* EMU_JIT_X86_64 */
