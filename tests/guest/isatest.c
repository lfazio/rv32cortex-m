/* SPDX-License-Identifier: Apache-2.0 */
/*
 * isatest.c - RV32 self-test, run as a guest inside the emulator.
 *
 * This is the primary correctness check for the core. It runs identically
 * on the host runner and on the STM32 firmware, so a result that differs
 * between them points at the platform layer rather than the interpreter.
 *
 * Values are pushed through opaque() before use so the compiler cannot
 * constant-fold the operation under test: without it, most of these checks
 * would be evaluated at compile time and would prove nothing about the
 * emulator.
 */

#include <stdint.h>

/*
 * Whether this build has PMP, passed by tests/guest/CMakeLists.txt as
 * RV32_MISALIGNED is. Without it the PMP CSRs do not exist, nothing
 * below M-mode is denied, and the tests that use PMP to make something
 * fault -- including the one that relies on a fetch fault to get out of
 * S-mode -- are replaced by one that the CSRs trap.
 */
#ifndef RV32_EXT_PMP
#define RV32_EXT_PMP 1
#endif

/* ------------------------------------------------------------------ */
/* Platform                                                            */
/* ------------------------------------------------------------------ */

#define UART_BASE 0x10000000u
#define UART_THR (*(volatile uint8_t *)(UART_BASE + 0x00u))
#define UART_LSR (*(volatile uint8_t *)(UART_BASE + 0x05u))

#define CLINT_BASE 0x02000000u
#define CLINT_MTIMECMP_LO (*(volatile uint32_t *)(CLINT_BASE + 0x4000u))
#define CLINT_MTIMECMP_HI (*(volatile uint32_t *)(CLINT_BASE + 0x4004u))
#define CLINT_MTIME_LO (*(volatile uint32_t *)(CLINT_BASE + 0xBFF8u))
#define CLINT_MTIME_HI (*(volatile uint32_t *)(CLINT_BASE + 0xBFFCu))

#define csr_read(name)                                                         \
    ({                                                                         \
        uint32_t v_;                                                           \
        __asm__ volatile("csrr %0, " name : "=r"(v_));                         \
        v_;                                                                    \
    })

#define csr_write(name, val)                                                   \
    __asm__ volatile("csrw " name ", %0" ::"r"((uint32_t)(val)))

#define csr_set(name, val)                                                     \
    __asm__ volatile("csrs " name ", %0" ::"r"((uint32_t)(val)))

#define csr_clear(name, val)                                                   \
    __asm__ volatile("csrc " name ", %0" ::"r"((uint32_t)(val)))

/* ------------------------------------------------------------------ */
/* Console                                                             */
/* ------------------------------------------------------------------ */

static void putc_(char c)
{
    UART_THR = (uint8_t)c;
}

static void puts_(const char *s)
{
    while (*s != '\0') {
        putc_(*s++);
    }
}

static void puthex(uint32_t v)
{
    static const char hex[] = "0123456789abcdef";
    putc_('0');
    putc_('x');
    for (int i = 28; i >= 0; i -= 4) {
        putc_(hex[(v >> i) & 0xFu]);
    }
}

/* ------------------------------------------------------------------ */
/* Harness                                                             */
/* ------------------------------------------------------------------ */

static int g_checks;
static int g_failures;

static void check(const char *name, uint32_t got, uint32_t want)
{
    g_checks++;
    if (got != want) {
        g_failures++;
        puts_("FAIL ");
        puts_(name);
        puts_(": got ");
        puthex(got);
        puts_(" want ");
        puthex(want);
        putc_('\n');
    }
}

/* Defeat constant folding so the emulator actually executes the op. */
static inline uint32_t opaque(uint32_t v)
{
    __asm__ volatile("" : "+r"(v));
    return v;
}

/* ------------------------------------------------------------------ */
/* APLIC (AIA), direct delivery mode                                   */
/* ------------------------------------------------------------------ */

#define APLIC_BASE 0x0C000000u
#define APLIC_R(off) (*(volatile uint32_t *)(APLIC_BASE + (off)))
#define APLIC_DOMAINCFG APLIC_R(0x0000u)
#define APLIC_SOURCECFG(i) APLIC_R(0x0004u + 4u * ((i) - 1u))
#define APLIC_SETIP APLIC_R(0x1C00u)
#define APLIC_SETIPNUM APLIC_R(0x1CDCu)
#define APLIC_CLRIPNUM APLIC_R(0x1DDCu)
#define APLIC_SETIE APLIC_R(0x1E00u)
#define APLIC_SETIENUM APLIC_R(0x1EDCu)
#define APLIC_CLRIENUM APLIC_R(0x1FDCu)
#define APLIC_TARGET(i) APLIC_R(0x3004u + 4u * ((i) - 1u))
#define APLIC_IDELIVERY APLIC_R(0x4000u)
#define APLIC_ITHRESHOLD APLIC_R(0x4008u)
#define APLIC_TOPI APLIC_R(0x4018u)
#define APLIC_CLAIMI APLIC_R(0x401Cu)

#define APLIC_SM_INACTIVE 0u
#define APLIC_SM_EDGE_RISE 4u

/* ------------------------------------------------------------------ */
/* Trap handling                                                       */
/* ------------------------------------------------------------------ */

static volatile uint32_t g_trap_count;
static volatile uint32_t g_last_cause;
static volatile uint32_t g_last_tval;
/* The trap before the last: a test that ends in an ECALL reads its own
 * trap here, after the ECALL has overwritten g_last_*. */
static volatile uint32_t g_prev_cause;
static volatile uint32_t g_prev_tval;
static volatile uint32_t g_last_claim;
static volatile uint32_t g_ext_count;
static volatile uint32_t g_lcof_count; /* Sscofpmf overflow interrupts */

/* Where to resume after an instruction access fault. See trap_handler. */
static volatile uint32_t g_fetch_resume;

/* Where to resume in M-mode when an S-mode test ecalls its way back. */
static volatile uint32_t g_smode_resume;

/*
 * GCC's "machine" interrupt attribute emits the register save/restore and
 * the closing mret. Exceptions still need mepc advanced past the faulting
 * instruction by hand, otherwise the handler would return straight onto it
 * and loop forever.
 */
__attribute__((interrupt("machine"), aligned(4), used)) static void
trap_handler(void)
{
    const uint32_t cause = csr_read("mcause");

    g_prev_cause = g_last_cause;
    g_prev_tval = g_last_tval;
    g_last_cause = cause;
    g_last_tval = csr_read("mtval");
    g_trap_count++;

    if (cause & 0x80000000u) {
        /* Interrupt. mepc must not move: it already points at the resume. */
        if ((cause & 0xFFu) == 7u) {
            CLINT_MTIMECMP_HI = 0xFFFFFFFFu;
            CLINT_MTIMECMP_LO = 0xFFFFFFFFu;
        } else if ((cause & 0xFFu) == 11u) {
            /*
             * External interrupt. Claiming is what clears the pending bit,
             * so without it the hart would re-enter this handler forever.
             */
            g_last_claim = APLIC_CLAIMI;
            g_ext_count++;
        } else if ((cause & 0xFFu) == 13u) {
            /*
             * A counter overflowed (Sscofpmf). LCOFIP is software's to
             * clear -- nothing else will -- so without this the hart
             * would take it again on the mret.
             */
            __asm__ volatile("csrc mip, %0" ::"r"(1u << 13));
            g_lcof_count++;
        }
        return;
    }

    if (cause == 8u || cause == 9u) {
        /*
         * ECALL from S-mode or U-mode: how a test below M hands control
         * back.
         *
         * MPP is set to M so the mret below returns to machine mode rather
         * than to the supervisor that called, and mepc to the point the
         * caller recorded before it left. This is the whole return path --
         * there is no other way back up a privilege level.
         */
        csr_write("mepc", g_smode_resume);
        csr_set("mstatus", 0x1800u); /* MPP = M */
        return;
    }

    if (cause == 1u || cause == 12u) {
        /*
         * Instruction access fault, or instruction page fault. Stepping
         * over is not an option: the next instruction is in the same
         * denied or unmapped page and would fault again, forever. The test
         * that provoked it says where to resume.
         *
         * Reading the faulting parcel to measure its length -- what the
         * generic path below does -- is not an option either, for the same
         * reason it faulted.
         */
        csr_write("mepc", g_fetch_resume);
        return;
    }

    /* Exception: step over the instruction, which may be 2 or 4 bytes. */
    const uint32_t epc = csr_read("mepc");
    const uint16_t parcel = *(volatile uint16_t *)epc;
    csr_write("mepc", epc + (((parcel & 3u) == 3u) ? 4u : 2u));
}

static void install_trap_handler(void)
{
    /* Direct mode: mtvec low bits clear. The handler is 4-byte aligned. */
    csr_write("mtvec", (uint32_t)&trap_handler);
}

/* ------------------------------------------------------------------ */
/* RV32I                                                               */
/* ------------------------------------------------------------------ */

static void test_alu(void)
{
    uint32_t a = opaque(0x12345678u);
    uint32_t b = opaque(0x0000FFFFu);

    check("addi", opaque(100u) + 23u, 123u);
    check("addi-neg", opaque(100u) + (uint32_t)-1, 99u);
    check("slti", (uint32_t)((int32_t)opaque((uint32_t)-5) < 3), 1u);
    check("sltiu", (uint32_t)(opaque((uint32_t)-5) < 3u), 0u);
    check("xori", opaque(0xF0F0F0F0u) ^ 0xFFu, 0xF0F0F00Fu);
    check("ori", opaque(0xF0F0F000u) | 0x0Fu, 0xF0F0F00Fu);
    check("andi", opaque(0xF0F0F0F0u) & 0xFFu, 0xF0u);

    check("slli", opaque(1u) << 31, 0x80000000u);
    check("srli", opaque(0x80000000u) >> 31, 1u);
    check("srai", (uint32_t)((int32_t)opaque(0x80000000u) >> 31), 0xFFFFFFFFu);
    check("srai-4", (uint32_t)((int32_t)opaque(0xFFFFFFF0u) >> 4), 0xFFFFFFFFu);

    check("add", a + b, 0x12355677u);
    check("sub", a - b, 0x12335679u);
    check("slt", (uint32_t)((int32_t)a < (int32_t)b), 0u);
    check("sltu", (uint32_t)(a < b), 0u);
    check("xor", a ^ b, 0x1234A987u);
    check("or", a | b, 0x1234FFFFu);
    check("and", a & b, 0x00005678u);

    /* Register shifts use only the low 5 bits of the amount. */
    uint32_t sh = opaque(15u);
    check("sll", a << sh, 0x12345678u << 15);
    check("srl", a >> sh, 0x12345678u >> 15);
    check("sra", (uint32_t)((int32_t)opaque(0x80000000u) >> sh),
          (uint32_t)((int32_t)0x80000000 >> 15));

    /* Shift amounts use only the low 5 bits of the register. */
    check("sll-mask", opaque(1u) << (opaque(33u) & 31u), 2u);
}

static void test_lui_auipc(void)
{
    uint32_t v;
    __asm__ volatile("lui %0, 0x12345" : "=r"(v));
    check("lui", v, 0x12345000u);

    /* auipc 0 yields the address of the auipc itself. */
    uint32_t pc, here;
    __asm__ volatile("1: auipc %0, 0\n\t la %1, 1b" : "=r"(pc), "=r"(here));
    check("auipc", pc, here);
}

static void test_branches(void)
{
    uint32_t taken = 0;
    int32_t neg = (int32_t)opaque((uint32_t)-1);
    uint32_t big = opaque(0xFFFFFFFFu);

    if (opaque(5u) == 5u)
        taken |= 1u << 0; /* beq  */
    if (opaque(5u) != 6u)
        taken |= 1u << 1; /* bne  */
    if (neg < 1)
        taken |= 1u << 2; /* blt  */
    if ((int32_t)opaque(1u) >= neg)
        taken |= 1u << 3; /* bge  */
    if (opaque(1u) < big)
        taken |= 1u << 4; /* bltu */
    if (big >= opaque(1u))
        taken |= 1u << 5; /* bgeu */

    check("branches", taken, 0x3Fu);
}

static uint8_t g_mem8[8];
static uint16_t g_mem16[4];
static uint32_t g_mem32[4];

/*
 * Sub-word stores are checked through a union rather than by casting the
 * address of a uint32_t: the cast would be a strict-aliasing violation and
 * the compiler is entitled to reorder around it.
 */
static volatile union {
    uint32_t w;
    uint16_t h[2];
    uint8_t b[4];
} g_pun;

static void test_loadstore(void)
{
    g_mem8[0] = 0x80u;
    g_mem8[1] = 0x7Fu;
    check("lbu", g_mem8[0], 0x80u);
    check("lb", (uint32_t)(int32_t)(int8_t)g_mem8[0], 0xFFFFFF80u);
    check("lb+", (uint32_t)(int32_t)(int8_t)g_mem8[1], 0x7Fu);

    g_mem16[0] = 0x8000u;
    g_mem16[1] = 0x7FFFu;
    check("lhu", g_mem16[0], 0x8000u);
    check("lh", (uint32_t)(int32_t)(int16_t)g_mem16[0], 0xFFFF8000u);
    check("lh+", (uint32_t)(int32_t)(int16_t)g_mem16[1], 0x7FFFu);

    g_mem32[0] = 0xDEADBEEFu;
    check("lw", g_mem32[0], 0xDEADBEEFu);

    /* Byte and halfword stores must not disturb their neighbours. */
    g_pun.w = 0xFFFFFFFFu;
    g_pun.b[1] = 0x00u;
    check("sb", g_pun.w, 0xFFFF00FFu);

    g_pun.w = 0xFFFFFFFFu;
    g_pun.h[0] = 0x1234u;
    check("sh", g_pun.w, 0xFFFF1234u);
}

/* ------------------------------------------------------------------ */
/* M extension                                                         */
/* ------------------------------------------------------------------ */

static void test_muldiv(void)
{
    check("mul", opaque(0x12345678u) * opaque(0x10u), 0x23456780u);
    check("mul-neg", (uint32_t)((int32_t)opaque((uint32_t)-3) * 5),
          (uint32_t)-15);

    check("mulh",
          (uint32_t)(((int64_t)(int32_t)opaque(0x40000000u) *
                      (int64_t)(int32_t)opaque(0x00000004u)) >>
                     32),
          1u);
    check("mulhu",
          (uint32_t)(((uint64_t)opaque(0x80000000u) *
                      (uint64_t)opaque(0x00000002u)) >>
                     32),
          1u);
    check("mulhsu",
          (uint32_t)(((int64_t)(int32_t)opaque(0xFFFFFFFFu) *
                      (int64_t)(uint64_t)opaque(0x00000002u)) >>
                     32),
          0xFFFFFFFFu);

    check("div", (uint32_t)((int32_t)opaque(100u) / (int32_t)opaque(7u)), 14u);
    check("div-neg",
          (uint32_t)((int32_t)opaque((uint32_t)-100) / (int32_t)opaque(7u)),
          (uint32_t)-14);
    check("divu", opaque(100u) / opaque(7u), 14u);
    check("rem", (uint32_t)((int32_t)opaque(100u) % (int32_t)opaque(7u)), 2u);
    check("rem-neg",
          (uint32_t)((int32_t)opaque((uint32_t)-100) % (int32_t)opaque(7u)),
          (uint32_t)-2);
    check("remu", opaque(100u) % opaque(7u), 2u);

    /*
     * RISC-V defines division by zero and signed overflow rather than
     * leaving them undefined, so these are emitted as raw instructions:
     * C would give the compiler licence to do anything here.
     */
    uint32_t r;
    __asm__ volatile("div %0, %1, %2"
                     : "=r"(r)
                     : "r"(opaque(1u)), "r"(opaque(0u)));
    check("div-by-0", r, 0xFFFFFFFFu);

    __asm__ volatile("divu %0, %1, %2"
                     : "=r"(r)
                     : "r"(opaque(1u)), "r"(opaque(0u)));
    check("divu-by-0", r, 0xFFFFFFFFu);

    __asm__ volatile("rem %0, %1, %2"
                     : "=r"(r)
                     : "r"(opaque(123u)), "r"(opaque(0u)));
    check("rem-by-0", r, 123u);

    __asm__ volatile("remu %0, %1, %2"
                     : "=r"(r)
                     : "r"(opaque(123u)), "r"(opaque(0u)));
    check("remu-by-0", r, 123u);

    __asm__ volatile("div %0, %1, %2"
                     : "=r"(r)
                     : "r"(opaque(0x80000000u)), "r"(opaque(0xFFFFFFFFu)));
    check("div-overflow", r, 0x80000000u);

    __asm__ volatile("rem %0, %1, %2"
                     : "=r"(r)
                     : "r"(opaque(0x80000000u)), "r"(opaque(0xFFFFFFFFu)));
    check("rem-overflow", r, 0u);
}

/* ------------------------------------------------------------------ */
/* A extension                                                         */
/* ------------------------------------------------------------------ */

static volatile uint32_t g_atomic;

static void test_atomics(void)
{
    uint32_t old, sc;

    g_atomic = 100u;
    __asm__ volatile("amoadd.w %0, %2, (%1)"
                     : "=&r"(old)
                     : "r"(&g_atomic), "r"(11u)
                     : "memory");
    check("amoadd-old", old, 100u);
    check("amoadd-new", g_atomic, 111u);

    g_atomic = 0xF0u;
    __asm__ volatile("amoswap.w %0, %2, (%1)"
                     : "=&r"(old)
                     : "r"(&g_atomic), "r"(0x0Fu)
                     : "memory");
    check("amoswap-old", old, 0xF0u);
    check("amoswap-new", g_atomic, 0x0Fu);

    g_atomic = 0xF0F0u;
    __asm__ volatile("amoand.w %0, %2, (%1)"
                     : "=&r"(old)
                     : "r"(&g_atomic), "r"(0xFF00u)
                     : "memory");
    check("amoand", g_atomic, 0xF000u);

    g_atomic = 0xF000u;
    __asm__ volatile("amoor.w %0, %2, (%1)"
                     : "=&r"(old)
                     : "r"(&g_atomic), "r"(0x000Fu)
                     : "memory");
    check("amoor", g_atomic, 0xF00Fu);

    g_atomic = 0xFFFFu;
    __asm__ volatile("amoxor.w %0, %2, (%1)"
                     : "=&r"(old)
                     : "r"(&g_atomic), "r"(0x0FF0u)
                     : "memory");
    check("amoxor", g_atomic, 0xF00Fu);

    /* Signed min/max must treat the stored value as signed. */
    g_atomic = (uint32_t)-5;
    __asm__ volatile("amomin.w %0, %2, (%1)"
                     : "=&r"(old)
                     : "r"(&g_atomic), "r"(3u)
                     : "memory");
    check("amomin", g_atomic, (uint32_t)-5);

    g_atomic = (uint32_t)-5;
    __asm__ volatile("amomax.w %0, %2, (%1)"
                     : "=&r"(old)
                     : "r"(&g_atomic), "r"(3u)
                     : "memory");
    check("amomax", g_atomic, 3u);

    /* ...and unsigned min/max must not. */
    g_atomic = (uint32_t)-5;
    __asm__ volatile("amominu.w %0, %2, (%1)"
                     : "=&r"(old)
                     : "r"(&g_atomic), "r"(3u)
                     : "memory");
    check("amominu", g_atomic, 3u);

    g_atomic = (uint32_t)-5;
    __asm__ volatile("amomaxu.w %0, %2, (%1)"
                     : "=&r"(old)
                     : "r"(&g_atomic), "r"(3u)
                     : "memory");
    check("amomaxu", g_atomic, (uint32_t)-5);

    /* An uninterrupted LR/SC pair must succeed. */
    g_atomic = 7u;
    __asm__ volatile("lr.w %0, (%2)\n\t"
                     "sc.w %1, %3, (%2)"
                     : "=&r"(old), "=&r"(sc)
                     : "r"(&g_atomic), "r"(42u)
                     : "memory");
    check("lr.w", old, 7u);
    check("sc.w-ok", sc, 0u);
    check("sc.w-stored", g_atomic, 42u);

    /* A store between LR and SC must break the reservation. */
    g_atomic = 7u;
    __asm__ volatile("lr.w %0, (%2)\n\t"
                     "sw %3, 0(%2)\n\t"
                     "sc.w %1, %4, (%2)"
                     : "=&r"(old), "=&r"(sc)
                     : "r"(&g_atomic), "r"(9u), "r"(42u)
                     : "memory");
    check("sc.w-broken", sc != 0u, 1u);
    check("sc.w-nostore", g_atomic, 9u);

    /* SC without a preceding LR must fail. */
    g_atomic = 1u;
    __asm__ volatile("sc.w %0, %2, (%1)"
                     : "=&r"(sc)
                     : "r"(&g_atomic), "r"(99u)
                     : "memory");
    check("sc.w-bare", sc != 0u, 1u);
}

/* ------------------------------------------------------------------ */
/* CSRs                                                                */
/* ------------------------------------------------------------------ */

static void test_csr(void)
{
    csr_write("mscratch", 0xA5A5A5A5u);
    check("mscratch-rw", csr_read("mscratch"), 0xA5A5A5A5u);

    /* csrrs with a non-zero source sets bits and returns the old value. */
    uint32_t old;
    __asm__ volatile("csrrs %0, mscratch, %1" : "=r"(old) : "r"(0x0000FFFFu));
    check("csrrs-old", old, 0xA5A5A5A5u);
    check("csrrs-new", csr_read("mscratch"), 0xA5A5FFFFu);

    __asm__ volatile("csrrc %0, mscratch, %1" : "=r"(old) : "r"(0x0000FF00u));
    check("csrrc-new", csr_read("mscratch"), 0xA5A500FFu);

    /* csrrs with x0 as the source must not write. */
    csr_write("mscratch", 0x5A5A5A5Au);
    __asm__ volatile("csrrs %0, mscratch, zero" : "=r"(old));
    check("csrrs-nowrite", csr_read("mscratch"), 0x5A5A5A5Au);

    /* misa must advertise exactly the extensions we built. */
    const uint32_t misa = csr_read("misa");
    check("misa-mxl", misa >> 30, 1u); /* MXL=32 */
    check("misa-I", (misa >> ('I' - 'A')) & 1u, 1u);
    check("misa-M", (misa >> ('M' - 'A')) & 1u, 1u);
    check("misa-A", (misa >> ('A' - 'A')) & 1u, 1u);
    check("misa-C", (misa >> ('C' - 'A')) & 1u, 1u);

    check("mhartid", csr_read("mhartid"), 0u);

    /* Counters must advance. */
    const uint32_t c0 = csr_read("mcycle");
    const uint32_t i0 = csr_read("minstret");
    for (volatile int i = 0; i < 10; i++) {
    }
    check("mcycle-advances", csr_read("mcycle") > c0, 1u);
    check("minstret-advances", csr_read("minstret") > i0, 1u);
}

/* ------------------------------------------------------------------ */
/* Traps                                                               */
/* ------------------------------------------------------------------ */

static void test_traps(void)
{
    uint32_t before;

    /*
     * All-ones is a permanently illegal 32-bit encoding. All-zeros would
     * also be illegal, but its low two bits are 00, which makes it two
     * illegal *16-bit* instructions and would trap twice.
     */
    before = g_trap_count;
    __asm__ volatile(".word 0xffffffff");
    check("trap-illegal-taken", g_trap_count - before, 1u);
    check("trap-illegal-cause", g_last_cause, 2u);
    check("trap-illegal-tval", g_last_tval, 0xFFFFFFFFu);

    /* A 16-bit illegal encoding must report the 2-byte parcel in mtval. */
    before = g_trap_count;
    __asm__ volatile(".half 0x0000");
    check("trap-illegal16-taken", g_trap_count - before, 1u);
    check("trap-illegal16-cause", g_last_cause, 2u);
    check("trap-illegal16-tval", g_last_tval, 0u);

    /* Breakpoint. */
    before = g_trap_count;
    __asm__ volatile("ebreak");
    check("trap-ebreak-taken", g_trap_count - before, 1u);
    check("trap-ebreak-cause", g_last_cause, 3u);

    /*
     * Misaligned access, **whichever way this build was configured**.
     *
     * The architecture permits either: a core may complete the access
     * or raise. Most embedded cores raise and this emulator did, until
     * the C library forced the question -- picolibc's strcmp is
     * word-at-a-time with no alignment preamble, so a guest that links
     * it and compares an odd-addressed string takes a trap inside the
     * library.
     *
     * So the behaviour is a build option, and this asserts what the
     * build chose rather than one answer. Asserting only the trap would
     * fail on the default; asserting only completion would leave the
     * trap path untested by anything.
     */
    before = g_trap_count;
    {
        uint32_t dst;
        volatile uint32_t *bad = (volatile uint32_t *)((uintptr_t)g_mem32 + 1u);
        __asm__ volatile("lw %0, 0(%1)" : "=r"(dst) : "r"(bad));
        (void)dst;
    }
#if RV32_MISALIGNED
    check("misaligned-lw-completed", g_trap_count - before, 0u);
#else
    check("trap-lw-misaligned-taken", g_trap_count - before, 1u);
    check("trap-lw-misaligned-cause", g_last_cause, 4u);
#endif

    /* Misaligned store. */
    before = g_trap_count;
    {
        volatile uint32_t *bad = (volatile uint32_t *)((uintptr_t)g_mem32 + 1u);
        __asm__ volatile("sw %0, 0(%1)" ::"r"(0u), "r"(bad));
    }
#if RV32_MISALIGNED
    check("misaligned-sw-completed", g_trap_count - before, 0u);
#else
    check("trap-sw-misaligned-taken", g_trap_count - before, 1u);
    check("trap-sw-misaligned-cause", g_last_cause, 6u);
#endif

    /* Access fault: nothing is mapped here. */
    before = g_trap_count;
    {
        uint32_t dst;
        volatile uint32_t *bad = (volatile uint32_t *)0x70000000u;
        __asm__ volatile("lw %0, 0(%1)" : "=r"(dst) : "r"(bad));
        (void)dst;
    }
    check("trap-access-taken", g_trap_count - before, 1u);
    check("trap-access-cause", g_last_cause, 5u);
    check("trap-access-tval", g_last_tval, 0x70000000u);

    /* A non-existent CSR must raise illegal instruction, not read as zero. */
    before = g_trap_count;
    {
        uint32_t v;
        __asm__ volatile("csrr %0, 0x7c0" : "=r"(v));
        (void)v;
    }
    check("trap-badcsr-taken", g_trap_count - before, 1u);
    check("trap-badcsr-cause", g_last_cause, 2u);
}

/* ------------------------------------------------------------------ */
/* Timer interrupt                                                     */
/* ------------------------------------------------------------------ */

static void test_timer_interrupt(void)
{
    const uint32_t before = g_trap_count;

    /*
     * Arm mtimecmp just ahead of the current time. Writing the high half to
     * all-ones first means the compare can never be briefly satisfied by a
     * half-updated value.
     */
    CLINT_MTIMECMP_HI = 0xFFFFFFFFu;
    CLINT_MTIMECMP_LO = CLINT_MTIME_LO + 20u;
    CLINT_MTIMECMP_HI = CLINT_MTIME_HI;

    csr_set("mie", 1u << 7); /* MTIE */
    csr_set("mstatus", 1u << 3); /* MIE  */

    /* Spin until the handler fires or we give up. */
    volatile int guard = 0;
    while (g_trap_count == before && guard < 100000) {
        guard++;
    }

    csr_write("mie", 0u);
    csr_write("mstatus", 0u);

    check("timer-fired", g_trap_count > before, 1u);
    check("timer-cause", g_last_cause, 0x80000007u);
}

/* ------------------------------------------------------------------ */
/* Cache block operations (Zicbom / Zicboz)                            */
/* ------------------------------------------------------------------ */

/*
 * One cache block -- 64 bytes, Zic64b -- and the block after it, aligned
 * so cbo.* on the first covers exactly that half.
 */
static volatile uint32_t g_block[32] __attribute__((aligned(64)));

static void test_cbo(void)
{
    for (int i = 0; i < 32; i++) {
        g_block[i] = 0xFFFFFFFFu;
    }

    /*
     * cbo.zero is the only one of these with an architecturally visible
     * effect, so it is the one that can be checked directly. The block
     * is 64 bytes, so exactly the first 16 words must be cleared and the
     * seventeenth on must be untouched -- which also says the block is
     * not 32, the size it was until Zic64b.
     */
    __asm__ volatile("cbo.zero (%0)" ::"r"(&g_block[0]) : "memory");

    uint32_t cleared = 0, intact = 0;
    for (int i = 0; i < 16; i++) {
        if (g_block[i] == 0u) {
            cleared++;
        }
    }
    for (int i = 16; i < 32; i++) {
        if (g_block[i] == 0xFFFFFFFFu) {
            intact++;
        }
    }
    check("cbo.zero-cleared", cleared, 16u);
    check("cbo.zero-bounded", intact, 16u);

    /*
     * The maintenance operations have no visible effect on a coherent
     * single-hart system, so all that can be checked is that they retire
     * without trapping rather than raising illegal instruction.
     */
    const uint32_t before = g_trap_count;
    __asm__ volatile("cbo.clean (%0)" ::"r"(&g_block[0]) : "memory");
    __asm__ volatile("cbo.inval (%0)" ::"r"(&g_block[0]) : "memory");
    __asm__ volatile("cbo.flush (%0)" ::"r"(&g_block[0]) : "memory");
    check("cbo-maint-no-trap", g_trap_count - before, 0u);

    /* A CBO against an unmapped address must fault, not fault silently. */
    const uint32_t b2 = g_trap_count;
    __asm__ volatile("cbo.clean (%0)" ::"r"(0x70000000u) : "memory");
    check("cbo-unmapped-traps", g_trap_count - b2, 1u);
}

/* ------------------------------------------------------------------ */
/* Zacas                                                               */
/* ------------------------------------------------------------------ */

/*
 * Only built when the guest is compiled with _zacas, which the default
 * march is.
 *
 * (This comment used to say the checks below localised "the remaining
 * Zacas problem" to somewhere else, and pointed at a roadmap note. There
 * was no problem in the emulator: the architecture suite's failure was
 * the Sail configuration declaring guest RAM `AMOArithmetic`, so the
 * golden model trapped on amocas and recorded the trap as the expected
 * result. Both Zacas tests pass.)
 *
 * This guard is on the *guest* compiler's march, which cannot see whether
 * the emulator was built with RV_EXT_ZACAS. Building the guest with _zacas
 * against an emulator without it is a valid configuration and these checks
 * will fail there, correctly: amocas raises illegal-instruction.
 */
#if defined(__riscv_zacas)
static volatile uint32_t g_cas;

