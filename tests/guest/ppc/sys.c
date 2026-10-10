/* SPDX-License-Identifier: Apache-2.0 */
/*
 * sys.c - the e200z7's system level: interrupts, privilege, the special
 * purpose registers, reservations, the timers and the two floating-point
 * interrupts.
 *
 * What a generated instruction cannot reach. alu.c and the generated
 * cases say whether an add is right; nothing in them takes an
 * interrupt on purpose, runs in problem state, or asks what a register
 * that does not exist reads as.
 *
 * **Every expectation is the manual's**, with the section beside it --
 * the e200z759n3 Core Reference Manual, Rev. 2. None was read back from
 * the emulator. That matters more than usual here: there is no board
 * for this frontend, so a value this file got from the implementation
 * would be the implementation agreeing with itself.
 *
 * Runs interpreted and translated. Most of what it does ends a
 * translated block on purpose (see ppc_ir.c), which is the point of
 * running it there: an interrupt taken from inside a block, a return
 * into user mode and a timer read all cross that boundary.
 */
#include "ppc_clock.h"

typedef unsigned int u32;

#define UART (*(volatile unsigned char *)0x10000000u)

/* ESR, table 7-3 */
#define ESR_PIL 0x08000000u
#define ESR_PPR 0x04000000u
#define ESR_PTR 0x02000000u
#define ESR_ST 0x00800000u
#define ESR_SPE 0x00000080u

/* MSR, table 7-4 */
#define MSR_SPE 0x02000000u
#define MSR_CE 0x00020000u
#define MSR_EE 0x00008000u
#define MSR_PR 0x00004000u
#define MSR_FP 0x00002000u
#define MSR_ME 0x00001000u
#define MSR_DE 0x00000200u
#define MSR_RI 0x00000002u

/* SPEFSCR, table 5-1 */
#define FINXS 0x00200000u
#define FDBZS 0x00080000u
#define FG 0x00002000u
#define FX 0x00001000u
#define FDBZ 0x00000400u
#define FINXE 0x00000040u
#define FDBZE 0x00000010u

struct trap {
    u32 ivor, srr0, srr1, esr, dear, msr, count, resume, cr, r6;
};
volatile struct trap g_trap;
volatile u32 g_uval;

/* sys_stubs.S */
u32 sys_install(void);
u32 stub_illegal(void), stub_trap(void), stub_twi_not(void), stub_twi_taken(void);
u32 stub_sc(u32 nr), stub_mfmsr(void), stub_mtmsr(u32 v);
u32 rd_pvr(void), rd_svr(void), rd_l1cfg0(void), rd_l1cfg1(void), rd_tbl(void);
u32 rd_tbl_w(void), rd_dec(void), rd_tsr(void), rd_esr(void), rd_dear(void);
u32 rd_spefscr(void), rd_sprg4(void), rd_usprg0(void), rd_hid0(void);
u32 rd_undef(void), rd_undef_priv(void);
u32 wr_pvr(u32 v), wr_l1cfg0(u32 v), wr_tbl(u32 v), wr_dec(u32 v), wr_tsr(u32 v);
u32 wr_esr(u32 v), wr_dear(u32 v), wr_spefscr(u32 v), wr_sprg4(u32 v);
u32 wr_usprg0(u32 v), wr_l1csr0(u32 v), wr_undef(u32 v);
void stub_wrteei0(void), stub_wrteei1(void);
u32 stub_load(u32 a), stub_store(u32 a, u32 v), stub_jump(u32 a), stub_lwarx(u32 a);
u32 stub_load_mid(u32 a), stub_store_mid(u32 a, u32 v), stub_trap_mid(void);
u32 stub_resv_w(u32 a, u32 v), stub_stwcx(u32 a, u32 v);
u32 stub_resv_other(u32 a, u32 v, u32 other), stub_resv_b(u32 a, u32 v);
u32 stub_resv_h(u32 a, u32 v), stub_lharx(u32 a), stub_multi(u32 buf);
u32 stub_lmw(u32 a), stub_dcbz(u32 a), stub_wait(void), stub_evaddw(void);
u32 fp_div(u32 a, u32 b, u32 spefscr), fp_add(u32 a, u32 b, u32 spefscr);
u32 fp_mul(u32 a, u32 b, u32 spefscr);
u32 stub_user(void (*fn)(void), u32 unused, u32 arg);
void u_mfmsr(void), u_mtmsr(void), u_rd_srr0(void), u_rd_undef(void);
void u_rd_undef_priv(void), u_wr_l1cfg0(void), u_rd_tbl(void), u_rd_sprg4(void);
void u_rd_xer(void), u_wrteei(void), u_rfi(void), u_dcbi(void), u_nothing(void);
void u_usprg0(void), u_host_write(void);

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

