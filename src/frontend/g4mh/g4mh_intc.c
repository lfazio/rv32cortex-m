/* SPDX-License-Identifier: Apache-2.0 */
/*
 * g4mh_intc.c - INTC1 and INTC2.
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

    if ((c & G4MH_EEIC_EICT) != 0u) {
        v |= G4MH_EIC_EICT;
    }
    if ((c & G4MH_EEIC_EIRF) != 0u) {
        v |= G4MH_EIC_EIRF;
    }
    if ((c & G4MH_EEIC_EIMK) != 0u) {
        v |= G4MH_EIC_EIMK;
    }
    if ((c & G4MH_EEIC_EITB) != 0u) {
        v |= G4MH_EIC_EITB;
    }
    if ((c & G4MH_EEIC_EIOV) != 0u) {
        v |= G4MH_EIC_EIOV;
    }
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
    const uint32_t ro =
        G4MH_EEIC_EICT | (((cur & G4MH_EEIC_EICT) != 0u) ? G4MH_EEIC_EIRF : 0u);
    uint32_t out = (val & ~ro) | (cur & ro);

    if (eip_keep) {
        out = (out & ~G4MH_EEIC_EIP_MASK) | (cur & 0x30u) | (val & 0x0Fu);
    }
    return out;
}

static uint32_t eic_to_chan(uint32_t cur, uint16_t v)
{
    uint32_t n = (uint32_t)(v & G4MH_EIC_EIP_MASK);

    if ((v & G4MH_EIC_EICT) != 0u) {
        n |= G4MH_EEIC_EICT;
    }
    if ((v & G4MH_EIC_EIRF) != 0u) {
        n |= G4MH_EEIC_EIRF;
    }
    if ((v & G4MH_EIC_EIMK) != 0u) {
        n |= G4MH_EEIC_EIMK;
    }
    if ((v & G4MH_EIC_EITB) != 0u) {
        n |= G4MH_EEIC_EITB;
    }
    if ((v & G4MH_EIC_EIOV) != 0u) {
        n |= G4MH_EEIC_EIOV;
    }
    return chan_merge(cur, n, true);
}

/* EEICn's reserved bits read back zero, so only the defined ones pass. */
static uint32_t eeic_to_chan(uint32_t cur, uint32_t v)
{
    const uint32_t defined = G4MH_EEIC_EICT | G4MH_EEIC_EIRF | G4MH_EEIC_EIMK |
                             G4MH_EEIC_EITB | G4MH_EEIC_EIOV |
                             G4MH_EEIC_EIP_MASK;

    return chan_merge(cur, v & defined, false);
}

/* ------------------------------------------------------------------ */
/* The channel words, and the one place they are written               */
/* ------------------------------------------------------------------ */

/* Channels 0..31 are this PE's, 32 and up the system's. */
static uint32_t chan_get(const g4mh_intc_t *ic, unsigned n)
{
    return (n < G4MH_INTC1_CHANNELS) ? ic->chan[n]
                                     : ic->intc2->chan[n - G4MH_INTC1_CHANNELS];
}

static bool requested(uint32_t c)
{
    return (c & G4MH_EEIC_EIRF) != 0u && (c & G4MH_EEIC_EIMK) == 0u;
}

/*
 * Install a channel word and its pending bit with it.
 *
 * **Every write of a channel word is through here**, which is what lets
 * the search walk the bitmaps instead of the words: a request flag set,
 * cleared or masked anywhere else would leave a bit that disagrees with
 * its word, and the channel would be taken when it should not be or
 * never be taken at all. test_intc_pending_summary holds the two equal
 * after every step of a random walk through every way in.
 */
static void chan_put(g4mh_intc_t *ic, unsigned n, uint32_t v)
{
    if (n < G4MH_INTC1_CHANNELS) {
        const uint32_t bit = 1u << n;

        ic->chan[n] = v;
        ic->pend = requested(v) ? (ic->pend | bit) : (ic->pend & ~bit);
        return;
    }
    {
        g4mh_intc2_t *const g = ic->intc2;
        const unsigned i = n - G4MH_INTC1_CHANNELS;
        const uint32_t bit = 1u << (i % 32u);
        uint32_t *const w = &g->pend[i / 32u];

        g->chan[i] = v;
        *w = requested(v) ? (*w | bit) : (*w & ~bit);
    }
}

/*
 * Tell whichever core may now have a different answer to "what is
 * pending". A core's own channel is its own news; a global one may be
 * bound to any core, so all of them are told.
 */
static void notify(g4mh_intc_t *ic, unsigned n)
{
    if (n < G4MH_INTC1_CHANNELS) {
        if (ic->cpu != NULL) {
            ic->cpu->irq_dirty = true;
        }
        return;
    }
    for (unsigned pe = 0u; pe < G4MH_INTC2_PES; pe++) {
        if (ic->intc2->cpu[pe] != NULL) {
            ic->intc2->cpu[pe]->irq_dirty = true;
        }
    }
}

