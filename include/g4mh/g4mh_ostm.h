/* SPDX-License-Identifier: Apache-2.0 */
/*
 * g4mh_ostm.h - the OS timer, RH850/U2B section 34.
 *
 * One 32-bit counter per unit on PCLK (CLKC_HSB, 80 MHz), counting down
 * from OSTMnCMP in interval mode or up from zero in free-run compare
 * mode, with an interrupt on reaching zero or on matching. The U2B6 has
 * three units, OSTM0 to OSTM2 -- "one per core" in the sense that an
 * operating system on each PE takes one, though every unit's interrupt
 * can go to any PE: they are INTC2 channels 360, 361 and 841, and EIBD
 * decides who takes them.
 *
 * **Time is computed, not ticked.** The counter's value is a function of
 * the time since it was started, so a read is exact wherever it falls
 * between two of the platform's time updates, and an interrupt is due at
 * a time that can be calculated rather than discovered by stepping the
 * counter. The platform's clock is the LTSC's -- microseconds -- and the
 * same factor of 80 converts it.
 *
 * What is modelled and what is not:
 *
 *   - interval and free-run compare mode, the start interrupt (MD0), the
 *     no-reload start (MD2), the forced restart in interval mode, and
 *     CMP rewritten while counting;
 *   - OSTM0's output, as the bit software reads in OSTMnTO -- toggled on
 *     each interrupt in timer-output mode. Nothing outside it consumes
 *     the pin: the ECM it drives is not modelled.
 *   - not the PIC's simultaneous start (OSTMnTSST), nor the error
 *     notification to the ECM.
 *
 * Interrupts arrive at the platform's time resolution: if several are
 * due between two updates they are one edge, which EIOV would report as
 * an overrun had the guest not taken the first -- the same as a guest
 * that masked the channel for that long.
 */
#ifndef G4MH_OSTM_H
#define G4MH_OSTM_H

#include <stdbool.h>
#include <stdint.h>

#include "emu/emu_dev.h"

struct g4mh_intc;

/* Three units on the U2B6 (table 34.1); the larger parts have six. */
#define G4MH_OSTM_COUNT 3u

/* Table 34.3: even units in P-Bus group 5, odd ones in group 11H. */
#define G4MH_OSTM_BASE(n)                                                      \
    ((((n) & 1u) != 0u ? 0xFF89A100u : 0xFFBF0000u) + 0x200u * ((n) >> 1))
#define G4MH_OSTM_SIZE 0x100u

/* INTOSTMnTINT, table 34.5. */
#define G4MH_OSTM_CHANNEL(n)                                                   \
    ((n) < 2u ? 360u + (n) : ((n) < 4u ? 839u + (n) : 964u + (n)))

#define G4MH_OSTM_CMP 0x00u /* 32 */
#define G4MH_OSTM_CNT 0x04u /* 32, writable while stopped */
#define G4MH_OSTM_TO 0x08u  /* 8 */
#define G4MH_OSTM_TOE 0x0Cu /* 8 */
#define G4MH_OSTM_TE 0x10u  /* 8, read-only */
#define G4MH_OSTM_TS 0x14u  /* 8, write-only */
#define G4MH_OSTM_TT 0x18u  /* 8, write-only */
#define G4MH_OSTM_CTL 0x20u /* 8, writable while stopped */

#define G4MH_OSTM_CTL_IE 0x80u  /* the interrupt is enabled          */
#define G4MH_OSTM_CTL_MD2 0x04u /* do not load the counter at start  */
#define G4MH_OSTM_CTL_MD1 0x02u /* free-run compare, not interval    */
#define G4MH_OSTM_CTL_MD0 0x01u /* interrupt at count start too      */
#define G4MH_OSTM_CTL_WRITABLE 0x87u

/* PCLK and the platform's clock, as the LTSC has them. */
#define G4MH_OSTM_HZ 80000000u
#define G4MH_OSTM_PLATFORM_HZ 1000000u

typedef struct g4mh_ostm {
    struct g4mh_intc *intc; /* where the interrupt goes: the global unit */
    uint32_t channel;

    uint32_t cmp;
    uint32_t ctl;
    uint8_t to;
    uint8_t toe;
    bool running;

    /*
     * The counter at `base` (in PCLK counts) was `value`. While stopped
     * `value` is the counter; while running the current one is computed
     * from the time since `base`.
     */
    uint32_t value;
    uint64_t base;

    /*
     * Nothing has been written to CNT or started since reset, so the
     * counter reads the current mode's start value (table 34.12).
     */
    bool pristine;

    /*
     * Events since `base` already accounted for -- the zeros or the
     * matches -- so an update raises only for the ones after them.
     */
    uint64_t seen;

    uint64_t now; /* the platform's time, converted, at the last update */
} g4mh_ostm_t;

void g4mh_ostm_init(g4mh_ostm_t *t, struct g4mh_intc *intc, uint32_t channel);

/* The platform's time: absolute, in its microseconds, or a delta. */
void g4mh_ostm_set_time(g4mh_ostm_t *t, uint64_t now);
void g4mh_ostm_advance(g4mh_ostm_t *t, uint32_t ticks);

/* A context is one g4mh_ostm_t, mapped at G4MH_OSTM_BASE(n). */
extern const emu_dev_ops_t g4mh_ostm_ops;

#endif /* G4MH_OSTM_H */
