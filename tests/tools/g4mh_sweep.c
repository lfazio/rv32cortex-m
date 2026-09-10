/* SPDX-License-Identifier: Apache-2.0 */
/*
 * g4mh_sweep.c - what this frontend cannot decode, counted over real
 * compiler output.
 *
 * Walk a flat G4MH image the way the interpreter does -- length from the
 * first halfword, then the 48- and 64-bit refinements -- disassemble each
 * instruction, and histogram the ones that come back `.short`.
 *
 * **Why over a binary rather than over a mnemonic list.** The obvious
 * sweep assembles every mnemonic the manual names and checks each one.
 * That needs an operand form invented per instruction, gets the awkward
 * ones wrong, and answers a question nobody has: what matters is not
 * whether an encoding *exists* but whether the compiler *emits* it. Six
 * megabytes of CC-RH output answers that directly, and the histogram is
 * ordered by how often a guest would actually hit each gap.
 *
 * **A `.short` is not proof of a decoder gap.** This tree has recorded
 * twice that the disassembler is not the decoder: it printed `.short`
 * for MUL imm9 and for LDL.W's pointer-updating neighbours while the
 * interpreter executed both correctly. So this reports candidates, and
 * each one has to be confirmed against the interpreter -- which is the
 * honest shape, because the reverse error is worse: a slot the
 * disassembler *names* can still be decoded wrongly, and this tool
 * cannot see that at all. Bcond disp17 was exactly that case, printing
 * `.short` while being executed as a load.
 *
 *   g4mh-sweep <image.bin> [load-address]
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "g4mh/g4mh_decode.h"
#include "g4mh/g4mh_disasm.h"

/*
 * Keyed on the first halfword's opcode and the second halfword, which
 * together are what every decode in this frontend switches on. Kept as a
 * flat table because the interesting answer is the top of the list and
 * there are only a few thousand distinct pairs in any real image.
 */
typedef struct {
    uint32_t op;
    uint32_t sub;
    uint32_t reg2;
    uint32_t count;
    uint32_t first_pc;
    uint16_t w0;
    uint16_t w1;
} slot_t;

#define MAX_SLOTS 4096
static slot_t g_slots[MAX_SLOTS];
static unsigned g_nslots;

static void note(uint32_t op, uint32_t sub, uint32_t reg2, uint32_t pc,
                 uint16_t w0, uint16_t w1)
{
    for (unsigned i = 0; i < g_nslots; i++) {
        if (g_slots[i].op == op && g_slots[i].sub == sub &&
            g_slots[i].reg2 == reg2) {
            g_slots[i].count++;
            return;
        }
    }
    if (g_nslots < MAX_SLOTS) {
        g_slots[g_nslots].op = op;
        g_slots[g_nslots].sub = sub;
        g_slots[g_nslots].reg2 = reg2;
        g_slots[g_nslots].count = 1u;
        g_slots[g_nslots].first_pc = pc;
        g_slots[g_nslots].w0 = w0;
        g_slots[g_nslots].w1 = w1;
        g_nslots++;
    }
}

static int by_count(const void *a, const void *b)
{
    const slot_t *x = (const slot_t *)a;
    const slot_t *y = (const slot_t *)b;

    return (x->count < y->count) ? 1 : (x->count > y->count) ? -1 : 0;
}

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8));
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: g4mh-sweep <image.bin> [load-address]\n");
        return 2;
    }

    const uint32_t base =
        (argc > 2) ? (uint32_t)strtoul(argv[2], NULL, 0) : 0x80000000u;

    FILE *f = fopen(argv[1], "rb");

    if (f == NULL) {
        fprintf(stderr, "g4mh-sweep: cannot open %s\n", argv[1]);
        return 2;
    }
    (void)fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    (void)fseek(f, 0, SEEK_SET);

    uint8_t *img = (uint8_t *)malloc((size_t)size);

    if (img == NULL || fread(img, 1u, (size_t)size, f) != (size_t)size) {
        fprintf(stderr, "g4mh-sweep: cannot read %s\n", argv[1]);
        return 2;
    }
    (void)fclose(f);

    unsigned long total = 0;
    unsigned long undecoded = 0;
    long off = 0;

    while (off + 2 <= size) {
        const uint16_t w0 = rd16(img + off);
        unsigned len = g4mh_insn_len(w0);
        uint16_t w1 = 0;
        uint16_t w2 = 0;

        if (len == 4u && off + 4 <= size) {
            w1 = rd16(img + off + 2);
            if (g4mh_insn_is_48(w0, w1)) {
                len = g4mh_insn_is_64(w0, w1) ? 8u : 6u;
            }
        }
        if (off + (long)len > size) {
            break;
        }
        if (len >= 6u) {
            w2 = rd16(img + off + 4);
        }

        uint64_t insn = (uint64_t)w0;

        if (len >= 4u) {
            insn |= (uint64_t)w1 << 16;
        }
        if (len >= 6u) {
            insn |= (uint64_t)w2 << 32;
        }
        if (len >= 8u) {
            insn |= (uint64_t)rd16(img + off + 6) << 48;
        }

        char buf[96];

        (void)g4mh_disasm(buf, sizeof buf, base + (uint32_t)off, insn, len);
        total++;
        if (strncmp(buf, ".short", 6) == 0) {
            undecoded++;
            note((uint32_t)((w0 >> 5) & 0x3Fu), (uint32_t)w1 & 0x7FFu,
                 (uint32_t)((w0 >> 11) & 0x1Fu), base + (uint32_t)off, w0, w1);
        }
        off += (long)len;
    }

    printf("%s: %lu instructions, %lu not named (%.2f%%)\n", argv[1], total,
           undecoded, total ? 100.0 * (double)undecoded / (double)total : 0.0);
    printf("\n  count  op6  sub    reg2  first pc    halfwords\n");

    qsort(g_slots, g_nslots, sizeof g_slots[0], by_count);
    for (unsigned i = 0; i < g_nslots && i < 25u; i++) {
        printf("  %5u  0x%02x 0x%03x  %2u    0x%08x  %04x %04x\n",
               g_slots[i].count, g_slots[i].op, g_slots[i].sub,
               g_slots[i].reg2, g_slots[i].first_pc, g_slots[i].w0,
               g_slots[i].w1);
    }
    if (g_nslots > 25u) {
        printf("  ... and %u more distinct slots\n", g_nslots - 25u);
    }

    /*
     * A flat image is code and data together, so walking it from the
     * first byte decodes constants as instructions and desynchronises
     * after any that is not one. The number above is therefore an upper
     * bound, and the *slots* are what to read: a real gap appears
     * thousands of times at a consistent sub-opcode, and noise appears
     * once each, scattered.
     */
    free(img);
    return 0;
}
