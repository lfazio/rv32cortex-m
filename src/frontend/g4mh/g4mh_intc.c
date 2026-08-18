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
/* For G4MH_TPTM_EI_CHANNEL: TPTMSEL routes the TPTM's interval
 * interrupt, so this file needs the timer's channel number even though
 * the timer is next door. */
#include "g4mh/g4mh_intercpu.h"

#include <string.h>

/* ------------------------------------------------------------------ */
/* The three windows onto one channel word                             */
/* ------------------------------------------------------------------ */

/*
 * EICn is the 16-bit window. Its priority field is EIP[3:0], so a
 * channel configured through EEICn in 64-priority mode reads back here
 * with its top two priority bits invisible -- which is the
 * architecture's arrangement and the reason the manual says the two
 * "must be used exclusively".
 */
static uint16_t chan_to_eic(uint32_t c)
{
    uint16_t v = (uint16_t)(c & G4MH_EIC_EIP_MASK);

    if ((c & G4MH_EEIC_EICT) != 0u) { v |= G4MH_EIC_EICT; }
    if ((c & G4MH_EEIC_EIRF) != 0u) { v |= G4MH_EIC_EIRF; }
    if ((c & G4MH_EEIC_EIMK) != 0u) { v |= G4MH_EIC_EIMK; }
    if ((c & G4MH_EEIC_EITB) != 0u) { v |= G4MH_EIC_EITB; }
    if ((c & G4MH_EEIC_EIOV) != 0u) { v |= G4MH_EIC_EIOV; }
    return v;
}

/*
 * Merge a write through one of the windows into the channel word.
 *
 * Two bits are not the software's to set and are carried over from the
 * current value:
 *
 *   EICT  read-only in both windows. It says how the *source* is wired,
 *         not how software would like it to behave.
 *   EIRF  read-only while EICT is set: in level detection the flag
 *         follows the line, and "this bit cannot be set or cleared by
 *         the software" (table 6.15). In edge detection it is R/W, so a
 *         guest can both clear a stale request and inject one.
 *
 * `eip_keep` carries EIP[5:4] through a 16-bit write, which cannot
 * express them. Clearing them instead would silently promote a
 * 64-priority channel by up to 48 levels on any write to EICn.
 */
static uint32_t chan_merge(uint32_t cur, uint32_t val, bool eip_keep)
{
    const uint32_t ro = G4MH_EEIC_EICT |
                        (((cur & G4MH_EEIC_EICT) != 0u) ? G4MH_EEIC_EIRF : 0u);
    uint32_t out = (val & ~ro) | (cur & ro);

    if (eip_keep) {
        out = (out & ~G4MH_EEIC_EIP_MASK) | (cur & 0x30u) | (val & 0x0Fu);
    }
    return out;
}

static uint32_t eic_to_chan(uint32_t cur, uint16_t v)
{
    uint32_t n = (uint32_t)(v & G4MH_EIC_EIP_MASK);

    if ((v & G4MH_EIC_EICT) != 0u) { n |= G4MH_EEIC_EICT; }
    if ((v & G4MH_EIC_EIRF) != 0u) { n |= G4MH_EEIC_EIRF; }
    if ((v & G4MH_EIC_EIMK) != 0u) { n |= G4MH_EEIC_EIMK; }
    if ((v & G4MH_EIC_EITB) != 0u) { n |= G4MH_EEIC_EITB; }
    if ((v & G4MH_EIC_EIOV) != 0u) { n |= G4MH_EEIC_EIOV; }
    return chan_merge(cur, n, true);
}

/* EEICn's reserved bits read back zero, so only the defined ones pass. */
static uint32_t eeic_to_chan(uint32_t cur, uint32_t v)
{
    const uint32_t defined = G4MH_EEIC_EICT | G4MH_EEIC_EIRF |
                             G4MH_EEIC_EIMK | G4MH_EEIC_EITB |
                             G4MH_EEIC_EIOV | G4MH_EEIC_EIP_MASK;

    return chan_merge(cur, v & defined, false);
}

