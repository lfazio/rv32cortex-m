/* SPDX-License-Identifier: Apache-2.0 */
/*
 * mmu.c - the e200z7's MMU: the TLB, the MAS registers, the five TLB
 * instructions, and translation of fetches and data accesses.
 *
 * **Every expectation is the manual's** -- the e200z759n3 Core Reference
 * Manual, Rev. 2, chapter 10 and the interrupt tables of chapter 7 --
 * or a value this guest wrote to physical memory itself. None was read
 * back from the emulator. Two are the *model's* choices where the
 * manual leaves one or the model departs from it, and say so: the reset
 * entry, and MAS2[E].
 *
 * It programs the TLB the way an MPC57xx start-up does -- RAM and the
 * console in protected entries, the reset entry overwritten -- and then
 * runs with translation on for the rest, under --jit as translated
 * blocks.
 *
 * **The JIT's cases** are the ones marked "jit-": each runs code once,
 * so that a block of it exists, changes exactly one thing the block was
 * built under -- the mapping, the address space, the PID, the privilege,
 * translation itself -- and runs it again. Under the interpreter they
 * are ordinary checks; under --jit each is the only thing that can see
 * its mechanism removed. The guest's other checks were written before
 * any block ran under a programmed TLB, and six of eight single-point
 * defects in that machinery passed every one of them.
 */
typedef unsigned int u32;
typedef unsigned char u8;

#define UART (*(volatile unsigned char *)0x10000000u)

/* ESR, table 7-3 */
#define ESR_PPR 0x04000000u
#define ESR_ST 0x00800000u
#define ESR_BO 0x00020000u
#define ESR_VLEMI 0x00000020u
#define ESR_MIF 0x00000002u

#define MSR_IS 0x00000020u
#define MSR_DS 0x00000010u

/* The MAS fields, tables 10-9 to 10-14. */
#define TLBSEL1 0x10000000u
#define V 0x80000000u
#define IPROT 0x40000000u
#define TS 0x00001000u
#define TID(n) ((u32)(n) << 16)
#define TSIZE(n) ((u32)(n) << 7)
#define K4 TSIZE(2) /* 2^2 KB */
#define M1 TSIZE(10) /* 2^10 KB */
#define VLE 0x20u
#define WIMG_I 0x08u
#define WIMG_G 0x02u
#define UX 0x20u
#define SX 0x10u
#define UW 0x08u
#define SW 0x04u
#define UR 0x02u
#define SR 0x01u

struct trap {
    u32 ivor, srr0, srr1, esr, dear, msr, count, resume, cr, r6;
};
volatile struct trap g_trap;
volatile u32 g_uval;

/* sys_stubs.S */
u32 sys_install(void);
u32 stub_load(u32 a), stub_store(u32 a, u32 v), stub_jump(u32 a);
u32 stub_user(void (*fn)(void), u32 unused, u32 arg);

/* mmu_stubs.S */
u32 rd_mas0(void), rd_mas1(void), rd_mas2(void), rd_mas3(void);
u32 rd_mas4(void), rd_mas6(void), rd_pid0(void);
u32 rd_mmucfg(void), rd_tlb0cfg(void), rd_tlb1cfg(void), rd_mmucsr0(void);
void wr_mas0(u32), wr_mas1(u32), wr_mas2(u32), wr_mas3(u32);
void wr_mas4(u32), wr_mas6(u32), wr_pid0(u32), wr_mmucsr0(u32);
void stub_tlbre(void), stub_tlbwe(void), stub_tlbsx(u32 ea),
    stub_tlbivax(u32 ea);
u32 stub_load_ds(u32 a), stub_jump_is(u32 a), stub_load_ds_shared(u32 a);
u32 stub_jump_is_r5(u32 target, u32 unused, u32 r5);
void u_tlbwe(void), u_tlbsx(void), u_load(void);
extern u8 mmu_page0[], mmu_page2[];

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

