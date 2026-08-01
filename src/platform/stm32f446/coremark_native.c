/* SPDX-License-Identifier: Apache-2.0 */
/*
 * coremark_native.c - CoreMark port for the ARM host itself.
 *
 * Runs the *same* CoreMark sources the guest runs, compiled for Cortex-M4
 * and executed directly. That is what turns the emulator's numbers into a
 * meaningful figure: interpreter and JIT can only be compared to each
 * other until there is a native baseline for the same workload on the same
 * silicon.
 *
 * CoreMark's core_main.c defines main(), so it is compiled with
 * -Dmain=coremark_native_main and called from the firmware's own main.
 *
 * Timing uses the DWT cycle counter scaled to 1 MHz, matching the tick
 * rate the guest sees from the emulated CLINT, so "Total ticks" means the
 * same thing in both.
 */

#include "coremark.h"

#include "stm32f4xx_hal.h"

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
volatile ee_s32 seed4_volatile = ITERATIONS;
volatile ee_s32 seed5_volatile = 0;

ee_u32 default_num_contexts = 1;

/* Provided by main.c. */
void rv_console_putc(uint8_t c);

static CORETIMETYPE start_time_val, stop_time_val;

static CORETIMETYPE now_us(void)
{
    return (CORETIMETYPE)(DWT->CYCCNT / (SystemCoreClock / 1000000u));
}

void start_time(void) { start_time_val = now_us(); }
void stop_time(void)  { stop_time_val = now_us(); }

CORE_TICKS get_time(void)
{
    return (CORE_TICKS)(stop_time_val - start_time_val);
}

secs_ret time_in_secs(CORE_TICKS ticks)
{
    return (secs_ret)(ticks / EE_TICKS_PER_SEC);
}

void portable_init(core_portable *p, int *argc, char *argv[])
{
    (void)argc;
    (void)argv;
    p->portable_id = 1;
}

void portable_fini(core_portable *p) { p->portable_id = 0; }

void uart_send_char(char c)
{
    rv_console_putc((uint8_t)c);
}

/* ------------------------------------------------------------------ */
/* A small printf, shared in spirit with the guest port                */
/* ------------------------------------------------------------------ */

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
    int n = 0, neg = 0;

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
        if (*p == '0') { zero_pad = 1; p++; }
        int width = 0;
        while (*p >= '0' && *p <= '9') { width = width * 10 + (*p++ - '0'); }
        while (*p == 'l' || *p == 'h') { p++; }

        switch (*p) {
        case 'd': case 'i':
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
            emit_num((ee_u32)(ee_ptr_int)__builtin_va_arg(ap, void *), 16u, 0, 8, 1, 0);
            break;
        case 's': emit_str(__builtin_va_arg(ap, const char *)); break;
        case 'c': uart_send_char((char)__builtin_va_arg(ap, int)); break;
        case '%': uart_send_char('%'); break;
        default:  uart_send_char('%'); uart_send_char(*p); break;
        }
    }
    __builtin_va_end(ap);
    return 0;
}
