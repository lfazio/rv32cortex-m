/* SPDX-License-Identifier: Apache-2.0 */
/*
 * nvic.c - SysTick and the exception model, from the guest's side.
 *
 * **The only thing that can prove an interrupt works is waiting for
 * one.** Every register in the NVIC can be written and read back by a
 * device that never signals, and this project has already been caught
 * by exactly that: the first virtio guest accepted every write, read
 * every feature word back correctly, and the device did nothing at all
 * because the driver and the device disagreed about where the rings
 * were. Reading registers proves wiring; a completion proves delivery.
 *
 * So the checks here are about *observable consequences* -- a handler
 * that ran, a counter that moved, a frame that came back intact -- and
 * never about a register holding what was put in it.
 */

#define UART (*(volatile unsigned char *)0x10000000u)

#define SYST_CSR (*(volatile unsigned *)0xE000E010u)
#define SYST_RVR (*(volatile unsigned *)0xE000E014u)
#define SYST_CVR (*(volatile unsigned *)0xE000E018u)

#define NVIC_ISER (*(volatile unsigned *)0xE000E100u)
#define NVIC_ICER (*(volatile unsigned *)0xE000E180u)
#define NVIC_ISPR (*(volatile unsigned *)0xE000E200u)

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

/*
 * What the handler touches. Volatile because the only writer is an
 * asynchronous handler and the only reader is the loop waiting for it --
 * without it a compiler is entitled to hoist the load out of the wait
 * and spin for ever on a stale value.
 */
volatile unsigned g_ticks;
volatile unsigned g_witness;
volatile unsigned g_irq0;

/*
 * The SysTick handler, placed in the vector table by start.S.
 *
 * It deliberately clobbers r0-r3 and r12, which are the registers
 * exception entry stacks. If the frame were stacked or restored
 * wrongly, the *interrupted* code would resume with a corrupted
 * register and the failure would appear to be in the arithmetic that
 * followed -- so the test below checks a value held across the
 * interrupt.
 */
void SysTick_Handler(void);
void SysTick_Handler(void)
{
    g_ticks++;
    __asm__ volatile("movs r0, #0xAA\n\t"
                     "movs r1, #0xBB\n\t"
                     "movs r2, #0xCC\n\t"
                     "movs r3, #0xDD\n\t"
                     "mov  r12, r0\n\t" ::
                         : "r0", "r1", "r2", "r3", "r12");
    g_witness = 0x5A5Au;
}

/*
 * In frame.S, because gcc's register allocation cannot be kept out of
 * the registers under test. Returns a bitmask of what did not survive.
 */
unsigned armv7m_frame_check(unsigned sp_bias);

/* External source 0, which is exception 16. */
void IRQ0_Handler(void);
void IRQ0_Handler(void)
{
    g_irq0 = 1u;
}

