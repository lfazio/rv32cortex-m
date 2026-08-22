/* SPDX-License-Identifier: Apache-2.0 */
/*
 * emu_pairstats.c - the histogram. See emu_pairstats.h for why it is here
 * rather than in a frontend.
 */

#include "emu/emu_pairstats.h"

#if EMU_PAIR_STATS

#include <stdio.h>
#include <stdlib.h>

/*
 * Whether the second instruction consumes the first's result, and whether
 * the first's result is then dead. Those two facts are what decide if a
 * pair is fusible at all, so they are part of the key -- a histogram of
 * bare opcode pairs would count operations that have nothing to do with
 * each other alongside the ones that form an address.
 */
#define LINK_NONE   0u
#define LINK_RS1    1u   /* second reads first's rd as rs1  */
#define LINK_RS2    2u   /* ... as rs2                      */
#define LINK_DEAD   4u   /* and the second overwrites it    */

typedef struct {
    uint32_t k0, k1;
    uint32_t link;
    uint64_t count;
} entry_t;

static entry_t  g_tab[8192];
static unsigned g_used;
static uint64_t g_total;
static uint64_t g_pairs;

static const emu_pair_ops_t *g_ops;

static uint64_t g_prev_insn;
static uint32_t g_prev_next_pc;
static bool     g_have_prev;

void emu_pair_note(const emu_pair_ops_t *ops, uint32_t pc, uint64_t insn,
                   unsigned len)
{
    g_ops = ops;
    g_total++;

    if (g_have_prev && g_prev_next_pc == pc) {
        const uint32_t prd = ops->rd(g_prev_insn);
        uint32_t link = LINK_NONE;

        if (prd != 0u) {
            if (ops->rs1(insn) == prd) {
                link |= LINK_RS1;
            }
            if (ops->rs2(insn) == prd) {
                link |= LINK_RS2;
            }
            if (link != LINK_NONE && ops->rd(insn) == prd) {
                link |= LINK_DEAD;
            }
        }

        const uint32_t k0 = ops->kind(g_prev_insn);
        const uint32_t k1 = ops->kind(insn);

        unsigned i;

        for (i = 0; i < g_used; i++) {
            if (g_tab[i].k0 == k0 && g_tab[i].k1 == k1 &&
                g_tab[i].link == link) {
                g_tab[i].count++;
                break;
            }
        }
        if (i == g_used && g_used < sizeof(g_tab) / sizeof(g_tab[0])) {
            g_tab[g_used].k0 = k0;
            g_tab[g_used].k1 = k1;
            g_tab[g_used].link = link;
            g_tab[g_used].count = 1u;
            g_used++;
        }
        g_pairs++;
    }

    g_prev_insn = insn;
    g_prev_next_pc = pc + len;
    g_have_prev = true;
}

/* ------------------------------------------------------------------ */

static int by_count(const void *a, const void *b)
{
    const entry_t *x = (const entry_t *)a;
    const entry_t *y = (const entry_t *)b;

    return (y->count > x->count) - (y->count < x->count);
}

void emu_pair_report(unsigned top_n)
{
    if (g_ops == NULL || g_pairs == 0u) {
        fprintf(stderr, "\n# no instruction pairs recorded\n");
        return;
    }

    qsort(g_tab, g_used, sizeof(g_tab[0]), by_count);

    fprintf(stderr, "\n# executed %llu instructions, %llu adjacent pairs, "
                    "%u distinct\n", (unsigned long long)g_total,
                    (unsigned long long)g_pairs, g_used);

    /*
     * The aggregate is what decides whether fusion is worth doing at all.
     * With the register file in memory, *every* pair whose second
     * instruction consumes the first's result pays a store followed
     * immediately by a load of the same slot -- two host instructions of
     * pure round-trip. That share is the ceiling on what any amount of
     * fusion or peepholing can recover, and on CoreMark it is 29.6% while
     * the textbook fusions are 0.2%.
     */
    uint64_t linked = 0, dead = 0, addr = 0;

    for (unsigned i = 0; i < g_used; i++) {
        if (g_tab[i].link == LINK_NONE) {
            continue;
        }
        linked += g_tab[i].count;
        if ((g_tab[i].link & LINK_DEAD) != 0u) {
            dead += g_tab[i].count;
        }
        /* address generation feeding a load or store */
        if (g_ops->kind_is_mem(g_tab[i].k1) &&
            g_ops->kind_is_alu(g_tab[i].k0) &&
            (g_tab[i].link & LINK_RS1) != 0u) {
            addr += g_tab[i].count;
        }
    }

    fprintf(stderr, "# dependent pairs      %10llu  %6.2f%%"
                    "   (each pays a store+load today)\n",
            (unsigned long long)linked,
            100.0 * (double)linked / (double)g_pairs);
    fprintf(stderr, "#   of which dead      %10llu  %6.2f%%"
                    "   (intermediate never read again)\n",
            (unsigned long long)dead,
            100.0 * (double)dead / (double)g_pairs);
    fprintf(stderr, "#   addr-gen -> mem    %10llu  %6.2f%%"
                    "   (fusible to a scaled-index access)\n\n",
            (unsigned long long)addr,
            100.0 * (double)addr / (double)g_pairs);

    fprintf(stderr, "%-10s %-10s %-7s %12s %7s\n",
            "first", "second", "link", "count", "share");

    for (unsigned i = 0; i < g_used && i < top_n; i++) {
        char link[8];
        char n0[24], n1[24];
        unsigned n = 0;

        if ((g_tab[i].link & LINK_RS1) != 0u)  { link[n++] = 's'; link[n++] = '1'; }
        if ((g_tab[i].link & LINK_RS2) != 0u)  { link[n++] = 's'; link[n++] = '2'; }
        if ((g_tab[i].link & LINK_DEAD) != 0u) { link[n++] = '!'; }
        if (n == 0u) { link[n++] = '-'; }
        link[n] = '\0';

        g_ops->kind_name(g_tab[i].k0, n0, sizeof(n0));
        g_ops->kind_name(g_tab[i].k1, n1, sizeof(n1));

        fprintf(stderr, "%-10s %-10s %-7s %12llu %6.2f%%\n",
                n0, n1, link, (unsigned long long)g_tab[i].count,
                100.0 * (double)g_tab[i].count / (double)g_pairs);
    }
}

#endif /* EMU_PAIR_STATS */
