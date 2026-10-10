/* SPDX-License-Identifier: Apache-2.0 */
/*
 * ppc_dis.c - the e200z7 decoder and disassembler, driven from a script.
 *
 *     ppc-dis < words
 *
 * Each input line is `hex len mode [pc]` -- the instruction as it sits in
 * memory (first halfword high), its length, `v` for a VLE page or `b`
 * for Book E, and an optional address. Each output line is the text the
 * disassembler prints and, after a tab, every field the decoder filled
 * in. tests/ppc-check compares the second column across an assembler
 * round trip, so that two encodings of one instruction -- an SCI8
 * immediate has several -- count as the same answer.
 */

#include "ppc/ppc_decode.h"
#include "ppc/ppc_disasm.h"

#include <stdio.h>
#include <string.h>

int main(void)
{
    char line[128];

    while (fgets(line, sizeof(line), stdin) != NULL) {
        unsigned w = 0u;
        unsigned len = 0u;
        unsigned pc = 0u;
        char mode = 'v';
        char text[128];
        ppc_insn_t d;

        if (sscanf(line, "%x %u %c %x", &w, &len, &mode, &pc) < 3) {
            continue;
        }
        ppc_disasm_mode(text, sizeof(text), pc, w, len, mode == 'v');
        ppc_decode(w, len, mode == 'v', &d);
        printf("%s\t%s s%u d%u a%u b%u rc%u oe%u cf%u cs%u bo%u bi%u aa%u lk%u "
               "sh%u mb%u me%u sz%u sx%u rv%u u%u x%u z%u i%08x\n",
               text, ppc_mn_name(d.id), d.sem, d.rd, d.ra, d.rb, d.rc, d.oe, d.crf,
               d.crs, d.bo, d.bi, d.aa, d.lk, d.sh, d.mb, d.me, d.size, d.sext,
               d.rev, d.upd, d.idx, d.ra0, (unsigned)d.imm);
    }
    return 0;
}
