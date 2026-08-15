/* SPDX-License-Identifier: Apache-2.0 */
/*
 * g4mh_tptm.c - TPTM, the time-protection timers. U2B manual section 3.7.
 *
 * One timer set per PE, and the set is the interesting part: two
 * interval timers (down-counting, reloaded from ILD, underflow raises
 * one shared interrupt), a free-run timer (up-counting, no interrupt),
 * and two up timers, each with four compare values and a separate
 * interrupt per value. Each of the three groups has its own divider off
 * cpu_clk.
 *
 * Time comes from the same place the OSTM's does -- the platform calls
 * advance_time once per run slice -- which is why every divider carries
 * a remainder. A slice is thousands of ticks, so dropping the remainder
 * at each boundary would not be a rounding error, it would be a clock
 * running slow by a factor of the slice length.
 *
 * What is *not* here, and would be a second piece of work each:
 *
 *   the global up-timer control channels (TPTMGgURUN and friends). They
 *   are a separate address block letting one PE start every PE's up
 *   timers together, selected by UTRG.GTRGEN. UTRG is stored and read
 *   back, and setting GTRGEN does not detach the counter from the local
 *   run registers here -- it would have to, once those channels exist.
 *
 *   the debug-mode counter stop signals, which need a debug mode.
 *
 * The interval interrupt goes to FEINT or EIINT31 depending on TPTMSEL,
 * which lives in the INTC because that is where the manual puts it
 * (section 6.3.15) and because the FE path is the INTC's to deliver.
 */

#include "g4mh/g4mh_intercpu.h"
#include "g4mh/g4mh_intc.h"

#include <string.h>

/*
 * UIEN and UCSTR put up timer 0's four bits at 3:0 and up timer 1's at
 * 11:8, so the bit for (timer m, compare i) is 8*m + i. Named once
 * because it is needed in three places, and two of them writing it out
 * is how the halves come to disagree -- the same shape as the G4MH FP
 * field split that only cond=15 could settle.
 */
#define U_BIT(m, i)     ((unsigned)(8u * (unsigned)(m) + (unsigned)(i)))

void g4mh_tptm_init(g4mh_tptm_t *t)
{
    memset(t, 0, sizeof(*t));
}

void g4mh_tptm_bind(g4mh_tptm_t *t, unsigned pe, struct g4mh_intc *ic)
{
    if (pe < G4MH_PE_COUNT) {
        t->intc[pe] = ic;
    }
}

/*
 * How many counter steps `ticks` of cpu_clk buys at divider `div`,
 * carrying the remainder in *acc.
 *
 * count_clock = cpu_clk / (div + 1), so div 0 is every tick.
 */
static uint32_t steps(uint32_t ticks, uint8_t div, uint32_t *acc)
{
    const uint32_t period = (uint32_t)div + 1u;
    uint64_t total;

    if (period == 1u) {
        return ticks;
    }
    total = (uint64_t)*acc + ticks;
    *acc  = (uint32_t)(total % period);
    return (uint32_t)(total / period);
}

/* The interval timers' shared interrupt, TPTM_IRQ[n]. */
static void tptm_interval_irq(g4mh_tptm_t *t, unsigned pe)
{
    if (pe < G4MH_PE_COUNT && t->intc[pe] != NULL) {
        g4mh_intc_raise_tptm(t->intc[pe], pe);
    }
}

