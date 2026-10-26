/* SPDX-License-Identifier: Apache-2.0 */
/*
 * fuzz_ppc_jit.c - the PowerPC (e200z7) interpreter against the x86-64
 * JIT.
 *
 * The input is a configuration word and a program. The first four
 * bytes (little-endian) choose VLE or Book E, supervisor or user state,
 * whether the SPE/EFPU2 unit is available, and SPEFSCR's rounding and
 * exception enables; the rest is the program, in the guest's byte order
 * -- big-endian -- copied to the start of a read-only region that is
 * otherwise `wait`, and both backends run it from there on cores and
 * buses built identically.
 *
 * The interpreter is the reference, and as for G4MH it and the manual
 * are all there is: no e200 is attached and no other emulator is
 * consulted, so this finds where the two backends read the manual
 * differently and nothing about whether either read it right.
 *
 * How every run ends at the same instruction on both sides:
 *
 *   - `wait` parks the core and the run reports WFI. MSR[EE] starts
 *     clear. 0x7C00007C is `wait` in both encodings, VLE keeping opcode
 *     31's X-forms as they are.
 *   - IVPR names a table in ROM that is `wait` from end to end, and
 *     IVORn is 16n, so every interrupt parks one instruction into its
 *     own slot and the pc says which it was. The table runs on to
 *     cover the INTC's 337 hardware vectors too (IVPR | vector << 4),
 *     which therefore share the low slots' addresses.
 *   - The core's INTC is mapped where the frontend puts it, so a
 *     program can set priorities, raise its eight software-settable
 *     sources, acknowledge through IACKR and end through EOIR, in
 *     either vector mode. A write after which the core would take an
 *     interrupt at once sets the run aside (fuzz_harness.h); one made
 *     takeable by wrteei, mtmsr or an rfi is compared, because the
 *     translator declines all three.
 *   - lr starts at a `wait` too, so `blr` ends a program that reads
 *     like a function.
 *   - Code is ROM and data RAM is not executable, for the reasons the
 *     RV32 target gives.
 *   - `sc` reaches no syscall hook: it is the architectural interrupt.
 *
 * A second core, when the configuration asks for it: core 1 runs the
 * same program once core 0 parks, on its own bus with its own INTC, over
 * shared memory, both marked as running together (what ppc_cpu_start
 * does), so stores go the way that breaks the other core's reservation.
 * The JIT's blocks are the thread's, so a block core 0 built is the one
 * core 1 runs. Neither core can reach the other's INTC, so a parked
 * core stays parked; what this exercises is the blocks, the reservation
 * path and the shared memory, not wake-ups.
 *
 * Built only with -DEMU_FUZZ=ON; see scripts/fuzz.sh, docs/fuzzing.md
 * and fuzz_harness.h.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "emu/emu_bus.h"
#include "ppc/ppc_cpu.h"
#include "ppc/ppc_decode.h"
#include "ppc/ppc_disasm.h"
#include "ppc/ppc_intc.h"

#include "fuzz_harness.h"

#if PPC_CORE_COUNT > 1
#define NCORES 2u
#else
#define NCORES 1u
#endif

#define CODE_BASE 0x20000000u
#define CODE_SIZE 0x1000u /* the program, then `wait` */
#define TRAP_BASE 0x20010000u
#define TRAP_SIZE 0x1800u /* IVOR 0..35 and the INTC's vectors, 16 each */
#define LR_INIT (TRAP_BASE + 0x3F0u)
#define RAM_BASE 0x80000000u
#define RAM_SIZE 0x4000u
#define REG_BASE (RAM_BASE + 0x2000u) /* r0-r31 start pointing here */

#define WAIT 0x7C00007Cu

/* The configuration word: the input's first four bytes. */
#define CFG_BOOKE (1u << 0) /* Book E, not VLE                   */
#define CFG_PR (1u << 1) /* MSR[PR]: user state               */
#define CFG_NOSPE (1u << 2) /* MSR[SPE] clear                    */
#define CFG_FRMC_SHIFT 3 /* SPEFSCR[FRMC], two bits           */
#define CFG_FXE_SHIFT 5 /* SPEFSCR's five exception enables  */
#define CFG_ME (1u << 10) /* MSR[ME]                           */
#define CFG_CORE1 (1u << 11) /* a second core, if the build has one */

