/* SPDX-License-Identifier: Apache-2.0 */
/*
 * emu_ir.c - Building and optimising the IR. See emu_ir.h.
 *
 * Every pass here is a backward or forward linear walk over one block.
 * That is not a placeholder for something cleverer: a block has one
 * entry, no joins and averages about four guest instructions in this
 * project's guests, so anything with a worklist would spend more time
 * setting up than walking. Translation is already 48-54% of host cycles
 * at the code-cache size a microcontroller gets, and a pass that costs
 * more than the code it saves is a net loss however good the output.
 */

#include "emu/emu_ir.h"
#include "emu/emu_jit.h" /* EMU_HAVE_JIT */

#include <string.h>

/* ------------------------------------------------------------------ */
/* Building                                                            */
/* ------------------------------------------------------------------ */

void emu_ir_reset(emu_ir_block_t *b)
{
    b->count = 0u;
    b->next_temp = 0u;
    b->guest_insns = 0u;
    b->overflow = false;
    /* A frontend that says nothing gets the helpers, which always work. */
    b->has_fast = false;
}

uint16_t emu_ir_temp(emu_ir_block_t *b)
{
    if (b->next_temp >= EMU_IR_MAX_TEMPS) {
        b->overflow = true;
        return EMU_IR_NO_TEMP;
    }
    return (uint16_t)b->next_temp++;
}

/* Does this op produce a value? */
static bool op_writes(emu_ir_op_t op)
{
    switch (op) {
    case EMU_IR_PUT:
    case EMU_IR_FPUT:
    case EMU_IR_STORE:
    case EMU_IR_SETF:
    case EMU_IR_RETIRE:
    case EMU_IR_SETPC:
    case EMU_IR_EXIT:
    case EMU_IR_EXIT_IF:
    case EMU_IR_NOP:
    /* The memory bit ops define a flag and change memory, not a temp. */
    case EMU_IR_BITOP_SET:
    case EMU_IR_BITOP_CLR:
    case EMU_IR_BITOP_INV:
    case EMU_IR_BITOP_TST:
        return false;
    default:
        return true;
    }
}

uint16_t emu_ir_emit(emu_ir_block_t *b, emu_ir_op_t op, uint8_t aux, uint16_t a,
                     uint16_t bb, uint32_t imm, uint8_t defs)
{
    if (b->count >= EMU_IR_MAX_INSNS) {
        b->overflow = true;
        return EMU_IR_NO_TEMP;
    }

    emu_ir_insn_t *const in = &b->insn[b->count++];
    in->op = (uint8_t)op;
    in->aux = aux;
    in->a = a;
    in->b = bb;
    in->c = EMU_IR_NO_TEMP;
    in->imm = imm;
    in->defs = defs;
    /*
     * Live until a pass proves otherwise. The conservative direction
     * matters: an uninitialised `live` that happened to be zero would
     * delete a flag definition something depends on, and the failure is
     * a wrong branch several instructions later.
     */
    in->live = defs;
    in->uses = 0u;
    in->dead = false;
    in->dst = op_writes(op) ? emu_ir_temp(b) : EMU_IR_NO_TEMP;
    return in->dst;
}

/*
 * The three-operand form. Everything emu_ir_emit does, plus the operand
 * that only EMU_IR_FMA has.
 */
uint16_t emu_ir_emit3(emu_ir_block_t *b, emu_ir_op_t op, uint8_t aux,
                      uint16_t a, uint16_t bb, uint16_t c)
{
    const uint16_t dst = emu_ir_emit(b, op, aux, a, bb, 0u, 0u);

    if (dst != EMU_IR_NO_TEMP && b->count > 0u) {
        b->insn[b->count - 1u].c = c;
    }
    return dst;
}

uint16_t emu_ir_get(emu_ir_block_t *b, uint32_t guest_reg)
{
    return emu_ir_emit(b, EMU_IR_GET, 0u, EMU_IR_NO_TEMP, EMU_IR_NO_TEMP,
                       guest_reg, 0u);
}

void emu_ir_put(emu_ir_block_t *b, uint32_t guest_reg, uint16_t val)
{
    (void)emu_ir_emit(b, EMU_IR_PUT, 0u, val, EMU_IR_NO_TEMP, guest_reg, 0u);
}

uint16_t emu_ir_const(emu_ir_block_t *b, uint32_t v)
{
    return emu_ir_emit(b, EMU_IR_CONST, 0u, EMU_IR_NO_TEMP, EMU_IR_NO_TEMP, v,
                       0u);
}

uint16_t emu_ir_alu(emu_ir_block_t *b, emu_ir_op_t op, uint16_t a, uint16_t bb)
{
    return emu_ir_emit(b, op, 0u, a, bb, 0u, 0u);
}

void emu_ir_bitop(emu_ir_block_t *b, emu_ir_op_t op, uint16_t addr,
                  uint16_t bit, uint32_t disp, uint8_t width)
{
    (void)emu_ir_emit(b, op, width, addr, bit, disp, EMU_IR_F_Z);
}

/* ------------------------------------------------------------------ */
/* Pass: dead flag elimination                                         */
/* ------------------------------------------------------------------ */

