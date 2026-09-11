/* SPDX-License-Identifier: Apache-2.0 */
/*
 * g4mh_ltsc.c - the Long-Term System Counter, RH850/U2B section 44.
 *
 * A 64-bit counter on PCLK with no compare and no interrupt. The whole
 * device is four behaviours: it runs or it does not, it can be preset
 * while stopped, and a read of the low half captures the high one.
 *
 * See g4mh_ltsc.h for why the capture is the part that matters.
 */

#include "g4mh/g4mh_ltsc.h"

/*
 * Platform time to counter steps. The platform speaks microseconds and
 * the counter runs at 80 MHz, so this is x80 -- done in 64 bits and as
 * a single expression so it stays exact for any ratio rather than only
 * for integer ones.
 */
static uint64_t ltsc_counts(uint64_t platform_ticks)
{
    return (platform_ticks * (uint64_t)G4MH_LTSC_HZ) /
           (uint64_t)G4MH_LTSC_PLATFORM_HZ;
}

void g4mh_ltsc_init(g4mh_ltsc_t *t)
{
    t->cnt = 0u;
    t->captured_high = 0u;
    t->pending_low = 0u;
    /*
     * Stopped after reset, which is what "value after reset 0000 0000H"
     * says about CSTR. A guest must start it -- and one that reads the
     * counter without starting it reads zero for ever rather than
     * getting a plausible-looking time, which is the failure that says
     * what it is.
     */
    t->running = 0u;
    t->rmsk = 0u;
    t->origin = 0u;
    t->offset = 0u;
    t->last_now = 0u;
}

void g4mh_ltsc_set_time(g4mh_ltsc_t *t, uint64_t now)
{
    t->last_now = now;
    if (t->running != 0u) {
        t->cnt = t->offset + ltsc_counts(now - t->origin);
    }
}

void g4mh_ltsc_advance(g4mh_ltsc_t *t, uint32_t ticks)
{
    if (t->running != 0u) {
        t->cnt += ltsc_counts((uint64_t)ticks);
    }
}

static emu_fault_t ltsc_read(void *ctx, uint32_t off, uint32_t size,
                             uint32_t *out)
{
    g4mh_ltsc_t *t = (g4mh_ltsc_t *)ctx;

    /*
     * 32-bit only. The manual allows 8-bit access to the control
     * registers, which nothing here needs and which would make the
     * capture below depend on which byte of CNTL was touched.
     */
    if (size != 4u || (off & 3u) != 0u) {
        return EMU_FAULT_LOAD;
    }

    switch (off) {
    case G4MH_LTSC_CNTL:
        /*
         * **The capture, and it happens on the read of the low half.**
         *
         * "In case of reading the 64-bit counter value while the counter
         * is running, the current 64-bit counter value will be captured
         * on read access to the lower 32-bit value. The captured upper
         * 32-bit value can be read afterwards. This is to avoid that the
         * upper or lower 32-bit value changes between the two
         * consecutive 32-bit read accesses."
         *
         * So a guest reads CNTL then CNTH and gets one instant. Reading
         * them the other way round gets a high half from before the low
         * half and is the tear the capture exists to prevent -- this
         * device cannot detect that and does not try to.
         *
         * Captured unconditionally rather than only while running: a
         * stopped counter does not change, so capturing costs nothing
         * and not capturing would leave CNTH answering from whatever the
         * last running read left behind.
         */
        t->captured_high = (uint32_t)(t->cnt >> 32);
        *out = (uint32_t)t->cnt;
        break;

    case G4MH_LTSC_CNTH:
        *out = t->captured_high;
        break;

    case G4MH_LTSC_CSTR:
        *out = (t->running != 0u) ? G4MH_LTSC_CST : 0u;
        break;

    case G4MH_LTSC_RMSK:
        *out = t->rmsk;
        break;

    /*
     * "LTSCnTCS is always read as 0000 0000H", and the same for TCT.
     * They are write-only strobes, so they fall through to the default
     * with everything else the block does not define.
     */
    default:
        *out = 0u;
        break;
    }
    return EMU_FAULT_NONE;
}

static emu_fault_t ltsc_write(void *ctx, uint32_t off, uint32_t size,
                              uint32_t val)
{
    g4mh_ltsc_t *t = (g4mh_ltsc_t *)ctx;

    if (size != 4u || (off & 3u) != 0u) {
        return EMU_FAULT_STORE;
    }

    switch (off) {
    case G4MH_LTSC_TCS:
        if ((val & G4MH_LTSC_TS) != 0u) {
            /*
             * Starting does **not** clear the counter. Unlike the TPTM's
             * free-running channel, which restarts from zero, section 44
             * gives LTSC no such behaviour: the counter is preset
             * through CNTL/CNTH while stopped and continues from where
             * it was otherwise. A guest that stops and restarts it is
             * pausing a clock, not resetting one.
             */
            t->running = 1u;
            /*
             * Anchor the counter to the clock as it is now, so the time
             * the guest spent with it stopped is not credited to it the
             * moment it starts.
             */
            t->origin = t->last_now;
            t->offset = t->cnt;
        }
        break;

    case G4MH_LTSC_TCT:
        /*
         * "Setting this bit is ignored as long as LTSCnCSTR.LTSCnCST =
         * 0" -- stopping an already-stopped counter is not an error and
         * is not a no-op by accident, it is specified.
         */
        if ((val & G4MH_LTSC_TT) != 0u && t->running != 0u) {
            t->running = 0u;
            t->offset = t->cnt;
        }
        break;

    case G4MH_LTSC_CNTL:
        /*
         * "Write access is permitted only while counter is stopped."
         * Ignored while running rather than faulted: a guest doing this
         * has made a mistake this device cannot fix, and the rest of
         * the frontend answers that the same way.
         *
         * Held rather than applied -- see the note on pending_low.
         */
        if (t->running == 0u) {
            t->pending_low = val;
        }
        break;

    case G4MH_LTSC_CNTH:
        /*
         * The write that makes the pair effective. A guest writing only
         * the high half gets the low half it last wrote, which is what
         * "the register must be accessed from the lower 32 bits, and
         * then the upper 32 bits" asks of it.
         */
        if (t->running == 0u) {
            t->cnt = ((uint64_t)val << 32) | (uint64_t)t->pending_low;
            /* A preset re-anchors, or the next set_time would undo it. */
            t->offset = t->cnt;
            t->origin = t->last_now;
        }
        break;

    case G4MH_LTSC_RMSK:
        t->rmsk = (uint8_t)(val & G4MH_LTSC_RM);
        break;

    default:
        /* Unassigned and the read-only registers: ignored. */
        break;
    }
    return EMU_FAULT_NONE;
}

const emu_dev_ops_t g4mh_ltsc_ops = {
    .read = ltsc_read,
    .write = ltsc_write,
    .tick = NULL,
};
