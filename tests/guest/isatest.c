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

/* ------------------------------------------------------------------ */
/* Platform                                                            */
/* ------------------------------------------------------------------ */

#define UART_BASE       0x10000000u
#define UART_THR        (*(volatile uint8_t *)(UART_BASE + 0x00u))
#define UART_LSR        (*(volatile uint8_t *)(UART_BASE + 0x05u))

#define CLINT_BASE      0x02000000u
#define CLINT_MTIMECMP_LO (*(volatile uint32_t *)(CLINT_BASE + 0x4000u))
#define CLINT_MTIMECMP_HI (*(volatile uint32_t *)(CLINT_BASE + 0x4004u))
#define CLINT_MTIME_LO    (*(volatile uint32_t *)(CLINT_BASE + 0xBFF8u))
#define CLINT_MTIME_HI    (*(volatile uint32_t *)(CLINT_BASE + 0xBFFCu))

#define csr_read(name) ({                                   \
    uint32_t v_;                                            \
    __asm__ volatile ("csrr %0, " name : "=r"(v_));         \
    v_;                                                     \
})

#define csr_write(name, val) \
    __asm__ volatile ("csrw " name ", %0" :: "r"((uint32_t)(val)))

#define csr_set(name, val) \
    __asm__ volatile ("csrs " name ", %0" :: "r"((uint32_t)(val)))

#define csr_clear(name, val) \
    __asm__ volatile ("csrc " name ", %0" :: "r"((uint32_t)(val)))

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
    __asm__ volatile ("" : "+r"(v));
    return v;
}

/* ------------------------------------------------------------------ */
/* APLIC (AIA), direct delivery mode                                   */
/* ------------------------------------------------------------------ */

#define APLIC_BASE          0x0C000000u
#define APLIC_R(off)        (*(volatile uint32_t *)(APLIC_BASE + (off)))
#define APLIC_DOMAINCFG     APLIC_R(0x0000u)
#define APLIC_SOURCECFG(i)  APLIC_R(0x0004u + 4u * ((i) - 1u))
#define APLIC_SETIP         APLIC_R(0x1C00u)
#define APLIC_SETIPNUM      APLIC_R(0x1CDCu)
#define APLIC_CLRIPNUM      APLIC_R(0x1DDCu)
#define APLIC_SETIE         APLIC_R(0x1E00u)
#define APLIC_SETIENUM      APLIC_R(0x1EDCu)
#define APLIC_CLRIENUM      APLIC_R(0x1FDCu)
#define APLIC_TARGET(i)     APLIC_R(0x3004u + 4u * ((i) - 1u))
#define APLIC_IDELIVERY     APLIC_R(0x4000u)
#define APLIC_ITHRESHOLD    APLIC_R(0x4008u)
#define APLIC_TOPI          APLIC_R(0x4018u)
#define APLIC_CLAIMI        APLIC_R(0x401Cu)

#define APLIC_SM_INACTIVE   0u
#define APLIC_SM_EDGE_RISE  4u

/* ------------------------------------------------------------------ */
/* Trap handling                                                       */
/* ------------------------------------------------------------------ */

static volatile uint32_t g_trap_count;
static volatile uint32_t g_last_cause;
static volatile uint32_t g_last_tval;
static volatile uint32_t g_last_claim;
static volatile uint32_t g_ext_count;

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
__attribute__((interrupt("machine"), aligned(4), used))
static void trap_handler(void)
{
    const uint32_t cause = csr_read("mcause");

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
        }
        return;
    }

    if (cause == 9u) {
        /*
         * ECALL from S-mode: how an S-mode test hands control back.
         *
         * MPP is set to M so the mret below returns to machine mode rather
         * than to the supervisor that called, and mepc to the point the
         * caller recorded before it left. This is the whole return path --
         * there is no other way back up a privilege level.
         */
        csr_write("mepc", g_smode_resume);
        csr_set("mstatus", 0x1800u);          /* MPP = M */
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

    check("addi",  opaque(100u) + 23u, 123u);
    check("addi-neg", opaque(100u) + (uint32_t)-1, 99u);
    check("slti",  (uint32_t)((int32_t)opaque((uint32_t)-5) < 3), 1u);
    check("sltiu", (uint32_t)(opaque((uint32_t)-5) < 3u), 0u);
    check("xori",  opaque(0xF0F0F0F0u) ^ 0xFFu, 0xF0F0F00Fu);
    check("ori",   opaque(0xF0F0F000u) | 0x0Fu, 0xF0F0F00Fu);
    check("andi",  opaque(0xF0F0F0F0u) & 0xFFu, 0xF0u);

    check("slli",  opaque(1u) << 31, 0x80000000u);
    check("srli",  opaque(0x80000000u) >> 31, 1u);
    check("srai",  (uint32_t)((int32_t)opaque(0x80000000u) >> 31), 0xFFFFFFFFu);
    check("srai-4",(uint32_t)((int32_t)opaque(0xFFFFFFF0u) >> 4), 0xFFFFFFFFu);

    check("add",   a + b, 0x12355677u);
    check("sub",   a - b, 0x12335679u);
    check("slt",   (uint32_t)((int32_t)a < (int32_t)b), 0u);
    check("sltu",  (uint32_t)(a < b), 0u);
    check("xor",   a ^ b, 0x1234A987u);
    check("or",    a | b, 0x1234FFFFu);
    check("and",   a & b, 0x00005678u);

    /* Register shifts use only the low 5 bits of the amount. */
    uint32_t sh = opaque(15u);
    check("sll",   a << sh, 0x12345678u << 15);
    check("srl",   a >> sh, 0x12345678u >> 15);
    check("sra",   (uint32_t)((int32_t)opaque(0x80000000u) >> sh),
                   (uint32_t)((int32_t)0x80000000 >> 15));

    /* Shift amounts use only the low 5 bits of the register. */
    check("sll-mask", opaque(1u) << (opaque(33u) & 31u), 2u);
}

static void test_lui_auipc(void)
{
    uint32_t v;
    __asm__ volatile ("lui %0, 0x12345" : "=r"(v));
    check("lui", v, 0x12345000u);

    /* auipc 0 yields the address of the auipc itself. */
    uint32_t pc, here;
    __asm__ volatile ("1: auipc %0, 0\n\t la %1, 1b" : "=r"(pc), "=r"(here));
    check("auipc", pc, here);
}

