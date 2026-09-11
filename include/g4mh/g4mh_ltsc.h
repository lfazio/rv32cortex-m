/* SPDX-License-Identifier: Apache-2.0 */
/*
 * g4mh_ltsc.h - the Long-Term System Counter.
 *
 * RH850/U2B section 44. The simplest peripheral in this frontend and the
 * most useful one to a guest that wants to know how long something took:
 * a 64-bit free-running counter on PCLK, with **neither a compare nor an
 * interrupt**. There is nothing to program and nothing to service -- a
 * guest starts it once and reads it.
 *
 * That is why it exists here. The TPTM's free-running channel is 32 bits
 * and wraps in an hour at a megahertz; the OSTM is an interval timer
 * whose counter is a means to an interrupt. Neither is a timebase, and a
 * guest wanting `gettimeofday` had to pick one and work around it.
 *
 * **Reading the low half captures the high half**, which is the one
 * behaviour here that is not obvious and the one that cannot be left
 * out. Two independent 32-bit reads of a running 64-bit counter tear at
 * every low-half wrap: the low half is read just before it wraps and the
 * high half just after, so the result jumps back by 2^32. The
 * architecture solves it by making the low read a capture, and a guest
 * that reads high-then-low gets a value this device did not promise.
 * See the note on the read path in g4mh_ltsc.c.
 *
 * One channel. The U2B has LTSC0 and the register block is documented
 * per channel; a second would be another instance of this state at
 * another base rather than anything new here.
 */

#ifndef G4MH_LTSC_H
#define G4MH_LTSC_H

#include <stdint.h>

#include "emu/emu_dev.h"

/*
 * Register offsets from <LTSCn_base>, table 44.2. The gaps are real --
 * the block is sparse and everything unlisted reads zero.
 */
#define G4MH_LTSC_TCS 0x0010u  /* W:  bit 0 starts the counter        */
#define G4MH_LTSC_TCT 0x0014u  /* W:  bit 0 stops it, clears CST      */
#define G4MH_LTSC_CSTR 0x0018u /* R:  bit 0 is set while running      */
#define G4MH_LTSC_RMSK 0x0034u /* RW: bit 0 masks the software reset  */
#define G4MH_LTSC_CNTL 0x0040u /* RW: low 32; a read captures all 64  */
#define G4MH_LTSC_CNTH 0x0044u /* RW: high 32; a read returns capture */

/*
 * **The counter runs at 80 MHz**, which is PCLK for this unit: section
 * 44's clock-supply table names CLKC_HSB, and section 15 calls that
 * "CLKC_HSB (80-MHz clean clock)".
 *
 * It matters because the platform does not supply counts, it supplies
 * *time* -- the host reads a monotonic clock in microseconds and a board
 * ticks at 1 MHz -- so this ratio is what turns one into the other.
 * Getting it wrong does not fail: the counter still counts, monotonically
 * and smoothly, just at the wrong rate, and a guest measuring anything
 * against it is wrong by exactly this factor.
 */
#define G4MH_LTSC_HZ 80000000u

/*
 * What the platform's time is measured in. Both platforms here speak
 * microseconds; the conversion is written out rather than folded into a
 * constant so that changing either end is one edit.
 */
#define G4MH_LTSC_PLATFORM_HZ 1000000u

#define G4MH_LTSC_TS 0x1u   /* LTSCnTCS.LTSCnTS   */
#define G4MH_LTSC_TT 0x1u   /* LTSCnTCT.LTSCnTT   */
#define G4MH_LTSC_CST 0x1u  /* LTSCnCSTR.LTSCnCST */
#define G4MH_LTSC_RM 0x1u   /* LTSCnRMSK.LTSCnRM  */

typedef struct g4mh_ltsc {
    uint64_t cnt; /* the counter itself */

    /*
     * The value a read of CNTL captured, so the following read of CNTH
     * describes the same instant. Only the upper half is ever served
     * from here -- CNTL answers from `cnt` and captures as a side
     * effect, which is the order the architecture specifies.
     */
    uint32_t captured_high;

    /*
     * A 64-bit write arrives as two 32-bit ones and "the total 64-bit
     * register value will become effective after the higher 32-bit
     * value has been written to". So the low half is held here rather
     * than in `cnt`: writing it must not move the counter, or a guest
     * setting a value larger than the current one steps through every
     * intermediate value in between.
     */
    uint32_t pending_low;

    /*
     * Where the counter was, and when, so absolute host time can drive
     * it. `origin` is the host time at the last start or preset and
     * `offset` the counter value at that moment, so a running counter
     * reads offset + (now - origin) -- which keeps the architectural
     * start/stop and preset behaviour while the *platform* supplies a
     * wall clock rather than a tick count.
     *
     * `last_now` is remembered because a start has to know what "now"
     * is, and the only thing that tells this device is set_time.
     */
    uint64_t origin;
    uint64_t offset;
    uint64_t last_now;

    uint8_t running; /* CSTR.CST */
    uint8_t rmsk;    /* RMSK.RM  */
} g4mh_ltsc_t;

void g4mh_ltsc_init(g4mh_ltsc_t *t);

/*
 * Advance by `ticks` of PCLK. There is no divider -- section 44 gives it
 * none -- so this is one for one, and on the host the platform tick is a
 * microsecond, which makes the counter a microsecond clock.
 */
void g4mh_ltsc_advance(g4mh_ltsc_t *t, uint32_t ticks);

/*
 * Absolute time, which is how a *host* drives this: the platform reads a
 * real clock and says what time it is rather than how much has passed.
 *
 * A platform uses one or the other, never both -- the same arrangement
 * rv_clint_set_time and rv_clint_advance already have, and for the same
 * reason: mixing them counts the same microsecond twice. The host
 * runner calls this one; a board with only a tick counter calls
 * g4mh_ltsc_advance.
 */
void g4mh_ltsc_set_time(g4mh_ltsc_t *t, uint64_t now);

extern const emu_dev_ops_t g4mh_ltsc_ops;

#endif /* G4MH_LTSC_H */
