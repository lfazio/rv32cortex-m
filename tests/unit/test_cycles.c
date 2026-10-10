/* SPDX-License-Identifier: Apache-2.0 */
/*
 * test_cycles.c - a 32-bit counter read as a 64-bit one.
 *
 * The property is one line and it is the whole point: the count goes on
 * past 2^32. The firmware's two clocks were each a 32-bit subtraction
 * from an epoch, and a run longer than one wrap of the counter came out
 * a whole wrap short -- 5.47 s reported to a guest for 24.99 s of wall
 * clock. See emu_cycles.h.
 *
 * Nothing here can run on the part that had the bug, and nothing needs
 * to: the defect was arithmetic.
 */

#include "tests.h"

#include "emu/emu_cycles.h"

/*
 * Started near the top of the range, because a counter that has been
 * running since reset is wherever it is -- a test that starts at zero
 * puts the wrap four billion cycles away and never reaches it.
 */
static void test_count_crosses_the_wrap(void)
{
    emu_cycles_t c;

    emu_cycles_start(&c, 0xFFFFFF00u);
    CHECK_EQ64(emu_cycles_read(&c, 0xFFFFFF00u), 0u);
    CHECK_EQ64(emu_cycles_read(&c, 0xFFFFFFF0u), 0xF0u);

    /* The counter is now small. The count is not. */
    CHECK_EQ64(emu_cycles_read(&c, 0x00000010u), 0x110u);
    CHECK_EQ64(emu_cycles_read(&c, 0x00000010u), 0x110u);
}

/*
 * Five whole wraps, read four times a wrap -- which is the firmware's
 * situation scaled down: the run loop reads its clock every slice, and
 * a run lasts many wraps.
 *
 * After exactly five the counter is back where it started, so an epoch
 * subtraction reports **zero** elapsed: the case that reads most like a
 * run that never happened, and the reason the expectation is written
 * as a number of wraps rather than as whatever the helper returns.
 */
static void test_count_keeps_whole_wraps(void)
{
    emu_cycles_t c;
    uint32_t now = 0x12345678u;
    uint64_t got = 0u;

    emu_cycles_start(&c, now);
    for (unsigned i = 0; i < 20u; i++) {
        now += 0x40000000u;
        got = emu_cycles_read(&c, now);
    }
    CHECK_EQ(now, 0x12345678u); /* five wraps: back at the start */
    CHECK_EQ64(got, 5ull << 32);
}

void test_cycles(void)
{
    test_count_crosses_the_wrap();
    test_count_keeps_whole_wraps();
}
