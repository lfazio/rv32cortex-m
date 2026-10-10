/* SPDX-License-Identifier: Apache-2.0 */
/*
 * emu_cycles.h - a free-running 32-bit counter, read as a 64-bit one.
 *
 * A Cortex-M's cycle counter is 32 bits wide and wraps: every 19.9
 * seconds at 216 MHz, every 23.9 at 180, every 5.4 at 800. Subtracting
 * an epoch from it is right for any gap shorter than one wrap and
 * silently wrong for every longer one -- the result is the true figure
 * less a whole number of wraps, which is still a plausible number.
 *
 * That is not hypothetical. The firmware took both of its clocks that
 * way: the guest's time base, and the host-cycle total behind "host
 * cycles per guest instruction". A CoreMark run of 24.99 s by the wall
 * clock reported 5.47 s to the guest, which then declared its own run
 * too short to be valid, and 39.73 host cycles per instruction where
 * the part had spent 182.8. A PowerPC run reported the JIT 5.3x faster
 * than the interpreter while taking 21.9 s against its 12.3. Every
 * figure was the right shape, and every one was a wrap short.
 *
 * So the counter is *accumulated* instead: each reading adds the
 * distance from the previous one, which is wrap-correct in unsigned
 * arithmetic, into a 64-bit total. The one condition is that it is read
 * more often than it wraps -- and the reader is the run loop's clock
 * hook, once per guest slice, which is microseconds.
 *
 * Not safe against itself: read it from one context. A reading taken in
 * an interrupt between another reading's two statements would count the
 * same cycles twice.
 *
 * Here rather than under a platform because it is arithmetic with no
 * part number in it, and because that is what lets a host unit test
 * hold it to the one property that matters.
 */
#ifndef EMU_CYCLES_H
#define EMU_CYCLES_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct emu_cycles {
    uint32_t last; /* the counter at the previous reading */
    uint64_t total; /* everything counted since emu_cycles_start */
} emu_cycles_t;

/* Begin counting from `now`. */
static inline void emu_cycles_start(emu_cycles_t *c, uint32_t now)
{
    c->last = now;
    c->total = 0u;
}

/* The count since emu_cycles_start, given the counter's current value. */
static inline uint64_t emu_cycles_read(emu_cycles_t *c, uint32_t now)
{
    c->total += (uint32_t)(now - c->last);
    c->last = now;
    return c->total;
}

#ifdef __cplusplus
}
#endif

#endif /* EMU_CYCLES_H */