static void test_branches(void)
{
    uint32_t taken = 0;
    int32_t neg = (int32_t)opaque((uint32_t)-1);
    uint32_t big = opaque(0xFFFFFFFFu);

    if (opaque(5u) == 5u)                   taken |= 1u << 0;   /* beq  */
    if (opaque(5u) != 6u)                   taken |= 1u << 1;   /* bne  */
    if (neg < 1)                            taken |= 1u << 2;   /* blt  */
    if ((int32_t)opaque(1u) >= neg)         taken |= 1u << 3;   /* bge  */
    if (opaque(1u) < big)                   taken |= 1u << 4;   /* bltu */
    if (big >= opaque(1u))                  taken |= 1u << 5;   /* bgeu */

    check("branches", taken, 0x3Fu);
}

static uint8_t  g_mem8[8];
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
    uint8_t  b[4];
} g_pun;

static void test_loadstore(void)
{
    g_mem8[0] = 0x80u;
    g_mem8[1] = 0x7Fu;
    check("lbu", g_mem8[0], 0x80u);
    check("lb",  (uint32_t)(int32_t)(int8_t)g_mem8[0], 0xFFFFFF80u);
    check("lb+", (uint32_t)(int32_t)(int8_t)g_mem8[1], 0x7Fu);

    g_mem16[0] = 0x8000u;
    g_mem16[1] = 0x7FFFu;
    check("lhu", g_mem16[0], 0x8000u);
    check("lh",  (uint32_t)(int32_t)(int16_t)g_mem16[0], 0xFFFF8000u);
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
    check("mul-neg", (uint32_t)((int32_t)opaque((uint32_t)-3) * 5), (uint32_t)-15);

    check("mulh",
          (uint32_t)(((int64_t)(int32_t)opaque(0x40000000u) *
                      (int64_t)(int32_t)opaque(0x00000004u)) >> 32), 1u);
    check("mulhu",
          (uint32_t)(((uint64_t)opaque(0x80000000u) *
                      (uint64_t)opaque(0x00000002u)) >> 32), 1u);
    check("mulhsu",
          (uint32_t)(((int64_t)(int32_t)opaque(0xFFFFFFFFu) *
                      (int64_t)(uint64_t)opaque(0x00000002u)) >> 32),
          0xFFFFFFFFu);

    check("div",  (uint32_t)((int32_t)opaque(100u) / (int32_t)opaque(7u)), 14u);
    check("div-neg",
          (uint32_t)((int32_t)opaque((uint32_t)-100) / (int32_t)opaque(7u)),
          (uint32_t)-14);
    check("divu", opaque(100u) / opaque(7u), 14u);
    check("rem",  (uint32_t)((int32_t)opaque(100u) % (int32_t)opaque(7u)), 2u);
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
    __asm__ volatile ("div %0, %1, %2"
                      : "=r"(r) : "r"(opaque(1u)), "r"(opaque(0u)));
    check("div-by-0", r, 0xFFFFFFFFu);

    __asm__ volatile ("divu %0, %1, %2"
                      : "=r"(r) : "r"(opaque(1u)), "r"(opaque(0u)));
    check("divu-by-0", r, 0xFFFFFFFFu);

    __asm__ volatile ("rem %0, %1, %2"
                      : "=r"(r) : "r"(opaque(123u)), "r"(opaque(0u)));
    check("rem-by-0", r, 123u);

    __asm__ volatile ("remu %0, %1, %2"
                      : "=r"(r) : "r"(opaque(123u)), "r"(opaque(0u)));
    check("remu-by-0", r, 123u);

    __asm__ volatile ("div %0, %1, %2"
                      : "=r"(r)
                      : "r"(opaque(0x80000000u)), "r"(opaque(0xFFFFFFFFu)));
    check("div-overflow", r, 0x80000000u);

    __asm__ volatile ("rem %0, %1, %2"
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
    __asm__ volatile ("amoadd.w %0, %2, (%1)"
                      : "=&r"(old) : "r"(&g_atomic), "r"(11u) : "memory");
    check("amoadd-old", old, 100u);
    check("amoadd-new", g_atomic, 111u);

    g_atomic = 0xF0u;
    __asm__ volatile ("amoswap.w %0, %2, (%1)"
                      : "=&r"(old) : "r"(&g_atomic), "r"(0x0Fu) : "memory");
    check("amoswap-old", old, 0xF0u);
    check("amoswap-new", g_atomic, 0x0Fu);

    g_atomic = 0xF0F0u;
    __asm__ volatile ("amoand.w %0, %2, (%1)"
                      : "=&r"(old) : "r"(&g_atomic), "r"(0xFF00u) : "memory");
    check("amoand", g_atomic, 0xF000u);

    g_atomic = 0xF000u;
    __asm__ volatile ("amoor.w %0, %2, (%1)"
                      : "=&r"(old) : "r"(&g_atomic), "r"(0x000Fu) : "memory");
    check("amoor", g_atomic, 0xF00Fu);

    g_atomic = 0xFFFFu;
    __asm__ volatile ("amoxor.w %0, %2, (%1)"
                      : "=&r"(old) : "r"(&g_atomic), "r"(0x0FF0u) : "memory");
    check("amoxor", g_atomic, 0xF00Fu);

    /* Signed min/max must treat the stored value as signed. */
    g_atomic = (uint32_t)-5;
    __asm__ volatile ("amomin.w %0, %2, (%1)"
                      : "=&r"(old) : "r"(&g_atomic), "r"(3u) : "memory");
    check("amomin", g_atomic, (uint32_t)-5);

    g_atomic = (uint32_t)-5;
    __asm__ volatile ("amomax.w %0, %2, (%1)"
                      : "=&r"(old) : "r"(&g_atomic), "r"(3u) : "memory");
    check("amomax", g_atomic, 3u);

    /* ...and unsigned min/max must not. */
    g_atomic = (uint32_t)-5;
    __asm__ volatile ("amominu.w %0, %2, (%1)"
                      : "=&r"(old) : "r"(&g_atomic), "r"(3u) : "memory");
    check("amominu", g_atomic, 3u);

    g_atomic = (uint32_t)-5;
    __asm__ volatile ("amomaxu.w %0, %2, (%1)"
                      : "=&r"(old) : "r"(&g_atomic), "r"(3u) : "memory");
    check("amomaxu", g_atomic, (uint32_t)-5);

    /* An uninterrupted LR/SC pair must succeed. */
    g_atomic = 7u;
    __asm__ volatile ("lr.w %0, (%2)\n\t"
                      "sc.w %1, %3, (%2)"
                      : "=&r"(old), "=&r"(sc)
                      : "r"(&g_atomic), "r"(42u)
                      : "memory");
    check("lr.w", old, 7u);
    check("sc.w-ok", sc, 0u);
    check("sc.w-stored", g_atomic, 42u);

    /* A store between LR and SC must break the reservation. */
    g_atomic = 7u;
    __asm__ volatile ("lr.w %0, (%2)\n\t"
                      "sw %3, 0(%2)\n\t"
                      "sc.w %1, %4, (%2)"
                      : "=&r"(old), "=&r"(sc)
                      : "r"(&g_atomic), "r"(9u), "r"(42u)
                      : "memory");
    check("sc.w-broken", sc != 0u, 1u);
    check("sc.w-nostore", g_atomic, 9u);

    /* SC without a preceding LR must fail. */
    g_atomic = 1u;
    __asm__ volatile ("sc.w %0, %2, (%1)"
                      : "=&r"(sc) : "r"(&g_atomic), "r"(99u) : "memory");
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
    __asm__ volatile ("csrrs %0, mscratch, %1" : "=r"(old) : "r"(0x0000FFFFu));
    check("csrrs-old", old, 0xA5A5A5A5u);
    check("csrrs-new", csr_read("mscratch"), 0xA5A5FFFFu);

    __asm__ volatile ("csrrc %0, mscratch, %1" : "=r"(old) : "r"(0x0000FF00u));
    check("csrrc-new", csr_read("mscratch"), 0xA5A500FFu);

    /* csrrs with x0 as the source must not write. */
    csr_write("mscratch", 0x5A5A5A5Au);
    __asm__ volatile ("csrrs %0, mscratch, zero" : "=r"(old));
    check("csrrs-nowrite", csr_read("mscratch"), 0x5A5A5A5Au);

    /* misa must advertise exactly the extensions we built. */
    const uint32_t misa = csr_read("misa");
    check("misa-mxl", misa >> 30, 1u);                        /* MXL=32 */
    check("misa-I", (misa >> ('I' - 'A')) & 1u, 1u);
    check("misa-M", (misa >> ('M' - 'A')) & 1u, 1u);
    check("misa-A", (misa >> ('A' - 'A')) & 1u, 1u);
    check("misa-C", (misa >> ('C' - 'A')) & 1u, 1u);

    check("mhartid", csr_read("mhartid"), 0u);

    /* Counters must advance. */
    const uint32_t c0 = csr_read("mcycle");
    const uint32_t i0 = csr_read("minstret");
    for (volatile int i = 0; i < 10; i++) { }
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
    __asm__ volatile (".word 0xffffffff");
    check("trap-illegal-taken", g_trap_count - before, 1u);
    check("trap-illegal-cause", g_last_cause, 2u);
    check("trap-illegal-tval", g_last_tval, 0xFFFFFFFFu);

    /* A 16-bit illegal encoding must report the 2-byte parcel in mtval. */
    before = g_trap_count;
    __asm__ volatile (".half 0x0000");
    check("trap-illegal16-taken", g_trap_count - before, 1u);
    check("trap-illegal16-cause", g_last_cause, 2u);
    check("trap-illegal16-tval", g_last_tval, 0u);

    /* Breakpoint. */
    before = g_trap_count;
    __asm__ volatile ("ebreak");
    check("trap-ebreak-taken", g_trap_count - before, 1u);
    check("trap-ebreak-cause", g_last_cause, 3u);

    /* Misaligned load: the core reports rather than emulating. */
    before = g_trap_count;
    {
        uint32_t dst;
        volatile uint32_t *bad = (volatile uint32_t *)((uintptr_t)g_mem32 + 1u);
        __asm__ volatile ("lw %0, 0(%1)" : "=r"(dst) : "r"(bad));
        (void)dst;
    }
    check("trap-lw-misaligned-taken", g_trap_count - before, 1u);
    check("trap-lw-misaligned-cause", g_last_cause, 4u);

    /* Misaligned store. */
    before = g_trap_count;
    {
        volatile uint32_t *bad = (volatile uint32_t *)((uintptr_t)g_mem32 + 1u);
        __asm__ volatile ("sw %0, 0(%1)" :: "r"(0u), "r"(bad));
    }
    check("trap-sw-misaligned-taken", g_trap_count - before, 1u);
    check("trap-sw-misaligned-cause", g_last_cause, 6u);

    /* Access fault: nothing is mapped here. */
    before = g_trap_count;
    {
        uint32_t dst;
        volatile uint32_t *bad = (volatile uint32_t *)0x70000000u;
        __asm__ volatile ("lw %0, 0(%1)" : "=r"(dst) : "r"(bad));
        (void)dst;
    }
    check("trap-access-taken", g_trap_count - before, 1u);
    check("trap-access-cause", g_last_cause, 5u);
    check("trap-access-tval", g_last_tval, 0x70000000u);

    /* A non-existent CSR must raise illegal instruction, not read as zero. */
    before = g_trap_count;
    {
        uint32_t v;
        __asm__ volatile ("csrr %0, 0x7c0" : "=r"(v));
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

    csr_set("mie", 1u << 7);        /* MTIE */
    csr_set("mstatus", 1u << 3);    /* MIE  */

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

/* One cache block, aligned so cbo.* covers exactly this object. */
static volatile uint32_t g_block[16] __attribute__((aligned(64)));

static void test_cbo(void)
{
    for (int i = 0; i < 16; i++) {
        g_block[i] = 0xFFFFFFFFu;
    }

    /*
     * cbo.zero is the only one of these with an architecturally visible
     * effect, so it is the one that can be checked directly. The block
     * size is 32 bytes, so exactly the first 8 words must be cleared and
     * the ninth must be untouched.
     */
    __asm__ volatile ("cbo.zero (%0)" :: "r"(&g_block[0]) : "memory");

    uint32_t cleared = 0, intact = 0;
    for (int i = 0; i < 8; i++) {
        if (g_block[i] == 0u) {
            cleared++;
        }
    }
    for (int i = 8; i < 16; i++) {
        if (g_block[i] == 0xFFFFFFFFu) {
            intact++;
        }
    }
    check("cbo.zero-cleared", cleared, 8u);
    check("cbo.zero-bounded", intact, 8u);

    /*
     * The maintenance operations have no visible effect on a coherent
     * single-hart system, so all that can be checked is that they retire
     * without trapping rather than raising illegal instruction.
     */
    const uint32_t before = g_trap_count;
    __asm__ volatile ("cbo.clean (%0)" :: "r"(&g_block[0]) : "memory");
    __asm__ volatile ("cbo.inval (%0)" :: "r"(&g_block[0]) : "memory");
    __asm__ volatile ("cbo.flush (%0)" :: "r"(&g_block[0]) : "memory");
    check("cbo-maint-no-trap", g_trap_count - before, 0u);

    /* A CBO against an unmapped address must fault, not fault silently. */
    const uint32_t b2 = g_trap_count;
    __asm__ volatile ("cbo.clean (%0)" :: "r"(0x70000000u) : "memory");
    check("cbo-unmapped-traps", g_trap_count - b2, 1u);
}

/* ------------------------------------------------------------------ */
/* Zacas                                                               */
/* ------------------------------------------------------------------ */

/*
 * Only built when the guest is compiled with _zacas. These checks pass,
 * which is what localises the remaining Zacas problem: amocas.w's
 * compare-and-swap semantics are right, so the official suite's failure is
 * elsewhere. See the roadmap note in README.md.
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
    __asm__ volatile ("amocas.w %0, %2, (%1)"
                      : "+r"(rd) : "r"(&g_cas), "r"(sw) : "memory");
    check("amocas-eq-rd",  rd, 0x11112222u);
    check("amocas-eq-mem", g_cas, 0xAABBCCDDu);

    /* Comparand differs: no store, rd still returns what memory held. */
    g_cas = 0x11112222u;
    rd = 0x99998888u;
    sw = 0xAABBCCDDu;
    __asm__ volatile ("amocas.w %0, %2, (%1)"
                      : "+r"(rd) : "r"(&g_cas), "r"(sw) : "memory");
    check("amocas-ne-rd",  rd, 0x11112222u);
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
        register uint32_t p0 __asm__("a0") = 0x33334444u;   /* comparand lo */
        register uint32_t p1 __asm__("a1") = 0x11112222u;   /* comparand hi */
        register uint32_t s0 __asm__("a2") = 0xCCCCDDDDu;   /* swap lo      */
        register uint32_t s1 __asm__("a3") = 0xAAAABBBBu;   /* swap hi      */

        g_cas64 = 0x1111222233334444ull;
        __asm__ volatile ("amocas.d %0, %2, (%4)"
                          : "+r"(p0), "+r"(p1)
                          : "r"(s0), "r"(s1), "r"(&g_cas64) : "memory");
        got_lo = p0;
        got_hi = p1;
    }
    mem_lo = (uint32_t)g_cas64;
    mem_hi = (uint32_t)(g_cas64 >> 32);
    check("amocas.d-eq-lo",   got_lo, 0x33334444u);
    check("amocas.d-eq-hi",   got_hi, 0x11112222u);
    check("amocas.d-eq-mem",  mem_lo, 0xCCCCDDDDu);
    check("amocas.d-eq-memh", mem_hi, 0xAAAABBBBu);

    /* Mismatching comparand: no store, but rd still takes the loaded value. */
    {
        register uint32_t p0 __asm__("a0") = 0xDEADBEEFu;
        register uint32_t p1 __asm__("a1") = 0x11112222u;
        register uint32_t s0 __asm__("a2") = 0xCCCCDDDDu;
        register uint32_t s1 __asm__("a3") = 0xAAAABBBBu;

        g_cas64 = 0x1111222233334444ull;
        __asm__ volatile ("amocas.d %0, %2, (%4)"
                          : "+r"(p0), "+r"(p1)
                          : "r"(s0), "r"(s1), "r"(&g_cas64) : "memory");
        got_lo = p0;
        got_hi = p1;
    }
    mem_lo = (uint32_t)g_cas64;
    mem_hi = (uint32_t)(g_cas64 >> 32);
    check("amocas.d-ne-lo",   got_lo, 0x33334444u);
    check("amocas.d-ne-hi",   got_hi, 0x11112222u);
    check("amocas.d-ne-mem",  mem_lo, 0x33334444u);
    check("amocas.d-ne-memh", mem_hi, 0x11112222u);
}
#endif

/* ------------------------------------------------------------------ */
/* F extension                                                         */
/* ------------------------------------------------------------------ */

#if defined(__riscv_flen) && (__riscv_flen >= 32)

/* Operate on raw bit patterns: the guest ABI is ilp32 (soft-float), so
 * floats cannot be passed to check() as floats without conversion. */
#define FOP2(mn, a, b) ({                                       \
    uint32_t r_, x_ = (a), y_ = (b);                            \
    __asm__ volatile ("fmv.w.x fa0, %1\n\t"                    \
                      "fmv.w.x fa1, %2\n\t"                    \
                      mn " fa2, fa0, fa1\n\t"                  \
                      "fmv.x.w %0, fa2"                         \
                      : "=r"(r_) : "r"(x_), "r"(y_)             \
                      : "fa0", "fa1", "fa2");                   \
    r_; })