void g4mh_intc_init(g4mh_intc_t *ic, g4mh_cpu_t *cpu, g4mh_intc_t *global,
                    g4mh_intc2_t *intc2)
{
    memset(ic, 0, sizeof(*ic));
    ic->cpu = cpu;
    ic->global = (global != NULL) ? global : ic;
    ic->intc2 = (global != NULL) ? global->intc2 : intc2;

    /*
     * Masked, lowest priority, edge detection. This is also IMRm's
     * FFFF_FFFFH reset, because IMRm *is* these EIMK bits -- there is no
     * second array to initialise, which is the point of holding it once.
     * Masked means not pending, so the bitmaps start clear.
     */
    for (unsigned i = 0; i < G4MH_INTC1_CHANNELS; i++) {
        ic->chan[i] = G4MH_EEIC_RESET;
    }
    if (global == NULL) {
        memset(intc2, 0, sizeof(*intc2));
        for (unsigned i = 0; i < G4MH_INTC2_SLOTS; i++) {
            intc2->chan[i] = G4MH_EEIC_RESET;
        }
    }
    if (cpu->coreid < G4MH_INTC2_PES) {
        ic->intc2->cpu[cpu->coreid] = cpu;
    }

    cpu->intc = ic;
}

void g4mh_intc_set_unmask(g4mh_intc_t *ic, emu_unmask_fn fn, void *ctx)
{
    ic->unmask = fn;
    ic->unmask_ctx = ctx;
}

static void g4mh_intc_raise_impl(g4mh_intc_t *ic, uint32_t channel)
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
    uint32_t c = chan_get(ic, channel);

    if ((c & G4MH_EEIC_EICT) == 0u && (c & G4MH_EEIC_EIRF) != 0u) {
        c |= G4MH_EEIC_EIOV;
    }
    chan_put(ic, channel, c | G4MH_EEIC_EIRF);
    notify(ic, channel);
}

static void g4mh_intc_ack_impl(g4mh_intc_t *ic, uint32_t channel)
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
    const uint32_t c = chan_get(ic, channel);

    if ((c & G4MH_EEIC_EICT) == 0u) {
        chan_put(ic, channel, c & ~G4MH_EEIC_EIRF);
    }
}

static int g4mh_intc_pending_impl(const g4mh_intc_t *ic, unsigned pe)
{
    return g4mh_intc_pending_pri(ic, pe, NULL);
}

static bool g4mh_intc_chan_is_table_impl(const g4mh_intc_t *ic,
                                         uint32_t channel)
{
    if (ic == NULL || channel >= (uint32_t)G4MH_INT_CHANNELS) {
        return false;
    }
    return (chan_get(ic, channel) & G4MH_EEIC_EITB) != 0u;
}

static bool g4mh_intc_chan_priority_impl(const g4mh_intc_t *ic,
                                         uint32_t channel, unsigned *priority)
{
    if (ic == NULL || channel >= (uint32_t)G4MH_INT_CHANNELS) {
        return false;
    }
    *priority = (unsigned)(chan_get(ic, channel) & G4MH_EEIC_EIP_MASK);
    return true;
}

/*
 * EIP 0 is the highest priority, so smaller wins, and the comparison is
 * strictly less so a tie keeps the lower channel number -- "the channel
 * with the lowest number is given priority over other channels" (6.4.6)
 * -- which holds because the bits are visited in channel order, this
 * PE's own before the global ones.
 *
 * Six bits, not four: a channel configured through EEICn in 64-priority
 * mode carries EIP[5:4], and comparing only the low nibble would rank
 * priority 16 equal with priority 0.
 */
static void consider(uint32_t word, int n, int *best, unsigned *best_pri)
{
    const unsigned pri = word & G4MH_EEIC_EIP_MASK;

    if (pri < *best_pri) {
        *best_pri = pri;
        *best = n;
    }
}

