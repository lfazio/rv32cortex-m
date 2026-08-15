/* SPDX-License-Identifier: Apache-2.0 */
/*
 * g4mh_barrier.c - BARR, the hardware barrier. U2B manual section 3.6.
 *
 * Sixteen channels. Each PE that participates writes its BRnCHKm to say
 * it has arrived; when every *enabled* PE has arrived, the hardware
 * clears all the check bits and sets every enabled PE's BRnSYNCm in one
 * step. That single step is the whole peripheral: doing it in software
 * needs a lock and a poll from every core.
 *
 * Two rules in the register descriptions are easy to lose and both are
 * tested:
 *
 *   BRnCHKm is set by *any* write, "regardless of the writing value" --
 *   so writing 0 arrives at the barrier just as writing 1 does.
 *   BRnSYNCm is not: it takes the value written, so software clears it
 *   by writing 0.
 *
 *   "If all bits of the BRnEN register are 0, BRCHK bit cannot be set."
 *   With no participants there is no barrier, and a check bit that could
 *   be set would never be cleared by anything except BRnINIT.
 */

#include "g4mh/g4mh_intercpu.h"

#include <string.h>

void g4mh_barrier_init(g4mh_barrier_t *b)
{
    memset(b, 0, sizeof(*b));
}

/*
 * Test the barrier of channel n and, if it is complete, take it.
 *
 * "All the BRnCHKm.BRCHK bits of participating PEs are set" -- the
 * participants are BRnEN and nothing else, so a check bit belonging to a
 * PE that is not enabled neither blocks the barrier nor is cleared by
 * it. That asymmetry is the manual's: BRnCHKm "can be set even if the
 * BRnEN.BRENm bit is 0, but it does not affect the barrier".
 */
static void barrier_eval(g4mh_barrier_t *b, unsigned n)
{
    const uint8_t en = b->en[n];

    if (en == 0u) {
        return;
    }
    if ((b->chk[n] & en) != en) {
        return;
    }

    /*
     * Only the participants' bits move. Clearing b->chk[n] outright
     * would discard an arrival by a PE that is about to be enabled,
     * which the hardware keeps.
     */
    b->chk[n] &= (uint8_t)~en;
    b->sync[n] |= en;
}

/*
 * Decode an offset into a channel and a register. Returns false for a
 * hole, which reads as zero and ignores writes -- "read access of an
 * undefined register or bit returns 0, and write access is ignored".
 */
static bool barrier_decode(uint32_t off, unsigned self_pe, unsigned *chan,
                           unsigned *pe, unsigned *reg)
{
    if (off < 0x100u) {                     /* INIT / EN, per channel   */
        const uint32_t n = off / 0x10u;
        const uint32_t r = off % 0x10u;

        if (n >= G4MH_BARR_CHANNELS || (r != 0u && r != 4u)) {
            return false;
        }
        *chan = (unsigned)n;
        *pe   = 0u;
        *reg  = (r == 0u) ? G4MH_BARR_INIT : G4MH_BARR_EN;
        return true;
    }

    if (off < 0x200u) {                     /* the self region          */
        const uint32_t n = (off - 0x100u) / 0x10u;
        const uint32_t r = (off - 0x100u) % 0x10u;

        if (n >= G4MH_BARR_CHANNELS || (r != 0u && r != 4u)) {
            return false;
        }
        *chan = (unsigned)n;
        *pe   = self_pe;
        *reg  = (r == 0u) ? G4MH_BARR_CHK : G4MH_BARR_SYNC;
        return true;
    }

    if (off >= 0x800u) {                    /* the absolute windows     */
        const uint32_t rel = off - 0x800u;
        const uint32_t m   = rel / 0x100u;
        const uint32_t n   = (rel % 0x100u) / 0x10u;
        const uint32_t r   = rel % 0x10u;

        if (m >= G4MH_INTERCPU_PES || n >= G4MH_BARR_CHANNELS ||
            (r != 0u && r != 4u)) {
            return false;
        }
        *chan = (unsigned)n;
        *pe   = (unsigned)m;
        *reg  = (r == 0u) ? G4MH_BARR_CHK : G4MH_BARR_SYNC;
        return true;
    }

    return false;
}

static emu_fault_t barrier_read(void *ctx, uint32_t off, uint32_t size,
                                uint32_t *out)
{
    const g4mh_intercpu_port_t *p = (const g4mh_intercpu_port_t *)ctx;
    const g4mh_barrier_t *b = (const g4mh_barrier_t *)p->state;
    unsigned n, pe, reg;

    (void)size;
    *out = 0u;

    if (!barrier_decode(off, p->pe, &n, &pe, &reg)) {
        return EMU_FAULT_NONE;
    }

    switch (reg) {
    case G4MH_BARR_EN:   *out = b->en[n]; break;
    case G4MH_BARR_CHK:  *out = (b->chk[n]  >> pe) & 1u; break;
    case G4MH_BARR_SYNC: *out = (b->sync[n] >> pe) & 1u; break;
    default:             break;      /* BRnINIT reads 0 always */
    }
    return EMU_FAULT_NONE;
}

static emu_fault_t barrier_write(void *ctx, uint32_t off, uint32_t size,
                                 uint32_t val)
{
    g4mh_intercpu_port_t *p = (g4mh_intercpu_port_t *)ctx;
    g4mh_barrier_t *b = (g4mh_barrier_t *)p->state;
    unsigned n, pe, reg;

    (void)size;

    if (!barrier_decode(off, p->pe, &n, &pe, &reg)) {
        return EMU_FAULT_NONE;
    }

    switch (reg) {
    case G4MH_BARR_INIT:
        if ((val & 1u) != 0u) {
            b->chk[n]  = 0u;
            b->sync[n] = 0u;
        }
        break;

    case G4MH_BARR_EN:
        b->en[n] = (uint8_t)(val & 0x3Fu);
        /*
         * Enabling the last outstanding participant completes the
         * barrier there and then. Without this an arrival recorded
         * before the enable is invisible until the *next* arrival, and
         * a two-PE barrier whose second PE enables itself last would
         * hang -- which is the ordinary bring-up order.
         */
        barrier_eval(b, n);
        break;

    case G4MH_BARR_CHK:
        /*
         * Any write sets it: the value is not consulted.
         *
         * Except with no participants at all. The manual states two
         * different things about BRnEN here and they are easy to
         * collapse into one:
         *
         *   "This bit can be set even if the BRnEN.BRENm bit is 0, but
         *    it does not affect the barrier-synchronization"   -- so a
         *    non-participant may arrive, and is simply ignored;
         *
         *   "If all bits of the BRnEN register are 0, BRCHK bit cannot
         *    be set"                                           -- so an
         *    arrival at a channel nobody has configured is refused.
         *
         * The first is about this PE's bit and the second about the
         * whole register. Implementing only the first leaves a check
         * bit set on an unconfigured channel with nothing able to
         * complete the barrier and clear it, so the next guest to
         * configure that channel starts with a stale arrival.
         */
        if (b->en[n] != 0u) {
            b->chk[n] |= (uint8_t)(1u << pe);
            barrier_eval(b, n);
        }
        break;

    case G4MH_BARR_SYNC:
        /* This one *does* take the value written. */
        if ((val & 1u) != 0u) {
            b->sync[n] |= (uint8_t)(1u << pe);
        } else {
            b->sync[n] &= (uint8_t)~(1u << pe);
        }
        break;

    default:
        break;
    }
    return EMU_FAULT_NONE;
}

const emu_dev_ops_t g4mh_barrier_ops = {
    .read  = barrier_read,
    .write = barrier_write,
};
