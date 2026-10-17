/* SPDX-License-Identifier: Apache-2.0 */
/*
 * g4mh_ostm.c - the OS timer: see g4mh_ostm.h.
 *
 * While a unit runs, its counter is a function of the time since
 * `base`, and its interrupts are an arithmetic sequence of times from
 * there -- the zeros in interval mode, the matches in free-run compare
 * mode. So an update counts the terms of that sequence up to now, and
 * raises if the count moved; nothing steps through the counts between.
 */

#include "g4mh/g4mh_ostm.h"
#include "g4mh/g4mh_intc.h"

static uint64_t counts(uint64_t platform)
{
    return (platform * (uint64_t)G4MH_OSTM_HZ) / (uint64_t)G4MH_OSTM_PLATFORM_HZ;
}

static bool free_run(const g4mh_ostm_t *t)
{
    return (t->ctl & G4MH_OSTM_CTL_MD1) != 0u;
}

/*
 * The event sequence: the first at base + first, then every period.
 *
 * Interval: the counter reaches zero `value` counts after `base`, and
 * then every CMP + 1 -- the reload is a count of its own ("OSTMnTINT
 * generation cycle = Count-clock cycle x (OSTMnCMP + 1)").
 *
 * Free-run: the counter equals CMP after (CMP - value) mod 2^32 counts,
 * and again every 2^32. Zero is a match: with CMP equal to the start
 * value the first one is at the start, which is figure 34.12's
 * "generated over two clock cycles" when MD0 adds its own.
 */
static void sequence(const g4mh_ostm_t *t, uint64_t *first, uint64_t *period)
{
    if (free_run(t)) {
        *first = (uint32_t)(t->cmp - t->value);
        *period = 1ull << 32;
    } else {
        *first = t->value;
        *period = (uint64_t)t->cmp + 1u;
    }
}

/* Events at or before `at`, counted from `base`. */
static uint64_t events_upto(const g4mh_ostm_t *t, uint64_t at)
{
    uint64_t first;
    uint64_t period;

    sequence(t, &first, &period);
    if (at < t->base + first) {
        return 0u;
    }
    return (at - t->base - first) / period + 1u;
}

/*
 * The counter `at` a time while running, from the same period as the
 * events -- one statement of it, so the count a guest reads and the
 * interrupts it takes cannot drift apart.
 */
static uint32_t counter_at(const g4mh_ostm_t *t, uint64_t at)
{
    const uint64_t e = at - t->base;
    uint64_t first;
    uint64_t period;

    if (free_run(t)) {
        return (uint32_t)(t->value + e);
    }
    if (e <= t->value) {
        return (uint32_t)(t->value - e);
    }
    sequence(t, &first, &period);
    return (uint32_t)((uint64_t)t->cmp - (e - first - 1u) % period);
}

/* An interrupt request: the edge, and OSTM0's output in toggle mode. */
static void fire(g4mh_ostm_t *t, uint64_t n)
{
    if (t->toe != 0u && (n & 1u) != 0u) {
        t->to ^= 1u;
    }
    if ((t->ctl & G4MH_OSTM_CTL_IE) != 0u && t->intc != NULL) {
        g4mh_intc_raise(t->intc, t->channel);
    }
}

static void update(g4mh_ostm_t *t)
{
    if (!t->running) {
        return;
    }
    const uint64_t n = events_upto(t, t->now);

    if (n > t->seen) {
        fire(t, n - t->seen);
        t->seen = n;
    }
}

/*
 * Restart the sequence from now with the counter at `value`, every
 * event up to now counted. For a start, the events *at* now are new
 * -- a counter started at zero in interval mode is at zero -- so the
 * caller then updates; for a CMP rewrite they are not.
 */
static void rebase(g4mh_ostm_t *t, uint32_t value)
{
    t->value = value;
    t->base = t->now;
    t->seen = 0u;
}

void g4mh_ostm_init(g4mh_ostm_t *t, struct g4mh_intc *intc, uint32_t channel)
{
    t->intc = intc;
    t->channel = channel;
    t->cmp = 0u;
    t->ctl = 0u;
    t->to = 0u;
    t->toe = 0u;
    t->running = false;
    /*
     * FFFF_FFFFH, the interval mode's start value -- and table 34.12's
     * free-run one, 0, if the mode is changed before anything is
     * written; see the CTL write.
     */
    t->value = 0xFFFFFFFFu;
    t->pristine = true;
    t->base = 0u;
    t->seen = 0u;
    t->now = 0u;
}