int main(void)
{
    /*
     * COUNTFLAG with the interrupt *off*, which is how a bare-metal
     * delay loop is written and the case that needs no handler at all.
     * It must latch on the wrap and clear when read.
     */
    SYST_RVR = 400u;
    SYST_CVR = 0u;
    SYST_CSR = 1u; /* ENABLE, TICKINT clear */

    unsigned spins = 0u;

    while ((SYST_CSR & (1u << 16)) == 0u && spins < 100000u) {
        spins++;
    }
    want("countflag-latched", spins < 100000u, 1u);
    /* And it cleared on that read, so a second look must be false. */
    want("countflag-clears", (SYST_CSR & (1u << 16)) != 0u, 0u);

    /*
     * Now the interrupt. TICKINT set, and the handler increments a
     * counter -- so what is asserted is that *code ran*, not that a
     * register changed.
     */
    g_ticks = 0u;
    g_witness = 0u;
    SYST_RVR = 400u;
    SYST_CVR = 0u;
    SYST_CSR = 3u; /* ENABLE | TICKINT */

    /* A value the handler must not be able to disturb. */
    unsigned held = 0x1234u;

    spins = 0u;
    while (g_ticks == 0u && spins < 100000u) {
        spins++;
    }
    SYST_CSR = 0u;

    want("handler-ran", g_ticks != 0u, 1u);
    want("handler-witness", g_witness, 0x5A5Au);
    want("frame-held-local", held, 0x1234u);

    /*
     * **The frame, register by register, and in a real assembly
     * function.** See frame.S for why: the inline-asm version passed
     * with the restore of r0 and of r12 broken, because gcc needed
     * those registers for the operands and the test was measuring its
     * allocation rather than the frame.
     *
     * Twice, the second time from a stack biased by four, which is the
     * only way to exercise the 8-byte-alignment padding that entry
     * inserts and the return has to undo.
     */
    {
        SYST_CVR = 0u;
        SYST_CSR = 3u;
        want("frame-aligned", armv7m_frame_check(0u), 0u);
        want("frame-biased", armv7m_frame_check(4u), 0u);
        SYST_CSR = 0u;
    }

    /*
     * PRIMASK: with interrupts masked the handler must not run, and the
     * pending interrupt must arrive the moment it is unmasked. The
     * second half is what distinguishes masking from *losing*.
     */
    g_ticks = 0u;
    __asm__ volatile("cpsid i" ::: "memory");
    SYST_RVR = 400u;
    SYST_CVR = 0u;
    SYST_CSR = 3u;
    for (spins = 0u; spins < 2000u; spins++) {
        __asm__ volatile("" ::: "memory");
    }
    want("primask-blocks", g_ticks, 0u);

    __asm__ volatile("cpsie i" ::: "memory");
    spins = 0u;
    while (g_ticks == 0u && spins < 100000u) {
        spins++;
    }
    SYST_CSR = 0u;
    want("primask-releases", g_ticks != 0u, 1u);

    /*
     * An exception taken inside an IT block must resume with the block
     * intact -- which is why ITSTATE lives in xPSR, where the frame
     * carries it. There is no way to force the interrupt to land on a
     * chosen instruction, so this is a weaker check than it looks: it
     * proves the common case still works with interrupts arriving, not
     * that the awkward one does.
     */
    /*
     * **The reload has to exceed the handler**, and 7 did not.
     *
     * SysTick counts once per retired instruction here, so a reload
     * shorter than the handler re-enters it before it can return and
     * the guest makes no forward progress at all. That is what a real
     * core does with the same configuration -- it is the test that was
     * wrong, not the frontend -- but it presents as the emulator hanging
     * and reaching the instruction cap, so it is worth naming.
     *
     * 400 leaves room for the ~25-instruction handler and still fires
     * repeatedly across the loop below.
     */
    g_ticks = 0u;
    SYST_RVR = 400u;
    SYST_CVR = 0u;
    SYST_CSR = 3u;

    unsigned acc = 0u;

    for (spins = 0u; spins < 3000u; spins++) {
        unsigned t = 0u;

        __asm__ volatile("movs  %0, #0\n\t"
                         "cmp   %0, #0\n\t"
                         "itt   eq\n\t"
                         "addeq %0, %0, #5\n\t"
                         "addeq %0, %0, #7\n\t"
                         : "=&r"(t));
        acc += t;
    }
    SYST_CSR = 0u;
    want("it-under-interrupts", acc, 3000u * 12u);
    want("it-interrupts-arrived", g_ticks != 0u, 1u);

    /*
     * **The same thing with a condition that is FALSE**, which is what
     * makes ITSTATE observable across an exception.
     *
     * The block above uses EQ with Z set, and that cannot distinguish a
     * frontend which clears ITSTATE on entry from one which does not: a
     * handler inheriting a *true* condition runs exactly as it would
     * have anyway. Breaking both the clear-on-entry and the
     * restore-with-xPSR left every check above passing.
     *
     * With NE and Z set, the four adds must all be skipped, so `acc`
     * stays zero. Two things then become visible:
     *
     *   - a handler that inherits ITSTATE is conditioned on NE, which is
     *     false, so its first four instructions are skipped -- and
     *     `g_ticks++` is among them, so the tick never lands and the
     *     wait below times out;
     *   - a return that restores the flags but not ITSTATE leaves the
     *     rest of the block unconditional, so the remaining adds run
     *     and `acc` comes back non-zero.
     *
     * The reload is deliberately short and *not* a divisor of the loop
     * body, so interrupts land at every offset including inside the
     * block. It has to stay above the handler's length or the guest
     * makes no progress at all -- see the note above.
     */
    g_ticks = 0u;
    SYST_RVR = 41u;
    SYST_CVR = 0u;
    SYST_CSR = 3u;

    unsigned acc_false = 0u;

    for (spins = 0u; spins < 4000u; spins++) {
        unsigned t = 0u;

        __asm__ volatile("movs  %0, #0\n\t"
                         "cmp   %0, #0\n\t" /* Z set, so NE is false */
                         "itttt ne\n\t"
                         "addne %0, %0, #1\n\t"
                         "addne %0, %0, #2\n\t"
                         "addne %0, %0, #4\n\t"
                         "addne %0, %0, #8\n\t"
                         : "=&r"(t));
        acc_false += t;
    }
    SYST_CSR = 0u;
    want("it-false-skipped", acc_false, 0u);
    want("it-false-ticked", g_ticks != 0u, 1u);

    /*
     * An **external** source, through ISER and ISPR.
     *
     * Nothing else here touches them: SysTick is exception 15 and has
     * no NVIC enable bit at all, so breaking ISER from a
     * read-modify-write into a plain assignment left every check above
     * passing. Pending source 0 by hand is the only way a guest can
     * reach that path without a device to raise the line.
     *
     * Two sources, enabled one at a time, is what distinguishes `|=`
     * from `=`: enabling source 1 second must not disable source 0.
     */
    g_irq0 = 0u;
    NVIC_ISER = 1u << 0; /* enable source 0 */
    NVIC_ISER = 1u << 1; /* and source 1 -- must not clear source 0 */
    NVIC_ISPR = 1u << 0; /* pend source 0 */

    spins = 0u;
    while (g_irq0 == 0u && spins < 100000u) {
        spins++;
    }
    want("external-irq-ran", g_irq0, 1u);

    /* Disabled sources latch but must not deliver. */
    NVIC_ICER = 1u << 0;
    g_irq0 = 0u;
    NVIC_ISPR = 1u << 0;
    for (spins = 0u; spins < 2000u; spins++) {
        __asm__ volatile("" ::: "memory");
    }
    want("disabled-irq-blocked", g_irq0, 0u);

    /* ...and arrive when enabled again, rather than having been lost. */
    NVIC_ISER = 1u << 0;
    spins = 0u;
    while (g_irq0 == 0u && spins < 100000u) {
        spins++;
    }
    want("latched-irq-delivered", g_irq0, 1u);

    if (g_fail == 0u) {
        ps_("ARMV7M-NVIC-OK\n");
    }
    return (int)g_fail;
}