#define FCMP(mn, a, b) ({                                       \
    uint32_t r_, x_ = (a), y_ = (b);                            \
    __asm__ volatile ("fmv.w.x fa0, %1\n\t"                    \
                      "fmv.w.x fa1, %2\n\t"                    \
                      mn " %0, fa0, fa1"                        \
                      : "=r"(r_) : "r"(x_), "r"(y_)             \
                      : "fa0", "fa1");                          \
    r_; })

#define F1_0  0x3F800000u
#define F2_0  0x40000000u
#define F3_0  0x40400000u
#define F4_0  0x40800000u
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
__attribute__((noinline))
static uint32_t fp_site(uint32_t bits)
{
    uint32_t r;
    __asm__ volatile ("fmv.w.x fa0, %1\n\t fadd.s fa1, fa0, fa0\n\t"
                      "fmv.x.w %0, fa1"
                      : "=r"(r) : "r"(bits) : "fa0", "fa1");
    return r;
}

__attribute__((noinline))
static uint32_t cvt_w_dyn(uint32_t bits)
{
    uint32_t r;
    __asm__ volatile ("fmv.w.x fa0, %1\n\t fcvt.w.s %0, fa0, dyn"
                      : "=r"(r) : "r"(bits) : "fa0");
    return r;
}

static void test_fpu(void)
{
    /* Arithmetic. */
    check("fadd",  FOP2("fadd.s", F1_0, F2_0), F3_0);
    check("fsub",  FOP2("fsub.s", F3_0, F1_0), F2_0);
    check("fmul",  FOP2("fmul.s", F2_0, F2_0), F4_0);
    check("fdiv",  FOP2("fdiv.s", F4_0, F2_0), F2_0);
    check("fsqrt", FOP2("fadd.s", 0u, 0u) | 0u, 0u);   /* +0 + +0 == +0 */

    {   /* sqrt(4) == 2 */
        uint32_t r, x = F4_0;
        __asm__ volatile ("fmv.w.x fa0, %1\n\t fsqrt.s fa1, fa0\n\t"
                          "fmv.x.w %0, fa1"
                          : "=r"(r) : "r"(x) : "fa0", "fa1");
        check("fsqrt-4", r, F2_0);
    }

    /* Comparisons return 0/1 in an integer register. */
    check("feq",   FCMP("feq.s", F1_0, F1_0), 1u);
    check("flt",   FCMP("flt.s", F1_0, F2_0), 1u);
    check("fle",   FCMP("fle.s", F2_0, F1_0), 0u);
    /* Any comparison against NaN is false. */
    check("feq-nan", FCMP("feq.s", F_QNAN, F1_0), 0u);

    /* Sign injection and min/max. */
    check("fsgnj", FOP2("fsgnj.s", F1_0, 0x80000000u), F1_0 | 0x80000000u);
    check("fmin",  FOP2("fmin.s", F1_0, F2_0), F1_0);
    check("fmax",  FOP2("fmax.s", F1_0, F2_0), F2_0);
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
    check("fmax-nan",    FOP2("fmax.s", F_QNAN, F2_0), F2_0);
    check("fmin-nan-rs2", FOP2("fmin.s", F2_0, F_QNAN), F2_0);
    /* Two NaNs give the canonical NaN, not either input. */
    check("fmin-2nan",   FOP2("fmin.s", F_QNAN, F_QNAN), 0x7FC00000u);
    check("fmax-2nan",   FOP2("fmax.s", F_QNAN, F_QNAN), 0x7FC00000u);
    /*
     * -0.0 is *below* +0.0 here, though IEEE comparison calls them equal.
     * A select driven by VCMP returns whichever operand it was told to
     * prefer on equality, so it gets one of these two wrong whichever way
     * it is written.
     */
    check("fmin-zeros",  FOP2("fmin.s", 0x80000000u, 0u), 0x80000000u);
    check("fmax-zeros",  FOP2("fmax.s", 0x80000000u, 0u), 0u);
    check("fmin-zeros-rev", FOP2("fmin.s", 0u, 0x80000000u), 0x80000000u);
    check("fmax-zeros-rev", FOP2("fmax.s", 0u, 0x80000000u), 0u);

    /* A quiet NaN operand is not an invalid operation; a signalling one is. */
    csr_write("fflags", 0u);
    (void)FOP2("fmin.s", F_QNAN, F2_0);
    check("fmin-qnan-noflag", csr_read("fflags") & 0x10u, 0u);
    csr_write("fflags", 0u);
    (void)FOP2("fmin.s", 0x7F800001u, F2_0);   /* signalling NaN */
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
        __asm__ volatile ("fmv.w.x fa0, %1\n\t fmv.w.x fa1, %2\n\t"
                          "fmv.w.x fa2, %3\n\t fmadd.s fa3, fa0, fa1, fa2\n\t"
                          "fmv.x.w %0, fa3"
                          : "=r"(r) : "r"(a), "r"(b), "r"(c)
                          : "fa0", "fa1", "fa2", "fa3");
        check("fmadd", r, 0x40A00000u);            /* 5.0 */

        __asm__ volatile ("fmv.w.x fa0, %1\n\t fmv.w.x fa1, %2\n\t"
                          "fmv.w.x fa2, %3\n\t fnmsub.s fa3, fa0, fa1, fa2\n\t"
                          "fmv.x.w %0, fa3"
                          : "=r"(r) : "r"(a), "r"(b), "r"(c)
                          : "fa0", "fa1", "fa2", "fa3");
        check("fnmsub", r, 0x40400000u | 0x80000000u);  /* -3.0 */
    }

    /* Conversions both ways. */
    {
        uint32_t r; int32_t i = -7;
        __asm__ volatile ("fcvt.s.w fa0, %1\n\t fmv.x.w %0, fa0"
                          : "=r"(r) : "r"(i) : "fa0");
        check("fcvt.s.w", r, 0x40E00000u | 0x80000000u);   /* -7.0 */

        uint32_t back, src = 0x41200000u;                  /* 10.0 */
        __asm__ volatile ("fmv.w.x fa0, %1\n\t fcvt.w.s %0, fa0, rtz"
                          : "=r"(back) : "r"(src) : "fa0");
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
#define CVT_W(dst, bits, mode)                                            \
    __asm__ volatile ("fmv.w.x fa0, %1\n\t fcvt.w.s %0, fa0, " mode       \
                      : "=r"(dst) : "r"(bits) : "fa0")
#define CVT_WU(dst, bits, mode)                                           \
    __asm__ volatile ("fmv.w.x fa0, %1\n\t fcvt.wu.s %0, fa0, " mode      \
                      : "=r"(dst) : "r"(bits) : "fa0")

    {
        uint32_t r;

        /* The divergence itself. NaN goes to the maximum, not to zero. */
        csr_write("fflags", 0u);
        CVT_W(r, 0x7FC00000u, "rtz");            /* canonical qNaN */
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
        CVT_W(r, 0x7F800000u, "rtz");            /* +inf */
        check("fcvt.w.s-posinf", r, 0x7FFFFFFFu);
        check("fcvt.w.s-posinf-nv", csr_read("fflags"), 0x10u);

        CVT_W(r, 0xFF800000u, "rtz");            /* -inf */
        check("fcvt.w.s-neginf", r, 0x80000000u);

        CVT_W(r, 0x4F800000u, "rtz");            /* 2^32, over signed range */
        check("fcvt.w.s-over", r, 0x7FFFFFFFu);

        CVT_WU(r, 0xBF800000u, "rtz");           /* -1.0, under unsigned */
        check("fcvt.wu.s-neg", r, 0u);

        CVT_WU(r, 0xFF800000u, "rtz");           /* -inf */
        check("fcvt.wu.s-neginf", r, 0u);

        /* An exact conversion raises nothing at all. */
        csr_write("fflags", 0u);
        CVT_W(r, 0xC1200000u, "rtz");            /* -10.0 */
        check("fcvt.w.s-exact", r, (uint32_t)-10);
        check("fcvt.w.s-exact-noflags", csr_read("fflags"), 0u);

        /* Every rounding mode the JIT claims to translate, on a tie. */
        CVT_W(r, 0x40200000u, "rne");            /* 2.5 -> 2, ties to even */
        check("fcvt.w.s-rne", r, 2u);
        CVT_W(r, 0x40600000u, "rne");            /* 3.5 -> 4, ties to even */
        check("fcvt.w.s-rne-odd", r, 4u);
        CVT_W(r, 0x40200000u, "rtz");
        check("fcvt.w.s-rtz", r, 2u);
        CVT_W(r, 0x40200000u, "rdn");
        check("fcvt.w.s-rdn", r, 2u);
        CVT_W(r, 0x40200000u, "rup");
        check("fcvt.w.s-rup", r, 3u);

        CVT_W(r, 0xC0200000u, "rtz");            /* -2.5 */
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
        CVT_WU(r, 0xBF000000u, "rtz");           /* -0.5 */
        check("fcvt.wu.s-negzero", r, 0u);
        check("fcvt.wu.s-negzero-nx", csr_read("fflags"), 0x01u);

        /* Dynamic rounding: the mode comes from frm, not the encoding. */
        csr_write("fflags", 0u);
        csr_write("frm", 3u);                    /* RUP */
        CVT_W(r, 0x40200000u, "dyn");
        check("fcvt.w.s-dyn-rup", r, 3u);
        csr_write("frm", 1u);                    /* RTZ */
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
        const uint32_t f2_5 = 0x40200000u;      /*  2.5 */
        const uint32_t fn2_5 = 0xC0200000u;     /* -2.5 */

        csr_write("frm", 3u);                   /* RUP */
        check("dyn-rup", cvt_w_dyn(f2_5), 3u);

        csr_write("frm", 2u);                   /* RDN: needs the rebuild */
        check("dyn-rdn", cvt_w_dyn(f2_5), 2u);

        csr_write("frm", 0u);                   /* RNE: ties to even */
        check("dyn-rne", cvt_w_dyn(f2_5), 2u);

        csr_write("frm", 4u);                   /* RMM: ties away */
        check("dyn-rmm", cvt_w_dyn(f2_5), 3u);
        check("dyn-rmm-neg", cvt_w_dyn(fn2_5), (uint32_t)-3);

        csr_write("frm", 3u);                   /* RUP: re-specialise */
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
        check("fs-on", fp_site(F1_0), F2_0);        /* 1.0 + 1.0 */

        const uint32_t before = g_trap_count;
        csr_clear("mstatus", 3u << 13);             /* FS = Off */
        (void)fp_site(F1_0);
        const uint32_t traps = g_trap_count - before;

        /* Restore before checking, so a failure here does not kill the
         * remaining FP tests as well. */
        csr_set("mstatus", 1u << 13);               /* FS = Initial */

        check("fs-off-traps", traps, 3u);           /* one per FP insn */
        check("fs-off-cause", g_last_cause, 2u);    /* illegal instruction */

        /* And with FS back on it works again. */
        check("fs-restored", fp_site(F1_0), F2_0);
    }
#undef CVT_W
#undef CVT_WU

    /* fclass: 1<<6 is a positive normal, 1<<3 is negative zero. */
    {
        uint32_t r, x = F1_0;
        __asm__ volatile ("fmv.w.x fa0, %1\n\t fclass.s %0, fa0"
                          : "=r"(r) : "r"(x) : "fa0");
        check("fclass-norm", r, 1u << 6);
        x = 0x80000000u;
        __asm__ volatile ("fmv.w.x fa0, %1\n\t fclass.s %0, fa0"
                          : "=r"(r) : "r"(x) : "fa0");
        check("fclass-negzero", r, 1u << 3);

        /*
         * The rest of the ten-way split. FCLASS has no ARM equivalent at
         * all, so it runs through the helper; these pin every bit of the
         * result so that stays honest.
         */
#define FCLASS(bits) ({                                                   \
    uint32_t c_;                                                          \
    const uint32_t b_ = (bits);                                           \
    __asm__ volatile ("fmv.w.x fa0, %1\n\t fclass.s %0, fa0"              \
                      : "=r"(c_) : "r"(b_) : "fa0");                      \
    c_; })

        check("fclass-neginf",  FCLASS(0xFF800000u), 1u << 0);
        check("fclass-negnorm", FCLASS(0xBF800000u), 1u << 1);  /* -1.0 */
        check("fclass-negsub",  FCLASS(0x80000001u), 1u << 2);
        check("fclass-poszero", FCLASS(0x00000000u), 1u << 4);
        check("fclass-possub",  FCLASS(0x00000001u), 1u << 5);
        check("fclass-posinf",  FCLASS(0x7F800000u), 1u << 7);
        check("fclass-snan",    FCLASS(0x7F800001u), 1u << 8);
        check("fclass-qnan",    FCLASS(0x7FC00000u), 1u << 9);
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
        __asm__ volatile ("fmv.w.x fa0, %1\n\t fsw fa0, 0(%2)"
                          :: "r"(0), "r"(F3_0), "r"(&g_fmem)
                          : "fa0", "memory");
        check("fsw", g_fmem, F3_0);
        __asm__ volatile ("flw fa0, 0(%1)\n\t fmv.x.w %0, fa0"
                          : "=r"(r) : "r"(&g_fmem) : "fa0");
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
__attribute__((noinline))
static void pmp_store(volatile uint32_t *p, uint32_t v)
{
    *p = v;
}

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
     * Entry 0: NAPOT over the eight bytes, locked, readable but not
     * writable. Locked is what makes it apply to M-mode at all.
     */
    csr_write("pmpaddr0", ((uint32_t)(uintptr_t)g_pmp_area) >> 2);
    csr_write("pmpcfg0", 0x99u);          /* L | NAPOT | R */

    /* Reads still work. */
    check("pmp-read-ok", g_pmp_area[0], 0xA5A5A5A5u);

    /* Writes now fault, and report a store access fault at the address. */
    uint32_t before = g_trap_count;
    __asm__ volatile ("sw %0, 0(%1)"
                      :: "r"(0xDEADBEEFu), "r"(g_pmp_area) : "memory");
    check("pmp-write-traps", g_trap_count - before, 1u);
    check("pmp-write-cause", g_last_cause, 7u);   /* store access fault */
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
    csr_write("pmpcfg0", 0x9900u);        /* byte 1: L | NAPOT | R */
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

    __asm__ volatile (
        "la   t0, 1f\n"
        "sw   t0, 0(%1)\n"
        "jalr %0\n"
        "1:\n"
        :: "r"(entry), "r"(&g_fetch_resume), "r"(a0_), "r"(a1_)
         : "t0", "ra", "memory");
}

