/* SPDX-License-Identifier: Apache-2.0 */
/*
 * barrier3.c - three PEs, one barrier, one console.
 *
 * The smallest program that is actually multicore: every PE runs this
 * same image from the same reset address, tells itself apart by HTCFG0,
 * does some work into a shared array, meets the others at a hardware
 * barrier, and then *one* of them reports what all three produced.
 *
 * Why it is shaped like this
 * -------------------------
 * The report is the test. PE0 reads the other PEs' slots only after the
 * barrier completes, so if the barrier did nothing -- if BRnSYNC came
 * back without waiting -- PE0 would print slots the others had not
 * written yet. A demo where each PE just prints its own line proves only
 * that three cores ran, which HTCFG0 already told us.
 *
 * Output goes to the NS16550 at 0x1000_0000, the same console the RV32
 * guests use. It is the *platform's* device rather than a frontend's --
 * the host runner maps it into every core's bus -- so a G4MH guest gets
 * it for free and no translation exists on either side.
 *
 * Two hazards this has to get right, both of them the barrier's own
 * rules rather than anything about this emulator:
 *
 *   - **A PE that arrives before the barrier is enabled is lost.** "If
 *     all bits of the BRnEN register are 0, BRCHK bit cannot be set."
 *     PE0 configures BRnEN and the others spin until they can see it,
 *     so nobody arrives at a barrier that does not yet exist.
 *
 *   - **BRnSYNC must be cleared by software.** BRnCHK is set by any
 *     write and cleared by the hardware when the barrier completes;
 *     BRnSYNC takes the value written, so it stays set until the PE
 *     writes 0. Leaving it set makes the *next* barrier on that channel
 *     return immediately.
 *
 * Built with CC-RH, which is the only compiler that emits G4MH; see
 * scripts/g4mh-build-guest.sh.
 *
 * **This needs a three-PE emulator** (-DG4MH_PE_COUNT=3). BR0EN names
 * three participants, so on a one-PE build PE0 arrives at a barrier the
 * other two can never reach and the run ends on the instruction cap with
 * nothing printed. That is what the hardware would do and there is no
 * register here to discover the PE count from, so it is stated rather
 * than detected -- but it means a silent run is a configuration problem,
 * not a barrier problem.
 */

/* ------------------------------------------------------------------ */
/* The machine                                                         */
/* ------------------------------------------------------------------ */

#define BARR_BASE   0xFFFB8000u
#define BR0EN       (*(volatile unsigned *)(BARR_BASE + 0x004u))
#define BR0CHKS     (*(volatile unsigned *)(BARR_BASE + 0x100u))
#define BR0SYNCS    (*(volatile unsigned *)(BARR_BASE + 0x104u))

/*
 * BOOTCTRL. **Only PE0 runs at reset release**; PE1 and PE2 sit held
 * until PE0 asserts their bit (U2B 11.4.79), so a multicore program
 * needs a start-up step and this is it. Without the write below this
 * program is a single-core program that waits forever at a barrier
 * whose other participants were never started.
 */
#define BOOTCTRL    (*(volatile unsigned *)0xFFFB2000u)

/*
 * The console. An NS16550's transmit holding register is at offset 0 and
 * this model accepts a byte unconditionally, so there is no line-status
 * poll -- a real driver would wait on LSR.THRE and this one would be
 * wrong on hardware. Stated rather than left as a silent simplification.
 */
#define UART_THR    (*(volatile unsigned char *)0x10000000u)

/* Where the PEs leave their results for PE0 to collect. */
#define SHARED      ((volatile unsigned *)0x80000800u)

#define NPE         3u

#pragma inline_asm read_htcfg0
static unsigned read_htcfg0(void)
{
    stsr 0, r10, 2      ; HTCFG0 is selID 2, register 0
    mov  r10, r10
}

/* ------------------------------------------------------------------ */
/* Console                                                             */
/* ------------------------------------------------------------------ */

