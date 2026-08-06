/* SPDX-License-Identifier: Apache-2.0 */
/*
 * g4mh_intc.c - INTC1, INTC2 and the time base.
 *
 * Register offsets and bit positions from the RH850/U2B hardware manual
 * (R01UH0923EJ0130), Section 6.3. One state struct behind three devices,
 * because that is what the hardware is: EICn is a single channel array
 * that INTC1 answers for below channel 32 and INTC2 above it.
 */

#include "g4mh/g4mh_intc.h"
#include "g4mh/g4mh_cpu.h"

#include <string.h>

void g4mh_intc_init(g4mh_intc_t *ic, g4mh_cpu_t *cpu, g4mh_intc_t *global)
{
    memset(ic, 0, sizeof(*ic));
    ic->cpu = cpu;
    ic->global = (global != NULL) ? global : ic;
    ic->ostm_cmp = UINT64_MAX;   /* no compare match until software sets one */

    /* Masked, lowest priority, edge detection -- the reset value. */
    for (unsigned i = 0; i < G4MH_INT_CHANNELS; i++) {
        ic->eic[i] = G4MH_EIC_RESET;
    }

    cpu->intc = ic;
    cpu->time = &ic->ostm_cnt;
}

void g4mh_intc_set_unmask(g4mh_intc_t *ic, emu_unmask_fn fn, void *ctx)
{
    ic->unmask = fn;
    ic->unmask_ctx = ctx;
}

void g4mh_intc_raise(g4mh_intc_t *ic, uint32_t channel)
{
    if (channel >= G4MH_INT_CHANNELS) {
        return;
    }
    ic->eic[channel] |= G4MH_EIC_EIRF;
    if (ic->cpu != NULL) {
        ic->cpu->irq_dirty = true;
    }
}

void g4mh_intc_ack(g4mh_intc_t *ic, uint32_t channel)
{
    if (channel < G4MH_INT_CHANNELS) {
        ic->eic[channel] &= (uint16_t)~G4MH_EIC_EIRF;
    }
}

int g4mh_intc_pending(const g4mh_intc_t *ic, unsigned pe)
{
    int best = -1;
    unsigned best_pri = 16u;

    for (unsigned i = 0; i < G4MH_INT_CHANNELS; i++) {
        /*
         * Below 32 the channel is this core's own and is his by
         * definition; at 32 and above it lives in the shared INTC2 and is
         * only a candidate if EIBD binds it to this PE.
         */
        const bool local = (i < G4MH_INTC1_CHANNELS);
        const g4mh_intc_t *src = local ? ic : ic->global;

        if (!local &&
            (src->eibd[i] & G4MH_EIBD_PEID_MASK) != (uint32_t)pe) {
            continue;
        }

        const uint16_t e = src->eic[i];
        if ((e & G4MH_EIC_EIRF) == 0u || (e & G4MH_EIC_EIMK) != 0u) {
            continue;
        }
        /* EIP 0 is the highest priority, so smaller wins; strictly less,
         * so a tie keeps the lower channel number. */
        const unsigned pri = e & G4MH_EIC_EIP_MASK;
        if (pri < best_pri) {
            best_pri = pri;
            best = (int)i;
        }
    }
    return best;
}

void g4mh_intc_set_time(g4mh_intc_t *ic, uint64_t now)
{
    ic->ostm_cnt = now;
    /* Unsigned comparison, so a wrap does not fire the channel spuriously. */
    if (now >= ic->ostm_cmp) {
        g4mh_intc_raise(ic, G4MH_OSTM_CHANNEL);
    }
}

void g4mh_intc_advance(g4mh_intc_t *ic, uint32_t delta)
{
    g4mh_intc_set_time(ic, ic->ostm_cnt + delta);
}

/* ------------------------------------------------------------------ */
/* EICn                                                                */
/* ------------------------------------------------------------------ */

/*
 * A write to EICn. EIRF is set by hardware and cleared by software, so a
 * write can only clear it -- the guest's bit is ANDed with what is
 * actually pending rather than taken as written.
 */