static void test_zacas(void)
{
    uint32_t rd, sw;

    /* Comparand matches: the swap happens and rd returns the old value. */
    g_cas = 0x11112222u;
    rd = 0x11112222u;
    sw = 0xAABBCCDDu;
    __asm__ volatile("amocas.w %0, %2, (%1)"
                     : "+r"(rd)
                     : "r"(&g_cas), "r"(sw)
                     : "memory");
    check("amocas-eq-rd", rd, 0x11112222u);
    check("amocas-eq-mem", g_cas, 0xAABBCCDDu);

    /* Comparand differs: no store, rd still returns what memory held. */
    g_cas = 0x11112222u;
    rd = 0x99998888u;
    sw = 0xAABBCCDDu;
    __asm__ volatile("amocas.w %0, %2, (%1)"
                     : "+r"(rd)
                     : "r"(&g_cas), "r"(sw)
                     : "memory");
    check("amocas-ne-rd", rd, 0x11112222u);
    check("amocas-ne-mem", g_cas, 0x11112222u);

    /*
     * amocas.d: 64-bit CAS over even-odd register pairs, low half first.
     *
     * The pair is pinned with local register variables and the results are
     * copied into plain locals before anything else runs. That second part
     * matters: check() takes its arguments in a0 and a1, the very registers
     * pinned here, and a local register variable is only guaranteed live at
     * the asm statement itself. Reading them across a call is how an earlier
     * version of this test appeared to show the low half landing in the high
     * half's register.
     */
    static volatile uint64_t g_cas64 __attribute__((aligned(8)));
    uint32_t got_lo, got_hi, mem_lo, mem_hi;

    {
        register uint32_t p0 __asm__("a0") = 0x33334444u; /* comparand lo */
        register uint32_t p1 __asm__("a1") = 0x11112222u; /* comparand hi */
        register uint32_t s0 __asm__("a2") = 0xCCCCDDDDu; /* swap lo      */
        register uint32_t s1 __asm__("a3") = 0xAAAABBBBu; /* swap hi      */

        g_cas64 = 0x1111222233334444ull;
        __asm__ volatile("amocas.d %0, %2, (%4)"
                         : "+r"(p0), "+r"(p1)
                         : "r"(s0), "r"(s1), "r"(&g_cas64)
                         : "memory");
        got_lo = p0;
        got_hi = p1;
    }
    mem_lo = (uint32_t)g_cas64;
    mem_hi = (uint32_t)(g_cas64 >> 32);
    check("amocas.d-eq-lo", got_lo, 0x33334444u);
    check("amocas.d-eq-hi", got_hi, 0x11112222u);
    check("amocas.d-eq-mem", mem_lo, 0xCCCCDDDDu);
    check("amocas.d-eq-memh", mem_hi, 0xAAAABBBBu);

    /* Mismatching comparand: no store, but rd still takes the loaded value. */
    {
        register uint32_t p0 __asm__("a0") = 0xDEADBEEFu;
        register uint32_t p1 __asm__("a1") = 0x11112222u;
        register uint32_t s0 __asm__("a2") = 0xCCCCDDDDu;
        register uint32_t s1 __asm__("a3") = 0xAAAABBBBu;

        g_cas64 = 0x1111222233334444ull;
        __asm__ volatile("amocas.d %0, %2, (%4)"
                         : "+r"(p0), "+r"(p1)
                         : "r"(s0), "r"(s1), "r"(&g_cas64)
                         : "memory");
        got_lo = p0;
        got_hi = p1;
    }
    mem_lo = (uint32_t)g_cas64;
    mem_hi = (uint32_t)(g_cas64 >> 32);
    check("amocas.d-ne-lo", got_lo, 0x33334444u);
    check("amocas.d-ne-hi", got_hi, 0x11112222u);
    check("amocas.d-ne-mem", mem_lo, 0x33334444u);
    check("amocas.d-ne-memh", mem_hi, 0x11112222u);
}
#endif

/* ------------------------------------------------------------------ */
/* F extension                                                         */
/* ------------------------------------------------------------------ */

#if defined(__riscv_flen) && (__riscv_flen >= 32)

/* Operate on raw bit patterns: the guest ABI is ilp32 (soft-float), so
 * floats cannot be passed to check() as floats without conversion. */
#define FOP2(mn, a, b)                                                         \
    ({                                                                         \
        uint32_t r_, x_ = (a), y_ = (b);                                       \
        __asm__ volatile("fmv.w.x fa0, %1\n\t"                                 \
                         "fmv.w.x fa1, %2\n\t" mn " fa2, fa0, fa1\n\t"         \
                         "fmv.x.w %0, fa2"                                     \
                         : "=r"(r_)                                            \
                         : "r"(x_), "r"(y_)                                    \
                         : "fa0", "fa1", "fa2");                               \
        r_;                                                                    \
    })

#define FCMP(mn, a, b)                                                         \
    ({                                                                         \
        uint32_t r_, x_ = (a), y_ = (b);                                       \
        __asm__ volatile("fmv.w.x fa0, %1\n\t"                                 \
                         "fmv.w.x fa1, %2\n\t" mn " %0, fa0, fa1"              \
                         : "=r"(r_)                                            \
                         : "r"(x_), "r"(y_)                                    \
                         : "fa0", "fa1");                                      \
        r_;                                                                    \
    })

#define F1_0 0x3F800000u
#define F2_0 0x40000000u
#define F3_0 0x40400000u
#define F4_0 0x40800000u
#define F_QNAN 0x7FC00000u

static volatile uint32_t g_fmem;

/*
 * One `fcvt.w.s ..., dyn` at one address, called repeatedly under different
 * rounding modes. Kept out of line and un-inlined on purpose: the point is
 * that the *same* instruction is executed each time, so a JIT that
 * specialises a translated block on frm has to notice frm changing and
 * rebuild. Separate call sites would let each get its own translation and
 * prove nothing.
 */
/*
 * Three FP instructions at one address, called with mstatus.FS on and then
 * Off. Out of line for the same reason as cvt_w_dyn: the block must already
 * be translated when FS changes, so that a JIT deciding FP legality at
 * translation time -- rather than per execution, or with a flush -- is
 * caught running instructions the guest has just disabled.
 */
__attribute__((noinline)) static uint32_t fp_site(uint32_t bits)
{
    uint32_t r;
    __asm__ volatile("fmv.w.x fa0, %1\n\t fadd.s fa1, fa0, fa0\n\t"
                     "fmv.x.w %0, fa1"
                     : "=r"(r)
                     : "r"(bits)
                     : "fa0", "fa1");
    return r;
}

__attribute__((noinline)) static uint32_t cvt_w_dyn(uint32_t bits)
{
    uint32_t r;
    __asm__ volatile("fmv.w.x fa0, %1\n\t fcvt.w.s %0, fa0, dyn"
                     : "=r"(r)
                     : "r"(bits)
                     : "fa0");
    return r;
}

static void test_fpu(void)
{
    /* Arithmetic. */
    check("fadd", FOP2("fadd.s", F1_0, F2_0), F3_0);
    check("fsub", FOP2("fsub.s", F3_0, F1_0), F2_0);
    check("fmul", FOP2("fmul.s", F2_0, F2_0), F4_0);
    check("fdiv", FOP2("fdiv.s", F4_0, F2_0), F2_0);
    check("fsqrt", FOP2("fadd.s", 0u, 0u) | 0u, 0u); /* +0 + +0 == +0 */

    { /* sqrt(4) == 2 */
        uint32_t r, x = F4_0;
        __asm__ volatile("fmv.w.x fa0, %1\n\t fsqrt.s fa1, fa0\n\t"
                         "fmv.x.w %0, fa1"
                         : "=r"(r)
                         : "r"(x)
                         : "fa0", "fa1");
        check("fsqrt-4", r, F2_0);
    }

    /* Comparisons return 0/1 in an integer register. */
    check("feq", FCMP("feq.s", F1_0, F1_0), 1u);
    check("flt", FCMP("flt.s", F1_0, F2_0), 1u);
    check("fle", FCMP("fle.s", F2_0, F1_0), 0u);
    /* Any comparison against NaN is false. */
    check("feq-nan", FCMP("feq.s", F_QNAN, F1_0), 0u);

    /* Sign injection and min/max. */
    check("fsgnj", FOP2("fsgnj.s", F1_0, 0x80000000u), F1_0 | 0x80000000u);
    check("fmin", FOP2("fmin.s", F1_0, F2_0), F1_0);
    check("fmax", FOP2("fmax.s", F1_0, F2_0), F2_0);
    /* min/max return the non-NaN operand when only one is NaN. */
    check("fmin-nan", FOP2("fmin.s", F_QNAN, F2_0), F2_0);

    /*
     * The parts of min/max that an open-coded version gets wrong.
     *
     * ARMv7-M has no scalar VMINNM/VMAXNM, so these run through the helper
     * and share the interpreter's implementation. These checks pin the
     * behaviour anyway, because the tempting inline version -- compare and
     * conditionally select -- is wrong on both of the cases below.
     */
    check("fmax-nan", FOP2("fmax.s", F_QNAN, F2_0), F2_0);
    check("fmin-nan-rs2", FOP2("fmin.s", F2_0, F_QNAN), F2_0);
    /* Two NaNs give the canonical NaN, not either input. */
    check("fmin-2nan", FOP2("fmin.s", F_QNAN, F_QNAN), 0x7FC00000u);
    check("fmax-2nan", FOP2("fmax.s", F_QNAN, F_QNAN), 0x7FC00000u);
    /*
     * -0.0 is *below* +0.0 here, though IEEE comparison calls them equal.
     * A select driven by VCMP returns whichever operand it was told to
     * prefer on equality, so it gets one of these two wrong whichever way
     * it is written.
     */
    check("fmin-zeros", FOP2("fmin.s", 0x80000000u, 0u), 0x80000000u);
    check("fmax-zeros", FOP2("fmax.s", 0x80000000u, 0u), 0u);
    check("fmin-zeros-rev", FOP2("fmin.s", 0u, 0x80000000u), 0x80000000u);
    check("fmax-zeros-rev", FOP2("fmax.s", 0u, 0x80000000u), 0u);

    /* A quiet NaN operand is not an invalid operation; a signalling one is. */
    csr_write("fflags", 0u);
    (void)FOP2("fmin.s", F_QNAN, F2_0);
    check("fmin-qnan-noflag", csr_read("fflags") & 0x10u, 0u);
    csr_write("fflags", 0u);
    (void)FOP2("fmin.s", 0x7F800001u, F2_0); /* signalling NaN */
    check("fmin-snan-nv", csr_read("fflags") & 0x10u, 0x10u);
    csr_write("fflags", 0u);

    /*
     * The fused multiply-adds differ in which term is negated. This is the
     * check that catches negating the finished sum instead, which gives the
     * right magnitude with the wrong sign.
     *   fmadd  2*2+1 = 5     fnmsub -(2*2)+1 = -3
     */
    {
        uint32_t r, a = F2_0, b = F2_0, c = F1_0;
        __asm__ volatile("fmv.w.x fa0, %1\n\t fmv.w.x fa1, %2\n\t"
                         "fmv.w.x fa2, %3\n\t fmadd.s fa3, fa0, fa1, fa2\n\t"
                         "fmv.x.w %0, fa3"
                         : "=r"(r)
                         : "r"(a), "r"(b), "r"(c)
                         : "fa0", "fa1", "fa2", "fa3");
        check("fmadd", r, 0x40A00000u); /* 5.0 */

        __asm__ volatile("fmv.w.x fa0, %1\n\t fmv.w.x fa1, %2\n\t"
                         "fmv.w.x fa2, %3\n\t fnmsub.s fa3, fa0, fa1, fa2\n\t"
                         "fmv.x.w %0, fa3"
                         : "=r"(r)
                         : "r"(a), "r"(b), "r"(c)
                         : "fa0", "fa1", "fa2", "fa3");
        check("fnmsub", r, 0x40400000u | 0x80000000u); /* -3.0 */
    }

    /* Conversions both ways. */
    {
        uint32_t r;
        int32_t i = -7;
        __asm__ volatile("fcvt.s.w fa0, %1\n\t fmv.x.w %0, fa0"
                         : "=r"(r)
                         : "r"(i)
                         : "fa0");
        check("fcvt.s.w", r, 0x40E00000u | 0x80000000u); /* -7.0 */

        uint32_t back, src = 0x41200000u; /* 10.0 */
        __asm__ volatile("fmv.w.x fa0, %1\n\t fcvt.w.s %0, fa0, rtz"
                         : "=r"(back)
                         : "r"(src)
                         : "fa0");
        check("fcvt.w.s", back, 10u);
    }

    /*
     * Float to integer, in detail.
     *
     * The JIT translates these to ARM's VCVTR, which agrees with RISC-V on
     * saturation and on the exception flags but *not* on NaN: ARM yields
     * zero where RISC-V requires the maximum value of the target type. The
     * translation patches that up with a compare-against-self, so these
     * checks exist to prove the patch fires exactly when it should and
     * never otherwise. The single 10.0 case above would pass with the
     * fixup deleted.
     */
#define CVT_W(dst, bits, mode)                                                 \
    __asm__ volatile("fmv.w.x fa0, %1\n\t fcvt.w.s %0, fa0, " mode             \
                     : "=r"(dst)                                               \
                     : "r"(bits)                                               \
                     : "fa0")
#define CVT_WU(dst, bits, mode)                                                \
    __asm__ volatile("fmv.w.x fa0, %1\n\t fcvt.wu.s %0, fa0, " mode            \
                     : "=r"(dst)                                               \
                     : "r"(bits)                                               \
                     : "fa0")

    {
        uint32_t r;

        /* The divergence itself. NaN goes to the maximum, not to zero. */
        csr_write("fflags", 0u);
        CVT_W(r, 0x7FC00000u, "rtz"); /* canonical qNaN */
        check("fcvt.w.s-nan", r, 0x7FFFFFFFu);
        check("fcvt.w.s-nan-nv", csr_read("fflags"), 0x10u);

        csr_write("fflags", 0u);
        CVT_WU(r, 0x7FC00000u, "rtz");
        check("fcvt.wu.s-nan", r, 0xFFFFFFFFu);
        check("fcvt.wu.s-nan-nv", csr_read("fflags"), 0x10u);

        /* A negative NaN is still a NaN, and still goes to the maximum. */
        CVT_W(r, 0xFFC00000u, "rtz");
        check("fcvt.w.s-negnan", r, 0x7FFFFFFFu);

        /* Saturation, where the two agree. Invalid, and *not* inexact. */
        csr_write("fflags", 0u);
        CVT_W(r, 0x7F800000u, "rtz"); /* +inf */
        check("fcvt.w.s-posinf", r, 0x7FFFFFFFu);
        check("fcvt.w.s-posinf-nv", csr_read("fflags"), 0x10u);

        CVT_W(r, 0xFF800000u, "rtz"); /* -inf */
        check("fcvt.w.s-neginf", r, 0x80000000u);

        CVT_W(r, 0x4F800000u, "rtz"); /* 2^32, over signed range */
        check("fcvt.w.s-over", r, 0x7FFFFFFFu);

        CVT_WU(r, 0xBF800000u, "rtz"); /* -1.0, under unsigned */
        check("fcvt.wu.s-neg", r, 0u);

        CVT_WU(r, 0xFF800000u, "rtz"); /* -inf */
        check("fcvt.wu.s-neginf", r, 0u);

        /* An exact conversion raises nothing at all. */
        csr_write("fflags", 0u);
        CVT_W(r, 0xC1200000u, "rtz"); /* -10.0 */
        check("fcvt.w.s-exact", r, (uint32_t)-10);
        check("fcvt.w.s-exact-noflags", csr_read("fflags"), 0u);

        /* Every rounding mode the JIT claims to translate, on a tie. */
        CVT_W(r, 0x40200000u, "rne"); /* 2.5 -> 2, ties to even */
        check("fcvt.w.s-rne", r, 2u);
        CVT_W(r, 0x40600000u, "rne"); /* 3.5 -> 4, ties to even */
        check("fcvt.w.s-rne-odd", r, 4u);
        CVT_W(r, 0x40200000u, "rtz");
        check("fcvt.w.s-rtz", r, 2u);
        CVT_W(r, 0x40200000u, "rdn");
        check("fcvt.w.s-rdn", r, 2u);
        CVT_W(r, 0x40200000u, "rup");
        check("fcvt.w.s-rup", r, 3u);

        CVT_W(r, 0xC0200000u, "rtz"); /* -2.5 */
        check("fcvt.w.s-neg-rtz", r, (uint32_t)-2);
        CVT_W(r, 0xC0200000u, "rdn");
        check("fcvt.w.s-neg-rdn", r, (uint32_t)-3);
        CVT_W(r, 0xC0200000u, "rup");
        check("fcvt.w.s-neg-rup", r, (uint32_t)-2);

        /* Rounding away a fraction is inexact and nothing else. */
        csr_write("fflags", 0u);
        CVT_W(r, 0x40200000u, "rtz");
        check("fcvt.w.s-inexact", csr_read("fflags"), 0x01u);

        /*
         * Unsigned, negative, but rounding to zero: in range, so inexact
         * only. Getting this wrong by testing the sign rather than the
         * rounded value would raise invalid here.
         */
        csr_write("fflags", 0u);
        CVT_WU(r, 0xBF000000u, "rtz"); /* -0.5 */
        check("fcvt.wu.s-negzero", r, 0u);
        check("fcvt.wu.s-negzero-nx", csr_read("fflags"), 0x01u);

        /* Dynamic rounding: the mode comes from frm, not the encoding. */
        csr_write("fflags", 0u);
        csr_write("frm", 3u); /* RUP */
        CVT_W(r, 0x40200000u, "dyn");
        check("fcvt.w.s-dyn-rup", r, 3u);
        csr_write("frm", 1u); /* RTZ */
        CVT_W(r, 0x40200000u, "dyn");
        check("fcvt.w.s-dyn-rtz", r, 2u);
        csr_write("frm", 0u);
    }

    /*
     * Dynamic rounding, same instruction, mode changed underneath it.
     *
     * Two separate things are on trial. That a JIT specialising a block on
     * frm notices frm changing and rebuilds -- the RDN and second RUP cases
     * fail if it does not. And that RMM, which no ARM rounding mode can
     * express, is declined rather than translated as round-to-nearest: 2.5
     * is a tie, so ties-away gives 3 where ties-to-even gives 2, and a JIT
     * quietly substituting the wrong mode reports 2.
     *
     * 2.5 rather than 3.5 on purpose. 3.5 rounds to 4 under both, and would
     * have passed throughout the entire period this was broken.
     */
    {
        const uint32_t f2_5 = 0x40200000u; /*  2.5 */
        const uint32_t fn2_5 = 0xC0200000u; /* -2.5 */

        csr_write("frm", 3u); /* RUP */
        check("dyn-rup", cvt_w_dyn(f2_5), 3u);

        csr_write("frm", 2u); /* RDN: needs the rebuild */
        check("dyn-rdn", cvt_w_dyn(f2_5), 2u);

        csr_write("frm", 0u); /* RNE: ties to even */
        check("dyn-rne", cvt_w_dyn(f2_5), 2u);

        csr_write("frm", 4u); /* RMM: ties away */
        check("dyn-rmm", cvt_w_dyn(f2_5), 3u);
        check("dyn-rmm-neg", cvt_w_dyn(fn2_5), (uint32_t)-3);

        csr_write("frm", 3u); /* RUP: re-specialise */
        check("dyn-rup-again", cvt_w_dyn(f2_5), 3u);

        csr_write("frm", 0u);
    }

    /*
     * mstatus.FS gates the whole extension, so with it Off every FP
     * instruction must raise illegal-instruction -- including fmv, which
     * touches no arithmetic.
     *
     * The site is called once first so the block exists, then again with FS
     * cleared. A backend that decides FP legality when it *translates*,
     * rather than when it executes, runs the instructions anyway and reports
     * no traps at all.
     */
    {
        check("fs-on", fp_site(F1_0), F2_0); /* 1.0 + 1.0 */

        const uint32_t before = g_trap_count;
        csr_clear("mstatus", 3u << 13); /* FS = Off */
        (void)fp_site(F1_0);
        const uint32_t traps = g_trap_count - before;

        /* Restore before checking, so a failure here does not kill the
         * remaining FP tests as well. */
        csr_set("mstatus", 1u << 13); /* FS = Initial */

        check("fs-off-traps", traps, 3u); /* one per FP insn */
        check("fs-off-cause", g_last_cause, 2u); /* illegal instruction */

        /*
         * A 16-bit FP instruction reports its own 16 bits in mtval, not
         * the 32-bit FLW it is expanded to: c.flwsp ft0, 0(sp) is 0x6002,
         * and the expansion, 0x00012007, is what came back.
         */
        csr_clear("mstatus", 3u << 13);
        __asm__ volatile(".half 0x6002" ::: "memory", "ft0");
        csr_set("mstatus", 1u << 13);
        check("fs-off-c-tval", g_last_tval, 0x6002u);

        /* And with FS back on it works again. */
        check("fs-restored", fp_site(F1_0), F2_0);
    }
#undef CVT_W
#undef CVT_WU

    /* fclass: 1<<6 is a positive normal, 1<<3 is negative zero. */
    {
        uint32_t r, x = F1_0;
        __asm__ volatile("fmv.w.x fa0, %1\n\t fclass.s %0, fa0"
                         : "=r"(r)
                         : "r"(x)
                         : "fa0");
        check("fclass-norm", r, 1u << 6);
        x = 0x80000000u;
        __asm__ volatile("fmv.w.x fa0, %1\n\t fclass.s %0, fa0"
                         : "=r"(r)
                         : "r"(x)
                         : "fa0");
        check("fclass-negzero", r, 1u << 3);

        /*
         * The rest of the ten-way split. FCLASS has no ARM equivalent at
         * all, so it runs through the helper; these pin every bit of the
         * result so that stays honest.
         */
#define FCLASS(bits)                                                           \
    ({                                                                         \
        uint32_t c_;                                                           \
        const uint32_t b_ = (bits);                                            \
        __asm__ volatile("fmv.w.x fa0, %1\n\t fclass.s %0, fa0"                \
                         : "=r"(c_)                                            \
                         : "r"(b_)                                             \
                         : "fa0");                                             \
        c_;                                                                    \
    })

        check("fclass-neginf", FCLASS(0xFF800000u), 1u << 0);
        check("fclass-negnorm", FCLASS(0xBF800000u), 1u << 1); /* -1.0 */
        check("fclass-negsub", FCLASS(0x80000001u), 1u << 2);
        check("fclass-poszero", FCLASS(0x00000000u), 1u << 4);
        check("fclass-possub", FCLASS(0x00000001u), 1u << 5);
        check("fclass-posinf", FCLASS(0x7F800000u), 1u << 7);
        check("fclass-snan", FCLASS(0x7F800001u), 1u << 8);
        check("fclass-qnan", FCLASS(0x7FC00000u), 1u << 9);
        /* Classifying a signalling NaN must not itself raise invalid. */
        csr_write("fflags", 0u);
        (void)FCLASS(0x7F800001u);
        check("fclass-snan-noflag", csr_read("fflags"), 0u);
#undef FCLASS
    }

    /* FLW / FSW through real memory. */
    {
        uint32_t r;
        g_fmem = 0;
        __asm__ volatile("fmv.w.x fa0, %1\n\t fsw fa0, 0(%2)" ::"r"(0),
                         "r"(F3_0), "r"(&g_fmem)
                         : "fa0", "memory");
        check("fsw", g_fmem, F3_0);
        __asm__ volatile("flw fa0, 0(%1)\n\t fmv.x.w %0, fa0"
                         : "=r"(r)
                         : "r"(&g_fmem)
                         : "fa0");
        check("flw", r, F3_0);
    }

    /* fflags accumulates: 1/0 raises divide-by-zero. */
    csr_write("fflags", 0u);
    (void)FOP2("fdiv.s", F1_0, 0u);
    check("fflags-dz", csr_read("fflags") & 0x08u, 0x08u);
    csr_write("fflags", 0u);
}
#endif /* __riscv_flen */

/* ------------------------------------------------------------------ */
/* PMP                                                                 */
/* ------------------------------------------------------------------ */

/*
 * Guarded region for the lock test. Eight bytes, eight-byte aligned, so a
 * single NAPOT entry covers it exactly and nothing else.
 *
 * Locking is permanent until reset, so this buffer must not be touched by
 * anything else afterwards -- which is why it is a dedicated object rather
 * than a slice of an existing one.
 */
static volatile uint32_t g_pmp_area[2] __attribute__((aligned(8)));
static volatile uint32_t g_pmp_area2[2] __attribute__((aligned(8)));

/*
 * One store at one address, used against both protected regions.
 *
 * Out of line and un-inlined so the *same* translated block is reused. A JIT
 * may inline the memory access and test the PMP range at translation time;
 * if it does, this is the site that catches it still using bounds the guest
 * has since changed.
 */
__attribute__((noinline)) static void pmp_store(volatile uint32_t *p,
                                                uint32_t v)
{
    *p = v;
}

#if !RV32_EXT_PMP
/*
 * No PMP: its CSRs are not there, so reading or writing one is an
 * illegal instruction -- not a read of zero and a write that vanishes,
 * which is what an `#if` around a `default:` once made every absent CSR
 * do (CLAUDE.md).
 */
static void test_pmp_absent(void)
{
    uint32_t before = g_trap_count;

    (void)csr_read("pmpcfg0");
    check("pmp-absent-read-traps", g_trap_count - before, 1u);
    check("pmp-absent-read-cause", g_last_cause, 2u);

    before = g_trap_count;
    csr_write("pmpaddr0", 0x1234u);
    check("pmp-absent-write-traps", g_trap_count - before, 1u);
    check("pmp-absent-write-cause", g_last_cause, 2u);
}
#endif

static void test_pmp(void)
{
    /* The address registers are plain read/write while unlocked. */
    csr_write("pmpaddr1", 0x12345678u);
    check("pmpaddr-rw", csr_read("pmpaddr1"), 0x12345678u);
    csr_write("pmpaddr1", 0u);

    /* Reserved bits of a cfg byte read back as zero. */
    csr_write("pmpcfg0", 0x00006000u);
    check("pmpcfg-wpri", csr_read("pmpcfg0") & 0x6000u, 0u);
    csr_write("pmpcfg0", 0u);

    g_pmp_area[0] = 0xA5A5A5A5u;
    g_pmp_area[1] = 0x5A5A5A5Au;

    /*
     * An unlocked entry does not restrict M -- except an access half in
     * it, which fails "irrespective of the L, R, W, and X bits". NA4 over
     * the upper word, R W, unlocked, nothing locked anywhere, and a word
     * load from two bytes below it. The hart skipped PMP in M with no
     * entry locked, so it read (fuzz-rv32-mmu; Sail and Spike agree,
     * probes/pmpstraddle.S). The word the entry holds still reads.
     */
    const uintptr_t straddle = (uintptr_t)g_pmp_area + 2u;
    uint32_t before;

    csr_write("pmpaddr0", ((uint32_t)(uintptr_t)&g_pmp_area[1]) >> 2);
    csr_write("pmpcfg0", 0x13u); /* NA4 | W | R, unlocked */
    before = g_trap_count;
    /* One lw, in asm: C split this known-misaligned load into two lhu,
     * neither of which is half in the entry. */
    {
        uint32_t tmp;

        __asm__ volatile("lw %0, 0(%1)" : "=r"(tmp) : "r"(straddle) : "memory");
        (void)tmp;
    }
    check("pmp-straddle-unlocked-traps", g_trap_count - before, 1u);
#if RV32_MISALIGNED
    check("pmp-straddle-unlocked-cause", g_last_cause, 5u);
#else
    check("pmp-straddle-unlocked-cause", g_last_cause, 4u);
#endif
    check("pmp-straddle-unlocked-tval", g_last_tval, (uint32_t)straddle);
    before = g_trap_count;
    check("pmp-whole-unlocked-read", g_pmp_area[1], 0x5A5A5A5Au);
    check("pmp-whole-unlocked-notrap", g_trap_count - before, 0u);
    csr_write("pmpcfg0", 0u);

    /*
     * Entry 0: NAPOT over the eight bytes, locked, readable but not
     * writable. Locked is what makes it apply to M-mode at all.
     */
    csr_write("pmpaddr0", ((uint32_t)(uintptr_t)g_pmp_area) >> 2);
    csr_write("pmpcfg0", 0x99u); /* L | NAPOT | R */

    /* Reads still work. */
    check("pmp-read-ok", g_pmp_area[0], 0xA5A5A5A5u);

    /* Writes now fault, and report a store access fault at the address. */
    before = g_trap_count;
    __asm__ volatile("sw %0, 0(%1)" ::"r"(0xDEADBEEFu), "r"(g_pmp_area)
                     : "memory");
    check("pmp-write-traps", g_trap_count - before, 1u);
    check("pmp-write-cause", g_last_cause, 7u); /* store access fault */
    check("pmp-write-blocked", g_pmp_area[0], 0xA5A5A5A5u);

    /* A locked entry is immutable until reset: both cfg and address. */
    csr_write("pmpcfg0", 0u);
    check("pmp-cfg-locked", csr_read("pmpcfg0") & 0xFFu, 0x99u);
    const uint32_t locked_addr = csr_read("pmpaddr0");
    csr_write("pmpaddr0", 0xFFFFFFFFu);
    check("pmp-addr-locked", csr_read("pmpaddr0"), locked_addr);

    /* Memory outside the entry is unaffected. */
    static volatile uint32_t elsewhere;
    elsewhere = 0x1234u;
    check("pmp-outside-ok", elsewhere, 0x1234u);

    /*
     * A *second* locked entry, added while PMP is already active.
     *
     * This is the case a JIT can miss. Inlining the memory access means
     * baking in the bounds of the one enabled entry and testing against
     * them, which is only sound while there is one; a backend that watches
     * whether PMP is active, rather than what it contains, sees no change
     * here, because it was already active before entry 1 was locked.
     *
     * The store site is shared with the call below, so it is translated
     * before entry 1 exists and reused afterwards. Doing it the other way
     * round -- a fresh call site each time -- would be translated against
     * the current configuration and would pass regardless.
     */
    g_pmp_area2[0] = 0x11223344u;

    /* Unprotected so far: the write lands, and nothing traps. */
    before = g_trap_count;
    pmp_store(&g_pmp_area2[0], 0x55667788u);
    check("pmp2-before-ok", g_pmp_area2[0], 0x55667788u);
    check("pmp2-before-notrap", g_trap_count - before, 0u);

    /* Entry 1: same shape as entry 0, over the second area. */
    csr_write("pmpaddr1", ((uint32_t)(uintptr_t)g_pmp_area2) >> 2);
    csr_write("pmpcfg0", 0x9900u); /* byte 1: L | NAPOT | R */
    check("pmp2-cfg", (csr_read("pmpcfg0") >> 8) & 0xFFu, 0x99u);

    /* The same store must now be denied. */
    before = g_trap_count;
    pmp_store(&g_pmp_area2[0], 0xDEADBEEFu);
    check("pmp2-write-traps", g_trap_count - before, 1u);
    check("pmp2-write-cause", g_last_cause, 7u);
    check("pmp2-write-blocked", g_pmp_area2[0], 0x55667788u);

    /* And entry 0 still denies, with two entries in play. */
    before = g_trap_count;
    pmp_store(&g_pmp_area[0], 0xDEADBEEFu);
    check("pmp1-still-traps", g_trap_count - before, 1u);
    check("pmp1-still-blocked", g_pmp_area[0], 0xA5A5A5A5u);
}