/*
 * Which flags does this instruction *read*?
 *
 * Only the conditional forms do. Everything else is a pure value
 * computation -- which is exactly what the SETF split buys: without it,
 * every flag-setting arithmetic op would both define and, as far as this
 * walk could tell, potentially read.
 *
 * A helper call is the conservative case. It gets the cpu pointer and may
 * do anything, including reading the guest's flag word, so it reads
 * everything.
 */
static uint8_t op_reads_flags(const emu_ir_insn_t *in)
{
    switch ((emu_ir_op_t)in->op) {
    case EMU_IR_GETCOND:
    case EMU_IR_SELECT:
    case EMU_IR_EXIT_IF:
        /*
         * Conservatively all of them rather than only the ones the
         * condition names. Narrowing this is a real further win for
         * G4MH -- a BE reads only Z -- but it has to be a table keyed on
         * emu_ir_cond_t, and a wrong entry deletes a definition that is
         * read, so it wants its own change and its own test.
         */
        return EMU_IR_F_ALL;
    /*
     * A faulting access hands control to a trap handler, which can read
     * anything -- so a flag defined before one is live for the same
     * reason a guest register written before one is.
     */
    case EMU_IR_LOAD:
    case EMU_IR_STORE:
    case EMU_IR_BITOP_SET:
    case EMU_IR_BITOP_CLR:
    case EMU_IR_BITOP_INV:
    case EMU_IR_BITOP_TST:
    case EMU_IR_HELPER:
    case EMU_IR_HELPER_TRAP:
        return EMU_IR_F_ALL;
    default:
        return 0u;
    }
}

/*
 * Walk backwards with a live set, exactly as a register allocator would
 * for values, and delete any SETF that defines nothing still wanted.
 *
 * This is the pass the whole IR was worth building for on a flag
 * machine. On x86-64 a full G4MH flag materialisation is `seto al` plus
 * `lahf` plus masking and a store -- and most of them are overwritten by
 * the next arithmetic instruction before any conditional looks.
 */
static void pass_dead_flags(emu_ir_block_t *b, uint8_t live_out,
                            emu_ir_opt_stats_t *st)
{
    uint8_t live = live_out;

    for (uint32_t i = b->count; i-- > 0;) {
        emu_ir_insn_t *const in = &b->insn[i];
        if (in->dead) {
            continue;
        }

        if (in->defs != 0u) {
            in->live = (uint8_t)(in->defs & live);
            if (in->op == (uint8_t)EMU_IR_SETF && in->live == 0u) {
                in->dead = true;
                st->flags_removed++;
                continue;
            }
            /* What it defines is no longer live above it. */
            live = (uint8_t)(live & ~in->defs);
        }

        live |= op_reads_flags(in);
    }
}

/* ------------------------------------------------------------------ */
/* Pass: guest register traffic                                        */
/* ------------------------------------------------------------------ */

/*
 * Forward walk keeping, per guest register, the temp that currently holds
 * its value. A GET of a register whose value is already in a temp becomes
 * a MOV from that temp, which the value pass below then deletes outright
 * if nothing else changed.
 *
 * This is the measured round trip: with the register file in memory a
 * dependent pair emits `store slot` then `load slot`, and a quarter to a
 * third of adjacent pairs in this project's guests are dependent.
 *
 * The table is invalidated wholesale at anything that could write the
 * register file behind our back -- a helper call, or the end of the
 * block. Being wrong here is a stale value read as a guest register,
 * which is a wrong answer rather than a crash, so the invalidation is
 * deliberately blunt.
 *
 * A hardwired-zero register is not tracked at all, and that is the whole
 * reason this pass needs the target.
 *
 * Every lowering already discards a PUT to x0 and answers a GET of it
 * with a constant, so the *emitted* code is right whatever this table
 * says. The trap is that this pass runs first and rewrites the IR: a PUT
 * to x0 recorded `cur[0] = temp`, and the next GET of x0 then became a
 * MOV of that temp -- a real value, in place of the zero every lowering
 * would have produced. The reference interpreter runs the same optimised
 * IR, so it agreed with the compiled code exactly, and the differential
 * checker reported nothing while 36 of 39 architecture tests failed.
 * x0 is written constantly: every discarded result and every canonical
 * NOP is a PUT to it.
 *
 * A pass that rewrites the IR must therefore honour the same guest
 * invariants the lowerings do. Being right in all three backends does
 * not help when the thing that is wrong runs before them.
 */
#define IR_MAX_GUEST_REGS 64u

/*
 * Whether a guest register reads as zero and discards writes. NULL target
 * means none does -- the passes stay usable by a unit test that builds a
 * block without one.
 */
static bool reg_is_zero(const emu_ir_target_t *t, uint32_t n)
{
    return t != NULL && t->reg_is_zero != NULL && t->reg_is_zero(n);
}

