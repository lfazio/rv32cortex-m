/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core_portme.c - CoreMark port for an rv32cortex-m guest.
 *
 * Replaces CoreMark's barebones template, whose barebones_clock() is a
 * deliberate #error. Timing comes from the emulated CLINT's mtime, which
 * on the STM32 platform is driven from the ARM DWT cycle counter at 1 MHz,
 * and output goes to the virtual console UART.
 *
 * Built with HAS_FLOAT=0 so nothing here needs soft-float support: guest
 * images link -nostdlib, so pulling in the
 * floating-point helpers would mean linking libgcc for the sake of
 * printing one number. CoreMark reports ticks instead, and the iteration
 * count and tick rate are enough to compute the score.
 *
 * NOTE ON SCORES: a run this short is not a reportable CoreMark score.
 * EEMBC requires at least 10 seconds of execution and a specific
 * disclosure format. What is measured here is the emulator, not the
 * silicon, and the number is only meaningful next to the same workload run
 * natively.
 */

#include "coremark.h"

#if VALIDATION_RUN
volatile ee_s32 seed1_volatile = 0x3415;
volatile ee_s32 seed2_volatile = 0x3415;
volatile ee_s32 seed3_volatile = 0x66;
#endif
#if PERFORMANCE_RUN
volatile ee_s32 seed1_volatile = 0x0;
volatile ee_s32 seed2_volatile = 0x0;
volatile ee_s32 seed3_volatile = 0x66;
#endif
#if PROFILE_RUN
volatile ee_s32 seed1_volatile = 0x8;
volatile ee_s32 seed2_volatile = 0x8;
volatile ee_s32 seed3_volatile = 0x8;
#endif
volatile ee_s32 seed4_volatile = ITERATIONS;
volatile ee_s32 seed5_volatile = 0;

ee_u32 default_num_contexts = 1;

/* ------------------------------------------------------------------ */
/* Platform                                                            */
/* ------------------------------------------------------------------ */

#define UART_THR   (*(volatile unsigned char *)0x10000000u)
#define CLINT_MTIME_LO (*(volatile ee_u32 *)0x0200BFF8u)

static CORETIMETYPE start_time_val, stop_time_val;

#define GETMYTIME(_t)        (*_t = CLINT_MTIME_LO)
#define MYTIMEDIFF(fin, ini) ((fin) - (ini))

void start_time(void)
{
    GETMYTIME(&start_time_val);
}

void stop_time(void)
{
    GETMYTIME(&stop_time_val);
}

CORE_TICKS get_time(void)
{
    return (CORE_TICKS)MYTIMEDIFF(stop_time_val, start_time_val);
}

secs_ret time_in_secs(CORE_TICKS ticks)
{
    /* HAS_FLOAT=0, so secs_ret is integral and this is whole seconds. */
    return (secs_ret)(ticks / EE_TICKS_PER_SEC);
}

void portable_init(core_portable *p, int *argc, char *argv[])
{
    (void)argc;
    (void)argv;
    p->portable_id = 1;
}

void portable_fini(core_portable *p)
{
    p->portable_id = 0;
}

/* ------------------------------------------------------------------ */
/* Output                                                              */
/* ------------------------------------------------------------------ */

void uart_send_char(char c)
{
    UART_THR = (unsigned char)c;
}

/*
 * CoreMark ships an ee_printf, but its barebones copy *defines*
 * uart_send_char with a deliberate #error in the body, so the file cannot
 * be compiled without editing upstream source. Supplying a small printf
 * here keeps the checkout pristine -- which matters, because CoreMark's
 * licence governs how results are reported -- and drops 15 KB of code that
 * only ever prints a handful of integers and strings.
 *
 * Supports the conversions CoreMark actually uses: %d %i %u %x %X %s %c %%,
 * with optional field width, zero padding and an ignored 'l' modifier.
 */
static void emit_str(const char *s)
{
    while (*s != '\0') {
        uart_send_char(*s++);
    }
}

static void emit_num(ee_u32 v, unsigned base, int is_signed,
                     int width, int zero_pad, int upper)
{
    char tmp[12];
    int n = 0;
    int neg = 0;

    if (is_signed && (ee_s32)v < 0) {
        neg = 1;
        v = (ee_u32)(-(ee_s32)v);
    }
    do {
        const unsigned d = v % base;
        tmp[n++] = (char)(d < 10u ? ('0' + d) : ((upper ? 'A' : 'a') + d - 10u));
        v /= base;
    } while (v != 0u);

    if (neg) {
        tmp[n++] = '-';
    }
    for (int pad = width - n; pad > 0; pad--) {
        uart_send_char(zero_pad ? '0' : ' ');
    }
    while (n != 0) {
        uart_send_char(tmp[--n]);
    }
}

int ee_printf(const char *fmt, ...)
{
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);

    for (const char *p = fmt; *p != '\0'; p++) {
        if (*p != '%') {
            uart_send_char(*p);
            continue;
        }
        p++;

        int zero_pad = 0;
        if (*p == '0') {
            zero_pad = 1;
            p++;
        }
        int width = 0;
        while (*p >= '0' && *p <= '9') {
            width = width * 10 + (*p++ - '0');
        }
        while (*p == 'l' || *p == 'h') {
            p++;               /* RV32: long and int are the same width */
        }

        switch (*p) {
        case 'd':
        case 'i':
            emit_num((ee_u32)__builtin_va_arg(ap, int), 10u, 1, width, zero_pad, 0);
            break;
        case 'u':
            emit_num(__builtin_va_arg(ap, ee_u32), 10u, 0, width, zero_pad, 0);
            break;
        case 'x':
            emit_num(__builtin_va_arg(ap, ee_u32), 16u, 0, width, zero_pad, 0);
            break;
        case 'X':
            emit_num(__builtin_va_arg(ap, ee_u32), 16u, 0, width, zero_pad, 1);
            break;
        case 'p':
            emit_str("0x");
            emit_num((ee_u32)(ee_ptr_int)__builtin_va_arg(ap, void *), 16u, 0,
                     8, 1, 0);
            break;
        case 's':
            emit_str(__builtin_va_arg(ap, const char *));
            break;
        case 'c':
            uart_send_char((char)__builtin_va_arg(ap, int));
            break;
        case '%':
            uart_send_char('%');
            break;
        default:
            uart_send_char('%');
            uart_send_char(*p);
            break;
        }
    }

    __builtin_va_end(ap);
    return 0;
}
