/* SPDX-License-Identifier: Apache-2.0 */
/*
 * smp.c - the MPC5777C's second e200z7.
 *
 * Core 1 is held in reset until core 0 writes SIU_RSTVEC1 with RST clear
 * (NXP AN5191, 3.9), and starts at the address in it, in the encoding
 * its VLE bit names, reading 1 from PIR. From then on the two share
 * memory and nothing else: each has its own registers, its own SPRs and
 * its own TLB. What this checks is the part of that two cores can see:
 *
 *   - that core 1 is held, starts where it is told, and knows who it is;
 *   - that lwarx/stwcx. are atomic between the cores -- an increment both
 *     make N times each comes to 2N -- and that a reservation is broken
 *     by the other core's store to its word and by nothing else;
 *   - that the TLB is per core, for data and for code. The code case is
 *     the one a JIT can get wrong: core 0 runs the address until it is a
 *     block, then core 1 calls the same address through its own mapping,
 *     which on a build where the cores share one block table finds core
 *     0's block unless the core is part of what a block is built for;
 *   - that RST set again holds the core, and a second release starts it
 *     afresh -- and in VLE when RSTVEC1 says so.
 *
 * Core 1 takes commands through three shared words and reports through
 * a fourth: core 0 is the only one that prints. Every wait is bounded,
 * so a core that never answers fails a check instead of hanging the
 * run, and the run must print PPC-SMP-END to pass at all.
 */
typedef unsigned int u32;
typedef unsigned char u8;

#define UART (*(volatile unsigned char *)0x10000000u)
#define SIU_RSTVEC1 (*(volatile u32 *)0xC3F909B0u)
#define RSTVEC_VLE 0x1u
#define RSTVEC_RST 0x2u

/* The MAS fields, tables 10-9 to 10-12. */
#define V 0x80000000u
#define IPROT 0x40000000u
#define TSIZE(n) ((u32)(n) << 7)
#define K4 TSIZE(2)
#define G4 TSIZE(22)
#define SX 0x10u
#define SW 0x04u
#define SR 0x01u
#define ALL 0x3Fu

#define DATA_VA 0x50000000u
#define CODE_VA 0x51000000u

#define N_INC 2000u
#define PATIENCE 20000000u

/* crt0.S and smp_stubs.S */
#define PPC_CORE1_STACK 4096u
u8 ppc_core1_stack[PPC_CORE1_STACK] __attribute__((aligned(16)));
void _start1(void);
void core1_vle(void);
void smp_tlbwe(u32 esel, u32 mas1, u32 mas2, u32 mas3);
extern u8 smp_page_a[], smp_page_b[];
volatile u32 g_vle_ran;

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
        pc_("0123456789abcdef"[(v >> i) & 15u]);
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

static u32 mfpir(void)
{
    u32 v;

    __asm__ volatile("mfspr %0, 286" : "=r"(v));
    return v;
}

static u32 sp_now(void)
{
    u32 v;

    __asm__ volatile("mr %0, 1" : "=r"(v));
    return v;
}

static u32 lwarx(volatile u32 *p)
{
    u32 v;

    __asm__ volatile("lwarx %0, 0, %1" : "=r"(v) : "r"(p) : "memory");
    return v;
}

/* 1 if it stored. */
static u32 stwcx(volatile u32 *p, u32 v)
{
    u32 cr;

    __asm__ volatile("stwcx. %1, 0, %2\n\tmfcr %0"
                     : "=r"(cr)
                     : "r"(v), "r"(p)
                     : "cr0", "memory");
    return (cr >> 29) & 1u; /* CR0[EQ] */
}

