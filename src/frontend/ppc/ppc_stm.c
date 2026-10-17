/* SPDX-License-Identifier: Apache-2.0 */
/*
 * ppc_stm.c - the System Timer Module: a counter, a prescaler and four
 * compare channels.
 *
 * MPC5675K Reference Manual, Rev. 10, chapter 51. 51.4 is the whole
 * behaviour: the counter increments at the system clock divided by
 * CPS + 1 while TEN is set, rolls over at 0xFFFF_FFFF, and an enabled
 * channel whose compare value matches it sets CIF and requests an
 * interrupt until software writes 1 to CIF.
 *
 * Four choices where the chapter says nothing, each stated so a test can
 * hold it:
 *
 *   - **a match is the counter reaching the compare value**, by
 *     counting. Writing STM_CNT or STM_CMPn so that they are equal, or
 *     enabling a channel whose values already are, matches nothing. The
 *     decrementer's zero is read the same way (ppc_cpu_advance), and
 *     for the same reason: time arrives in slices of many ticks, so the
 *     rule has to be about the transition;
 *   - **the request is CIF**, whatever CEN says. Clearing CEN stops new
 *     matches; it does not withdraw one already flagged;
 *   - **the prescaler restarts** when CPS changes or the timer is
 *     enabled, so a divide takes effect on the next full period; any
 *     other write of STM_CR leaves it counting;
 *   - **a 16-bit write of STM_CR** is a bus error like any other access
 *     that is not a word (51.3) -- including the halfword holding CPS.
 */

#include "ppc/ppc_stm.h"
#include "ppc/ppc_cpu.h"
#include "ppc/ppc_intc.h"

static uint32_t divisor(const ppc_stm_t *s)
{
    return ((s->cr & PPC_STM_CR_CPS) >> PPC_STM_CR_CPS_SHIFT) + 1u;
}

/* The core's next stop: the earliest match, from its last clock sync. */
static void publish(ppc_stm_t *s)
{
    if (s->core != NULL) {
        s->core->stm_until = ppc_stm_until(s);
    }
}

static void request(ppc_stm_t *s, uint32_t n)
{
    if (s->intc != NULL) {
        ppc_intc_set_source(s->intc, PPC_STM_IRQ0 + n,
                            (s->cir[n] & PPC_STM_CIR_CIF) != 0u);
    }
}

void ppc_stm_reset(ppc_stm_t *s)
{
    s->cr = 0u;
    s->cnt = 0u;
    s->presc = 0u;
    for (uint32_t n = 0u; n < PPC_STM_CHANNELS; n++) {
        s->ccr[n] = 0u;
        s->cir[n] = 0u;
        s->cmp[n] = 0u;
        request(s, n);
    }
    publish(s);
}

uint32_t ppc_stm_until(const ppc_stm_t *s)
{
    uint64_t best = 0xFFFFFFFFu;

    if ((s->cr & PPC_STM_CR_TEN) == 0u) {
        return 0xFFFFFFFFu;
    }
    for (uint32_t n = 0u; n < PPC_STM_CHANNELS; n++) {
        if ((s->ccr[n] & PPC_STM_CCR_CEN) == 0u ||
            (s->cir[n] & PPC_STM_CIR_CIF) != 0u) {
            continue;
        }
        /*
         * Increments until the counter reaches the compare value -- a
         * whole turn when it is there already -- and the ticks that
         * takes, less those the prescaler has counted towards the next.
         * Never zero: presc is below the divisor.
         */
        const uint32_t d = s->cmp[n] - s->cnt;
        const uint64_t need = (d == 0u) ? (1ull << 32) : (uint64_t)d;
        const uint64_t t = need * divisor(s) - s->presc;

        if (t < best) {
            best = t;
        }
    }
    return (uint32_t)best;
}