static void test_pmp_exec(void)
{
    g_xbuf[0] = 0x00b52023u;             /* sw   a1, 0(a0)  */
    g_xbuf[1] = 0x00008067u;             /* jalr x0, 0(ra)  */
    __asm__ volatile ("fence.i" ::: "memory");

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
    check("pmpx-exec-cause", g_last_cause, 1u);   /* insn access fault */
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
#define MSTATUS_MPP_S    0x00000800u
#define MSTATUS_SUM      0x00040000u
#define MSTATUS_TW       0x00200000u
#define MSTATUS_TSR      0x00400000u

#define PTE_V 0x001u
#define PTE_R 0x002u
#define PTE_W 0x004u
#define PTE_X 0x008u
#define PTE_U 0x010u
#define PTE_A 0x040u
#define PTE_D 0x080u

#define SATP_SV32 0x80000000u

/*
 * Page tables. The root alone would do for an identity map built from
 * megapages; the second level exists so the 4 KiB path is exercised too,
 * and so there is somewhere to change one PTE and watch the effect.
 */
static uint32_t g_root[1024] __attribute__((aligned(4096)));
static uint32_t g_leaf[1024] __attribute__((aligned(4096)));
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
#define PAGE_DATA  0u
#define PAGE_EXEC1 1u
#define PAGE_EXEC2 2u
static uint32_t g_page[3 * 1024] __attribute__((aligned(4096)));

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
#define VA_RW   0x90000000u
#define VA_RO   0x90001000u
#define VA_USER 0x90002000u
#define VA_NOA  0x90003000u
#define VA_HOLE 0x90004000u
#define VA_X    0x90005000u

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
        g_leaf[i] = 0u;                       /* unmapped by default */
    }

    const uint32_t pa = page_pa(PAGE_DATA);

    g_leaf[VPN0(VA_RW)]   = leaf_pte(pa, PTE_V|PTE_R|PTE_W|PTE_A|PTE_D);
    g_leaf[VPN0(VA_RO)]   = leaf_pte(pa, PTE_V|PTE_R|PTE_A);
    g_leaf[VPN0(VA_USER)] = leaf_pte(pa, PTE_V|PTE_R|PTE_W|PTE_U|PTE_A|PTE_D);
    /* A is clear: under Svade the hardware never sets it, so the first
     * touch faults and software is expected to fix it up. */
    g_leaf[VPN0(VA_NOA)]  = leaf_pte(pa, PTE_V|PTE_R|PTE_W|PTE_D);
    g_leaf[VPN0(VA_X)]    = leaf_pte(page_pa(PAGE_EXEC1),
                                     PTE_V|PTE_R|PTE_X|PTE_A);
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
    __asm__ volatile (
        "la   t0, g_smode_regs\n"
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
        "lw   s11, 48(t0)\n"
        :: "r"(fn), "r"(&g_smode_resume),
           "i"(MSTATUS_MPP_MASK), "i"(MSTATUS_MPP_S)
        : "t0", "t1", "t2", "a0", "a1", "a2", "a3", "a4", "a5",
          "a6", "a7", "ra", "memory");
}

