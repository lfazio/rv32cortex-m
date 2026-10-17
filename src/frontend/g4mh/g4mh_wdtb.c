/* SPDX-License-Identifier: Apache-2.0 */
/*
 * g4mh_wdtb.c - the window watchdog timer: see g4mh_wdtb.h.
 *
 * Like the OS timer, the counter is a function of the time since the
 * last trigger rather than something stepped: an update compares that
 * elapsed time with the two moments that matter, the interrupt and the
 * overflow.
 */

#include "g4mh/g4mh_wdtb.h"
#include "g4mh/g4mh_intc.h"

static uint64_t counts(uint64_t platform)
{
    return (platform * (uint64_t)G4MH_WDTB_HZ) / (uint64_t)G4MH_WDTB_PLATFORM_HZ;
}

/* The overflow interval: 2^9 counts at OVF = 0, up to 2^16. */
static uint64_t overflow(const g4mh_wdtb_t *w)
{
    return 1ull << (9u + ((w->md & G4MH_WDTB_MD_OVF_MASK) >> G4MH_WDTB_MD_OVF_SHIFT));
}

/* When WDTBnTIT is due: 75% of the interval, or WIS. */
static uint64_t interrupt_at(const g4mh_wdtb_t *w)
{
    if ((w->md & G4MH_WDTB_MD_WMS) != 0u) {
        return w->wis;
    }
    return overflow(w) * 3u / 4u;
}

/*
 * Where the window opens: from WOST, or with WS selecting how much of
 * the interval at its end is open -- 25%, 50%, 75% or all of it.
 */
static uint64_t window_opens(const g4mh_wdtb_t *w)
{
    if ((w->md & G4MH_WDTB_MD_WMS) != 0u) {
        return w->wost;
    }
    return overflow(w) * (3u - (w->md & G4MH_WDTB_MD_WS_MASK)) / 4u;
}

/* WDTBnTRES: latch, and tell the ECM. */
static void fail(g4mh_wdtb_t *w)
{
    if (w->failed) {
        return;
    }
    w->failed = true;
    if (w->error != NULL) {
        w->error(w->error_ctx, w->unit);
    }
}

static void update(g4mh_wdtb_t *w)
{
    if (!w->running || w->failed) {
        return;
    }
    const uint64_t e = w->now - w->base;

    if (!w->tit_done && e >= interrupt_at(w)) {
        w->tit_done = true;
        if ((w->md & G4MH_WDTB_MD_WIE) != 0u && w->intc != NULL) {
            g4mh_intc_raise(w->intc, G4MH_WDTB_CHANNEL);
        }
    }
    if (e >= overflow(w)) {
        fail(w);
    }
}

void g4mh_wdtb_init(g4mh_wdtb_t *w, unsigned unit, struct g4mh_intc *intc,
                    const g4mh_wdtb_options_t *opts, g4mh_wdtb_error_fn error,
                    void *error_ctx)
{
    w->unit = unit;
    w->intc = intc;
    w->error = error;
    w->error_ctx = error_ctx;
    w->vac = opts->vac;
    w->default_start = opts->default_start;
    w->failed = false;
    w->md = G4MH_WDTB_MD_RESET;
    w->wost = G4MH_WDTB_WOST_RESET;
    w->wis = 0u;
    w->md_writes = 0u;
    w->wost_writes = 0u;
    w->wis_writes = 0u;
    w->ref = 0u;
    w->evac = 0u;
    w->evac_written = false;
    w->base = 0u;
    w->tit_done = false;
    w->now = 0u;
    /* In default start mode the counter runs from the reset release. */
    w->running = opts->default_start;
}

void g4mh_wdtb_set_time(g4mh_wdtb_t *w, uint64_t now)
{
    w->now = counts(now);
    update(w);
}

void g4mh_wdtb_advance(g4mh_wdtb_t *w, uint32_t ticks)
{
    w->now += counts(ticks);
    update(w);
}

/*
 * A write of the activation code to whichever register the option byte
 * chose. The first one starts the counter, with the settings written so
 * far; later ones restart it, and only inside the window.
 */