static int g4mh_intc_pending_pri_impl(const g4mh_intc_t *ic, unsigned pe,
                                      unsigned *priority)
{
    int best = -1;
    /* 64 levels, so no real priority can tie with "nothing found". */
    unsigned best_pri = 64u;
    const g4mh_intc2_t *const g = ic->intc2;

    /*
     * The bitmaps hold exactly the channels with a request and no mask,
     * so this visits the candidates and nothing else. Below 32 a channel
     * is this core's own by definition; at 32 and above it is a
     * candidate only if EIBD binds it to this PE.
     */
    for (uint32_t bits = ic->pend; bits != 0u; bits &= bits - 1u) {
        const unsigned n = (unsigned)__builtin_ctz(bits);

        consider(ic->chan[n], (int)n, &best, &best_pri);
    }
    for (unsigned w = 0u; w < G4MH_INTC2_WORDS; w++) {
        for (uint32_t bits = g->pend[w]; bits != 0u; bits &= bits - 1u) {
            const unsigned i = w * 32u + (unsigned)__builtin_ctz(bits);

            if ((g->eibd[i] & G4MH_EIBD_PEID_MASK) != (uint32_t)pe) {
                continue;
            }
            consider(g->chan[i], (int)(i + G4MH_INTC1_CHANNELS), &best, &best_pri);
        }
    }
    if (priority != NULL) {
        *priority = best_pri;
    }
    return best;
}

static bool g4mh_intc_wakeable_impl(const g4mh_intc_t *ic, unsigned pe)
{
    const g4mh_intc2_t *const g = ic->intc2;

    for (unsigned n = 0u; n < G4MH_INTC1_CHANNELS; n++) {
        if ((ic->chan[n] & G4MH_EEIC_EIMK) == 0u) {
            return true;
        }
    }
    for (unsigned i = 0u; i < G4MH_INTC2_SLOTS; i++) {
        if ((g->chan[i] & G4MH_EEIC_EIMK) == 0u &&
            (g->eibd[i] & G4MH_EIBD_PEID_MASK) == (uint32_t)pe) {
            return true;
        }
    }
    return false;
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
    const bool was_masked = (chan_get(ic, ch) & G4MH_EEIC_EIMK) != 0u;

    chan_put(ic, ch, val);

    if (was_masked && (val & G4MH_EEIC_EIMK) == 0u && ic->unmask != NULL) {
        ic->unmask(ic->unmask_ctx, ch);
    }
    notify(ic, ch);
}

static void eic_write(g4mh_intc_t *ic, unsigned ch, uint16_t val)
{
    chan_store(ic, ch, eic_to_chan(chan_get(ic, ch), val));
}

