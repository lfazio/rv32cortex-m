/* SPDX-License-Identifier: Apache-2.0 */
/*
 * rv_pairstats.c - Adjacent-instruction-pair histogram.
 */

#include "rv32/rv_pairstats.h"

#if RV_PAIR_STATS

#include "rv32/rv_decode.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * A pair is keyed on a compact "kind" per instruction rather than on the
 * raw encoding: what a fusion rule matches on is the operation, not the
 * registers. 12 bits is opcode[6:2] plus funct3 plus a bit for funct7's
 * bit 30, which is enough to separate ADD from SUB and SRLI from SRAI.
 */
#define KIND_BITS   12u
#define KIND_COUNT  (1u << KIND_BITS)

static uint32_t kind_of(uint32_t insn)
{
    return ((insn >> 2) & 0x1Fu)
         | (((insn >> 12) & 0x7u) << 5)
         | (((insn >> 30) & 0x1u) << 8);
}

/*
 * Whether the second instruction consumes the first's result, and whether
 * the first's result is then dead. Those two facts are what decide if a
 * pair is fusible at all, so they are part of the key -- a histogram of
 * bare opcode pairs would count ADDs that have nothing to do with each
 * other alongside the ones that form an address.
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

static uint32_t g_prev_insn;
static uint32_t g_prev_next_pc;
static bool     g_have_prev;

/* rd of an instruction, or 0 when it writes nothing. */
static uint32_t rd_of(uint32_t insn)
{
    switch (insn & 0x7Fu) {
    case 0x23:  /* STORE  */
    case 0x63:  /* BRANCH */
    case 0x27:  /* STORE-FP */
        return 0u;
    default:
        return (insn >> 7) & 0x1Fu;
    }
}

static bool reads_rs1(uint32_t insn)
{
    switch (insn & 0x7Fu) {
    case 0x37: case 0x17: case 0x6F:   /* LUI, AUIPC, JAL */
        return false;
    default:
        return true;
    }
}

static bool reads_rs2(uint32_t insn)
{
    switch (insn & 0x7Fu) {
    case 0x33: case 0x23: case 0x63: case 0x2F:  /* OP, STORE, BRANCH, AMO */
        return true;
    default:
        return false;
    }
}