typedef struct side {
    fz_side_t run; /* first: before_run is handed this */
    ppc_cpu_t cpu[NCORES];
    ppc_intc_t intc[NCORES];
    fz_dev_t intc_dev[NCORES];
    emu_bus_t bus[NCORES];
    uint8_t ram[RAM_SIZE];
} side_t;

static uint8_t g_code[CODE_SIZE];
static size_t g_len;
static uint32_t g_cfg;
static uint8_t g_trap[TRAP_SIZE];
static uint8_t g_ram_init[RAM_SIZE];
static unsigned g_ncores; /* this input's */

static side_t g_interp = {
    .run = {.name = "interp", .backend = &ppc_backend_interp}};
static side_t g_jit = {.run = {.name = "jit", .backend = &ppc_backend_jit}};

static void dump_program(void);

/*
 * A store breaks the reservations of the cores the frontend knows by id,
 * so those are the side about to run -- both sides are built before
 * either runs, and the last built would otherwise be the one told.
 */
static void before_run(fz_side_t *run)
{
    side_t *const sd = (side_t *)(void *)run;

    for (unsigned k = 0; k < NCORES; k++) {
        ppc_cpu_register(&sd->cpu[k]);
    }
}

static const fz_target_t g_target = {
    .name = "fuzz-ppc-jit",
    .active = &ppc_backend,
    .budget = 20000u,
    .slice = 1024u,
    .before_run = before_run,
    .after_run = NULL,
    .dump = dump_program,
};

