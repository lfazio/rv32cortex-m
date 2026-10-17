/* SPDX-License-Identifier: Apache-2.0 */
/*
 * ppc_swt.h - the MPC5675K's Software Watchdog Timer.
 *
 * MPC5675K Reference Manual, Rev. 10 (docs/nxp), chapter 52: a 32-bit
 * down-counter loaded with the timeout when the watchdog is enabled or
 * serviced. Expiring is a system reset, or with SWT_CR[ITR] an
 * interrupt the first time and a reset the second. Servicing is a
 * two-write sequence to SWT_SR, fixed or keyed, optionally only inside
 * a window; the configuration can be soft- or hard-locked; and an
 * invalid access is a bus error or, with SWT_CR[RIA], a reset. In
 * decoupled parallel mode each core has one: SWT_0 at 0xFFF3_8000 and
 * SWT_1 at 0x8FF3_8000, whose timeouts are INTC sources 28 and 29.
 *
 * **A system reset stops the core** whose watchdog it was, and the run
 * says why (emu_cpu_status_t::reset_by). That is what ARMv7-M's
 * SYSRESETREQ does too: this emulator has no reset generation module,
 * and a guest that restarted silently would hide the very event a
 * watchdog test is about.
 *
 * Choices, each stated where it is made (ppc_swt.c): the watchdog is
 * off out of reset, as the flash option that enables it can say; both
 * of SWT_CR[CSL]'s clocks are the processor clock; MAPn, STP and FRZ
 * are held and do nothing; and each SWT requests through its own core's
 * INTC, where on the part table 32-10 gives both timeouts to both.
 */
#ifndef PPC_SWT_H
#define PPC_SWT_H

#include "emu/emu_dev.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PPC_SWT0_BASE 0xFFF38000u
#define PPC_SWT1_BASE 0x8FF38000u
#define PPC_SWT_SIZE 0x4000u /* table 2-2 */
#define PPC_SWT_IRQ0 28u /* table 32-10: SWT_0's; SWT_1's is 29 */

/* Table 52-2. */
#define PPC_SWT_CR 0x00u
#define PPC_SWT_IR 0x04u
#define PPC_SWT_TO 0x08u
#define PPC_SWT_WN 0x0Cu
#define PPC_SWT_SR 0x10u
#define PPC_SWT_CO 0x14u
#define PPC_SWT_SK 0x18u

/* Figure 52-1, in the number software writes. */
#define PPC_SWT_CR_MAP 0xFF000000u
#define PPC_SWT_CR_KEY 0x00000200u
#define PPC_SWT_CR_RIA 0x00000100u
#define PPC_SWT_CR_WND 0x00000080u
#define PPC_SWT_CR_ITR 0x00000040u
#define PPC_SWT_CR_HLK 0x00000020u
#define PPC_SWT_CR_SLK 0x00000010u
#define PPC_SWT_CR_CSL 0x00000008u
#define PPC_SWT_CR_STP 0x00000004u
#define PPC_SWT_CR_FRZ 0x00000002u
#define PPC_SWT_CR_WEN 0x00000001u
#define PPC_SWT_CR_WRITABLE 0xFF0003FFu /* the rest reads as zero */
#define PPC_SWT_IR_TIF 0x00000001u

/* Table 52-2's reset values -- CR with WEN clear; see ppc_swt.c. */
#define PPC_SWT_CR_RESET 0xFF00011Au
#define PPC_SWT_TO_RESET 0x0003FDE0u
/* 52.3.2.3: the counter is loaded with WTO or 0x100, whichever is larger. */
#define PPC_SWT_TO_MIN 0x100u

/* Table 52-7: the fixed service sequence, and the soft-lock's unlock. */
#define PPC_SWT_SERVICE1 0xA602u
#define PPC_SWT_SERVICE2 0xB480u
#define PPC_SWT_UNLOCK1 0xC520u
#define PPC_SWT_UNLOCK2 0xD928u

struct ppc_cpu;
struct ppc_intc;

typedef struct ppc_swt {
    uint32_t cr;
    uint32_t ir;
    uint32_t to;
    uint32_t wn;
    uint32_t sk;
    uint32_t count; /* the internal down counter */
    bool expired; /* an initial timeout has interrupted: the next resets */
    bool svc_half; /* the first write of a service sequence was made */
    bool unl_half; /* 0xC520 was the last unlock write */
    bool reset; /* a system reset was requested */
    uint32_t index; /* 0 for SWT_0, 1 for SWT_1 */
    struct ppc_cpu *core; /* whose clock it counts and whom it resets */
    struct ppc_intc *intc; /* where its timeout interrupt goes, or NULL */
} ppc_swt_t;

/* Reset values; `index` and the links are kept. */
void ppc_swt_reset(ppc_swt_t *w);

/* `ticks` of the processor clock have passed. */
void ppc_swt_advance(ppc_swt_t *w, uint32_t ticks);

/* Ticks until the counter expires, or 0xFFFFFFFF when it will not. */
uint32_t ppc_swt_until(const ppc_swt_t *w);

/*
 * PPC_SWT_SIZE bytes at the module's base. Word accesses only, as 52.3
 * says, and every invalid access is a bus error or a reset.
 */
extern const emu_dev_ops_t ppc_swt_ops;

#ifdef __cplusplus
}
#endif

#endif /* PPC_SWT_H */
