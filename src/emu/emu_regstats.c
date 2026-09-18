/* SPDX-License-Identifier: Apache-2.0 */
/*
 * emu_regstats.c - guest-register traffic in translated blocks.
 *
 * See emu_regstats.h for what this measures and, more importantly, what
 * it deliberately does not.
 */

#include "emu/emu_regstats.h"

#if EMU_JIT_HOT_REG_STATS

#include "emu/emu_ir.h"

/*
 * stdio, and to stderr, exactly as emu_pairstats.c does: this is
 * measurement scaffolding built only on a host, and src/emu/ may not
 * reach a platform header. Guest output goes to stdout, so stderr is
 * what keeps a histogram out of a suite's comparison.
 */
#include <stdio.h>
#include <string.h>

/*
 * The widest guest register file among the frontends, plus room. RV32
 * has 32 integer registers, G4MH has 32, PowerPC has 32; the FP files
 * are addressed separately and do not reach a GET or a PUT.
 *
 * A register number past this is counted in the overflow bucket rather
 * than dropped: silently ignoring input is how a histogram comes to
 * report that a category is empty when it is unreachable.
 */
#define REG_MAX 64u

/* Reference counts above this share the top bucket. */
#define REF_BUCKETS 9u

static struct {
    uint64_t blocks;
    uint64_t gets; /* surviving reads  */
    uint64_t puts; /* surviving writes */
    uint64_t insns; /* IR instructions, for a sense of block size */

    /* Distinct guest registers touched by a block. */
    uint64_t distinct_hist[REG_MAX + 1u];

    /*
     * References to one register within one block, bucketed. Index 0 is
     * unused -- a register with no references is not a (block, register)
     * pair at all, and counting it would swamp everything with the 30-odd
     * registers a four-instruction block does not mention.
     */
    uint64_t ref_hist[REF_BUCKETS];

    /* Per register, across every block. */
    uint64_t reg_gets[REG_MAX];
    uint64_t reg_puts[REG_MAX];

    uint64_t out_of_range;
    uint64_t zero_reg; /* skipped: hardwired to zero */
} g;

void emu_reg_note_block(const struct emu_ir_block *b,
                        const struct emu_ir_target *t)
{
    uint8_t gets[REG_MAX];
    uint8_t puts[REG_MAX];
    unsigned distinct = 0u;

    if (b == NULL || b->overflow) {
        return;
    }

    memset(gets, 0, sizeof(gets));
    memset(puts, 0, sizeof(puts));

    g.blocks++;

    for (uint32_t i = 0u; i < b->count; i++) {
        const emu_ir_insn_t *const in = &b->insn[i];

        /*
         * Dead instructions are skipped for the same reason the count is
         * taken after the optimiser: a backend does not emit them, so
         * counting them would measure the IR rather than the code.
         */
        if (in->dead) {
            continue;
        }
        g.insns++;

        if (in->op != EMU_IR_GET && in->op != EMU_IR_PUT) {
            continue;
        }

        const uint32_t r = in->imm;

        if (r >= REG_MAX) {
            g.out_of_range++;
            continue;
        }

        /*
         * A hardwired-zero register is not traffic. Its GET survives the
         * passes deliberately and every backend answers it with a
         * constant at lowering; counting it reported RV32's x0 as the
         * busiest register in every guest measured.
         */
        if (t != NULL && t->reg_is_zero != NULL && t->reg_is_zero(r)) {
            g.zero_reg++;
            continue;
        }

        if (in->op == EMU_IR_GET) {
            g.gets++;
            g.reg_gets[r]++;
            if (gets[r] < 255u) {
                gets[r]++;
            }
        } else {
            g.puts++;
            g.reg_puts[r]++;
            if (puts[r] < 255u) {
                puts[r]++;
            }
        }
    }

    for (unsigned r = 0u; r < REG_MAX; r++) {
        const unsigned refs = (unsigned)gets[r] + (unsigned)puts[r];

        if (refs == 0u) {
            continue;
        }
        distinct++;
        g.ref_hist[(refs < REF_BUCKETS) ? refs : (REF_BUCKETS - 1u)]++;
    }
    g.distinct_hist[distinct]++;
}

