/* SPDX-License-Identifier: Apache-2.0 */
/*
 * intc.c - the MPC5675K's System Timer Module and interrupt controller,
 * through the core.
 *
 * tests/unit/test_ppc_soc.c holds the two devices to the manual register
 * by register. What only a running core can show is here: a compare
 * match taken as an external input at the instruction it falls on, the
 * software vector handshake (IACKR, EOIR) with a C ISR, one priority
 * preempting another and the LIFO giving the first one back, priority
 * masking, the hardware vector, and a `wait` that only the STM can end.
 *
 * **Every expectation is the Reference Manual's** (Rev. 10, docs/nxp),
 * with the section beside it. There is no MPC5675K attached, so none
 * was read back from the emulator.
 *
 * Runs interpreted and translated. A translated block looks for
 * interrupts between blocks, not after each store, so where an ISR
 * raises a priority the test waits for the nested one rather than
 * assuming it lands on the next instruction; and the latency it accepts
 * between a match and its ISR is a block's, not an instruction's.
 */

typedef unsigned int u32;

#define UART (*(volatile unsigned char *)0x10000000u)
#define REG(a) (*(volatile u32 *)(a))
#define REG8(a) (*(volatile unsigned char *)(a))

/* Table 51-2, STM_0. */
#define STM 0xFFF3C000u
#define STM_CR REG(STM + 0x00u)
#define STM_CNT REG(STM + 0x04u)
#define STM_CCR(n) REG(STM + 0x10u + 0x10u * (n))
#define STM_CIR(n) REG(STM + 0x14u + 0x10u * (n))
#define STM_CMP(n) REG(STM + 0x18u + 0x10u * (n))
#define CR_TEN 0x1u
#define CR_CPS(d) (((d) - 1u) << 8) /* divide by d */

/* Table 32-2, INTC_0. */
#define INTC 0xFFF48000u
#define INTC_BCR REG(INTC + 0x00u)
#define INTC_CPR REG(INTC + 0x08u)
#define INTC_IACKR REG(INTC + 0x10u)
#define INTC_SSCIR(n) REG8(INTC + 0x20u + (n))
#define INTC_PSR(n) REG8(INTC + 0x40u + (n))
#define BCR_HVEN 0x1u
#define SSCIR_SET 0x02u
#define SSCIR_CLR 0x01u

#define VTBA 0x80070000u
#define TCR_DIE 0x04000000u

/* Table 32-10: STM_0's four channels. */
#define VEC_STM0 30u

/* sys_stubs.S, for the faults */
struct trap {
    u32 ivor, srr0, srr1, esr, dear, msr, count, resume, cr, r6;
};
volatile struct trap g_trap;
volatile u32 g_uval;
u32 sys_install(void);
u32 stub_load(u32 a);
u32 stub_wait(void);

/* intc_stubs.S */
void install_ext_sw(void);
u32 rd_ivpr(void);
u32 wr_ivpr(u32 v);
u32 rd_tcr(void);
u32 wr_tcr(u32 v);
u32 ld_b(u32 a);
u32 ld_h(u32 a);
u32 st_h(u32 a, u32 v);
u32 stm_span(u32 n);
extern char hwvec_page[];

static void pc_(char c)
{
    UART = (unsigned char)c;
}

static void ps_(const char *s)
{
    while (*s) {
        pc_(*s++);
    }
}

static void phex(u32 v)
{
    int i;

    for (i = 28; i >= 0; i -= 4) {
        pc_("0123456789abcdef"[(v >> i) & 0xFu]);
    }
}

static u32 g_fail;
static u32 g_checks;

static void check(const char *name, u32 got, u32 want)
{
    g_checks++;
    if (got != want) {
        g_fail++;
        ps_("FAIL ");
        ps_(name);
        ps_(" got ");
        phex(got);
        ps_(" want ");
        phex(want);
        pc_('\n');
    }
}

/* lo <= got <= hi, printed either way: the figures are worth reading. */
static void within(const char *name, u32 got, u32 lo, u32 hi)
{
    g_checks++;
    if (got < lo || got > hi) {
        g_fail++;
        ps_("FAIL ");
    }
    ps_(name);
    ps_(" ");
    phex(got);
    pc_('\n');
}

/* ------------------------------------------------------------------ */
/* The ISRs                                                            */
/* ------------------------------------------------------------------ */

/* What they saw, in order: the vector, 0x8000 on the way out, and the
 * priority INTC_CPR held at that moment in the top half. */
#define LOG_MAX 32u
static volatile u32 g_log[LOG_MAX];
static volatile u32 g_nlog;
static volatile u32 g_irqs;
static volatile u32 g_iackr;
static volatile u32 g_cnt_at; /* STM_CNT as an STM ISR began */
static volatile u32 g_seen[8];
/* From inside the ISR for `g_chain_from`, set `g_chain_to`'s request. */
static volatile u32 g_chain_from = 0xFFu;
static volatile u32 g_chain_to;

