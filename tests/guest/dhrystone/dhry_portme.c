/* SPDX-License-Identifier: Apache-2.0 */
/*
 * dhry_portme.c - the libc Dhrystone 2.1 expects, for a -nostdlib guest.
 *
 * dhry_1.c and dhry_2.c are netlib's, unmodified
 * (https://www.netlib.org/benchmark/dhry-c). Everything the benchmark
 * needs from a hosted environment is supplied here instead, which keeps
 * the measured code pristine -- the same reason CoreMark is built from an
 * upstream checkout rather than vendored with edits.
 *
 * What it needs, and what is done about it:
 *
 *   printf    a tiny subset: %d, %c, %s and literals. Dhrystone's format
 *             strings are fixed, so the subset is complete for them and
 *             deliberately not more.
 *   scanf     never called -- the run count is compiled in. See below.
 *   malloc    a bump allocator over a static arena. Dhrystone allocates
 *             exactly twice and never frees, so free() is a no-op and
 *             that is not a simplification but the whole requirement.
 *   time      the guest cycle counter, which is what this platform has.
 *
 * **The number of runs is compiled in, not read.** Upstream prompts for
 * it with scanf, and a guest with no console input would block forever
 * or, worse, read whatever a stub returned. DHRY_RUNS is passed by the
 * build so the figure is visible in the build log rather than typed at a
 * prompt that does not exist.
 */

/*
 * Written in gnu89 like the sources it serves: the whole image is
 * compiled that way because netlib's Dhrystone is 1988 K&R, so this file
 * cannot use declarations-after-statements or in-loop declarations
 * either. Keeping one standard for the image is simpler than two.
 */
#include <stdint.h>
#include <stdarg.h>
#include <stddef.h>

#define UART_THR   (*(volatile uint8_t *)0x10000000u)

static void out_char(char ch)
{
    UART_THR = (uint8_t)ch;
}

static void out_str(const char *s)
{
    while (*s != '\0') {
        out_char(*s++);
    }
}

static void out_dec(long v)
{
    char tmp[12];
    unsigned n = 0;
    unsigned long u;

    if (v < 0) {
        out_char('-');
        u = (unsigned long)(-v);
    } else {
        u = (unsigned long)v;
    }
    do {
        tmp[n++] = (char)('0' + (u % 10ul));
        u /= 10ul;
    } while (u != 0ul && n < sizeof(tmp));
    while (n-- != 0u) {
        out_char(tmp[n]);
    }
}

/*
 * %f, to one decimal place, from a double.
 *
 * Dhrystone's two result lines are the only floating-point output, and
 * they arrive as doubles through varargs promotion. Formatting them here
 * rather than pulling in a real printf keeps the image free of a
 * formatted-output library on three toolchains -- and the value has
 * already been computed by then, so nothing about the measurement
 * depends on this being fast.
 */
static void out_fixed1(double v)
{
    long whole;
    long frac;

    if (v < 0.0) {
        out_char('-');
        v = -v;
    }
    whole = (long)v;
    frac  = (long)((v - (double)whole) * 10.0 + 0.5);
    if (frac >= 10) {          /* 9.96 rounds to 10.0, not 9.10 */
        whole += 1;
        frac = 0;
    }
    out_dec(whole);
    out_char('.');
    out_dec(frac);
}

/*
 * The format subset Dhrystone actually uses. Anything else is passed
 * through literally rather than silently dropped, so an unsupported
 * conversion shows up in the output instead of vanishing.
 */
int printf(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    const char *p;

    for (p = fmt; *p != '\0'; p++) {
        if (*p != '%') {
            out_char(*p);
            continue;
        }
        /*
         * Field widths and precisions are parsed and discarded:
         * Dhrystone writes "%6.1f", and the number matters where the
         * column alignment does not.
         */
        p++;
        while ((*p >= '0' && *p <= '9') || *p == '.' || *p == '-') {
            p++;
        }
        switch (*p) {
        case 'f': out_fixed1(va_arg(ap, double));       break;
        case 'd': out_dec((long)va_arg(ap, int));       break;
        case 'l':
            /* "%ld" -- the only length modifier in these sources. */
            if (p[1] == 'd') { p++; out_dec(va_arg(ap, long)); }
            else             { out_char('%'); out_char('l'); }
            break;
        case 'c': out_char((char)va_arg(ap, int));      break;
        case 's': out_str(va_arg(ap, const char *));    break;
        case '%': out_char('%');                        break;
        case '\0': out_char('%'); p--;                  break;
        default:  out_char('%'); out_char(*p);          break;
        }
    }
    va_end(ap);
    return 0;
}

/*
 * A bump allocator. Dhrystone calls malloc twice, for one Rec_Type each,
 * and never frees -- so this is the complete requirement rather than a
 * corner cut. Running out returns NULL, which Dhrystone does not check,
 * so the arena is sized with room to spare and a guest that overran it
 * would fault on the null dereference rather than corrupt itself
 * quietly.
 */
#define ARENA_BYTES 1024u
static uint8_t g_arena[ARENA_BYTES];
static unsigned g_arena_used;

void *malloc(size_t n)
{
    n = (n + 7u) & ~(size_t)7u;          /* keep the arena 8-aligned */
    if (g_arena_used + n > ARENA_BYTES) {
        return NULL;
    }
    {
        void *p = &g_arena[g_arena_used];
        g_arena_used += (unsigned)n;
        return p;
    }
}

void free(void *p)
{
    (void)p;
}

/*
 * The clock. `TIME` is defined by the build, so dhry_1.c calls time()
 * and divides by HZ; both come from here.
 *
 * mtime counts at the platform's timer rate, not the CPU clock, which is
 * why HZ is what it is and why the reported DMIPS is a *guest* figure.
 * What this benchmark is for here is comparing frontends and backends
 * against each other, so the absolute number matters less than that both
 * sides of a comparison use the same clock.
 */
#define MTIME_LO   (*(volatile uint32_t *)0x0200BFF8u)

long time(long *t)
{
    long now = (long)MTIME_LO;

    if (t != NULL) {
        *t = now;
    }
    return now;
}

/* Dhrystone's only string calls, and both are on the measured path --
 * so they are written out here rather than pulled from a libc whose
 * implementation would vary between the three toolchains this has to
 * build with. */
char *strcpy(char *d, const char *s)
{
    char *r = d;
    while ((*d++ = *s++) != '\0') { }
    return r;
}

int strcmp(const char *a, const char *b)
{
    while (*a != '\0' && *a == *b) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}
