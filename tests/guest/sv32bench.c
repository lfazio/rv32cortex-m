/* SPDX-License-Identifier: Apache-2.0 */
/*
 * sv32bench.c - what the inlined memory path is worth, and what turns it off.
 *
 * `rv_ir_fast_mem` lets a translated block reach guest RAM with a bounds
 * test and a host load instead of a call into `rv_ir_load`. It declines
 * whenever `vm_active`, `pmp_active` or `trig_active` is set -- and
 * **both of the first two are true for every guest running below
 * M-mode**, which is every operating system. `rv_pmp_refresh` sets
 * `pmp_active` from the privilege alone, before it looks at a single
 * entry, so a kernel gets no inlined access even with no PMP region
 * configured at all.
 *
 * So "the JIT is slow under Linux because of paging" is half the story
 * and mis-scoped as a work item: an inline TLB probe would leave the
 * second gate shut. This guest measures the size of the prize before
 * either is built.
 *
 * It is built twice from one source, differing in one `-D`:
 *
 *   sv32bench-bare   M-mode, satp Bare      -- the fast path is on
 *   sv32bench-paged  S-mode, satp Sv32      -- both gates shut
 *
 * The kernel between the markers is byte-identical in the two, and the
 * only thing that differs is the privilege and the translation. Compare
 * the emulator's own host-cycles and ratio lines across the two runs;
 * the guest's own retired count should agree to within the setup.
 *
 * **The identity map is deliberate.** A guest whose pages are scattered
 * would measure the TLB's hit rate as well, and the question here is
 * what an access costs when the walk is *not* the expensive part. This
 * is therefore an upper bound on what inlining can recover, which is
 * the number worth having before writing any.
 */

#include <stdint.h>

#ifndef SV32BENCH_PAGING
#define SV32BENCH_PAGING 1
#endif

#define UART_THR (*(volatile uint8_t *)0x10000000u)

#define csr_read(name)                                                         \
    ({                                                                         \
        uint32_t v_;                                                           \
        __asm__ volatile("csrr %0, " name : "=r"(v_));                         \
        v_;                                                                    \
    })

#define csr_write(name, v) __asm__ volatile("csrw " name ", %0" ::"r"(v))

static void puts_(const char *s)
{
    while (*s != '\0') {
        UART_THR = (uint8_t)*s++;
    }
}

static void putu(uint32_t v)
{
    char tmp[10];
    unsigned n = 0;

    do {
        tmp[n++] = (char)('0' + (v % 10u));
        v /= 10u;
    } while (v != 0u);
    while (n != 0u) {
        UART_THR = (uint8_t)tmp[--n];
    }
}

/* ------------------------------------------------------------------ */
/* The kernel                                                          */
/* ------------------------------------------------------------------ */

/*
 * Memory-bound on purpose: this exists to count loads and stores, not to
 * be a balanced workload. Every iteration is two loads, an add and a
 * store, which is the shape `rv_ir_fast_mem` either inlines or sends to
 * a helper -- so the difference between the two builds is as close to
 * "the cost of the call" as a guest can get.
 *
 * The stride walks whole pages so that a translated block sees more than
 * one mapping, which is what a real guest does; with a 4 MiB megapage
 * identity map it stays a TLB hit either way.
 */
#define N 4096u
static uint32_t g_a[N];
static uint32_t g_b[N];
static volatile uint32_t g_sink;

static uint32_t kernel(unsigned rounds)
{
    uint32_t acc = 0u;

    for (unsigned r = 0u; r < rounds; r++) {
        for (unsigned i = 0u; i < N; i++) {
            g_a[i] = g_a[i] + g_b[i] + r;
        }
        for (unsigned i = 0u; i < N; i += 17u) {
            acc += g_a[i];
        }
    }
    return acc;
}

static void run_kernel(void)
{
    /*
     * `instret`, not `minstret`. The machine counter is illegal below
     * M-mode whatever mcounteren says -- mcounteren governs access to
     * the *shadow* at 0xC02, not to the machine CSR at 0xB02 -- so the
     * paged build trapped on the first instruction of the measured
     * region. M-mode can read the shadow too, which is what lets both
     * builds share this line.
     */
    const uint32_t i0 = csr_read("instret");

    g_sink = kernel(400u);

    const uint32_t i1 = csr_read("instret");

    puts_("retired ");
    putu(i1 - i0);
    puts_("\n");
}

/* ------------------------------------------------------------------ */
/* Paging                                                              */
/* ------------------------------------------------------------------ */

#if SV32BENCH_PAGING

