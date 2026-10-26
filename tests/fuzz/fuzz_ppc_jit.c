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

typedef struct side {
    fz_side_t run;
    ppc_cpu_t cpu;
    ppc_intc_t intc;
    fz_dev_t intc_dev;
    emu_bus_t bus;
    uint8_t ram[RAM_SIZE];
} side_t;

static uint8_t g_code[CODE_SIZE];
static size_t g_len;
static uint32_t g_cfg;
static uint8_t g_trap[TRAP_SIZE];
static uint8_t g_ram_init[RAM_SIZE];

static side_t g_interp = {
    .run = {.name = "interp", .backend = &ppc_backend_interp}};
static side_t g_jit = {.run = {.name = "jit", .backend = &ppc_backend_jit}};

static void dump_program(void);

static const fz_target_t g_target = {
    .name = "fuzz-ppc-jit",
    .active = &ppc_backend,
    .budget = 20000u,
    .slice = 1024u,
    .before_run = NULL,
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
    g_interp.run.cpu = (emu_cpu_t *)(void *)&g_interp.cpu;
    g_jit.run.cpu = (emu_cpu_t *)(void *)&g_jit.cpu;
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
    return (ppc_cpu_pending_irq((const ppc_cpu_t *)(void *)d->side->cpu) >= 0)
               ? FZ_ASIDE_ASYNC
               : 0u;
}

/* The same machine on each side, from the same state. */
static void build(side_t *sd)
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
    ppc_cpu_t *c = &sd->cpu;
    uint32_t msr;

    emu_bus_init(&sd->bus);
    memcpy(sd->ram, g_ram_init, RAM_SIZE);
    if (!emu_bus_add(&sd->bus, &ram) ||
        !emu_bus_add_rom(&sd->bus, "code", CODE_BASE, g_code, CODE_SIZE) ||
        !emu_bus_add_rom(&sd->bus, "trap", TRAP_BASE, g_trap, TRAP_SIZE) ||
        !emu_bus_add_mmio(&sd->bus, "intc", PPC_INTC0_BASE, PPC_INTC_SIZE,
                          &fz_dev_ops, &sd->intc_dev)) {
        fprintf(stderr, "%s: could not build the bus\n", g_target.name);
        abort();
    }

    /*
     * What ppc_ops_init, ppc_ops_reset and ppc_ops_boot do, on our own
     * core: the encoding is chosen after the reset, and the reset TLB
     * entry carries it (MAS2[VLE]), so the MMU is reset again after.
     * No INTC, STM or SWT: the core runs without them.
     */
    ppc_cpu_init(c, &sd->bus, 0u);
    ppc_cpu_reset(c, CODE_BASE);
    c->vle = (g_cfg & CFG_BOOKE) == 0u;
    ppc_mmu_reset(c);
    ppc_intc_reset(&sd->intc);
    sd->intc.core = c;
    c->intc = &sd->intc;
    sd->intc_dev = (fz_dev_t){
        .ops = &ppc_intc_ops,
        .ctx = &sd->intc,
        .side = &sd->run,
        .after_write = intc_after_write,
    };

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

