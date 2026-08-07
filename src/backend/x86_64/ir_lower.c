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
 * Temporaries live in a stack frame rather than in host registers.
 *
 * That is a deliberate first step and it is not free -- it is a store and
 * a reload per value where the hand-written translators keep operands in
 * registers -- but it is the ordering this project's own measurements
 * argue for. A register allocator is the single largest piece of work in
 * a backend and the easiest to get subtly wrong, and until the IR path is
 * running real guests there is nothing to measure it against. The frame
 * is also what makes the lowering obviously correct: every value has
 * exactly one home, so there is no state to get wrong between
 * instructions.
 *
 * What that means in practice: do not compare this against the direct
 * translators on speed yet. The IR pays for itself first in what the
 * optimiser removes -- dead flags on a flag machine, the guest-register
 * round trip -- and a register allocator is the next change, not a
 * missing part of this one.
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

#define SCRATCH_ADDR ((uint16_t)(g_ntemps))
#define SCRATCH_VAL  ((uint16_t)(g_ntemps + 1u))

/* mov reg, [rsp + disp] and its inverse, with the SIB byte rsp needs. */
static void ld_slot(int reg, uint16_t n)
{
    emu_jit_emit8(0x8B);
    emu_jit_emit8((uint8_t)(0x84u | ((unsigned)reg << 3)));  /* mod=10 rm=100 */
    emu_jit_emit8(0x24);                                     /* SIB: [rsp]   */
    emu_jit_emit32(slot(n));
}

static void st_slot(int reg, uint16_t n)
{
    emu_jit_emit8(0x89);
    emu_jit_emit8((uint8_t)(0x84u | ((unsigned)reg << 3)));
    emu_jit_emit8(0x24);
    emu_jit_emit32(slot(n));
}

