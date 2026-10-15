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

/*
 * What the fault handler saw, and what it puts right. See the last
 * check in main().
 */
unsigned g_fault_xpsr;
unsigned g_fault_fix;
unsigned g_faults;

/*
 * Escalated from the UsageFault a misaligned LDRD raises. Records the
 * stacked xPSR, gives the faulting instruction an aligned base by
 * rewriting the stacked r0, and returns -- so the same instruction runs
 * again, under the ITSTATE it was stacked with.
 */
__attribute__((naked)) void HardFault_Handler(void)
{
    __asm__ volatile("mrs   r0, msp\n\t"
                     "ldr   r1, [r0, #28]\n\t"
                     "ldr   r2, =g_fault_xpsr\n\t"
                     "str   r1, [r2]\n\t"
                     "ldr   r2, =g_fault_fix\n\t"
                     "ldr   r1, [r2]\n\t"
                     "str   r1, [r0]\n\t"
                     "ldr   r2, =g_faults\n\t"
                     "ldr   r1, [r2]\n\t"
                     "adds  r1, r1, #1\n\t"
                     "str   r1, [r2]\n\t"
                     "bx    lr\n\t"
                     ".ltorg\n\t");
}

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

    /*
     * **Memory inside a block.** A compiler writes these -- Dhrystone's
     * Func_1 is `ittte eq; ldreq; moveq; strbeq; movne` -- and the
     * translator used to give the whole block back to the interpreter
     * for them, because everything else in a block is computed and then
     * selected and an access cannot be done and then undone. It is
     * predicated now, which is what these check: a skipped access must
     * not happen at all, not happen and be ignored.
     *
     * A skipped store is the visible half: the word must be unchanged.
     */
    {
        static volatile unsigned w[4];
        unsigned *p = (unsigned *)&w[0];

        w[0] = 0x11111111u;
        __asm__ volatile("movs  %0, #0x55\n\t"
                         "cmp   %0, #0\n\t"
                         "it    eq\n\t"
                         "streq %0, [%1]\n\t"
                         : "=&r"(a)
                         : "r"(p)
                         : "memory", "cc");
        want("IT-str-skipped", w[0], 0x11111111u);

        __asm__ volatile("movs  %0, #0x55\n\t"
                         "cmp   %0, #0x55\n\t"
                         "it    eq\n\t"
                         "streq %0, [%1]\n\t"
                         : "=&r"(a)
                         : "r"(p)
                         : "memory", "cc");
        want("IT-str-taken", w[0], 0x55u);

        /*
         * Then and else, each a byte store to its own address: exactly
         * one may land. The shape of Dhrystone's block, with the store
         * on both sides.
         */
        w[1] = 0u;
        __asm__ volatile("movs  %0, #7\n\t"
                         "cmp   %0, #7\n\t"
                         "ite   eq\n\t"
                         "strbeq %0, [%1, #4]\n\t"
                         "strbne %0, [%1, #5]\n\t"
                         : "=&r"(a)
                         : "r"(p)
                         : "memory", "cc");
        want("ITE-strb", w[1], 0x00000007u);

        /*
         * A load with writeback, skipped: neither the destination nor
         * the base may move. Then taken: both do.
         */
        w[2] = 0xCAFEF00Du;
        {
            unsigned *q = (unsigned *)&w[2];

            a = 0x99u;
            __asm__ volatile("cmp   %1, #0\n\t"
                             "it    eq\n\t"
                             "ldreq %0, [%1], #4\n\t"
                             : "+&r"(a), "+&r"(q)
                             :
                             : "memory", "cc");
            want("IT-ldr-wb-skipped", a, 0x99u);
            want("IT-ldr-wb-base-kept", (unsigned)q, (unsigned)&w[2]);

            __asm__ volatile("cmp   %1, #0\n\t"
                             "it    ne\n\t"
                             "ldrne %0, [%1], #4\n\t"
                             : "+&r"(a), "+&r"(q)
                             :
                             : "memory", "cc");
            want("IT-ldr-wb-taken", a, 0xCAFEF00Du);
            want("IT-ldr-wb-base-moved", (unsigned)q, (unsigned)&w[3]);
        }
    }

    /*
     * **A skipped access that would have faulted.** LDRD and LDM fault
     * on a misaligned base on every ARMv7-M, whatever CCR says, so this
     * needs no address map: if the predicate does not stop the access,
     * the guest takes a UsageFault it has no reason to expect and the
     * count below says so.
     */
    {
        static unsigned dw[4] __attribute__((aligned(8)));
        unsigned char *odd = (unsigned char *)&dw[0] + 1;
        unsigned lo = 0x1u;
        unsigned hi = 0x2u;

        g_faults = 0u;
        __asm__ volatile("cmp    %2, #0\n\t"
                         "it     eq\n\t"
                         "ldrdeq %0, %1, [%2]\n\t"
                         : "+&r"(lo), "+&r"(hi)
                         : "r"(odd)
                         : "memory", "cc");
        want("IT-ldrd-skipped", lo | (hi << 8), 0x201u);

        __asm__ volatile("cmp    %0, #0\n\t"
                         "it     eq\n\t"
                         "ldmeq  %0, {r4, r5}\n\t"
                         :
                         : "r"(odd)
                         : "r4", "r5", "memory", "cc");
        want("IT-ldm-skipped-no-fault", g_faults, 0u);
    }

    /*
     * **A fault from inside a block, and the ITSTATE it stacks.** The
     * translator keeps ITSTATE out of xPSR everywhere else -- nothing
     * else in a block can be observed part way. An access can, and the
     * handler returns into the *rest* of the block by the ITSTATE it
     * finds on the stack. Lose it, and what follows the fault runs
     * unconditionally.
     *
     * `itee eq` with EQ true: the LDRD runs and faults, the two ADDs
     * are the else half and must not run. ITSTATE at the LDRD is
     * firstcond 0b0000, mask 0b1110 -- x and y are E, so NOT
     * firstcond[0], then the terminating 1 -- so 0x0E: IT[1:0] = 0b10
     * in bits 26:25, IT[7:2] = 0b000011 in bits 15:10.
     */
    {
        static unsigned dw[4]
            __attribute__((aligned(8))) = {0x0A0A0A0Au, 0x0B0B0B0Bu};
        register unsigned base __asm__("r0") =
            (unsigned)((unsigned char *)&dw[0] + 2);
        register unsigned lo __asm__("r4") = 0u;
        register unsigned hi __asm__("r5") = 0u;
        register unsigned n __asm__("r2") = 0u;

        g_faults = 0u;
        g_fault_xpsr = 0u;
        g_fault_fix = (unsigned)&dw[0];
        __asm__ volatile("cmp    %3, #0\n\t"
                         "itee   eq\n\t"
                         "ldrdeq %0, %1, [%2]\n\t"
                         "addne  %3, %3, #1\n\t"
                         "addne  %3, %3, #2\n\t"
                         : "+&r"(lo), "+&r"(hi), "+&r"(base), "+&r"(n)
                         :
                         : "memory", "cc");
        /*
         * Out of the pinned registers before the first call: a local
         * register variable is only that register at an asm operand,
         * and r2 is the third argument.
         */
        const unsigned got_lo = lo;
        const unsigned got_n = n;

        want("IT-fault-taken", g_faults, 1u);
        want("IT-fault-itstate", g_fault_xpsr & 0x0600FC00u, 0x04000C00u);
        want("IT-fault-retried", got_lo, 0x0A0A0A0Au);
        want("IT-fault-else-skipped", got_n, 0u);
    }

    if (g_fail == 0u) {
        ps_("ARMV7M-IT-OK\n");
    }
    return (int)g_fail;
}