static void putc_(char c)
{
    UART_THR = (unsigned char)c;
}

static void puts_(const char *s)
{
    while (*s != '\0') {
        putc_(*s++);
    }
}

/*
 * Decimal, most significant digit first, with no buffer.
 *
 * The first version built the digits backwards into a stack array and
 * printed with puts_. It emitted the right thing for zero -- which takes
 * an early return -- and nothing at all otherwise, so the output read
 * "PE0= PE= PE=" and looked like a barrier problem rather than a
 * formatting one. Whatever the cause, a routine with no buffer and no
 * pointer arithmetic cannot have it, and this program is a
 * demonstration of the barrier: it should not also be a test of the
 * compiler's stack handling.
 */
static void putu(unsigned v)
{
    unsigned scale = 1u;

    while (v / scale >= 10u) {
        scale *= 10u;
    }
    while (scale != 0u) {
        putc_((char)('0' + ((v / scale) % 10u)));
        scale /= 10u;
    }
}

/* Stop this PE. HALT is what a core executes when it has nothing left. */
#pragma inline_asm halt_
static void halt_(void)
{
    halt
}

/* ------------------------------------------------------------------ */
/* Barrier                                                             */
/* ------------------------------------------------------------------ */

/*
 * Arrive at channel 0 and wait for every participant.
 *
 * The spin is a plain load in a loop. A real system would SNOOZE here to
 * hand the core back, and this emulator's scheduler is round-robin with
 * a quantum so a busy loop still makes progress -- but a single-quantum
 * run would spin the whole slice. Kept simple because the barrier is
 * what is being shown; see the SNOOZE note in the run loop.
 */
static void barrier_wait(void)
{
    BR0CHKS = 1u;                    /* any write arrives */

    while (BR0SYNCS == 0u) {
        /* wait for the last participant */
    }
    BR0SYNCS = 0u;                   /* software clears it */
}

/* ------------------------------------------------------------------ */
/* Entry                                                               */
/* ------------------------------------------------------------------ */

int main(void)
{
    const unsigned pe = read_htcfg0() & 0x7u;
    unsigned i;

    if (pe == 0u) {
        /*
         * PE0 owns the configuration. Clearing the slots before enabling
         * the barrier is what makes the report meaningful: if a slot
         * still held a value from before, PE0 could print a complete
         * line without the other PEs having run at all.
         */
        for (i = 0; i < NPE; i++) {
            SHARED[i] = 0u;
        }
        BR0EN = (1u << NPE) - 1u;    /* PE0, PE1 and PE2 participate */

        /*
         * Start the others -- *after* the barrier is configured and the
         * slots are cleared, because a released PE runs immediately and
         * would otherwise arrive at a barrier that does not exist yet.
         * That arrival would be dropped ("if all bits of BRnEN are 0,
         * BRCHK cannot be set") and the barrier would never complete.
         */
        BOOTCTRL = (1u << NPE) - 1u;
    } else {
        while (BR0EN == 0u) {
            /* wait for PE0 to open the barrier */
        }
    }

    /* The "work": each PE produces something only it can. */
    SHARED[pe] = pe * 100u + 7u;

    barrier_wait();

    /*
     * Past this point every PE has written its slot. Only PE0 reports,
     * so the output is one line rather than three interleaved ones --
     * the cores share a console and nothing serialises a half-written
     * string.
     */
    if (pe == 0u) {
        puts_("g4mh: 3 cores past the barrier:");
        for (i = 0; i < NPE; i++) {
            puts_(" PE");
            putu(i);
            puts_("=");
            putu(SHARED[i]);
        }
        putc_('\n');
    }

    /*
     * Every PE stops itself. Returning from main would run off into
     * whatever follows in flash -- there is no crt0 to return to -- and
     * the run then ends on the instruction cap rather than because the
     * program finished, which is the difference between "it worked" and
     * "it did not obviously fail".
     */
    halt_();
    return 0;
}
