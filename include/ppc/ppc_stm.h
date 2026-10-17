/* SPDX-License-Identifier: Apache-2.0 */
/*
 * ppc_stm.h - the MPC5675K's System Timer Module.
 *
 * MPC5675K Reference Manual, Rev. 10 (docs/nxp), chapter 51: a 32-bit
 * up-counter on the system clock divided by 1 to 256, and four compare
 * channels, each setting its flag and requesting an interrupt when the
 * counter reaches its compare value. In decoupled parallel mode each
 * core has its own: STM_0 at 0xFFF3_C000 and STM_1 at 0x8FF3_C000,
 * requesting through INTC_0 and INTC_1 as sources 30 to 33 (table
 * 32-10).
 *
 * **The system clock is the core's clock here**, one cycle per
 * instruction -- the clock the time base counts when HID0 does not
 * select p_tbclk. So the counter moves with the core it belongs to, a
 * match is taken at the instruction it falls on, and a core in `wait`
 * is carried to its next match as it is to its decrementer
 * (ppc_cpu_wake). Reading STM_CNT syncs the core's clock first, as
 * reading the time base does.
 *
 * Not modelled: FRZ, which stops the counter in debug mode -- the
 * emulator's debugger stops guest time altogether -- and lockstep mode,
 * where STM_0 serves both cores.
 */
#ifndef PPC_STM_H
#define PPC_STM_H

#include "emu/emu_dev.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PPC_STM0_BASE 0xFFF3C000u
#define PPC_STM1_BASE 0x8FF3C000u
#define PPC_STM_SIZE 0x4000u /* table 2-2: 16 KiB, reserved past 0x4C */

#define PPC_STM_CHANNELS 4u
/* Table 32-10: the INTC sources of channels 0 to 3. */
#define PPC_STM_IRQ0 30u

/* Table 51-2. */
#define PPC_STM_CR 0x00u
#define PPC_STM_CNT 0x04u
#define PPC_STM_CCR(n) (0x10u + 0x10u * (n))
#define PPC_STM_CIR(n) (0x14u + 0x10u * (n))
#define PPC_STM_CMP(n) (0x18u + 0x10u * (n))

/*
 * Figure 51-1, in the manual's big-endian bit numbering: CPS is bits
 * 16:23, FRZ 30 and TEN 31 -- so in the number software writes, CPS is
 * 0x0000FF00, FRZ 0x2 and TEN 0x1.
 */
#define PPC_STM_CR_CPS_SHIFT 8u
#define PPC_STM_CR_CPS 0x0000FF00u
#define PPC_STM_CR_FRZ 0x00000002u
#define PPC_STM_CR_TEN 0x00000001u
#define PPC_STM_CCR_CEN 0x00000001u
#define PPC_STM_CIR_CIF 0x00000001u

struct ppc_cpu;
struct ppc_intc;

typedef struct ppc_stm {
    uint32_t cr;
    uint32_t cnt;
    /* System-clock ticks counted towards the next increment, < CPS + 1. */
    uint32_t presc;
    uint32_t ccr[PPC_STM_CHANNELS];
    uint32_t cir[PPC_STM_CHANNELS];
    uint32_t cmp[PPC_STM_CHANNELS];
    /* The core whose clock it counts and whose next stop it sets. */
    struct ppc_cpu *core;
    /* Where its four requests go, or NULL. */
    struct ppc_intc *intc;
} ppc_stm_t;

/* Reset values (table 51-2, all zero), and no stop owed to the core. */
void ppc_stm_reset(ppc_stm_t *s);

/*
 * `ticks` of the system clock have passed. Sets the flag of every
 * enabled channel whose compare value the counter reached on the way --
 * reached, not landed on: a slice is many ticks, as for the decrementer.
 */
void ppc_stm_advance(ppc_stm_t *s, uint32_t ticks);

/*
 * System-clock ticks until the next match that would set a flag, or
 * 0xFFFFFFFF for none within that many. A channel whose flag is already
 * set is not counted -- matching again changes nothing.
 */
uint32_t ppc_stm_until(const ppc_stm_t *s);

/*
 * PPC_STM_SIZE bytes at the module's base. Word accesses only: anything
 * else, and any reserved offset, is a bus error, as 51.3 says.
 */
extern const emu_dev_ops_t ppc_stm_ops;

#ifdef __cplusplus
}
#endif

#endif /* PPC_STM_H */