static void eeic_write(g4mh_intc_t *ic, unsigned ch, uint32_t val)
{
    chan_store(ic, ch, eeic_to_chan(chan_get(ic, ch), val));
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
        if (ch >= G4MH_INT_CHANNELS || (chan_get(ic, ch) & G4MH_EEIC_EIMK) != 0u) {
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
        const uint32_t cur = chan_get(ic, ch);
        const uint32_t want = ((val >> b) & 1u) != 0u ? (cur | G4MH_EEIC_EIMK)
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
                   ? 0u
                   : chan_to_eic(chan_get(ic, off / 2u));
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
        *out = (n < G4MH_INTC1_CHANNELS)
                   ? 0u
                   : ic->intc2->eibd[n - G4MH_INTC1_CHANNELS];
    } else if (off >= G4MH_INTC2_EEIC &&
               off < G4MH_INTC2_EEIC + G4MH_INT_CHANNELS * 4u) {
        const unsigned n = (off - G4MH_INTC2_EEIC) / 4u;
        *out = (n < G4MH_INTC1_CHANNELS) ? 0u : chan_get(ic, n);
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
            /* A pending request may now be another core's to take. */
            ic->intc2->eibd[n - G4MH_INTC1_CHANNELS] = val;
            notify(ic, n);
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

static void g4mh_intc_raise_tptm_impl(g4mh_intc_t *ic, unsigned pe)
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

static bool g4mh_intc_fe_pending_impl(const g4mh_intc_t *ic, unsigned pe)
{
    (void)pe;
    return ic->feint != 0u;
}

static void g4mh_intc_ack_fe_impl(g4mh_intc_t *ic, unsigned pe)
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

static emu_fault_t fenc_read(void *ctx, uint32_t off, uint32_t size,
                             uint32_t *out)
{
    const g4mh_intc_t *ic = (const g4mh_intc_t *)ctx;
    (void)size;

    *out = (off == G4MH_FENC_FENMIF) ? (ic->global->fenc_nmif & 1u) : 0u;
    return EMU_FAULT_NONE;
}

static emu_fault_t fenc_write(void *ctx, uint32_t off, uint32_t size,
                              uint32_t val)
{
    g4mh_intc_t *ic = (g4mh_intc_t *)ctx;
    (void)size;

    if (off == G4MH_FENC_FENMIC && (val & 1u) != 0u) {
        ic->global->fenc_nmif = 0u;
    }
    return EMU_FAULT_NONE;
}

const emu_dev_ops_t g4mh_fenc_ops = {
    .read = fenc_read,
    .write = fenc_write,
    .tick = NULL,
};

const emu_dev_ops_t g4mh_intc1_ops = {
    .read = intc1_read,
    .write = intc1_write,
    .tick = NULL,
};
const emu_dev_ops_t g4mh_intc2_ops = {
    .read = intc2_read,
    .write = intc2_write,
    .tick = NULL,
};
const emu_dev_ops_t g4mh_intif_ops = {
    .read = intif_read,
    .write = intif_write,
    .tick = NULL,
};

/* ------------------------------------------------------------------ */
/* The ways in, under g4mh_sys_lock                                    */
/* ------------------------------------------------------------------ */

/*
 * What a core calls during its own quantum -- to decide on an interrupt,
 * to acknowledge one -- while another core may be writing the same
 * controller through a device. The lock is empty unless the host runs
 * cores in parallel, and recursive, so a device that is already holding
 * it raises an interrupt through here for the price of a compare.
 */
void g4mh_intc_raise(g4mh_intc_t *ic, uint32_t channel)
{
    emu_lock(&g4mh_sys_lock);
    g4mh_intc_raise_impl(ic, channel);
    emu_unlock(&g4mh_sys_lock);
}

void g4mh_intc_ack(g4mh_intc_t *ic, uint32_t channel)
{
    emu_lock(&g4mh_sys_lock);
    g4mh_intc_ack_impl(ic, channel);
    emu_unlock(&g4mh_sys_lock);
}

int g4mh_intc_pending(const g4mh_intc_t *ic, unsigned pe)
{
    emu_lock(&g4mh_sys_lock);
    const int r = g4mh_intc_pending_impl(ic, pe);

    emu_unlock(&g4mh_sys_lock);
    return r;
}

bool g4mh_intc_chan_is_table(const g4mh_intc_t *ic, uint32_t channel)
{
    emu_lock(&g4mh_sys_lock);
    const bool r = g4mh_intc_chan_is_table_impl(ic, channel);

    emu_unlock(&g4mh_sys_lock);
    return r;
}

void g4mh_intc_raise_tptm(g4mh_intc_t *ic, unsigned pe)
{
    emu_lock(&g4mh_sys_lock);
    g4mh_intc_raise_tptm_impl(ic, pe);
    emu_unlock(&g4mh_sys_lock);
}

bool g4mh_intc_fe_pending(const g4mh_intc_t *ic, unsigned pe)
{
    emu_lock(&g4mh_sys_lock);
    const bool r = g4mh_intc_fe_pending_impl(ic, pe);

    emu_unlock(&g4mh_sys_lock);
    return r;
}

void g4mh_intc_ack_fe(g4mh_intc_t *ic, unsigned pe)
{
    emu_lock(&g4mh_sys_lock);
    g4mh_intc_ack_fe_impl(ic, pe);
    emu_unlock(&g4mh_sys_lock);
}

void g4mh_intc_raise_nmi(g4mh_intc_t *ic)
{
    g4mh_intc_t *const g = ic->global;

    emu_lock(&g4mh_sys_lock);
    g->fenc_nmif = 1u;
    for (unsigned pe = 0; pe < G4MH_INTC2_PES; pe++) {
        struct g4mh_cpu *const cpu = g->intc2->cpu[pe];

        if (cpu != NULL && cpu->intc != NULL) {
            cpu->intc->fenmi = 1u;
            cpu->irq_dirty = true;
        }
    }
    if (g->intc2->cpu[0] == NULL && g->cpu != NULL) {
        g->fenmi = 1u; /* a lone controller with no system around it */
        g->cpu->irq_dirty = true;
    }
    emu_unlock(&g4mh_sys_lock);
}

bool g4mh_intc_fenmi_pending(const g4mh_intc_t *ic)
{
    return ic->fenmi != 0u;
}

void g4mh_intc_ack_fenmi(g4mh_intc_t *ic)
{
    emu_lock(&g4mh_sys_lock);
    ic->fenmi = 0u;
    emu_unlock(&g4mh_sys_lock);
}

bool g4mh_intc_chan_priority(const g4mh_intc_t *ic, uint32_t channel,
                             unsigned *priority)
{
    emu_lock(&g4mh_sys_lock);
    const bool r = g4mh_intc_chan_priority_impl(ic, channel, priority);

    emu_unlock(&g4mh_sys_lock);
    return r;
}

int g4mh_intc_pending_pri(const g4mh_intc_t *ic, unsigned pe,
                          unsigned *priority)
{
    emu_lock(&g4mh_sys_lock);
    const int r = g4mh_intc_pending_pri_impl(ic, pe, priority);

    emu_unlock(&g4mh_sys_lock);
    return r;
}

bool g4mh_intc_wakeable(const g4mh_intc_t *ic, unsigned pe)
{
    emu_lock(&g4mh_sys_lock);
    const bool r = g4mh_intc_wakeable_impl(ic, pe);

    emu_unlock(&g4mh_sys_lock);
    return r;
}
