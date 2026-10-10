/* SPDX-License-Identifier: Apache-2.0 */
/*
 * sys.c - the ARMv7-M exception model, MPU and System Control Space,
 * against a real Cortex-M7.
 *
 * The instruction cases in cases.S can only see what one instruction
 * does to registers and memory. Everything here is about *when* code
 * runs -- which exception preempts which, what a mask defers, where a
 * fault lands and what it says -- so each test logs handler entries
 * (0x1nn) and exits (0x2nn) with the values it reads, and prints one
 * line. The same program runs on the board and as an emulator guest,
 * and tests/armv7m-diff/run.py --suite sys compares the lines.
 *
 * **Nothing printed may depend on where the code is.** The two memory
 * maps differ, so a stacked pc is printed relative to the label of the
 * instruction that faulted, an MMFAR relative to the buffer it names,
 * and a frame address relative to the stack it was pushed on.
 */

#include "diff.h"

#define REG(a) (*(volatile uint32_t *)(a))
#define REG8(a) (*(volatile uint8_t *)(a))

#define ICTR REG(0xE000E004u)
#define SYST_CSR REG(0xE000E010u)
#define SYST_RVR REG(0xE000E014u)
#define SYST_CVR REG(0xE000E018u)
#define SYST_CALIB REG(0xE000E01Cu)
#define NVIC_ISER(n) REG(0xE000E100u + 4u * (n))
#define NVIC_ICER(n) REG(0xE000E180u + 4u * (n))
#define NVIC_ISPR(n) REG(0xE000E200u + 4u * (n))
#define NVIC_ICPR(n) REG(0xE000E280u + 4u * (n))
#define NVIC_IABR(n) REG(0xE000E300u + 4u * (n))
#define NVIC_IPR8(n) REG8(0xE000E400u + (n))
#define NVIC_IPR(n) REG(0xE000E400u + 4u * (n))
#define CPUID REG(0xE000ED00u)
#define ICSR REG(0xE000ED04u)
#define VTOR REG(0xE000ED08u)
#define AIRCR REG(0xE000ED0Cu)
#define SCR REG(0xE000ED10u)
#define CCR REG(0xE000ED14u)
#define SHPR8(n) REG8(0xE000ED14u + (n)) /* n = exception number */
#define SHPR1 REG(0xE000ED18u)
#define SHPR2 REG(0xE000ED1Cu)
#define SHPR3 REG(0xE000ED20u)
#define SHCSR REG(0xE000ED24u)
#define CFSR REG(0xE000ED28u)
#define HFSR REG(0xE000ED2Cu)
#define MMFAR REG(0xE000ED34u)
#define BFAR REG(0xE000ED38u)
#define CPACR REG(0xE000ED88u)
#define MPU_TYPE REG(0xE000ED90u)
#define MPU_CTRL REG(0xE000ED94u)
#define MPU_RNR REG(0xE000ED98u)
#define MPU_RBAR REG(0xE000ED9Cu)
#define MPU_RASR REG(0xE000EDA0u)
#define STIR REG(0xE000EF00u)
#define FPCCR REG(0xE000EF34u)
#define MVFR0 REG(0xE000EF40u)
#define MVFR1 REG(0xE000EF44u)
#define MVFR2 REG(0xE000EF48u)

#define barrier() __asm__ volatile("dsb\n isb" ::: "memory")

