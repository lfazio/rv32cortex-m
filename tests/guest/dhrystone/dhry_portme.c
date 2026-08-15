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
 *   times     the CLINT's mtime, at 1 MHz. Which clock that *is* differs
 *             between the host and the board, and it changes what the
 *             DMIPS figure means -- see the note above the function.
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

/* The shim beside this file, not the toolchain's -- see sys/times.h. */
#include <sys/times.h>

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
 * The clock.
 *
 * Dhrystone offers two: time(), which must return whole *seconds*, and
 * times(), which returns ticks at a rate the build declares as HZ. This
 * guest uses times(), and the reason is resolution. The CLINT runs at
 * 1 MHz, a run is a few seconds of guest time, and time() would quantise
 * that to one part in a few -- the first version of this file did exactly
 * that and reported 470016000 microseconds per run, because with TIME
 * selected Dhrystone divides by nothing and takes the raw tick count for
 * a count of seconds. HZ is not even referenced on that path.
 *
 * mtime is the same 1 MHz on every platform, which is what makes HZ a
 * constant here rather than something the build has to discover. What
 * differs between platforms is what that clock *is*, and it changes what
 * the resulting figure means:
 *
 *   on the board   mtime is derived from the DWT cycle counter, so it is
 *                  real elapsed time and the DMIPS figure is the emulated
 *                  system's -- interpreter and JIT differ, and that
 *                  difference is the emulator's throughput.
 *
 *   on the host    guest time advances one tick per retired instruction
 *                  (see --timer-hz), so a "second" is a million guest
 *                  instructions. The figure is then a property of the
 *                  guest binary at an assumed IPC of 1: it says how much
 *                  work the frontend's compiler gets done per guest
 *                  clock, and says nothing whatever about how fast this
 *                  emulator runs. Comparing backends with it is the
 *                  mistake it invites -- interpreter and JIT agree to the
 *                  last digit, as they must.
 *
 *                  Not *bit*-identical, though, and the reason is worth
 *                  knowing before reading a difference as a result: the
 *                  run loop advances guest time once per round rather
 *                  than per instruction, and the JIT's rounds end on a
 *                  block boundary rather than exactly on the budget. So
 *                  the tick count at a given guest instruction can differ
 *                  by up to one round. Measured across a five-fold change
 *                  in DHRY_RUNS it moves the last digit of the
 *                  per-second figure and nothing above it.
 *
 * Neither is comparable with a published DMIPS number, and the second is
 * the one that reads as though it were. Dhrystone does not print DMIPS at
 * all: that is this figure over 1757, the VAX 11/780's rate.
 */
#define MTIME_LO   (*(volatile uint32_t *)0x0200BFF8u)

/*
 * Declared `extern int times ();` by dhry_1.c and not declared at all by
 * the sys/times.h beside this file -- see the note there for why the
 * return types are allowed to disagree. Only tms_utime is ever read.
 */
long times(struct tms *buf)
{
    long now = (long)MTIME_LO;

    buf->tms_utime  = now;
    buf->tms_stime  = 0;
    buf->tms_cutime = 0;
    buf->tms_cstime = 0;
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
