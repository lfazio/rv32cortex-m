/* SPDX-License-Identifier: Apache-2.0 */
/*
 * swt.c - the MPC5675K's Software Watchdog Timer, through the core.
 *
 * tests/unit/test_ppc_soc.c holds the SWT to chapter 52 register by
 * register. Here it runs against the core's clock: serviced in time it
 * does nothing; left alone with SWT_CR[ITR] set it interrupts through
 * INTC_0 as source 28 (table 32-10); and the last thing this guest does
 * is let it expire for good, so **a pass is the run ending in a system
 * reset** -- the terminator line, then the emulator's report of the
 * reset. A guest that finished normally would be the failure.
 *
 * Every expectation is the Reference Manual's (Rev. 10, docs/nxp), or
 * one of the choices ppc_swt.c states.
 */

typedef unsigned int u32;

#define UART (*(volatile unsigned char *)0x10000000u)
#define REG(a) (*(volatile u32 *)(a))
#define REG8(a) (*(volatile unsigned char *)(a))

/* Table 52-2, SWT_0. */
#define SWT 0xFFF38000u
#define SWT_CR REG(SWT + 0x00u)
#define SWT_IR REG(SWT + 0x04u)
#define SWT_TO REG(SWT + 0x08u)
#define SWT_SR REG(SWT + 0x10u)
#define SWT_CO REG(SWT + 0x14u)
#define CR_MAP 0xFF000000u
#define CR_ITR 0x40u
#define CR_SLK 0x10u
#define CR_WEN 0x01u

/* The STM counts the same clock: the ruler for the timeout's latency. */
#define STM_CR REG(0xFFF3C000u)
#define STM_CNT REG(0xFFF3C004u)

#define INTC 0xFFF48000u
#define INTC_CPR REG(INTC + 0x08u)
#define INTC_PSR(n) REG8(INTC + 0x40u + (n))

#define VEC_SWT0 28u
#define TCR_DIE 0x04000000u

/* sys_stubs.S, for the faults */
struct trap {
    u32 ivor, srr0, srr1, esr, dear, msr, count, resume, cr, r6;
};
volatile struct trap g_trap;
volatile u32 g_uval;
u32 sys_install(void);
u32 stub_store(u32 a, u32 v);
u32 stub_wait(void);

/* intc_stubs.S */
void install_ext_sw(void);
u32 rd_tcr(void);
u32 wr_tcr(u32 v);
u32 ld_b(u32 a);

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
    for (int i = 28; i >= 0; i -= 4) {
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

static volatile u32 g_irqs;
static volatile u32 g_t_isr;
static volatile u32 g_vec;
static volatile u32 g_service_in_isr;

static void service(void)
{
    SWT_SR = 0xA602u; /* table 52-7 */
    SWT_SR = 0xB480u;
}

/* ext_sw's (intc_stubs.S). */
void isr(u32 iackr)
{
    g_t_isr = STM_CNT;
    g_vec = (iackr >> 2) & 0x1FFu;
    g_irqs++;
    SWT_IR = 1u; /* TIF, write one to clear */
    if (g_service_in_isr) {
        service();
    }
}

void hw_isr(u32 vec)
{
    (void)vec;
}

/* lo <= got <= hi, printed either way. */
static void within(const char *name, u32 got, u32 lo, u32 hi)
{
    g_checks++;
    if (got < lo || got > hi) {
        g_fail++;
        ps_("FAIL ");
    }
    ps_(name);
    pc_(' ');
    phex(got);
    pc_('\n');
}

static void spin(u32 n)
{
    for (volatile u32 i = 0u; i < n; i++) {
    }
}

int main(void)
{
    u32 n;
    u32 t0;

    (void)sys_install();
    install_ext_sw();
    (void)wr_tcr(rd_tcr() & ~TCR_DIE);

    /* --- out of reset (table 52-2, WEN clear) --------------------------- */
    check("cr-reset", SWT_CR, 0xFF00011Au);
    check("to-reset", SWT_TO, 0x0003FDE0u);
    check("byte-is-a-bus-error", ld_b(SWT + 3u), 2u);
    /* Soft-locked: a write to TO is invalid -- a bus error, WEN clear. */
    check("locked-write-faults", stub_store(SWT + 0x08u, 1234u), 2u);
    check("locked-to-unchanged", SWT_TO, 0x0003FDE0u);
    SWT_SR = 0xC520u;
    SWT_SR = 0xD928u;
    check("unlocked", SWT_CR & CR_SLK, 0u);

    /* --- serviced in time, nothing happens ------------------------------ */
    INTC_PSR(VEC_SWT0) = 3u;
    INTC_CPR = 0u;
    SWT_TO = 3000u;
    SWT_CR = CR_MAP | CR_ITR | CR_WEN;
    for (u32 i = 0u; i < 30u; i++) {
        spin(200u); /* a few hundred instructions each time round */
        service();
    }
    check("serviced-no-irq", g_irqs, 0u);
    check("serviced-no-tif", SWT_IR, 0u);

    /* --- left alone, ITR: the initial timeout interrupts (52.4) -------- */
    g_service_in_isr = 1u;
    STM_CR = 1u; /* TEN, dividing by one */
    service();
    t0 = STM_CNT;
    n = g_irqs;
    for (u32 i = 0u; i < 100000u && g_irqs == n; i++) {
    }
    service(); /* the ISR did too; this keeps the next one away */
    check("timeout-interrupted", g_irqs, n + 1u);
    /*
     * 3000 ticks after the service, plus the handler's way in. A
     * service that reloads the counter without ending the stretch was
     * noticed a quantum late, and this is where that shows.
     */
    within("timeout-latency", g_t_isr - t0, 3000u, 3400u);
    check("timeout-vector", g_vec, VEC_SWT0);
    check("tif-cleared", SWT_IR, 0u);

    /* --- a wait that only the watchdog's interrupt can end ------------- */
    n = g_irqs;
    (void)stub_wait();
    check("wait-woken-by-swt", g_irqs, n + 1u);

    /* --- disabled, CO shows the counter (52.3.2.6) ---------------------- */
    SWT_CR = CR_MAP | CR_ITR;
    n = SWT_CO;
    check("co-below-to", n != 0u && n <= 3000u, 1u);
    spin(100u);
    check("co-stopped", SWT_CO, n);

    ps_("PPC-SWT-END checks=");
    phex(g_checks);
    ps_(" fails=");
    phex(g_fail);
    pc_('\n');

    /*
     * --- and the reset: no ITR, nobody servicing. The line above is the
     * last this guest prints; the emulator reports the reset after it.
     */
    SWT_TO = 2000u;
    SWT_CR = CR_MAP | CR_WEN;
    spin(100000u);
    ps_("FAIL still running after the watchdog's timeout\n");
    return 1;
}