static void trigger(g4mh_wdtb_t *w, uint8_t code)
{
    bool right;

    update(w);
    if (w->failed) {
        return;
    }
    if (w->vac) {
        /*
         * Expected = ACH - REF, and REF follows the *expected* value
         * rotated left by one, "even when the writing value ... is
         * different from the expected value" (33.4.6.2).
         */
        const uint8_t expected = (uint8_t)(G4MH_WDTB_CODE - w->ref);

        right = code == expected;
        w->ref = (uint8_t)((expected << 1) | (expected >> 7));
        w->evac = code;
        w->evac_written = true;
    } else {
        right = code == G4MH_WDTB_CODE;
    }
    if (!right) {
        fail(w);
        return;
    }
    if (w->running && w->now - w->base < window_opens(w)) {
        fail(w); /* the window is closed */
        return;
    }
    w->running = true;
    w->base = w->now;
    w->tit_done = false;
}

/*
 * MD, WOST and WIS: a second write before the first trigger, or any
 * write once counting, must repeat the value or it is an error (table
 * 33.22). In default start mode the counter runs from reset, so every
 * write is the "once counting" case.
 */
static bool setting(g4mh_wdtb_t *w, uint8_t *writes, uint32_t cur, uint32_t val)
{
    if (w->failed) {
        return false;
    }
    if (!w->running && *writes == 0u) {
        *writes = 1u;
        return true;
    }
    if (val != cur) {
        update(w);
        fail(w);
    }
    return false;
}

static emu_fault_t wdtb_read(void *ctx, uint32_t off, uint32_t size,
                             uint32_t *out)
{
    const g4mh_wdtb_t *w = (const g4mh_wdtb_t *)ctx;
    const uint32_t run = w->running ? 0x80u : 0u;

    (void)size;
    switch (off) {
    case G4MH_WDTB_WDTE:
        *out = w->vac ? G4MH_WDTB_IDLE : (G4MH_WDTB_IDLE | run);
        break;
    case G4MH_WDTB_EVAC:
        if (!w->vac) {
            *out = G4MH_WDTB_IDLE;
        } else {
            *out = w->evac_written ? w->evac : (G4MH_WDTB_IDLE | run);
        }
        break;
    case G4MH_WDTB_REF:
        *out = w->vac ? w->ref : 0u;
        break;
    case G4MH_WDTB_MD:
        *out = w->md;
        break;
    case G4MH_WDTB_WOST:
        *out = w->wost;
        break;
    case G4MH_WDTB_WIS:
        *out = w->wis;
        break;
    default:
        *out = 0u;
        break;
    }
    return EMU_FAULT_NONE;
}

static emu_fault_t wdtb_write(void *ctx, uint32_t off, uint32_t size,
                              uint32_t val)
{
    g4mh_wdtb_t *w = (g4mh_wdtb_t *)ctx;

    (void)size;
    switch (off) {
    case G4MH_WDTB_WDTE:
        if (!w->vac) { /* "access to WDTE is ignored" with VAC */
            trigger(w, (uint8_t)val);
        }
        break;
    case G4MH_WDTB_EVAC:
        if (w->vac) {
            trigger(w, (uint8_t)val);
        }
        break;
    case G4MH_WDTB_MD:
        if (setting(w, &w->md_writes, w->md, val & 0xFFu)) {
            w->md = (uint8_t)val;
        }
        break;
    case G4MH_WDTB_WOST:
        if (setting(w, &w->wost_writes, w->wost, val & 0xFFFFu)) {
            w->wost = (uint16_t)val;
        }
        break;
    case G4MH_WDTB_WIS:
        if (setting(w, &w->wis_writes, w->wis, val & 0xFFFFu)) {
            w->wis = (uint16_t)val;
        }
        break;
    default:
        break; /* REF is read-only */
    }
    return EMU_FAULT_NONE;
}

const emu_dev_ops_t g4mh_wdtb_ops = {
    .read = wdtb_read,
    .write = wdtb_write,
    .tick = NULL,
};
