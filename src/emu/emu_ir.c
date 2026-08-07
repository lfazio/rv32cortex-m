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

uint16_t emu_ir_emit(emu_ir_block_t *b, emu_ir_op_t op, uint8_t aux,
                     uint16_t a, uint16_t bb, uint32_t imm, uint8_t defs)
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
    in->imm = imm;
    in->defs = defs;
    /*
     * Live until a pass proves otherwise. The conservative direction
     * matters: an uninitialised `live` that happened to be zero would
     * delete a flag definition something depends on, and the failure is
     * a wrong branch several instructions later.
     */
    in->live = defs;
    in->dead = false;
    in->dst = op_writes(op) ? emu_ir_temp(b) : EMU_IR_NO_TEMP;
    return in->dst;
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
    return emu_ir_emit(b, EMU_IR_CONST, 0u, EMU_IR_NO_TEMP, EMU_IR_NO_TEMP,
                       v, 0u);
}

uint16_t emu_ir_alu(emu_ir_block_t *b, emu_ir_op_t op, uint16_t a,
                    uint16_t bb)
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
 */
#define IR_MAX_GUEST_REGS 64u

static void pass_reg_traffic(emu_ir_block_t *b, emu_ir_opt_stats_t *st)
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

        switch ((emu_ir_op_t)in->op) {
        case EMU_IR_GET:
            if (in->imm < IR_MAX_GUEST_REGS) {
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
            if (in->imm < IR_MAX_GUEST_REGS) {
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

        case EMU_IR_HELPER:
        case EMU_IR_HELPER_TRAP:
        case EMU_IR_EXIT:
        case EMU_IR_EXIT_IF:
            /* Everything written before this point is observable. */
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
        const bool has_effect =
            (in->op == (uint8_t)EMU_IR_PUT) ||
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
    }
}

/* ------------------------------------------------------------------ */

void emu_ir_optimise(emu_ir_block_t *b, uint8_t live_out,
                     emu_ir_opt_stats_t *stats)
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
    pass_reg_traffic(b, stats);
    pass_dead_puts(b, stats);
    pass_dead_values(b, stats);
}