/* Every S-mode test body ends with this. */
#define LEAVE_SMODE() __asm__ volatile ("ecall" ::: "memory")

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
    __asm__ volatile ("wfi");
    check("s-tw-wfi-traps", g_trap_count - before, 1u);
    check("s-tw-wfi-cause", g_last_cause, 2u);
    LEAVE_SMODE();
}

static void smode_tsr_sret(void)
{
    const uint32_t before = g_trap_count;
    __asm__ volatile ("sret");
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
    check("sv32-noa-unchanged",
          g_leaf[VPN0(VA_NOA)] & PTE_A, 0u);

    /* D is the same rule for stores: readable, not writable. */
    g_leaf[VPN0(VA_NOA)] |= PTE_A;
    g_leaf[VPN0(VA_NOA)] &= ~PTE_D;
    __asm__ volatile ("sfence.vma" ::: "memory");
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
    g_leaf[VPN0(VA_HOLE)] = leaf_pte(page_pa(PAGE_DATA),
                                     PTE_V|PTE_R|PTE_W|PTE_A|PTE_D);
    __asm__ volatile ("sfence.vma" ::: "memory");
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

    g_root[VPN1(VA_RW)] = leaf_pte((uint32_t)(uintptr_t)g_leaf,
                                   PTE_V|PTE_W);
    __asm__ volatile ("sfence.vma" ::: "memory");
    before = g_trap_count;
    (void)*(volatile uint32_t *)VA_RW;
    check("sv32-wnor-traps", g_trap_count - before, 1u);
    check("sv32-wnor-cause", g_last_cause, 13u);
    g_root[VPN1(VA_RW)] = saved_root;
    __asm__ volatile ("sfence.vma" ::: "memory");

    /*
     * D, A and U are reserved in a *non-leaf* PTE and must be clear. An
     * implementation that ignores them still translates every legal table
     * correctly, which is why this needs asking directly.
     */
    g_root[VPN1(VA_RW)] |= PTE_A;
    __asm__ volatile ("sfence.vma" ::: "memory");
    before = g_trap_count;
    (void)*(volatile uint32_t *)VA_RW;
    check("sv32-nonleaf-a-traps", g_trap_count - before, 1u);
    check("sv32-nonleaf-a-cause", g_last_cause, 13u);

    g_root[VPN1(VA_RW)] &= ~PTE_A;
    g_root[VPN1(VA_RW)] |= PTE_U;
    __asm__ volatile ("sfence.vma" ::: "memory");
    before = g_trap_count;
    (void)*(volatile uint32_t *)VA_RW;
    check("sv32-nonleaf-u-traps", g_trap_count - before, 1u);

    /* Put the table back, and prove it: the same access now works. */
    g_root[VPN1(VA_RW)] &= ~PTE_U;
    __asm__ volatile ("sfence.vma" ::: "memory");
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
    __asm__ volatile (
        "la   t0, 1f\n"
        "sw   t0, 0(%1)\n"
        "jalr %0\n"
        "1:\n"
        :: "r"(entry), "r"(&g_fetch_resume)
         : "t0", "t1", "t2", "ra", "a0", "a1", "a2", "a3", "a4", "a5",
           "a6", "a7", "memory");
}