/* The interrupt a stub ended in, with its syndrome. */
static void took(const char *name, u32 ret, u32 ivor, u32 esr)
{
    check(name, ret, ivor);
    check(name, g_trap.ivor, ivor);
    check(name, g_trap.esr, esr);
}

static u32 g_mem[24];

int main(void)
{
    const u32 msr0 = stub_mfmsr();
    u32 n;
    u32 top;

    top = sys_install();
    /* An IVOR is an offset in IVPR's page: the vectors must be in it. */
    check("vectors-in-page", top >> 16, 0x8000u);

    /* --- who this is (2.4.3, 11.4.3, 11.4.4) ------------------------ */
    check("pvr", rd_pvr() >> 16, 0x8169u); /* Freescale, Zen Z7, e200z759n3 */
    check("l1cfg0", rd_l1cfg0(), 0x284D1810u);
    check("l1cfg1", rd_l1cfg1(), 0x084D1810u);

    /* --- the program interrupt (7.7.7) ------------------------------ */
    n = g_trap.count;
    took("illegal", stub_illegal(), 6u, ESR_PIL);
    check("illegal-srr0", g_trap.srr0, (u32)&stub_illegal);
    check("illegal-srr1", g_trap.srr1, msr0);
    check("illegal-once", g_trap.count, n + 1u);
    took("trap", stub_trap(), 6u, ESR_PTR);
    check("trap-srr0", g_trap.srr0, (u32)&stub_trap);
    n = g_trap.count;
    check("twi-not-taken", stub_twi_not(), 0xAAu);
    check("twi-not-taken-count", g_trap.count, n);
    took("twi-taken", stub_twi_taken(), 6u, ESR_PTR);

    /*
     * What an interrupt leaves of MSR: CE, ME, DE and RI survive a
     * non-critical one and everything else is cleared (table 7-18).
     * Set some of each first, or "cleared" proves nothing.
     */
    (void)stub_mtmsr(msr0 | MSR_ME | MSR_RI | MSR_FP | MSR_SPE);
    (void)stub_illegal();
    check("entry-msr", g_trap.msr, MSR_ME | MSR_RI);
    check("entry-srr1", g_trap.srr1, msr0 | MSR_ME | MSR_RI | MSR_FP | MSR_SPE);
    (void)stub_mtmsr(msr0);

    /* MSR has the bits of table 7-4 and no others. PR, EE and the
     * address-space bits are left out so that this stays where it is. */
    (void)stub_mtmsr(0xFFF93DCFu);
    n = stub_mfmsr();
    (void)stub_mtmsr(msr0);
    check("msr-implemented", n, 0x06003906u);

    /* --- a system call the host does not answer (7.7.9) ------------- */
    g_trap.resume = 1u;
    n = g_trap.count;
    check("sc-returns", stub_sc(0x7777u), 0xAAu);
    g_trap.resume = 0u;
    check("sc-ivor", g_trap.ivor, 8u);
    check("sc-count", g_trap.count, n + 1u);
    check("sc-srr0-is-next", g_trap.srr0, (u32)&stub_sc + 8u);
    check("sc-esr", g_trap.esr, 0u);

    /* --- special purpose registers (2.5.1, 3.15) -------------------- */
    took("spr-undefined", rd_undef(), 6u, ESR_PIL);
    took("spr-undefined-privileged", rd_undef_priv(), 6u, ESR_PIL);
    took("spr-write-undefined", wr_undef(1u), 6u, ESR_PIL);
    took("spr-write-read-only", wr_pvr(1u), 6u, ESR_PIL);
    took("spr-write-read-only-2", wr_l1cfg0(1u), 6u, ESR_PIL);
    took("spr-read-write-only", rd_tbl_w(), 6u, ESR_PIL);
    check("spr-esr-write", wr_esr(0x12340000u), 0xAAu);
    check("spr-esr-read", rd_esr(), 0x12340000u);
    check("spr-dear-write", wr_dear(0xCAFEF00Du), 0xAAu);
    check("spr-dear-read", rd_dear(), 0xCAFEF00Du);
    check("spr-sprg4-write", wr_sprg4(0x600DF00Du), 0xAAu);
    check("spr-sprg4-read", rd_sprg4(), 0x600DF00Du);
    /* SPEFSCR: MODE reads 0 (table 5-1) and the reserved bits with it. */
    (void)wr_spefscr(0xFFFFFFFFu);
    check("spefscr-implemented", rd_spefscr(), 0xFF3EFF7Fu);
    (void)wr_spefscr(0u);

    /* --- problem state (7.7.7, 2.5.1) -------------------------------- */
    took("user-mfmsr", stub_user(u_mfmsr, 0u, 0u), 6u, ESR_PPR);
    check("user-srr1-pr", g_trap.srr1 & MSR_PR, MSR_PR);
    check("back-in-supervisor", stub_mfmsr() & MSR_PR, 0u);
    took("user-mtmsr", stub_user(u_mtmsr, 0u, 0u), 6u, ESR_PPR);
    took("user-rfi", stub_user(u_rfi, 0u, 0u), 6u, ESR_PPR);
    took("user-wrteei", stub_user(u_wrteei, 0u, 0u), 6u, ESR_PPR);
    took("user-dcbi", stub_user(u_dcbi, 0u, 0u), 6u, ESR_PPR);
    took("user-privileged-spr", stub_user(u_rd_srr0, 0u, 0u), 6u, ESR_PPR);
    /* Privilege is the number's, whether or not the register exists. */
    took("user-undefined-privileged-spr", stub_user(u_rd_undef_priv, 0u, 0u), 6u,
         ESR_PPR);
    took("user-undefined-spr", stub_user(u_rd_undef, 0u, 0u), 6u, ESR_PIL);
    took("user-read-only-spr", stub_user(u_wr_l1cfg0, 0u, 0u), 6u, ESR_PIL);
    /* What it may do: it gets as far as its system call. */
    g_uval = 0u;
    check("user-runs", stub_user(u_nothing, 0u, 0u), 8u);
    check("user-ran", g_uval, 0x55u);
    check("user-sc-esr", g_trap.esr, 0u);
    check("user-tbl", stub_user(u_rd_tbl, 0u, 0u), 8u);
    check("user-tbl-moves", g_uval != 0u, 1u);
    check("user-sprg4", stub_user(u_rd_sprg4, 0u, 0u), 8u);
    check("user-sprg4-value", g_uval, 0x600DF00Du);
    check("user-usprg0", stub_user(u_usprg0, 0u, 0xBEEF1234u), 8u);
    check("user-usprg0-value", g_uval, 0xBEEF1234u);
    check("user-xer", stub_user(u_rd_xer, 0u, 0u), 8u);
    /*
     * A system call from problem state with the host's write number is
     * the guest's, not the host's: it must reach IVOR8, and the stub
     * hangs if the host answers it instead.
     */
    check("user-host-number", stub_user(u_host_write, 0u, 0u), 8u);

    /* --- storage (7.7.3, 7.7.4) -------------------------------------- */
    took("load-unmapped", stub_load(0xD0000000u), 2u, 0u);
    check("load-dear", g_trap.dear, 0xD0000000u);
    check("load-srr0", g_trap.srr0, (u32)&stub_load);
    took("store-unmapped", stub_store(0xD0000004u, 1u), 2u, ESR_ST);
    check("store-dear", g_trap.dear, 0xD0000004u);
    took("fetch-unmapped", stub_jump(0xD0000100u), 3u, 0u);
    check("fetch-srr0", g_trap.srr0, 0xD0000100u);

    /*
     * The same again two instructions into a block: the address has to
     * be the faulting instruction's and not the block's, and what the
     * block computed before the fault has to be in the register. See
     * the stubs -- everything above faults on a block's first
     * instruction, where neither can be wrong.
     */
    took("load-mid-block", stub_load_mid(0xD0000000u), 2u, 0u);
    check("load-mid-srr0", g_trap.srr0, (u32)&stub_load_mid + 8u);
    check("load-mid-r6", g_trap.r6, 0x33u);
    took("store-mid-block", stub_store_mid(0xD0000004u, 1u), 2u, ESR_ST);
    check("store-mid-srr0", g_trap.srr0, (u32)&stub_store_mid + 8u);
    check("store-mid-r6", g_trap.r6, 0x55u);
    took("trap-mid-block", stub_trap_mid(), 6u, ESR_PTR);
    check("trap-mid-srr0", g_trap.srr0, (u32)&stub_trap_mid + 8u);
    check("trap-mid-r6", g_trap.r6, 0x77u);

    /* --- alignment (7.7.6): only what the list names ----------------- */
    took("lwarx-odd", stub_lwarx((u32)g_mem + 1u), 5u, 0u);
    check("lwarx-dear", g_trap.dear, (u32)g_mem + 1u);
    took("lharx-odd", stub_lharx((u32)g_mem + 1u), 5u, 0u);
    took("lmw-odd", stub_lmw((u32)g_mem + 2u), 5u, 0u);
    took("stwcx-odd", stub_resv_other((u32)g_mem, 1u, (u32)g_mem + 2u), 5u, ESR_ST);
    /* dcbz with the data cache off, which it is out of reset. */
    took("dcbz-cache-off", stub_dcbz((u32)g_mem), 5u, ESR_ST);
    /* An ordinary unaligned access is performed (3.4). */
    g_mem[4] = 0x11223344u;
    g_mem[5] = 0x55667788u;
    check("unaligned-load", stub_load((u32)&g_mem[4] + 1u), 0x22334455u);
    check("unaligned-store", stub_store((u32)&g_mem[4] + 3u, 0xAABBCCDDu), 0xAAu);
    check("unaligned-store-lo", g_mem[4], 0x112233AAu);
    check("unaligned-store-hi", g_mem[5], 0xBBCCDD88u);

    /* --- reservations (3.5, 3.13) ------------------------------------ */
    g_mem[0] = 0u;
    check("stwcx-reserved", stub_resv_w((u32)g_mem, 0x1234u) & 0xF0000000u, 0x20000000u);
    check("stwcx-stored", g_mem[0], 0x1234u);
    /* Used up: a second store conditional has nothing to store under. */
    check("stwcx-unreserved", stub_stwcx((u32)g_mem, 0x9999u) & 0xF0000000u, 0u);
    check("stwcx-not-stored", g_mem[0], 0x1234u);
    /* ...and with no reservation it is a no-op that takes no exception. */
    n = g_trap.count;
    check("stwcx-odd-unreserved", stub_stwcx((u32)g_mem + 1u, 1u) & 0xF0000000u, 0u);
    check("stwcx-odd-unreserved-count", g_trap.count, n);
    g_mem[1] = 0xFFFFFFFFu;
    check("stbcx", stub_resv_b((u32)&g_mem[1], 0x12u) & 0xF0000000u, 0x20000000u);
    check("stbcx-stored", g_mem[1], 0x12FFFFFFu);
    check("sthcx", stub_resv_h((u32)&g_mem[1] + 2u, 0x3456u) & 0xF0000000u,
          0x20000000u);
    check("sthcx-stored", g_mem[1], 0x12FF3456u);

    /* --- load and store multiple -------------------------------------- */
    check("multi", stub_multi((u32)&g_mem[8]), (0x11112222u ^ 0x33334444u) + 0x55556666u);
    check("multi-first", g_mem[8], 0x11112222u);
    check("multi-last", g_mem[10], 0x55556666u);

    /* --- the timers (table 2-7, Book E chapter 8) --------------------- */
    {
        const u32 t0 = rd_tbl();
        const u32 t1 = rd_tbl();
        u32 ticks;
        u32 spin;

        check("tbl-advances", (t1 - t0) != 0u && (t1 - t0) < 1000u, 1u);
        /*
         * wait: stops until an interrupt, which is the decrementer crt0
         * set going. Nothing has run for a whole period yet, so this is
         * also the first tick -- a core waiting on its own clock, which
         * only moves when instructions do.
         */
        ticks = ppc_dec_ticks;
        check("wait-returns", stub_wait(), 0xAAu);
        check("wait-woke-on-tick", ppc_dec_ticks != ticks, 1u);
        /*
         * With interrupts off the event is still recorded: TSR[DIS] is
         * set whether or not anything takes it, and writing a one
         * clears it.
         */
        stub_wrteei0();
        (void)wr_tsr(0x08000000u);
        check("tsr-cleared", rd_tsr() & 0x08000000u, 0u);
        for (spin = 0u; spin < 100000u && (rd_tsr() & 0x08000000u) == 0u; spin++) {
        }
        check("tsr-dis-sets-masked", rd_tsr() & 0x08000000u, 0x08000000u);
        ticks = ppc_dec_ticks;
        stub_wrteei1();
        /* ...and is taken the moment it may be. */
        check("dis-taken-on-enable", ppc_dec_ticks, ticks + 1u);
    }

    /* --- the SPE unit, which this model does not have (7.7.18) -------- */
    took("spe-unavailable", stub_evaddw(), 32u, ESR_SPE);
    check("spe-srr0", g_trap.srr0, (u32)&stub_evaddw);

    /* --- the embedded floating-point interrupts (5.2.5) --------------- */
    /* Disabled: a default result, and the flags (table 5-2). */
    check("fp-div-zero", fp_div(0x3F800000u, 0u, 0u), 0x7F7FFFFFu);
    check("fp-div-zero-flags", rd_spefscr(), FDBZ | FDBZS);
    /* Enabled: the data interrupt, and the destination untouched. */
    n = g_trap.count;
    took("fp-data", fp_div(0x3F800000u, 0u, FDBZE), 33u, ESR_SPE);
    check("fp-data-count", g_trap.count, n + 1u);
    check("fp-data-dest", g_trap.r6, 0x12345678u);
    check("fp-data-srr0", g_trap.srr0, (u32)&fp_div + 12u);
    check("fp-data-flags", rd_spefscr(), FDBZE | FDBZ | FDBZS);
    /* Scalar FP needs no MSR[SPE] (7.7.18): it ran above with it clear. */
    check("fp-msr-spe-clear", stub_mfmsr() & MSR_SPE, 0u);
    /*
     * 16777216 + 1 is not representable. Inexact enabled: the round
     * interrupt, the *truncated* sum written, the guard bit saying what
     * was dropped, and the next instruction named.
     */
    check("fp-inexact-rounds", fp_add(0x4B800000u, 0x3F800000u, 0u), 0x4B800000u);
    check("fp-inexact-flags", rd_spefscr(), FINXS | FG);
    check("fp-inexact-up", fp_add(0x4B800001u, 0x3F800000u, 0u), 0x4B800002u);
    g_trap.resume = 1u;
    n = g_trap.count;
    check("fp-round-truncates", fp_add(0x4B800001u, 0x3F800000u, FINXE), 0x4B800001u);
    g_trap.resume = 0u;
    check("fp-round-ivor", g_trap.ivor, 34u);
    check("fp-round-count", g_trap.count, n + 1u);
    check("fp-round-esr", g_trap.esr, ESR_SPE);
    check("fp-round-srr0-is-next", g_trap.srr0, (u32)&fp_add + 16u);
    check("fp-round-flags", rd_spefscr(), FINXE | FINXS | FG);
    (void)wr_spefscr(0u);
    check("fp-mul", fp_mul(0x40400000u, 0x40E00000u, 0u), 0x41A80000u); /* 3 * 7 */

    ps_("PPC-SYS-END checks=");
    phex(g_checks);
    ps_(" fails=");
    phex(g_fail);
    pc_('\n');
    return (int)g_fail;
}