#define SAME(field)                                                            \
    fz_diff(&g_target, #field, 0u, g_interp.cpu.field, g_jit.cpu.field)
#define SAME_AT(field, i)                                                      \
    fz_diff(&g_target, #field, (unsigned)(i), g_interp.cpu.field[i],           \
            g_jit.cpu.field[i])

static void dump_program(void)
{
    const bool vle = (g_cfg & CFG_BOOKE) == 0u;
    char buf[96];

    fprintf(stderr,
            "  (ram[n] is the word at 0x%08x + 4n, as the host "
            "holds it: the guest's bytes in order)\n",
            RAM_BASE);
    fprintf(stderr, "  config 0x%08x (%s); program, %zu bytes at 0x%08x:\n",
            g_cfg, vle ? "VLE" : "Book E", g_len, CODE_BASE);
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

static void compare(void)
{
    fz_diff(&g_target, "reason", 0u, g_interp.run.reason, g_jit.run.reason);
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
    if (g_interp.cpu.reserve) {
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
        fz_diff(&g_target, "tlb.mas1", i, g_interp.cpu.tlb[i].mas1,
                g_jit.cpu.tlb[i].mas1);
        fz_diff(&g_target, "tlb.mas2", i, g_interp.cpu.tlb[i].mas2,
                g_jit.cpu.tlb[i].mas2);
        fz_diff(&g_target, "tlb.mas3", i, g_interp.cpu.tlb[i].mas3,
                g_jit.cpu.tlb[i].mas3);
    }
    SAME(mas0);
    SAME(mas1);
    SAME(mas2);
    SAME(mas3);
    SAME(mas4);
    SAME(mas6);
    SAME(mmu_on);
    SAME(ext_pending);
    fz_diff(&g_target, "intc.bcr", 0u, g_interp.intc.bcr, g_jit.intc.bcr);
    fz_diff(&g_target, "intc.cpr", 0u, g_interp.intc.cpr, g_jit.intc.cpr);
    fz_diff(&g_target, "intc.vtba", 0u, g_interp.intc.vtba, g_jit.intc.vtba);
    fz_diff(&g_target, "intc.intvec", 0u, g_interp.intc.intvec,
            g_jit.intc.intvec);
    fz_diff(&g_target, "intc.intpri", 0u, g_interp.intc.intpri,
            g_jit.intc.intpri);
    fz_diff(&g_target, "intc.req", 0u, g_interp.intc.req, g_jit.intc.req);
    fz_diff(&g_target, "intc.lifo_head", 0u, g_interp.intc.lifo_head,
            g_jit.intc.lifo_head);
    fz_diff(&g_target, "intc.lifo_depth", 0u, g_interp.intc.lifo_depth,
            g_jit.intc.lifo_depth);
    for (unsigned i = 0; i < PPC_INTC_LIFO; i++) {
        fz_diff(&g_target, "intc.lifo", i, g_interp.intc.lifo[i],
                g_jit.intc.lifo[i]);
    }
    fz_diff_mem(&g_target, "intc.psr", g_interp.intc.psr, g_jit.intc.psr,
                sizeof(g_jit.intc.psr) & ~(size_t)3);
    fz_diff(&g_target, "intc.psr", PPC_INTC_NSRC - 1u,
            g_interp.intc.psr[PPC_INTC_NSRC - 1u],
            g_jit.intc.psr[PPC_INTC_NSRC - 1u]); /* the odd one out */
    for (unsigned i = 0; i < (PPC_INTC_NSRC + 31u) / 32u; i++) {
        fz_diff(&g_target, "intc.flag", i, g_interp.intc.flag[i],
                g_jit.intc.flag[i]);
    }
    fz_diff_mem(&g_target, "ram", g_interp.ram, g_jit.ram, RAM_SIZE);
    fz_verdict(&g_target, &g_interp.run, &g_jit.run);
    /* Stopped in External Input's slot, or acknowledged something. */
    if (g_interp.cpu.pc == TRAP_BASE + PPC_IVOR_EXTERNAL * 16u + 4u ||
        g_interp.intc.lifo_depth != 0u) {
        fz_took_interrupt();
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
    g_len = (size < CODE_SIZE - 8u) ? size : CODE_SIZE - 8u;
    g_len &= (g_cfg & CFG_BOOKE) ? ~(size_t)3 : ~(size_t)1;
    memset(g_code, 0, CODE_SIZE);
    memcpy(g_code, data, g_len);
    fill_wait(g_code + g_len, CODE_SIZE - g_len);

    build(&g_interp);
    build(&g_jit);
    if (fz_pair(&g_target, &g_interp.run, &g_jit.run)) {
        if (timer_interrupt(&g_interp.cpu) || timer_interrupt(&g_jit.cpu)) {
            fz_asynchronous();
        } else {
            compare();
        }
    }
    return 0;
}