static void tptm_advance_pe(g4mh_tptm_t *t, unsigned pe, uint32_t ticks)
{
    g4mh_tptm_pe_t *p = &t->pe[pe];

    /* ---- interval timers: down, reload, underflow ------------------ */
    if (p->irun != 0u) {
        const uint32_t n = steps(ticks, p->idiv, &p->iacc);

        for (unsigned m = 0; m < G4MH_TPTM_INTERVALS; m++) {
            if (((p->irun >> m) & 1u) == 0u || n == 0u) {
                continue;
            }
            /*
             * A period is ILD + 1 counts, because the underflow is the
             * step *from* zero rather than the arrival at it. Modelled
             * by counting down through zero and reloading, which also
             * gets a long slice right: several periods can elapse
             * inside one call, and the flag is a flag rather than a
             * count, so it is set once either way.
             */
            const uint32_t period = p->ild[m] + 1u;
            uint32_t left = n;

            if (left > p->icnt[m]) {
                left -= p->icnt[m] + 1u;
                p->iustr |= (uint8_t)(1u << m);
                p->icnt[m] = p->ild[m] - (left % period);
            } else {
                p->icnt[m] -= left;
            }
        }
        if ((p->iustr & p->iien) != 0u) {
            tptm_interval_irq(t, pe);
        }
    }

    /* ---- free-run timer: up, wrapping, no interrupt ---------------- */
    if ((p->frun & 1u) != 0u) {
        p->fcnt += steps(ticks, p->fdiv, &p->facc);
    }

    /* ---- up timers: up, four compare values each ------------------- */
    if (p->urun != 0u) {
        const uint32_t n = steps(ticks, p->udiv, &p->uacc);

        for (unsigned m = 0; m < G4MH_TPTM_UPTIMERS; m++) {
            if (((p->urun >> m) & 1u) == 0u || n == 0u) {
                continue;
            }
            {
                const uint32_t from = p->ucnt[m];
                const uint32_t to   = from + n;

                p->ucnt[m] = to;

                for (unsigned i = 0; i < G4MH_TPTM_COMPARES; i++) {
                    const uint32_t cmp = p->ucmp[m][i];
                    const unsigned bit = U_BIT(m, i);
                    bool hit;

                    /*
                     * The counter passes cmp somewhere inside this
                     * slice, so the test is on the interval and not on
                     * equality -- with a divider or a long slice the
                     * counter steps over the compare value and an
                     * `==` would miss every match. Written to survive
                     * the wrap, which is why it is two comparisons.
                     */
                    hit = (to < from) ? (cmp > from || cmp <= to)
                                      : (cmp > from && cmp <= to);
                    if (!hit) {
                        continue;
                    }

                    /*
                     * UICFG: with it set, a new interrupt is raised only
                     * while *no* comparison flag of either up timer is
                     * set. The flag itself is set regardless -- the bit
                     * gates the interrupt, not the status.
                     */
                    if (((p->uicfg & 1u) == 0u) || p->ucstr == 0u) {
                        if (((p->uien >> bit) & 1u) != 0u &&
                            pe < G4MH_PE_COUNT && t->intc[pe] != NULL) {
                            g4mh_intc_raise(t->intc[pe],
                                            G4MH_TPTM_U_CHANNEL(pe, m, i));
                        }
                    }
                    p->ucstr |= (uint16_t)(1u << bit);
                }
            }
        }
    }
}

void g4mh_tptm_advance(g4mh_tptm_t *t, uint32_t ticks)
{
    if (ticks == 0u) {
        return;
    }
    for (unsigned pe = 0; pe < G4MH_PE_COUNT; pe++) {
        tptm_advance_pe(t, pe, ticks);
    }
}

/* ------------------------------------------------------------------ */
/* Registers                                                           */
/* ------------------------------------------------------------------ */