#define MRS(reg)                                                               \
    ({                                                                         \
        uint32_t v_;                                                           \
        __asm__ volatile("mrs %0, " #reg : "=r"(v_));                          \
        v_;                                                                    \
    })
#define MSR(reg, v) __asm__ volatile("msr " #reg ", %0" ::"r"(v) : "memory")

/* ---- the assembly pieces ---- */
extern void sys_exc_entry(void);
extern void sys_do_udf(void);
extern char sys_after_udf[];
extern void sys_do_svc(void);
extern char sys_after_svc[];
extern uint32_t sys_do_load(uint32_t addr);
extern char sys_load_insn[];
extern char sys_after_load[];
extern void sys_do_store(uint32_t addr, uint32_t v);
extern char sys_store_insn[];
extern char sys_after_store[];
extern void sys_do_ldrd(uint32_t addr);
extern char sys_ldrd_insn[];
extern char sys_after_ldrd[];
extern int32_t sys_do_sdiv(int32_t a, int32_t b);
extern char sys_sdiv_insn[];
extern char sys_after_sdiv[];
extern void sys_do_bx(uint32_t target);
extern uint32_t sys_excl_across_svc(volatile uint32_t *p);
extern void sys_thread_on_psp(void (*fn)(void), uint32_t npriv,
                              uint32_t *stack_top);

/* The process stack: its own memory, or handler frames land on it. */
static uint32_t g_pstack[512] __attribute__((aligned(8)));
#define PSTACK_TOP (g_pstack + sizeof(g_pstack) / sizeof(g_pstack[0]))
extern uint32_t sys_read_sp(void);
extern uint32_t sys_pendsv_misaligned(uint32_t icsr_addr);

/* ---- output ---- */

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

/* ---- the log ---- */

#define LOGN 96u
static volatile uint32_t g_log[LOGN];
static volatile uint32_t g_nlog;

static void logv(uint32_t v)
{
    if (g_nlog < LOGN) {
        g_log[g_nlog++] = v;
    }
}

/* "t name v v v ...", then clear the log for the next test. */
static void emit(const char *name)
{
    puts_("t ");
    puts_(name);
    for (uint32_t i = 0u; i < g_nlog; i++) {
        diff_putc(' ');
        putx(g_log[i]);
    }
    diff_putc('\n');
    g_nlog = 0u;
}

/* ---- the handlers ---- */

typedef void (*hook_fn)(uint32_t *frame, uint32_t excret, uint32_t exc);
static volatile hook_fn g_hook;
static volatile uint32_t g_phase;

/* Where a fault handler resumes, set by the test before provoking it. */
static volatile uint32_t g_resume;

void sys_handler(uint32_t *frame, uint32_t excret, uint32_t exc);
void sys_handler(uint32_t *frame, uint32_t excret, uint32_t exc)
{
    logv(0x100u | exc);
    if (g_hook != 0) {
        g_hook(frame, excret, exc);
    }
    logv(0x200u | exc);
}

static void (*g_vtab[16 + 32])(void) __attribute__((aligned(512)));

static void irq_prio(uint32_t irq, uint32_t p)
{
    NVIC_IPR8(irq) = (uint8_t)p;
}

static void irq_pend(uint32_t irq)
{
    NVIC_ISPR(irq >> 5) = 1u << (irq & 31u);
    barrier();
}

static void irqs_reset(void)
{
    NVIC_ICER(0) = 0xFFFFFFFFu;
    NVIC_ICPR(0) = 0xFFFFFFFFu;
    for (uint32_t i = 0u; i < 8u; i++) {
        NVIC_IPR(i) = 0u;
    }
    barrier();
}

/* A fault's status, printed and then cleared -- they are sticky. */
static void log_fault(uint32_t *frame, uint32_t insn)
{
    logv(CFSR);
    logv(HFSR);
    logv(frame[6] - (insn & ~1u));
    CFSR = CFSR;
    HFSR = HFSR;
    frame[6] = g_resume & ~1u;
    frame[7] |= 1u << 24; /* resume in Thumb state, whatever faulted */
}

/* ------------------------------------------------------------------ */

/*
 * The identification registers and which bits of the configuration
 * registers are writable -- every one of them an implementation choice
 * the emulator has to make the board's way.
 */
static void t_ids(void)
{
    uint32_t save;

    logv(CPUID);
    logv(ICTR);
    logv(MPU_TYPE);
    logv(MVFR0);
    logv(MVFR1);
    logv(MVFR2);
    logv(CCR);
    logv(AIRCR);
    logv(FPCCR);
    logv(SHPR1);
    logv(SHPR2);
    logv(SHPR3);
    logv(ICSR);
    logv(SHCSR);
    emit("ids");

    NVIC_IPR(0) = 0xFFFFFFFFu;
    logv(NVIC_IPR(0));
    NVIC_IPR(0) = 0u;
    SHPR3 = 0xFFFFFFFFu;
    logv(SHPR3);
    SHPR3 = 0u;
    SHPR1 = 0xFFFFFFFFu;
    logv(SHPR1);
    SHPR1 = 0u;
    MPU_RNR = 0xFFu;
    logv(MPU_RNR);
    MPU_RNR = 0u;
    save = CCR;
    CCR = 0u;
    logv(CCR);
    CCR = 0xFFFFFFFFu;
    logv(CCR);
    CCR = save;
    NVIC_ISER(3) = 0xFFFFFFFFu;
    logv(NVIC_ISER(3));
    NVIC_ICER(3) = 0xFFFFFFFFu;
    AIRCR = 0x05FA0700u;
    logv(AIRCR);
    AIRCR = 0x05FA0000u;
    SCR = 0xFFu;
    logv(SCR);
    SCR = 0u;
    save = FPCCR;
    FPCCR = 0u;
    logv(FPCCR);
    FPCCR = save;
    CPACR = 0xFFFFFFFFu;
    logv(CPACR);
    CPACR = 0x00F00000u;
    barrier();
    emit("writable");
}

/* ------------------------------------------------------------------ */

static void hook_order(uint32_t *frame, uint32_t excret, uint32_t exc)
{
    (void)frame;
    (void)excret;
    if (g_phase == 0u && exc == 19u) {
        irq_pend(1u); /* higher priority: preempts this handler */
    }
    if (g_phase == 1u && exc == 16u) {
        irq_pend(2u); /* equal priority: waits */
        irq_pend(3u); /* lower: waits */
    }
}

/*
 * Priority order, preemption by a higher priority, and no preemption by
 * an equal or lower one. Equal priorities are taken lowest number first.
 */
static void t_order(void)
{
    irqs_reset();
    irq_prio(0u, 0x80u);
    irq_prio(1u, 0x40u);
    irq_prio(2u, 0x80u);
    irq_prio(3u, 0xC0u);
    g_hook = hook_order;

    g_phase = 0u;
    MSR(primask, 1u);
    irq_pend(3u);
    irq_pend(0u);
    irq_pend(2u);
    NVIC_ISER(0) = 0xFu;
    barrier();
    MSR(primask, 0u);
    barrier();
    emit("order");

    g_phase = 1u;
    irq_pend(0u);
    emit("nopreempt");

    g_hook = 0;
    irqs_reset();
}

/* ------------------------------------------------------------------ */

static void hook_group(uint32_t *frame, uint32_t excret, uint32_t exc)
{
    (void)frame;
    (void)excret;
    if (exc == 16u) {
        irq_pend(1u);
        logv(ICSR & 0x1FFFFFu);
    }
}

/*
 * PRIGROUP: with the group in bits 7:6, priorities 0x50 and 0x40 are the
 * same group and do not preempt each other; with PRIGROUP 0 they do.
 */
static void t_prigroup(void)
{
    irqs_reset();
    irq_prio(0u, 0x50u);
    irq_prio(1u, 0x40u);
    NVIC_ISER(0) = 3u;
    g_hook = hook_group;

    AIRCR = 0x05FA0500u;
    irq_pend(0u);
    emit("group5");
    AIRCR = 0x05FA0000u;
    irq_pend(0u);
    emit("group0");

    /* Both pending at once: the lower value first, even within a group. */
    AIRCR = 0x05FA0500u;
    g_hook = 0;
    MSR(primask, 1u);
    irq_pend(0u);
    irq_pend(1u);
    MSR(primask, 0u);
    barrier();
    AIRCR = 0x05FA0000u;
    emit("subprio");
    irqs_reset();
}

/* ------------------------------------------------------------------ */

static void t_basepri(void)
{
    irqs_reset();
    irq_prio(0u, 0x80u);
    irq_prio(1u, 0x40u);
    NVIC_ISER(0) = 3u;

    MSR(basepri, 0x80u);
    irq_pend(0u); /* masked: priority not above BASEPRI */
    logv(ICSR & 0x7FF000u);
    irq_pend(1u); /* taken */
    MSR(basepri_max, 0xC0u);
    logv(MRS(basepri));
    MSR(basepri_max, 0x60u);
    logv(MRS(basepri));
    MSR(basepri_max, 0u);
    logv(MRS(basepri));
    MSR(basepri, 0x13u); /* low bits are not implemented */
    logv(MRS(basepri));
    MSR(basepri, 0u);
    barrier();
    emit("basepri");

    /*
     * The board keeps BASEPRI's low bits (0x13 reads back 0x13) although
     * the priority registers drop them. Does 0x13 then mask priority
     * 0x10? With the low bits dropped it would; kept, it would not.
     */
    irq_prio(0u, 0x10u);
    MSR(basepri, 0x13u);
    irq_pend(0u);
    logv(ICSR & 0x7FF000u);
    MSR(basepri, 0x10u);
    irq_pend(0u);
    logv(ICSR & 0x7FF000u);
    MSR(basepri, 0u);
    barrier();
    emit("basepri-low");
    irqs_reset();
}

/* ------------------------------------------------------------------ */

static void hook_fm(uint32_t *frame, uint32_t excret, uint32_t exc)
{
    (void)frame;
    (void)excret;
    if (exc == 16u) {
        __asm__ volatile("cpsid f" ::: "memory");
        logv(MRS(faultmask));
    }
}

static void t_faultmask(void)
{
    irqs_reset();
    irq_prio(0u, 0x80u);
    irq_prio(1u, 0x40u);
    NVIC_ISER(0) = 3u;

    __asm__ volatile("cpsid f" ::: "memory");
    logv(MRS(faultmask));
    irq_pend(1u); /* deferred */
    logv(ICSR & 0x7FF000u);
    __asm__ volatile("cpsie f" ::: "memory");
    barrier();
    logv(MRS(faultmask));

    /* Set inside a handler, cleared by the return from it. */
    g_hook = hook_fm;
    irq_pend(0u);
    logv(MRS(faultmask));
    g_hook = 0;
    emit("faultmask");

    /* NMI is not masked by PRIMASK or FAULTMASK. */
    MSR(primask, 1u);
    __asm__ volatile("cpsid f" ::: "memory");
    ICSR = 1u << 31;
    barrier();
    __asm__ volatile("cpsie f" ::: "memory");
    MSR(primask, 0u);
    emit("nmi");
    irqs_reset();
}

/* ------------------------------------------------------------------ */

static void hook_svc(uint32_t *frame, uint32_t excret, uint32_t exc)
{
    if (exc == 11u) {
        const uint16_t insn = *(const uint16_t *)(frame[6] - 2u);

        logv(insn & 0xFFu);
        logv(excret);
        logv(frame[7] & 0x1FFu);
        if ((insn & 0xFFu) == 0x42u) {
            logv(frame[6] - ((uint32_t)sys_after_svc & ~1u));
            logv(SHCSR & 0x80u);
        }
        if ((insn & 0xFFu) == 0x44u) {
            /* Back to privileged: CONTROL.nPRIV, from Handler mode. */
            MSR(control, MRS(control) & ~1u);
        }
    }
    if (exc == 17u && g_phase == 7u) {
        g_resume = (uint32_t)sys_after_svc;
        sys_do_svc(); /* SVCall cannot preempt this: HardFault */
    }
    if (exc == 3u) {
        log_fault(frame, (uint32_t)sys_do_svc);
    }
}

static void t_svc(void)
{
    irqs_reset();
    g_hook = hook_svc;
    SHPR8(11) = 0x80u;
    sys_do_svc();
    emit("svc");

    /* An SVC that cannot preempt escalates to HardFault. */
    irq_prio(1u, 0x40u);
    NVIC_ISER(0) = 2u;
    g_phase = 7u;
    irq_pend(1u);
    g_phase = 0u;
    emit("svc-escalate");
    SHPR8(11) = 0u;
    g_hook = 0;
    irqs_reset();
}

/* ------------------------------------------------------------------ */

static void hook_fault(uint32_t *frame, uint32_t excret, uint32_t exc)
{
    (void)excret;
    if (exc == 3u || exc == 4u || exc == 5u || exc == 6u) {
        log_fault(frame, g_phase);
    }
}

static uint32_t g_words[4] __attribute__((aligned(8)));

static void t_usage(void)
{
    g_hook = hook_fault;
    SHCSR |= 1u << 18; /* USGFAULTENA */

    g_phase = (uint32_t)sys_do_udf;
    g_resume = (uint32_t)sys_after_udf;
    sys_do_udf();
    emit("udf");

    CCR |= 1u << 4; /* DIV_0_TRP */
    g_phase = (uint32_t)sys_sdiv_insn;
    g_resume = (uint32_t)sys_after_sdiv;
    (void)sys_do_sdiv(7, 0);
    CCR &= ~(1u << 4);
    logv((uint32_t)sys_do_sdiv(7, 0)); /* and without the trap: zero */
    emit("div0");

    CCR |= 1u << 3; /* UNALIGN_TRP */
    g_phase = (uint32_t)sys_load_insn;
    g_resume = (uint32_t)sys_after_load;
    (void)sys_do_load((uint32_t)g_words + 1u);
    CCR &= ~(1u << 3);
    emit("unalign-trap");

    g_phase = (uint32_t)sys_ldrd_insn;
    g_resume = (uint32_t)sys_after_ldrd;
    sys_do_ldrd((uint32_t)g_words + 2u); /* LDRD needs word alignment */
    emit("ldrd-unaligned");

    /* A branch with bit 0 clear: INVSTATE at the target. */
    g_phase = (uint32_t)sys_after_udf;
    g_resume = (uint32_t)sys_after_udf;
    sys_do_bx((uint32_t)sys_after_udf & ~1u);
    emit("invstate");

    /* Disabled, a UsageFault is a HardFault, FORCED. */
    SHCSR &= ~(1u << 18);
    g_phase = (uint32_t)sys_do_udf;
    g_resume = (uint32_t)sys_after_udf;
    sys_do_udf();
    emit("udf-forced");
    g_hook = 0;
}

/* ------------------------------------------------------------------ */

static void hook_bus(uint32_t *frame, uint32_t excret, uint32_t exc)
{
    (void)excret;
    if (exc == 5u) {
        logv(BFAR - g_phase);
        log_fault(frame, (uint32_t)sys_load_insn);
    }
    if (exc == 4u) {
        /* A fetch fault: resume where the call would have returned. */
        logv(CFSR);
        logv(frame[6]);
        CFSR = CFSR;
        frame[6] = frame[5] & ~1u;
    }
}

static void t_bus(void)
{
    g_hook = hook_bus;
    SHCSR |= (1u << 17) | (1u << 16);

    /* Reserved peripheral space: a precise BusFault on a load. */
    g_phase = 0x5FFFFFF0u;
    g_resume = (uint32_t)sys_after_load;
    (void)sys_do_load(0x5FFFFFF0u);
    emit("busfault");

    /* The default map's execute-never: a fetch from the peripherals. */
    ((void (*)(void))0x40000001u)();
    emit("xn-default");

    SHCSR &= ~((1u << 17) | (1u << 16));
    g_hook = 0;
}

/* ------------------------------------------------------------------ */

static uint8_t g_mpubuf[1024] __attribute__((aligned(1024), section(".sram1")));

static void hook_mpu(uint32_t *frame, uint32_t excret, uint32_t exc)
{
    (void)excret;
    if (exc == 4u) {
        const uint32_t cfsr = CFSR;

        logv(cfsr);
        logv((cfsr & 0x80u) ? MMFAR - (uint32_t)g_mpubuf : 0xFFFFFFFFu);
        if ((cfsr & 1u) != 0u) { /* IACCVIOL: back to the caller */
            logv(frame[6] - (uint32_t)g_mpubuf);
            frame[6] = frame[5] & ~1u;
        } else {
            logv(frame[6] - ((uint32_t)sys_store_insn & ~1u));
            frame[6] = (uint32_t)sys_after_store & ~1u;
        }
        CFSR = cfsr;
    }
    if (exc == 11u) {
        const uint16_t insn = *(const uint16_t *)(frame[6] - 2u);

        if ((insn & 0xFFu) == 0x44u) {
            MSR(control, MRS(control) & ~1u);
        }
    }
}

static void mpu_region(uint32_t n, uint32_t base, uint32_t sizelog2,
                       uint32_t ap, uint32_t xn, uint32_t srd)
{
    MPU_RNR = n;
    MPU_RBAR = base;
    MPU_RASR = (xn << 28) | (ap << 24) | (srd << 8) | ((sizelog2 - 1u) << 1) | 1u;
}

static void unpriv_body(void)
{
    logv(sys_do_load((uint32_t)g_mpubuf) & 0xFFu); /* read: allowed */
    sys_do_store((uint32_t)g_mpubuf, 1u);          /* write: refused */
}

static void t_mpu(void)
{
    const uint32_t b = (uint32_t)g_mpubuf;

    g_hook = hook_mpu;
    SHCSR |= 1u << 16;
    for (uint32_t i = 0u; i < sizeof(g_mpubuf); i++) {
        g_mpubuf[i] = (uint8_t)i;
    }
    *(volatile uint16_t *)(g_mpubuf + 64) = 0x4770u; /* bx lr */
    barrier();

    /* Region 0: everything, read-write, executable -- the background. */
    mpu_region(0u, 0u, 32u, 3u, 0u, 0u);
    /* Region 1: the buffer, read-only for everyone, never executable. */
    mpu_region(1u, b, 10u, 6u, 1u, 0u);
    MPU_CTRL = 1u; /* no PRIVDEFENA: region 0 is the map */
    barrier();
    logv(sys_do_load(b + 8u));
    sys_do_store(b + 8u, 0u);
    ((void (*)(void))(b + 64u + 1u))();
    emit("mpu-ro-xn");

    /* Region 2, higher numbered, overrides: its first 256 bytes RW. */
    mpu_region(2u, b, 8u, 3u, 0u, 0u);
    barrier();
    sys_do_store(b + 8u, 0u);   /* region 2: allowed */
    sys_do_store(b + 512u, 0u); /* region 1 only: refused */
    emit("mpu-overlap");

    /* Subregions: region 2 over the whole buffer, eighth 0 disabled. */
    mpu_region(2u, b, 10u, 3u, 0u, 0x01u);
    barrier();
    sys_do_store(b + 8u, 0u);   /* subregion 0 falls through to region 1 */
    sys_do_store(b + 136u, 0u); /* subregion 1: allowed */
    emit("mpu-srd");

    /* Privileged read-write, unprivileged read-only. */
    MPU_RNR = 2u;
    MPU_RASR = 0u;
    mpu_region(1u, b, 10u, 2u, 1u, 0u);
    barrier();
    sys_do_store(b + 8u, 0u); /* privileged: allowed */
    sys_thread_on_psp(unpriv_body, 1u, PSTACK_TOP);
    emit("mpu-unpriv");

    MPU_CTRL = 0u;
    for (uint32_t i = 0u; i < 3u; i++) {
        MPU_RNR = i;
        MPU_RASR = 0u;
    }
    barrier();
    SHCSR &= ~(1u << 16);
    g_hook = 0;
}

/* ------------------------------------------------------------------ */

static volatile uint32_t g_psp_seen;

static void hook_psp(uint32_t *frame, uint32_t excret, uint32_t exc)
{
    if (exc == 14u) {
        logv(excret);
        logv(g_psp_seen - (uint32_t)frame);
        logv(MRS(psp) == (uint32_t)frame);
        logv(sys_read_sp() != (uint32_t)frame);
    }
    if (exc == 11u) {
        const uint16_t insn = *(const uint16_t *)(frame[6] - 2u);

        if ((insn & 0xFFu) == 0x44u) {
            MSR(control, MRS(control) & ~1u);
        }
    }
    if (exc == 5u) {
        logv(CFSR);
        CFSR = CFSR;
        frame[6] = (uint32_t)sys_after_load & ~1u;
    }
}

static void psp_body(void)
{
    const uint32_t sp = sys_read_sp();

    logv(MRS(psp) == sp);
    logv(MRS(msp) != sp);
    logv(MRS(control));
    g_psp_seen = sp;
    ICSR = 1u << 28; /* PendSV */
    barrier();
}

static void unpriv_regs(void)
{
    logv(MRS(control));
    MSR(primask, 1u); /* ignored */
    logv(MRS(primask));
    (void)sys_do_load(0xE000ED00u); /* CPUID: unprivileged, a BusFault */
}

static void t_psp(void)
{
    g_hook = hook_psp;
    SHCSR |= 1u << 17;
    sys_thread_on_psp(psp_body, 0u, PSTACK_TOP);
    logv(MRS(control));
    emit("psp");

    sys_thread_on_psp(unpriv_regs, 1u, PSTACK_TOP);
    logv(MRS(control));
    logv(MRS(primask));
    emit("unpriv");
    SHCSR &= ~(1u << 17);
    g_hook = 0;
}

/* ------------------------------------------------------------------ */

static void hook_misc(uint32_t *frame, uint32_t excret, uint32_t exc)
{
    (void)excret;
    if (exc == 14u) {
        logv((frame[7] >> 9) & 1u);
        logv((uint32_t)frame & 7u);
    }
    if (exc == 16u) {
        irq_pend(3u);
        logv(ICSR & 0x7FF9FFu);
        logv(NVIC_IABR(0));
    }
}

static void t_misc(void)
{
    static volatile uint32_t word = 5u;

    g_hook = hook_misc;
    /* An exception clears the exclusive monitor. */
    logv(sys_excl_across_svc(&word));
    emit("excl-exc");

    /* A misaligned stack: the frame is realigned and bit 9 says so. */
    logv(sys_pendsv_misaligned(0xE000ED04u));
    emit("align");

    /* ICSR from inside a handler, with something lower pending. */
    irqs_reset();
    irq_prio(0u, 0x40u);
    irq_prio(3u, 0x80u);
    NVIC_ISER(0) = 9u;
    irq_pend(0u);
    emit("icsr");
    irqs_reset();
    g_hook = 0;
}

/* ------------------------------------------------------------------ */

static void t_systick(void)
{
    uint32_t a;
    uint32_t b;
    uint32_t seen = 0u;

    SYST_CSR = 0u;
    SYST_RVR = 0x00FFFFFFu;
    SYST_CVR = 0u;
    logv(SYST_CVR);
    SYST_CSR = 5u;
    for (volatile int i = 0; i < 10; i++) {
    }
    a = SYST_CVR;
    for (volatile int i = 0; i < 10; i++) {
    }
    b = SYST_CVR;
    logv(a > b);
    logv(a != 0u);
    logv((SYST_CSR >> 16) & 1u);
    SYST_CSR = 0u;
    SYST_RVR = 50u;
    SYST_CVR = 0u;
    SYST_CSR = 5u;
    for (uint32_t i = 0u; i < 100000u && !seen; i++) {
        seen = (SYST_CSR >> 16) & 1u;
    }
    logv(seen);
    /*
     * Stop the counter before testing that a read clears COUNTFLAG: at
     * 16 MHz a 50-tick SysTick wraps again before the next read, which
     * the first version of this line measured instead.
     */
    SYST_CSR = 0u;
    (void)SYST_CSR;
    logv((SYST_CSR >> 16) & 1u);
    logv(SYST_RVR);
    emit("systick");
}

/* ------------------------------------------------------------------ */

#define FPCAR REG(0xE000EF38u)
#define FPDSCR REG(0xE000EF3Cu)

static volatile uint32_t g_fp_sp;

static inline void set_s0(uint32_t v)
{
    __asm__ volatile("vmov s0, %0" ::"r"(v) : "memory");
}

static inline uint32_t get_s0(void)
{
    uint32_t v;

    __asm__ volatile("vmov %0, s0" : "=r"(v));
    return v;
}

static void hook_fp(uint32_t *frame, uint32_t excret, uint32_t exc)
{
    if (exc != 14u) {
        return;
    }
    logv(excret);
    logv(g_fp_sp - (uint32_t)frame);
    logv(FPCCR);
    logv(FPCAR - (uint32_t)frame);
    logv(MRS(control));
    /* The handler's first FP instruction: the deferred save happens. */
    set_s0(0x11111111u);
    logv(FPCCR);
    logv(frame[8]);  /* the thread's s0, saved into the frame */
    logv(frame[24]); /* the thread's FPSCR */
    {
        uint32_t f;

        __asm__ volatile("vmrs %0, fpscr" : "=r"(f));
        logv(f); /* FPDSCR's modes, for this new context */
    }
    logv(MRS(control));
}

static void t_fpctx(void)
{
    uint32_t f;

    g_hook = hook_fp;
    CPACR = 0x00F00000u;
    barrier();
    FPDSCR = 0x00C00000u; /* RZ for new contexts */
    __asm__ volatile("vmsr fpscr, %0" ::"r"(0x01000000u)); /* FZ */
    set_s0(0x3F800000u);
    logv(MRS(control) & 4u); /* FPCA: the thread has used the FPU */
    g_fp_sp = sys_read_sp();
    ICSR = 1u << 28;
    barrier();
    logv(get_s0()); /* restored from the frame */
    __asm__ volatile("vmrs %0, fpscr" : "=r"(f));
    logv(f);
    emit("fp-lazy");

    /* LSPEN clear: the state is saved at entry, not deferred. */
    FPCCR = FPCCR & ~(1u << 30);
    set_s0(0x40000000u);
    g_fp_sp = sys_read_sp();
    ICSR = 1u << 28;
    barrier();
    logv(get_s0());
    FPCCR = FPCCR | (1u << 30);
    emit("fp-nolazy");
    FPDSCR = 0u;
    g_hook = 0;
}

/* ------------------------------------------------------------------ */

/*
 * Encoding probes: one instruction, executed from RAM, and what the core
 * made of it -- ran, UNDEFINSTR, NOCP, or some other fault.
 *
 * This is how a question about a *reserved* encoding gets answered. The
 * generated cases only ever contain instructions an assembler will
 * write, so they cannot say what happens to a should-be-zero bit that is
 * one, a register list that runs off the end of the bank, or a D
 * register a D16 part does not have. The manual calls most of those
 * UNPREDICTABLE, which is permission for the silicon to do anything and
 * no guide to what it does. The frontend's two decoders disagreed on
 * twelve such slots; every answer below is the Cortex-M7's.
 *
 * **The controls come first and are the instrument's own test**: a load
 * that must run, a UDF that must not, and the same for the FPU. A probe
 * harness that reported "ran" for everything, or "UNDEFINED" for
 * everything, would agree with itself perfectly.
 *
 * Every register a probe can reach holds either a pointer into a scratch
 * buffer or a fixed value, and a probe's result is logged only where it
 * does not depend on where the image was linked.
 */
static uint16_t g_stub[4] __attribute__((aligned(4)));
static uint32_t g_scratch[64] __attribute__((aligned(8)));

static void hook_probe(uint32_t *frame, uint32_t excret, uint32_t exc)
{
    (void)excret;
    if (exc >= 3u && exc <= 6u) {
        log_fault(frame, (uint32_t)g_stub);
    }
}

static void probe(uint32_t enc)
{
    typedef uint32_t (*stub_fn)(uint32_t, uint32_t, uint32_t, uint32_t);
    char name[] = "probe-00000000";
    const uint32_t p = (uint32_t)&g_scratch[16];
    uint32_t r;

    for (uint32_t i = 0u; i < 8u; i++) {
        name[6u + i] = "0123456789abcdef"[(enc >> (28u - 4u * i)) & 0xFu];
    }
    for (uint32_t i = 0u; i < 64u; i++) {
        g_scratch[i] = 0xC0DE0000u + i;
    }
    g_stub[0] = (uint16_t)(enc >> 16);
    g_stub[1] = (uint16_t)enc;
    g_stub[2] = 0x4770u; /* bx lr */
    g_stub[3] = 0xBF00u; /* nop   */
    barrier();
    g_resume = (uint32_t)&g_stub[2];
    r = ((stub_fn)((uint32_t)g_stub | 1u))(p, 0x12345678u, 0x00FF8001u, p);
    /* r0 untouched is still the pointer, which is not the same number on
     * the board as in the emulator. */
    logv((r == p) ? 0xAAAA0000u : r);
    emit(name);
}

static void t_probe(void)
{
    static const uint32_t k_enc[] = {
        /* controls: runs, undefined, runs, runs, undefined */
        0xF8D00004u, /* ldr.w r0, [r0, #4]                          */
        0xF7F0A000u, /* udf.w #0                                    */
        0xEC900A04u, /* vldmia r0, {s0-s3}                          */
        0xEE000B10u, /* vmov.32 d0[0], r0                           */
        0xEE300B00u, /* vadd.f64 d0, d0, d0 -- no double precision  */

        /* Load word with bit 24 set: no row of the table has it. */
        0xF9D00004u, /* as ldr.w r0, [r0, #4]                       */
        0xF9500C04u, /* as ldr r0, [r0, #-4]                        */
        0xF95F0004u, /* as ldr.w r0, [pc, #-4]                      */
        0xF9DF0000u, /* as ldr.w r0, [pc, #0]                       */

        /* SSAT16/USAT16 with the should-be-zero bits 5:4 set. */
        0xF3210006u, /* ssat16 r0, #7, r1 -- the real one           */
        0xF3210026u, /* bit 5                                       */
        0xF3210016u, /* bit 4                                       */
        0xF3A10026u, /* usat16, bit 5                               */

        /* FP register lists that leave the bank. */
        0xEC90FA04u, /* vldmia r0, {s30-s33}                        */
        0xEC900A00u, /* vldmia r0, {} -- no registers               */
        0xEC900B22u, /* vldmia r0, {d0-d16}                         */
        0xEC900B05u, /* fldmiax r0, {d0-d1} -- odd imm8             */

        /* D16 and up, on a part with sixteen. */
        0xEDD00B01u, /* vldr d16, [r0, #4]                          */
        0xEE000B90u, /* vmov.32 d16[0], r0                          */
        0xEC400B30u, /* vmov d16, r0, r0                            */
        0xEC510B30u, /* vmov r0, r1, d16                            */
    };

    g_hook = hook_probe;
    SHCSR |= 7u << 16; /* MEMFAULTENA, BUSFAULTENA, USGFAULTENA */
    CPACR = 0x00F00000u;
    barrier();
    for (uint32_t i = 0u; i < sizeof(k_enc) / sizeof(k_enc[0]); i++) {
        probe(k_enc[i]);
    }
    SHCSR &= ~(7u << 16);
    g_hook = 0;
}

/* ------------------------------------------------------------------ */

int main(void)
{
    for (uint32_t i = 0u; i < sizeof(g_vtab) / sizeof(g_vtab[0]); i++) {
        g_vtab[i] = sys_exc_entry;
    }
    VTOR = (uint32_t)g_vtab;
    barrier();

    puts_("SYS-BEGIN\n");
    t_ids();
    t_order();
    t_prigroup();
    t_basepri();
    t_faultmask();
    t_svc();
    t_usage();
    t_bus();
    t_mpu();
    t_psp();
    t_misc();
    t_systick();
    t_fpctx();
    t_probe();
    puts_("SYS-END\nDIFF-END\n");
    return 0;
}