/* Load an operand, tolerating EMU_IR_NO_TEMP so callers need not check. */
static void ld_operand(int reg, uint16_t n)
{
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

static bool lower_one(const emu_ir_insn_t *in, const emu_ir_target_t *t)
{
    switch ((emu_ir_op_t)in->op) {
    case EMU_IR_NOP:
        break;

    case EMU_IR_GET:
        if (t->reg_is_zero != NULL && t->reg_is_zero(in->imm)) {
            x86_mov_imm32(T0, 0u);
        } else {
            x86_ld_cpu(T0, t->reg_offset(in->imm));
        }
        st_slot(T0, in->dst);
        break;

    case EMU_IR_PUT:
        if (t->reg_is_zero != NULL && t->reg_is_zero(in->imm)) {
            break;              /* writes discarded */
        }
        ld_operand(T0, in->a);
        x86_st_cpu(T0, t->reg_offset(in->imm));
        break;

    case EMU_IR_CONST:
        x86_mov_imm32(T0, in->imm);
        st_slot(T0, in->dst);
        break;

    case EMU_IR_MOV:
        ld_operand(T0, in->a);
        st_slot(T0, in->dst);
        break;

    case EMU_IR_ADD: case EMU_IR_SUB: case EMU_IR_AND:
    case EMU_IR_OR:  case EMU_IR_XOR: {
        static const uint8_t k_alu[] = {
            [EMU_IR_ADD - EMU_IR_ADD] = X86_ADD,
            [EMU_IR_SUB - EMU_IR_ADD] = X86_SUB,
            [EMU_IR_AND - EMU_IR_ADD] = X86_AND,
            [EMU_IR_OR  - EMU_IR_ADD] = X86_OR,
            [EMU_IR_XOR - EMU_IR_ADD] = X86_XOR,
        };
        ld_operand(T0, in->a);
        ld_operand(T1, in->b);
        x86_alu_rr(k_alu[in->op - (uint8_t)EMU_IR_ADD], T0, T1);
        st_slot(T0, in->dst);
        break;
    }

    case EMU_IR_SHL: case EMU_IR_SHR: case EMU_IR_SAR: {
        static const uint8_t k_sh[] = {
            [EMU_IR_SHL - EMU_IR_SHL] = X86_SHL,
            [EMU_IR_SHR - EMU_IR_SHL] = X86_SHR,
            [EMU_IR_SAR - EMU_IR_SHL] = X86_SAR,
        };
        ld_operand(T0, in->a);
        ld_operand(T1, in->b);      /* count must be in cl */
        x86_shift_cl(T0, k_sh[in->op - (uint8_t)EMU_IR_SHL]);
        st_slot(T0, in->dst);
        break;
    }

    case EMU_IR_SHLI: case EMU_IR_SHRI: case EMU_IR_SARI: {
        static const uint8_t k_sh[] = {
            [EMU_IR_SHLI - EMU_IR_SHLI] = X86_SHL,
            [EMU_IR_SHRI - EMU_IR_SHLI] = X86_SHR,
            [EMU_IR_SARI - EMU_IR_SHLI] = X86_SAR,
        };
        ld_operand(T0, in->a);
        x86_shift_imm(T0, k_sh[in->op - (uint8_t)EMU_IR_SHLI], in->imm);
        st_slot(T0, in->dst);
        break;
    }

    case EMU_IR_NOT:
        ld_operand(T0, in->a);
        emu_jit_emit8(0xF7);
        emu_jit_emit8(0xD0);                       /* not eax */
        st_slot(T0, in->dst);
        break;

    case EMU_IR_NEG:
        ld_operand(T0, in->a);
        emu_jit_emit8(0xF7);
        emu_jit_emit8(0xD8);                       /* neg eax */
        st_slot(T0, in->dst);
        break;

    /*
     * The bit and byte group, which is why these are IR operations
     * rather than open-coded shift sequences: each is one host
     * instruction here, and would be four to six emitted bytes of shift
     * and mask if the IR had made the frontend spell it out.
     */
    case EMU_IR_BSWAP32:
        ld_operand(T0, in->a);
        emu_jit_emit8(0x0F);
        emu_jit_emit8(0xC8);                       /* bswap eax */
        st_slot(T0, in->dst);
        break;

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

    case EMU_IR_HSWAP:
        ld_operand(T0, in->a);
        emu_jit_emit8(0xC1); emu_jit_emit8(0xC8);
        emu_jit_emit8(0x10);                       /* ror eax, 16 */
        st_slot(T0, in->dst);
        break;

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

    case EMU_IR_SEXT8:  ld_operand(T0, in->a); x86_movsx8(T0, T0);
                        st_slot(T0, in->dst); break;
    case EMU_IR_SEXT16: ld_operand(T0, in->a); x86_movsx16(T0, T0);
                        st_slot(T0, in->dst); break;
    case EMU_IR_ZEXT8:  ld_operand(T0, in->a); x86_movzx8(T0, T0);
                        st_slot(T0, in->dst); break;
    case EMU_IR_ZEXT16: ld_operand(T0, in->a); x86_movzx16(T0, T0);
                        st_slot(T0, in->dst); break;

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
    case EMU_IR_LOAD:
        if (t->load == NULL) {
            return false;
        }
        emit_addr(in, X86_ESI);
        lea_rcx_slot(in->dst);           /* the result lands in its slot */
        emit_mem_call((const void *)t->load, in->aux);
        break;

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
    case EMU_IR_POPCNT:
    case EMU_IR_MUL: case EMU_IR_MULHS: case EMU_IR_MULHU:
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
    const uint32_t frame = (((b->next_temp + 2u) * 4u) + 15u) & ~15u;

    g_nexits = 0u;

    if (frame != 0u) {
        emu_jit_emit8(0x48); emu_jit_emit8(0x81);
        emu_jit_emit8(0xEC); emu_jit_emit32(frame);       /* sub rsp, imm32 */
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
     * Every early exit lands here, *before* the frame teardown, so all
     * paths undo the same rsp adjustment. Jumping straight to the
     * caller's epilogue instead would leave rsp low and return to
     * whatever the frame happened to contain.
     */
    for (uint32_t i = 0; i < g_nexits; i++) {
        x86_patch_rel32(g_exits[i], emu_jit_here());
    }

    if (frame != 0u) {
        emu_jit_emit8(0x48); emu_jit_emit8(0x81);
        emu_jit_emit8(0xC4); emu_jit_emit32(frame);       /* add rsp, imm32 */
    }
    return !emu_jit_overflowed();
}

#endif /* EMU_JIT_X86_64 */