static void pass_reg_traffic(emu_ir_block_t *b, const emu_ir_target_t *t,
                             emu_ir_opt_stats_t *st)
{
    uint16_t cur[IR_MAX_GUEST_REGS];
    static uint16_t copy[EMU_IR_MAX_TEMPS];

    for (uint32_t i = 0; i < IR_MAX_GUEST_REGS; i++) {
        cur[i] = EMU_IR_NO_TEMP;
    }
    for (uint32_t i = 0; i < b->next_temp && i < EMU_IR_MAX_TEMPS; i++) {
        copy[i] = (uint16_t)i;
    }

    /*
     * Copy propagation is folded into this walk rather than run as its
     * own pass, because it is what makes the rewrite above pay. Turning
     * a redundant GET into a MOV saves the memory access but still emits
     * an instruction; resolving its readers straight to the original
     * temp leaves the MOV with no users at all, and the dead-value pass
     * then deletes it outright.
     *
     * Sound without any dominance reasoning because a temp is written
     * exactly once by construction and a block has one entry -- which is
     * the property that let this IR skip SSA.
     */
    for (uint32_t i = 0; i < b->count; i++) {
        emu_ir_insn_t *const in = &b->insn[i];
        if (in->dead) {
            continue;
        }

        if (in->a != EMU_IR_NO_TEMP && in->a < EMU_IR_MAX_TEMPS) {
            in->a = copy[in->a];
        }
        if (in->b != EMU_IR_NO_TEMP && in->b < EMU_IR_MAX_TEMPS) {
            in->b = copy[in->b];
        }
        if (in->c != EMU_IR_NO_TEMP && in->c < EMU_IR_MAX_TEMPS) {
            in->c = copy[in->c];
        }

        switch ((emu_ir_op_t)in->op) {
        case EMU_IR_GET:
            if (in->imm < IR_MAX_GUEST_REGS && !reg_is_zero(t, in->imm)) {
                if (cur[in->imm] != EMU_IR_NO_TEMP) {
                    /* Already in a temp: reuse it instead of reloading. */
                    in->op = (uint8_t)EMU_IR_MOV;
                    in->a = cur[in->imm];
                    st->gets_removed++;
                } else {
                    cur[in->imm] = in->dst;
                }
            }
            break;

        case EMU_IR_PUT:
            if (in->imm < IR_MAX_GUEST_REGS && !reg_is_zero(t, in->imm)) {
                cur[in->imm] = in->a;
            }
            break;

        case EMU_IR_HELPER:
        case EMU_IR_HELPER_TRAP:
            /* May touch anything. */
            for (uint32_t r = 0; r < IR_MAX_GUEST_REGS; r++) {
                cur[r] = EMU_IR_NO_TEMP;
            }
            break;

        default:
            break;
        }

        if (in->op == (uint8_t)EMU_IR_MOV && in->dst != EMU_IR_NO_TEMP &&
            in->dst < EMU_IR_MAX_TEMPS && in->a != EMU_IR_NO_TEMP) {
            copy[in->dst] = in->a;
        }
    }
}

/*
 * Delete a PUT that a later PUT to the same register overwrites with
 * nothing in between that could observe it.
 *
 * "Nothing that could observe it" is the whole difficulty. A helper call
 * reads the register file, and so does the end of the block -- the next
 * block, an interrupt handler and the debugger all see guest registers --
 * so only a PUT strictly dominated by another PUT with no helper and no
 * block exit between them can go.
 */