static void took(const char *name, u32 ret, u32 ivor, u32 esr)
{
    check(name, ret, ivor);
    check(name, g_trap.ivor, ivor);
    check(name, g_trap.esr, esr);
}

/* Two physical pages the mappings below point into. */
static volatile u8 g_pg[8192] __attribute__((aligned(4096)));
#define PAGE_A ((u32) & g_pg[0])
#define PAGE_B ((u32) & g_pg[4096])

static u32 rd32(u32 pa)
{
    return *(volatile u32 *)pa;
}
static void wr32(u32 pa, u32 v)
{
    *(volatile u32 *)pa = v;
}

static void tlb_write(u32 esel, u32 mas1, u32 mas2, u32 mas3)
{
    wr_mas0(TLBSEL1 | (esel << 16));
    wr_mas1(mas1);
    wr_mas2(mas2);
    wr_mas3(mas3);
    stub_tlbwe();
}

static void tlb_read(u32 esel)
{
    wr_mas0(TLBSEL1 | (esel << 16));
    stub_tlbre();
}

int main(void)
{
    (void)sys_install();

    /* --- what the MMU says it is (tables 10-3 and 10-5) ------------- */
    /* RASIZE 32, one PID register of 8 bits, one TLB. */
    check("mmucfg", rd_mmucfg(), 0x004009C4u);
    check("tlb0cfg", rd_tlb0cfg(), 0u);
    /* 32 entries, fully associative, 1 KB to 4 GB, IPROT supported. */
    check("tlb1cfg", rd_tlb1cfg(), 0x200BE020u);

    /*
     * --- the reset entry: the MODEL's, not table 10-7's --------------
     * The manual's entry 0 is one 4 KB page that a boot ROM extends;
     * this model has no ROM, so entry 0 is the whole 4 GB, identity,
     * every permission (ppc_mmu.c). Its other attributes are the
     * manual's: valid, IPROT, TS 0, TID 0, cache-inhibited, and the
     * image's encoding -- Book E here.
     */
    tlb_read(0u);
    check("reset-mas1", rd_mas1(), V | IPROT | TSIZE(22));
    check("reset-mas2", rd_mas2(), WIMG_I);
    check("reset-mas3", rd_mas3(), UX | SX | UW | SW | UR | SR);

    /* --- the MAS registers hold what they implement (10.6) ---------- */
    /* TLBSEL is "ignored" rather than reserved, so what it reads back
     * is not stated: only ESEL and NV are held to the table. */
    wr_mas0(0xFFFFFFFFu);
    check("mas0-impl", rd_mas0() & ~0x30000000u, 0x001F001Fu);
    wr_mas1(0xFFFFFFFFu);
    check("mas1-impl", rd_mas1(), 0xC0FF1F80u); /* V, IPROT, TID, TS, TSIZE */
    /* EPN, VLE, WIMG -- and E reads 0: the MODEL does not do
     * little-endian pages, and says so by refusing the bit. */
    wr_mas2(0xFFFFFFFFu);
    check("mas2-impl", rd_mas2(), 0xFFFFFC3Eu);
    wr_mas3(0xFFFFFFFFu);
    check("mas3-impl", rd_mas3(), 0xFFFFFFFFu); /* RPN, U0-U3, perms */
    wr_mas4(0xFFFFFFFFu);
    check("mas4-impl", rd_mas4(), 0x30030FBEu); /* DE as E, above */
    wr_mas6(0xFFFFFFFFu);
    check("mas6-impl", rd_mas6(), 0x00FF0001u);
    /* PID0 is eight bits on this core (10.2.3). */
    wr_pid0(0x1234u);
    check("pid0-8-bits", rd_pid0(), 0x34u);
    wr_pid0(0u);

    /* --- tlbwe and tlbre round-trip an entry (10.5.1, 10.5.2) ------- */
    tlb_write(17u, V | TID(0x5A) | TS | TSIZE(31), 0x12345C3Eu, 0xABCDE7FFu);
    tlb_read(17u);
    check("tlbre-mas1", rd_mas1(), V | TID(0x5A) | TS | TSIZE(31));
    check("tlbre-mas2", rd_mas2(), 0x12345C3Eu);
    check("tlbre-mas3", rd_mas3(), 0xABCDE7FFu);
    check("tlbre-keeps-mas0", rd_mas0(), TLBSEL1 | (17u << 16));
    tlb_write(17u, 0u, 0u, 0u);

    /*
     * MSR[DS] alone turns translation on: the reset entry is TS 0
     * (table 10-7), so a data access in address space 1 misses it.
     */
    took("reset-as1-miss", stub_load_ds(PAGE_A), 13u, 0u);
    check("reset-as1-miss-dear", g_trap.dear, PAGE_A);
    /*
     * jit-: the same load instructions, run first with translation off
     * and then with only MSR[DS] set -- no TLB write between, so the
     * generation does not move and only the context can tell the block
     * built the first time from the one this needs.
     */
    wr32(PAGE_A + 0x20u, 0x0A0A0020u);
    check("jit-load-identity", stub_load(PAGE_A + 0x20u), 0x0A0A0020u);
    took("jit-load-ds-same-code", stub_load_ds_shared(PAGE_A + 0x20u), 13u, 0u);

    /*
     * --- program the TLB as a start-up does -------------------------
     * RAM -- code, data, stack and the vectors -- in one protected
     * megabyte with every permission, the console in a protected 4 KB
     * page, and then the reset entry overwritten. From here on
     * translation is on and nothing is the identity by default.
     */
    tlb_write(1u, V | IPROT | M1, 0x80000000u,
              0x80000000u | UX | SX | UW | SW | UR | SR);
    tlb_write(2u, V | IPROT | K4, 0x10000000u | WIMG_I | WIMG_G,
              0x10000000u | SW | SR);
    tlb_write(20u, V | K4, 0x66000000u, (u32)mmu_page0 | SX | SR);
    tlb_write(0u, 0u, 0u, 0u);
    /*
     * Straight into a mapping only the TLB provides, with no MSR write
     * and no interrupt since the first tlbwe above. The interpreter
     * chooses its loop where a stretch begins, so here only the TLB
     * change itself can have told it to choose again.
     */
    check("first-fetch-after-startup", stub_jump(0x66000000u), 0x5Au);
    tlb_read(0u);
    check("reset-entry-gone", rd_mas1() & V, 0u);

    /* --- data translation (figures 10-1, 10-2) ---------------------- */
    wr32(PAGE_A + 0x10u, 0xAAAA0010u);
    wr32(PAGE_B + 0x10u, 0xBBBB0010u);
    tlb_write(3u, V | K4, 0x40000000u, PAGE_A | SW | SR);
    check("va-load", stub_load(0x40000010u), 0xAAAA0010u);
    check("va-store", stub_store(0x40000014u, 0x5151A5A5u), 0xAAu);
    check("va-store-lands", rd32(PAGE_A + 0x14u), 0x5151A5A5u);
    /* The bits below the page size are the offset, in EPN and in RPN
     * alike: compared on EA[0:11] and taken from RPN[0:11] for 1 MB. */
    tlb_write(5u, V | M1, 0x48055400u, 0x80055400u | SW | SR);
    check("va-1m-offset",
          stub_load(0x48000000u + (PAGE_A - 0x80000000u) + 0x10u), 0xAAAA0010u);

    /* --- an access that crosses into another page ------------------- */
    /* Two virtual pages, physically the wrong way round. */
    tlb_write(18u, V | K4, 0x41000000u, PAGE_B | SW | SR);
    tlb_write(19u, V | K4, 0x41001000u, PAGE_A | SW | SR);
    g_pg[4096 + 4094] = 0x11u;
    g_pg[4096 + 4095] = 0x22u;
    g_pg[0] = 0x33u;
    g_pg[1] = 0x44u;
    check("cross-load", stub_load(0x41000FFEu), 0x11223344u);
    check("cross-store", stub_store(0x41000FFFu, 0xA1B2C3D4u), 0xAAu);
    check("cross-store-b", g_pg[4096 + 4095], 0xA1u);
    check("cross-store-a", rd32(PAGE_A) >> 8, 0xB2C3D4u);
    /* 3.4: the miss is the second page's; DEAR "a byte of the load". */
    took("cross-miss", stub_load(0x40000FFEu), 13u, 0u);
    check("cross-miss-dear", g_trap.dear, 0x40001000u);

    /* --- a data TLB miss, and what it leaves in the MAS (table 10-15) */
    wr_mas4(TLBSEL1 | K4 | WIMG_I | WIMG_G); /* TIDSELD 0: PID0 */
    wr_mas0(TLBSEL1 | 3u); /* NV 3 */
    wr_pid0(0x21u);
    took("dtlb-load", stub_load(0x50000123u), 13u, 0u);
    check("dtlb-srr0", g_trap.srr0, (u32)&stub_load);
    check("dtlb-dear", g_trap.dear, 0x50000123u);
    check("dtlb-mas0", rd_mas0(), TLBSEL1 | (3u << 16) | 3u);
    check("dtlb-mas1", rd_mas1(), V | TID(0x21) | K4);
    check("dtlb-mas2", rd_mas2(), 0x50000000u | WIMG_I | WIMG_G);
    check("dtlb-mas3", rd_mas3(), 0u);
    check("dtlb-mas6", rd_mas6(), TID(0x21));
    took("dtlb-store", stub_store(0x50000200u, 1u), 13u, ESR_ST);
    /* TIDSELD 11 is TIDZ: the handler's entry is shared. */
    wr_mas4(TLBSEL1 | 0x00030000u | K4);
    took("dtlb-tidz", stub_load(0x50000000u), 13u, 0u);
    check("dtlb-tidz-mas1", rd_mas1(), V | K4);
    wr_pid0(0u);

    /* --- address space: MSR[DS] selects TS (10.3) ------------------- */
    tlb_write(4u, V | TS | K4, 0x40000000u, PAGE_B | SW | SR);
    check("as0-load", stub_load(0x40000010u), 0xAAAA0010u);
    check("as1-load", stub_load_ds(0x40000010u), 0xBBBB0010u);
    took("as1-miss", stub_load_ds(0x48000000u), 13u, 0u);
    check("as1-miss-ts", rd_mas1() & TS, TS);
    check("as1-miss-sas", rd_mas6() & 1u, 1u);
    /* 7.7.14: the handler runs in address space 0. */
    check("as1-miss-srr1-ds", g_trap.srr1 & MSR_DS, MSR_DS);
    check("as1-miss-handler-ds", g_trap.msr & (MSR_IS | MSR_DS), 0u);

    /* --- process ID: TID 0 is shared, others match PID0 alone ------- */
    tlb_write(6u, V | TID(5) | K4, 0x42000000u, PAGE_A | SW | SR);
    took("pid-mismatch", stub_load(0x42000010u), 13u, 0u);
    wr_pid0(5u);
    check("pid-match", stub_load(0x42000010u), 0xAAAA0010u);
    check("pid-shared", stub_load(0x40000010u), 0xAAAA0010u);
    wr_pid0(0u);

    /* --- permissions (figure 10-3, table 7-13) ---------------------- */
    tlb_write(7u, V | K4, 0x44000000u, PAGE_A | SR);
    check("ro-load", stub_load(0x44000010u), 0xAAAA0010u);
    took("ro-store", stub_store(0x44000010u, 0xDEADu), 2u, ESR_ST);
    check("ro-store-dear", g_trap.dear, 0x44000010u);
    check("ro-store-not-done", rd32(PAGE_A + 0x10u), 0xAAAA0010u);
    tlb_write(8u, V | K4, 0x45000000u, PAGE_A | SW);
    took("wo-load", stub_load(0x45000010u), 2u, 0u);
    check("wo-store", stub_store(0x45000018u, 0x0F0F0F0Fu), 0xAAu);
    check("wo-store-lands", rd32(PAGE_A + 0x18u), 0x0F0F0F0Fu);
    /* Supervisor-only, from problem state. Entry 1 grants U*, so the
     * routine itself runs; the control reads RAM through it. */
    g_uval = 0u;
    check("user-load-ok", stub_user(u_load, 0u, PAGE_A + 0x10u), 8u);
    check("user-load-ok-value", g_uval, 0xAAAA0010u);
    took("user-load-s-only", stub_user(u_load, 0u, 0x40000010u), 2u, 0u);
    check("user-load-s-only-dear", g_trap.dear, 0x40000010u);
    /* Every TLB instruction is privileged. */
    took("user-tlbwe", stub_user(u_tlbwe, 0u, 0u), 6u, ESR_PPR);
    took("user-tlbsx", stub_user(u_tlbsx, 0u, 0x40000000u), 6u, ESR_PPR);

    /* --- tlbsx (10.5.3, table 10-15) -------------------------------- */
    wr_mas4(TLBSEL1 | K4 | VLE | WIMG_G);
    wr_mas0(TLBSEL1 | 7u); /* NV 7 */
    wr_mas6(0u);
    stub_tlbsx(0x40000FFCu);
    check("tlbsx-hit-mas0", rd_mas0(), TLBSEL1 | (3u << 16) | 7u);
    check("tlbsx-hit-mas1", rd_mas1(), V | K4);
    check("tlbsx-hit-mas2", rd_mas2(), 0x40000000u);
    check("tlbsx-hit-mas3", rd_mas3(), PAGE_A | SW | SR);
    wr_mas6(TID(5) | 1u); /* searched as PID 5, address space 1 */
    stub_tlbsx(0x40000000u);
    check("tlbsx-as1-hit", rd_mas0(), TLBSEL1 | (4u << 16) | 7u);
    stub_tlbsx(0x42000000u);
    check("tlbsx-as1-not-as0", rd_mas1() & V, 0u);
    check("tlbsx-miss-mas0", rd_mas0(), TLBSEL1 | (7u << 16) | 7u);
    check("tlbsx-miss-mas1", rd_mas1(), TID(5) | TS | K4);
    check("tlbsx-miss-mas2", rd_mas2(), 0x42000000u | VLE | WIMG_G);
    check("tlbsx-miss-mas3", rd_mas3(), 0u);
    wr_mas6(TID(5));
    stub_tlbsx(0x42000000u);
    check("tlbsx-pid", rd_mas0(), TLBSEL1 | (6u << 16) | 7u);

    /* --- instruction translation ------------------------------------ */
    tlb_write(10u, V | K4, 0x60000000u, (u32)mmu_page0 | SX | SR);
    check("fetch-va", stub_jump(0x60000000u), 0x5Au);
    took("itlb", stub_jump(0x70000000u), 14u, 0u);
    check("itlb-srr0", g_trap.srr0, 0x70000000u);
    check("itlb-mas2-epn", rd_mas2() & 0xFFFFFC00u, 0x70000000u);
    took("itlb-is1", stub_jump_is(0x60000000u), 14u, 0u);
    /* jit-: fetch-va just ran this address in address space 0. The miss
     * is at the target; a block reused across MSR[IS] runs it and
     * misses at the return instead, with every other field the same. */
    check("jit-itlb-is1-srr0", g_trap.srr0, 0x60000000u);
    check("itlb-is1-ts", rd_mas1() & TS, TS);
    check("itlb-is1-srr1", g_trap.srr1 & MSR_IS, MSR_IS);
    tlb_write(11u, V | K4, 0x61000000u, (u32)mmu_page0 | SR);
    took("isi-no-sx", stub_jump(0x61000000u), 3u, 0u);
    check("isi-srr0", g_trap.srr0, 0x61000000u);
    /* 2.4.6.2: bcctr keeps bit 62, and a Book E page at an odd
     * halfword is a misaligned fetch. */
    took("isi-mif", stub_jump(0x60000002u), 3u, ESR_MIF);
    check("isi-mif-srr0", g_trap.srr0, 0x60000002u);

    /* --- the encoding is the page's (MAS2[VLE]) --------------------- */
    tlb_write(12u, V | K4, 0x62000000u | VLE, (u32)mmu_page0 | SX | SR);
    check("vle-page", stub_jump(0x62000100u), 0x55u);
    tlb_write(15u, V | K4, 0x64000000u | VLE, (u32)mmu_page0 | SR);
    took("isi-vle", stub_jump(0x64000100u), 3u, ESR_VLEMI);
    /* A 32-bit VLE instruction across a page boundary (table 7-14,
     * 2.4.6.2): the second page missing, Book E, and VLE. */
    tlb_write(13u, V | K4, 0x63000000u | VLE, (u32)mmu_page0 | SX | SR);
    took("straddle-itlb", stub_jump(0x63000FFEu), 14u, ESR_MIF);
    check("straddle-itlb-srr0", g_trap.srr0, 0x63000FFEu);
    check("straddle-itlb-mas2", rd_mas2() & 0xFFFFFC00u, 0x63001000u);
    tlb_write(14u, V | K4, 0x63001000u, ((u32)mmu_page0 + 0x1000u) | SX | SR);
    took("straddle-bo", stub_jump(0x63000FFEu), 3u, ESR_BO | ESR_VLEMI);
    tlb_write(14u, V | K4, 0x63001000u | VLE,
              ((u32)mmu_page0 + 0x1000u) | SX | SR);
    check("straddle-ok", stub_jump(0x63000FFEu), 0x77u);

    /* --- the JIT's assumptions, one at a time ----------------------- */
    /*
     * The mapping: an address run through entry 1's identity, then
     * remapped by entry 0 -- the lowest-numbered entry wins -- to other
     * code, then back. A block outliving the tlbwe runs the old code; a
     * translator that fetched the effective address as a physical one
     * runs page 2's bytes where page 0's are mapped.
     */
    check("jit-map-identity", stub_jump((u32)mmu_page2), 0xA2u);
    tlb_write(0u, V | K4, (u32)mmu_page2, (u32)mmu_page0 | SX | SR);
    check("jit-map-remapped", stub_jump((u32)mmu_page2), 0x5Au);
    tlb_write(0u, 0u, 0u, 0u);
    check("jit-map-back", stub_jump((u32)mmu_page2), 0xA2u);
    /*
     * PID0: one address, two entries by TID, two pages. Changing PID0 is
     * an mtspr, which moves no generation; only the context separates
     * the two blocks.
     */
    tlb_write(21u, V | TID(7) | K4, 0x69000000u, (u32)mmu_page0 | SX | SR);
    tlb_write(22u, V | TID(8) | K4, 0x69000000u, (u32)mmu_page2 | SX | SR);
    wr_pid0(7u);
    check("jit-pid-7", stub_jump(0x69000000u), 0x5Au);
    wr_pid0(8u);
    check("jit-pid-8", stub_jump(0x69000000u), 0xA2u);
    wr_pid0(0u);
    /*
     * Problem state: SX and not UX. Run once as supervisor, which leaves
     * by a system call, then as user, which must not run at all.
     */
    tlb_write(23u, V | K4, 0x6B000000u, (u32)mmu_page2 | SX | SR);
    g_uval = 0u;
    check("jit-s-exec", stub_jump(0x6B000200u), 8u);
    check("jit-s-exec-ran", g_uval, 0xA3u);
    g_uval = 0u;
    took("jit-u-exec-no-ux", stub_user((void (*)(void))0x6B000200u, 0u, 0u), 3u,
         0u);
    check("jit-u-exec-srr0", g_trap.srr0, 0x6B000200u);
    check("jit-u-exec-not-run", g_uval, 0u);
    /*
     * The same two, from a block. jit-u-exec-no-ux above enters the
     * user routine straight from an rfi, which the fallback interprets
     * past -- so it cannot see a block cached in supervisor state, and
     * did not. mmu_caller loops first and then calls; the call is a
     * lookup, under the new mode. X at 0x6B000000 was run in supervisor
     * state just now (jit-s-exec) and is SX only.
     */
    tlb_write(24u, V | K4, 0x6E000000u, (u32)mmu_page2 | UX | SX | UR | SR);
    check("jit-s-exec-x", stub_jump(0x6B000000u), 0xA2u);
    g_uval = 0u;
    check("jit-u-call-from-block",
          stub_user((void (*)(void))0x6E000300u, 0u, 0x6B000000u), 8u);
    /* The ISI's number, stored by the caller the recorder returned to;
     * its SRR0 is gone by now, overwritten by the closing system call. */
    check("jit-u-call-isi", g_uval, 3u);
    /*
     * Address space: one address, page 0 in TS 0 and page 2 in TS 1.
     * Run in address space 0, then called from a block in address space
     * 1. MSR[IS] is changed by an rfi, which moves no generation.
     */
    tlb_write(25u, V | K4, 0x6D000000u, (u32)mmu_page0 | SX | SR);
    tlb_write(26u, V | TS | K4, 0x6D000000u, (u32)mmu_page2 | SX | SR);
    tlb_write(27u, V | TS | K4, 0x6C000000u, (u32)mmu_page2 | SX | SR);
    check("jit-is0", stub_jump(0x6D000000u), 0x5Au);
    g_uval = 0u;
    check("jit-is1-call-from-block",
          stub_jump_is_r5(0x6C000300u, 0u, 0x6D000000u), 8u);
    check("jit-is1-ran-ts1", g_uval, 0xA2u);
    for (u32 e = 21u; e <= 27u; e++) {
        tlb_write(e, 0u, 0u, 0u);
    }

    /* --- invalidation (10.5.4, 10.8) -------------------------------- */
    /* By EPN alone: TS and TID are not compared, so both entries for
     * 0x40000000 go. TLBSEL (EA bit 28) set, as the manual asks. */
    stub_tlbivax(0x40000123u | 0x8u);
    took("ivax-as0", stub_load(0x40000010u), 13u, 0u);
    took("ivax-as1", stub_load_ds(0x40000010u), 13u, 0u);
    check("ivax-other", stub_load(0x44000010u), 0xAAAA0010u);
    tlb_write(16u, V | IPROT | K4, 0x47000000u, PAGE_A | SW | SR);
    stub_tlbivax(0x47000000u | 0x8u);
    check("ivax-iprot", stub_load(0x47000010u), 0xAAAA0010u);
    /* INV_ALL, EA bit 29: everything without IPROT. */
    stub_tlbivax(0x4u | 0x8u);
    took("ivax-all", stub_load(0x44000010u), 13u, 0u);
    check("ivax-all-iprot", stub_load(0x47000010u), 0xAAAA0010u);
    /* MMUCSR0[TLB1_FI], the same, and it reads back 0 (table 10-8). */
    tlb_write(3u, V | K4, 0x40000000u, PAGE_A | SW | SR);
    check("fi-before", stub_load(0x40000010u), 0xAAAA0010u);
    wr_mmucsr0(0x2u);
    check("fi-reads-0", rd_mmucsr0(), 0u);
    took("fi", stub_load(0x40000010u), 13u, 0u);
    check("fi-iprot", stub_load(0x47000010u), 0xAAAA0010u);

    ps_("PPC-MMU-END checks=");
    phex(g_checks);
    ps_(" fails=");
    phex(g_fail);
    pc_('\n');
    return (int)g_fail;
}
