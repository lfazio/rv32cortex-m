/* SPDX-License-Identifier: Apache-2.0 */
/*
 * ppc_fpu.c - the e200z7's embedded FP unit, one operation per line.
 *
 * For tests/ppc-check/efpu.py, which holds the other opinion: it reads
 *
 *     <mnemonic> <spefscr> <rA> <rB> <rD>
 *
 * (hex but for the name), runs ppc_fpu_exec, and prints rD, SPEFSCR,
 * what it returned (0, or the IVOR of the interrupt it asks for) and
 * the CR field a compare wrote. Nothing here decides whether an answer
 * is right.
 */
#include "ppc/ppc_cpu.h"
#include "ppc/ppc_decode.h"
#include "ppc/ppc_fpu.h"

#include <stdio.h>
#include <string.h>

int main(void)
{
    static ppc_cpu_t c;
    char name[32];
    unsigned spefscr;
    unsigned a;
    unsigned b;
    unsigned d;

    while (scanf("%31s %x %x %x %x", name, &spefscr, &a, &b, &d) == 5) {
        uint32_t sem = PPC_S_COUNT;
        uint32_t ret;

        for (uint32_t id = 0u; id < PPC_M_COUNT; id++) {
            /* The semantics are the decoder's own, found by the name
             * the assembler uses. */
            if (strcmp(ppc_mn_name(id), name) == 0) {
                sem = ppc_mn_sem(id);
                break;
            }
        }
        if (sem == PPC_S_COUNT) {
            printf("? unknown %s\n", name);
            continue;
        }
        c.spefscr = spefscr;
        c.cr = 0u;
        c.r[3] = d;
        c.r[4] = a;
        c.r[5] = b;
        ret = ppc_fpu_exec(&c, sem, 3u, 4u, 5u, 2u);
        printf("%08x %08x %u %x\n", (unsigned)c.r[3], (unsigned)c.spefscr,
               (ret == PPC_EXC_NONE) ? 0u : (unsigned)ret, (unsigned)((c.cr >> 20) & 15u));
    }
    return 0;
}