static void eic_write(g4mh_intc_t *ic, unsigned ch, uint16_t val)
{
    const bool was_masked = (ic->eic[ch] & G4MH_EIC_EIMK) != 0u;

    const uint16_t rf = (uint16_t)(ic->eic[ch] & val & G4MH_EIC_EIRF);
    ic->eic[ch] = (uint16_t)((val & (uint16_t)~G4MH_EIC_EIRF) | rf);

    /*
     * Unmasking is the guest saying it has dealt with the device, so this
     * is where the host line goes back on. Nothing on the host side can
     * service the peripheral -- only the guest's driver knows how -- so a
     * level-triggered source left unmasked would re-enter the host handler
     * forever without the guest ever running.
     */
    if (was_masked && (ic->eic[ch] & G4MH_EIC_EIMK) == 0u &&
        ic->unmask != NULL) {
        ic->unmask(ic->unmask_ctx, ch);
    }
    if (ic->cpu != NULL) {
        ic->cpu->irq_dirty = true;
    }
}

/* ------------------------------------------------------------------ */
/* INTC1: channels 0..31, core-local                                   */
/* ------------------------------------------------------------------ */

static emu_fault_t intc1_read(void *ctx, uint32_t off, uint32_t size,
                              uint32_t *out)
{
    const g4mh_intc_t *ic = (const g4mh_intc_t *)ctx;
    (void)size;

    if (off < G4MH_INTC1_CHANNELS * 2u) {
        *out = ic->eic[off / 2u];
    } else if (off == G4MH_INTC1_IMR0) {
        *out = ic->imr[0];
    } else if (off >= G4MH_INTC1_EIBD &&
               off < G4MH_INTC1_EIBD + G4MH_INTC1_CHANNELS * 4u) {
        *out = ic->eibd[(off - G4MH_INTC1_EIBD) / 4u];
    } else if (off == G4MH_INTC1_FIBD) {
        *out = ic->fibd;
    } else if (off == G4MH_INTC1_EIBG) {
        *out = ic->eibg;
    } else if (off == G4MH_INTC1_FIBG) {
        *out = ic->fibg;
    } else if (off == G4MH_INTC1_IHVCFG) {
        *out = ic->ihvcfg;
    } else {
        /* EEIC and the reserved holes read as zero rather than faulting,
         * which is what a reserved register does. */
        *out = 0u;
    }
    return EMU_FAULT_NONE;
}

static emu_fault_t intc1_write(void *ctx, uint32_t off, uint32_t size,
                               uint32_t val)
{
    g4mh_intc_t *ic = (g4mh_intc_t *)ctx;
    (void)size;

    if (off < G4MH_INTC1_CHANNELS * 2u) {
        eic_write(ic, off / 2u, (uint16_t)val);
    } else if (off == G4MH_INTC1_IMR0) {
        ic->imr[0] = val;
    } else if (off >= G4MH_INTC1_EIBD &&
               off < G4MH_INTC1_EIBD + G4MH_INTC1_CHANNELS * 4u) {
        ic->eibd[(off - G4MH_INTC1_EIBD) / 4u] = val;
    } else if (off == G4MH_INTC1_FIBD) {
        ic->fibd = val;
    } else if (off == G4MH_INTC1_EIBG) {
        ic->eibg = val;
    } else if (off == G4MH_INTC1_FIBG) {
        ic->fibg = val;
    } else if (off == G4MH_INTC1_IHVCFG) {
        ic->ihvcfg = val;
    }
    return EMU_FAULT_NONE;
}

/* ------------------------------------------------------------------ */
/* INTC2: channels 32 and up, shared                                   */
/* ------------------------------------------------------------------ */

/*
 * EICn is at the same 0x02 * n for the whole channel range, so INTC2's
 * window starts at channel 0's address and the low 64 bytes of it are
 * simply not its to answer -- INTC1 has those. Offsets below 32 * 2 are
 * therefore reserved here rather than aliasing onto INTC1's channels.
 */