#define PTE_V 0x001u
#define PTE_R 0x002u
#define PTE_W 0x004u
#define PTE_X 0x008u
#define PTE_A 0x040u
#define PTE_D 0x080u
#define SATP_SV32 0x80000000u

#define MSTATUS_MPP_S 0x00000800u

static uint32_t g_root[1024] __attribute__((aligned(4096)));

/*
 * Where to resume, and the callee-saved state to resume with.
 *
 * S-mode is entered by `mret` and left by `ecall`, so the function's
 * prologue runs and its epilogue never does: sp comes back below where
 * it started and every callee-saved register still holds the callee's
 * value. Saving them here and reloading at the resume label is what
 * stops that presenting as the program restarting from the top.
 */
static volatile uint32_t g_regs[13];

/* What the handler caught, so main() can insist it was the ecall. */
volatile uint32_t g_cause;
volatile uint32_t g_epc;

static void smode_body(void)
{
    run_kernel();
    __asm__ volatile("ecall");
}

int main(void)
{
    puts_("SV32BENCH-START paged\n");

    /*
     * Identity map, 4 MiB megapages, every permission. A leaf at the
     * root level is a megapage: R, W or X set with V is what makes it
     * one rather than a pointer to a second level.
     */
    for (uint32_t i = 0u; i < 1024u; i++) {
        g_root[i] = (i << 20) | PTE_V | PTE_R | PTE_W | PTE_X | PTE_A | PTE_D;
    }

    /*
     * A background PMP entry, and it is not optional: below M-mode,
     * matching *no* entry denies rather than permits, so S-mode would
     * fault on its first fetch with no handler able to say so.
     *
     * Note what this costs and why the measurement is still honest:
     * `pmp_active` is set by privilege alone, so it would be true here
     * even without this entry. The fast path is off in this build for
     * two reasons, not one, and that is the point the file header makes.
     */
    csr_write("pmpaddr0", 0xFFFFFFFFu);
    csr_write("pmpcfg0", 0x0Fu); /* NAPOT, RWX, unlocked */

    /*
     * Let S-mode read the counters. `run_kernel` opens with a read of
     * minstret, which is an M-mode CSR: without this it raises
     * illegal-instruction on the *first instruction* of the measured
     * region, the handler below resumes, and the run prints its start
     * and end markers with no kernel between them. That is exactly what
     * the first version of this guest did -- 34,011 retired against
     * 531,496 -- and it is why the handler now reports the cause
     * instead of resuming quietly.
     */
    csr_write("mcounteren", 0xFFFFFFFFu);

    csr_write("satp", SATP_SV32 | ((uint32_t)(uintptr_t)g_root >> 12));

    uint32_t mstatus = csr_read("mstatus");

    mstatus = (mstatus & ~0x00001800u) | MSTATUS_MPP_S;
    csr_write("mstatus", mstatus);
    csr_write("mepc", (uint32_t)(uintptr_t)smode_body);

    /*
     * The resume point is a label *inside* the asm, between two
     * instructions the compiler already believes control reaches.
     * Handing a C label's address to the trap handler instead resumes
     * with a block the compiler was free to place ahead of the fault.
     */
    __asm__ volatile(
        "la    t0, g_regs\n"
        "sw    sp, 0(t0)\n"
        "sw    s0, 4(t0)\n"
        "sw    s1, 8(t0)\n"
        "la    t0, 1f\n"
        "csrw  mtvec, t0\n"
        "mret\n"
        ".align 2\n"
        "1:\n"
        "csrr  t1, mcause\n"
        "la    t0, g_cause\n"
        "sw    t1, 0(t0)\n"
        "csrr  t1, mepc\n"
        "la    t0, g_epc\n"
        "sw    t1, 0(t0)\n"
        "la    t0, g_regs\n"
        "lw    sp, 0(t0)\n"
        "lw    s0, 4(t0)\n"
        "lw    s1, 8(t0)\n" ::
            : "t0", "t1", "memory");

    /* Back in M-mode. Leave nothing armed behind us. */
    csr_write("satp", 0u);

    /*
     * **A trap only reports if something catches it.** Anything other
     * than the ecall this guest leaves S-mode with means the kernel did
     * not finish, and without this the run prints both markers and a
     * plausible-looking summary while having measured nothing.
     */
    if (g_cause != 9u) {
        puts_("SV32BENCH-TRAP cause ");
        putu(g_cause);
        puts_(" epc ");
        putu(g_epc);
        puts_("\n");
        return 1;
    }

    puts_("SV32BENCH-END\n");
    return 0;
}

#else /* !SV32BENCH_PAGING */

int main(void)
{
    puts_("SV32BENCH-START bare\n");
    run_kernel();
    puts_("SV32BENCH-END\n");
    return 0;
}

#endif