/*
 * Execute permission, which is the one PMP question a JIT answers
 * differently from an interpreter.
 *
 * The interpreter checks X as part of fetching, once per instruction. A JIT
 * reads the guest's instruction bytes at *translation* time and then emits
 * nothing at all for the fetch -- so unless the translator refuses to
 * translate what PMP will not let the guest execute, X is simply
 * unenforced, and no amount of running the block will discover that.
 *
 * Sixty-four bytes, 64-byte aligned, so one NAPOT entry covers the buffer
 * exactly and nothing that shares its cache line. It holds a single
 * instruction: `jalr x0, 0(ra)`, a bare return.
 */
static uint32_t g_xbuf[16] __attribute__((aligned(64)));

/*
 * Written by the stub, and the reason the stub is two instructions rather
 * than a bare return.
 *
 * A one-instruction stub does not discriminate. `jalr` is not translatable,
 * so the block would be empty, the interpreter would run it, and the
 * interpreter's own fetch check would raise the fault whether or not the
 * translator has one -- the test would pass with the fix reverted, which
 * was exactly what happened. Leading with a store gives the translator
 * something it *will* take, so a missing check shows up as the store having
 * happened before the fault the trailing `jalr` eventually causes.
 */
static volatile uint32_t g_xflag;

/*
 * Call the stub with a0/a1 set up for its store, recording where to resume
 * if the call faults.
 *
 * The resume label lives inside the asm, immediately after the jalr, so the
 * point the handler returns to is one the compiler already believes control
 * reaches -- taking the address of a C label and mret-ing to it would resume
 * with the register state of a call that never returned.
 */
static void call_stub(uint32_t entry, uint32_t val)
{
    register uint32_t a0_ __asm__("a0") = (uint32_t)(uintptr_t)&g_xflag;
    register uint32_t a1_ __asm__("a1") = val;

    __asm__ volatile("la   t0, 1f\n"
                     "sw   t0, 0(%1)\n"
                     "jalr %0\n"
                     "1:\n" ::"r"(entry),
                     "r"(&g_fetch_resume), "r"(a0_), "r"(a1_)
                     : "t0", "ra", "memory");
}

static void test_pmp_exec(void)
{
    g_xbuf[0] = 0x00b52023u; /* sw   a1, 0(a0)  */
    g_xbuf[1] = 0x00008067u; /* jalr x0, 0(ra)  */
    __asm__ volatile("fence.i" ::: "memory");

    const uint32_t base = (uint32_t)(uintptr_t)g_xbuf;

    /*
     * Run it once before protecting it. This is the load-bearing half of
     * the test: it is what puts a *translated block* for this address in
     * the code cache, so the call below reuses it. A backend that trusts a
     * cached block after the configuration it was built against has changed
     * fails here and passes if the order is reversed.
     */
    uint32_t before = g_trap_count;
    g_xflag = 0u;
    call_stub(base, 0x600Du);
    check("pmpx-before-ok", g_trap_count - before, 0u);
    check("pmpx-before-ran", g_xflag, 0x600Du);

    /*
     * Entry 2: NAPOT over the 64 bytes, locked, readable and writable but
     * *not* executable. A NAPOT region of 2^k bytes sets the low k-3 bits
     * of pmpaddr, so 64 bytes is three bits.
     */
    csr_write("pmpaddr2", (base >> 2) | 0x7u);
    csr_write("pmpcfg0", (csr_read("pmpcfg0") & 0xFFFFu) | 0x9B0000u);
    check("pmpx-cfg", (csr_read("pmpcfg0") >> 16) & 0xFFu, 0x9Bu);

    /* Reading it is still allowed -- only X was withheld. */
    check("pmpx-read-ok", g_xbuf[0], 0x00b52023u);

    /* Executing it now faults, before the instruction has any effect. */
    before = g_trap_count;
    g_xflag = 0u;
    call_stub(base, 0xBADu);
    check("pmpx-exec-traps", g_trap_count - before, 1u);
    check("pmpx-exec-cause", g_last_cause, 1u); /* insn access fault */
    check("pmpx-exec-tval", g_last_tval, base);

    /*
     * And nothing in the region ran. This is the check that separates the
     * two backends: the trap above happens either way, because the `jalr`
     * ends the block and lands on the interpreter, which checks. Only a
     * translator that also checks stops the store in front of it.
     */
    check("pmpx-exec-noeffect", g_xflag, 0u);
}

/* ------------------------------------------------------------------ */
/* APLIC                                                               */
/* ------------------------------------------------------------------ */

/*
 * Delivery is driven through setipnum rather than a real interrupt line, so
 * this runs identically on the host and on the board. What it proves is the
 * whole path a bridged hardware interrupt takes once the platform has
 * called rv_aplic_raise: pending and enable combine, the domain and the IDC
 * both gate delivery, MEIP reaches the hart, and claiming clears it.
 */
/* ------------------------------------------------------------------ */
/* S-mode and Sv32                                                     */
/* ------------------------------------------------------------------ */

/*
 * These are the checks the host suites cannot give the *JIT*.
 *
 * riscv-tests and the architecture suite cover S-mode and paging
 * thoroughly, but only on the host, where the x86-64 backend hands
 * anything behind fetch_guard -- which includes every translated access
 * while paging is on -- straight to the interpreter. The Thumb-2 backend
 * does translate under Sv32: it declines to inline memory accesses and
 * walks the page table itself to find the instruction bytes. Neither of
 * those paths had ever run on hardware before this file exercised them.
 */

#define MSTATUS_MPP_MASK 0x00001800u
#define MSTATUS_MPP_S 0x00000800u
#define MSTATUS_SPP 0x00000100u
#define MSTATUS_SIE 0x00000002u
#define MSTATUS_SPIE 0x00000020u
#define MSTATUS_MIE 0x00000008u
#define MSTATUS_SUM 0x00040000u
#define MSTATUS_MXR 0x00080000u
#define MSTATUS_MPRV 0x00020000u
#define MSTATUS_TW 0x00200000u
#define MSTATUS_TSR 0x00400000u

#define PTE_V 0x001u
#define PTE_R 0x002u
#define PTE_W 0x004u
#define PTE_X 0x008u
#define PTE_U 0x010u
#define PTE_A 0x040u
#define PTE_D 0x080u

#define SATP_SV32 0x80000000u

/*
 * Every page-aligned thing this test owns, as one object in a section
 * the link script puts first in .bss. Seven pages are most of what the
 * test needs, and as separate arrays the linker padded each one to a
 * page boundary after whatever small variable preceded it: 11 KiB of
 * holes, which took the test from 33 KiB to 44 and off the F446.
 * Packed, it fits the 32 KiB that board's guests are linked for, with
 * room for the stack that test_stack_depth checks.
 *
 * Page tables. The root alone would do for an identity map built from
 * megapages; the second level exists so the 4 KiB path is exercised too,
 * and so there is somewhere to change one PTE and watch the effect.
 * root2 and leaf2 are the second address space, described where it is
 * built (smode_dtlb).
 *
 * Volatile, all of it: the tables are read by the page walker and the
 * pages are reached through virtual addresses, and the compiler knows
 * neither. With the arrays separate, smode_paging's check of g_page[0]
 * happened to be scheduled after the store through VA_RW that it
 * checks; in one struct GCC hoisted the load above the store -- which
 * it may, since a constant address and a named object do not alias --
 * and sv32-4k-write read 0 on every backend.
 */
static volatile struct {
    uint32_t root[1024];
    uint32_t leaf[1024];
    uint32_t root2[1024];
    uint32_t leaf2[1024];
    uint32_t page[3 * 1024 + 16]; /* last: see the sentinel below */
} g_pt __attribute__((aligned(4096), section(".bss.pages")));
#define g_root (g_pt.root)
#define g_leaf (g_pt.leaf)
#define g_root2 (g_pt.root2)
#define g_leaf2 (g_pt.leaf2)
#define g_page (g_pt.page)
/*
 * Three pages, and they must be three.
 *
 * Page 0 is the data page every data VA below maps to; pages 1 and 2 hold
 * the two stubs the execute test points one VA at each of in turn. Sharing
 * page 0 between the data tests and the stub is what the first version did,
 * and the store through VA_RW then overwrote the instruction the execute
 * test was about to jump to -- which presents as a fetch fault on a page
 * that is mapped executable, and reads as an emulator bug.
 */
#define PAGE_DATA 0u
#define PAGE_EXEC1 1u
#define PAGE_EXEC2 2u
/*
 * And 64 bytes nothing maps, after the third. test_split_access and the
 * fetch table's straddling instruction ask for memory that runs off the
 * end of the third page; an emulator that takes it physically contiguous
 * lands here, in zeros, rather than in whatever the linker put next. The
 * widest such access is an 8-byte fsd two bytes short of the end, so a
 * whole page of sentinel -- which this used to be -- bought nothing but
 * the 4 KiB the F446 did not have.
 */

static uint32_t page_pa(uint32_t which)
{
    return (uint32_t)(uintptr_t)g_page + which * 4096u;
}

/*
 * Virtual addresses for the mapped test page. Two of them, so the same
 * physical page can be reached through PTEs with different permissions --
 * which is what separates "the walk found the page" from "the walk applied
 * the permissions".
 */
#define VA_RW 0x90000000u
#define VA_RO 0x90001000u
#define VA_USER 0x90002000u
#define VA_NOA 0x90003000u
#define VA_HOLE 0x90004000u
#define VA_X 0x90005000u

#define VPN1(va) ((va) >> 22)
#define VPN0(va) (((va) >> 12) & 0x3FFu)

/* A leaf PTE mapping `pa`, which must be page aligned. */
static uint32_t leaf_pte(uint32_t pa, uint32_t perm)
{
    return ((pa >> 12) << 10) | perm;
}

static void build_page_tables(void)
{
    /*
     * Identity map the whole address space with 4 MiB megapages, so that
     * turning translation on changes nothing about where anything lives.
     * That is what makes the *failures* below legible: any fault is
     * something the test asked for, not the map being wrong.
     *
     * These are supervisor pages (no U bit): S-mode code runs from them,
     * and U-mode is not entered here.
     */
    for (uint32_t i = 0; i < 1024u; i++) {
        g_root[i] = (i << 20) | PTE_V | PTE_R | PTE_W | PTE_X | PTE_A | PTE_D;
    }

    /* One second-level table, reached through a pointer PTE -- V and
     * nothing else, which is what distinguishes a pointer from a leaf. */
    g_root[VPN1(VA_RW)] = leaf_pte((uint32_t)(uintptr_t)g_leaf, PTE_V);

    for (uint32_t i = 0; i < 1024u; i++) {
        g_leaf[i] = 0u; /* unmapped by default */
    }

    const uint32_t pa = page_pa(PAGE_DATA);

    g_leaf[VPN0(VA_RW)] = leaf_pte(pa, PTE_V | PTE_R | PTE_W | PTE_A | PTE_D);
    g_leaf[VPN0(VA_RO)] = leaf_pte(pa, PTE_V | PTE_R | PTE_A);
    g_leaf[VPN0(VA_USER)] =
        leaf_pte(pa, PTE_V | PTE_R | PTE_W | PTE_U | PTE_A | PTE_D);
    /* A is clear: under Svade the hardware never sets it, so the first
     * touch faults and software is expected to fix it up. */
    g_leaf[VPN0(VA_NOA)] = leaf_pte(pa, PTE_V | PTE_R | PTE_W | PTE_D);
    g_leaf[VPN0(VA_X)] =
        leaf_pte(page_pa(PAGE_EXEC1), PTE_V | PTE_R | PTE_X | PTE_A);
    /* VA_HOLE is left invalid on purpose. */
}

/*
 * Run `fn` in S-mode and come back.
 *
 * The resume label lives inside the asm, immediately after the mret, so
 * the point the handler returns to is one the compiler already believes
 * control reaches. Taking the address of a C label and returning there
 * instead resumes with a register state belonging to a call that never
 * returned -- which is how the first attempt at this re-entered earlier
 * tests rather than continuing.
 *
 * `fn` ends by executing ecall; cause 9 is not delegated, so it lands in
 * the machine handler, which restores MPP and mepc from here.
 */
/*
 * Somewhere to park the registers that survive a call but not this.
 *
 * `fn` is entered by mret rather than called, and leaves by ecall rather
 * than returning: its prologue runs and its epilogue never does. So every
 * callee-saved register it touched keeps fn's value, and the stack pointer
 * comes back below where this function left it -- the caller's locals are
 * simply gone. That does not present as a stack bug; it presents as the
 * test suite restarting from the top, which is how the first attempt at
 * this was misread as a resume-address problem.
 *
 * The base address is reloaded with `la` on the way back rather than kept
 * in an operand register, because that register is caller-saved and is one
 * of the things fn was free to destroy.
 */
static volatile uint32_t g_smode_regs[13];

static void enter_smode(void (*fn)(void))
{
    __asm__ volatile("la   t0, g_smode_regs\n"
                     "sw   sp,  0(t0)\n"
                     "sw   s0,  4(t0)\n"
                     "sw   s1,  8(t0)\n"
                     "sw   s2, 12(t0)\n"
                     "sw   s3, 16(t0)\n"
                     "sw   s4, 20(t0)\n"
                     "sw   s5, 24(t0)\n"
                     "sw   s6, 28(t0)\n"
                     "sw   s7, 32(t0)\n"
                     "sw   s8, 36(t0)\n"
                     "sw   s9, 40(t0)\n"
                     "sw   s10, 44(t0)\n"
                     "sw   s11, 48(t0)\n"
                     "la   t0, 1f\n"
                     "sw   t0, 0(%1)\n"
                     "csrw mepc, %0\n"
                     "li   t0, %2\n"
                     "csrc mstatus, t0\n"
                     "li   t0, %3\n"
                     "csrs mstatus, t0\n"
                     "mret\n"
                     "1:\n"
                     "la   t0, g_smode_regs\n"
                     "lw   sp,  0(t0)\n"
                     "lw   s0,  4(t0)\n"
                     "lw   s1,  8(t0)\n"
                     "lw   s2, 12(t0)\n"
                     "lw   s3, 16(t0)\n"
                     "lw   s4, 20(t0)\n"
                     "lw   s5, 24(t0)\n"
                     "lw   s6, 28(t0)\n"
                     "lw   s7, 32(t0)\n"
                     "lw   s8, 36(t0)\n"
                     "lw   s9, 40(t0)\n"
                     "lw   s10, 44(t0)\n"
                     "lw   s11, 48(t0)\n" ::"r"(fn),
                     "r"(&g_smode_resume), "i"(MSTATUS_MPP_MASK),
                     "i"(MSTATUS_MPP_S)
                     : "t0", "t1", "t2", "a0", "a1", "a2", "a3", "a4", "a5",
                       "a6", "a7", "ra", "memory");
}

/* Every S-mode test body ends with this. */
#define LEAVE_SMODE() __asm__ volatile("ecall" ::: "memory")

/*
 * One instruction below M-mode, then back: a stub entered by SRET.
 *
 * The stubs try what a lower mode may not do, MRET among them -- and an
 * MRET that wrongly runs returns to mepc. Entered by MRET as
 * enter_smode does, mepc would still be the stub, and a broken MRET
 * would run it again for ever. So mepc is set to the stub's ECALL first
 * and the mode is entered by SRET, which M-mode may execute: an MRET
 * that runs lands on the ECALL, in whatever mode MPP named -- U, set
 * here -- and the trap count says so. a0 carries an argument in, and
 * comes back as whatever was in it at the ECALL: below_m_strap, an S-mode
 * handler for the one test that delegates, leaves 0x200 | scause there.
 */
__asm__(".pushsection .text\n"
        ".balign 4\n"
        "below_m_mret:\n"
        "    mret\n"
        "below_m_exit:\n"
        "    ecall\n"
        "below_m_wfi:\n"
        "    wfi\n"
        "    ecall\n"
        "below_m_load:\n"
        "    lw   t0, 0(a0)\n"
        "    ecall\n"
        "    .balign 4\n"
        "below_m_strap:\n"
        "    csrr a0, scause\n"
        "    ori  a0, a0, 0x200\n"
        "    csrr t0, sepc\n"
        "    addi t0, t0, 4\n"
        "    csrw sepc, t0\n"
        "    sret\n"
        ".popsection\n");
extern const char below_m_mret[], below_m_wfi[], below_m_load[];
extern const char below_m_strap[];

static uint32_t run_below_m(const char *stub, uint32_t to_s, uint32_t arg)
{
    uint32_t ret;

    __asm__ volatile("la   t0, 1f\n"
                     "sw   t0, 0(%[resume])\n"
                     "csrw sepc, %[stub]\n"
                     "la   t0, below_m_exit\n"
                     "csrw mepc, t0\n"
                     "li   t0, %[mpp]\n"
                     "csrc mstatus, t0\n"
                     "li   t0, %[spp]\n"
                     "csrc mstatus, t0\n"
                     "beqz %[to_s], 2f\n"
                     "csrs mstatus, t0\n"
                     "2:\n"
                     "mv   a0, %[arg]\n"
                     "sret\n"
                     "1:\n"
                     "mv   %[ret], a0\n"
                     : [ret] "=r"(ret)
                     : [stub] "r"(stub), [to_s] "r"(to_s), [arg] "r"(arg),
                       [resume] "r"(&g_smode_resume),
                       [mpp] "i"(MSTATUS_MPP_MASK), [spp] "i"(MSTATUS_SPP)
                     : "t0", "a0", "memory");
    return ret;
}

static volatile uint32_t g_trig_word;

/* ---- the S-mode bodies -------------------------------------------- */

static volatile uint32_t g_s_scratch;

static void smode_basics(void)
{
    /*
     * Reading an M-mode CSR from S-mode is an illegal instruction, and
     * that -- not a flag anywhere -- is the only way code can tell what
     * privilege it is actually running at.
     */
    const uint32_t before = g_trap_count;
    (void)csr_read("mstatus");
    check("s-mstatus-traps", g_trap_count - before, 1u);
    check("s-mstatus-cause", g_last_cause, 2u);

    /* sstatus is a *view* of mstatus, so it must be readable here. */
    (void)csr_read("sstatus");
    check("s-sstatus-ok", g_trap_count - before, 1u);

    /* And writable, through the S-visible bits only. */
    csr_write("sscratch", 0xA5A5A5A5u);
    check("s-sscratch", csr_read("sscratch"), 0xA5A5A5A5u);

    LEAVE_SMODE();
}

static void smode_ecall_cause(void)
{
    /* Recorded by the handler on the way out; checked by the caller. */
    LEAVE_SMODE();
}

static void smode_tw_wfi(void)
{
    const uint32_t before = g_trap_count;
    __asm__ volatile("wfi");
    check("s-tw-wfi-traps", g_trap_count - before, 1u);
    check("s-tw-wfi-cause", g_last_cause, 2u);
    LEAVE_SMODE();
}

static void smode_tsr_sret(void)
{
    const uint32_t before = g_trap_count;
    __asm__ volatile("sret");
    check("s-tsr-sret-traps", g_trap_count - before, 1u);
    check("s-tsr-sret-cause", g_last_cause, 2u);
    LEAVE_SMODE();
}

static void smode_paging(void)
{
    uint32_t before;

    /* The identity map is transparent: this is ordinary RAM, reached at
     * the address it has always had, with translation now in the path. */
    g_s_scratch = 0x12345678u;
    check("sv32-identity", g_s_scratch, 0x12345678u);

    /* A 4 KiB page, reached through the second-level table. Writing it
     * through VA_RW must land in the physical page underneath. */
    *(volatile uint32_t *)VA_RW = 0xCAFEBABEu;
    check("sv32-4k-write", g_page[0], 0xCAFEBABEu);

    /* The same physical page through a read-only PTE: the load works and
     * the store faults, which is the permission being applied rather than
     * the mapping being found. */
    check("sv32-ro-load", *(volatile uint32_t *)VA_RO, 0xCAFEBABEu);

    before = g_trap_count;
    *(volatile uint32_t *)VA_RO = 0u;
    check("sv32-ro-store-traps", g_trap_count - before, 1u);
    check("sv32-ro-store-cause", g_last_cause, 15u);
    check("sv32-ro-store-tval", g_last_tval, VA_RO);
    check("sv32-ro-store-blocked", g_page[0], 0xCAFEBABEu);

    /* An invalid PTE. */
    before = g_trap_count;
    (void)*(volatile uint32_t *)VA_HOLE;
    check("sv32-hole-traps", g_trap_count - before, 1u);
    check("sv32-hole-cause", g_last_cause, 13u);
    check("sv32-hole-tval", g_last_tval, VA_HOLE);

    /*
     * A U-page is unreachable from S-mode unless SUM says otherwise. This
     * is the check that a supervisor cannot read user memory by accident,
     * and it is a property of the *access*, not of the mapping -- the same
     * PTE serves both halves below.
     */
    before = g_trap_count;
    (void)*(volatile uint32_t *)VA_USER;
    check("sv32-upage-nosum-traps", g_trap_count - before, 1u);
    check("sv32-upage-nosum-cause", g_last_cause, 13u);

    csr_set("sstatus", MSTATUS_SUM);
    before = g_trap_count;
    check("sv32-upage-sum-load", *(volatile uint32_t *)VA_USER, 0xCAFEBABEu);
    check("sv32-upage-sum-notrap", g_trap_count - before, 0u);
    csr_clear("sstatus", MSTATUS_SUM);

    /*
     * Svade: A is checked and never set by the walk, so the first touch of
     * a page whose A is clear faults. An implementation that updated the
     * PTE instead would pass silently here and differ from the golden
     * model on every A/D test.
     */
    before = g_trap_count;
    (void)*(volatile uint32_t *)VA_NOA;
    check("sv32-noa-traps", g_trap_count - before, 1u);
    check("sv32-noa-cause", g_last_cause, 13u);
    check("sv32-noa-unchanged", g_leaf[VPN0(VA_NOA)] & PTE_A, 0u);

    /* D is the same rule for stores: readable, not writable. */
    g_leaf[VPN0(VA_NOA)] |= PTE_A;
    g_leaf[VPN0(VA_NOA)] &= ~PTE_D;
    __asm__ volatile("sfence.vma" ::: "memory");
    before = g_trap_count;
    (void)*(volatile uint32_t *)VA_NOA;
    check("sv32-nod-load-ok", g_trap_count - before, 0u);
    *(volatile uint32_t *)VA_NOA = 0u;
    check("sv32-nod-store-traps", g_trap_count - before, 1u);
    check("sv32-nod-store-cause", g_last_cause, 15u);

    /*
     * A PTE change is not visible until SFENCE.VMA. Making VA_HOLE valid
     * and *not* fencing may legally still fault -- so the check that means
     * something is the one after the fence.
     */
    g_leaf[VPN0(VA_HOLE)] =
        leaf_pte(page_pa(PAGE_DATA), PTE_V | PTE_R | PTE_W | PTE_A | PTE_D);
    __asm__ volatile("sfence.vma" ::: "memory");
    before = g_trap_count;
    check("sv32-sfence-visible", *(volatile uint32_t *)VA_HOLE, 0xCAFEBABEu);
    check("sv32-sfence-notrap", g_trap_count - before, 0u);

    /*
     * The reserved encodings, which are the half of a walk that has no
     * legitimate use and so never appears by accident.
     *
     * W without R is reserved. It is not "write-only" -- it is a fault,
     * and reserving it is what keeps the encoding free for a future
     * extension to define.
     *
     * Applied at the *root*, deliberately. At the leaf level the same
     * encoding faults whether or not anything checks for it, because a PTE
     * with neither R nor X is by definition a pointer and there is no
     * level below the last one -- so a test there passes against an
     * implementation that never heard of the rule. At the root, an
     * implementation that skips the check follows it as a pointer to a
     * perfectly good second-level table and the access *succeeds*.
     */
    const uint32_t saved_root = g_root[VPN1(VA_RW)];

    g_root[VPN1(VA_RW)] = leaf_pte((uint32_t)(uintptr_t)g_leaf, PTE_V | PTE_W);
    __asm__ volatile("sfence.vma" ::: "memory");
    before = g_trap_count;
    (void)*(volatile uint32_t *)VA_RW;
    check("sv32-wnor-traps", g_trap_count - before, 1u);
    check("sv32-wnor-cause", g_last_cause, 13u);
    g_root[VPN1(VA_RW)] = saved_root;
    __asm__ volatile("sfence.vma" ::: "memory");

    /*
     * A leaf whose PPN reaches past 4 GiB -- PTE bit 31 is physical bit
     * 33. Nothing is there on a 32-bit bus, so the load is an access
     * fault; the walk used to drop the bit and read the page 8 GiB below,
     * which is this one (fuzz-rv32-mmu; Sail and Spike agree, probe
     * sv32ppn.S). The page's own word must come back unread.
     */
    const uint32_t saved_hole = g_leaf[VPN0(VA_HOLE)];

    g_leaf[VPN0(VA_HOLE)] = saved_hole | 0x80000000u;
    __asm__ volatile("sfence.vma" ::: "memory");
    before = g_trap_count;
    (void)*(volatile uint32_t *)VA_HOLE;
    check("sv32-past-bus-traps", g_trap_count - before, 1u);
    check("sv32-past-bus-cause", g_last_cause, 5u);
    check("sv32-past-bus-tval", g_last_tval, VA_HOLE);
    g_leaf[VPN0(VA_HOLE)] = saved_hole;
    __asm__ volatile("sfence.vma" ::: "memory");

    /*
     * D, A and U are reserved in a *non-leaf* PTE and must be clear. An
     * implementation that ignores them still translates every legal table
     * correctly, which is why this needs asking directly.
     */
    g_root[VPN1(VA_RW)] |= PTE_A;
    __asm__ volatile("sfence.vma" ::: "memory");
    before = g_trap_count;
    (void)*(volatile uint32_t *)VA_RW;
    check("sv32-nonleaf-a-traps", g_trap_count - before, 1u);
    check("sv32-nonleaf-a-cause", g_last_cause, 13u);

    g_root[VPN1(VA_RW)] &= ~PTE_A;
    g_root[VPN1(VA_RW)] |= PTE_U;
    __asm__ volatile("sfence.vma" ::: "memory");
    before = g_trap_count;
    (void)*(volatile uint32_t *)VA_RW;
    check("sv32-nonleaf-u-traps", g_trap_count - before, 1u);

    /* Put the table back, and prove it: the same access now works. */
    g_root[VPN1(VA_RW)] &= ~PTE_U;
    __asm__ volatile("sfence.vma" ::: "memory");
    before = g_trap_count;
    check("sv32-restored", *(volatile uint32_t *)VA_RW, 0xCAFEBABEu);
    check("sv32-restored-notrap", g_trap_count - before, 0u);

    LEAVE_SMODE();
}

/* ---- executing from a translated page ------------------------------ */

/*
 * The JIT's own path. A block translated from a *virtual* address has to
 * be found by walking the page table, and invalidated when the mapping
 * changes -- neither of which the interpreter has to get right, and
 * neither of which any host suite reaches on the Thumb-2 backend.
 *
 * The stub is `li a0, imm; jalr x0, 0(ra)`, so the value it returns says
 * which copy actually ran.
 */
/*
 * Jump to `entry`, resuming after the jump if the fetch faults.
 *
 * The resume address is a label *inside* the asm, and that is the whole
 * point. Taking the address of a C label with `&&label` and handing it to
 * the trap handler looks equivalent and is not: the compiler owns basic
 * block layout, and here it placed the label's block ahead of the call, so
 * resuming there re-ran the call that faulted -- forever. A label the
 * assembler emits between two instructions cannot be moved out from under
 * the code that jumps to it.
 */
static void call_may_fault(uint32_t entry)
{
    __asm__ volatile("la   t0, 1f\n"
                     "sw   t0, 0(%1)\n"
                     "jalr %0\n"
                     "1:\n" ::"r"(entry),
                     "r"(&g_fetch_resume)
                     : "t0", "t1", "t2", "ra", "a0", "a1", "a2", "a3", "a4",
                       "a5", "a6", "a7", "memory");
}

static void smode_exec(void)
{
    uint32_t (*fn)(void) = (uint32_t (*)(void))VA_X;
    uint32_t before;

    check("sv32-exec-first", fn(), 0x111u);

    /* Repoint the same VA at the second stub. Without the fence a stale
     * translation -- in the TLB or in a cached JIT block -- would keep
     * answering with the first. */
    g_leaf[VPN0(VA_X)] =
        leaf_pte(page_pa(PAGE_EXEC2), PTE_V | PTE_R | PTE_X | PTE_A);
    __asm__ volatile("sfence.vma" ::: "memory");
    check("sv32-exec-remapped", fn(), 0x222u);

    /* Withdraw execute permission and the fetch itself must fault. */
    g_leaf[VPN0(VA_X)] = leaf_pte(page_pa(PAGE_EXEC2), PTE_V | PTE_R | PTE_A);
    __asm__ volatile("sfence.vma" ::: "memory");
    before = g_trap_count;
    call_may_fault(VA_X);
    check("sv32-exec-nox-traps", g_trap_count - before, 1u);
    check("sv32-exec-nox-cause", g_last_cause, 12u);
    check("sv32-exec-nox-tval", g_last_tval, VA_X);

    LEAVE_SMODE();
}

/* ---- a trap from inside a translated block ------------------------ */

/*
 * A page of its own in .text, executable by M-mode and denied to S-mode.
 *
 * That split is what PMP gives for free: an *unlocked* entry binds below
 * M only, so the page can be the machine trap vector and still be one
 * S-mode must not fetch from. The stub is
 *
 *   +0  j +8                    translatable; the block the bug files
 *   +4  nop                     under the wrong context
 *   +8  csrrs x0, mscratch, x0  declined, so no block is ever built here
 *   +12 j trap_handler
 *
 * and the page is padded on both sides so that nothing else shares the
 * PMP region. `norvc`, because those offsets are the test's expectation:
 * compressed, `j` and `nop` are two bytes each and the second
 * instruction moves to +4.
 */
__asm__(".pushsection .text\n"
        ".balign 4096\n"
        ".option push\n"
        ".option norvc\n"
        "g_ctx_stub:\n"
        "    j 1f\n"
        "    nop\n"
        "1:  csrrs x0, mscratch, x0\n"
        "    j trap_handler\n"
        ".option pop\n"
        ".balign 4096\n"
        ".popsection\n");
extern const uint32_t g_ctx_stub[];

static volatile uint32_t g_ctx_load_cause;
static volatile uint32_t g_ctx_fetch_cause;
static volatile uint32_t g_ctx_fetch_tval;

/*
 * The load faults inside a translated block -- it is in the body's first
 * block, which has nothing to decline -- so the trap is raised by the
 * JIT's load helper rather than by the interpreter. Then the jump: S-mode
 * may not fetch the stub page, so the fetch must fault, *at the stub*.
 *
 * The causes are copied out before LEAVE_SMODE, whose ecall would
 * overwrite them.
 */
static void smode_trap_in_block(void)
{
    (void)*(volatile const uint32_t *)&g_ctx_stub[64];
    g_ctx_load_cause = g_last_cause;

    call_may_fault((uint32_t)(uintptr_t)g_ctx_stub);
    g_ctx_fetch_cause = g_last_cause;
    g_ctx_fetch_tval = g_last_tval;

    LEAVE_SMODE();
}

