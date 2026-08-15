/* SPDX-License-Identifier: Apache-2.0 */
/*
 * whetstone_portme.c - the platform Whetstone 1.2 expects, for a
 * -nostdlib guest.
 *
 * Same arrangement as dhry_portme.c beside it: everything the benchmark
 * needs from a hosted environment lives here so the measured code stays
 * as vendored. What it needs:
 *
 *   printf         %d, %ld, %lu, %s, a real %w.pf and %e -- the report
 *                  prints three decimal places and POUT prints four in
 *                  scientific notation, so the one-decimal formatter
 *                  Dhrystone gets is not enough here.
 *   whet_micros    the CLINT's mtime, which is 1 MHz on every platform.
 *   the math       sinf, cosf, atanf, logf, expf and sqrtf come from the
 *                  toolchain -- see below, because that is the one thing
 *                  this guest cannot supply for itself.
 *
 * **The loop count is compiled in.** Upstream takes it from argv; a guest
 * has no argv and no console input, so WHET_LOOPS comes from the build
 * and appears both in the build log and in the output.
 *
 * ---------------------------------------------------------------------
 * Why this one links against the toolchain's libc and the others do not
 * ---------------------------------------------------------------------
 *
 * Modules 7 and 11 *are* transcendental functions -- that is what
 * Whetstone is for -- so writing them here would replace a third of the
 * benchmark with something hand-rolled, and the figure would measure that
 * instead. newlib's are used, and on this multilib they are inside
 * libc.a rather than libm.a (libm.a is a stub holding one empty object),
 * so the link names -lc.
 *
 * That pulls in only the objects reached from those six symbols. Nothing
 * else here calls a libc function -- printf below is this file's own, and
 * would collide with newlib's if the linker ever had cause to pull that
 * in.
 */

#include <stdint.h>
#include <stdarg.h>
#include <stddef.h>

#define UART_THR   (*(volatile uint8_t *)0x10000000u)

/*
 * mtime, at 1 MHz. On the host it advances one tick per retired guest
 * instruction, so a "microsecond" is one guest instruction and the MIPS
 * figure is a property of the guest binary at an assumed IPC of 1 --
 * deterministic, and the same under either backend. On the board it is
 * derived from the DWT cycle counter and is real elapsed time, so there
 * the same image measures this emulator's throughput. The full argument
 * is in dhry_portme.c, which has the same split.
 */
#define MTIME_LO   (*(volatile uint32_t *)0x0200BFF8u)

unsigned long whet_micros(void)
{
    return (unsigned long)MTIME_LO;
}

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

static void out_udec(unsigned long u)
{
    char tmp[24];
    unsigned n = 0;

    do {
        tmp[n++] = (char)('0' + (u % 10ul));
        u /= 10ul;
    } while (u != 0ul && n < sizeof(tmp));
    while (n-- != 0u) {
        out_char(tmp[n]);
    }
}

static void out_dec(long v)
{
    if (v < 0) {
        out_char('-');
        out_udec((unsigned long)(-v));
    } else {
        out_udec((unsigned long)v);
    }
}

/*
 * %f to `prec` decimal places.
 *
 * The scale is built by repeated multiplication rather than by a table,
 * so prec is not silently capped at whatever a table happened to hold --
 * the report asks for six places and a three-entry table would have
 * printed something plausible and wrong. Values here are at most a few
 * thousand, so a double holds the scaled integer exactly and no rounding
 * of the *printing* enters the result.
 */
static void out_fixed(double v, unsigned prec)
{
    double scale = 1.0;
    unsigned long whole;
    unsigned long frac;
    unsigned i;

    if (v < 0.0) {
        out_char('-');
        v = -v;
    }
    for (i = 0; i < prec; i++) {
        scale *= 10.0;
    }

    whole = (unsigned long)v;
    frac  = (unsigned long)((v - (double)whole) * scale + 0.5);
    if (frac >= (unsigned long)scale) {   /* 9.9996 at prec 3 is 10.000 */
        whole += 1ul;
        frac = 0ul;
    }

    out_udec(whole);
    if (prec == 0u) {
        return;
    }
    out_char('.');
    /* Leading zeros of the fraction, which out_udec would drop. */
    for (i = prec, scale /= 10.0; i > 1u; i--, scale /= 10.0) {
        if ((double)frac >= scale) {
            break;
        }
        out_char('0');
    }
    out_udec(frac);
}

/*
 * %e, which POUT uses for the per-module digest under PRINTOUT.
 *
 * The exponent is found by scaling, not by logf: this file must not need
 * the math library to print, or a link error in the benchmark's own
 * arithmetic would arrive as a link error here instead. Scaling by 10
 * repeatedly is not exact, but it is the *same* inexactness on every
 * platform running this image, which is what the digest is compared
 * across -- and the mantissa is printed to four places, well inside it.
 */
static void out_exp(double v, unsigned prec)
{
    int exp10 = 0;

    if (v < 0.0) {
        out_char('-');
        v = -v;
    }
    if (v != 0.0) {
        while (v >= 10.0) { v /= 10.0; exp10++; }
        while (v <  1.0)  { v *= 10.0; exp10--; }
    }

    out_fixed(v, prec);
    out_char('e');
    if (exp10 < 0) {
        out_char('-');
        exp10 = -exp10;
    } else {
        out_char('+');
    }
    if (exp10 < 10) {
        out_char('0');
    }
    out_udec((unsigned long)exp10);
}

/*
 * The format subset Whetstone uses, with width parsed and discarded and
 * precision honoured. Anything unrecognised is printed literally, so an
 * unsupported conversion is visible rather than silently dropped.
 */
int printf(const char *fmt, ...)
{
    va_list ap;
    const char *p;

    va_start(ap, fmt);

    for (p = fmt; *p != '\0'; p++) {
        unsigned prec;
        int is_long;

        if (*p != '%') {
            out_char(*p);
            continue;
        }
        p++;

        while (*p == '-' || *p == '+' || *p == ' ' || *p == '0') {
            p++;
        }
        while (*p >= '0' && *p <= '9') {         /* width, discarded */
            p++;
        }
        prec = 6u;                                /* C's default for %f */
        if (*p == '.') {
            p++;
            prec = 0u;
            while (*p >= '0' && *p <= '9') {
                prec = prec * 10u + (unsigned)(*p - '0');
                p++;
            }
        }
        is_long = 0;
        while (*p == 'l') {
            is_long = 1;
            p++;
        }

        switch (*p) {
        case 'f':
            /* float arguments promote to double through varargs. */
            out_fixed(va_arg(ap, double), prec);
            break;
        case 'e':
            out_exp(va_arg(ap, double), prec);
            break;
        case 'd':
            out_dec(is_long ? va_arg(ap, long) : (long)va_arg(ap, int));
            break;
        case 'u':
            out_udec(is_long ? va_arg(ap, unsigned long)
                             : (unsigned long)va_arg(ap, unsigned int));
            break;
        case 's':
            out_str(va_arg(ap, const char *));
            break;
        case 'c':
            out_char((char)va_arg(ap, int));
            break;
        case '%':
            out_char('%');
            break;
        case '\0':
            out_char('%');
            p--;
            break;
        default:
            out_char('%');
            out_char(*p);
            break;
        }
    }

    va_end(ap);
    return 0;
}