static emu_fault_t tptm_read(void *ctx, uint32_t off, uint32_t size,
                             uint32_t *out)
{
    const g4mh_intercpu_port_t *port = (const g4mh_intercpu_port_t *)ctx;
    const g4mh_tptm_t *t = (const g4mh_tptm_t *)port->state;
    const g4mh_tptm_pe_t *p;
    unsigned pe;
    uint32_t r;

    (void)size;
    *out = 0u;

    if (off < G4MH_TPTM_BLOCK) {
        pe = port->pe;                          /* the self block       */
        r  = off;
    } else {
        pe = (off - G4MH_TPTM_BLOCK) / G4MH_TPTM_BLOCK;
        r  = (off - G4MH_TPTM_BLOCK) % G4MH_TPTM_BLOCK;
    }
    if (pe >= G4MH_PE_COUNT) {
        return EMU_FAULT_NONE;
    }
    p = &t->pe[pe];

    switch (r) {
    case G4MH_TPTM_ISTR:  *out = p->irun;  break;
    case G4MH_TPTM_IIEN:  *out = p->iien;  break;
    case G4MH_TPTM_IUSTR: *out = p->iustr; break;
    case G4MH_TPTM_IDIV:  *out = p->idiv;  break;
    case G4MH_TPTM_FSTR:  *out = p->frun;  break;
    case G4MH_TPTM_FDIV:  *out = p->fdiv;  break;
    case G4MH_TPTM_USTR:  *out = p->urun;  break;
    case G4MH_TPTM_UIEN:  *out = p->uien;  break;
    case G4MH_TPTM_UCSTR: *out = p->ucstr; break;
    case G4MH_TPTM_UDIV:  *out = p->udiv;  break;
    case G4MH_TPTM_UTRG:  *out = p->utrg;  break;
    case G4MH_TPTM_UICFG: *out = p->uicfg; break;
    case G4MH_TPTM_ICNT0: *out = p->icnt[0]; break;
    case G4MH_TPTM_ILD0:  *out = p->ild[0];  break;
    case G4MH_TPTM_ICNT1: *out = p->icnt[1]; break;
    case G4MH_TPTM_ILD1:  *out = p->ild[1];  break;
    case G4MH_TPTM_FCNT:  *out = p->fcnt;    break;
    case G4MH_TPTM_UCNT0: *out = p->ucnt[0]; break;
    case G4MH_TPTM_UCNT1: *out = p->ucnt[1]; break;
    default:
        if (r >= G4MH_TPTM_UCMP0(0) && r <= G4MH_TPTM_UCMP0(3)) {
            *out = p->ucmp[0][(r - G4MH_TPTM_UCMP0(0)) / 4u];
        } else if (r >= G4MH_TPTM_UCMP1(0) && r <= G4MH_TPTM_UCMP1(3)) {
            *out = p->ucmp[1][(r - G4MH_TPTM_UCMP1(0)) / 4u];
        }
        /* Everything else, including the write-only run registers,
         * reads as zero. The manual says so of each in turn. */
        break;
    }
    return EMU_FAULT_NONE;
}