static emu_fault_t intc2_read(void *ctx, uint32_t off, uint32_t size,
                              uint32_t *out)
{
    const g4mh_intc_t *ic = (const g4mh_intc_t *)ctx;
    (void)size;

    if (off < G4MH_INT_CHANNELS * 2u) {
        *out = (off < G4MH_INTC1_CHANNELS * 2u) ? 0u : ic->eic[off / 2u];
    } else if (off >= G4MH_INTC2_IMR && off < G4MH_INTC2_IMR + 32u * 4u) {
        *out = ic->imr[(off - G4MH_INTC2_IMR) / 4u];
    } else if (off >= G4MH_INTC2_EIBD &&
               off < G4MH_INTC2_EIBD + G4MH_INT_CHANNELS * 4u) {
        const unsigned n = (off - G4MH_INTC2_EIBD) / 4u;
        *out = (n < G4MH_INTC1_CHANNELS) ? 0u : ic->eibd[n];
    } else {
        *out = 0u;
    }
    return EMU_FAULT_NONE;
}

static emu_fault_t intc2_write(void *ctx, uint32_t off, uint32_t size,
                               uint32_t val)
{
    g4mh_intc_t *ic = (g4mh_intc_t *)ctx;
    (void)size;

    if (off >= G4MH_INTC1_CHANNELS * 2u && off < G4MH_INT_CHANNELS * 2u) {
        eic_write(ic, off / 2u, (uint16_t)val);
    } else if (off >= G4MH_INTC2_IMR && off < G4MH_INTC2_IMR + 32u * 4u) {
        ic->imr[(off - G4MH_INTC2_IMR) / 4u] = val;
    } else if (off >= G4MH_INTC2_EIBD &&
               off < G4MH_INTC2_EIBD + G4MH_INT_CHANNELS * 4u) {
        const unsigned n = (off - G4MH_INTC2_EIBD) / 4u;
        if (n >= G4MH_INTC1_CHANNELS) {
            ic->eibd[n] = val;
        }
    }
    return EMU_FAULT_NONE;
}

/* ------------------------------------------------------------------ */
/* OS timer                                                            */
/* ------------------------------------------------------------------ */

static emu_fault_t ostm_read(void *ctx, uint32_t off, uint32_t size,
                             uint32_t *out)
{
    const g4mh_intc_t *ic = (const g4mh_intc_t *)ctx;
    (void)size;

    switch (off) {
    case G4MH_OSTM_CNT:      *out = (uint32_t)ic->ostm_cnt; break;
    case G4MH_OSTM_CNT + 4u: *out = (uint32_t)(ic->ostm_cnt >> 32); break;
    case G4MH_OSTM_CMP:      *out = (uint32_t)ic->ostm_cmp; break;
    case G4MH_OSTM_CMP + 4u: *out = (uint32_t)(ic->ostm_cmp >> 32); break;
    default:                 *out = 0u; break;
    }
    return EMU_FAULT_NONE;
}

static emu_fault_t ostm_write(void *ctx, uint32_t off, uint32_t size,
                              uint32_t val)
{
    g4mh_intc_t *ic = (g4mh_intc_t *)ctx;
    (void)size;

    switch (off) {
    case G4MH_OSTM_CMP:
        ic->ostm_cmp = (ic->ostm_cmp & 0xFFFFFFFF00000000ull) | val;
        break;
    case G4MH_OSTM_CMP + 4u:
        ic->ostm_cmp = (ic->ostm_cmp & 0xFFFFFFFFull) | ((uint64_t)val << 32);
        break;
    default:
        /* The counter is read-only; writes are dropped, not faulted. */
        break;
    }
    return EMU_FAULT_NONE;
}

const emu_dev_ops_t g4mh_intc1_ops = {
    .read = intc1_read, .write = intc1_write, .tick = NULL,
};
const emu_dev_ops_t g4mh_intc2_ops = {
    .read = intc2_read, .write = intc2_write, .tick = NULL,
};
const emu_dev_ops_t g4mh_ostm_ops = {
    .read = ostm_read, .write = ostm_write, .tick = NULL,
};