void ppc_stm_advance(ppc_stm_t *s, uint32_t ticks)
{
    if ((s->cr & PPC_STM_CR_TEN) == 0u || ticks == 0u) {
        return;
    }
    const uint32_t div = divisor(s);
    const uint64_t t = (uint64_t)s->presc + ticks;
    const uint32_t k = (uint32_t)(t / div);

    s->presc = (uint32_t)(t % div);
    if (k != 0u) {
        const uint32_t old = s->cnt;

        s->cnt = old + k;
        for (uint32_t n = 0u; n < PPC_STM_CHANNELS; n++) {
            /* Reached on the way: cmp is in (old, old + k], modulo 2^32. */
            if ((s->ccr[n] & PPC_STM_CCR_CEN) != 0u &&
                (s->cir[n] & PPC_STM_CIR_CIF) == 0u &&
                (uint32_t)(s->cmp[n] - old - 1u) < k) {
                s->cir[n] |= PPC_STM_CIR_CIF;
                request(s, n);
            }
        }
    }
    publish(s);
}

/*
 * The register at `off`, or NULL for a reserved one. Channels are 16
 * bytes apart from 0x10, with their fourth word reserved.
 */
static uint32_t *reg(ppc_stm_t *s, uint32_t off, uint32_t *chan)
{
    if (off == PPC_STM_CR) {
        return &s->cr;
    }
    if (off == PPC_STM_CNT) {
        return &s->cnt;
    }
    if (off < PPC_STM_CCR(0) || off >= PPC_STM_CCR(PPC_STM_CHANNELS)) {
        return NULL;
    }
    *chan = (off - PPC_STM_CCR(0)) / 0x10u;
    switch ((off - PPC_STM_CCR(0)) % 0x10u) {
    case 0x0u:
        return &s->ccr[*chan];
    case 0x4u:
        return &s->cir[*chan];
    case 0x8u:
        return &s->cmp[*chan];
    default:
        return NULL;
    }
}

static emu_fault_t stm_read(void *ctx, uint32_t off, uint32_t size,
                            uint32_t *out)
{
    ppc_stm_t *const s = (ppc_stm_t *)ctx;
    uint32_t chan = 0u;
    const uint32_t *r;

    if (size != 4u || (off & 3u) != 0u) {
        return EMU_FAULT_LOAD;
    }
    r = reg(s, off, &chan);
    if (r == NULL) {
        return EMU_FAULT_LOAD;
    }
    /* The counter and the flags as of this instruction, not the slice. */
    if (s->core != NULL) {
        ppc_cpu_sync_clock(s->core);
    }
    *out = *r;
    return EMU_FAULT_NONE;
}

static emu_fault_t stm_write(void *ctx, uint32_t off, uint32_t size,
                             uint32_t val)
{
    ppc_stm_t *const s = (ppc_stm_t *)ctx;
    uint32_t chan = 0u;
    uint32_t *const r = reg(s, off, &chan);

    if (size != 4u || (off & 3u) != 0u || r == NULL) {
        return EMU_FAULT_STORE;
    }
    /* Count up to this instruction at the old settings first. */
    if (s->core != NULL) {
        ppc_cpu_sync_clock(s->core);
    }
    if (r == &s->cr) {
        const uint32_t v =
            val & (PPC_STM_CR_CPS | PPC_STM_CR_FRZ | PPC_STM_CR_TEN);

        if (((v ^ s->cr) & PPC_STM_CR_CPS) != 0u ||
            ((v & ~s->cr) & PPC_STM_CR_TEN) != 0u) {
            s->presc = 0u;
        }
        s->cr = v;
    } else if (r == &s->cnt) {
        s->cnt = val;
    } else if (r == &s->ccr[chan]) {
        s->ccr[chan] = val & PPC_STM_CCR_CEN;
    } else if (r == &s->cir[chan]) {
        if ((val & PPC_STM_CIR_CIF) != 0u) { /* w1c; a 0 does nothing */
            s->cir[chan] = 0u;
            request(s, chan);
        }
    } else {
        s->cmp[chan] = val;
    }
    /* Any of these moves the next match: the run has to stop again. */
    publish(s);
    if (s->core != NULL) {
        s->core->irq_dirty = true;
    }
    return EMU_FAULT_NONE;
}

const emu_dev_ops_t ppc_stm_ops = {
    .read = stm_read,
    .write = stm_write,
    .tick = NULL,
};
