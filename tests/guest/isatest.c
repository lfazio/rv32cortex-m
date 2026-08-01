/* SPDX-License-Identifier: Apache-2.0 */
/*
 * isatest.c - RV32IMAC self-test, run as a guest inside the emulator.
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
/* Trap handling                                                       */
/* ------------------------------------------------------------------ */

static volatile uint32_t g_trap_count;
static volatile uint32_t g_last_cause;
static volatile uint32_t g_last_tval;

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
        /* Interrupt. Disarm the timer; mepc must not move. */
        if ((cause & 0xFFu) == 7u) {
            CLINT_MTIMECMP_HI = 0xFFFFFFFFu;
            CLINT_MTIMECMP_LO = 0xFFFFFFFFu;
        }
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