static double share(uint64_t part, uint64_t whole)
{
    return (whole == 0u) ? 0.0 : ((double)part * 100.0 / (double)whole);
}

void emu_reg_report(void)
{
    if (g.blocks == 0u) {
        fprintf(stderr, "\n# no blocks were translated\n");
        return;
    }

    fprintf(stderr,
            "\n# guest-register traffic, per translation and NOT per block\n"
            "# entry -- a block entered a million times counts once here.\n"
            "# %llu blocks, %llu live IR insns, %llu gets, %llu puts\n"
            "# (%llu references to a hardwired-zero register excluded --\n"
            "#  a backend answers those with a constant, not a load)\n",
            (unsigned long long)g.blocks, (unsigned long long)g.insns,
            (unsigned long long)g.gets, (unsigned long long)g.puts,
            (unsigned long long)g.zero_reg);

    /*
     * The number the cost model turns on.
     *
     * Pinning a register costs a load at block entry and a store at block
     * exit, so it breaks even at two references within a block and only
     * pays above them. Everything at 1 and 2 is a loss however hot the
     * block is, which is why a negative result here needs no dynamic
     * confirmation and a positive one does.
     */
    {
        uint64_t pairs = 0u;
        uint64_t payable = 0u;

        for (unsigned n = 1u; n < REF_BUCKETS; n++) {
            pairs += g.ref_hist[n];
            if (n >= 3u) {
                payable += g.ref_hist[n];
            }
        }

        fprintf(stderr, "\n# references to one register within one block\n");
        for (unsigned n = 1u; n < REF_BUCKETS; n++) {
            if (g.ref_hist[n] == 0u) {
                continue;
            }
            fprintf(stderr, "%s%u %14llu %7.2f%%\n",
                    (n == REF_BUCKETS - 1u) ? ">=" : "  ", n,
                    (unsigned long long)g.ref_hist[n],
                    share(g.ref_hist[n], pairs));
        }
        fprintf(stderr, "3 or more -- all that pinning can pay for: %.2f%%\n",
                share(payable, pairs));
    }

    /*
     * What a pinned set costs every block, including the ones that never
     * touch it: the save and restore are paid on entry regardless.
     */
    {
        uint64_t sum = 0u;

        for (unsigned n = 0u; n <= REG_MAX; n++) {
            sum += g.distinct_hist[n] * (uint64_t)n;
        }
        fprintf(stderr, "\n# distinct guest registers per block: mean %.2f\n",
                (double)sum / (double)g.blocks);
    }

    /*
     * Per register, so a *fixed* pinned set can be costed: if the traffic
     * is spread evenly there is no small set worth choosing.
     */
    {
        const uint64_t total = g.gets + g.puts;
        uint8_t taken[REG_MAX];
        uint64_t cum = 0u;

        memset(taken, 0, sizeof(taken));
        fprintf(stderr, "\n# busiest registers\n");
        fprintf(stderr, "%-5s %12s %8s %8s %10s %10s\n", "reg", "refs",
                "share", "cumul", "reads", "writes");
        for (unsigned k = 0u; k < 8u; k++) {
            unsigned best = REG_MAX;
            uint64_t best_n = 0u;

            for (unsigned r = 0u; r < REG_MAX; r++) {
                const uint64_t n = g.reg_gets[r] + g.reg_puts[r];

                if (taken[r] == 0u && n > best_n) {
                    best_n = n;
                    best = r;
                }
            }
            if (best == REG_MAX || best_n == 0u) {
                break;
            }
            taken[best] = 1u;
            cum += best_n;
            fprintf(stderr, "r%-4u %12llu %7.2f%% %7.2f%% %10llu %10llu\n",
                    best, (unsigned long long)best_n, share(best_n, total),
                    share(cum, total),
                    (unsigned long long)g.reg_gets[best],
                    (unsigned long long)g.reg_puts[best]);
        }
    }

    if (g.out_of_range != 0u) {
        fprintf(stderr,
                "\n# WARNING: %llu references above r%u were not counted;\n"
                "# REG_MAX is too small and the histogram is incomplete.\n",
                (unsigned long long)g.out_of_range, REG_MAX - 1u);
    }
}

#endif /* EMU_JIT_HOT_REG_STATS */
