/* SPDX-License-Identifier: Apache-2.0 */
/*
 * ppc_intc.h - the MPC5675K's interrupt controller, one per core.
 *
 * MPC5675K Reference Manual, Rev. 10 (docs/nxp), chapter 32. 337
 * sources -- 8 software-settable, vectors 0 to 7, and the peripherals
 * from 8 -- each with a priority from 0 to 15 in INTC_PSRn. The highest
 * asserted priority above the current one (INTC_CPR) requests an
 * interrupt from the core, ties going to the lowest vector; taking it
 * pushes the current priority onto a 14-deep LIFO and raises it to the
 * source's, and INTC_EOIR pops it back.
 *
 * Both handshakes are modelled. In **software vector mode** (BCR[HVEN]
 * clear) the core takes the external input through IVOR4 and the
 * handler reads INTC_IACKR, which is the acknowledgement: the read
 * pushes, raises and negates the request, and returns the vector table
 * entry's address. In **hardware vector mode** the core's taking the
 * interrupt is the acknowledgement, and it vectors to IVPR[0:15] ||
 * vector << 4 (e200z759n3 manual, table 7-16) -- ppc_intc_ack.
 *
 * In decoupled parallel mode, the default and the only mode modelled,
 * there are two of these, INTC_0 at 0xFFF4_8000 for core 0 and INTC_1
 * at 0x8FF4_8000 for core 1, each mapped on its own core's bus only.
 * Which other core may reach it on the part is not modelled; one that
 * reaches only its own needs no lock under EMU_SMP.
 */
#ifndef PPC_INTC_H
#define PPC_INTC_H

#include "emu/emu_dev.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PPC_INTC0_BASE 0xFFF48000u
#define PPC_INTC1_BASE 0x8FF48000u
#define PPC_INTC_SIZE 0x4000u /* table 2-2 */

#define PPC_INTC_NSRC 337u
#define PPC_INTC_NSW 8u /* software-settable: vectors 0 to 7 */
#define PPC_INTC_LIFO 14u /* 32.4.2.2 */
#define PPC_INTC_PRI_MAX 15u

/* Table 32-2. */
#define PPC_INTC_BCR 0x00u
#define PPC_INTC_CPR 0x08u
#define PPC_INTC_IACKR 0x10u
#define PPC_INTC_EOIR 0x18u
#define PPC_INTC_SSCIR0_3 0x20u
#define PPC_INTC_SSCIR4_7 0x24u
#define PPC_INTC_PSR0 0x40u /* one byte per source */

/* Figure 32-2: VTES is bit 26 and HVEN bit 31, big-endian numbering. */
#define PPC_INTC_BCR_VTES 0x00000020u
#define PPC_INTC_BCR_HVEN 0x00000001u
/* Figure 32-6: each SSCIR byte holds SETx at bit 6 and CLRx at bit 7. */
#define PPC_INTC_SSCIR_SET 0x02u
#define PPC_INTC_SSCIR_CLR 0x01u

struct ppc_cpu;

typedef struct ppc_intc {
    uint32_t bcr;
    uint32_t cpr; /* PRI: the current priority */
    uint32_t vtba; /* IACKR's writable bits */
    uint32_t intvec; /* the vector latched when the request asserted */
    uint32_t intpri; /* and its priority, which acknowledging installs */
    bool req; /* the interrupt request to the core */
    /*
     * 32.4.2.2: a push onto a full LIFO overwrites the oldest entry, and
     * a pop of an empty one gives 0 -- so a ring, and a depth that
     * saturates at its size.
     */
    uint8_t lifo[PPC_INTC_LIFO];
    uint32_t lifo_head; /* where the next push goes */
    uint32_t lifo_depth; /* entries a pop can return, at most PPC_INTC_LIFO */
    uint8_t psr[PPC_INTC_NSRC];
    uint32_t flag[(PPC_INTC_NSRC + 31u) / 32u]; /* asserted requests */
    struct ppc_cpu *core; /* the core it requests from, or NULL */
} ppc_intc_t;

/* Reset: every priority 0, CPR 15 (32.5.1), nothing asserted. */
void ppc_intc_reset(ppc_intc_t *ic);

/*
 * A peripheral's request, which is level: the source's flag in its own
 * module drives it, so it is asserted until the module clears it.
 * Sources below PPC_INTC_NSW are software-settable and ignore this.
 */
void ppc_intc_set_source(ppc_intc_t *ic, uint32_t source, bool level);

/* Hardware vector mode: BCR[HVEN]. */
bool ppc_intc_hw_vectored(const ppc_intc_t *ic);

/*
 * The core's acknowledgement, in hardware vector mode: push the current
 * priority, install the request's, negate the request, and return the
 * vector to branch on. Software vector mode ignores the core's
 * acknowledge signal (32.2.1.1); its acknowledgement is the IACKR read.
 */
uint32_t ppc_intc_ack(ppc_intc_t *ic);

/*
 * PPC_INTC_SIZE bytes at the module's base. Any access that stays
 * within one 32-bit word, as 32.3.2 allows; a reserved word is a bus
 * error.
 */
extern const emu_dev_ops_t ppc_intc_ops;

#ifdef __cplusplus
}
#endif

#endif /* PPC_INTC_H */