void rv_pair_note(uint32_t pc, uint32_t insn, unsigned len)
{
    g_total++;

    if (g_have_prev && g_prev_next_pc == pc) {
        const uint32_t prd = rd_of(g_prev_insn);
        uint32_t link = LINK_NONE;

        if (prd != 0u) {
            if (reads_rs1(insn) && ((insn >> 15) & 0x1Fu) == prd) {
                link |= LINK_RS1;
            }
            if (reads_rs2(insn) && ((insn >> 20) & 0x1Fu) == prd) {
                link |= LINK_RS2;
            }
            if (link != LINK_NONE && rd_of(insn) == prd) {
                link |= LINK_DEAD;
            }
        }

        const uint32_t k0 = kind_of(g_prev_insn);
        const uint32_t k1 = kind_of(insn);

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

static const char *kind_name(uint32_t k)
{
    static char buf[8][24];
    static unsigned slot;
    char *b = buf[slot++ & 7u];

    const uint32_t op = (k & 0x1Fu) << 2 | 3u;
    const uint32_t f3 = (k >> 5) & 7u;
    const uint32_t f7 = (k >> 8) & 1u;

    switch (op) {
    case 0x37: return "lui";
    case 0x17: return "auipc";
    case 0x6F: return "jal";
    case 0x67: return "jalr";
    case 0x63: {
        static const char *const b3[8] = { "beq","bne","?","?","blt","bge","bltu","bgeu" };
        return b3[f3];
    }
    case 0x03: {
        static const char *const l3[8] = { "lb","lh","lw","?","lbu","lhu","?","?" };
        return l3[f3];
    }
    case 0x23: {
        static const char *const s3[8] = { "sb","sh","sw","?","?","?","?","?" };
        return s3[f3];
    }
    case 0x13: {
        static const char *const i3[8] = { "addi","slli","slti","sltiu","xori","srli","ori","andi" };
        if (f3 == 5u && f7) { return "srai"; }
        return i3[f3];
    }
    case 0x33: {
        static const char *const r3[8] = { "add","sll","slt","sltu","xor","srl","or","and" };
        if (f3 == 0u && f7) { return "sub"; }
        if (f3 == 5u && f7) { return "sra"; }
        return r3[f3];
    }
    case 0x0F: return "fence";
    case 0x73: return "system";
    case 0x2F: return "amo";
    default:
        snprintf(b, 24, "op%02x.f%u", (unsigned)op, (unsigned)f3);
        return b;
    }
}

static int by_count(const void *a, const void *b)
{
    const entry_t *x = (const entry_t *)a;
    const entry_t *y = (const entry_t *)b;
    return (y->count > x->count) - (y->count < x->count);
}

void rv_pair_report(unsigned top_n)
{
    qsort(g_tab, g_used, sizeof(g_tab[0]), by_count);

    fprintf(stderr, "\n# executed %llu instructions, %llu adjacent pairs, "
                    "%u distinct\n", (unsigned long long)g_total,
                    (unsigned long long)g_pairs, g_used);
    /*
     * The aggregate is what decides whether fusion is worth doing at all.
     * With the register file in memory, *every* pair whose second
     * instruction consumes the first's result pays an STR followed
     * immediately by an LDR of the same slot -- two host instructions of
     * pure round-trip. That share is the ceiling on what any amount of
     * fusion or peepholing can recover.
     */
    uint64_t linked = 0, dead = 0, addr = 0;
    for (unsigned i = 0; i < g_used; i++) {
        if (g_tab[i].link == LINK_NONE) {
            continue;
        }
        linked += g_tab[i].count;
        if (g_tab[i].link & LINK_DEAD) {
            dead += g_tab[i].count;
        }
        /* address generation feeding a load or store */
        const uint32_t op1 = ((g_tab[i].k1 & 0x1Fu) << 2) | 3u;
        const uint32_t op0 = ((g_tab[i].k0 & 0x1Fu) << 2) | 3u;
        if ((op1 == 0x03u || op1 == 0x23u) &&
            (op0 == 0x13u || op0 == 0x33u) &&
            (g_tab[i].link & LINK_RS1)) {
            addr += g_tab[i].count;
        }
    }
    fprintf(stderr, "# dependent pairs      %10llu  %6.2f%%"
                    "   (each pays STR+LDR today)\n",
            (unsigned long long)linked,
            100.0 * (double)linked / (double)g_pairs);
    fprintf(stderr, "#   of which dead      %10llu  %6.2f%%"
                    "   (intermediate never read again)\n",
            (unsigned long long)dead,
            100.0 * (double)dead / (double)g_pairs);
    fprintf(stderr, "#   addr-gen -> mem    %10llu  %6.2f%%"
                    "   (fusible to LDR/STR [Rn,Rm])\n\n",
            (unsigned long long)addr,
            100.0 * (double)addr / (double)g_pairs);

    fprintf(stderr, "%-10s %-10s %-7s %12s %7s\n",
            "first", "second", "link", "count", "share");

    for (unsigned i = 0; i < g_used && i < top_n; i++) {
        char link[8];
        unsigned n = 0;
        if (g_tab[i].link & LINK_RS1)  { link[n++] = 's'; link[n++] = '1'; }
        if (g_tab[i].link & LINK_RS2)  { link[n++] = 's'; link[n++] = '2'; }
        if (g_tab[i].link & LINK_DEAD) { link[n++] = '!'; }
        if (n == 0u) { link[n++] = '-'; }
        link[n] = '\0';

        fprintf(stderr, "%-10s %-10s %-7s %12llu %6.2f%%\n",
                kind_name(g_tab[i].k0), kind_name(g_tab[i].k1), link,
                (unsigned long long)g_tab[i].count,
                100.0 * (double)g_tab[i].count / (double)g_pairs);
    }
}

#endif /* RV_PAIR_STATS */
