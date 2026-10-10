/* SPDX-License-Identifier: Apache-2.0 */
/*
 * armv7m_dis.c - disassemble ranges of a flat Thumb-2 image.
 *
 *     armv7m-dis image.bin 0x08000000 < ranges
 *
 * `ranges` is one `start end` pair per line, in hex, absolute addresses.
 * A flat image is code and data together -- literal pools sit inside
 * functions -- so the ranges come from the ELF's `$t` mapping symbols,
 * which is the assembler's own statement of where instructions are.
 *
 * Output is one instruction per line, tab separated: the address, the
 * length, the encoding (first halfword first) and the text.
 * tests/armv7m-diff/disasm-check.py reads exactly that.
 */

#include "armv7m/armv7m_decode.h"
#include "armv7m/armv7m_disasm.h"

#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    unsigned char *img;
    unsigned long base;
    long size;
    FILE *f;
    unsigned long lo;
    unsigned long hi;

    if (argc != 3) {
        fprintf(stderr, "usage: %s image.bin base < ranges\n", argv[0]);
        return 2;
    }
    base = strtoul(argv[2], NULL, 0);
    f = fopen(argv[1], "rb");
    if (f == NULL) {
        perror(argv[1]);
        return 2;
    }
    (void)fseek(f, 0, SEEK_END);
    size = ftell(f);
    (void)fseek(f, 0, SEEK_SET);
    img = malloc((size_t)size + 4u);
    if (img == NULL || fread(img, 1u, (size_t)size, f) != (size_t)size) {
        fprintf(stderr, "%s: short read\n", argv[1]);
        return 2;
    }
    (void)fclose(f);

    while (scanf("%lx %lx", &lo, &hi) == 2) {
        unsigned long pc = lo;

        while (pc + 2u <= hi && pc - base + 2u <= (unsigned long)size) {
            const unsigned char *const p = img + (pc - base);
            const unsigned w0 = (unsigned)p[0] | ((unsigned)p[1] << 8);
            const unsigned len = armv7m_insn_len((uint16_t)w0);
            unsigned w1 = 0u;
            char text[96];

            if (len == 4u) {
                if (pc + 4u > hi || pc - base + 4u > (unsigned long)size) {
                    break;
                }
                w1 = (unsigned)p[2] | ((unsigned)p[3] << 8);
            }
            (void)armv7m_disasm(text, sizeof(text), (uint32_t)pc,
                                (uint64_t)w0 | ((uint64_t)w1 << 16), len);
            if (len == 4u) {
                printf("%08lx\t4\t%04x%04x\t%s\n", pc, w0, w1, text);
            } else {
                printf("%08lx\t2\t%04x\t%s\n", pc, w0, text);
            }
            pc += len;
        }
    }
    free(img);
    return 0;
}