static void note(u32 v)
{
    if (g_nlog < LOG_MAX) {
        g_log[g_nlog++] = v | (INTC_CPR << 16);
    }
}

/* The flag that drives the request, cleared: 32.4.3.1.2 says first. */
static void clear_source(u32 vec)
{
    if (vec >= VEC_STM0 && vec < VEC_STM0 + 4u) {
        STM_CIR(vec - VEC_STM0) = 1u;
    } else if (vec < 8u) {
        INTC_SSCIR(vec) = SSCIR_CLR;
    }
}

/* ext_sw's: called with what reading INTC_IACKR returned. */
void isr(u32 iackr)
{
    const u32 vec = (iackr >> 2) & 0x1FFu;

    if (vec >= VEC_STM0 && vec < VEC_STM0 + 4u) {
        g_cnt_at = STM_CNT;
    }
    g_iackr = iackr;
    g_irqs++;
    note(vec);
    if (vec == g_chain_from) {
        const u32 to = g_chain_to;

        INTC_SSCIR(to) = SSCIR_SET;
        /*
         * A higher priority preempts this ISR -- interrupts are enabled
         * in it, as Example 32-1 has them. Wait for it, a bounded while:
         * translated, it lands between blocks. A lower one must not.
         */
        for (u32 i = 0u; i < 3000u && g_seen[to] == 0u; i++) {
        }
    }
    if (vec < 8u) {
        g_seen[vec] = 1u;
    }
    clear_source(vec);
    note(vec | 0x8000u);
}

/* hwvec_page's: called with the vector the entry it ran from names. */
static volatile u32 g_hw_vec, g_hw_iackr, g_hw_cpr, g_hw_count;

void hw_isr(u32 vec)
{
    g_hw_vec = vec;
    g_hw_iackr = INTC_IACKR; /* no side effect in this mode, 32.3.2.3 */
    g_hw_cpr = INTC_CPR;
    g_hw_count++;
    clear_source(vec);
}

static void spin(u32 n)
{
    for (volatile u32 i = 0u; i < n; i++) {
    }
}

/* Spin until the count moves, a bounded while; false if it did not. */
static int until_irq(volatile u32 *count, u32 was)
{
    for (u32 i = 0u; i < 200000u; i++) {
        if (*count != was) {
            return 1;
        }
    }
    return 0;
}

static void reset_log(void)
{
    g_nlog = 0u;
    for (u32 i = 0u; i < 8u; i++) {
        g_seen[i] = 0u;
    }
}