static void smode_exec(void)
{
    uint32_t (*fn)(void) = (uint32_t (*)(void))VA_X;
    uint32_t before;

    check("sv32-exec-first", fn(), 0x111u);

    /* Repoint the same VA at the second stub. Without the fence a stale
     * translation -- in the TLB or in a cached JIT block -- would keep
     * answering with the first. */
    g_leaf[VPN0(VA_X)] = leaf_pte(page_pa(PAGE_EXEC2),
                                  PTE_V|PTE_R|PTE_X|PTE_A);
    __asm__ volatile ("sfence.vma" ::: "memory");
    check("sv32-exec-remapped", fn(), 0x222u);

    /* Withdraw execute permission and the fetch itself must fault. */
    g_leaf[VPN0(VA_X)] = leaf_pte(page_pa(PAGE_EXEC2),
                                  PTE_V|PTE_R|PTE_A);
    __asm__ volatile ("sfence.vma" ::: "memory");
    before = g_trap_count;
    call_may_fault(VA_X);
    check("sv32-exec-nox-traps", g_trap_count - before, 1u);
    check("sv32-exec-nox-cause", g_last_cause, 12u);
    check("sv32-exec-nox-tval", g_last_tval, VA_X);

    LEAVE_SMODE();
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
    csr_write("pmpaddr15", 0xFFFFFFFFu);
    csr_write("pmpcfg3", 0x9F000000u);     /* byte 3: L | NAPOT | RWX */
    check("s-pmp-background", (csr_read("pmpcfg3") >> 24) & 0xFFu, 0x9Fu);

    csr_write("stvec", 0u);
    csr_write("medeleg", 0u);              /* everything to M for now */
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

    /* medeleg and mideleg are real registers now, and WARL. */
    csr_write("medeleg", 0xFFFFFFFFu);
    check("s-medeleg-warl", csr_read("medeleg"), 0x3FFu);
    csr_write("medeleg", 0u);
    csr_write("mideleg", 0xFFFFFFFFu);
    check("s-mideleg-warl", csr_read("mideleg"), 0x222u);
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
    g_page[1024] = 0x11100513u;   /* li a0, 0x111 */
    g_page[1025] = 0x00008067u;   /* ret          */
    g_page[2048] = 0x22200513u;
    g_page[2049] = 0x00008067u;
    __asm__ volatile ("fence.i" ::: "memory");

    csr_write("satp", SATP_SV32 |
                      ((uint32_t)(uintptr_t)g_root >> 12));
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
    APLIC_TARGET(A) = 5u;               /* lower number is higher priority */
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
    APLIC_SETIPNUM = A;                 /* priority 5, not below 5 */
    check("aplic-threshold-blocks", APLIC_TOPI, 0u);
    APLIC_ITHRESHOLD = 6u;
    check("aplic-threshold-admits", APLIC_TOPI, (A << 16) | 5u);
    APLIC_ITHRESHOLD = 0u;              /* zero disables the filter */
    APLIC_CLRIPNUM = A;
    check("aplic-clripnum", APLIC_SETIP & (1u << A), 0u);

    /* Now the delivery path, end to end. */
    APLIC_IDELIVERY = 1u;
    csr_set("mie", 1u << 11);           /* MEIE */

    const uint32_t before = g_ext_count;
    APLIC_SETIPNUM = B;
    csr_set("mstatus", 1u << 3);        /* MIE: the trap fires here */
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
    APLIC_IDELIVERY = 0u;
    APLIC_DOMAINCFG = 0u;
}

/* ------------------------------------------------------------------ */

int main(void)
{
    puts_("rv32cortex-m ISA self-test\n");

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
    test_pmp();
    test_pmp_exec();
    test_smode();
    test_sv32();
    test_aplic();
#if defined(__riscv_zacas)
    test_zacas();
#endif
#if defined(__riscv_flen) && (__riscv_flen >= 32)
    test_fpu();
#endif
    test_timer_interrupt();

    puts_("checks   ");
    puthex((uint32_t)g_checks);
    puts_("\nfailures ");
    puthex((uint32_t)g_failures);
    putc_('\n');
    puts_(g_failures == 0 ? "PASS\n" : "FAIL\n");

    return g_failures;
}
