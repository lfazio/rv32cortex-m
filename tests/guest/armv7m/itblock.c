/* SPDX-License-Identifier: Apache-2.0 */
/*
 * itblock.c - IT blocks, which make the decoder stateful.
 *
 * **Written as inline asm, not as C the compiler might turn into IT
 * blocks.** gcc emits them only when it judges them cheaper than a
 * branch, so a C-level test of conditional code proves nothing about
 * which encoding was used -- and the point here is the encoding. Each
 * check names the thing it is about.
 *
 * The expectations are constants, derived by hand from the
 * architecture, because a test that asks the implementation what the
 * answer is encodes the bug as the expectation. This project has done
 * that once already, to a CMOV test that mirrored the compare it was
 * checking so an inverted implementation cancelled out.
 */

#define UART (*(volatile unsigned char *)0x10000000u)
static void pc_(char c)
{
    UART = (unsigned char)c;
}
static void ps_(const char *s)
{
    while (*s != '\0') {
        pc_(*s++);
    }
}
static void phex(unsigned v)
{
    for (int i = 28; i >= 0; i -= 4) {
        pc_("0123456789abcdef"[(v >> i) & 0xFu]);
    }
}

static unsigned g_fail;

static void want(const char *tag, unsigned got, unsigned expect)
{
    if (got != expect) {
        g_fail++;
        ps_("FAIL ");
        ps_(tag);
        ps_(" got ");
        phex(got);
        ps_(" want ");
        phex(expect);
        pc_('\n');
    }
}

int main(void)
{
    unsigned a;
    unsigned b;

    /*
     * The shape a compiler actually emits: a compare, then one
     * conditional instruction.
     */
    __asm__ volatile("movs  %0, #1\n\t"
                     "cmp   %0, #1\n\t"
                     "it    eq\n\t"
                     "moveq %0, #7\n\t"
                     : "=&r"(a));
    want("IT-taken", a, 7u);

    __asm__ volatile("movs  %0, #1\n\t"
                     "cmp   %0, #2\n\t"
                     "it    eq\n\t"
                     "moveq %0, #7\n\t"
                     : "=&r"(a));
    want("IT-not-taken", a, 1u);

    /*
     * ITT: two instructions under one condition. A frontend that
     * advanced ITSTATE wrongly gets the first right and the second
     * wrong, which is why one conditional instruction is not enough to
     * test this.
     */
    __asm__ volatile("movs  %0, #0\n\t"
                     "movs  %1, #0\n\t"
                     "cmp   %0, #0\n\t"
                     "itt   eq\n\t"
                     "moveq %0, #3\n\t"
                     "moveq %1, #4\n\t"
                     : "=&r"(a), "=&r"(b));
    want("ITT-a", a, 3u);
    want("ITT-b", b, 4u);

    /*
     * ITE: then *and* else, which is the case that proves the mask bit
     * becomes the condition's low bit. Exactly one of the two must run.
     */
    __asm__ volatile("movs  %0, #9\n\t"
                     "movs  %1, #9\n\t"
                     "cmp   %0, #9\n\t"
                     "ite   eq\n\t"
                     "moveq %0, #1\n\t"
                     "movne %1, #1\n\t"
                     : "=&r"(a), "=&r"(b));
    want("ITE-then", a, 1u);
    want("ITE-else", b, 9u);

    __asm__ volatile("movs  %0, #9\n\t"
                     "movs  %1, #9\n\t"
                     "cmp   %0, #8\n\t"
                     "ite   eq\n\t"
                     "moveq %0, #1\n\t"
                     "movne %1, #1\n\t"
                     : "=&r"(a), "=&r"(b));
    want("ITE-then-skipped", a, 9u);
    want("ITE-else-taken", b, 1u);

    /*
     * Four instructions, the longest block there is, so the mask is
     * exhausted and ITAdvance is exercised three times.
     */
    __asm__ volatile("movs  %0, #0\n\t"
                     "cmp   %0, #0\n\t"
                     "itttt eq\n\t"
                     "addeq %0, %0, #1\n\t"
                     "addeq %0, %0, #2\n\t"
                     "addeq %0, %0, #4\n\t"
                     "addeq %0, %0, #8\n\t"
                     : "=&r"(a));
    want("ITTTT-eq", a, 15u);

    /*
     * **The same block under MI, and the condition is the whole point.**
     *
     * ITAdvance shifts ITSTATE's low *five* bits and leaves the top
     * three alone. Shifting the whole byte instead is a plausible
     * misreading, and the check above cannot see it: EQ is condition
     * 0b0000, so ITSTATE's top three bits are zero and both versions
     * give the same answer. It is the one condition of the sixteen that
     * cannot distinguish them, and it was the one this test originally
     * used -- the weak-test trap this project keeps a table of.
     *
     * Under MI (0b0100) the broken advance yields HI (0b1000) instead.
     * `subs` of 0 - 1 leaves N set and C clear, so MI holds and HI does
     * not: the correct implementation runs all four adds, the broken one
     * runs the first and skips three.
     */
    /*
     * `cmp`, not `subs` then `movs`: the first version set up N with a
     * subtract and then zeroed the accumulator with `movs`, which sets
     * flags outside a block and cleared the N it had just arranged. The
     * test failed against a correct frontend. CMP writes no register,
     * so it can arrange the flags without disturbing the operand.
     */
    __asm__ volatile("movs  %0, #0\n\t"
                     "cmp   %0, #1\n\t"
                     "itttt mi\n\t"
                     "addmi %0, %0, #1\n\t"
                     "addmi %0, %0, #2\n\t"
                     "addmi %0, %0, #4\n\t"
                     "addmi %0, %0, #8\n\t"
                     : "=&r"(a));
    want("ITTTT-mi", a, 15u);

    /*
     * **The flag rule, which is the subtle half.** Inside a block a
     * 16-bit data-processing instruction does not set flags, so the
     * *second* instruction here is conditioned on the original compare
     * and not on the first ADD's result.
     *
     * r0 starts at 0 and the compare makes Z set. If the ADD wrote
     * flags, its result of 5 would clear Z and the second ADD would be
     * skipped -- giving 5 instead of 12. That one value is the whole
     * difference between implementing this rule and not.
     */
    __asm__ volatile("movs  %0, #0\n\t"
                     "cmp   %0, #0\n\t"
                     "itt   eq\n\t"
                     "addeq %0, %0, #5\n\t"
                     "addeq %0, %0, #7\n\t"
                     : "=&r"(a));
    want("IT-no-setflags", a, 12u);

    /*
     * And the converse: a block whose condition is false must leave the
     * flags alone too, so a compare *after* it still reads what it set.
     */
    __asm__ volatile("movs  %0, #4\n\t"
                     "cmp   %0, #0\n\t"
                     "it    eq\n\t"
                     "moveq %0, #99\n\t"
                     "it    ne\n\t"
                     "movne %0, #5\n\t"
                     : "=&r"(a));
    want("IT-flags-survive", a, 5u);

    /*
     * A wide instruction inside a block. The 32-bit forms carry their
     * own S bit, so they are *not* subject to the setflags rule above --
     * conditioning them is all a block does. `mov.w` with a modified
     * immediate is one the earlier guest exercises unconditionally.
     */
    __asm__ volatile("movs  %0, #0\n\t"
                     "cmp   %0, #0\n\t"
                     "it    eq\n\t"
                     "moveq %0, #0x00FF0000\n\t"
                     : "=&r"(a));
    want("IT-wide", a, 0x00FF0000u);

    if (g_fail == 0u) {
        ps_("ARMV7M-IT-OK\n");
    }
    return (int)g_fail;
}