static emu_fault_t tptm_write(void *ctx, uint32_t off, uint32_t size,
                              uint32_t val)
{
    g4mh_intercpu_port_t *port = (g4mh_intercpu_port_t *)ctx;
    g4mh_tptm_t *t = (g4mh_tptm_t *)port->state;
    g4mh_tptm_pe_t *p;
    unsigned pe;
    uint32_t r;

    (void)size;

    if (off < G4MH_TPTM_BLOCK) {
        pe = port->pe;
        r  = off;
    } else {
        pe = (off - G4MH_TPTM_BLOCK) / G4MH_TPTM_BLOCK;
        r  = (off - G4MH_TPTM_BLOCK) % G4MH_TPTM_BLOCK;
    }
    if (pe >= G4MH_PE_COUNT) {
        return EMU_FAULT_NONE;
    }
    p = &t->pe[pe];

    switch (r) {
    case G4MH_TPTM_IRUN:
        /* Start: load ILD into ICNT, then run. */
        for (unsigned m = 0; m < G4MH_TPTM_INTERVALS; m++) {
            if (((val >> m) & 1u) != 0u) {
                p->icnt[m] = p->ild[m];
                p->irun |= (uint8_t)(1u << m);
            }
        }
        break;

    case G4MH_TPTM_IRRUN:
        /*
         * Restart reloads without needing a stop first, and leaves a
         * stopped channel stopped -- which is what distinguishes it
         * from IRUN and is easy to collapse into the same code.
         */
        for (unsigned m = 0; m < G4MH_TPTM_INTERVALS; m++) {
            if (((val >> m) & 1u) != 0u && ((p->irun >> m) & 1u) != 0u) {
                p->icnt[m] = p->ild[m];
            }
        }
        break;

    case G4MH_TPTM_ISTP:
        p->irun &= (uint8_t)~(val & 0x3u);
        break;

    case G4MH_TPTM_IIEN:
        p->iien = (uint8_t)(val & 0x3u);
        /*
         * "TPTM_IRQ[n] will be asserted immediately after IIENm bit is
         * set if" the flag is already up -- the manual's own caution,
         * and the reason this is not just a store.
         */
        if ((p->iustr & p->iien) != 0u) {
            tptm_interval_irq(t, pe);
        }
        break;

    case G4MH_TPTM_IUSTR:
        /* Write 0 to clear; writing 1 is ignored. */
        p->iustr &= (uint8_t)(val & 0x3u);
        break;

    case G4MH_TPTM_IDIV:  p->idiv = (uint8_t)val; p->iacc = 0u; break;

    case G4MH_TPTM_FRUN:
        if ((val & 1u) != 0u) {
            p->fcnt = 0u;               /* start is from zero          */
            p->frun = 1u;
        }
        break;
    case G4MH_TPTM_FRRUN:
        if ((val & 1u) != 0u && p->frun != 0u) {
            p->fcnt = 0u;
        }
        break;
    case G4MH_TPTM_FSTP:  if ((val & 1u) != 0u) { p->frun = 0u; } break;
    case G4MH_TPTM_FDIV:  p->fdiv = (uint8_t)val; p->facc = 0u; break;

    case G4MH_TPTM_URUN:
        for (unsigned m = 0; m < G4MH_TPTM_UPTIMERS; m++) {
            if (((val >> m) & 1u) != 0u) {
                p->ucnt[m] = 0u;
                p->urun |= (uint8_t)(1u << m);
            }
        }
        break;
    case G4MH_TPTM_URRUN:
        for (unsigned m = 0; m < G4MH_TPTM_UPTIMERS; m++) {
            if (((val >> m) & 1u) != 0u && ((p->urun >> m) & 1u) != 0u) {
                p->ucnt[m] = 0u;
            }
        }
        break;
    case G4MH_TPTM_USTP:  p->urun &= (uint8_t)~(val & 0x3u); break;
    case G4MH_TPTM_UIEN:  p->uien  = (uint16_t)(val & 0x0F0Fu); break;
    case G4MH_TPTM_UCSTR: p->ucstr &= (uint16_t)(val & 0x0F0Fu); break;
    case G4MH_TPTM_UDIV:  p->udiv = (uint8_t)val; p->uacc = 0u; break;
    case G4MH_TPTM_UTRG:  p->utrg = (uint16_t)val; break;
    case G4MH_TPTM_UICFG: p->uicfg = (uint8_t)(val & 1u); break;

    case G4MH_TPTM_ICNT0: p->icnt[0] = val; break;
    case G4MH_TPTM_ILD0:  p->ild[0]  = val; break;
    case G4MH_TPTM_ICNT1: p->icnt[1] = val; break;
    case G4MH_TPTM_ILD1:  p->ild[1]  = val; break;
    case G4MH_TPTM_FCNT:  p->fcnt    = val; break;
    case G4MH_TPTM_UCNT0: p->ucnt[0] = val; break;
    case G4MH_TPTM_UCNT1: p->ucnt[1] = val; break;

    default:
        if (r >= G4MH_TPTM_UCMP0(0) && r <= G4MH_TPTM_UCMP0(3)) {
            p->ucmp[0][(r - G4MH_TPTM_UCMP0(0)) / 4u] = val;
        } else if (r >= G4MH_TPTM_UCMP1(0) && r <= G4MH_TPTM_UCMP1(3)) {
            p->ucmp[1][(r - G4MH_TPTM_UCMP1(0)) / 4u] = val;
        }
        break;
    }
    return EMU_FAULT_NONE;
}

const emu_dev_ops_t g4mh_tptm_ops = {
    .read  = tptm_read,
    .write = tptm_write,
};
