/* SPDX-License-Identifier: Apache-2.0 */
/*
 * alu.c - the ARMv7-M frontend's arithmetic, against values computed
 * elsewhere.
 *
 * **Compiled C, and that is the point.** The hand-written hello.S
 * exercises what someone thought to write; a compiler reaches for
 * MOVW/MOVT, the modified immediate, shifts by register and the wide
 * data-processing forms without being asked, and it found three decode
 * defects here that no hand-written test had:
 *
 *   - MOVW and MOVT masked with a bit the pattern did not contain, so
 *     neither ever decoded;
 *   - the modified-immediate test read bits 10:9 when the field is bit 9
 *     alone, rejecting every such instruction whose immediate had its
 *     high bit set -- half of them;
 *   - the whole 0xE8xx-0xEFxx range sat in a block the decoder could not
 *     enter, so `add.w r5, r5, r5` reported as unimplemented while its
 *     handler was a few lines away.
 *
 * The expected values are **not** computed by this program. They are
 * constants, worked out independently, because a test that derives its
 * expectation from the implementation encodes the bug as the
 * expectation -- which this project has already done once, to a CMOV
 * test that mirrored the compare it was checking.
 */
#define UART (*(volatile unsigned char *)0x10000000u)
static void pc_(char c)
{
    UART = (unsigned char)c;
}
static void ps_(const char *s)
{
    while (*s) {
        pc_(*s++);
    }
}
static void phex(unsigned v)
{
    for (int i = 28; i >= 0; i -= 4) {
        pc_("0123456789abcdef"[(v >> i) & 0xFu]);
    }
}
/* Not static, so the compiler cannot fold them away. */
unsigned g_a = 0xDEADBEEFu; /* needs MOVW+MOVT to materialise */
volatile unsigned g_b = 7u;
/*
 * What the answers are, independently of how the guest computes them.
 * 0xDEADBEEF and 7 chosen so the divide is not a power of two and the
 * rotate crosses the word boundary.
 */
#define WANT_A 0xD1A2B1E0u
#define WANT_B 0x00000020u
#define WANT_C 0xFFFFFFF9u
#define WANT_D 0x0006F56Du
#define WANT_E 0x56DF77EFu
#define WANT_F 0x1FCFAD8Fu
#define WANT_G 0x00000006u

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
    unsigned x = g_a ^ 0x0F0F0F0Fu; /* modified immediate with i set */
    unsigned y = g_b * 3u + 11u;
    int neg = -(int)g_b;
    unsigned sh = g_a >> 13;
    unsigned ro = (g_a << 7) | (g_a >> 25);

    /*
     * The encodings whose *awkward* input is the whole difficulty, and
     * which the ordinary arithmetic above does not reach. Written as
     * inline asm because a compiler will not be told which encoding to
     * use, and each of these was a real defect:
     *
     *   - a modified immediate with the `i` bit set. Every constant gcc
     *     chose had i == 0, which is the one value where reading bits
     *     10:9 and reading bit 9 alone agree -- so the test passed
     *     against the bug. 0x00FF0000 is rotation 16, imm12 0x87F.
     *   - MOVW and MOVT, which gcc avoided entirely in favour of a
     *     literal pool, and which were masked with a bit their pattern
     *     does not contain -- so neither ever decoded.
     */
    {
        unsigned t;

        __asm__ volatile("mov.w %0, #0x00FF0000" : "=r"(t));
        want("H", t, 0x00FF0000u);

        __asm__ volatile("eor.w %0, %1, #0x0000FF00" : "=r"(t) : "r"(0u));
        want("I", t, 0x0000FF00u);

        __asm__ volatile("movw %0, #0xBEEF\n\tmovt %0, #0xDEAD" : "=r"(t));
        want("J", t, 0xDEADBEEFu);

        /* MOVT must preserve the low half rather than replace it. */
        __asm__ volatile("movw %0, #0x1234\n\tmovt %0, #0x5678" : "=r"(t));
        want("K", t, 0x56781234u);
    }

    want("A", x, WANT_A);
    want("B", y, WANT_B);
    want("C", (unsigned)neg, WANT_C);
    want("D", sh, WANT_D);
    want("E", ro, WANT_E);
    want("F", g_a / g_b, WANT_F);
    want("G", g_a % g_b, WANT_G);

    /*
     * A terminator, so *running* is a precondition of passing. A guest
     * that stops early prints nothing and a test asserting "no FAIL
     * lines" would pass on it -- the defect this project found in its
     * PowerPC guest, where zero failures and zero execution were the
     * same observable.
     */
    if (g_fail == 0u) {
        ps_("ARMV7M-ALU-OK\n");
    }
    return (int)g_fail;
}