/*
 * A trap taken inside a translated block must move the JIT's context.
 *
 * It did not. The context -- which carries the privilege -- was
 * refreshed only on the interpreter fallback, so after the load above
 * faulted the dispatch looked the machine-mode handler up under the
 * *supervisor's* context, translated it there, and filed it there. The
 * jump that follows then found that block and ran it in S-mode, from a
 * page S-mode may not execute: the fetch fault arrived at +8, where the
 * translator had stopped, instead of at +0.
 *
 * So the check that names the mechanism is the tval. A cause of 1 alone
 * would pass against the bug.
 */
static void test_jit_trap_context(void)
{
    const uint32_t stub = (uint32_t)(uintptr_t)g_ctx_stub;

    csr_write("pmpaddr8", (stub >> 2) | 0x1FFu); /* NAPOT, 4 KiB */
    csr_write("pmpcfg2", 0x18u); /* entry 8: NAPOT, nothing, unlocked */
    check("jit-ctx-pmp", csr_read("pmpcfg2") & 0xFFu, 0x18u);

    csr_write("medeleg", 0u);
    csr_write("mtvec", stub);

    enter_smode(smode_trap_in_block);

    install_trap_handler();
    csr_write("pmpcfg2", 0u);

    check("jit-ctx-load-cause", g_ctx_load_cause, 5u);
    check("jit-ctx-fetch-cause", g_ctx_fetch_cause, 1u);
    check("jit-ctx-fetch-tval", g_ctx_fetch_tval, stub);
}

/* ---- the M-mode driver --------------------------------------------- */

static void test_smode(void)
{
    /*
     * A background PMP entry first, and this is not optional.
     *
     * Below M-mode, matching *no* PMP entry denies rather than permits,
     * and the tests above have locked three small regions. Entering S-mode
     * without a catch-all would fault on the first instruction fetched,
     * with no handler reachable to report it. Index 15 is the lowest
     * priority, so the three locked entries keep their meaning.
     */
#if RV32_EXT_PMP
    csr_write("pmpaddr15", 0xFFFFFFFFu);
    csr_write("pmpcfg3", 0x9F000000u); /* byte 3: L | NAPOT | RWX */
    check("s-pmp-background", (csr_read("pmpcfg3") >> 24) & 0xFFu, 0x9Fu);
#endif

    csr_write("stvec", 0u);
    csr_write("medeleg", 0u); /* everything to M for now */
    csr_write("mideleg", 0u);

    uint32_t before = g_trap_count;
    enter_smode(smode_basics);
    /* The illegal mstatus read, and the ecall on the way out. */
    check("s-returned", g_trap_count - before, 2u);

    before = g_trap_count;
    enter_smode(smode_ecall_cause);
    check("s-ecall-cause", g_last_cause, 9u);
    check("s-ecall-once", g_trap_count - before, 1u);

    /* TW and TSR are how M-mode keeps a supervisor from waiting or
     * returning behind its back. Both apply to S-mode only. */
    csr_set("mstatus", MSTATUS_TW);
    enter_smode(smode_tw_wfi);
    csr_clear("mstatus", MSTATUS_TW);

    csr_set("mstatus", MSTATUS_TSR);
    enter_smode(smode_tsr_sret);
    csr_clear("mstatus", MSTATUS_TSR);

    /* With TSR clear, SRET is legal again -- the trap above was the bit
     * doing its job, not SRET being unimplemented. */
    check("s-tsr-cleared", (csr_read("mstatus") & MSTATUS_TSR), 0u);

    /*
     * What a lower mode may not execute, from each of them, with TW and
     * TSR clear so the rule is the instruction's own. MRET ran in S-mode
     * and U-mode alike, returning to whatever MPP held -- machine mode
     * for the asking, from U -- and WFI parked the hart from U-mode.
     * The trap and then the stub's ECALL make two; an MRET that runs
     * reaches the ECALL alone, from U.
     */
    {
        uint32_t before = g_trap_count;

        run_below_m(below_m_mret, 1u, 0u);
        check("s-mret-traps", g_trap_count - before, 2u);
        check("s-mret-cause", g_prev_cause, 2u);
        check("s-mret-tval", g_prev_tval, 0x30200073u);
        check("s-mret-stays-s", g_last_cause, 9u);

        before = g_trap_count;
        run_below_m(below_m_mret, 0u, 0u);
        check("u-mret-traps", g_trap_count - before, 2u);
        check("u-mret-cause", g_prev_cause, 2u);
        check("u-mret-tval", g_prev_tval, 0x30200073u);

        before = g_trap_count;
        run_below_m(below_m_wfi, 0u, 0u);
        check("u-wfi-traps", g_trap_count - before, 2u);
        check("u-wfi-cause", g_prev_cause, 2u);
        check("u-wfi-tval", g_prev_tval, 0x10500073u);
    }

    /*
     * A trigger's m, s and u bits are the modes it matches in. Only m
     * was read: a trigger set for S never fired, and one set for M alone
     * fired in every mode. medeleg is clear, so the breakpoint goes to M
     * and Sdtrig's rule for S -- quiet while breakpoints are delegated
     * and SIE is clear -- does not come into it.
     */
    {
        const uint32_t word = (uint32_t)(uintptr_t)&g_trig_word;
        uint32_t before;

        csr_write("tselect", 0u);
        csr_write("tdata2", word);
        csr_write("tdata1", 0x20000011u); /* mcontrol: S, load */
        /*
         * With SIE clear in S (SRET sets it from SPIE): breakpoints go
         * to M, so the reentrancy rule below is not in force and the
         * trigger fires. SIE set would let a rule that forgot the
         * delegation pass.
         */
        csr_clear("mstatus", MSTATUS_SPIE);
        before = g_trap_count;
        run_below_m(below_m_load, 1u, word);
        check("trig-s-fires-in-s", g_trap_count - before, 2u);
        check("trig-s-cause", g_prev_cause, 3u);
        check("trig-s-tval", g_prev_tval, word);
        before = g_trap_count;
        (void)g_trig_word;
        check("trig-s-quiet-in-m", g_trap_count - before, 0u);

        csr_write("tdata1", 0x20000041u); /* mcontrol: M, load */
        before = g_trap_count;
        run_below_m(below_m_load, 1u, word);
        check("trig-m-quiet-in-s", g_trap_count - before, 1u);
        before = g_trap_count;
        (void)g_trig_word;
        check("trig-m-fires-in-m", g_trap_count - before, 1u);
        check("trig-m-cause", g_last_cause, 3u);

        /*
         * Reentrancy (debug spec 1.0, 5.4): no breakpoint trigger
         * matches in M-mode while MIE is clear, nor in S-mode while
         * breakpoints go to S and SIE is clear -- the state a handler
         * is entered in, where firing again would overwrite the cause
         * and epc it is handling. Spike holds to the same
         * (tests/arch-test/probes/sdtrig.S). mie is cleared so that
         * setting MIE takes nothing else.
         */
        const uint32_t mstatus_was = csr_read("mstatus");
        const uint32_t mie_was = csr_read("mie");
        uint32_t got;

        csr_write("mie", 0u);
        csr_clear("mstatus", MSTATUS_MIE);
        before = g_trap_count;
        (void)g_trig_word;
        check("trig-m-mie0-quiet", g_trap_count - before, 0u);
        csr_set("mstatus", MSTATUS_MIE);
        before = g_trap_count;
        (void)g_trig_word;
        check("trig-m-mie1-fires", g_trap_count - before, 1u);
        csr_clear("mstatus", MSTATUS_MIE);

        csr_write("stvec", (uint32_t)(uintptr_t)below_m_strap);
        csr_write("medeleg", 1u << 3);
        csr_write("tdata1", 0x20000011u); /* mcontrol: S, load */
        /* S is entered by SRET, which sets SIE from SPIE. */
        csr_clear("mstatus", MSTATUS_SPIE);
        before = g_trap_count;
        got = run_below_m(below_m_load, 1u, word);
        check("trig-s-deleg-sie0-quiet", got, word);
        check("trig-s-deleg-sie0-traps", g_trap_count - before, 1u);
        csr_set("mstatus", MSTATUS_SPIE);
        got = run_below_m(below_m_load, 1u, word);
        check("trig-s-deleg-sie1-fires", got, 0x203u);
        csr_write("medeleg", 0u);
        csr_write("stvec", 0u);
        csr_write("mstatus", mstatus_was);
        csr_write("mie", mie_was);
        csr_write("tdata1", 0u);
    }

    /* medeleg and mideleg are real registers now, and WARL. */
    /*
     * Causes 0-9, plus the three page faults -- 12, 13, 15 -- because
     * this build has Sv32 and can raise them. 14 is reserved and must
     * read back clear, which is what makes this a WARL test rather than
     * a constant: 0xB3FF and not 0xF3FF.
     */
    csr_write("medeleg", 0xFFFFFFFFu);
    check("s-medeleg-warl", csr_read("medeleg"), 0xB3FFu);
    csr_write("medeleg", 0u);
    csr_write("mideleg", 0xFFFFFFFFu);
    /* SSIP, STIP, SEIP -- and LCOFIP, bit 13, with Sscofpmf. */
    check("s-mideleg-warl", csr_read("mideleg"), 0x2222u);
    csr_write("mideleg", 0u);
}

static void test_sv32(void)
{
    /*
     * satp.PPN is 20 bits here, not the 22 Sv32 defines: the field is WARL
     * and its width follows the *physical* address space, which is 32-bit
     * on this bus. Reporting a root table the walk cannot reach would be
     * worse than reporting a narrower one.
     */
    csr_write("satp", 0xFFFFFFFFu);
    /* MODE, nine bits of ASID, and twenty of PPN: 0xFFCFFFFF. The two
     * bits missing from the architectural 22-bit PPN are the ones a
     * 32-bit physical address cannot name. */
    check("sv32-satp-warl", csr_read("satp"), 0xFFCFFFFFu);
    csr_write("satp", 0u);

    build_page_tables();

    /*
     * The two stubs the execute test jumps to, written into the two pages
     * that VA_X is pointed at in turn.
     *
     *   li a0, imm ; jalr x0, 0(ra)
     */
    g_page[1024] = 0x11100513u; /* li a0, 0x111 */
    g_page[1025] = 0x00008067u; /* ret          */
    g_page[2048] = 0x22200513u;
    g_page[2049] = 0x00008067u;
    __asm__ volatile("fence.i" ::: "memory");

    csr_write("satp", SATP_SV32 | ((uint32_t)(uintptr_t)g_root >> 12));
    check("sv32-satp-on", csr_read("satp") >> 31, 1u);

    /* M-mode is never translated, so nothing has changed here yet. */
    g_s_scratch = 0x5A5A5A5Au;
    check("sv32-m-untranslated", g_s_scratch, 0x5A5A5A5Au);

    enter_smode(smode_paging);
    enter_smode(smode_exec);

    /* Back to Bare, so nothing after this runs under a stale map. */
    csr_write("satp", 0u);
    check("sv32-satp-off", csr_read("satp"), 0u);
}

static void test_aplic(void)
{
    const uint32_t A = 3u, B = 4u;

    /* Bits 31:24 read back 0x80 so software can identify the register. */
    APLIC_DOMAINCFG = 0u;
    check("aplic-domaincfg-id", APLIC_DOMAINCFG & 0xFF000000u, 0x80000000u);
    check("aplic-ie-clear", APLIC_DOMAINCFG & 0x100u, 0u);
    APLIC_DOMAINCFG = 0x100u;
    check("aplic-ie-set", APLIC_DOMAINCFG & 0x100u, 0x100u);

    /* SM is WARL: 2 and 3 are undefined and must read back as Inactive. */
    APLIC_SOURCECFG(A) = APLIC_SM_EDGE_RISE;
    check("aplic-sourcecfg", APLIC_SOURCECFG(A), APLIC_SM_EDGE_RISE);
    APLIC_SOURCECFG(A) = 2u;
    check("aplic-sourcecfg-warl", APLIC_SOURCECFG(A), APLIC_SM_INACTIVE);

    /* An inactive source cannot become pending at all. */
    APLIC_SETIPNUM = A;
    check("aplic-inactive-not-pending", APLIC_SETIP & (1u << A), 0u);

    APLIC_SOURCECFG(A) = APLIC_SM_EDGE_RISE;
    APLIC_SOURCECFG(B) = APLIC_SM_EDGE_RISE;
    APLIC_TARGET(A) = 5u; /* lower number is higher priority */
    APLIC_TARGET(B) = 2u;
    APLIC_SETIENUM = A;
    APLIC_SETIENUM = B;
    check("aplic-setie", APLIC_SETIE & ((1u << A) | (1u << B)),
          (1u << A) | (1u << B));

    /* Pending but not yet delivered: idelivery is still clear. */
    APLIC_SETIPNUM = A;
    check("aplic-pending", APLIC_SETIP & (1u << A), 1u << A);
    check("aplic-topi", APLIC_TOPI, (A << 16) | 5u);

    /* Claim without delivery still works, and clears the pending bit. */
    check("aplic-claimi", APLIC_CLAIMI, (A << 16) | 5u);
    check("aplic-claim-clears", APLIC_SETIP & (1u << A), 0u);
    check("aplic-topi-empty", APLIC_TOPI, 0u);

    /* Priority: the lower IPRIO wins regardless of source number. */
    APLIC_SETIPNUM = A;
    APLIC_SETIPNUM = B;
    check("aplic-priority", APLIC_TOPI, (B << 16) | 2u);
    (void)APLIC_CLAIMI;
    check("aplic-priority-next", APLIC_TOPI, (A << 16) | 5u);
    (void)APLIC_CLAIMI;

    /* ithreshold admits only priorities strictly below it. */
    APLIC_ITHRESHOLD = 5u;
    APLIC_SETIPNUM = A; /* priority 5, not below 5 */
    check("aplic-threshold-blocks", APLIC_TOPI, 0u);
    APLIC_ITHRESHOLD = 6u;
    check("aplic-threshold-admits", APLIC_TOPI, (A << 16) | 5u);
    APLIC_ITHRESHOLD = 0u; /* zero disables the filter */
    APLIC_CLRIPNUM = A;
    check("aplic-clripnum", APLIC_SETIP & (1u << A), 0u);

    /* Now the delivery path, end to end. */
    APLIC_IDELIVERY = 1u;
    csr_set("mie", 1u << 11); /* MEIE */

    const uint32_t before = g_ext_count;
    APLIC_SETIPNUM = B;
    csr_set("mstatus", 1u << 3); /* MIE: the trap fires here */
    csr_clear("mstatus", 1u << 3);

    check("aplic-delivered", g_ext_count - before, 1u);
    check("aplic-claim-value", g_last_claim, (B << 16) | 2u);
    check("aplic-cause-ext", g_last_cause, 0x8000000Bu);
    check("aplic-quiet-after", APLIC_TOPI, 0u);

    /*
     * domaincfg.IE gates delivery without disturbing any other state:
     * topi keeps reporting the source even while nothing can be delivered.
     */
    APLIC_DOMAINCFG = 0u;
    APLIC_SETIPNUM = B;
    check("aplic-ie-off-topi", APLIC_TOPI, (B << 16) | 2u);
    const uint32_t before2 = g_ext_count;
    csr_set("mstatus", 1u << 3);
    csr_clear("mstatus", 1u << 3);
    check("aplic-ie-off-quiet", g_ext_count - before2, 0u);

    /* Re-enabling the domain delivers what was already pending. */
    APLIC_DOMAINCFG = 0x100u;
    csr_set("mstatus", 1u << 3);
    csr_clear("mstatus", 1u << 3);
    check("aplic-ie-on-delivers", g_ext_count - before2, 1u);

    csr_clear("mie", 1u << 11);

    /*
     * A read whose value is thrown away is still a read -- and this is
     * the one that matters here, because reading claimi is what
     * acknowledges the interrupt. The load's destination is overwritten
     * on the next line with nothing in between, which is exactly the
     * shape the JIT's dead-value pass deleted: see test_discarded_reads.
     * On a real peripheral it is `(void)UART->DR;`.
     */
    APLIC_SETIPNUM = B;
    check("aplic-discard-pending", APLIC_TOPI, (B << 16) | 2u);
    __asm__ volatile("lw   t0, 0(%0)\n"
                     "li   t0, 0" ::"r"(&APLIC_CLAIMI)
                     : "t0", "memory");
    check("aplic-discarded-claim-acks", APLIC_TOPI, 0u);

    APLIC_IDELIVERY = 0u;
    APLIC_DOMAINCFG = 0u;
}

/* ------------------------------------------------------------------ */
/* Extensions the guest compiler is not asked for                      */
/* ------------------------------------------------------------------ */

/*
 * Everything from here to main() tests an extension the emulator
 * implements and RV_GUEST_MARCH does not name. That is deliberate: the
 * march decides what the *compiler* emits for every guest in the tree,
 * and adding Zicond to it would change the instruction mix -- and so
 * every published retired count -- for the sake of a test. `.option
 * arch` scopes the extension to one statement instead, and the
 * assembler still does the encoding, which is the half that matters: a
 * `.word` would be this file's reading of the manual checked against
 * the emulator's reading of the same manual.
 *
 * (It is also the only way to reach Zalasr at all. GCC 15 does not take
 * it in -march; binutils 2.47 assembles it.)
 */
#define WITH_EXT(ext, body)                                                    \
    ".option push\n.option arch, +" ext "\n" body "\n.option pop\n"

static void arm_timer_in(uint32_t ticks)
{
    /* High half to all-ones first, as test_timer_interrupt does. */
    CLINT_MTIMECMP_HI = 0xFFFFFFFFu;
    CLINT_MTIMECMP_LO = CLINT_MTIME_LO + ticks;
    CLINT_MTIMECMP_HI = CLINT_MTIME_HI;
}

static void disarm_timer(void)
{
    CLINT_MTIMECMP_HI = 0xFFFFFFFFu;
    CLINT_MTIMECMP_LO = 0xFFFFFFFFu;
}

/* ---- Zicond -------------------------------------------------------- */

/* rd, rs1 and rs2 all distinct, and chosen by the compiler. */
#define CZERO(mn, a, b)                                                        \
    ({                                                                         \
        uint32_t r_;                                                           \
        __asm__ volatile(WITH_EXT("zicond", mn " %0, %1, %2")                  \
                         : "=&r"(r_)                                           \
                         : "r"(opaque(a)), "r"(opaque(b)));                    \
        r_;                                                                    \
    })

/*
 * The same instruction with its registers named: t0 holds the value, t1
 * the condition, and `seq` leaves its answer in t2.
 *
 * Named because the aliasings are the whole risk. czero is a select
 * between rs1 and zero keyed on rs2, and an implementation that writes
 * rd before it has finished reading -- which is what a lowering to "test,
 * then move" does when rd is the register being tested -- is right for
 * every case the compiler happens to allocate and wrong for these.
 */
#define CZERO_REGS(seq, a, b)                                                  \
    ({                                                                         \
        register uint32_t t0_ __asm__("t0") = opaque(a);                       \
        register uint32_t t1_ __asm__("t1") = opaque(b);                       \
        register uint32_t t2_ __asm__("t2") = 0xDEADBEEFu;                     \
        __asm__ volatile(WITH_EXT("zicond", seq)                               \
                         : "+r"(t0_), "+r"(t1_), "+r"(t2_));                   \
        t2_;                                                                   \
    })

static void test_zicond(void)
{
    const uint32_t before = g_trap_count;
    const uint32_t v = 0x12345678u;

    /*
     * The condition is "rs2 is zero", over all 32 bits. One and zero
     * cannot tell that from "bit 0 is clear" or from a signed compare;
     * a condition that is only its sign bit, and one whose low half is
     * zero, can.
     */
    check("czero.eqz-0", CZERO("czero.eqz", v, 0u), 0u);
    check("czero.eqz-1", CZERO("czero.eqz", v, 1u), v);
    check("czero.eqz-msb", CZERO("czero.eqz", v, 0x80000000u), v);
    check("czero.eqz-hi", CZERO("czero.eqz", v, 0x00010000u), v);
    check("czero.nez-0", CZERO("czero.nez", v, 0u), v);
    check("czero.nez-1", CZERO("czero.nez", v, 1u), 0u);
    check("czero.nez-msb", CZERO("czero.nez", v, 0x80000000u), 0u);
    check("czero.nez-hi", CZERO("czero.nez", v, 0x00010000u), 0u);

    /* rd == rs1: the value is replaced in place. */
    check("czero.eqz-rd=rs1-z",
          CZERO_REGS("czero.eqz t0, t0, t1\nmv t2, t0", v, 0u), 0u);
    check("czero.eqz-rd=rs1-nz",
          CZERO_REGS("czero.eqz t0, t0, t1\nmv t2, t0", v, 5u), v);

    /* rd == rs2: the condition is overwritten by what it selects. */
    check("czero.eqz-rd=rs2-z",
          CZERO_REGS("czero.eqz t1, t0, t1\nmv t2, t1", v, 0u), 0u);
    check("czero.eqz-rd=rs2-nz",
          CZERO_REGS("czero.eqz t1, t0, t1\nmv t2, t1", v, 5u), v);
    check("czero.nez-rd=rs2-z",
          CZERO_REGS("czero.nez t1, t0, t1\nmv t2, t1", v, 0u), v);
    check("czero.nez-rd=rs2-nz",
          CZERO_REGS("czero.nez t1, t0, t1\nmv t2, t1", v, 5u), 0u);

    /* rs1 == rs2: a value conditioned on itself. */
    check("czero.eqz-rs1=rs2", CZERO_REGS("czero.eqz t2, t1, t1", 0u, 7u), 7u);
    check("czero.nez-rs1=rs2", CZERO_REGS("czero.nez t2, t1, t1", 0u, 7u), 0u);

    /* x0 as the condition is always zero, and as the value is zero. */
    check("czero.eqz-rs2=x0", CZERO_REGS("czero.eqz t2, t0, x0", v, 5u), 0u);
    check("czero.nez-rs2=x0", CZERO_REGS("czero.nez t2, t0, x0", v, 5u), v);
    check("czero.nez-rs1=x0", CZERO_REGS("czero.nez t2, x0, t1", v, 0u), 0u);

    /*
     * x0 as the destination. The select here *would* produce v, so a
     * backend that forwards the last value written to a register hands
     * v to the read that follows -- where the architecture guarantees
     * zero. See pass_reg_traffic in CLAUDE.md for the time that happened.
     */
    check("czero-rd=x0",
          CZERO_REGS("czero.nez x0, t0, x0\nadd t2, x0, x0", v, 5u), 0u);

    check("zicond-no-trap", g_trap_count - before, 0u);
}

/* ---- Zba, Zbb, Zbc, Zbs ------------------------------------------- */

/*
 * The four bit-manipulation extensions, instruction by instruction.
 *
 * **This guest executed almost none of them before**: 33 sh2add, one
 * zext.h and six single-bit operations in a whole run, every one of
 * them whatever the compiler happened to emit for an array index. The
 * official suite covers the extensions properly and cannot run on a
 * board -- so the Thumb-2 lowering of each of these was checked by
 * nothing, which did not matter while the translator declined them all
 * and matters now that it does not.
 *
 * Every expectation is from tests/guest/bitmanip-expect.py, a model
 * written from the specification with unbounded integers: no 32-bit
 * shift, no builtin, nothing shared with either implementation. Never
 * assert a value the implementation computed.
 *
 * The inputs are the awkward ones, because those are where the two
 * hosts differ from the guest and from each other:
 *
 *   clz, ctz of zero       32, where x86's BSR/BSF leave the result
 *                          undefined and ARM's CLZ does not
 *   rotate, bit index 32   the low five bits only -- an ARM register
 *                          shift reads the low *byte* and saturates
 *   rotate by 0            which on ARM is the RRX encoding
 *   min/max across the     0x80000000 against 0x7FFFFFFF, where signed
 *   sign boundary          and unsigned disagree about every answer
 *   clmulh / clmulr        differ by one bit position, and agree for
 *                          any product that fits 31 bits
 *   andn / orn             not symmetric: both operand orders
 *
 * And each family again with its registers named, for the aliasings: an
 * instruction whose destination is one of its own operands is right for
 * every allocation the compiler chooses and wrong for these when a
 * lowering writes before it has finished reading.
 */
#define BM_ARCH "zba,+zbb,+zbc,+zbs"

/* rd, rs1 and rs2 distinct, chosen by the compiler. */
#define B2(mn, a, b)                                                           \
    ({                                                                         \
        uint32_t r_;                                                           \
        __asm__ volatile(WITH_EXT(BM_ARCH, mn " %0, %1, %2")                   \
                         : "=&r"(r_)                                           \
                         : "r"(opaque(a)), "r"(opaque(b)));                    \
        r_;                                                                    \
    })

#define B1(mn, a)                                                              \
    ({                                                                         \
        uint32_t r_;                                                           \
        __asm__ volatile(WITH_EXT(BM_ARCH, mn " %0, %1")                       \
                         : "=&r"(r_)                                           \
                         : "r"(opaque(a)));                                    \
        r_;                                                                    \
    })