int main(void)
{
    u32 tcr;
    u32 n;
    u32 cmp;

    (void)sys_install();
    install_ext_sw();
    /*
     * The decrementer crt0 runs would land in the measured spans and
     * wake the `wait` below by itself; off for the whole test, and the
     * IVOR it uses is crt0's either way.
     */
    tcr = rd_tcr();
    (void)wr_tcr(tcr & ~TCR_DIE);

    /* --- registers, through the core (51.3, 32.3) ----------------------- */
    check("stm-cr-reset", STM_CR, 0u);
    check("stm-cnt-reset", STM_CNT, 0u);
    check("intc-cpr-reset", INTC_CPR, 15u);
    check("stm-byte-is-a-bus-error", ld_b(STM + 3u), 2u);
    check("stm-byte-dear", g_trap.dear, STM + 3u);
    check("stm-half-is-a-bus-error", ld_h(STM + 6u), 2u);
    check("stm-store-half", st_h(STM + 4u, 1u), 2u);
    check("stm-reserved", stub_load(STM + 0x1Cu), 2u);
    check("intc-byte-ok", ld_b(INTC + 0x0Bu), 15u);
    check("intc-reserved", stub_load(INTC + 0x04u), 2u);
    check("stm-unmoved", STM_CNT, 0u);

    /* --- the counter is the processor clock, divided (51.4) ------------- */
    STM_CR = CR_TEN;
    check("span-1000-at-1", stm_span(1000u), 1002u);
    STM_CR = CR_CPS(10u) | CR_TEN;
    within("span-10000-at-10", stm_span(10000u), 1000u, 1001u);
    STM_CR = 0u;
    n = STM_CNT;
    spin(100u);
    check("stm-stops-without-ten", STM_CNT, n);

    /* --- a match, taken in software vector mode (32.2.1.1) -------------- */
    INTC_IACKR = VTBA;
    INTC_PSR(VEC_STM0) = 5u;
    INTC_CPR = 0u;
    STM_CR = CR_TEN;
    n = g_irqs;
    cmp = STM_CNT + 2000u;
    STM_CMP(0) = cmp;
    STM_CCR(0) = 1u;
    check("match-taken", until_irq(&g_irqs, n), 1u);
    check("match-once", g_irqs, n + 1u);
    check("match-iackr", g_iackr, VTBA | (VEC_STM0 << 2));
    check("match-cpr-in-isr", g_log[0] >> 16, 5u);
    within("match-latency", g_cnt_at - cmp, 0u, 400u);
    check("match-cleared", STM_CIR(0), 0u);
    check("match-cpr-after", INTC_CPR, 0u);
    spin(3000u);
    check("match-not-again", g_irqs, n + 1u);
    STM_CCR(0) = 0u;

    /* --- a higher priority preempts, and the LIFO restores (32.4.2) ----- */
    INTC_PSR(1) = 3u;
    INTC_PSR(2) = 7u;
    reset_log();
    g_chain_from = 1u;
    g_chain_to = 2u;
    INTC_SSCIR(1) = SSCIR_SET;
    (void)until_irq(&g_nlog, 0u);
    spin(2000u);
    check("nest-count", g_nlog, 4u);
    check("nest-1-in", g_log[0], 1u | (3u << 16));
    check("nest-2-in", g_log[1], 2u | (7u << 16));
    check("nest-2-out", g_log[2], 2u | 0x8000u | (7u << 16));
    check("nest-1-out", g_log[3], 1u | 0x8000u | (3u << 16));
    check("nest-cpr-after", INTC_CPR, 0u);

    /* ...and a lower one waits for the ISR it would have interrupted. */
    reset_log();
    g_chain_from = 2u;
    g_chain_to = 1u;
    INTC_SSCIR(2) = SSCIR_SET;
    (void)until_irq(&g_nlog, 0u);
    spin(2000u);
    check("defer-count", g_nlog, 4u);
    check("defer-2-in", g_log[0], 2u | (7u << 16));
    check("defer-2-out", g_log[1], 2u | 0x8000u | (7u << 16));
    check("defer-1-in", g_log[2], 1u | (3u << 16));
    check("defer-1-out", g_log[3], 1u | 0x8000u | (3u << 16));
    g_chain_from = 0xFFu;

    /* --- the current priority masks; equal priorities by vector --------- */
    INTC_PSR(3) = 5u;
    INTC_PSR(4) = 5u;
    INTC_CPR = 7u; /* above both: 32.3.2.2 */
    reset_log();
    INTC_SSCIR(4) = SSCIR_SET;
    INTC_SSCIR(3) = SSCIR_SET;
    spin(2000u);
    check("masked", g_nlog, 0u);
    INTC_CPR = 5u; /* "same or lower" is masked too */
    spin(2000u);
    check("masked-at-equal", g_nlog, 0u);
    INTC_CPR = 0u;
    (void)until_irq(&g_nlog, 0u);
    spin(2000u);
    check("ties-count", g_nlog, 4u);
    check("ties-lower-vector-first", g_log[0] & 0xFFFFu, 3u);
    check("ties-then", g_log[2] & 0xFFFFu, 4u);

    /* --- hardware vector mode (32.2.1.2) -------------------------------- */
    {
        const u32 ivpr = rd_ivpr();
        const u32 sw_irqs = g_irqs;

        INTC_PSR(VEC_STM0 + 3u) = 6u;
        INTC_BCR = BCR_HVEN;
        (void)wr_ivpr((u32)hwvec_page);
        n = g_hw_count;
        STM_CMP(3) = STM_CNT + 500u;
        STM_CCR(3) = 1u;
        check("hw-taken", until_irq(&g_hw_count, n), 1u);
        check("hw-entry", g_hw_vec, VEC_STM0 + 3u);
        check("hw-intvec", (g_hw_iackr >> 2) & 0x1FFu, VEC_STM0 + 3u);
        check("hw-cpr-installed-by-the-take", g_hw_cpr, 6u);
        STM_CCR(3) = 0u;

        INTC_PSR(5) = 2u;
        n = g_hw_count;
        INTC_SSCIR(5) = SSCIR_SET;
        check("hw-software-taken", until_irq(&g_hw_count, n), 1u);
        check("hw-software-entry", g_hw_vec, 5u);
        check("hw-not-through-ivor4", g_irqs, sw_irqs); /* ext_sw never ran */

        (void)wr_ivpr(ivpr);
        INTC_BCR = 0u;
        check("hw-cpr-after", INTC_CPR, 0u);
    }

    /* --- a wait that only the STM can end ------------------------------- */
    INTC_PSR(VEC_STM0 + 1u) = 4u;
    n = g_irqs;
    cmp = STM_CNT + 100000u;
    STM_CMP(1) = cmp;
    STM_CCR(1) = 1u;
    (void)stub_wait();
    check("wait-woken", g_irqs, n + 1u);
    check("wait-vector", (g_iackr >> 2) & 0x1FFu, VEC_STM0 + 1u);
    within("wait-latency", g_cnt_at - cmp, 0u, 400u);
    STM_CCR(1) = 0u;

    STM_CR = 0u;
    INTC_CPR = 15u;
    (void)wr_tcr(tcr);

    ps_("PPC-INTC-END checks=");
    phex(g_checks);
    ps_(" fails=");
    phex(g_fail);
    pc_('\n');
    return (int)g_fail;
}