void g4mh_intc_init(g4mh_intc_t *ic, g4mh_cpu_t *cpu, g4mh_intc_t *global)
{
    memset(ic, 0, sizeof(*ic));
    ic->cpu = cpu;
    ic->global = (global != NULL) ? global : ic;
    ic->ostm_cmp = UINT64_MAX;   /* no compare match until software sets one */

    /*
     * Masked, lowest priority, edge detection. This is also IMRm's
     * FFFF_FFFFH reset, because IMRm *is* these EIMK bits -- there is no
     * second array to initialise, which is the point of holding it once.
     */
    for (unsigned i = 0; i < G4MH_INT_CHANNELS; i++) {
        ic->chan[i] = G4MH_EEIC_RESET;
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
    /*
     * A second edge arriving while the first is still pending sets
     * EIOVn -- "EIINTn rose in edge-detection mode when EICn.EIRF = 1"
     * (table 6.15). The flag is how a guest learns it lost a request,
     * and nothing set it before: an overrun was indistinguishable from
     * a single interrupt, which is the failure a driver written against
     * this bit exists to catch.
     *
     * Only in edge detection. With EICT set the flag tracks the line and
     * there is no second edge to overflow.
     */
    if ((ic->chan[channel] & G4MH_EEIC_EICT) == 0u &&
        (ic->chan[channel] & G4MH_EEIC_EIRF) != 0u) {
        ic->chan[channel] |= G4MH_EEIC_EIOV;
    }

    ic->chan[channel] |= G4MH_EEIC_EIRF;
    if (ic->cpu != NULL) {
        ic->cpu->irq_dirty = true;
    }
}

void g4mh_intc_ack(g4mh_intc_t *ic, uint32_t channel)
{
    if (channel >= G4MH_INT_CHANNELS) {
        return;
    }
    /*
     * "This flag is automatically cleared to 0 when an interrupt request
     * from its own channel is acknowledged by the CPU core" -- and only
     * in edge detection. A level-detected channel keeps EIRF until the
     * source drops the line, which is what makes it re-request if the
     * handler returns without servicing the device.
     */
    if ((ic->chan[channel] & G4MH_EEIC_EICT) == 0u) {
        ic->chan[channel] &= ~G4MH_EEIC_EIRF;
    }
}

int g4mh_intc_pending(const g4mh_intc_t *ic, unsigned pe)
{
    return g4mh_intc_pending_pri(ic, pe, NULL);
}

int g4mh_intc_pending_pri(const g4mh_intc_t *ic, unsigned pe,
                          unsigned *priority)
{
    int best = -1;
    /* 64 levels, so no real priority can tie with "nothing found". */
    unsigned best_pri = 64u;

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

        const uint32_t e = src->chan[i];
        if ((e & G4MH_EEIC_EIRF) == 0u || (e & G4MH_EEIC_EIMK) != 0u) {
            continue;
        }
        /*
         * EIP 0 is the highest priority, so smaller wins, and the
         * comparison is strictly less so a tie keeps the lower channel
         * number -- "the channel with the lowest number is given
         * priority over other channels" (6.4.6).
         *
         * Six bits, not four: a channel configured through EEICn in
         * 64-priority mode carries EIP[5:4], and comparing only the low
         * nibble would rank priority 16 equal with priority 0.
         */
        const unsigned pri = e & G4MH_EEIC_EIP_MASK;
        if (pri < best_pri) {
            best_pri = pri;
            best = (int)i;
        }
    }
    if (priority != NULL) {
        *priority = best_pri;
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
 * Install a new channel word, whichever window produced it, and do the
 * one thing that has to happen on an unmask.
 *
 * Unmasking is the guest saying it has dealt with the device, so this is
 * where the host line goes back on. Nothing on the host side can service
 * the peripheral -- only the guest's driver knows how -- so a
 * level-triggered source left unmasked would re-enter the host handler
 * forever without the guest ever running.
 *
 * It is one function because there are three ways in -- EICn, EEICn and
 * IMRm -- and the unmask handshake is owed by all three. It used to hang
 * off the EICn path alone, so a guest that unmasked through IMRm got no
 * callback and its host line stayed off.
 */
static void chan_store(g4mh_intc_t *ic, unsigned ch, uint32_t val)
{
    const bool was_masked = (ic->chan[ch] & G4MH_EEIC_EIMK) != 0u;

    ic->chan[ch] = val;

    if (was_masked && (val & G4MH_EEIC_EIMK) == 0u && ic->unmask != NULL) {
        ic->unmask(ic->unmask_ctx, ch);
    }
    if (ic->cpu != NULL) {
        ic->cpu->irq_dirty = true;
    }
}

static void eic_write(g4mh_intc_t *ic, unsigned ch, uint16_t val)
{
    chan_store(ic, ch, eic_to_chan(ic->chan[ch], val));
}

static void eeic_write(g4mh_intc_t *ic, unsigned ch, uint32_t val)
{
    chan_store(ic, ch, eeic_to_chan(ic->chan[ch], val));
}

/* ------------------------------------------------------------------ */
/* IMRm: the EIMK bits of 32 channels, gathered                        */
/* ------------------------------------------------------------------ */

/*
 * IMRm covers channels 32*m .. 32*m+31, so IMR0 is INTC1's own channels
 * and IMR1..31 are INTC2's. `ic` is therefore already the right unit --
 * the caller picks it the same way the EICn windows do.
 */
static uint32_t imr_read(const g4mh_intc_t *ic, unsigned m)
{
    uint32_t v = 0u;

    for (unsigned b = 0; b < 32u; b++) {
        const unsigned ch = m * 32u + b;

        /*
         * A channel this build does not have reads as masked. That is
         * the reset value and the safe direction: an absent channel
         * cannot be pending, so reporting it unmasked would only invite
         * a guest to believe it had enabled something.
         */
        if (ch >= G4MH_INT_CHANNELS ||
            (ic->chan[ch] & G4MH_EEIC_EIMK) != 0u) {
            v |= 1u << b;
        }
    }
    return v;
}

static void imr_write(g4mh_intc_t *ic, unsigned m, uint32_t val)
{
    for (unsigned b = 0; b < 32u; b++) {
        const unsigned ch = m * 32u + b;

        if (ch >= G4MH_INT_CHANNELS) {
            continue;
        }
        const uint32_t cur = ic->chan[ch];
        const uint32_t want = ((val >> b) & 1u) != 0u
                              ? (cur | G4MH_EEIC_EIMK)
                              : (cur & ~G4MH_EEIC_EIMK);

        if (want != cur) {
            chan_store(ic, ch, want);
        }
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
        *out = chan_to_eic(ic->chan[off / 2u]);
    } else if (off == G4MH_INTC1_IMR0) {
        *out = imr_read(ic, 0u);
    } else if (off >= G4MH_INTC1_EIBD &&
               off < G4MH_INTC1_EIBD + G4MH_INTC1_CHANNELS * 4u) {
        *out = ic->eibd[(off - G4MH_INTC1_EIBD) / 4u];
    } else if (off >= G4MH_INTC1_EEIC &&
               off < G4MH_INTC1_EEIC + G4MH_INTC1_CHANNELS * 4u) {
        *out = ic->chan[(off - G4MH_INTC1_EEIC) / 4u];
    } else if (off == G4MH_INTC1_FIBD) {
        *out = ic->fibd;
    } else if (off == G4MH_INTC1_EIBG) {
        *out = ic->eibg;
    } else if (off == G4MH_INTC1_FIBG) {
        *out = ic->fibg;
    } else if (off == G4MH_INTC1_IHVCFG) {
        *out = ic->ihvcfg;
    } else {
        /* The reserved holes read as zero rather than faulting, which is
         * what a reserved register does. */
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
        imr_write(ic, 0u, val);
    } else if (off >= G4MH_INTC1_EIBD &&
               off < G4MH_INTC1_EIBD + G4MH_INTC1_CHANNELS * 4u) {
        ic->eibd[(off - G4MH_INTC1_EIBD) / 4u] = val;
    } else if (off >= G4MH_INTC1_EEIC &&
               off < G4MH_INTC1_EEIC + G4MH_INTC1_CHANNELS * 4u) {
        eeic_write(ic, (off - G4MH_INTC1_EEIC) / 4u, val);
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
        *out = (off < G4MH_INTC1_CHANNELS * 2u)
               ? 0u : chan_to_eic(ic->chan[off / 2u]);
    } else if (off >= G4MH_INTC2_IMR && off < G4MH_INTC2_IMR + 32u * 4u) {
        /*
         * The address is <INTC2_base> + 1000H + 04H * n for n = 1..31,
         * so n comes straight from the offset -- and offset 0x1000 is
         * IMR0's slot, which belongs to INTC1 and is not mapped here.
         * Biasing the index by one instead would alias every register
         * onto its neighbour and put IMR31 out of range.
         */
        const unsigned m = (off - G4MH_INTC2_IMR) / 4u;
        *out = (m == 0u) ? 0u : imr_read(ic, m);
    } else if (off >= G4MH_INTC2_EIBD &&
               off < G4MH_INTC2_EIBD + G4MH_INT_CHANNELS * 4u) {
        const unsigned n = (off - G4MH_INTC2_EIBD) / 4u;
        *out = (n < G4MH_INTC1_CHANNELS) ? 0u : ic->eibd[n];
    } else if (off >= G4MH_INTC2_EEIC &&
               off < G4MH_INTC2_EEIC + G4MH_INT_CHANNELS * 4u) {
        const unsigned n = (off - G4MH_INTC2_EEIC) / 4u;
        *out = (n < G4MH_INTC1_CHANNELS) ? 0u : ic->chan[n];
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
        const unsigned m = (off - G4MH_INTC2_IMR) / 4u;
        if (m != 0u) {
            imr_write(ic, m, val);
        }
    } else if (off >= G4MH_INTC2_EIBD &&
               off < G4MH_INTC2_EIBD + G4MH_INT_CHANNELS * 4u) {
        const unsigned n = (off - G4MH_INTC2_EIBD) / 4u;
        if (n >= G4MH_INTC1_CHANNELS) {
            ic->eibd[n] = val;
        }
    } else if (off >= G4MH_INTC2_EEIC &&
               off < G4MH_INTC2_EEIC + G4MH_INT_CHANNELS * 4u) {
        const unsigned n = (off - G4MH_INTC2_EEIC) / 4u;
        if (n >= G4MH_INTC1_CHANNELS) {
            eeic_write(ic, n, val);
        }
    }
    return EMU_FAULT_NONE;
}

/* ------------------------------------------------------------------ */
/* INTIF: TPTMSEL, and the FEINT path it selects                       */
/* ------------------------------------------------------------------ */

void g4mh_intc_raise_tptm(g4mh_intc_t *ic, unsigned pe)
{
    const g4mh_intc_t *g = (ic->global != NULL) ? ic->global : ic;

    if (((g->tptmsel >> pe) & 1u) != 0u) {
        g4mh_intc_raise(ic, G4MH_TPTM_EI_CHANNEL);
        return;
    }

    /*
     * FEINT. Set on the PE's own controller rather than the global one,
     * because that is where the core looks and it keeps the pending set
     * per core exactly as eic[] is.
     */
    ic->feint = 1u;
    if (ic->cpu != NULL) {
        ic->cpu->irq_dirty = true;
    }
}

bool g4mh_intc_fe_pending(const g4mh_intc_t *ic, unsigned pe)
{
    (void)pe;
    return ic->feint != 0u;
}

void g4mh_intc_ack_fe(g4mh_intc_t *ic, unsigned pe)
{
    (void)pe;
    ic->feint = 0u;
}

static emu_fault_t intif_read(void *ctx, uint32_t off, uint32_t size,
                              uint32_t *out)
{
    const g4mh_intc_t *ic = (const g4mh_intc_t *)ctx;
    (void)size;

    *out = (off == G4MH_INTIF_TPTMSEL) ? ic->tptmsel : 0u;
    return EMU_FAULT_NONE;
}

static emu_fault_t intif_write(void *ctx, uint32_t off, uint32_t size,
                               uint32_t val)
{
    g4mh_intc_t *ic = (g4mh_intc_t *)ctx;
    (void)size;

    if (off == G4MH_INTIF_TPTMSEL) {
        ic->tptmsel = val & 0x3Fu;
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
const emu_dev_ops_t g4mh_intif_ops = {
    .read = intif_read, .write = intif_write, .tick = NULL,
};