static void atomic_inc(volatile u32 *p)
{
    for (;;) {
        const u32 v = lwarx(p);

        if (stwcx(p, v + 1u)) {
            return;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Core 1                                                              */
/* ------------------------------------------------------------------ */

enum {
    OP_INC = 1,
    OP_STORE,
    OP_RESV,
    OP_STCX,
    OP_MAP,
    OP_LOAD,
    OP_CALL,
    OP_RACE,
    OP_PARK
};

/* Posted by core 0, in this order, with g_seq last. */
static volatile u32 g_op, g_arg, g_arg2, g_seq;
/* Written by core 1, g_done last. */
static volatile u32 g_res, g_done;

static volatile struct {
    u32 up, pir, sp, boots;
} g_c1;

static volatile u32 g_counter, g_x, g_y, g_race_done;

static volatile u8 g_pg[8192] __attribute__((aligned(4096)));

/*
 * Data at DATA_VA and code at CODE_VA, both 4 KB, ahead of a 4 GB
 * identity entry for everything else -- written first, while the reset
 * entry still maps this code. The lowest-numbered entry wins, so 0 and
 * 1 are the two pages and 2 is the rest.
 */
static void map(u32 data_pa, u32 code_pa)
{
    smp_tlbwe(2u, V | IPROT | G4, 0u, ALL);
    smp_tlbwe(0u, V | K4, DATA_VA, data_pa | SR | SW);
    smp_tlbwe(1u, V | K4, CODE_VA, code_pa | SX | SR);
}

static u32 call_code(void)
{
    return ((u32 (*)(void))CODE_VA)();
}

void main1(void)
{
    u32 seen = g_seq;

    g_c1.pir = mfpir();
    g_c1.sp = sp_now();
    g_c1.boots++;
    g_c1.up = 1u;
    for (;;) {
        while (g_seq == seen) {
        }
        seen = g_seq;

        const u32 op = g_op;
        volatile u32 *const p = (volatile u32 *)g_arg;
        u32 res = 0u;

        switch (op) {
        case OP_INC:
            for (u32 i = 0u; i < g_arg; i++) {
                atomic_inc(&g_counter);
            }
            break;
        case OP_STORE:
            *p = g_arg2;
            break;
        case OP_RESV:
            res = lwarx(p);
            break;
        case OP_STCX:
            res = stwcx(p, g_arg2);
            break;
        case OP_MAP:
            map(g_arg, g_arg2);
            break;
        case OP_LOAD:
            res = *(volatile u32 *)DATA_VA;
            break;
        case OP_CALL:
            res = call_code();
            break;
        case OP_RACE:
            /*
             * Plain stores of rising even values, each read back. Core 0
             * meanwhile adds one to whatever it reserves, so a read-back
             * may be the value stored or one more -- and is never an odd
             * number *below* it, which would be core 0's stwcx. writing
             * over this store from an older value.
             */
            for (u32 i = 1u; i <= g_arg; i++) {
                g_x = 2u * i;

                const u32 r = g_x;

                if ((r & 1u) != 0u && r < 2u * i) {
                    res++;
                }
            }
            g_race_done = 1u;
            break;
        default: /* OP_PARK */
            g_res = 0u;
            g_done = seen;
            return; /* crt0 waits with interrupts off: parked */
        }
        g_res = res;
        g_done = seen;
    }
}

/* ------------------------------------------------------------------ */
/* Core 0                                                              */
/* ------------------------------------------------------------------ */

/* True if *p came to `want` before core 0 ran out of patience. */
static u32 wait_eq(volatile u32 *p, u32 want)
{
    for (u32 i = 0u; i < PATIENCE; i++) {
        if (*p == want) {
            return 1u;
        }
    }
    return 0u;
}

static u32 g_posted;

static void post(u32 op, u32 arg, u32 arg2)
{
    g_op = op;
    g_arg = arg;
    g_arg2 = arg2;
    g_seq = ++g_posted;
}

/* Post and wait; core 1's result, or 0xDEAD if it never answered. */
static u32 call1(u32 op, u32 arg, u32 arg2)
{
    post(op, arg, arg2);
    return wait_eq(&g_done, g_posted) ? g_res : 0xDEADu;
}

static void spin(u32 n)
{
    for (volatile u32 i = 0u; i < n; i++) {
    }
}

int main(void)
{
    /* Held at reset, and core 0 is core 0. */
    check("pir", mfpir(), 0u);
    check("rstvec1-reset", SIU_RSTVEC1, 0xFFFFFFFEu);
    spin(20000u);
    check("held-at-reset", g_c1.up, 0u);

    /* Start it, Book E, at _start1. */
    SIU_RSTVEC1 = (u32)_start1;
    check("rstvec1-written", SIU_RSTVEC1, (u32)_start1);
    check("core1-up", wait_eq(&g_c1.up, 1u), 1u);
    check("core1-pir", g_c1.pir, 1u);
    check("core1-own-stack",
          g_c1.sp > (u32)ppc_core1_stack &&
              g_c1.sp <= (u32)ppc_core1_stack + PPC_CORE1_STACK,
          1u);

    /* Atomic between the cores: N each, 2N in all. */
    g_counter = 0u;
    post(OP_INC, N_INC, 0u);
    for (u32 i = 0u; i < N_INC; i++) {
        atomic_inc(&g_counter);
    }
    check("inc-core1-done", wait_eq(&g_done, g_posted), 1u);
    check("inc-total", g_counter, 2u * N_INC);

    /* Core 0's reservation, broken by core 1's store to the word... */
    g_x = 1u;
    u32 v = lwarx(&g_x);
    check("resv-store-1", call1(OP_STORE, (u32)&g_x, 7u), 0u);
    check("resv-broken", stwcx(&g_x, v + 1u), 0u);
    check("resv-broken-mem", g_x, 7u);
    /*
     * ...even with the value it already held, which a compare against
     * the bytes lwarx read cannot see: only the store breaking the
     * reservation can.
     */
    g_x = 1u;
    v = lwarx(&g_x);
    check("resv-store-same", call1(OP_STORE, (u32)&g_x, 1u), 0u);
    check("resv-broken-same-value", stwcx(&g_x, v + 1u), 0u);
    check("resv-broken-same-mem", g_x, 1u);
    /* ...and not by its store to another. */
    v = lwarx(&g_x);
    check("resv-store-y", call1(OP_STORE, (u32)&g_y, 9u), 0u);
    check("resv-kept", stwcx(&g_x, v + 1u), 1u);
    check("resv-kept-mem", g_x, 2u);
    /* Core 1's, broken by core 0's store, and kept without one. */
    g_x = 3u;
    check("resv1-read", call1(OP_RESV, (u32)&g_x, 0u), 3u);
    g_x = 4u;
    check("resv1-broken", call1(OP_STCX, (u32)&g_x, 5u), 0u);
    check("resv1-broken-mem", g_x, 4u);
    check("resv1-read-again", call1(OP_RESV, (u32)&g_x, 0u), 4u);
    check("resv1-kept", call1(OP_STCX, (u32)&g_x, 6u), 1u);
    check("resv1-kept-mem", g_x, 6u);

    /*
     * A plain store landing between stwcx.'s check and its write. Only
     * a host running the cores at once can put it there, and then only
     * sometimes; a stwcx. that succeeded over it is a lost update, which
     * core 1 counts.
     */
    g_x = 0u;
    g_race_done = 0u;
    post(OP_RACE, 200000u, 0u);
    while (g_race_done == 0u) {
        const u32 w = lwarx(&g_x);

        if ((w & 1u) == 0u) {
            (void)stwcx(&g_x, w + 1u);
        }
    }
    check("race-done", wait_eq(&g_done, g_posted), 1u);
    check("race-no-lost-store", g_res, 0u);

    /* A TLB each: one address, a page each, data and code. */
    *(volatile u32 *)&g_pg[0] = 0xA0u;
    *(volatile u32 *)&g_pg[4096] = 0xB1u;
    map((u32)&g_pg[0], (u32)smp_page_a);
    check("map-core1", call1(OP_MAP, (u32)&g_pg[4096], (u32)smp_page_b), 0u);
    check("tlb-data-core0", *(volatile u32 *)DATA_VA, 0xA0u);
    check("tlb-data-core1", call1(OP_LOAD, 0u, 0u), 0xB1u);
    u32 r = 0u;
    for (u32 i = 0u; i < 100u; i++) {
        r |= call_code() ^ 0x5Au; /* until it is a block */
    }
    check("tlb-code-core0", r, 0u);
    check("tlb-code-core1", call1(OP_CALL, 0u, 0u), 0xA2u);
    check("tlb-code-core0-again", call_code(), 0x5Au);

    /* Held again, and started again: a fresh core, PIR still 1. */
    check("park", call1(OP_PARK, 0u, 0u), 0u);
    SIU_RSTVEC1 = (u32)_start1 | RSTVEC_RST;
    check("rstvec1-held", SIU_RSTVEC1, (u32)_start1 | RSTVEC_RST);
    g_c1.up = 0u;
    g_posted = g_seq = g_done = 0u;
    SIU_RSTVEC1 = (u32)_start1;
    check("restart-up", wait_eq(&g_c1.up, 1u), 1u);
    check("restart-boots", g_c1.boots, 2u);
    check("restart-pir", g_c1.pir, 1u);
    /*
     * Held while it runs, not parked: a command posted now must not be
     * carried out, and the core has to stop to see that. Then started a
     * third time.
     */
    SIU_RSTVEC1 = (u32)_start1 | RSTVEC_RST;
    spin(20000u);
    g_y = 0u;
    post(OP_STORE, (u32)&g_y, 0x99u);
    spin(200000u);
    check("held-runs-nothing", g_y, 0u);
    g_c1.up = 0u;
    g_posted = g_seq = g_done = 0u;
    SIU_RSTVEC1 = (u32)_start1;
    check("restart2-up", wait_eq(&g_c1.up, 1u), 1u);
    check("restart2-boots", g_c1.boots, 3u);

    /* And in VLE, which is RSTVEC1's to say. */
    check("park-again", call1(OP_PARK, 0u, 0u), 0u);
    SIU_RSTVEC1 = (u32)core1_vle | RSTVEC_RST;
    g_vle_ran = 0u;
    SIU_RSTVEC1 = (u32)core1_vle | RSTVEC_VLE;
    check("vle-start", wait_eq(&g_vle_ran, 0x56u), 1u);

    ps_("PPC-SMP-END checks=");
    phex(g_checks);
    ps_(" fails=");
    phex(g_fail);
    pc_('\n');
    return (int)g_fail;
}