#define BI(mn, a, n)                                                           \
    ({                                                                         \
        uint32_t r_;                                                           \
        __asm__ volatile(WITH_EXT(BM_ARCH, mn " %0, %1, " #n)                  \
                         : "=&r"(r_)                                           \
                         : "r"(opaque(a)));                                    \
        r_;                                                                    \
    })

/* t0 and t1 hold the operands; `seq` leaves its answer in t2. */
#define B_REGS(seq, a, b)                                                      \
    ({                                                                         \
        register uint32_t t0_ __asm__("t0") = opaque(a);                       \
        register uint32_t t1_ __asm__("t1") = opaque(b);                       \
        register uint32_t t2_ __asm__("t2") = 0xDEADBEEFu;                     \
        __asm__ volatile(WITH_EXT(BM_ARCH, seq)                                \
                         : "+r"(t0_), "+r"(t1_), "+r"(t2_));                   \
        t2_;                                                                   \
    })

static void test_bitmanip(void)
{
    const uint32_t before = g_trap_count;

    /*
     * Zba. (1, 1) alone tells the three apart -- 3, 5 and 9 -- which is
     * the funct3-to-shift mapping; the others carry out of bit 31.
     */
    check("sh1add 00000001,00000001", B2("sh1add", 0x00000001u, 0x00000001u),
          0x00000003u);
    check("sh1add 80000001,7fffffff", B2("sh1add", 0x80000001u, 0x7FFFFFFFu),
          0x80000001u);
    check("sh1add ffffffff,ffffffff", B2("sh1add", 0xFFFFFFFFu, 0xFFFFFFFFu),
          0xFFFFFFFDu);
    check("sh2add 00000001,00000001", B2("sh2add", 0x00000001u, 0x00000001u),
          0x00000005u);
    check("sh2add 40000001,00000003", B2("sh2add", 0x40000001u, 0x00000003u),
          0x00000007u);
    check("sh2add ffffffff,ffffffff", B2("sh2add", 0xFFFFFFFFu, 0xFFFFFFFFu),
          0xFFFFFFFBu);
    check("sh3add 00000001,00000001", B2("sh3add", 0x00000001u, 0x00000001u),
          0x00000009u);
    check("sh3add 20000001,fffffff8", B2("sh3add", 0x20000001u, 0xFFFFFFF8u),
          0x00000000u);
    check("sh3add ffffffff,ffffffff", B2("sh3add", 0xFFFFFFFFu, 0xFFFFFFFFu),
          0xFFFFFFF7u);
    check("sh2add rd=rs1",
          B_REGS("sh2add t0, t0, t1\nmv t2, t0", 0x00001000u, 0x00000234u),
          0x00004234u);
    check("sh2add rd=rs2",
          B_REGS("sh2add t1, t0, t1\nmv t2, t1", 0x00001000u, 0x00000234u),
          0x00004234u);
    check("sh2add rs1=rs2",
          B_REGS("sh2add t2, t1, t1", 0x00001000u, 0x00000234u), 0x00000B04u);
    check("sh3add rd=rs1=rs2",
          B_REGS("sh3add t1, t1, t1\nmv t2, t1", 0x00001000u, 0x00000234u),
          0x000013D4u);
    check("sh1add rd=x0",
          B_REGS("sh1add x0, t0, t1\nadd t2, x0, x0", 0x00001000u, 0x00000234u),
          0x00000000u);
    check("sh1add rs1=x0",
          B_REGS("sh1add t2, x0, t1", 0x00001000u, 0x00000234u), 0x00000234u);
    check("sh1add rs2=x0",
          B_REGS("sh1add t2, t0, x0", 0x00001000u, 0x00000234u), 0x00002000u);

    /* Zbb: the three logical operations with an inverted operand. */
    check("andn ff00ff00,0f0f0f0f", B2("andn", 0xFF00FF00u, 0x0F0F0F0Fu),
          0xF000F000u);
    check("andn 0f0f0f0f,ff00ff00", B2("andn", 0x0F0F0F0Fu, 0xFF00FF00u),
          0x000F000Fu);
    check("andn 00000000,00000000", B2("andn", 0x00000000u, 0x00000000u),
          0x00000000u);
    check("andn ffffffff,00000000", B2("andn", 0xFFFFFFFFu, 0x00000000u),
          0xFFFFFFFFu);
    check("andn 00000000,ffffffff", B2("andn", 0x00000000u, 0xFFFFFFFFu),
          0x00000000u);
    check("orn ff00ff00,0f0f0f0f", B2("orn", 0xFF00FF00u, 0x0F0F0F0Fu),
          0xFFF0FFF0u);
    check("orn 0f0f0f0f,ff00ff00", B2("orn", 0x0F0F0F0Fu, 0xFF00FF00u),
          0x0FFF0FFFu);
    check("orn 00000000,00000000", B2("orn", 0x00000000u, 0x00000000u),
          0xFFFFFFFFu);
    check("orn ffffffff,00000000", B2("orn", 0xFFFFFFFFu, 0x00000000u),
          0xFFFFFFFFu);
    check("orn 00000000,ffffffff", B2("orn", 0x00000000u, 0xFFFFFFFFu),
          0x00000000u);
    check("xnor ff00ff00,0f0f0f0f", B2("xnor", 0xFF00FF00u, 0x0F0F0F0Fu),
          0x0FF00FF0u);
    check("xnor 0f0f0f0f,ff00ff00", B2("xnor", 0x0F0F0F0Fu, 0xFF00FF00u),
          0x0FF00FF0u);
    check("xnor 00000000,00000000", B2("xnor", 0x00000000u, 0x00000000u),
          0xFFFFFFFFu);
    check("xnor ffffffff,00000000", B2("xnor", 0xFFFFFFFFu, 0x00000000u),
          0x00000000u);
    check("xnor 00000000,ffffffff", B2("xnor", 0x00000000u, 0xFFFFFFFFu),
          0x00000000u);
    check("andn rd=rs2",
          B_REGS("andn t1, t0, t1\nmv t2, t1", 0xFF00FF00u, 0x0F0F0F0Fu),
          0xF000F000u);
    check("andn rd=rs1",
          B_REGS("andn t0, t0, t1\nmv t2, t0", 0xFF00FF00u, 0x0F0F0F0Fu),
          0xF000F000u);
    check("andn rs1=rs2", B_REGS("andn t2, t1, t1", 0xFF00FF00u, 0x0F0F0F0Fu),
          0x00000000u);
    check("orn rd=rs2",
          B_REGS("orn t1, t0, t1\nmv t2, t1", 0xFF00FF00u, 0x0F0F0F0Fu),
          0xFFF0FFF0u);
    check("orn rs1=rs2", B_REGS("orn t2, t1, t1", 0xFF00FF00u, 0x0F0F0F0Fu),
          0xFFFFFFFFu);
    check("xnor rd=rs2",
          B_REGS("xnor t1, t0, t1\nmv t2, t1", 0xFF00FF00u, 0x0F0F0F0Fu),
          0x0FF00FF0u);
    check("xnor rs1=rs2", B_REGS("xnor t2, t1, t1", 0xFF00FF00u, 0x0F0F0F0Fu),
          0xFFFFFFFFu);
    check("orn rd=x0",
          B_REGS("orn x0, t0, t1\nadd t2, x0, x0", 0xFF00FF00u, 0x0F0F0F0Fu),
          0x00000000u);

    /* Zbb: counts. Zero is the input that matters. */
    check("clz 00000000", B1("clz", 0x00000000u), 0x00000020u);
    check("clz 00000001", B1("clz", 0x00000001u), 0x0000001Fu);
    check("clz 80000000", B1("clz", 0x80000000u), 0x00000000u);
    check("clz 00010000", B1("clz", 0x00010000u), 0x0000000Fu);
    check("clz 7fffffff", B1("clz", 0x7FFFFFFFu), 0x00000001u);
    check("clz ffffffff", B1("clz", 0xFFFFFFFFu), 0x00000000u);
    check("ctz 00000000", B1("ctz", 0x00000000u), 0x00000020u);
    check("ctz 00000001", B1("ctz", 0x00000001u), 0x00000000u);
    check("ctz 80000000", B1("ctz", 0x80000000u), 0x0000001Fu);
    check("ctz 00010000", B1("ctz", 0x00010000u), 0x00000010u);
    check("ctz fffffffe", B1("ctz", 0xFFFFFFFEu), 0x00000001u);
    check("ctz ffffffff", B1("ctz", 0xFFFFFFFFu), 0x00000000u);
    check("cpop 00000000", B1("cpop", 0x00000000u), 0x00000000u);
    check("cpop ffffffff", B1("cpop", 0xFFFFFFFFu), 0x00000020u);
    check("cpop 80000001", B1("cpop", 0x80000001u), 0x00000002u);
    check("cpop 12345678", B1("cpop", 0x12345678u), 0x0000000Du);
    check("cpop 00010000", B1("cpop", 0x00010000u), 0x00000001u);
    check("clz rd=rs1",
          B_REGS("clz t0, t0\nmv t2, t0", 0x00010000u, 0x00000000u),
          0x0000000Fu);
    check("clz rd=rs1 zero",
          B_REGS("clz t0, t0\nmv t2, t0", 0x00000000u, 0x00000000u),
          0x00000020u);
    check("ctz rd=rs1 zero",
          B_REGS("ctz t0, t0\nmv t2, t0", 0x00000000u, 0x00000000u),
          0x00000020u);
    check("cpop rd=rs1",
          B_REGS("cpop t0, t0\nmv t2, t0", 0x12345678u, 0x00000000u),
          0x0000000Du);
    check("clz rs1=x0", B_REGS("clz t2, x0", 0x00000005u, 0x00000005u),
          0x00000020u);
    check("cpop rd=x0",
          B_REGS("cpop x0, t0\nadd t2, x0, x0", 0xFFFFFFFFu, 0x00000000u),
          0x00000000u);

    /*
     * Zbb: extensions. The interesting bit is the top one of the field,
     * with the bits above it set the other way.
     */
    check("sext.b 0000007f", B1("sext.b", 0x0000007Fu), 0x0000007Fu);
    check("sext.b 00000080", B1("sext.b", 0x00000080u), 0xFFFFFF80u);
    check("sext.b 12345680", B1("sext.b", 0x12345680u), 0xFFFFFF80u);
    check("sext.b ffffff7f", B1("sext.b", 0xFFFFFF7Fu), 0x0000007Fu);
    check("sext.b 00000000", B1("sext.b", 0x00000000u), 0x00000000u);
    check("sext.h 00007fff", B1("sext.h", 0x00007FFFu), 0x00007FFFu);
    check("sext.h 00008000", B1("sext.h", 0x00008000u), 0xFFFF8000u);
    check("sext.h 12348000", B1("sext.h", 0x12348000u), 0xFFFF8000u);
    check("sext.h ffff7fff", B1("sext.h", 0xFFFF7FFFu), 0x00007FFFu);
    check("sext.h 00000080", B1("sext.h", 0x00000080u), 0x00000080u);
    check("zext.h ffffffff", B1("zext.h", 0xFFFFFFFFu), 0x0000FFFFu);
    check("zext.h 12348000", B1("zext.h", 0x12348000u), 0x00008000u);
    check("zext.h ffff0000", B1("zext.h", 0xFFFF0000u), 0x00000000u);
    check("zext.h 0000ffff", B1("zext.h", 0x0000FFFFu), 0x0000FFFFu);
    check("sext.b rd=rs1",
          B_REGS("sext.b t0, t0\nmv t2, t0", 0x12345680u, 0x00000000u),
          0xFFFFFF80u);
    check("zext.h rd=rs1",
          B_REGS("zext.h t0, t0\nmv t2, t0", 0xFFFFFFFFu, 0x00000000u),
          0x0000FFFFu);

    /* Zbb: minimum and maximum, signed and unsigned. */
    check("min 80000000,7fffffff", B2("min", 0x80000000u, 0x7FFFFFFFu),
          0x80000000u);
    check("min 7fffffff,80000000", B2("min", 0x7FFFFFFFu, 0x80000000u),
          0x80000000u);
    check("min ffffffff,00000001", B2("min", 0xFFFFFFFFu, 0x00000001u),
          0xFFFFFFFFu);
    check("min 00000001,ffffffff", B2("min", 0x00000001u, 0xFFFFFFFFu),
          0xFFFFFFFFu);
    check("min 00000005,00000005", B2("min", 0x00000005u, 0x00000005u),
          0x00000005u);
    check("min 00000000,ffffffff", B2("min", 0x00000000u, 0xFFFFFFFFu),
          0xFFFFFFFFu);
    check("max 80000000,7fffffff", B2("max", 0x80000000u, 0x7FFFFFFFu),
          0x7FFFFFFFu);
    check("max 7fffffff,80000000", B2("max", 0x7FFFFFFFu, 0x80000000u),
          0x7FFFFFFFu);
    check("max ffffffff,00000001", B2("max", 0xFFFFFFFFu, 0x00000001u),
          0x00000001u);
    check("max 00000001,ffffffff", B2("max", 0x00000001u, 0xFFFFFFFFu),
          0x00000001u);
    check("max 00000005,00000005", B2("max", 0x00000005u, 0x00000005u),
          0x00000005u);
    check("max 00000000,ffffffff", B2("max", 0x00000000u, 0xFFFFFFFFu),
          0x00000000u);
    check("minu 80000000,7fffffff", B2("minu", 0x80000000u, 0x7FFFFFFFu),
          0x7FFFFFFFu);
    check("minu 7fffffff,80000000", B2("minu", 0x7FFFFFFFu, 0x80000000u),
          0x7FFFFFFFu);
    check("minu ffffffff,00000001", B2("minu", 0xFFFFFFFFu, 0x00000001u),
          0x00000001u);
    check("minu 00000001,ffffffff", B2("minu", 0x00000001u, 0xFFFFFFFFu),
          0x00000001u);
    check("minu 00000005,00000005", B2("minu", 0x00000005u, 0x00000005u),
          0x00000005u);
    check("minu 00000000,ffffffff", B2("minu", 0x00000000u, 0xFFFFFFFFu),
          0x00000000u);
    check("maxu 80000000,7fffffff", B2("maxu", 0x80000000u, 0x7FFFFFFFu),
          0x80000000u);
    check("maxu 7fffffff,80000000", B2("maxu", 0x7FFFFFFFu, 0x80000000u),
          0x80000000u);
    check("maxu ffffffff,00000001", B2("maxu", 0xFFFFFFFFu, 0x00000001u),
          0xFFFFFFFFu);
    check("maxu 00000001,ffffffff", B2("maxu", 0x00000001u, 0xFFFFFFFFu),
          0xFFFFFFFFu);
    check("maxu 00000005,00000005", B2("maxu", 0x00000005u, 0x00000005u),
          0x00000005u);
    check("maxu 00000000,ffffffff", B2("maxu", 0x00000000u, 0xFFFFFFFFu),
          0xFFFFFFFFu);
    check("min rd=rs1",
          B_REGS("min t0, t0, t1\nmv t2, t0", 0x80000000u, 0x7FFFFFFFu),
          0x80000000u);
    check("maxu rd=rs2",
          B_REGS("maxu t1, t0, t1\nmv t2, t1", 0x80000000u, 0x7FFFFFFFu),
          0x80000000u);
    check("max rs1=rs2", B_REGS("max t2, t1, t1", 0x80000000u, 0x7FFFFFFFu),
          0x7FFFFFFFu);
    check("min rd=x0",
          B_REGS("min x0, t0, t1\nadd t2, x0, x0", 0x80000000u, 0x7FFFFFFFu),
          0x00000000u);
    check("minu rs1=x0", B_REGS("minu t2, x0, t1", 0x80000000u, 0x7FFFFFFFu),
          0x00000000u);
    check("maxu rs2=x0", B_REGS("maxu t2, t0, x0", 0x80000000u, 0x7FFFFFFFu),
          0x80000000u);

    /*
     * Zbb: rotations, orc.b and rev8. An amount of 32, 36 or -28 is the
     * same rotation as 0, 4 or 4.
     */
    check("rol 80000001,00000001", B2("rol", 0x80000001u, 0x00000001u),
          0x00000003u);
    check("rol 12345678,00000000", B2("rol", 0x12345678u, 0x00000000u),
          0x12345678u);
    check("rol 12345678,00000020", B2("rol", 0x12345678u, 0x00000020u),
          0x12345678u);
    check("rol 12345678,00000024", B2("rol", 0x12345678u, 0x00000024u),
          0x23456781u);
    check("rol 12345678,0000001f", B2("rol", 0x12345678u, 0x0000001Fu),
          0x091A2B3Cu);
    check("rol 12345678,ffffffe4", B2("rol", 0x12345678u, 0xFFFFFFE4u),
          0x23456781u);
    check("rol ffffffff,00000007", B2("rol", 0xFFFFFFFFu, 0x00000007u),
          0xFFFFFFFFu);
    check("ror 80000001,00000001", B2("ror", 0x80000001u, 0x00000001u),
          0xC0000000u);
    check("ror 12345678,00000000", B2("ror", 0x12345678u, 0x00000000u),
          0x12345678u);
    check("ror 12345678,00000020", B2("ror", 0x12345678u, 0x00000020u),
          0x12345678u);
    check("ror 12345678,00000024", B2("ror", 0x12345678u, 0x00000024u),
          0x81234567u);
    check("ror 12345678,0000001f", B2("ror", 0x12345678u, 0x0000001Fu),
          0x2468ACF0u);
    check("ror 12345678,ffffffe4", B2("ror", 0x12345678u, 0xFFFFFFE4u),
          0x81234567u);
    check("ror ffffffff,00000007", B2("ror", 0xFFFFFFFFu, 0x00000007u),
          0xFFFFFFFFu);
    check("rori 12345678,0", BI("rori", 0x12345678u, 0), 0x12345678u);
    check("rori 12345678,4", BI("rori", 0x12345678u, 4), 0x81234567u);
    check("rori 12345678,31", BI("rori", 0x12345678u, 31), 0x2468ACF0u);
    check("rori 80000001,1", BI("rori", 0x80000001u, 1), 0xC0000000u);
    check("rori 12345678,16", BI("rori", 0x12345678u, 16), 0x56781234u);
    check("rol rd=rs1",
          B_REGS("rol t0, t0, t1\nmv t2, t0", 0x12345678u, 0x00000004u),
          0x23456781u);
    check("ror rd=rs2",
          B_REGS("ror t1, t0, t1\nmv t2, t1", 0x12345678u, 0x00000004u),
          0x81234567u);
    check("rol rs1=rs2", B_REGS("rol t2, t1, t1", 0x00000000u, 0x00000105u),
          0x000020A0u);
    check("rori rd=rs1",
          B_REGS("rori t0, t0, 12\nmv t2, t0", 0x12345678u, 0x00000000u),
          0x67812345u);
    check("orc.b 00000000", B1("orc.b", 0x00000000u), 0x00000000u);
    check("orc.b 00010000", B1("orc.b", 0x00010000u), 0x00FF0000u);
    check("orc.b 80000001", B1("orc.b", 0x80000001u), 0xFF0000FFu);
    check("orc.b 01010101", B1("orc.b", 0x01010101u), 0xFFFFFFFFu);
    check("orc.b 00ff0100", B1("orc.b", 0x00FF0100u), 0x00FFFF00u);
    check("orc.b ffffffff", B1("orc.b", 0xFFFFFFFFu), 0xFFFFFFFFu);
    check("rev8 12345678", B1("rev8", 0x12345678u), 0x78563412u);
    check("rev8 000000ff", B1("rev8", 0x000000FFu), 0xFF000000u);
    check("rev8 ff000000", B1("rev8", 0xFF000000u), 0x000000FFu);
    check("rev8 00ff0000", B1("rev8", 0x00FF0000u), 0x0000FF00u);
    check("orc.b rd=rs1",
          B_REGS("orc.b t0, t0\nmv t2, t0", 0x00010000u, 0x00000000u),
          0x00FF0000u);
    check("rev8 rd=rs1",
          B_REGS("rev8 t0, t0\nmv t2, t0", 0x12345678u, 0x00000000u),
          0x78563412u);

    /*
     * Zbc. All-ones squared is 0x5555_5555_5555_5555, which makes
     * clmulr -- bits 62..31 -- 0xAAAAAAAA and unlike either neighbour.
     */
    check("clmul ffffffff,ffffffff", B2("clmul", 0xFFFFFFFFu, 0xFFFFFFFFu),
          0x55555555u);
    check("clmul 80000000,00000002", B2("clmul", 0x80000000u, 0x00000002u),
          0x00000000u);
    check("clmul 00000003,00000003", B2("clmul", 0x00000003u, 0x00000003u),
          0x00000005u);
    check("clmul 12345678,00000000", B2("clmul", 0x12345678u, 0x00000000u),
          0x00000000u);
    check("clmul 12345678,00000001", B2("clmul", 0x12345678u, 0x00000001u),
          0x12345678u);
    check("clmul 00000001,12345678", B2("clmul", 0x00000001u, 0x12345678u),
          0x12345678u);
    check("clmul 12345678,9abcdef0", B2("clmul", 0x12345678u, 0x9ABCDEF0u),
          0x5CD25A80u);
    check("clmul 80000000,80000000", B2("clmul", 0x80000000u, 0x80000000u),
          0x00000000u);
    check("clmul ffffffff,80000000", B2("clmul", 0xFFFFFFFFu, 0x80000000u),
          0x80000000u);
    check("clmulh ffffffff,ffffffff", B2("clmulh", 0xFFFFFFFFu, 0xFFFFFFFFu),
          0x55555555u);
    check("clmulh 80000000,00000002", B2("clmulh", 0x80000000u, 0x00000002u),
          0x00000001u);
    check("clmulh 00000003,00000003", B2("clmulh", 0x00000003u, 0x00000003u),
          0x00000000u);
    check("clmulh 12345678,00000000", B2("clmulh", 0x12345678u, 0x00000000u),
          0x00000000u);
    check("clmulh 12345678,00000001", B2("clmulh", 0x12345678u, 0x00000001u),
          0x00000000u);
    check("clmulh 00000001,12345678", B2("clmulh", 0x00000001u, 0x12345678u),
          0x00000000u);
    check("clmulh 12345678,9abcdef0", B2("clmulh", 0x12345678u, 0x9ABCDEF0u),
          0x08860E94u);
    check("clmulh 80000000,80000000", B2("clmulh", 0x80000000u, 0x80000000u),
          0x40000000u);
    check("clmulh ffffffff,80000000", B2("clmulh", 0xFFFFFFFFu, 0x80000000u),
          0x7FFFFFFFu);
    check("clmulr ffffffff,ffffffff", B2("clmulr", 0xFFFFFFFFu, 0xFFFFFFFFu),
          0xAAAAAAAAu);
    check("clmulr 80000000,00000002", B2("clmulr", 0x80000000u, 0x00000002u),
          0x00000002u);
    check("clmulr 00000003,00000003", B2("clmulr", 0x00000003u, 0x00000003u),
          0x00000000u);
    check("clmulr 12345678,00000000", B2("clmulr", 0x12345678u, 0x00000000u),
          0x00000000u);
    check("clmulr 12345678,00000001", B2("clmulr", 0x12345678u, 0x00000001u),
          0x00000000u);
    check("clmulr 00000001,12345678", B2("clmulr", 0x00000001u, 0x12345678u),
          0x00000000u);
    check("clmulr 12345678,9abcdef0", B2("clmulr", 0x12345678u, 0x9ABCDEF0u),
          0x110C1D28u);
    check("clmulr 80000000,80000000", B2("clmulr", 0x80000000u, 0x80000000u),
          0x80000000u);
    check("clmulr ffffffff,80000000", B2("clmulr", 0xFFFFFFFFu, 0x80000000u),
          0xFFFFFFFFu);
    check("clmul rd=rs1",
          B_REGS("clmul t0, t0, t1\nmv t2, t0", 0x12345678u, 0x9ABCDEF0u),
          0x5CD25A80u);
    check("clmulh rd=rs2",
          B_REGS("clmulh t1, t0, t1\nmv t2, t1", 0x12345678u, 0x9ABCDEF0u),
          0x08860E94u);
    check("clmulr rs1=rs2",
          B_REGS("clmulr t2, t1, t1", 0x12345678u, 0x9ABCDEF0u), 0x82888AA0u);
    check("clmul rd=x0",
          B_REGS("clmul x0, t0, t1\nadd t2, x0, x0", 0x12345678u, 0x9ABCDEF0u),
          0x00000000u);

    /* Zbs: one bit, indexed by the low five bits of rs2 or by shamt. */
    check("bset 00000000,0000001f", B2("bset", 0x00000000u, 0x0000001Fu),
          0x80000000u);
    check("bset 00000000,00000020", B2("bset", 0x00000000u, 0x00000020u),
          0x00000001u);
    check("bset 00000000,ffffffe4", B2("bset", 0x00000000u, 0xFFFFFFE4u),
          0x00000010u);
    check("bset ffffffff,00000005", B2("bset", 0xFFFFFFFFu, 0x00000005u),
          0xFFFFFFFFu);
    check("bset 80000000,00000000", B2("bset", 0x80000000u, 0x00000000u),
          0x80000001u);
    check("bclr ffffffff,0000001f", B2("bclr", 0xFFFFFFFFu, 0x0000001Fu),
          0x7FFFFFFFu);
    check("bclr ffffffff,00000020", B2("bclr", 0xFFFFFFFFu, 0x00000020u),
          0xFFFFFFFEu);
    check("bclr ffffffff,ffffffe4", B2("bclr", 0xFFFFFFFFu, 0xFFFFFFE4u),
          0xFFFFFFEFu);
    check("bclr 00000000,00000003", B2("bclr", 0x00000000u, 0x00000003u),
          0x00000000u);
    check("bclr 00000001,00000000", B2("bclr", 0x00000001u, 0x00000000u),
          0x00000000u);
    check("binv 00000000,0000001f", B2("binv", 0x00000000u, 0x0000001Fu),
          0x80000000u);
    check("binv ffffffff,00000020", B2("binv", 0xFFFFFFFFu, 0x00000020u),
          0xFFFFFFFEu);
    check("binv ffffffff,0000001f", B2("binv", 0xFFFFFFFFu, 0x0000001Fu),
          0x7FFFFFFFu);
    check("binv 00000000,ffffffe4", B2("binv", 0x00000000u, 0xFFFFFFE4u),
          0x00000010u);
    check("binv 80000000,0000001f", B2("binv", 0x80000000u, 0x0000001Fu),
          0x00000000u);
    check("bext 80000000,0000001f", B2("bext", 0x80000000u, 0x0000001Fu),
          0x00000001u);
    check("bext 80000000,0000003f", B2("bext", 0x80000000u, 0x0000003Fu),
          0x00000001u);
    check("bext 7fffffff,0000001f", B2("bext", 0x7FFFFFFFu, 0x0000001Fu),
          0x00000000u);
    check("bext 00000001,00000020", B2("bext", 0x00000001u, 0x00000020u),
          0x00000001u);
    check("bext fffffffe,00000000", B2("bext", 0xFFFFFFFEu, 0x00000000u),
          0x00000000u);
    check("bext 00010000,00000010", B2("bext", 0x00010000u, 0x00000010u),
          0x00000001u);
    check("bext 00010000,0000000f", B2("bext", 0x00010000u, 0x0000000Fu),
          0x00000000u);
    check("bseti 00000000,31", BI("bseti", 0x00000000u, 31), 0x80000000u);
    check("bseti 00000000,0", BI("bseti", 0x00000000u, 0), 0x00000001u);
    check("bseti ffffffff,7", BI("bseti", 0xFFFFFFFFu, 7), 0xFFFFFFFFu);
    check("bseti 00000000,16", BI("bseti", 0x00000000u, 16), 0x00010000u);
    check("bclri ffffffff,31", BI("bclri", 0xFFFFFFFFu, 31), 0x7FFFFFFFu);
    check("bclri ffffffff,0", BI("bclri", 0xFFFFFFFFu, 0), 0xFFFFFFFEu);
    check("bclri 00000000,7", BI("bclri", 0x00000000u, 7), 0x00000000u);
    check("bclri ffffffff,16", BI("bclri", 0xFFFFFFFFu, 16), 0xFFFEFFFFu);
    check("binvi 80000000,31", BI("binvi", 0x80000000u, 31), 0x00000000u);
    check("binvi 00000000,0", BI("binvi", 0x00000000u, 0), 0x00000001u);
    check("binvi ffffffff,31", BI("binvi", 0xFFFFFFFFu, 31), 0x7FFFFFFFu);
    check("binvi 00000000,16", BI("binvi", 0x00000000u, 16), 0x00010000u);
    check("bexti 80000000,31", BI("bexti", 0x80000000u, 31), 0x00000001u);
    check("bexti 80000000,30", BI("bexti", 0x80000000u, 30), 0x00000000u);
    check("bexti 00000001,0", BI("bexti", 0x00000001u, 0), 0x00000001u);
    check("bexti fffffffe,0", BI("bexti", 0xFFFFFFFEu, 0), 0x00000000u);
    check("bexti 00010000,16", BI("bexti", 0x00010000u, 16), 0x00000001u);
    check("bexti 00010000,17", BI("bexti", 0x00010000u, 17), 0x00000000u);
    check("bset rd=rs2",
          B_REGS("bset t1, t0, t1\nmv t2, t1", 0x00000100u, 0x00000005u),
          0x00000120u);
    check("bclr rd=rs1",
          B_REGS("bclr t0, t0, t1\nmv t2, t0", 0xFFFFFFFFu, 0x00000005u),
          0xFFFFFFDFu);
    check("bext rd=rs2",
          B_REGS("bext t1, t0, t1\nmv t2, t1", 0x00000020u, 0x00000005u),
          0x00000001u);
    check("binv rs1=rs2", B_REGS("binv t2, t1, t1", 0x00000000u, 0x00000005u),
          0x00000025u);
    check("bset rs1=rs2", B_REGS("bset t2, t1, t1", 0x00000000u, 0x00000024u),
          0x00000034u);
    check("bext rs1=rs2", B_REGS("bext t2, t1, t1", 0x00000000u, 0x00000024u),
          0x00000000u);
    check("bseti rd=x0",
          B_REGS("bseti x0, t0, 3\nadd t2, x0, x0", 0x00000000u, 0x00000000u),
          0x00000000u);
    check("bexti rd=rs1",
          B_REGS("bexti t0, t0, 31\nmv t2, t0", 0x80000000u, 0x00000000u),
          0x00000001u);
    check("bset rs1=x0", B_REGS("bset t2, x0, t1", 0x00000007u, 0x00000021u),
          0x00000002u);

    /*
     * A called operation beside a lowered one. The translator hands a
     * few of these -- clmul, cpop, orc.b, min/max, rol/ror -- to a C
     * function that reads and writes the register file directly, where
     * the rest become IR that may keep values in host registers. So what
     * the function reads has to have been stored by then, and what it
     * writes must not be answered from a copy made before the call.
     */
    check("helper reads prior write",
          B_REGS("addi t0, t0, 1\nclmul t2, t0, t1", 0x00000F0Fu, 0x00000033u),
          0x00010230u);
    check("helper result read back",
          B_REGS("li t2, 7\nclmul t2, t0, t1\naddi t2, t2, 1", 0x00000F0Fu,
                 0x00000033u),
          0x00010002u);
    check("helper overwrites operand",
          B_REGS("min t0, t0, t1\nadd t2, t0, t0", 0x00000F0Fu, 0x00000033u),
          0x00000066u);
    check("native after helper",
          B_REGS("cpop t0, t0\nsh2add t2, t0, t1", 0x00000F0Fu, 0x00000033u),
          0x00000053u);
    check("helper after native",
          B_REGS("sh2add t0, t0, t1\ncpop t2, t0", 0x00000F0Fu, 0x00000033u),
          0x0000000Au);
    check("two helpers",
          B_REGS("rol t0, t0, t1\norc.b t2, t0", 0x00000F0Fu, 0x00000033u),
          0xFFFF0000u);

    check("bitmanip-no-trap", g_trap_count - before, 0u);
}

/* ---- Zihintpause, Zihintntl, Zicbop -------------------------------- */

/*
 * Three extensions whose every instruction does nothing, so the only
 * thing there is to get wrong is refusing them.
 *
 * **That is the test: no trap, and the next instruction runs.** Each
 * hint is an encoding of something older -- `pause` a FENCE with an
 * otherwise meaningless predecessor set, the four `ntl.*` an ADD into
 * x0, and the compressed four a C.ADD into x0, which the C extension
 * reserves for exactly this. A decoder that validates FENCE's fields, or
 * that treats rd == x0 in C.ADD as the illegal encoding its neighbours
 * are, raises illegal-instruction on a guest that asked for nothing.
 *
 * The running sum is what says each one was the right *length*: a
 * 16-bit hint read as 32 swallows the addi after it, and the total comes
 * out short rather than anything trapping.
 */
static void test_hints(void)
{
    const uint32_t before = g_trap_count;
    register uint32_t t0_ __asm__("t0") = opaque(0xDEAD0000u);
    uint32_t sum, zero;

    __asm__ volatile(WITH_EXT("zihintpause, +zihintntl",
                              "li   %0, 1\n"
                              "pause\n"
                              "addi %0, %0, 2\n"
                              "ntl.p1\n"
                              "addi %0, %0, 4\n"
                              "ntl.pall\n"
                              "addi %0, %0, 8\n"
                              "ntl.s1\n"
                              "addi %0, %0, 16\n"
                              "ntl.all\n"
                              "addi %0, %0, 32\n"
                              "c.ntl.p1\n"
                              "addi %0, %0, 64\n"
                              "c.ntl.pall\n"
                              "addi %0, %0, 128\n"
                              "c.ntl.s1\n"
                              "addi %0, %0, 256\n"
                              "c.ntl.all\n"
                              "addi %0, %0, 512\n"
                              /*
                               * ntl.all is `add x0, x0, t0`, and t0 is
                               * not zero: x0 must still read as zero.
                               */
                              "add  %1, x0, x0")
                     : "=&r"(sum), "=&r"(zero)
                     : "r"(t0_));

    check("hints-no-trap", g_trap_count - before, 0u);
    check("hints-all-ran", sum, 1023u);

    /*
     * fence.tso, which is not a hint and not optional: the RVI20 profile
     * calls it mandatory in the base ISA and notes it was once described
     * as optional by mistake. A FENCE with fm = 1000; a decoder that
     * insists the reserved-looking top bits are zero traps on it.
     */
    {
        const uint32_t b1 = g_trap_count;

        __asm__ volatile("fence.tso");
        check("fence.tso-no-trap", g_trap_count - b1, 0u);
    }
    check("hints-x0-stays-zero", zero, 0u);

    /*
     * Zicbop: three more, ORIs into x0, and the one thing that earns
     * them lines of their own -- the operand looks like an address and
     * is not one. Nothing is mapped at 0x70000000: a load there is an
     * access fault, which test_traps asserts, and a prefetch there is
     * nothing at all. An implementation that "does" the prefetch by
     * reaching for the memory faults on a guest that asked for a hint.
     */
    {
        const uint32_t b2 = g_trap_count;
        uint32_t n, z;

        __asm__ volatile(WITH_EXT("zicbop", "li   %0, 1\n"
                                            "prefetch.i 0(%2)\n"
                                            "addi %0, %0, 2\n"
                                            "prefetch.r 32(%2)\n"
                                            "addi %0, %0, 4\n"
                                            "prefetch.w -64(%2)\n"
                                            "addi %0, %0, 8\n"
                                            "add  %1, x0, x0")
                         : "=&r"(n), "=&r"(z)
                         : "r"(0x70000000u));
        check("prefetch-no-trap", g_trap_count - b2, 0u);
        check("prefetch-all-ran", n, 15u);
        check("prefetch-x0-stays-zero", z, 0u);
    }
}

/* ---- Zalasr -------------------------------------------------------- */

static volatile uint32_t g_ar[2];

/* A load-acquire into a register that already holds a marker. */
#define LD_AQ(mn, addr)                                                        \
    ({                                                                         \
        uint32_t r_ = 0x5A5A5A5Au;                                             \
        __asm__ volatile(WITH_EXT("zalasr", mn " %0, (%1)")                    \
                         : "+r"(r_)                                            \
                         : "r"((uint32_t)(uintptr_t)(addr))                    \
                         : "memory");                                          \
        r_;                                                                    \
    })

#define ST_RL(mn, addr, val)                                                   \
    __asm__ volatile(WITH_EXT("zalasr", mn " %0, (%1)")                        \
                     ::"r"((uint32_t)(val)),                                   \
                     "r"((uint32_t)(uintptr_t)(addr))                          \
                     : "memory")

/*
 * A raw word in the AMO opcode, with the registers every encoding below
 * names: a0 the destination and holding a marker, a1 a valid address,
 * a2 a value. Returns a0, which an instruction that trapped has not
 * touched.
 */
#define AMO_RAW(word)                                                          \
    ({                                                                         \
        register uint32_t a0_ __asm__("a0") = 0x5A5A5A5Au;                     \
        register uint32_t a1_ __asm__("a1") = (uint32_t)(uintptr_t)g_ar;       \
        register uint32_t a2_ __asm__("a2") = 0x0BADF00Du;                     \
        __asm__ volatile(".word " word                                         \
                         : "+r"(a0_)                                           \
                         : "r"(a1_), "r"(a2_)                                  \
                         : "memory");                                          \
        a0_;                                                                   \
    })

/* One reserved encoding: illegal, reported as itself, and nothing moved. */
static void zalasr_reserved(const char *name, uint32_t got_a0, uint32_t word,
                            uint32_t traps)
{
    check(name, traps, 1u);
    check(name, g_last_cause, 2u);
    check(name, g_last_tval, word);
    check(name, got_a0, 0x5A5A5A5Au);
    check(name, g_ar[0], 0x11223344u);
}

static void test_zalasr(void)
{
    const uint8_t *const base = (const uint8_t *)g_ar;
    uint32_t before = g_trap_count;
    uint32_t got;

    /* Loads sign-extend, as LR and the AMOs do; there is no unsigned form. */
    g_ar[0] = 0x8182F384u;
    check("lb.aq", LD_AQ("lb.aq", base), 0xFFFFFF84u);
    check("lb.aq+1", LD_AQ("lb.aq", base + 1), 0xFFFFFFF3u);
    check("lh.aq", LD_AQ("lh.aq", base), 0xFFFFF384u);
    check("lh.aq+2", LD_AQ("lh.aq", base + 2), 0xFFFF8182u);
    check("lw.aq", LD_AQ("lw.aq", base), 0x8182F384u);
    check("lw.aqrl", LD_AQ("lw.aqrl", base), 0x8182F384u);

    /* Stores write exactly their width. */
    g_ar[0] = 0x11223344u;
    ST_RL("sb.rl", base, 0xAABBCCDDu);
    check("sb.rl", g_ar[0], 0x112233DDu);
    ST_RL("sh.rl", base + 2, 0xAABBCCDDu);
    check("sh.rl", g_ar[0], 0xCCDD33DDu);
    ST_RL("sw.rl", base, 0x55667788u);
    check("sw.rl", g_ar[0], 0x55667788u);
    ST_RL("sw.aqrl", base, 0x99AABBCCu);
    check("sw.aqrl", g_ar[0], 0x99AABBCCu);
    check("zalasr-no-trap", g_trap_count - before, 0u);

    /*
     * A store-release is a store: it ends a reservation on the word it
     * writes. Checked because the instruction is reached through the
     * atomic dispatch rather than the store path, and this file already
     * records one inlined store that had to drop the reservation by
     * hand.
     */
    {
        uint32_t sc = 0u;

        g_ar[0] = 1u;
        __asm__ volatile(WITH_EXT("zalasr", "lr.w  t0, (%1)\n"
                                            "sw.rl %2, (%1)\n"
                                            "sc.w  %0, %3, (%1)")
                         : "=&r"(sc)
                         : "r"(g_ar), "r"(2u), "r"(3u)
                         : "t0", "memory");
        check("sw.rl-ends-reservation", sc != 0u, 1u);
        check("sw.rl-ends-reservation-mem", g_ar[0], 2u);
    }

    /*
     * Misaligned, **and this is not the build option.** RV32_MISALIGNED
     * lets an ordinary load or store complete across a boundary; an
     * atomic never may, because splitting it is exactly what it exists
     * to rule out. So these trap in both configurations, with the
     * misaligned cause and the address -- not an access fault, and not
     * silently done.
     */
    g_ar[0] = 0x11223344u;
    before = g_trap_count;
    got = LD_AQ("lh.aq", base + 1);
    check("lh.aq-misaligned-taken", g_trap_count - before, 1u);
    check("lh.aq-misaligned-cause", g_last_cause, 4u);
    check("lh.aq-misaligned-tval", g_last_tval, (uint32_t)(uintptr_t)base + 1u);
    check("lh.aq-misaligned-rd", got, 0x5A5A5A5Au);

    before = g_trap_count;
    got = LD_AQ("lw.aq", base + 2);
    check("lw.aq-misaligned-taken", g_trap_count - before, 1u);
    check("lw.aq-misaligned-cause", g_last_cause, 4u);
    check("lw.aq-misaligned-rd", got, 0x5A5A5A5Au);

    before = g_trap_count;
    ST_RL("sh.rl", base + 1, 0xAABBCCDDu);
    check("sh.rl-misaligned-taken", g_trap_count - before, 1u);
    check("sh.rl-misaligned-cause", g_last_cause, 6u);
    check("sh.rl-misaligned-tval", g_last_tval, (uint32_t)(uintptr_t)base + 1u);

    before = g_trap_count;
    ST_RL("sw.rl", base + 2, 0xAABBCCDDu);
    check("sw.rl-misaligned-taken", g_trap_count - before, 1u);
    check("sw.rl-misaligned-cause", g_last_cause, 6u);
    check("zalasr-misaligned-mem", g_ar[0], 0x11223344u);
    check("zalasr-misaligned-mem1", g_ar[1], 0u);

    /*
     * The reserved encodings. The ordering bit that gives each
     * instruction its name is mandatory -- there is no load-release and
     * no store-acquire -- and the unused register field must be zero.
     * Each of these differs from a working instruction above by one bit,
     * and "one bit away from implemented" is where this decoder has
     * been wrong before: an implementation that matches on funct5 alone
     * runs all of them.
     *
     *   0x3405a52f  lw.aq a0, (a1)        0x3ac5a02f  sw.rl a2, (a1)
     */
#define RESERVED(name, word)                                                   \
    do {                                                                       \
        g_ar[0] = 0x11223344u; /* each case names only itself */               \
        const uint32_t b_ = g_trap_count;                                      \
        const uint32_t a0_ = AMO_RAW(#word);                                   \
        zalasr_reserved(name, a0_, word##u, g_trap_count - b_);                \
    } while (0)

    RESERVED("zalasr-load-no-aq", 0x3005a52f);
    RESERVED("zalasr-load-rl-only", 0x3205a52f);
    RESERVED("zalasr-load-rs2", 0x3415a52f);
    RESERVED("zalasr-load-64", 0x3405b52f);
    RESERVED("zalasr-store-no-rl", 0x38c5a02f);
    RESERVED("zalasr-store-aq-only", 0x3cc5a02f);
    RESERVED("zalasr-store-rd", 0x3ac5a0af);
    RESERVED("zalasr-store-64", 0x3ac5b02f);

    /*
     * The same shape for the base ISA's loads, stores and JALR, whose
     * reserved funct3 values the JIT's translator never checked: it
     * built an 8-byte load for LD, a LW for LWU, aliased funct3 4-7 of
     * a store onto SB/SH/SW and SD onto an 8-byte store, and jumped on
     * any JALR. fuzz-rv32-jit found the first on its second minute.
     * The interpreter raised illegal-instruction for all of them, and
     * these run in translated blocks wherever the JIT is.
     *
     *   0x0005b503  ld  a0, 0(a1)       0x00c5b023  sd a2, 0(a1)
     *   0x0005e503  lwu a0, 0(a1)       0x00c5c023  funct3 4 store
     *   0x0005f503  funct3 7 load       0x00c5f023  funct3 7 store
     */
    RESERVED("rv32-ld", 0x0005b503);
    RESERVED("rv32-lwu", 0x0005e503);
    RESERVED("rv32-load-f3-7", 0x0005f503);
    RESERVED("rv32-sd", 0x00c5b023);
    RESERVED("rv32-store-f3-4", 0x00c5c023);
    RESERVED("rv32-store-f3-7", 0x00c5f023);
#undef RESERVED

    /*
     * JALR with funct3 1 and 7, a0 the link and a1 the instruction
     * after it: run as a jump, it lands where execution was going
     * anyway, so only a0 -- the return address written over the
     * marker -- and the missing trap can tell.
     */
/*
 * a0 is copied out inside the statement expression, as AMO_RAW does: a
 * register variable is in its register only at the asm, and the first
 * check() call takes a0 for its own argument. The first version read it
 * after the calls and reported the name string's address on both
 * backends.
 */
#define JALR_RAW(word)                                                         \
    ({                                                                         \
        register uint32_t a0_ __asm__("a0") = 0x5A5A5A5Au;                     \
        register uint32_t a1_ __asm__("a1");                                   \
        __asm__ volatile("la   %1, 1f\n"                                       \
                         ".word " #word "\n"                                   \
                         "1:\n"                                                \
                         : "+r"(a0_), "=&r"(a1_)                               \
                         :                                                     \
                         : "memory");                                          \
        a0_;                                                                   \
    })
#define JALR_RESERVED(name, word)                                              \
    do {                                                                       \
        const uint32_t b_ = g_trap_count;                                      \
        const uint32_t a0_ = JALR_RAW(word);                                   \
        check(name, g_trap_count - b_, 1u);                                    \
        check(name, g_last_cause, 2u);                                         \
        check(name, g_last_tval, word##u);                                     \
        check(name, a0_, 0x5A5A5A5Au);                                         \
    } while (0)

    JALR_RESERVED("rv32-jalr-f3-1", 0x00059567);
    JALR_RESERVED("rv32-jalr-f3-7", 0x0005f567);
#undef JALR_RESERVED
#undef JALR_RAW
}

/* ---- Zawrs --------------------------------------------------------- */

#define MIP_MTIP (1u << 7)

/*
 * End any reservation, without changing memory: SC always ends one, and
 * if it happens to succeed it stores what was there.
 */
static void drop_reservation(void)
{
    __asm__ volatile("lw   t1, 0(%0)\n"
                     "sc.w t0, t1, (%0)" ::"r"(&g_atomic)
                     : "t0", "t1", "memory");
}

/* How far ahead the wake-up is set. Long against the handful of
 * instructions between arming it and waiting, so an implementation that
 * does not wait cannot pass by arriving late. */
#define WRS_TICKS 2000u

static void smode_tw_wrs(void)
{
    uint32_t before = g_trap_count;

    /*
     * TW forbids an instruction that may not return in bounded time. A
     * WRS.NTO with nothing reserved returns at once, so it is legal --
     * the bit is about waiting, not about the opcode.
     */
    drop_reservation();
    __asm__ volatile(WITH_EXT("zawrs", "wrs.nto"));
    check("s-tw-wrs.nto-no-resv", g_trap_count - before, 0u);

    /* WRS.STO has a timeout, so it is never the instruction TW means. */
    __asm__ volatile(WITH_EXT("zawrs", "lr.w t0, (%0)\n"
                                       "wrs.sto") ::"r"(&g_atomic)
                     : "t0", "memory");
    check("s-tw-wrs.sto", g_trap_count - before, 0u);

    /* And with something reserved it would wait: illegal, as WFI is. */
    before = g_trap_count;
    __asm__ volatile(WITH_EXT("zawrs", "lr.w t0, (%0)\n"
                                       "wrs.nto") ::"r"(&g_atomic)
                     : "t0", "memory");
    check("s-tw-wrs.nto-traps", g_trap_count - before, 1u);
    check("s-tw-wrs.nto-cause", g_last_cause, 2u);
    check("s-tw-wrs.nto-tval", g_last_tval, 0x00D00073u);

    LEAVE_SMODE();
}

static void test_zawrs(void)
{
    uint32_t before = g_trap_count;
    uint32_t woke, mip;

    csr_write("mie", 0u);
    csr_write("mstatus", 0u);
    disarm_timer();

    /*
     * Nothing reserved, and no interrupt source enabled at all. This is
     * the case that must not wait: a hart that parked here would never
     * be woken, and the run would end without reaching the next line.
     */
    drop_reservation();
    __asm__ volatile(WITH_EXT("zawrs", "wrs.nto"));
    check("wrs.nto-no-resv-returns", g_trap_count - before, 0u);

    /* The short wait returns whether or not anything is reserved. */
    __asm__ volatile(WITH_EXT("zawrs", "wrs.sto"));
    __asm__ volatile(WITH_EXT("zawrs", "lr.w t0, (%0)\n"
                                       "wrs.sto") ::"r"(&g_atomic)
                     : "t0", "memory");
    check("wrs.sto-returns", g_trap_count - before, 0u);

    /*
     * Reserved, and a timer two thousand ticks out. The count is read by
     * the instruction after the wait: it has moved only if the hart sat
     * there until the interrupt arrived, which is the whole instruction.
     */
    drop_reservation();
    before = g_trap_count;
    arm_timer_in(WRS_TICKS);
    csr_set("mie", MIP_MTIP);
    csr_set("mstatus", 1u << 3);
    __asm__ volatile(WITH_EXT("zawrs", "lr.w t0, (%1)\n"
                                       "wrs.nto\n"
                                       "lw   %0, 0(%2)")
                     : "=&r"(woke)
                     : "r"(&g_atomic), "r"(&g_trap_count)
                     : "t0", "memory");
    csr_clear("mstatus", 1u << 3);
    check("wrs.nto-waits-for-irq", woke - before, 1u);
    check("wrs.nto-irq-cause", g_last_cause, 0x80000007u);

    /*
     * The same wait with interrupts globally off, and TW set. It
     * resumes as WFI does -- on an interrupt that is pending and
     * enabled in mie, taken or not -- so it returns with MTIP up and no
     * trap. And M-mode is never the subject of its own control bit.
     */
    drop_reservation();
    before = g_trap_count;
    csr_set("mstatus", MSTATUS_TW);
    arm_timer_in(WRS_TICKS);
    __asm__ volatile(WITH_EXT("zawrs", "lr.w t0, (%1)\n"
                                       "wrs.nto\n"
                                       "csrr %0, mip")
                     : "=&r"(mip)
                     : "r"(&g_atomic)
                     : "t0", "memory");
    disarm_timer();
    csr_write("mie", 0u);
    check("wrs.nto-wakes-masked", mip & MIP_MTIP, MIP_MTIP);
    check("wrs.nto-masked-no-trap", g_trap_count - before, 0u);

    /* Below M, with TW still set. */
    csr_write("medeleg", 0u);
    csr_write("mideleg", 0u);
    enter_smode(smode_tw_wrs);
    csr_clear("mstatus", MSTATUS_TW);
}

/* ---- Zihpm --------------------------------------------------------- */

/*
 * What this implementation's counters count. The architecture leaves the
 * events to the platform and requires only that selector 0 means "no
 * event"; these are this emulator's, from rv_csr.h.
 */
#define HPM_EV_EXCEPTION 1u
#define HPM_EV_INTERRUPT 2u
#define HPM_EV_EXC_CAUSE(c) (0x100u | (c))
#define HPM_EV_IRQ_CAUSE(c) (0x200u | (c))

static void take_breakpoints(unsigned n)
{
    while (n-- != 0u) {
        __asm__ volatile("ebreak");
    }
}

static volatile uint32_t g_hpm_s[4];

/* Which of four counter CSRs an S-mode read of traps on. */
static void smode_hpm(void)
{
    uint32_t before;

    before = g_trap_count;
    (void)csr_read("0xc03"); /* hpmcounter3  */
    g_hpm_s[0] = g_trap_count - before;

    before = g_trap_count;
    (void)csr_read("0xc83"); /* hpmcounter3h */
    g_hpm_s[1] = g_trap_count - before;

    before = g_trap_count;
    (void)csr_read("0xc07"); /* hpmcounter7  */
    g_hpm_s[2] = g_trap_count - before;

    before = g_trap_count;
    (void)csr_read("0xb03"); /* mhpmcounter3: a machine register */
    g_hpm_s[3] = g_trap_count - before;

    LEAVE_SMODE();
}

static uint32_t hpm_s_pattern(void)
{
    return g_hpm_s[0] | (g_hpm_s[1] << 4) | (g_hpm_s[2] << 8) |
           (g_hpm_s[3] << 12);
}

/* A read of a CSR number that must not exist. */
#define CSR_ABSENT(name, num)                                                  \
    do {                                                                       \
        const uint32_t b_ = g_trap_count;                                      \
        (void)csr_read(num);                                                   \
        check(name, g_trap_count - b_, 1u);                                    \
        check(name, g_last_cause, 2u);                                         \
    } while (0)

static void test_zihpm(void)
{
    uint32_t before;

    /*
     * Three hundred checks' worth of traps have been taken by now, and
     * with no event selected none of them was counted. This is the
     * reset state, and it is also what "selector 0 is no event" means.
     */
    check("hpm-event-reset", csr_read("mhpmevent3"), 0u);
    check("hpm-idle-low", csr_read("mhpmcounter3"), 0u);
    check("hpm-idle-high", csr_read("mhpmcounter3h"), 0u);
    check("hpm-idle-last", csr_read("mhpmcounter31"), 0u);

    csr_write("mhpmevent3", HPM_EV_EXCEPTION);
    csr_write("mhpmevent4", HPM_EV_INTERRUPT);
    csr_write("mhpmevent5", HPM_EV_EXC_CAUSE(3u)); /* breakpoint */
    csr_write("mhpmevent6", HPM_EV_IRQ_CAUSE(7u)); /* machine timer */
    csr_write("mhpmevent31", HPM_EV_EXC_CAUSE(2u)); /* illegal */
    check("hpm-event-reads-back", csr_read("mhpmevent5"), 0x103u);

    /*
     * The selector is WARL. Something this core does not count reads
     * back as "no event" -- which is how a guest asks what exists -- and
     * that includes the value one past the last cause, the neighbour of
     * a legal selector.
     */
    csr_write("mhpmevent7", 0x7FFFu);
    check("hpm-event-unknown-is-none", csr_read("mhpmevent7"), 0u);
    csr_write("mhpmevent7", HPM_EV_EXC_CAUSE(16u));
    check("hpm-event-cause16-is-none", csr_read("mhpmevent7"), 0u);
    csr_write("mhpmevent7", HPM_EV_EXC_CAUSE(15u));
    check("hpm-event-cause15-exists", csr_read("mhpmevent7"), 0x10Fu);
    csr_write("mhpmevent7", 0x7FFFu);

    take_breakpoints(3u);
    __asm__ volatile(".word 0xffffffff");
    __asm__ volatile(".word 0xffffffff");

    check("hpm-exc-any", csr_read("mhpmcounter3"), 5u);
    check("hpm-exc-breakpoint", csr_read("mhpmcounter5"), 3u);
    check("hpm-exc-illegal-c31", csr_read("mhpmcounter31"), 2u);
    check("hpm-irq-not-yet", csr_read("mhpmcounter4"), 0u);
    check("hpm-unknown-event", csr_read("mhpmcounter7"), 0u);

    /* One timer interrupt, and only the two interrupt counters move. */
    before = g_trap_count;
    arm_timer_in(20u);
    csr_set("mie", MIP_MTIP);
    csr_set("mstatus", 1u << 3);
    {
        volatile int guard = 0;
        while (g_trap_count == before && guard < 100000) {
            guard++;
        }
    }
    csr_write("mie", 0u);
    csr_write("mstatus", 0u);
    check("hpm-irq-any", csr_read("mhpmcounter4"), 1u);
    check("hpm-irq-timer", csr_read("mhpmcounter6"), 1u);
    check("hpm-exc-unmoved-by-irq", csr_read("mhpmcounter3"), 5u);
    check("hpm-bp-unmoved-by-irq", csr_read("mhpmcounter5"), 3u);

    /* mcountinhibit stops one counter and not its neighbour. */
    csr_write("mcountinhibit", 1u << 3);
    check("hpm-inhibit-reads-back", csr_read("mcountinhibit"), 1u << 3);
    take_breakpoints(1u);
    check("hpm-inhibit-stops", csr_read("mhpmcounter3"), 5u);
    check("hpm-inhibit-per-counter", csr_read("mhpmcounter5"), 4u);
    csr_write("mcountinhibit", 0u);
    take_breakpoints(1u);
    check("hpm-inhibit-released", csr_read("mhpmcounter3"), 6u);

    /* Every counter has an inhibit bit; `time` (bit 1) does not. */
    csr_write("mcountinhibit", 0xFFFFFFFFu);
    check("hpm-inhibit-warl", csr_read("mcountinhibit"), 0xFFFFFFFDu);
    csr_write("mcountinhibit", 0u);

    /*
     * Sixty-four bits in two registers. The carry out of the low half
     * is the only thing here that can be wrong without the low half
     * being wrong, so that is the value to start from.
     */
    csr_write("mhpmcounter3", 0xFFFFFFFFu);
    csr_write("mhpmcounter3h", 7u);
    take_breakpoints(1u);
    check("hpm-carry-low", csr_read("mhpmcounter3"), 0u);
    check("hpm-carry-high", csr_read("mhpmcounter3h"), 8u);

    /* Nothing counts from here: the reads below take traps of their own. */
    csr_write("mhpmevent3", 0u);
    csr_write("mhpmevent4", 0u);
    csr_write("mhpmevent5", 0u);
    csr_write("mhpmevent6", 0u);
    csr_write("mhpmevent7", 0u);
    csr_write("mhpmevent31", 0u);
    take_breakpoints(1u);
    /* Six breakpoints counted: three, then one each for the inhibit, its
     * release and the carry. This seventh is the one that must not be. */
    check("hpm-deselected-stops", csr_read("mhpmcounter5"), 6u);

    /*
     * Each half is written on its own. The other half holds something
     * that is not zero when it is checked: a write that clears its
     * neighbour is the bug, and zero is what that leaves behind.
     */
    csr_write("mhpmcounter3", 0x0BADF00Du);
    csr_write("mhpmcounter3h", 0x12345678u);
    check("hpm-write-high-keeps-low", csr_read("mhpmcounter3"), 0x0BADF00Du);
    csr_write("mhpmcounter3", 0xCAFEF00Du);
    check("hpm-write-low-keeps-high", csr_read("mhpmcounter3h"), 0x12345678u);

    /* The unprivileged shadows read the same counter, and only read. */
    check("hpm-shadow-low", csr_read("0xc03"), 0xCAFEF00Du);
    check("hpm-shadow-high", csr_read("0xc83"), 0x12345678u);
    before = g_trap_count;
    csr_write("0xc03", 0u);
    check("hpm-shadow-readonly", g_trap_count - before, 1u);
    check("hpm-shadow-readonly-cause", g_last_cause, 2u);
    check("hpm-shadow-unwritten", csr_read("mhpmcounter3"), 0xCAFEF00Du);

    /*
     * The edges of the range. Counters 3 to 31 exist in each of five
     * banks, and what sits either side of each bank does not: 1 is the
     * hole `time` leaves in the machine banks, mhpmevent has no 1 or 2,
     * and 32 is past the end.
     */
    check("hpm-last-high", csr_read("mhpmcounter31h"), 0u);
    check("hpm-last-event", csr_read("mhpmevent31"), 0u);
    check("hpm-last-shadow", csr_read("0xc1f"), 2u); /* the two illegals */
    check("hpm-last-shadow-high", csr_read("0xc9f"), 0u);
    CSR_ABSENT("hpm-absent-b01", "0xb01");
    CSR_ABSENT("hpm-absent-b81", "0xb81");
    CSR_ABSENT("hpm-absent-321", "0x321");
    CSR_ABSENT("hpm-absent-322", "0x322");
    CSR_ABSENT("hpm-absent-b20", "0xb20");
    CSR_ABSENT("hpm-absent-ba0", "0xba0");
    CSR_ABSENT("hpm-absent-ca0", "0xca0");

    /*
     * Below M the shadows are gated by mcounteren, one bit per counter
     * *by its number*. Counter 7 is in the pattern for a reason: its
     * low two bits are 3, so a gate that looks at two bits of the CSR
     * number -- which was enough while only cycle, time and instret
     * existed -- opens it with counter 3's bit.
     */
    csr_write("medeleg", 0u);
    csr_write("mcounteren", 0u);
    enter_smode(smode_hpm);
    check("hpm-s-all-gated", hpm_s_pattern(), 0x1111u);

    csr_write("mcounteren", 1u << 3);
    enter_smode(smode_hpm);
    check("hpm-s-counter3-open", hpm_s_pattern(), 0x1100u);

    csr_write("mcounteren", 1u << 7);
    enter_smode(smode_hpm);
    check("hpm-s-counter7-open", hpm_s_pattern(), 0x1011u);

    csr_write("mcounteren", 0xFFFFFFFFu);
    check("hpm-mcounteren-warl", csr_read("mcounteren"), 0xFFFFFFFFu);
    csr_write("mcounteren", 0u);

    csr_write("mhpmcounter3", 0u);
    csr_write("mhpmcounter3h", 0u);
}

/* ---- Sscofpmf: overflow, and counting by mode ------------------------ */

static volatile uint32_t g_sc_s[2];

/* An ebreak in S-mode: a breakpoint *from* S, which M handles. */
static void smode_ebreak(void)
{
    __asm__ volatile("ebreak");
    LEAVE_SMODE();
}

/* What S-mode sees of the overflow machinery, and what it may clear. */
static void smode_sscof(void)
{
    g_sc_s[0] = csr_read("0xda0"); /* scountovf */
    g_sc_s[1] = csr_read("sip");
    __asm__ volatile("csrc sip, %0" ::"r"(1u << 13));
    LEAVE_SMODE();
}

#define LCOF (1u << 13)
#define OF (1u << 31)
#define MINH (1u << 30)
#define SINH (1u << 29)
#define UINH (1u << 28)

static void test_sscofpmf(void)
{
    uint32_t before;

    /*
     * mhpmevent3h: OF and the three inhibits this core has. VSINH and
     * VUINH are the hypervisor's, and EVENT[25:0] -- the selector's top
     * -- is zero for every event the list above names: read-only zero.
     */
    csr_write("0x723", 0xFFFFFFFFu);
    check("sscof-eventh-warl", csr_read("0x723"), OF | MINH | SINH | UINH);
    csr_write("0x723", 0u);
    check("sscof-eventh-clear", csr_read("0x723"), 0u);
    check("sscof-eventh-last", csr_read("0x73f"), 0u);
    CSR_ABSENT("sscof-eventh-absent-722", "0x722");

    /* The overflow interrupt is enabled and pended like any other. */
    csr_write("mie", LCOF);
    check("sscof-mie-lcofie", csr_read("mie"), LCOF);
    csr_write("mie", 0u);
    csr_set("mip", LCOF);
    check("sscof-mip-writable", csr_read("mip") & LCOF, LCOF);
    csr_clear("mip", LCOF);
    check("sscof-mip-clears", csr_read("mip") & LCOF, 0u);

    /*
     * The wrap. Counter 3 counts breakpoints, starts one short of 2^64
     * with OF clear and the interrupt disabled: one ebreak, and it is
     * zero, OF is set and LCOFIP is pending -- and nothing was taken.
     */
    csr_write("mhpmevent3", HPM_EV_EXC_CAUSE(3u));
    csr_write("mhpmcounter3", 0xFFFFFFFFu);
    csr_write("mhpmcounter3h", 0xFFFFFFFFu);
    g_lcof_count = 0u;
    take_breakpoints(1u);
    check("sscof-wrap-low", csr_read("mhpmcounter3"), 0u);
    check("sscof-wrap-high", csr_read("mhpmcounter3h"), 0u);
    check("sscof-wrap-of", csr_read("0x723") & OF, OF);
    check("sscof-wrap-pending", csr_read("mip") & LCOF, LCOF);
    check("sscof-wrap-not-taken", g_lcof_count, 0u);
    check("sscof-scountovf-m", csr_read("0xda0"), 1u << 3);

    /* Enabled, it is taken -- cause 13 -- and the handler clears it. */
    before = g_trap_count;
    csr_write("mie", LCOF);
    csr_set("mstatus", 1u << 3);
    csr_clear("mstatus", 1u << 3);
    csr_write("mie", 0u);
    check("sscof-taken", g_lcof_count, 1u);
    check("sscof-taken-cause", g_last_cause, 0x8000000Du);
    check("sscof-taken-once", g_trap_count - before, 1u);
    check("sscof-cleared", csr_read("mip") & LCOF, 0u);

    /*
     * OF still set: a second wrap is not a second interrupt. That is the
     * protocol -- software clears OF when it is ready for another.
     */
    csr_write("mhpmcounter3", 0xFFFFFFFFu);
    csr_write("mhpmcounter3h", 0xFFFFFFFFu);
    take_breakpoints(1u);
    check("sscof-of-set-no-pend", csr_read("mip") & LCOF, 0u);
    check("sscof-of-set-wrapped", csr_read("mhpmcounter3"), 0u);
    /* Clearing OF re-arms it. */
    csr_write("0x723", 0u);
    csr_write("mhpmcounter3", 0xFFFFFFFFu);
    csr_write("mhpmcounter3h", 0xFFFFFFFFu);
    take_breakpoints(1u);
    check("sscof-rearmed-pends", csr_read("mip") & LCOF, LCOF);
    csr_clear("mip", LCOF);

    /*
     * Counting by mode: MINH stops M's breakpoints, and an ebreak *in S*
     * -- which M handles, so the trap ends in M -- is still S's event and
     * still counts. Then SINH stops that one too.
     */
    csr_write("medeleg", 0u);
    csr_write("mhpmcounter3", 0u);
    csr_write("mhpmcounter3h", 0u);
    csr_write("0x723", MINH);
    take_breakpoints(1u);
    check("sscof-minh-stops-m", csr_read("mhpmcounter3"), 0u);
    enter_smode(smode_ebreak);
    check("sscof-minh-counts-s", csr_read("mhpmcounter3"), 1u);
    csr_write("0x723", MINH | SINH);
    enter_smode(smode_ebreak);
    check("sscof-sinh-stops-s", csr_read("mhpmcounter3"), 1u);
    csr_write("0x723", UINH);
    take_breakpoints(1u);
    check("sscof-uinh-not-m", csr_read("mhpmcounter3"), 2u);

    /*
     * S-mode's view: scountovf shows only what mcounteren grants, and a
     * delegated LCOFIP is in sip, where S may clear it.
     */
    csr_write("0x723", OF);
    csr_write("0x727", OF);
    check("sscof-scountovf-two", csr_read("0xda0"), (1u << 3) | (1u << 7));
    csr_write("mcounteren", 1u << 7);
    csr_write("mideleg", LCOF);
    csr_set("mip", LCOF);
    enter_smode(smode_sscof);
    check("sscof-scountovf-s-gated", g_sc_s[0], 1u << 7);
    check("sscof-sip-shows", g_sc_s[1] & LCOF, LCOF);
    check("sscof-sip-cleared", csr_read("mip") & LCOF, 0u);
    /* Not delegated, S sees nothing in sip and its write is dropped. */
    csr_write("mideleg", 0u);
    csr_set("mip", LCOF);
    enter_smode(smode_sscof);
    check("sscof-sip-hidden", g_sc_s[1] & LCOF, 0u);
    check("sscof-sip-not-cleared", csr_read("mip") & LCOF, LCOF);
    csr_clear("mip", LCOF);

    csr_write("mcounteren", 0u);
    csr_write("0x723", 0u);
    csr_write("0x727", 0u);
    csr_write("mhpmevent3", 0u);
    csr_write("mhpmcounter3", 0u);
    csr_write("mhpmcounter3h", 0u);
}

/* ------------------------------------------------------------------ */
/* What fifteen arch-test suites nobody had named found                */
/* ------------------------------------------------------------------ */

/*
 * riscv-arch-test builds a suite only if its directory is named, and
 * fifteen that this core is eligible for never had been. Offered, they
 * were 99 tests and 13 failures. The architecture suite checks all of
 * what follows now; it is repeated here because this file is the one
 * that runs on a board, under the Thumb-2 backend, and because one of
 * the thirteen was a defect in the *translator* that only a guest
 * executing under it can hold.
 */

/* ---- a read nothing reads ------------------------------------------ */

/*
 * A load whose result is thrown away is still a load.
 *
 * The JIT's dead-value pass deletes an operation nothing reads, and its
 * list of operations with an effect of their own did not include LOAD.
 * So a load whose destination was overwritten later in the same block
 * -- with no store, load, call or branch in between to make the first
 * write observable -- simply was not executed. It did not fault. And it
 * did not read: on a device, where reading is how a flag is cleared and
 * `(void)UART->DR; return 0;` is how a driver says so, the access never
 * reached the register.
 *
 * **Each of these overwrites its own destination on the next line**,
 * which is the whole condition. The same load followed by anything that
 * touches memory is kept, and that is why 378 architecture tests, this
 * file and every guest in the tree ran through the JIT without it
 * showing: a compiler rarely leaves a dead load, and a hand-written
 * test usually looks at what it read.
 */
static void test_discarded_reads(void)
{
    uint32_t before = g_trap_count;

    __asm__ volatile("lw   t0, 0(%0)\n"
                     "li   t0, 0" ::"r"(0x70000000u)
                     : "t0", "memory");
    check("discarded-load-faults", g_trap_count - before, 1u);
    check("discarded-load-cause", g_last_cause, 5u);
    check("discarded-load-tval", g_last_tval, 0x70000000u);

#if defined(__riscv_flen) && (__riscv_flen >= 32)
    /*
     * The same pass, the same omission, a different effect: a
     * floating-point comparison or conversion raises flags, and the
     * flags are the only reason some of them are executed at all. `flt`
     * signals on any NaN; a conversion of NaN to an integer is invalid.
     */
    csr_set("mstatus", 1u << 13);
    csr_write("fflags", 0u);
    __asm__ volatile("fmv.w.x ft0, %0\n"
                     "fmv.w.x ft1, %1\n"
                     "flt.s   t0, ft0, ft1\n"
                     "li      t0, 0" ::"r"(F_QNAN),
                     "r"(F1_0)
                     : "t0", "ft0", "ft1");
    check("discarded-flt-raises-nv", csr_read("fflags") & 0x10u, 0x10u);

    csr_write("fflags", 0u);
    __asm__ volatile("fmv.w.x   ft0, %0\n"
                     "fcvt.w.s  t0, ft0, rtz\n"
                     "li        t0, 0" ::"r"(F_QNAN)
                     : "t0", "ft0");
    check("discarded-fcvt-raises-nv", csr_read("fflags") & 0x10u, 0x10u);
    csr_write("fflags", 0u);
#endif
}

/* ---- WARL fields wider than the machine ---------------------------- */

static void test_warl_widths(void)
{
    /*
     * pmpaddr holds bits 33:2 of a physical address and this machine
     * has 32 of them. The top two bits of the register name memory that
     * cannot exist, so they read as zero -- the same rule, and the same
     * mistake, as satp.PPN.
     */
#if RV32_EXT_PMP
    csr_write("pmpaddr10", 0xFFFFFFFFu);
    check("pmpaddr-30-bits", csr_read("pmpaddr10"), 0x3FFFFFFFu);
    csr_write("pmpaddr10", 0u);
#endif

    /* menvcfg and senvcfg: the three cache-block fields and nothing
     * else, and CBIE cannot hold its reserved value. */
    csr_write("menvcfg", 0xFFFFFFFFu);
    check("menvcfg-warl", csr_read("menvcfg"), 0xF0u);
    csr_write("menvcfg", 0x20u);
    check("menvcfg-cbie-reserved", csr_read("menvcfg"), 0u);
    csr_write("senvcfg", 0xFFFFFFFFu);
    check("senvcfg-warl", csr_read("senvcfg"), 0xF0u);
    csr_write("senvcfg", 0u);
    csr_write("menvcfg", 0u);
}

/* ---- the cache-block operations, below M --------------------------- */

static volatile uint32_t g_cbo_s[2];

/*
 * All four, from S-mode. g_cbo_s[0] gets a bit for each that trapped --
 * zero, clean, flush, inval, from bit 0 -- and g_cbo_s[1] is non-zero if
 * any of those traps was something other than illegal-instruction.
 */
static void smode_cbo(void)
{
    uint32_t pat = 0u, odd = 0u, before;

#define CBO_TRY(bit, insn)                                                     \
    before = g_trap_count;                                                     \
    __asm__ volatile(insn " (%0)" ::"r"(g_block) : "memory");                  \
    if (g_trap_count != before) {                                              \
        pat |= (bit);                                                          \
        odd |= g_last_cause ^ 2u;                                              \
    }

    CBO_TRY(1u, "cbo.zero")
    CBO_TRY(2u, "cbo.clean")
    CBO_TRY(4u, "cbo.flush")
    CBO_TRY(8u, "cbo.inval")
#undef CBO_TRY

    g_cbo_s[0] = pat;
    g_cbo_s[1] = odd;
    LEAVE_SMODE();
}

static uint32_t cbo_from_smode(uint32_t menvcfg)
{
    csr_write("menvcfg", menvcfg);
    g_block[0] = 0x11111111u;
    enter_smode(smode_cbo);
    return g_cbo_s[0] | ((g_cbo_s[1] != 0u) ? 0x80000000u : 0u);
}

static volatile uint32_t g_cbo_paged[5];

/*
 * A page nothing maps. Not VA_HOLE: smode_paging makes that one valid
 * on its way through, to show that SFENCE.VMA is what publishes a PTE,
 * and it stays that way.
 */
#define VA_NEVER 0x90008000u

/* Under Sv32: a mapped page, a read-only one, and one that is not there. */
static void smode_cbo_paged(void)
{
    uint32_t before = g_trap_count;

    __asm__ volatile("cbo.clean (%0)" ::"r"(VA_RW) : "memory");
    g_cbo_paged[0] = g_trap_count - before;

    /* Readable is enough: the rule is "a load *or* a store would be". */
    before = g_trap_count;
    __asm__ volatile("cbo.inval (%0)" ::"r"(VA_RO) : "memory");
    g_cbo_paged[1] = g_trap_count - before;

    before = g_trap_count;
    __asm__ volatile("cbo.flush (%0)" ::"r"(VA_NEVER + 0x48u) : "memory");
    g_cbo_paged[2] = g_trap_count - before;
    g_cbo_paged[3] = g_last_cause;
    g_cbo_paged[4] = g_last_tval;

    LEAVE_SMODE();
}

static void test_cbo_below_m(void)
{
    csr_write("medeleg", 0u);

    /*
     * menvcfg decides what S-mode may run, one enable per kind, and for
     * as long as the register has existed nothing read it: every
     * operation ran at every privilege. The pattern has a bit for each
     * that was *refused*.
     */
    check("s-cbo-none-enabled", cbo_from_smode(0x00u), 0xFu);
    check("s-cbo-refused-untouched", g_block[0], 0x11111111u);
    check("s-cbo-zero-only", cbo_from_smode(0x80u), 0xEu);
    check("s-cbo-zero-zeroed", g_block[0], 0u);
    check("s-cbo-clean-flush-only", cbo_from_smode(0x40u), 0x9u);
    check("s-cbo-inval-as-flush", cbo_from_smode(0x10u), 0x7u);
    check("s-cbo-inval", cbo_from_smode(0x30u), 0x7u);
    check("s-cbo-all", cbo_from_smode(0xF0u), 0x0u);

    /*
     * A block that lies only *partly* in a PMP entry. The rule is that
     * the entry holding any byte of an access must hold all of it, and
     * the check found an entry by the access's first byte -- so an
     * access that began below an entry and ran into it was handed on to
     * the next one, here the background that permits everything. Four
     * bytes against four-byte entries cannot do that. A block can:
     * entry 9 is one word in the middle of it, with no permissions at
     * all.
     */
#if RV32_EXT_PMP
    csr_write("pmpaddr9", ((uint32_t)(uintptr_t)&g_block[4]) >> 2);
    csr_write("pmpcfg2", 0x1000u); /* entry 9: NA4, ---, unlocked */
    check("s-cbo-straddle-cfg", (csr_read("pmpcfg2") >> 8) & 0xFFu, 0x10u);
    check("s-cbo-straddle-refused", cbo_from_smode(0xF0u), 0x8000000Fu);
    /* cbo.zero is one store of the block. It was a store per word, so
     * the four words below the denied one were zeroed first. */
    check("s-cbo-straddle-nothing-zeroed", g_block[0], 0x11111111u);
    /* All four as a store access fault: 7, which is 2 with 5 xored in. */
    check("s-cbo-straddle-cause", g_cbo_s[1], 5u);
    csr_write("pmpcfg2", 0u);
    csr_write("pmpaddr9", 0u);
    check("s-cbo-straddle-cleared", cbo_from_smode(0xF0u), 0x0u);
#endif

    /*
     * And under paging the block is named by a *virtual* address. The
     * maintenance operations took it to the bus as a physical one: a
     * mapped page faulted, because nothing lives at 0x90000000.
     */
    csr_write("satp", SATP_SV32 | ((uint32_t)(uintptr_t)g_root >> 12));
    enter_smode(smode_cbo_paged);
    csr_write("satp", 0u);
    csr_write("menvcfg", 0u);

    check("s-cbo-paged-mapped", g_cbo_paged[0], 0u);
    check("s-cbo-paged-readonly", g_cbo_paged[1], 0u);
    check("s-cbo-paged-hole-taken", g_cbo_paged[2], 1u);
    /* A store page fault, though nothing was stored: the architecture
     * reports every refusal of these as the store kind. */
    check("s-cbo-paged-hole-cause", g_cbo_paged[3], 15u);
    check("s-cbo-paged-hole-tval", g_cbo_paged[4], VA_NEVER + 0x40u);
}

/* ---- a store-conditional that could never have stored -------------- */

static void test_sc_nowhere(void)
{
    uint32_t rd = 0x5A5A5A5Au;
    const uint32_t before = g_trap_count;

    /*
     * No reservation is held, so this SC fails -- and it used to say
     * only that. But nothing is mapped at the address, and whether a
     * store *could* land is asked before whether it will: an access
     * fault, exactly as a plain store there reports.
     */
    drop_reservation();
    __asm__ volatile("sc.w %0, %1, (%2)"
                     : "+r"(rd)
                     : "r"(opaque(0u)), "r"(0x70000000u)
                     : "memory");
    check("sc-nowhere-taken", g_trap_count - before, 1u);
    check("sc-nowhere-cause", g_last_cause, 7u);
    check("sc-nowhere-tval", g_last_tval, 0x70000000u);
    check("sc-nowhere-rd", rd, 0x5A5A5A5Au);
}

/* ---- Zfhmin: the double conversions --------------------------------- */

#if defined(__riscv_flen) && (__riscv_flen >= 32)
/*
 * FCVT.D.H and FCVT.H.D, which no architecture test covers: ZfhminD is
 * the same six instructions as Zfhmin, run with FLEN 64, and holds
 * neither. So these are the only check on them.
 *
 * The expectations are IEEE 754's, worked by hand and held to Python's
 * binary16 (struct format 'e') where it has an answer -- not read off
 * the emulator, which is how a test comes to encode its bug. Each
 * boundary is the awkward input: the tie at 65520, which overflows to
 * nearest and does *not* toward zero; a value below half the smallest
 * subnormal; a signalling NaN; a register that is not boxed.
 *
 * fa0 is loaded with the 64 bits in (hi, lo) and fa1 stored back, so a
 * result's box is visible in the high word.
 */
static volatile uint32_t g_hd[2] __attribute__((aligned(8)));

#define HD_RUN(op, hi, lo)                                                     \
    do {                                                                       \
        g_hd[0] = (lo);                                                        \
        g_hd[1] = (hi);                                                        \
        csr_write("fflags", 0u);                                               \
        __asm__ volatile(WITH_EXT("d, +zfhmin", "fld fa0, 0(%0)\n" op "\n"     \
                                                "fsd fa1, 0(%0)")              \
                         ::"r"(g_hd)                                           \
                         : "fa0", "fa1", "memory");                            \
    } while (0)

/*
 * A block that only moves a float, and leaves by a constant exit, marks
 * FS dirty.
 *
 * Both JIT backends decided whether a block may chain before lowering
 * it, and learned it wrote an f register only while lowering the FPUT.
 * So `flw` then `j` chained, the chained exit skipped the tail call that
 * marks FS dirty, and FS stayed Clean with f-state changed -- the state
 * a context switch reads to decide whether to save the registers.
 * fuzz-rv32-jit found it on x86-64; this is for the board, which no host
 * suite reaches. FS is set to Clean first, so Initial and Dirty differ
 * from the starting value and from each other.
 */
static void test_fs_dirty_chained(void)
{
    /* const: in .rodata, not .data -- see test_stack_depth's check. */
    static const float one = 1.0f;
    const uint32_t fs = 3u << 13;

    csr_clear("mstatus", fs);
    csr_set("mstatus", 2u << 13); /* Clean */
    __asm__ volatile("flw  ft0, 0(%0)\n"
                     "j    1f\n"
                     "1:\n" ::"r"(&one)
                     : "ft0", "memory");
    check("fs-dirty-flw-chained", (csr_read("mstatus") & fs) >> 13, 3u);
}

static void test_zfhmin(void)
{
    uint32_t before;

    csr_set("mstatus", 1u << 13);

    /* Widening is exact: 1.5, and the smallest subnormal half, 2^-24. */
    HD_RUN("fcvt.d.h fa1, fa0", 0xFFFFFFFFu, 0xFFFF3E00u);
    check("fcvt.d.h-1.5-hi", g_hd[1], 0x3FF80000u);
    check("fcvt.d.h-1.5-lo", g_hd[0], 0u);
    check("fcvt.d.h-exact", csr_read("fflags"), 0u);
    HD_RUN("fcvt.d.h fa1, fa0", 0xFFFFFFFFu, 0xFFFF0001u);
    check("fcvt.d.h-subnormal", g_hd[1], 0x3E700000u);
    HD_RUN("fcvt.d.h fa1, fa0", 0xFFFFFFFFu, 0xFFFF8000u);
    check("fcvt.d.h-negzero", g_hd[1], 0x80000000u);

    /* A signalling NaN: the canonical double NaN, and invalid. */
    HD_RUN("fcvt.d.h fa1, fa0", 0xFFFFFFFFu, 0xFFFF7C01u);
    check("fcvt.d.h-snan", g_hd[1], 0x7FF80000u);
    check("fcvt.d.h-snan-lo", g_hd[0], 0u);
    check("fcvt.d.h-snan-nv", csr_read("fflags"), 0x10u);

    /* A double's bits are not a boxed half: the canonical half NaN,
     * which is quiet, so no flag. */
    HD_RUN("fcvt.d.h fa1, fa0", 0x3FF00000u, 0x00003E00u);
    check("fcvt.d.h-unboxed", g_hd[1], 0x7FF80000u);
    check("fcvt.d.h-unboxed-quiet", csr_read("fflags"), 0u);

    /* 1/3 narrowed three ways, inexact each time, and boxed. */
    HD_RUN("fcvt.h.d fa1, fa0, rne", 0x3FD55555u, 0x55555555u);
    check("fcvt.h.d-third-rne", g_hd[0], 0xFFFF3555u);
    check("fcvt.h.d-third-box", g_hd[1], 0xFFFFFFFFu);
    check("fcvt.h.d-third-nx", csr_read("fflags"), 0x01u);
    HD_RUN("fcvt.h.d fa1, fa0, rup", 0x3FD55555u, 0x55555555u);
    check("fcvt.h.d-third-rup", g_hd[0], 0xFFFF3556u);
    HD_RUN("fcvt.h.d fa1, fa0, rtz", 0x3FD55555u, 0x55555555u);
    check("fcvt.h.d-third-rtz", g_hd[0], 0xFFFF3555u);

    /* 70000 is past the largest half: infinity to nearest, the largest
     * finite toward zero, overflow and inexact either way. */
    HD_RUN("fcvt.h.d fa1, fa0, rne", 0x40F11700u, 0u);
    check("fcvt.h.d-big-rne", g_hd[0], 0xFFFF7C00u);
    check("fcvt.h.d-big-rne-of", csr_read("fflags"), 0x05u);
    HD_RUN("fcvt.h.d fa1, fa0, rtz", 0x40F11700u, 0u);
    check("fcvt.h.d-big-rtz", g_hd[0], 0xFFFF7BFFu);
    check("fcvt.h.d-big-rtz-of", csr_read("fflags"), 0x05u);

    /* 65520 ties 65504 and 2^16. Even is 2^16, which overflows; toward
     * zero the unbounded result is 65504, which does not -- inexact
     * alone. */
    HD_RUN("fcvt.h.d fa1, fa0, rne", 0x40EFFE00u, 0u);
    check("fcvt.h.d-tie-rne", g_hd[0], 0xFFFF7C00u);
    check("fcvt.h.d-tie-rne-of", csr_read("fflags"), 0x05u);
    HD_RUN("fcvt.h.d fa1, fa0, rtz", 0x40EFFE00u, 0u);
    check("fcvt.h.d-tie-rtz", g_hd[0], 0xFFFF7BFFu);
    check("fcvt.h.d-tie-rtz-nx", csr_read("fflags"), 0x01u);

    /* 2^-30, under half the smallest subnormal: zero to nearest, the
     * smallest subnormal upward; underflow and inexact. */
    HD_RUN("fcvt.h.d fa1, fa0, rne", 0x3E100000u, 0u);
    check("fcvt.h.d-tiny-rne", g_hd[0], 0xFFFF0000u);
    check("fcvt.h.d-tiny-uf", csr_read("fflags"), 0x03u);
    HD_RUN("fcvt.h.d fa1, fa0, rup", 0x3E100000u, 0u);
    check("fcvt.h.d-tiny-rup", g_hd[0], 0xFFFF0001u);

    /* A signalling NaN double: the canonical half NaN, invalid. */
    HD_RUN("fcvt.h.d fa1, fa0, rne", 0x7FF00000u, 1u);
    check("fcvt.h.d-snan", g_hd[0], 0xFFFF7E00u);
    check("fcvt.h.d-snan-nv", csr_read("fflags"), 0x10u);

    /* FMV.X.H sign-extends the raw bits; FMV.H.X boxes them. */
    {
        uint32_t r;

        __asm__ volatile(WITH_EXT("d, +zfhmin", "fmv.h.x fa0, %1\n"
                                                "fmv.x.h %0, fa0\n"
                                                "fsd fa0, 0(%2)")
                         : "=&r"(r)
                         : "r"(0x1234BC00u), "r"(g_hd)
                         : "fa0", "memory");
        check("fmv.x.h-sext", r, 0xFFFFBC00u);
        check("fmv.h.x-box-lo", g_hd[0], 0xFFFFBC00u);
        check("fmv.h.x-box-hi", g_hd[1], 0xFFFFFFFFu);
    }

    /* A reserved rm is illegal, even where the conversion is exact. */
    before = g_trap_count;
    __asm__ volatile(".word 0x422555d3"); /* fcvt.d.h f11, f10, rm=5 */
    check("fcvt.d.h-rm5-taken", g_trap_count - before, 1u);
    check("fcvt.d.h-rm5-cause", g_last_cause, 2u);
    before = g_trap_count;
    __asm__ volatile(".word 0x441555d3"); /* fcvt.h.d f11, f10, rm=5 */
    check("fcvt.h.d-rm5-taken", g_trap_count - before, 1u);
}
#endif

/* ---- eight bytes are aligned to eight ------------------------------ */

#if defined(__riscv_flen) && (__riscv_flen >= 32)
static volatile uint32_t g_dbl[4] __attribute__((aligned(8)));

/*
 * FLD and FSD at an address that is 4 mod 8.
 *
 * The emulator carries a double out as two word accesses, and each of
 * those is aligned -- so a core configured to *report* misaligned
 * accesses completed this one. As with the word forms above, what is
 * right depends on the build, and both answers are asserted.
 */
static void test_double_alignment(void)
{
    uint32_t before;

    csr_set("mstatus", 1u << 13);
    g_dbl[0] = 0x11111111u;
    g_dbl[1] = 0x22222222u;
    g_dbl[2] = 0x33333333u;
    g_dbl[3] = 0x44444444u;

    before = g_trap_count;
    __asm__ volatile(WITH_EXT("d", "fmv.w.x ft0, %1\n"
                                   "fsd     ft0, 4(%0)") ::"r"(g_dbl),
                     "r"(0xCAFEF00Du)
                     : "ft0", "memory");
#if RV32_MISALIGNED
    check("fsd-4mod8-completes", g_trap_count - before, 0u);
    check("fsd-4mod8-low", g_dbl[1], 0xCAFEF00Du);
    check("fsd-4mod8-high", g_dbl[2], 0xFFFFFFFFu); /* a boxed single */
#else
    check("fsd-4mod8-taken", g_trap_count - before, 1u);
    check("fsd-4mod8-cause", g_last_cause, 6u);
    check("fsd-4mod8-tval", g_last_tval, (uint32_t)(uintptr_t)&g_dbl[1]);
    check("fsd-4mod8-untouched", g_dbl[1], 0x22222222u);
#endif

    before = g_trap_count;
    __asm__ volatile(WITH_EXT("d", "fld ft0, 4(%0)") ::"r"(g_dbl)
                     : "ft0", "memory");
#if RV32_MISALIGNED
    check("fld-4mod8-completes", g_trap_count - before, 0u);
#else
    check("fld-4mod8-taken", g_trap_count - before, 1u);
    check("fld-4mod8-cause", g_last_cause, 4u);
    check("fld-4mod8-tval", g_last_tval, (uint32_t)(uintptr_t)&g_dbl[1]);
#endif

    /* An aligned one is untouched by any of this. */
    before = g_trap_count;
    __asm__ volatile(WITH_EXT("d", "fsd ft0, 8(%0)") ::"r"(g_dbl)
                     : "memory");
    check("fsd-aligned", g_trap_count - before, 0u);
}
#endif

/* ---- an access in two parts ---------------------------------------- */

/*
 * Two virtually adjacent pages over physical memory that is not: A maps
 * the *third* page of g_page and B, the page after it, maps the *first*.
 * C, after B, maps nothing.
 *
 * A misaligned access that runs from A into B is two pieces of physical
 * memory with a different translation each. The emulator translated its
 * first byte and then took four *physically* contiguous bytes -- so it
 * read the tail of the third page and whatever followed it in memory,
 * not the first page, and it did so without complaint when the second
 * virtual page was not mapped at all. RV32_MISALIGNED is on by default.
 */
#define VA_SPLIT_A 0x90010000u
#define VA_SPLIT_B 0x90011000u
#define VA_SPLIT_C 0x90012000u

static volatile uint32_t g_split[20];

static void smode_split(void)
{
#if RV32_MISALIGNED
    volatile uint32_t *const a_tail =
        (volatile uint32_t *)(page_pa(PAGE_EXEC2) + 0xFFCu);
    volatile uint32_t *const b_head = (volatile uint32_t *)page_pa(PAGE_DATA);
    uint32_t before, v;

    /* Read across: two bytes from each side. */
    __asm__ volatile("lw %0, 0(%1)" : "=&r"(v) : "r"(VA_SPLIT_B - 2u));
    g_split[0] = v;
    __asm__ volatile("lh %0, 0(%1)" : "=&r"(v) : "r"(VA_SPLIT_B - 1u));
    g_split[1] = v;

    /* Write across: three bytes on one side and one on the other. */
    __asm__ volatile("sw %0, 0(%1)" ::"r"(0xA1B2C3D4u),
                     "r"(VA_SPLIT_B - 3u)
                     : "memory");
    g_split[2] = *a_tail;
    g_split[3] = *b_head;

    /*
     * Into the page that is not there. A fault, naming *that* page --
     * the first half is mapped, so a handler told the first address
     * would find nothing to fix and return to the same fault -- and
     * with the half that could have been written left alone.
     */
    before = g_trap_count;
    __asm__ volatile("sw %0, 0(%1)" ::"r"(0xDEADBEEFu),
                     "r"(VA_SPLIT_C - 2u)
                     : "memory");
    g_split[4] = g_trap_count - before;
    g_split[5] = g_last_cause;
    g_split[6] = g_last_tval;

    before = g_trap_count;
    v = 0x5A5A5A5Au;
    __asm__ volatile("lw %0, 0(%1)" : "+r"(v) : "r"(VA_SPLIT_C - 1u));
    g_split[7] = g_trap_count - before;
    g_split[8] = g_last_cause;
    g_split[9] = g_last_tval;
    g_split[10] = v;

#if defined(__riscv_flen) && (__riscv_flen >= 32)
    /* A double whose two words are in two pages... */
    __asm__ volatile(WITH_EXT("d", "fmv.w.x ft0, %1\n"
                                   "fsd     ft0, 0(%0)") ::"r"(VA_SPLIT_B - 4u),
                     "r"(0x0D0E0F00u)
                     : "ft0", "memory");
    g_split[11] = *a_tail;
    g_split[12] = *b_head;

    /* ...and one whose second word has nowhere to go. */
    before = g_trap_count;
    __asm__ volatile(WITH_EXT("d", "fsd ft0, 0(%0)") ::"r"(VA_SPLIT_C - 4u)
                     : "memory");
    g_split[13] = g_trap_count - before;
    g_split[14] = g_last_cause;
    g_split[15] = g_last_tval;
#endif
#endif /* RV32_MISALIGNED */
    LEAVE_SMODE();
}

#if defined(__riscv_flen) && (__riscv_flen >= 32) && RV32_EXT_PMP
/* An aligned double, with PMP over its upper word only. */
static void smode_fsd_pmp(void)
{
    const uint32_t before = g_trap_count;

    __asm__ volatile(WITH_EXT("d", "fmv.w.x ft0, %1\n"
                                   "fsd     ft0, 0(%0)") ::"r"(g_dbl),
                     "r"(0xCAFEF00Du)
                     : "ft0", "memory");
    g_split[16] = g_trap_count - before;
    g_split[17] = g_last_cause;
    g_split[18] = g_last_tval;
    LEAVE_SMODE();
}
#endif

static void test_split_access(void)
{
    volatile uint32_t *const a_tail =
        (volatile uint32_t *)(page_pa(PAGE_EXEC2) + 0xFFCu);
    volatile uint32_t *const b_head = (volatile uint32_t *)page_pa(PAGE_DATA);
    volatile uint32_t *const b_tail =
        (volatile uint32_t *)(page_pa(PAGE_DATA) + 0xFFCu);

    csr_write("medeleg", 0u);
    csr_set("mstatus", 1u << 13); /* FS on: the doubles below */

    *a_tail = 0x44332211u;
    *b_head = 0x88776655u;
    *b_tail = 0x0F0E0D0Cu;
    g_leaf[VPN0(VA_SPLIT_A)] = leaf_pte(
        page_pa(PAGE_EXEC2), PTE_V | PTE_R | PTE_W | PTE_A | PTE_D);
    g_leaf[VPN0(VA_SPLIT_B)] =
        leaf_pte(page_pa(PAGE_DATA), PTE_V | PTE_R | PTE_W | PTE_A | PTE_D);
    g_leaf[VPN0(VA_SPLIT_C)] = 0u;

    csr_write("satp", SATP_SV32 | ((uint32_t)(uintptr_t)g_root >> 12));
    __asm__ volatile("sfence.vma" ::: "memory");
    enter_smode(smode_split);
    csr_write("satp", 0u);

#if RV32_MISALIGNED
    check("split-lw", g_split[0], 0x66554433u);
    check("split-lh", g_split[1], 0x5544u);
    check("split-sw-first-page", g_split[2], 0xB2C3D411u);
    check("split-sw-second-page", g_split[3], 0x887766A1u);

    check("split-sw-hole-taken", g_split[4], 1u);
    check("split-sw-hole-cause", g_split[5], 15u);
    check("split-sw-hole-tval", g_split[6], VA_SPLIT_C);
    check("split-lw-hole-taken", g_split[7], 1u);
    check("split-lw-hole-cause", g_split[8], 13u);
    check("split-lw-hole-tval", g_split[9], VA_SPLIT_C);
    check("split-lw-hole-rd", g_split[10], 0x5A5A5A5Au);
#if defined(__riscv_flen) && (__riscv_flen >= 32)
    check("split-fsd-low", g_split[11], 0x0D0E0F00u);
    check("split-fsd-high", g_split[12], 0xFFFFFFFFu); /* a boxed single */
    check("split-fsd-hole-taken", g_split[13], 1u);
    check("split-fsd-hole-cause", g_split[14], 15u);
    check("split-fsd-hole-tval", g_split[15], VA_SPLIT_C);
#endif
    /* Neither refused store left its first half behind. */
    check("split-hole-nothing-written", *b_tail, 0x0F0E0D0Cu);
#endif

#if defined(__riscv_flen) && (__riscv_flen >= 32) && RV32_EXT_PMP
    /*
     * The same question without paging, and in every build: an aligned
     * double with PMP over its upper word. FSD was two word stores, each
     * checked alone, so the lower word was written and the fault named
     * the upper one. Eight bytes are one access: PMP has to hold all of
     * them in one entry, the fault is at the double, and nothing moves.
     */
    g_dbl[0] = 0x11111111u;
    g_dbl[1] = 0x22222222u;
    csr_write("pmpaddr9", ((uint32_t)(uintptr_t)&g_dbl[1]) >> 2);
    csr_write("pmpcfg2", 0x1000u); /* entry 9: NA4, ---, unlocked */
    enter_smode(smode_fsd_pmp);
    csr_write("pmpcfg2", 0u);
    csr_write("pmpaddr9", 0u);
    check("fsd-pmp-upper-taken", g_split[16], 1u);
    check("fsd-pmp-upper-cause", g_split[17], 7u);
    check("fsd-pmp-upper-tval", g_split[18], (uint32_t)(uintptr_t)&g_dbl[0]);
    check("fsd-pmp-upper-low-untouched", g_dbl[0], 0x11111111u);
    check("fsd-pmp-upper-high-untouched", g_dbl[1], 0x22222222u);
#endif
}

/* ------------------------------------------------------------------ */
/* The data TLB                                                        */
/* ------------------------------------------------------------------ */

/*
 * Below M the emulator caches, per page and per privilege, whether a load
 * or store is a plain host access -- the translation, PMP and the region
 * answered once for the whole page (include/emu/emu_dtlb.h). Any access
 * that succeeds the slow way may fill an entry, on either backend.
 *
 * So every case here first *makes* an entry and then changes exactly one
 * thing it depends on, and asks again. The Sv32 checks above change the
 * thing first and ask once, which is right for what they test and cannot
 * see a stale entry: SUM, for one, is tested there in the order -- denied,
 * then granted -- that passes whether or not a granted answer is kept.
 */
#define VA_XO 0x90006000u /* execute-only: readable through MXR alone */
#define USTUB_OFF 0x100u  /* the U-mode stub in EXEC1, clear of the     */
                          /* execute test's two words                   */

/*
 * A second address space: g_root, but with the 4 MiB holding g_page in
 * 4 KiB pages, page 0 replaced by page 2 and EXEC1 a user page. The
 * U-mode stub runs from it at its physical address because the trap
 * handler reads a faulting instruction from M-mode, untranslated: from a
 * virtual address it takes a second fault of its own. Its two tables are
 * g_root2 and g_leaf2, in g_pt with the others.
 */

/*
 * A word of the image, which is ROM: loads may be cached, stores never.
 *
 * The image's *first* word, because an entry is only ever made for a page
 * that is entirely one thing, and the first page is: all of it is image,
 * and no PMP entry lies across it. A lone constant lands in the last
 * page, which is partly outside the ROM window -- and with one there, a
 * TLB that let ROM be written passed. A page-aligned array of its own
 * would also do, at up to 12 KB of padding in an image a board embeds.
 */
extern const uint32_t _start[]; /* a word array: as a char[] it is not
                                   known to be aligned, and the store
                                   below became four byte stores */
#define ROM_WORD (*(volatile uint32_t *)(uintptr_t)_start)

static volatile uint32_t g_dtlb[24];

/*
 * The one load the MPRV case runs, out of line so that every call enters
 * the same translated block: lw a0, 0(a0) ; ret.
 */
__asm__(".pushsection .text\n"
        ".balign 4\n"
        "dtlb_mprv_load:\n"
        "    lw   a0, 0(a0)\n"
        "    ret\n"
        ".popsection\n");
extern const char dtlb_mprv_load[];

/* M-mode, loading `addr` as MPRV makes it: at `mpp`, translated. */
static uint32_t mprv_load(uint32_t addr, uint32_t mpp)
{
    uint32_t v;

    __asm__ volatile("csrc mstatus, %[mask]\n"
                     "csrs mstatus, %[on]\n"
                     "mv   a0, %[addr]\n"
                     "jalr ra, 0(%[fn])\n"
                     "csrc mstatus, %[mprv]\n"
                     "mv   %[out], a0\n"
                     : [out] "=r"(v)
                     : [mask] "r"(MSTATUS_MPP_MASK),
                       [on] "r"(MSTATUS_MPRV | mpp), [mprv] "r"(MSTATUS_MPRV),
                       [addr] "r"(addr), [fn] "r"(dtlb_mprv_load)
                     : "a0", "ra", "memory");
    return v;
}

static void smode_dtlb(void)
{
    const uint32_t p = page_pa(PAGE_DATA);
    uint32_t before, v, sc;

    /* An SFENCE.VMA naming the page, after it was used. */
    g_dtlb[0] = *(volatile uint32_t *)VA_RW;
    g_leaf[VPN0(VA_RW)] = leaf_pte(page_pa(PAGE_EXEC2),
                                   PTE_V | PTE_R | PTE_W | PTE_A | PTE_D);
    __asm__ volatile("sfence.vma %0, zero" ::"r"(VA_RW) : "memory");
    g_dtlb[1] = *(volatile uint32_t *)VA_RW;
    g_leaf[VPN0(VA_RW)] =
        leaf_pte(p, PTE_V | PTE_R | PTE_W | PTE_A | PTE_D);
    __asm__ volatile("sfence.vma %0, zero" ::"r"(VA_RW) : "memory");

    /* SUM granted, the page read, SUM taken away. */
    csr_set("sstatus", MSTATUS_SUM);
    g_dtlb[2] = *(volatile uint32_t *)VA_USER;
    csr_clear("sstatus", MSTATUS_SUM);
    before = g_trap_count;
    (void)*(volatile uint32_t *)VA_USER;
    g_dtlb[3] = g_trap_count - before;
    g_dtlb[4] = g_last_cause;

    /* MXR, the same shape. */
    csr_set("sstatus", MSTATUS_MXR);
    g_dtlb[5] = *(volatile uint32_t *)VA_XO;
    csr_clear("sstatus", MSTATUS_MXR);
    before = g_trap_count;
    (void)*(volatile uint32_t *)VA_XO;
    g_dtlb[6] = g_trap_count - before;
    g_dtlb[7] = g_last_cause;

    /* Another address space, under another ASID: page 0 is page 2 there. */
    g_dtlb[8] = *(volatile uint32_t *)p;
    csr_write("satp", SATP_SV32 | (1u << 22) |
                          ((uint32_t)(uintptr_t)g_root2 >> 12));
    g_dtlb[9] = *(volatile uint32_t *)p;
    csr_write("satp", SATP_SV32 | ((uint32_t)(uintptr_t)g_root >> 12));

    /* ROM: read, so there is an entry, and then written. */
    g_dtlb[10] = ROM_WORD;
    before = g_trap_count;
    ROM_WORD = ~g_dtlb[10];
    g_dtlb[11] = g_trap_count - before;
    g_dtlb[12] = g_last_cause;
    g_dtlb[13] = ROM_WORD ^ g_dtlb[10]; /* zero if nothing moved */

    /*
     * A plain store between LR and SC, to a page with an entry -- page 0
     * through VA_RW, not a global: the page holding the globals has a
     * locked PMP entry in it, from test_pmp, and so never gets one.
     */
    *(volatile uint32_t *)(VA_RW + 0x40u) = 7u;
    __asm__ volatile("lr.w %0, (%2)\n\t"
                     "sw %3, 0(%2)\n\t"
                     "sc.w %1, %4, (%2)"
                     : "=&r"(v), "=&r"(sc)
                     : "r"(VA_RW + 0x40u), "r"(9u), "r"(42u)
                     : "memory");
    g_dtlb[14] = sc;
    g_dtlb[15] = *(volatile uint32_t *)(VA_RW + 0x40u);

    /*
     * Misaligned, out of a page with an entry and into the next. An
     * entry vouches for aligned accesses only; taken for this one, it
     * reads the next *physical* page, which is page 1, not page 0.
     */
    (void)*(volatile uint32_t *)VA_SPLIT_A;
    before = g_trap_count;
    v = 0x5A5A5A5Au;
    __asm__ volatile("lw %0, 0(%1)" : "+r"(v) : "r"(VA_SPLIT_B - 2u));
    g_dtlb[16] = v;
    g_dtlb[17] = g_trap_count - before;

    LEAVE_SMODE();
}

static void test_dtlb(void)
{
    const uint32_t p = page_pa(PAGE_DATA);
    const uint32_t rwad = PTE_V | PTE_R | PTE_W | PTE_A | PTE_D;
    const char *const ustub = (const char *)(page_pa(PAGE_EXEC1) + USTUB_OFF);
    uint32_t before;

    csr_write("medeleg", 0u);
    csr_clear("mstatus", MSTATUS_SUM | MSTATUS_MXR | MSTATUS_MPRV);

    g_page[0] = 0xD0D0D0D0u;
    g_page[2048] = 0x2222D2D2u; /* the execute test is done with it */
    g_page[0x3FF] = 0x0403E1E1u; /* page 0's last word */
    g_page[1024] = 0x0C0B1111u;  /* page 1's first */

    g_leaf[VPN0(VA_RW)] = leaf_pte(p, rwad);
    g_leaf[VPN0(VA_USER)] = leaf_pte(p, rwad | PTE_U);
    g_leaf[VPN0(VA_XO)] = leaf_pte(p, PTE_V | PTE_X | PTE_A);
    g_leaf[VPN0(VA_SPLIT_A)] = leaf_pte(p, rwad);
    g_leaf[VPN0(VA_SPLIT_B)] = leaf_pte(page_pa(PAGE_EXEC2), rwad);

    /* below_m_load, for U-mode: lw t0, 0(a0) ; ecall. */
    g_page[1024 + USTUB_OFF / 4u] = 0x00052283u;
    g_page[1024 + USTUB_OFF / 4u + 1u] = 0x00000073u;
    __asm__ volatile("fence.i" ::: "memory");

    for (uint32_t i = 0; i < 1024u; i++) {
        g_root2[i] = g_root[i];
        g_leaf2[i] = leaf_pte((p & ~0x3FFFFFu) + (i << 12),
                              rwad | PTE_X);
    }
    g_leaf2[VPN0(p)] = leaf_pte(page_pa(PAGE_EXEC2), rwad);
    g_leaf2[VPN0(page_pa(PAGE_EXEC1))] =
        leaf_pte(page_pa(PAGE_EXEC1), PTE_V | PTE_R | PTE_X | PTE_U | PTE_A);
    g_root2[VPN1(p)] = leaf_pte((uint32_t)(uintptr_t)g_leaf2, PTE_V);

    csr_write("satp", SATP_SV32 | ((uint32_t)(uintptr_t)g_root >> 12));
    __asm__ volatile("sfence.vma" ::: "memory");
    enter_smode(smode_dtlb);

    check("dtlb-fill", g_dtlb[0], 0xD0D0D0D0u);
    check("dtlb-sfence-page", g_dtlb[1], 0x2222D2D2u);
    check("dtlb-sum-load", g_dtlb[2], 0xD0D0D0D0u);
    check("dtlb-nosum-traps", g_dtlb[3], 1u);
    check("dtlb-nosum-cause", g_dtlb[4], 13u);
    check("dtlb-mxr-load", g_dtlb[5], 0xD0D0D0D0u);
    check("dtlb-nomxr-traps", g_dtlb[6], 1u);
    check("dtlb-nomxr-cause", g_dtlb[7], 13u);
    check("dtlb-asid0", g_dtlb[8], 0xD0D0D0D0u);
    check("dtlb-asid1", g_dtlb[9], 0x2222D2D2u);
    check("dtlb-rom-load", g_dtlb[10], _start[0]);
    check("dtlb-rom-store-traps", g_dtlb[11], 1u);
    check("dtlb-rom-store-cause", g_dtlb[12], 7u);
    check("dtlb-rom-unchanged", g_dtlb[13], 0u);
    check("dtlb-sc-broken", g_dtlb[14] != 0u, 1u);
    check("dtlb-sc-nostore", g_dtlb[15], 9u);
#if RV32_MISALIGNED
    /* Page 0's last two bytes, then page 2's first two -- not page 1's. */
    check("dtlb-split-value", g_dtlb[16], 0xD2D20403u);
    check("dtlb-split-notrap", g_dtlb[17], 0u);
#else
    check("dtlb-split-traps", g_dtlb[17], 1u);
#endif

    /*
     * One table per privilege. S fills its entry for an S page, and U
     * must not find it; U fills its entry for a U page, and S without
     * SUM must not find that. Each run ends in an ecall, which is a trap
     * too: a fault makes it two, and the fault's cause is the previous.
     * In the second address space, where the U stub can run.
     */
    csr_write("satp", SATP_SV32 | (1u << 22) |
                          ((uint32_t)(uintptr_t)g_root2 >> 12));
    (void)run_below_m(below_m_load, 1u, VA_RW);
    before = g_trap_count;
    (void)run_below_m(ustub, 0u, VA_RW);
    check("dtlb-u-after-s-traps", g_trap_count - before, 2u);
    check("dtlb-u-after-s-cause", g_prev_cause, 13u);

    (void)run_below_m(ustub, 0u, VA_USER);
    before = g_trap_count;
    (void)run_below_m(below_m_load, 1u, VA_USER);
    check("dtlb-s-after-u-traps", g_trap_count - before, 2u);
    check("dtlb-s-after-u-cause", g_prev_cause, 13u);

#if RV32_EXT_PMP
    /*
     * PMP taking the page away after S used it, through entry 10, by each
     * of the two writes that can: the config, with the address already
     * in place, and then the address, with the config already on. Doing
     * both at once would let either flush stand in for the other.
     */
    csr_write("pmpaddr10", (p >> 2) | 0x1FFu); /* NAPOT 4 KiB, still OFF */
    (void)run_below_m(below_m_load, 1u, VA_RW);
    csr_write("pmpcfg2", 0x180000u); /* NAPOT, --- */
    before = g_trap_count;
    (void)run_below_m(below_m_load, 1u, VA_RW);
    check("dtlb-pmpcfg-traps", g_trap_count - before, 2u);
    check("dtlb-pmpcfg-cause", g_prev_cause, 5u);

    csr_write("pmpaddr10", (page_pa(PAGE_EXEC2) >> 2) | 0x1FFu);
    before = g_trap_count;
    (void)run_below_m(below_m_load, 1u, VA_RW);
    check("dtlb-pmpaddr-away-ok", g_trap_count - before, 1u);
    csr_write("pmpaddr10", (p >> 2) | 0x1FFu);
    before = g_trap_count;
    (void)run_below_m(below_m_load, 1u, VA_RW);
    check("dtlb-pmpaddr-traps", g_trap_count - before, 2u);
    check("dtlb-pmpaddr-cause", g_prev_cause, 5u);

    /* ...and over four bytes of it: the rest is allowed, those are not. */
    csr_write("pmpcfg2", 0u);
    csr_write("pmpaddr10", (p + 0x800u) >> 2);
    csr_write("pmpcfg2", 0x100000u); /* NA4, --- */
    before = g_trap_count;
    (void)run_below_m(below_m_load, 1u, VA_RW);
    check("dtlb-pmp-part-ok", g_trap_count - before, 1u);
    before = g_trap_count;
    (void)run_below_m(below_m_load, 1u, VA_RW + 0x800u);
    check("dtlb-pmp-part-traps", g_trap_count - before, 2u);
    check("dtlb-pmp-part-cause", g_prev_cause, 5u);
    csr_write("pmpcfg2", 0u);
    csr_write("pmpaddr10", 0u);
#endif

    /*
     * MPRV, in M-mode: the same load, made at S and then at U. A
     * translated block names the table it probes, so the one built for
     * S must not be the one run for U -- which the interpreter, asking
     * the privilege at every access, cannot get wrong.
     */
    for (uint32_t i = 0; i < 8u; i++) {
        (void)mprv_load(VA_RW, MSTATUS_MPP_S);
    }
    check("dtlb-mprv-s", mprv_load(VA_RW, MSTATUS_MPP_S), 0xD0D0D0D0u);
    before = g_trap_count;
    (void)mprv_load(VA_RW, 0u);
    check("dtlb-mprv-u-traps", g_trap_count - before, 1u);
    check("dtlb-mprv-u-cause", g_last_cause, 13u);

    csr_write("satp", 0u);
}

/* ------------------------------------------------------------------ */
/* SFENCE.VMA and a megapage                                           */
/* ------------------------------------------------------------------ */

/*
 * A megapage is cached a 4 KiB page at a time -- in the TLB, in the data
 * TLB, as translated blocks -- and an SFENCE.VMA naming one address must
 * reach every cached entry holding that *leaf*, which is all of them.
 * So: use one piece of a megapage as data and as code, take the
 * megapage away, fence an address in a different piece, and both uses
 * must fault. Fencing only the named 4 KiB left the other piece readable
 * and its block runnable.
 */
#define VA_MEGA 0xA0000000u /* 4 MiB aligned, and nothing of its own */
#define MEGA_STUB_OFF 0x200u /* in page 2, clear of its first word   */

static volatile uint32_t g_mega[12];

/* The megapage holding g_page, mapped at VA_MEGA. */
static uint32_t mega_base(void)
{
    return page_pa(PAGE_DATA) & ~0x003FFFFFu;
}

static uint32_t mega_va(uint32_t pa)
{
    return VA_MEGA + (pa - mega_base());
}

static void smode_mega(void)
{
    volatile uint32_t *const named =
        (volatile uint32_t *)mega_va(page_pa(PAGE_DATA));
    volatile uint32_t *const piece =
        (volatile uint32_t *)mega_va(page_pa(PAGE_EXEC2));
    const uint32_t stub_va = mega_va(page_pa(PAGE_EXEC2) + MEGA_STUB_OFF);
    uint32_t (*const stub)(void) = (uint32_t (*)(void))stub_va;
    uint32_t before;

    /* One piece, as data and as code; called twice so it is translated. */
    g_mega[0] = *piece;
    g_mega[1] = stub();
    g_mega[2] = stub();

    /* The megapage goes, and the fence names a different piece. */
    g_root[VPN1(VA_MEGA)] = 0u;
    __asm__ volatile("sfence.vma %0, zero" ::"r"(named) : "memory");

    before = g_trap_count;
    (void)*piece;
    g_mega[3] = g_trap_count - before;
    g_mega[4] = g_last_cause;

    before = g_trap_count;
    call_may_fault(stub_va);
    g_mega[5] = g_trap_count - before;
    g_mega[6] = g_last_cause;
    g_mega[7] = g_last_tval;

    LEAVE_SMODE();
}

static void test_mega_sfence(void)
{
    const uint32_t saved = g_root[VPN1(VA_MEGA)];
    volatile uint32_t *const stub_pa =
        (volatile uint32_t *)(page_pa(PAGE_EXEC2) + MEGA_STUB_OFF);

    csr_write("medeleg", 0u);
    g_page[2048] = 0x3333C3C3u;
    stub_pa[0] = 0x33300513u; /* li a0, 0x333 */
    stub_pa[1] = 0x00008067u; /* ret          */
    __asm__ volatile("fence.i" ::: "memory");

    g_root[VPN1(VA_MEGA)] =
        leaf_pte(mega_base(),
                 PTE_V | PTE_R | PTE_W | PTE_X | PTE_A | PTE_D);
    csr_write("satp", SATP_SV32 | ((uint32_t)(uintptr_t)g_root >> 12));
    __asm__ volatile("sfence.vma" ::: "memory");
    enter_smode(smode_mega);
    csr_write("satp", 0u);
    g_root[VPN1(VA_MEGA)] = saved;

    check("mega-piece-load", g_mega[0], 0x3333C3C3u);
    check("mega-piece-runs", g_mega[1], 0x333u);
    check("mega-piece-runs-again", g_mega[2], 0x333u);
    check("mega-sfence-load-traps", g_mega[3], 1u);
    check("mega-sfence-load-cause", g_mega[4], 13u);
    check("mega-sfence-fetch-traps", g_mega[5], 1u);
    check("mega-sfence-fetch-cause", g_mega[6], 12u);
    check("mega-sfence-fetch-tval", g_mega[7],
          mega_va(page_pa(PAGE_EXEC2) + MEGA_STUB_OFF));
}

/* ------------------------------------------------------------------ */
/* The fetch table                                                     */
/* ------------------------------------------------------------------ */

/*
 * The interpreter, and the JIT's translator, fetch below M through a
 * table that caches per page whether an instruction may be fetched there
 * (rv_itlb_fill). Each case runs code first, so its page is in the
 * table, then changes one thing the entry depends on, and runs it again.
 *
 * The stubs are `li a0, imm ; ret`, at addresses that are their own
 * physical ones wherever the trap handler has to read them back.
 */
#define VA_FX 0x90008000u /* a 4 KiB page of code, remapped        */
#define VA_SA 0x90009000u /* two pages of code, apart in memory    */
#define FX_STUB 0x400u
#define FT_A 0x300u /* two stubs in page 2, for PMP over part of it */
#define FT_B 0x900u

static volatile uint32_t g_ft[16];

static void put_stub(uint32_t pa, uint32_t imm)
{
    volatile uint32_t *const w = (volatile uint32_t *)pa;

    w[0] = (imm << 20) | 0x00000513u; /* li a0, imm */
    w[1] = 0x00008067u;               /* ret        */
}

static uint32_t call_stub_at(uint32_t va)
{
    return ((uint32_t (*)(void))va)();
}

static void smode_ft_remap(void)
{
    g_ft[0] = call_stub_at(VA_FX + FX_STUB);
    g_ft[1] = call_stub_at(VA_FX + FX_STUB);
    g_leaf[VPN0(VA_FX)] =
        leaf_pte(page_pa(PAGE_EXEC2), PTE_V | PTE_R | PTE_X | PTE_A);
    __asm__ volatile("sfence.vma %0, zero" ::"r"(VA_FX) : "memory");
    g_ft[2] = call_stub_at(VA_FX + FX_STUB);
    LEAVE_SMODE();
}

static void smode_ft_run(void)
{
    g_ft[3] = call_stub_at(page_pa(PAGE_EXEC2) + FT_A);
    g_ft[4] = call_stub_at(page_pa(PAGE_EXEC2) + FT_A);
    LEAVE_SMODE();
}

static void smode_ft_denied(void)
{
    const uint32_t before = g_trap_count;

    call_may_fault(page_pa(PAGE_EXEC2) + FT_A);
    g_ft[5] = g_trap_count - before;
    g_ft[6] = g_last_cause;
    LEAVE_SMODE();
}

static void smode_ft_partial(void)
{
    g_ft[7] = call_stub_at(page_pa(PAGE_EXEC2) + FT_A);
    const uint32_t before = g_trap_count;

    call_may_fault(page_pa(PAGE_EXEC2) + FT_B);
    g_ft[8] = g_trap_count - before;
    g_ft[9] = g_last_cause;
    LEAVE_SMODE();
}

static void smode_ft_trigger(void)
{
    const uint32_t before = g_trap_count;

    (void)call_stub_at(page_pa(PAGE_EXEC2) + FT_A);
    g_ft[10] = g_trap_count - before;
    g_ft[11] = g_last_cause;
    LEAVE_SMODE();
}

static void smode_ft_straddle(void)
{
    g_ft[12] = call_stub_at(VA_SA + 0x500u); /* the first page, cached */
    g_ft[13] = call_stub_at(VA_SA + 0xFFEu);
    LEAVE_SMODE();
}

static volatile uint32_t g_ft2[3];

/* The same, with PMP refusing to execute the page the second half is in. */
static void smode_ft_straddle_denied(void)
{
    (void)call_stub_at(VA_SA + 0x500u);
    const uint32_t before = g_trap_count;

    call_may_fault(VA_SA + 0xFFEu);
    g_ft2[0] = g_trap_count - before;
    g_ft2[1] = g_last_cause;
    g_ft2[2] = g_last_tval;
    LEAVE_SMODE();
}

static void smode_ft_user_page(void)
{
    const uint32_t before = g_trap_count;

    call_may_fault(page_pa(PAGE_EXEC1) + USTUB_OFF);
    g_ft[14] = g_trap_count - before;
    g_ft[15] = g_last_cause;
    LEAVE_SMODE();
}

static void test_fetch_table(void)
{
    const uint32_t root =
        SATP_SV32 | ((uint32_t)(uintptr_t)g_root >> 12);
    const uint32_t root2 =
        SATP_SV32 | (1u << 22) | ((uint32_t)(uintptr_t)g_root2 >> 12);

    csr_write("medeleg", 0u);
    put_stub(page_pa(PAGE_EXEC1) + FX_STUB, 0x611u);
    put_stub(page_pa(PAGE_EXEC2) + FX_STUB, 0x622u);
    put_stub(page_pa(PAGE_EXEC2) + FT_A, 0x5A1u);
    put_stub(page_pa(PAGE_EXEC2) + FT_B, 0x5B2u);
    put_stub(page_pa(PAGE_EXEC2) + 0x500u, 0x001u);
    /*
     * `li a0, 0x744` across VA_SA's two pages: its low half is the last
     * of page 2, its high half and the `ret` after it the first of page
     * 0. Physically page 2 is followed by page 3, the zero sentinel, so
     * reading the instruction contiguously gives `li a0, 0`.
     */
    g_page[2048 + 1023] = (g_page[2048 + 1023] & 0x0000FFFFu) | 0x05130000u;
    g_page[0] = 0x80677440u; /* hi half of li ; lo half of ret */
    g_page[1] = (g_page[1] & 0xFFFF0000u) | 0x00000000u;
    __asm__ volatile("fence.i" ::: "memory");

    g_leaf[VPN0(VA_FX)] =
        leaf_pte(page_pa(PAGE_EXEC1), PTE_V | PTE_R | PTE_X | PTE_A);
    g_leaf[VPN0(VA_SA)] =
        leaf_pte(page_pa(PAGE_EXEC2), PTE_V | PTE_R | PTE_X | PTE_A);
    g_leaf[VPN0(VA_SA + 0x1000u)] =
        leaf_pte(page_pa(PAGE_DATA), PTE_V | PTE_R | PTE_X | PTE_A);
    csr_write("satp", root);
    __asm__ volatile("sfence.vma" ::: "memory");

    /* An SFENCE.VMA naming the page, after its code was fetched. */
    enter_smode(smode_ft_remap);
    check("ft-first", g_ft[0], 0x611u);
    check("ft-first-again", g_ft[1], 0x611u);
    check("ft-sfence-page", g_ft[2], 0x622u);

    /* A 32-bit instruction across two pages that are not adjacent. */
    enter_smode(smode_ft_straddle);
    check("ft-straddle-first-page", g_ft[12], 0x001u);
    check("ft-straddle", g_ft[13], 0x744u);

#if RV32_EXT_PMP
    /*
     * The table holds the first page, so its PMP answer is known; the
     * second half is in another page, whose is not. Entry 10 over page 0
     * with R and W: the fetch must fault at the second half.
     */
    csr_write("pmpaddr10", (page_pa(PAGE_DATA) >> 2) | 0x1FFu);
    csr_write("pmpcfg2", 0x1B0000u); /* NAPOT, RW- */
    enter_smode(smode_ft_straddle_denied);
    csr_write("pmpcfg2", 0u);
    csr_write("pmpaddr10", 0u);
    check("ft-straddle-pmp-traps", g_ft2[0], 1u);
    check("ft-straddle-pmp-cause", g_ft2[1], 1u);
    check("ft-straddle-pmp-tval", g_ft2[2], VA_SA + 0x1000u);
#endif

#if RV32_EXT_PMP
    /* PMP taking X away from a page already run: entry 10, R and W. */
    enter_smode(smode_ft_run);
    check("ft-pmp-before", g_ft[3], 0x5A1u);
    check("ft-pmp-before-again", g_ft[4], 0x5A1u);
    csr_write("pmpaddr10", (page_pa(PAGE_EXEC2) >> 2) | 0x1FFu);
    csr_write("pmpcfg2", 0x1B0000u); /* NAPOT, RW- */
    enter_smode(smode_ft_denied);
    check("ft-pmp-denied-traps", g_ft[5], 1u);
    check("ft-pmp-denied-cause", g_ft[6], 1u);

    /* ...and over four bytes of it: B is denied, A next to it is not. */
    csr_write("pmpcfg2", 0u);
    csr_write("pmpaddr10", (page_pa(PAGE_EXEC2) + FT_B) >> 2);
    csr_write("pmpcfg2", 0x100000u); /* NA4, --- */
    enter_smode(smode_ft_partial);
    check("ft-pmp-part-ok", g_ft[7], 0x5A1u);
    check("ft-pmp-part-traps", g_ft[8], 1u);
    check("ft-pmp-part-cause", g_ft[9], 1u);
    csr_write("pmpcfg2", 0u);
    csr_write("pmpaddr10", 0u);
#endif

    /* An execute trigger on code the table already holds. */
    enter_smode(smode_ft_run);
    csr_write("tselect", 0u);
    csr_write("tdata2", page_pa(PAGE_EXEC2) + FT_A);
    csr_write("tdata1", 0x20000014u); /* mcontrol: S, execute */
    enter_smode(smode_ft_trigger);
    csr_write("tdata1", 0u);
    check("ft-trigger-traps", g_ft[10], 1u);
    check("ft-trigger-cause", g_ft[11], 3u);

    /*
     * One table per privilege: U runs its stub, from a user page of the
     * second address space, and S must not find that entry for the same
     * page -- S never executes a user page.
     */
    csr_write("satp", root2);
    __asm__ volatile("sfence.vma" ::: "memory");
    (void)run_below_m((const char *)(page_pa(PAGE_EXEC1) + USTUB_OFF), 0u,
                      VA_USER);
    enter_smode(smode_ft_user_page);
    check("ft-user-page-traps", g_ft[14], 1u);
    check("ft-user-page-cause", g_ft[15], 12u);

    csr_write("satp", 0u);
}

/* ------------------------------------------------------------------ */

/*
 * How deep the stack went, measured rather than assumed.
 *
 * The emulator hands the guest an sp at the top of the RAM it really
 * has, and nothing between that and _end is checked by anyone: a stack
 * that grows into .bss overwrites the page tables or a result word and
 * the run carries on. On the F446 the gap is about 4 KiB. So main
 * paints the window below its own frame -- STACK_WINDOW bytes, or down
 * to _end if that is nearer -- and the last test finds the lowest word
 * anything overwrote. The check is that the stack stayed 256 bytes
 * clear of the window's floor, which on a small part is .bss itself.
 *
 * The window is small on purpose: painting and scanning it is about
 * 7,000 instructions of the run's 96,000.
 */
#define STACK_PAINT 0x5354414Bu /* "STAK" */
#define STACK_WINDOW 4096u
extern char _end[];
static uint32_t g_stack_top;
static uint32_t g_stack_low;

static __attribute__((noinline)) void paint_stack(void)
{
    uint32_t sp;
    uint32_t low = (uint32_t)(uintptr_t)_end;

    __asm__ volatile("mv %0, sp" : "=r"(sp));
    if (sp < low + 128u) {
        return; /* no room at all: test_stack_depth fails on the zero */
    }
    if (sp - low > STACK_WINDOW) {
        low = sp - STACK_WINDOW;
    }
    low = (low + 3u) & ~3u;
    g_stack_top = sp;
    g_stack_low = low;
    /* Up to 64 bytes below this frame: nothing runs below it meanwhile. */
    for (uint32_t a = low; a < sp - 64u; a += 4u) {
        *(volatile uint32_t *)(uintptr_t)a = STACK_PAINT;
    }
}

static void test_stack_depth(void)
{
    uint32_t a = g_stack_low;

    while (a < g_stack_top &&
           *(volatile uint32_t *)(uintptr_t)a == STACK_PAINT) {
        a += 4u;
    }
    puts_("stack    ");
    puthex(g_stack_top - a);
    putc_('\n');
    check("stack-depth", g_stack_top != 0u && a - g_stack_low >= 256u, 1u);

    /*
     * And the page tables are where the link script put them, at the
     * base of RAM. The first initialised static in this file -- a float
     * for test_fs_dirty_chained -- gave .data four bytes, the 4 KiB
     * alignment of .bss.pages then pushed every table up a page, and
     * the F446 build stopped linking: "region RAM overflowed by 948
     * bytes". That shows only where RAM is 32 KiB; this shows it on
     * every host. Keep isatest's .data empty.
     */
    check("pages-at-ram-base", (uint32_t)(uintptr_t)&g_pt, 0x80000000u);
}

int main(void)
{
    paint_stack();
    puts_("emu ISA self-test\n");

    install_trap_handler();

    test_alu();
    test_lui_auipc();
    test_branches();
    test_loadstore();
    test_muldiv();
    test_atomics();
    test_csr();
    test_traps();
    test_cbo();
#if RV32_EXT_PMP
    test_pmp();
    test_pmp_exec();
#else
    test_pmp_absent();
    (void)test_pmp; /* kept compiled, so the build that has PMP is */
    (void)test_pmp_exec; /* not the only one that type-checks them   */
    (void)test_jit_trap_context;
#endif
    test_smode();
    test_sv32();
#if RV32_EXT_PMP
    test_jit_trap_context();
#endif
    test_aplic();
#if defined(__riscv_zacas)
    test_zacas();
#endif
#if defined(__riscv_flen) && (__riscv_flen >= 32)
    test_fpu();
    test_zfhmin();
    test_fs_dirty_chained();
#endif
    test_zicond();
    test_bitmanip();
    test_hints();
    test_zalasr();
    test_zawrs();
    test_zihpm();
    test_sscofpmf();
    test_discarded_reads();
    test_warl_widths();
    test_cbo_below_m();
    test_sc_nowhere();
#if defined(__riscv_flen) && (__riscv_flen >= 32)
    test_double_alignment();
#endif
    test_split_access();
    test_dtlb();
    test_mega_sfence();
    test_fetch_table();
    test_timer_interrupt();
    test_stack_depth(); /* last: everything above has run on the stack */

    puts_("checks   ");
    puthex((uint32_t)g_checks);
    puts_("\nfailures ");
    puthex((uint32_t)g_failures);
    putc_('\n');
    puts_(g_failures == 0 ? "PASS\n" : "FAIL\n");

    return g_failures;
}