static void pass_dead_puts(emu_ir_block_t *b, emu_ir_opt_stats_t *st)
{
    bool seen[IR_MAX_GUEST_REGS];

    memset(seen, 0, sizeof(seen));

    for (uint32_t i = b->count; i-- > 0;) {
        emu_ir_insn_t *const in = &b->insn[i];
        if (in->dead) {
            continue;
        }

        switch ((emu_ir_op_t)in->op) {
        case EMU_IR_PUT:
            if (in->imm < IR_MAX_GUEST_REGS) {
                if (seen[in->imm]) {
                    in->dead = true;
                    st->puts_removed++;
                } else {
                    seen[in->imm] = true;
                }
            }
            break;

        /*
         * Anything that can leave the block early is an observation
         * point: everything written before it is visible to whatever
         * runs next.
         *
         * The memory operations belong here and it is not obvious. A
         * store to a register followed by a *second* store to the same
         * register looks like a dead first store -- unless the
         * instruction between them can fault, because then the second
         * never runs and the first is the value the trap handler sees.
         *
         *     addi t1, s0, 1     ; t1 = the address about to fault
         *     lh   t1, 1(s0)     ; traps; never writes t1
         *
         * riscv-tests' ma_addr does exactly this and then has its
         * handler compare mtval against t1. Deleting the first store
         * left t1 stale, the comparison failed, and every other test in
         * the suite still passed.
         */
        case EMU_IR_LOAD:
        case EMU_IR_STORE:
        case EMU_IR_BITOP_SET:
        case EMU_IR_BITOP_CLR:
        case EMU_IR_BITOP_INV:
        case EMU_IR_BITOP_TST:
        case EMU_IR_HELPER:
        case EMU_IR_HELPER_TRAP:
        case EMU_IR_EXIT:
        case EMU_IR_EXIT_IF:
            memset(seen, 0, sizeof(seen));
            break;

        default:
            break;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Pass: dead values                                                   */
/* ------------------------------------------------------------------ */

/*
 * Backward walk deleting any instruction whose result nothing reads.
 *
 * Runs last, because it is what turns the rewrites above into an actual
 * saving: a GET rewritten to a MOV is still an instruction until nothing
 * reads it, and the operands of a deleted SETF are frequently used by
 * nothing else.
 */
static void pass_dead_values(emu_ir_block_t *b, emu_ir_opt_stats_t *st)
{
    bool used[EMU_IR_MAX_TEMPS];

    memset(used, 0, sizeof(used));

    for (uint32_t i = b->count; i-- > 0;) {
        emu_ir_insn_t *const in = &b->insn[i];
        if (in->dead) {
            continue;
        }

        /*
         * The memory bit ops are effectful even though three of the four
         * define only a flag: they change memory. A dead-value pass that
         * judged them by their flag alone would delete a SET1 whose Z
         * nobody read, which is a silent wrong answer in guest memory.
         */
        const bool has_effect = (in->op == (uint8_t)EMU_IR_PUT) ||
                                (in->op == (uint8_t)EMU_IR_STORE) ||
                                (in->op == (uint8_t)EMU_IR_SETF) ||
                                (in->op == (uint8_t)EMU_IR_HELPER) ||
                                (in->op == (uint8_t)EMU_IR_HELPER_TRAP) ||
                                (in->op == (uint8_t)EMU_IR_RETIRE) ||
                                (in->op == (uint8_t)EMU_IR_SETPC) ||
                                (in->op == (uint8_t)EMU_IR_EXIT) ||
                                (in->op == (uint8_t)EMU_IR_EXIT_IF) ||
                                (in->op >= (uint8_t)EMU_IR_BITOP_SET &&
                                 in->op <= (uint8_t)EMU_IR_BITOP_TST);

        if (!has_effect && in->dst != EMU_IR_NO_TEMP &&
            in->dst < EMU_IR_MAX_TEMPS && !used[in->dst]) {
            in->dead = true;
            st->dead_removed++;
            continue;
        }

        if (in->a != EMU_IR_NO_TEMP && in->a < EMU_IR_MAX_TEMPS) {
            used[in->a] = true;
        }
        if (in->b != EMU_IR_NO_TEMP && in->b < EMU_IR_MAX_TEMPS) {
            used[in->b] = true;
        }
        if (in->c != EMU_IR_NO_TEMP && in->c < EMU_IR_MAX_TEMPS) {
            used[in->c] = true;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Pass: how many readers each value has                               */
/* ------------------------------------------------------------------ */

/*
 * Counted last, after every pass that deletes code, so a value is not
 * credited with readers that are themselves about to go.
 *
 * Sound without any dominance reasoning for the same reason the rest of
 * this file is: a temp is written exactly once by construction and a
 * block has one entry.
 */
/* ------------------------------------------------------------------ */
/* Fusion                                                             */
/* ------------------------------------------------------------------ */

/*
 * Three rewrites, all of which turn two IR instructions into one.
 *
 * **Here rather than in a backend**, and that is the point rather than a
 * convenience. A frontend decodes and emits IR; passes over that IR fuse
 * and eliminate; backends lower what survives. Written here each of these
 * serves x86-64 and Thumb-2 at once and matches on operations rather than
 * on encodings the IR has already abstracted away. The one fusion this
 * project built the other way -- ARM's shifted second operand, as a
 * backend peephole -- was correct on hardware, fired on *nothing*, and
 * cost 10%.
 *
 * What the histogram says to attack, in order:
 *
 *   const -> immediate   a materialised constant feeding an ALU
 *                        operation, which every frontend emits because
 *                        the IR's register forms are the general case
 *   addr fold            an ADDI feeding a LOAD or STORE, which already
 *                        carries a displacement of its own
 *   identity             arithmetic that computes its own input
 *
 * Every one is guarded on `uses == 1`. Folding a value that something
 * else still reads computes it twice, which is larger and slower than not
 * folding at all -- the rule emu_ir.h states for the backends' own
 * fusion, and it applies identically here.
 */

/* The immediate form of a register-register operation, or NOP if it has
 * none. NOP rather than a bool so the table is the answer. */
static uint8_t imm_form_of(uint8_t op)
{
    switch ((emu_ir_op_t)op) {
    case EMU_IR_ADD:
        return (uint8_t)EMU_IR_ADDI;
    case EMU_IR_AND:
        return (uint8_t)EMU_IR_ANDI;
    case EMU_IR_OR:
        return (uint8_t)EMU_IR_ORI;
    case EMU_IR_XOR:
        return (uint8_t)EMU_IR_XORI;
    case EMU_IR_SHL:
        return (uint8_t)EMU_IR_SHLI;
    case EMU_IR_SHR:
        return (uint8_t)EMU_IR_SHRI;
    case EMU_IR_SAR:
        return (uint8_t)EMU_IR_SARI;
    case EMU_IR_ROTL:
        return (uint8_t)EMU_IR_ROTLI;
    default:
        return (uint8_t)EMU_IR_NOP;
    }
}

/* Whether the constant may come from either operand. A shift's amount is
 * its right operand and nothing else; getting this wrong would compute
 * `k >> x` for `x >> k`. */
static bool op_commutes(uint8_t op)
{
    switch ((emu_ir_op_t)op) {
    case EMU_IR_ADD:
    case EMU_IR_AND:
    case EMU_IR_OR:
    case EMU_IR_XOR:
        return true;
    default:
        return false;
    }
}

/*
 * Whether the backend can lower this, when there is a backend.
 *
 * emu_ir_can_lower is defined by whichever host emitter is compiled in,
 * so an interpreter-only build has none and referencing it fails to
 * link. Without a backend nothing lowers this IR at all -- emu_ir_interp
 * implements every immediate form -- so the fold is unconditionally
 * safe there.
 */
static bool backend_can_lower(uint8_t op, uint8_t aux)
{
#if EMU_HAVE_JIT
    return emu_ir_can_lower((emu_ir_op_t)op, aux);
#else
    (void)op;
    (void)aux;
    return true;
#endif
}

/* A shift amount only folds when it is one the immediate form can mean.
 * Guests mask this -- RISC-V takes rs2 modulo 32 -- and an immediate of
 * 33 would be masked by one backend and not another. */
static bool shift_imm_ok(uint8_t op, uint32_t imm)
{
    switch ((emu_ir_op_t)op) {
    case EMU_IR_SHLI:
    case EMU_IR_SHRI:
    case EMU_IR_SARI:
    case EMU_IR_ROTLI:
        return imm < 32u;
    default:
        return true;
    }
}


/*
 * Multiply-accumulate: dst = c +/- (a * b), ARM's MLA and MLS.
 *
 * **A separate pass, and it has to run after the dead-code sweeps.** The
 * frontend emits `PUT rd, MUL(...)` because RISC-V's mul really does
 * write rd, so at pass_fuse time every multiply has two readers -- the
 * PUT and the add -- and `uses == 1` is never true. Measured on the
 * board before this was moved: 277 add/subs whose second operand came
 * from a MUL, and **zero** with a single use. Only once pass_dead_puts
 * has removed the PUT of a register nothing reads again does the count
 * become the one fusion needs.
 *
 * `uses == 1` remains the soundness condition. Fusing a multiply that
 * something else still reads computes it twice, which is bigger and
 * slower than not fusing.
 */
static void pass_mac(emu_ir_block_t *b, emu_ir_opt_stats_t *st)
{
    static uint8_t uses[EMU_IR_MAX_TEMPS];
    static uint16_t def[EMU_IR_MAX_TEMPS];

    memset(uses, 0, sizeof(uses));
    for (uint32_t i = 0; i < EMU_IR_MAX_TEMPS; i++) {
        def[i] = (uint16_t)EMU_IR_NO_TEMP;
    }

    for (uint32_t i = 0; i < b->count; i++) {
        const emu_ir_insn_t *const in = &b->insn[i];

        if (in->dead) {
            continue;
        }
        if (in->a != EMU_IR_NO_TEMP && in->a < EMU_IR_MAX_TEMPS &&
            uses[in->a] != 255u) {
            uses[in->a]++;
        }
        if (in->b != EMU_IR_NO_TEMP && in->b < EMU_IR_MAX_TEMPS &&
            uses[in->b] != 255u) {
            uses[in->b]++;
        }
        if (in->c != EMU_IR_NO_TEMP && in->c < EMU_IR_MAX_TEMPS &&
            uses[in->c] != 255u) {
            uses[in->c]++;
        }
        if (in->dst != EMU_IR_NO_TEMP && in->dst < EMU_IR_MAX_TEMPS) {
            def[in->dst] = (uint16_t)i;
        }
    }

    for (uint32_t i = 0; i < b->count; i++) {
        emu_ir_insn_t *const in = &b->insn[i];

        if (in->dead) {
            continue;
        }
        /* --- multiply-accumulate ------------------------------------ */
        /*
         * dst = c +/- (a * b), where the multiply's *only* consumer is
         * this add or subtract. ARM's MLA and MLS; RISC-V has no such
         * instruction, so the pattern exists only as two.
         *
         * **`uses == 1` is the whole soundness condition.** Fusing a
         * multiply that something else also reads computes it twice,
         * which is bigger and slower than not fusing -- the rule the
         * `uses` field exists for, and which the reverted
         * shifted-operand fusion is recorded as needing.
         *
         * **Measured before it was written.** CoreMark retires 6,156
         * `mul` immediately followed by a dependent `add` in a
         * 400,000-instruction sample, out of 9,470 multiplies. The pair
         * histogram's top-N list did not show it -- a direct trace did,
         * which is worth remembering the next time a pattern looks
         * absent.
         *
         * The subtracting form is asymmetric and easy to get backwards:
         * MLS is `c - a*b`, so only a SUB whose *second* operand is the
         * product qualifies. `a*b - c` is not an MLS and is left alone,
         * which is also why the commuted case below is ADD-only.
         */
        if (in->op == (uint8_t)EMU_IR_ADD ||
            in->op == (uint8_t)EMU_IR_SUB) {
            const bool is_sub = in->op == (uint8_t)EMU_IR_SUB;
            uint16_t prod = (uint16_t)EMU_IR_NO_TEMP;
            uint16_t addend = (uint16_t)EMU_IR_NO_TEMP;

            if (in->b != EMU_IR_NO_TEMP && in->b < EMU_IR_MAX_TEMPS &&
                uses[in->b] == 1u &&
                def[in->b] != (uint16_t)EMU_IR_NO_TEMP &&
                b->insn[def[in->b]].op == (uint8_t)EMU_IR_MUL &&
                !b->insn[def[in->b]].dead) {
                prod = in->b;
                addend = in->a;
            } else if (!is_sub && in->a != EMU_IR_NO_TEMP &&
                       in->a < EMU_IR_MAX_TEMPS && uses[in->a] == 1u &&
                       def[in->a] != (uint16_t)EMU_IR_NO_TEMP &&
                       b->insn[def[in->a]].op == (uint8_t)EMU_IR_MUL &&
                       !b->insn[def[in->a]].dead) {
                prod = in->a;
                addend = in->b;
            }

            if (prod != (uint16_t)EMU_IR_NO_TEMP &&
                addend != (uint16_t)EMU_IR_NO_TEMP &&
                backend_can_lower((uint8_t)EMU_IR_MAC,
                                  is_sub ? EMU_IR_MAC_SUB : 0u)) {
                emu_ir_insn_t *const mul = &b->insn[def[prod]];

                in->op = (uint8_t)EMU_IR_MAC;
                in->aux = is_sub ? (uint8_t)EMU_IR_MAC_SUB : 0u;
                in->a = mul->a;
                in->b = mul->b;
                in->c = addend;
                mul->dead = true;
                st->macs++;
            }
        }
    }
}

static void pass_fuse(emu_ir_block_t *b, emu_ir_opt_stats_t *st)
{
    static uint8_t uses[EMU_IR_MAX_TEMPS];
    static uint16_t def[EMU_IR_MAX_TEMPS];

    memset(uses, 0, sizeof(uses));
    for (uint32_t i = 0; i < EMU_IR_MAX_TEMPS; i++) {
        def[i] = (uint16_t)EMU_IR_NO_TEMP;
    }

    /*
     * Use counts and definition sites, over the *live* instructions. A
     * temp is written once by construction, so `def` is unambiguous --
     * the same property the register allocator relies on.
     */
    for (uint32_t i = 0; i < b->count; i++) {
        const emu_ir_insn_t *const in = &b->insn[i];

        if (in->dead) {
            continue;
        }
        if (in->a != EMU_IR_NO_TEMP && in->a < EMU_IR_MAX_TEMPS &&
            uses[in->a] != 255u) {
            uses[in->a]++;
        }
        if (in->b != EMU_IR_NO_TEMP && in->b < EMU_IR_MAX_TEMPS &&
            uses[in->b] != 255u) {
            uses[in->b]++;
        }
        if (in->c != EMU_IR_NO_TEMP && in->c < EMU_IR_MAX_TEMPS &&
            uses[in->c] != 255u) {
            uses[in->c]++;
        }
        if (in->dst != EMU_IR_NO_TEMP && in->dst < EMU_IR_MAX_TEMPS) {
            def[in->dst] = (uint16_t)i;
        }
    }

    for (uint32_t i = 0; i < b->count; i++) {
        emu_ir_insn_t *const in = &b->insn[i];

        if (in->dead) {
            continue;
        }

        /* --- a constant operand becomes an immediate ---------------- */
        const uint8_t iform = imm_form_of(in->op);

        if (iform != (uint8_t)EMU_IR_NOP) {
            uint16_t src = (uint16_t)EMU_IR_NO_TEMP;
            bool from_a = false;

            if (in->b != EMU_IR_NO_TEMP && in->b < EMU_IR_MAX_TEMPS &&
                uses[in->b] == 1u && def[in->b] != (uint16_t)EMU_IR_NO_TEMP &&
                b->insn[def[in->b]].op == (uint8_t)EMU_IR_CONST) {
                src = in->b;
            } else if (op_commutes(in->op) && in->a != EMU_IR_NO_TEMP &&
                       in->a < EMU_IR_MAX_TEMPS && uses[in->a] == 1u &&
                       def[in->a] != (uint16_t)EMU_IR_NO_TEMP &&
                       b->insn[def[in->a]].op == (uint8_t)EMU_IR_CONST) {
                src = in->a;
                from_a = true;
            }

            if (src != (uint16_t)EMU_IR_NO_TEMP) {
                const uint32_t k = b->insn[def[src]].imm;

                /*
                 * **Ask before rewriting.** An immediate form only helps
                 * if the backend has one, and turning a lowerable
                 * register operation into an unlowerable immediate one
                 * loses the whole block to the interpreter.
                 *
                 * That is not hypothetical: the first version of this
                 * pass had no guard, and on CoreMark it took the x86-64
                 * backend from 350 blocks and 23,133 interpreted
                 * instructions to 208 blocks and **423,764** -- 82% of
                 * the run, because neither backend lowers ADDI or ANDI.
                 * Every test still passed, because declining is
                 * correct: it is a correctness-preserving way to have no
                 * JIT, and no test of correctness can see it.
                 *
                 * emu_ir_can_lower is exactly this question and already
                 * existed for the frontends to ask before emitting.
                 */
                if (shift_imm_ok(iform, k) &&
                    backend_can_lower(iform, in->aux)) {
                    if (from_a) {
                        in->a = in->b; /* the surviving operand */
                    }
                    in->op = iform;
                    in->b = (uint16_t)EMU_IR_NO_TEMP;
                    in->imm = k;
                    b->insn[def[src]].dead = true;
                    uses[src] = 0u;
                    st->folded++;
                }
            }
        }

        /* --- an ADDI feeding an access becomes its displacement ----- */
        if ((in->op == (uint8_t)EMU_IR_LOAD ||
             in->op == (uint8_t)EMU_IR_STORE) &&
            in->a != EMU_IR_NO_TEMP && in->a < EMU_IR_MAX_TEMPS &&
            uses[in->a] == 1u && def[in->a] != (uint16_t)EMU_IR_NO_TEMP) {
            emu_ir_insn_t *const src = &b->insn[def[in->a]];

            if (!src->dead && src->op == (uint8_t)EMU_IR_ADDI &&
                src->a != EMU_IR_NO_TEMP) {
                /*
                 * Wrapping is the guest's own address arithmetic, so the
                 * sum needs no range check: a displacement that overflows
                 * 32 bits wraps in exactly the same place either way.
                 */
                in->imm += src->imm;
                in->a = src->a;
                src->dead = true;
                uses[in->a] = (uses[in->a] == 255u) ? 255u : uses[in->a];
                st->addr_folded++;
            }
        }

        /* --- arithmetic that computes its own input ----------------- */
        switch ((emu_ir_op_t)in->op) {
        case EMU_IR_ADDI:
        case EMU_IR_ORI:
        case EMU_IR_XORI:
        case EMU_IR_SHLI:
        case EMU_IR_SHRI:
        case EMU_IR_SARI:
        case EMU_IR_ROTLI:
            if (in->imm == 0u && in->a != EMU_IR_NO_TEMP) {
                in->op = (uint8_t)EMU_IR_MOV;
                st->identities++;
            }
            break;
        case EMU_IR_ANDI:
            if (in->imm == 0xFFFFFFFFu && in->a != EMU_IR_NO_TEMP) {
                in->op = (uint8_t)EMU_IR_MOV;
                st->identities++;
            } else if (in->imm == 0u) {
                in->op = (uint8_t)EMU_IR_CONST;
                in->a = (uint16_t)EMU_IR_NO_TEMP;
                st->identities++;
            }
            break;
        default:
            break;
        }
    }
}

static void pass_count_uses(emu_ir_block_t *b, emu_ir_opt_stats_t *st)
{
    static uint8_t uses[EMU_IR_MAX_TEMPS];

    memset(uses, 0, sizeof(uses));

    for (uint32_t i = 0; i < b->count; i++) {
        const emu_ir_insn_t *const in = &b->insn[i];

        if (in->dead) {
            continue;
        }
        if (in->a != EMU_IR_NO_TEMP && in->a < EMU_IR_MAX_TEMPS &&
            uses[in->a] != 255u) {
            uses[in->a]++;
        }
        if (in->b != EMU_IR_NO_TEMP && in->b < EMU_IR_MAX_TEMPS &&
            uses[in->b] != 255u) {
            uses[in->b]++;
        }
        if (in->c != EMU_IR_NO_TEMP && in->c < EMU_IR_MAX_TEMPS &&
            uses[in->c] != 255u) {
            uses[in->c]++;
        }
    }

    for (uint32_t i = 0; i < b->count; i++) {
        emu_ir_insn_t *const in = &b->insn[i];

        in->uses = (!in->dead && in->dst != EMU_IR_NO_TEMP &&
                    in->dst < EMU_IR_MAX_TEMPS)
                       ? uses[in->dst]
                       : 0u;
        if (in->uses == 1u) {
            st->single_use++;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Register allocation                                                 */
/* ------------------------------------------------------------------ */

/*
 * Live intervals, indexed by temp. Static rather than automatic because
 * on the microcontroller these are 1 KB and the translator runs on the
 * guest's stack; the pass is not reentrant, which is true of every pass
 * here and of the block buffer they all walk.
 */
static uint16_t g_def[EMU_IR_MAX_TEMPS];
static uint16_t g_last[EMU_IR_MAX_TEMPS];

#define IR_NO_POS 0xFFFFu

uint32_t emu_ir_regalloc(const emu_ir_block_t *b, uint32_t nregs,
                         uint8_t *assign)
{
    uint16_t inreg[EMU_IR_MAX_HOST_REGS];
    const uint32_t nt =
        (b->next_temp < EMU_IR_MAX_TEMPS) ? b->next_temp : EMU_IR_MAX_TEMPS;
    uint32_t used = 0u;

    for (uint32_t i = 0; i < nt; i++) {
        assign[i] = EMU_IR_NO_REG;
        g_def[i] = IR_NO_POS;
        g_last[i] = 0u;
    }
    if (nregs == 0u || nregs > EMU_IR_MAX_HOST_REGS || b->overflow) {
        return 0u;
    }
    for (uint32_t r = 0; r < nregs; r++) {
        inreg[r] = EMU_IR_NO_TEMP;
    }

    /*
     * One walk for both ends of every interval. `a`, `b` and `c` are
     * EMU_IR_NO_TEMP when absent, which is 0xFFFF and so above `nt` --
     * the bound doubles as the absent test.
     *
     * **`c` has to be here or the allocator reuses a live register.**
     * It is read by the fused multiply-adds alone, so a version that
     * forgot it would be correct on every block without an FMA and wrong
     * on the ones that have one -- the shape of bug this file's history
     * is full of.
     */
    for (uint32_t i = 0; i < b->count; i++) {
        const emu_ir_insn_t *const in = &b->insn[i];

        if (in->dead) {
            continue;
        }
        if ((uint32_t)in->a < nt) {
            g_last[in->a] = (uint16_t)i;
        }
        if ((uint32_t)in->b < nt) {
            g_last[in->b] = (uint16_t)i;
        }
        if ((uint32_t)in->c < nt) {
            g_last[in->c] = (uint16_t)i;
        }
        if ((uint32_t)in->dst < nt) {
            g_def[in->dst] = (uint16_t)i;
        }
    }

    /*
     * Temps are numbered in emission order, so walking them in order is
     * walking the intervals by start point -- which is what linear scan
     * needs and what would otherwise be a sort.
     */
    for (uint32_t tmp = 0; tmp < nt; tmp++) {
        const uint32_t d = g_def[tmp];

        if (d == IR_NO_POS || (uint32_t)g_last[tmp] <= d) {
            continue; /* never defined, or never read */
        }

        /*
         * Leave the one-instruction values to the reload elision. It
         * carries them in the scratch register for no instruction at
         * all, where a register here would cost one of very few.
         */
        if (b->insn[d].uses == 1u) {
            uint32_t j = d + 1u;

            while (j < b->count && b->insn[j].dead) {
                j++;
            }
            if (j == (uint32_t)g_last[tmp]) {
                continue;
            }
        }

        for (uint32_t r = 0; r < nregs; r++) {
            if (inreg[r] != EMU_IR_NO_TEMP && (uint32_t)g_last[inreg[r]] < d) {
                inreg[r] = EMU_IR_NO_TEMP;
            }
        }

        uint32_t pick = nregs;
        for (uint32_t r = 0; r < nregs; r++) {
            if (inreg[r] == EMU_IR_NO_TEMP) {
                pick = r;
                break;
            }
        }

        if (pick == nregs) {
            /*
             * All busy. Evict whichever interval runs furthest past this
             * one -- and if that is this one, it simply does not get a
             * register. Eviction is total: the temp goes back to its
             * frame slot for its whole life, which is sound only because
             * nothing has been emitted yet.
             */
            uint32_t worst = 0u;

            for (uint32_t r = 1u; r < nregs; r++) {
                if (g_last[inreg[r]] > g_last[inreg[worst]]) {
                    worst = r;
                }
            }
            if (g_last[inreg[worst]] <= g_last[tmp]) {
                continue;
            }
            assign[inreg[worst]] = EMU_IR_NO_REG;
            pick = worst;
        }

        inreg[pick] = (uint16_t)tmp;
        assign[tmp] = (uint8_t)pick;
        if (pick + 1u > used) {
            used = pick + 1u;
        }
    }

    return used;
}

/* ------------------------------------------------------------------ */

static emu_ir_opt_stats_t g_opt_totals;

void emu_ir_opt_totals(emu_ir_opt_stats_t *out)
{
    *out = g_opt_totals;
}

void emu_ir_optimise(emu_ir_block_t *b, const emu_ir_target_t *t,
                     uint8_t live_out, emu_ir_opt_stats_t *stats)
{
    emu_ir_opt_stats_t local;

    if (stats == NULL) {
        stats = &local;
    }
    memset(stats, 0, sizeof(*stats));

    if (b->overflow) {
        return;
    }

    /*
     * Order matters. Flags first, because deleting a SETF is what makes
     * its operands dead; register traffic next, because it rewrites GETs
     * into MOVs; dead values last, to sweep up what the other two
     * orphaned.
     */
    pass_dead_flags(b, live_out, stats);
    pass_reg_traffic(b, t, stats);
    /*
     * After reg_traffic, which rewrites GETs into MOVs and so exposes
     * operands this can reach; before the two sweeps, which are what
     * remove the CONSTs and ADDIs it orphans.
     */
    pass_fuse(b, stats);
    pass_dead_puts(b, stats);
    pass_dead_values(b, stats);
    pass_mac(b, stats);
    pass_count_uses(b, stats);

    g_opt_totals.blocks++;
    g_opt_totals.single_use += stats->single_use;
    g_opt_totals.flags_removed += stats->flags_removed;
    g_opt_totals.gets_removed += stats->gets_removed;
    g_opt_totals.puts_removed += stats->puts_removed;
    g_opt_totals.folded += stats->folded;
    g_opt_totals.addr_folded += stats->addr_folded;
    g_opt_totals.identities += stats->identities;
    g_opt_totals.macs += stats->macs;
    g_opt_totals.dead_removed += stats->dead_removed;
}