void g4mh_ostm_set_time(g4mh_ostm_t *t, uint64_t now)
{
    t->now = counts(now);
    update(t);
}

void g4mh_ostm_advance(g4mh_ostm_t *t, uint32_t ticks)
{
    t->now += counts(ticks);
    update(t);
}

static void start(g4mh_ostm_t *t)
{
    if (t->running && free_run(t)) {
        return; /* "ignored as long as OSTMnTE = 1" */
    }
    /*
     * Interval mode loads CMP and free-run starts from zero, unless
     * MD2 says to keep the counter -- except that a forced restart in
     * interval mode reloads "regardless of the value of MD2".
     */
    uint32_t v = t->value;

    if (t->running || (t->ctl & G4MH_OSTM_CTL_MD2) == 0u) {
        v = free_run(t) ? 0u : t->cmp;
    }
    t->running = true;
    t->pristine = false;
    rebase(t, v);
    if ((t->ctl & G4MH_OSTM_CTL_MD0) != 0u) {
        fire(t, 1u);
    }
    update(t);
}

static void stop(g4mh_ostm_t *t)
{
    if (!t->running) {
        return;
    }
    update(t);
    t->value = counter_at(t, t->now);
    t->running = false;
}

static emu_fault_t ostm_read(void *ctx, uint32_t off, uint32_t size,
                             uint32_t *out)
{
    g4mh_ostm_t *t = (g4mh_ostm_t *)ctx;

    (void)size;
    switch (off) {
    case G4MH_OSTM_CMP:
        *out = t->cmp;
        break;
    case G4MH_OSTM_CNT:
        *out = t->running ? counter_at(t, t->now) : t->value;
        break;
    case G4MH_OSTM_TO:
        *out = t->to;
        break;
    case G4MH_OSTM_TOE:
        *out = t->toe;
        break;
    case G4MH_OSTM_TE:
        *out = t->running ? 1u : 0u;
        break;
    case G4MH_OSTM_CTL:
        *out = t->ctl;
        break;
    default:
        *out = 0u; /* TS and TT read as 00H, and the holes */
        break;
    }
    return EMU_FAULT_NONE;
}

static emu_fault_t ostm_write(void *ctx, uint32_t off, uint32_t size,
                              uint32_t val)
{
    g4mh_ostm_t *t = (g4mh_ostm_t *)ctx;

    switch (off) {
    case G4MH_OSTM_CMP:
        if (size != 4u) {
            return EMU_FAULT_STORE;
        }
        if (t->running) {
            /*
             * "Even during the count operation, OSTMnCMP rewrite is
             * immediately reflected": settle what the old value made
             * happen, then go on from the counter as it stands. In
             * interval mode the new value is first used at the next
             * reload, which is what continuing from the current count
             * gives.
             */
            update(t);
            const uint32_t now_value = counter_at(t, t->now);

            t->cmp = val;
            rebase(t, now_value);
            t->seen = events_upto(t, t->now); /* now is not an event */
        } else {
            t->cmp = val;
        }
        break;
    case G4MH_OSTM_CNT:
        if (size != 4u) {
            return EMU_FAULT_STORE;
        }
        if (!t->running) { /* writable only while stopped */
            t->value = val;
            t->pristine = false;
        }
        break;
    case G4MH_OSTM_TO:
        if (t->toe == 0u) { /* only in software control mode */
            t->to = (uint8_t)(val & 1u);
        }
        break;
    case G4MH_OSTM_TOE:
        t->toe = (uint8_t)(val & 1u);
        break;
    case G4MH_OSTM_TS:
        if ((val & 1u) != 0u) {
            start(t);
        }
        break;
    case G4MH_OSTM_TT:
        if ((val & 1u) != 0u) {
            stop(t);
        }
        break;
    case G4MH_OSTM_CTL:
        if (!t->running) { /* read-only while counting */
            const uint32_t was = t->ctl;

            t->ctl = val & G4MH_OSTM_CTL_WRITABLE;
            /*
             * Table 34.12: the counter reads the new mode's start value
             * after the mode is changed following reset -- FFFF_FFFFH
             * counting down, 0 counting up.
             */
            if (t->pristine && ((was ^ t->ctl) & G4MH_OSTM_CTL_MD1) != 0u) {
                t->value = free_run(t) ? 0u : 0xFFFFFFFFu;
            }
        }
        break;
    default:
        break;
    }
    return EMU_FAULT_NONE;
}

const emu_dev_ops_t g4mh_ostm_ops = {
    .read = ostm_read,
    .write = ostm_write,
    .tick = NULL,
};
