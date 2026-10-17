/* SPDX-License-Identifier: Apache-2.0 */
/*
 * g4mh_wdtb.h - the window watchdog timer, RH850/U2B section 33.
 *
 * One per PE on the U2B6, WDTB0 to WDTB2 (table 33.9 pairs WDTBn with
 * CPUn). A 16-bit counter on CLK_WDTB, 10 MHz, restarted by writing an
 * activation code to the trigger register; it overflows after 2^9 to
 * 2^16 counts, interrupts at 75% of that or at WIS, and may only be
 * triggered while its window is open.
 *
 * **An error goes to the ECM, not to a core.** Overflow, a wrong
 * activation code, a trigger outside the window and a changed setting
 * all assert WDTBnTRES, and the Error Control Module decides what
 * happens: an EI or FE level interrupt to the cores it is configured
 * for, an internal reset, both or neither. WDTBnTNMI, which NMI mode
 * adds, goes to the PIC's Hi-Z control for the motor timers' outputs,
 * not to an interrupt controller. So the device reports the error
 * through `error`, and the frontend applies the ECM's configuration --
 * which, the ECM not being modelled, is the one it has out of reset:
 * ECMIRCFG7 = 0000 0010H, error 228 -- WDTB0's -- is an internal
 * reset, and nothing else is enabled.
 *
 * The option bytes choose the start mode and the activation code; they
 * are not modelled either, so g4mh_wdtb_options_t carries them, and the
 * frontend uses the software trigger start and the fixed code 0xAC.
 *
 * After an error the counter "does not change until internal reset is
 * generated": the unit is latched and ignores everything until it is
 * reset.
 */
#ifndef G4MH_WDTB_H
#define G4MH_WDTB_H

#include <stdbool.h>
#include <stdint.h>

#include "emu/emu_dev.h"

struct g4mh_intc;

/* Three units on the U2B6 (table 33.1). */
#define G4MH_WDTB_COUNT 3u

/* Table 33.3: even units in P-Bus group 5, odd ones in group 11. */
#define G4MH_WDTB_BASE(n)                                                      \
    ((((n) & 1u) != 0u ? 0xFF899000u : 0xFFBF1000u) + 0x200u * ((n) >> 1))
#define G4MH_WDTB_SIZE 0x100u

/* INTWDTBnTIT is interrupt 22 for every unit, on its own PE's INTC1. */
#define G4MH_WDTB_CHANNEL 22u

/* Its error, as the ECM numbers it (table 33.6). */
#define G4MH_WDTB_ECM_ERROR(n) (228u + 32u * (n))

#define G4MH_WDTB_WDTE 0x00u /* 8: the trigger, fixed code    */
#define G4MH_WDTB_EVAC 0x04u /* 8: the trigger, varying code  */
#define G4MH_WDTB_REF 0x08u  /* 8: VAC reference, read-only   */
#define G4MH_WDTB_MD 0x0Cu   /* 8 */
#define G4MH_WDTB_WOST 0x10u /* 16 */
#define G4MH_WDTB_WIS 0x14u  /* 16 */

#define G4MH_WDTB_MD_WMS 0x80u /* window and interrupt by WOST/WIS */
#define G4MH_WDTB_MD_OVF_SHIFT 4u
#define G4MH_WDTB_MD_OVF_MASK 0x70u /* overflow at 2^(9 + OVF) counts */
#define G4MH_WDTB_MD_WIE 0x08u      /* WDTBnTIT enabled                */
#define G4MH_WDTB_MD_ERM 0x04u      /* reset mode, not NMI mode        */
#define G4MH_WDTB_MD_WS_MASK 0x03u  /* window open 25, 50, 75, 100%    */

/* Table 33.20, the software trigger start's reset settings. */
#define G4MH_WDTB_MD_RESET 0x7Fu
#define G4MH_WDTB_WOST_RESET 0x0010u

#define G4MH_WDTB_CODE 0xACu /* the fixed activation code           */
#define G4MH_WDTB_IDLE 0x2Cu /* what a trigger register reads, run 0 */

#define G4MH_WDTB_HZ 10000000u
#define G4MH_WDTB_PLATFORM_HZ 1000000u

/* The option bytes this unit reads at reset: OPWDRUNn and OPWDVAC. */
typedef struct g4mh_wdtb_options {
    bool default_start; /* counting from reset, settings fixed */
    bool vac;           /* the varying activation code         */
} g4mh_wdtb_options_t;

/* An error, as the ECM would see it: WDTBnTRES from unit `unit`. */
typedef void (*g4mh_wdtb_error_fn)(void *ctx, unsigned unit);

typedef struct g4mh_wdtb {
    unsigned unit;
    struct g4mh_intc *intc; /* this unit's PE's controller, or NULL */
    g4mh_wdtb_error_fn error;
    void *error_ctx;
    bool vac;
    bool default_start;

    bool running;
    bool failed; /* an error was reported: latched until reset */

    uint8_t md;
    uint16_t wost;
    uint16_t wis;
    /*
     * Writes to each setting since reset. Before the first trigger the
     * first is free and a second may only repeat it; once counting,
     * any change is an error (table 33.22).
     */
    uint8_t md_writes;
    uint8_t wost_writes;
    uint8_t wis_writes;

    uint8_t ref;  /* VAC: the reference the next code is computed from */
    uint8_t evac; /* VAC: the last code written, which EVAC reads      */
    bool evac_written;

    uint64_t base; /* in counts: the last trigger                      */
    bool tit_done; /* this period's interrupt has been raised         */
    uint64_t now;
} g4mh_wdtb_t;

void g4mh_wdtb_init(g4mh_wdtb_t *w, unsigned unit, struct g4mh_intc *intc,
                    const g4mh_wdtb_options_t *opts, g4mh_wdtb_error_fn error,
                    void *error_ctx);

void g4mh_wdtb_set_time(g4mh_wdtb_t *w, uint64_t now);
void g4mh_wdtb_advance(g4mh_wdtb_t *w, uint32_t ticks);

/* A context is one g4mh_wdtb_t, mapped at G4MH_WDTB_BASE(n). */
extern const emu_dev_ops_t g4mh_wdtb_ops;

#endif /* G4MH_WDTB_H */
