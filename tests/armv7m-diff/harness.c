/* SPDX-License-Identifier: Apache-2.0 */
/*
 * harness.c - run every generated case and print what it left behind.
 *
 * One source for both targets. Nothing here may depend on where it runs:
 * the initial state comes from a PRNG seeded per case, so the emulator and
 * the board start each case from identical registers, flags and memory,
 * and any difference in the printed line is a difference in how the
 * instruction executed.
 *
 * **The special values are the point of the random ones.** A random
 * 32-bit operand almost never is 0, 0x80000000 or 0xFFFFFFFF, and those
 * are where carry, overflow, saturation and the division corner cases
 * live -- this project's own rule is to test the awkward input, not a
 * representative one. A quarter of all operands come from the table.
 */

#include "diff.h"

uint8_t diff_mem[DIFF_MEM_BYTES] __attribute__((aligned(16)));

static uint32_t g_rng;

static uint32_t rnd(void)
{
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return g_rng;
}

static const uint32_t k_special[] = {
    0x00000000u, 0x00000001u, 0x00000002u, 0x7FFFFFFFu, 0x80000000u,
    0xFFFFFFFFu, 0x80000001u, 0xFFFFFFFEu, 0x0000FFFFu, 0x00010000u,
    0x00008000u, 0xFFFF8000u, 0x00007FFFu, 0x7FFF8000u, 0x8000FFFFu,
    0x0000001Fu, 0x00000020u, 0x00000021u, 0x0000007Fu, 0x00000080u,
    0x000000FFu, 0x00000100u, 0x80808080u, 0x7F7F7F7Fu, 0x01010101u,
};

static uint32_t value(void)
{
    const uint32_t r = rnd();

    if ((r & 3u) == 0u) {
        return k_special[(r >> 2) % (sizeof(k_special) / sizeof(k_special[0]))];
    }
    return rnd();
}

/*
 * Single-precision operands, weighted the same way: the zeros, the
 * infinities, both kinds of NaN with payloads, the subnormals and the
 * extremes are where an FPU's rules are, and a random bit pattern is a
 * normal number almost every time.
 */
static const uint32_t k_fspecial[] = {
    0x00000000u, 0x80000000u, 0x3F800000u, 0xBF800000u, 0x7F800000u,
    0xFF800000u, 0x7FC00000u, 0xFFC00000u, 0x7FC12345u, 0x7F812345u,
    0xFF800001u, 0x00000001u, 0x807FFFFFu, 0x00400000u, 0x00800000u,
    0x7F7FFFFFu, 0xFF7FFFFFu, 0x3F000000u, 0x4B000000u, 0x4F000000u,
    0xCF000000u, 0x4EFFFFFFu, 0x3FC00000u, 0x40490FDBu, 0x00800001u,
};

static uint32_t fvalue(void)
{
    const uint32_t r = rnd();

    if ((r & 1u) == 0u) {
        return k_fspecial[(r >> 1) %
                          (sizeof(k_fspecial) / sizeof(k_fspecial[0]))];
    }
    return rnd();
}

/* The memory a load sees: a fixed function of the offset. */
static uint8_t mem_pattern(uint32_t i)
{
    return (uint8_t)((i * 2654435761u) >> 24);
}

static void putx(uint32_t v)
{
    for (int i = 28; i >= 0; i -= 4) {
        diff_putc("0123456789abcdef"[(v >> i) & 0xFu]);
    }
}

static void puts_(const char *s)
{
    while (*s != '\0') {
        diff_putc(*s++);
    }
}

static void putsp(uint32_t v)
{
    diff_putc(' ');
    putx(v);
}

/* ------------------------------------------------------------------ */
/* Faults                                                              */
/* ------------------------------------------------------------------ */

#define REG(a) (*(volatile uint32_t *)(a))
#define VTOR REG(0xE000ED08u)
#define SHCSR REG(0xE000ED24u)
#define CFSR REG(0xE000ED28u)
#define HFSR REG(0xE000ED2Cu)

static volatile uint32_t g_case;
static volatile uint32_t g_fault;    /* CFSR at the fault, or zero        */
static volatile uint32_t g_fault_at; /* its pc, relative to the probe     */

/*
 * A fault inside a raw case is the case's result: record it and resume
 * after the four bytes. The status registers are sticky, so they are
 * cleared here or every later case would report this one's fault.
 *
 * The pc is recorded relative to the probe and is zero whenever the
 * encoding itself was refused. Anything else means it *ran* -- as a
 * branch, to somewhere that then faulted -- which is a much more
 * interesting answer than "it faulted" and must not look the same.
 *
 * A fault anywhere else is the harness's own, or a case that was never
 * legal, and there is no sensible way to carry on from it.
 */