static void put32be(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static void fill_wait(uint8_t *p, size_t n)
{
    for (size_t i = 0; i + 4u <= n; i += 4u) {
        put32be(&p[i], WAIT);
    }
}

static void once(void)
{
    static bool done;

    if (done) {
        return;
    }
    done = true;
    fill_wait(g_trap, TRAP_SIZE);
    fz_fill(g_ram_init, RAM_SIZE, 0x9E3779B9u);
    for (unsigned k = 0; k < NCORES; k++) {
        g_interp.run.cores[k] = (emu_cpu_t *)(void *)&g_interp.cpu[k];
        g_jit.run.cores[k] = (emu_cpu_t *)(void *)&g_jit.cpu[k];
    }
    g_interp.run.cpu = g_interp.run.cores[0];
    g_jit.run.cpu = g_jit.run.cores[0];
    fz_start(&g_target, &ppc_backend_jit, g_jit.run.cpu);
}

/*
 * After a write to the INTC: whether the core would now take an
 * interrupt before its next instruction. See fuzz_harness.h.
 */
static unsigned intc_after_write(fz_dev_t *d, uint32_t off, uint32_t size)
{
    (void)off;
    (void)size;
    return (ppc_cpu_pending_irq((const ppc_cpu_t *)(void *)fz_dev_core(d)) >= 0)
               ? FZ_ASIDE_ASYNC
               : 0u;
}

/* Core k: its bus, its INTC, its state. */
static void build_core(side_t *sd, unsigned k)
{
    const emu_region_t ram = {
        .base = RAM_BASE,
        .size = RAM_SIZE,
        .host = sd->ram,
        .name = "ram",
        .kind = EMU_MEM_RAM,
        .perm = EMU_PERM_RW, /* not X: see the header */
        .widths = EMU_WANY,
    };
    ppc_cpu_t *const c = &sd->cpu[k];
    emu_bus_t *const bus = &sd->bus[k];
    uint32_t msr;

    emu_bus_init(bus);
    if (!emu_bus_add(bus, &ram) ||
        !emu_bus_add_rom(bus, "code", CODE_BASE, g_code, CODE_SIZE) ||
        !emu_bus_add_rom(bus, "trap", TRAP_BASE, g_trap, TRAP_SIZE) ||
        !emu_bus_add_mmio(bus, "intc",
                          (k == 0u) ? PPC_INTC0_BASE : PPC_INTC1_BASE,
                          PPC_INTC_SIZE, &fz_dev_ops, &sd->intc_dev[k])) {
        fprintf(stderr, "%s: could not build core %u's bus\n", g_target.name,
                k);
        abort();
    }

    /*
     * What ppc_ops_init, ppc_ops_reset and ppc_ops_boot do, on our own
     * core: the encoding is chosen after the reset, and the reset TLB
     * entry carries it (MAS2[VLE]), so the MMU is reset again after.
     * No STM or SWT: the core runs without them.
     */
    ppc_cpu_init(c, bus, k);
    ppc_cpu_reset(c, CODE_BASE);
    c->vle = (g_cfg & CFG_BOOKE) == 0u;
    ppc_mmu_reset(c);
    ppc_intc_reset(&sd->intc[k]);
    sd->intc[k].core = c;
    c->intc = &sd->intc[k];
    sd->intc_dev[k] = (fz_dev_t){
        .ops = &ppc_intc_ops,
        .ctx = &sd->intc[k],
        .side = &sd->run,
        .cpu = (emu_cpu_t *)(void *)c,
        .after_write = intc_after_write,
    };
    if (g_ncores > 1u) {
        /* What ppc_cpu_start and ppc_cpu_take_start leave: every core
         * live, stores the slow way, and core 1 out of its reset hold. */
        c->smp_live = 1u;
        c->slow_store = true;
        c->state = EMU_STATE_RUNNING;
    }

    for (uint32_t i = 0; i < PPC_NGPR; i++) {
        c->r[i] = REG_BASE + i * 32u;
    }
    c->lr = LR_INIT;
    c->ctr = 16u;
    c->ivpr = TRAP_BASE;
    for (uint32_t i = 0; i < PPC_IVOR_COUNT; i++) {
        c->ivor[i] = i * 16u;
    }
    c->spefscr = ((g_cfg >> CFG_FRMC_SHIFT) & PPC_SPEFSCR_FRMC) |
                 (((g_cfg >> CFG_FXE_SHIFT) & 0x1Fu) << 2);
    msr = ((g_cfg & CFG_PR) ? PPC_MSR_PR : 0u) |
          ((g_cfg & CFG_NOSPE) ? 0u : PPC_MSR_SPE) |
          ((g_cfg & CFG_ME) ? PPC_MSR_ME : 0u);
    ppc_cpu_set_msr(c, msr);
    c->irq_dirty = false;
    c->jit_ctx = ppc_cpu_ctx(c);
}

/* The same machine on each side, from the same state. */
static void build(side_t *sd)
{
    memcpy(sd->ram, g_ram_init, RAM_SIZE);
    for (unsigned k = 0; k < NCORES; k++) {
        build_core(sd, k); /* both, so the registry never holds a stale one */
    }
    sd->run.ncores = g_ncores;
}

/*
 * Whether the core stopped in the decrementer's, the fixed-interval
 * timer's or the watchdog's slot -- one `wait` in, as every handler
 * stops. A program that sets HID0[TBEN], a TCR enable and MSR[EE] (the
 * guests' own start-up does) takes one, and when it does is the
 * backend's to choose to a block; see fz_asynchronous.
 */
static bool timer_interrupt(const ppc_cpu_t *c)
{
    for (uint32_t i = PPC_IVOR_DECREMENTER; i <= PPC_IVOR_WATCHDOG; i++) {
        if (c->pc == TRAP_BASE + i * 16u + 4u) {
            return true;
        }
    }
    return false;
}

/* Core k's field, named core<k>.<field> when there is a second core. */
#define SAME(field)                                                            \
    fz_diff(&g_target, core(k, #field), 0u, g_interp.cpu[k].field,             \
            g_jit.cpu[k].field)
#define SAME_AT(field, i)                                                      \
    fz_diff(&g_target, core(k, #field), (unsigned)(i),                         \
            g_interp.cpu[k].field[i], g_jit.cpu[k].field[i])

static const char *core(unsigned k, const char *field)
{
    static char buf[2][48];

    if (g_ncores == 1u) {
        return field;
    }
    snprintf(buf[k & 1u], sizeof(buf[0]), "core%u.%s", k, field);
    return buf[k & 1u];
}

static void dump_program(void)
{
    const bool vle = (g_cfg & CFG_BOOKE) == 0u;
    char buf[96];

    fprintf(stderr,
            "  (ram[n] is the word at 0x%08x + 4n, as the host "
            "holds it: the guest's bytes in order)\n",
            RAM_BASE);
    fprintf(stderr,
            "  config 0x%08x (%s, %u core%s); program, %zu bytes at 0x%08x:\n",
            g_cfg, vle ? "VLE" : "Book E", g_ncores,
            (g_ncores == 1u) ? "" : "s", g_len, CODE_BASE);
    for (size_t off = 0; off < g_len;) {
        const uint16_t w0 = (uint16_t)((g_code[off] << 8) | g_code[off + 1]);
        const unsigned n = vle ? ppc_vle_len(w0) : 4u;
        const uint16_t w1 =
            (n == 4u) ? (uint16_t)((g_code[off + 2] << 8) | g_code[off + 3])
                      : 0u;
        const uint32_t insn = ((uint32_t)w0 << 16) | w1;

        ppc_disasm_mode(buf, sizeof(buf), CODE_BASE + (uint32_t)off, insn, n,
                        vle);
        if (n == 4u) {
            fprintf(stderr, "    %08x: %08x  %s\n", CODE_BASE + (unsigned)off,
                    insn, buf);
        } else {
            fprintf(stderr, "    %08x: %04x      %s\n",
                    CODE_BASE + (unsigned)off, w0, buf);
        }
        off += n;
    }
}

static void compare_core(unsigned k)
{
    const ppc_intc_t *const a = &g_interp.intc[k];
    const ppc_intc_t *const b = &g_jit.intc[k];

    for (unsigned i = 0; i < PPC_NGPR; i++) {
        SAME_AT(r, i);
    }
    SAME(pc);
    SAME(cr);
    SAME(xer);
    SAME(lr);
    SAME(ctr);
    SAME(msr);
    SAME(spefscr);
    SAME(state);
    SAME(vle);
    SAME(reserve);
    if (g_interp.cpu[k].reserve) {
        SAME(resv_pa);
    }
    for (unsigned i = 0; i < 10u; i++) {
        SAME_AT(sprg, i);
    }
    SAME(usprg0);
    SAME(srr0);
    SAME(srr1);
    SAME(csrr0);
    SAME(csrr1);
    SAME(dsrr0);
    SAME(dsrr1);
    SAME(mcsrr0);
    SAME(mcsrr1);
    SAME(dear);
    SAME(esr);
    SAME(mcsr);
    SAME(mcar);
    SAME(ivpr);
    for (unsigned i = 0; i < PPC_IVOR_COUNT; i++) {
        SAME_AT(ivor, i);
    }
    SAME(tsr);
    SAME(tcr);
    SAME(pid0);
    SAME(hid0);
    SAME(hid1);
    SAME(l1csr0);
    SAME(l1csr1);
    SAME(bucsr);
    for (unsigned i = 0; i < 7u; i++) {
        SAME_AT(dbcr, i);
    }
    SAME(dbsr);
    SAME(tb);
    SAME(dec);
    SAME(decar);
    SAME(retired);
    SAME(cycles);
    for (unsigned i = 0; i < PPC_TLB_ENTRIES; i++) {
        fz_diff(&g_target, core(k, "tlb.mas1"), i, g_interp.cpu[k].tlb[i].mas1,
                g_jit.cpu[k].tlb[i].mas1);
        fz_diff(&g_target, core(k, "tlb.mas2"), i, g_interp.cpu[k].tlb[i].mas2,
                g_jit.cpu[k].tlb[i].mas2);
        fz_diff(&g_target, core(k, "tlb.mas3"), i, g_interp.cpu[k].tlb[i].mas3,
                g_jit.cpu[k].tlb[i].mas3);
    }
    SAME(mas0);
    SAME(mas1);
    SAME(mas2);
    SAME(mas3);
    SAME(mas4);
    SAME(mas6);
    SAME(mmu_on);
    SAME(ext_pending);
    fz_diff(&g_target, core(k, "intc.bcr"), 0u, a->bcr, b->bcr);
    fz_diff(&g_target, core(k, "intc.cpr"), 0u, a->cpr, b->cpr);
    fz_diff(&g_target, core(k, "intc.vtba"), 0u, a->vtba, b->vtba);
    fz_diff(&g_target, core(k, "intc.intvec"), 0u, a->intvec, b->intvec);
    fz_diff(&g_target, core(k, "intc.intpri"), 0u, a->intpri, b->intpri);
    fz_diff(&g_target, core(k, "intc.req"), 0u, a->req, b->req);
    fz_diff(&g_target, core(k, "intc.lifo_head"), 0u, a->lifo_head,
            b->lifo_head);
    fz_diff(&g_target, core(k, "intc.lifo_depth"), 0u, a->lifo_depth,
            b->lifo_depth);
    for (unsigned i = 0; i < PPC_INTC_LIFO; i++) {
        fz_diff(&g_target, core(k, "intc.lifo"), i, a->lifo[i], b->lifo[i]);
    }
    fz_diff_mem(&g_target, core(k, "intc.psr"), a->psr, b->psr,
                sizeof(b->psr) & ~(size_t)3);
    fz_diff(&g_target, core(k, "intc.psr"), PPC_INTC_NSRC - 1u,
            a->psr[PPC_INTC_NSRC - 1u],
            b->psr[PPC_INTC_NSRC - 1u]); /* the odd one out */
    for (unsigned i = 0; i < (PPC_INTC_NSRC + 31u) / 32u; i++) {
        fz_diff(&g_target, core(k, "intc.flag"), i, a->flag[i], b->flag[i]);
    }
}

static void compare(void)
{
    fz_diff(&g_target, "reason", 0u, g_interp.run.reason, g_jit.run.reason);
    for (unsigned k = 0; k < g_ncores; k++) {
        compare_core(k);
    }
    fz_diff_mem(&g_target, "ram", g_interp.ram, g_jit.ram, RAM_SIZE);
    fz_verdict(&g_target, &g_interp.run, &g_jit.run);
    /* Stopped in External Input's slot, or acknowledged something. */
    for (unsigned k = 0; k < g_ncores; k++) {
        if (g_interp.cpu[k].pc == TRAP_BASE + PPC_IVOR_EXTERNAL * 16u + 4u ||
            g_interp.intc[k].lifo_depth != 0u) {
            fz_took_interrupt();
            break;
        }
    }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    once();
    g_cfg = 0u;
    if (size >= 4u) {
        g_cfg = (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
                ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
        data += 4;
        size -= 4u;
    } else {
        size = 0u;
    }
    /*
     * Whole parcels -- halfwords in VLE, words in Book E -- so the
     * `wait`s begin exactly where the program ends.
     */
    g_ncores = (NCORES > 1u && (g_cfg & CFG_CORE1) != 0u) ? NCORES : 1u;
    g_len = (size < CODE_SIZE - 8u) ? size : CODE_SIZE - 8u;
    g_len &= (g_cfg & CFG_BOOKE) ? ~(size_t)3 : ~(size_t)1;
    memset(g_code, 0, CODE_SIZE);
    memcpy(g_code, data, g_len);
    fill_wait(g_code + g_len, CODE_SIZE - g_len);

    build(&g_interp);
    build(&g_jit);
    if (fz_pair(&g_target, &g_interp.run, &g_jit.run)) {
        bool timer = false;

        for (unsigned k = 0; k < g_ncores; k++) {
            timer = timer || timer_interrupt(&g_interp.cpu[k]) ||
                    timer_interrupt(&g_jit.cpu[k]);
        }
        if (timer) {
            fz_asynchronous();
        } else {
            compare();
        }
    }
    return 0;
}