void diff_fault(uint32_t *frame);
void diff_fault(uint32_t *frame)
{
    const uint32_t n = g_case;
    const uint32_t at = (n < diff_ncases) ? diff_meta[n].probe : 0u;

    if (at != 0u) {
        g_fault = CFSR;
        g_fault_at = frame[6] - at;
        CFSR = g_fault;
        HFSR = HFSR;
        frame[6] = at + 4u;
        frame[7] = (frame[7] & ~0x0600FC00u) | (1u << 24); /* Thumb, no IT */
        return;
    }
    puts_("\nDIFF-FAULT case ");
    putx(n);
    puts_(" pc ");
    putx(frame[6]);
    puts_(" cfsr ");
    putx(CFSR);
    puts_(" hfsr ");
    putx(HFSR);
    puts_("\nDIFF-END\n");
    diff_stop();
}

/* Thread mode here always runs on the main stack, so the frame is at sp. */
__attribute__((naked)) static void fault_entry(void)
{
    __asm__ volatile("mov r0, sp\n"
                     "push {r4, lr}\n"
                     "bl diff_fault\n"
                     "pop {r4, pc}\n");
}

static void (*g_vtab[16])(void) __attribute__((aligned(128)));

int main(void)
{
    static diff_state_t st;

    for (uint32_t i = 0u; i < DIFF_MEM_BYTES; i++) {
        diff_mem[i] = mem_pattern(i);
    }

    /*
     * CP10 and CP11 on, here rather than in either start-up: the
     * emulator guest's start.S leaves CPACR at its reset value, and the
     * first FP case then took -- correctly -- a NOCP UsageFault.
     */
    *(volatile uint32_t *)0xE000ED88u |= 0xFu << 20;
    __asm__ volatile("dsb\n isb" ::: "memory");

    /*
     * Our own vector table, the same on both targets: neither start-up's
     * fault vector knows how to resume a case. And the three configurable
     * faults enabled, so that what a raw case reports is the fault it
     * took and not a HardFault it escalated to.
     */
    for (uint32_t i = 0u; i < sizeof(g_vtab) / sizeof(g_vtab[0]); i++) {
        g_vtab[i] = fault_entry;
    }
    VTOR = (uint32_t)(uintptr_t)g_vtab;
    SHCSR |= 7u << 16;
    __asm__ volatile("dsb\n isb" ::: "memory");

    puts_("DIFF-BEGIN ");
    putx(diff_seed);
    putsp(diff_ncases);
    diff_putc('\n');

    for (uint32_t n = 0u; n < diff_ncases; n++) {
        const diff_meta_t *const m = &diff_meta[n];
        const uint32_t base = (uint32_t)(uintptr_t)diff_mem;

        g_rng = (diff_seed ^ (n * 0x9E3779B9u)) | 1u;
        for (uint32_t r = 0u; r < 13u; r++) {
            st.r[r] = value();
        }
        /* NZCVQ and the four GE bits; nothing else is writable. */
        st.apsr = rnd() & 0xF80F0000u;
        for (uint32_t r = 0u; r < 32u; r++) {
            st.s[r] = fvalue();
        }
        /*
         * A random rounding mode, flush-to-zero and default-NaN in one
         * case in four, and the cumulative flags clear so that what a
         * case raised is visible.
         */
        st.fpscr = ((rnd() & 3u) == 0u) ? (rnd() & 0x07C00000u) : 0u;

        g_fault = 0u;
        g_fault_at = 0u;
        g_case = n;
        diff_cases[n](&st);
        g_case = 0xFFFFFFFFu;

        diff_putc('c');
        putsp(n);
        for (uint32_t r = 0u; r < 13u; r++) {
            const uint32_t v = st.r[r];

            putsp(((m->addr_mask >> r) & 1u) != 0u ? v - base : v);
        }
        putsp(st.apsr);

        /* FNV-1a over the window the case declared, then restore it. */
        {
            uint32_t h = 2166136261u;

            for (uint32_t i = m->mem_lo; i < m->mem_hi; i++) {
                h = (h ^ diff_mem[i]) * 16777619u;
                diff_mem[i] = mem_pattern(i);
            }
            putsp(h);
        }

        if ((m->flags & DIFF_FLAG_FP) != 0u) {
            for (uint32_t r = 0u; r < 32u; r++) {
                putsp(st.s[r]);
            }
            putsp(st.fpscr);
        }
        if ((m->flags & DIFF_FLAG_RAW) != 0u) {
            putsp(g_fault);
            putsp(g_fault_at);
        }
        diff_putc('\n');
    }

    puts_("DIFF-END\n");
    return 0;
}
