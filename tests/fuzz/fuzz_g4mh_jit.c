/* SPDX-License-Identifier: Apache-2.0 */
/*
 * fuzz_g4mh_jit.c - the G4MH interpreter against the x86-64 JIT.
 *
 * The input is a configuration word and a program. The first four
 * bytes (little-endian) choose user or supervisor mode, whether the
 * FPU is usable, and FPSR's rounding, flush and exception-enable bits;
 * the rest is RH850 code, copied to the start of a read-only region
 * that is otherwise HALT, and both backends run it from there on cores
 * and buses built identically.
 *
 * The interpreter is the reference, and for this frontend the
 * interpreter and the manual are all there is: no RH850 is attached and
 * no other emulator is consulted. A disagreement says the two backends
 * read the manual differently; agreement says nothing about whether
 * either read it right.
 *
 * How every run ends at the same instruction on both sides:
 *
 *   - HALT parks the core, and the run reports WFI.
 *   - RBASE and EBASE name a 512-byte table in ROM that is HALTs from
 *     end to end, so every exception -- RIE, the traps, FPE, MAE, MDP
 *     -- lands on one and parks there.
 *   - lp starts at a HALT too, so `jmp [lp]` ends a program that reads
 *     like a function.
 *   - Code is ROM and data RAM is not executable, for the reasons the
 *     RV32 target gives.
 *   - TRAP reaches no syscall hook: it is the architectural trap.
 *   - INTC1 (at the SELF alias and at PE0's address), INTC2 and the
 *     IPIR are mapped, each side its own, so a program can configure,
 *     bind and raise channels and interrupt itself through the IPIR.
 *     PSW.ID starts set. A write after which the core would take an
 *     interrupt at once sets the run aside (fuzz_harness.h); one made
 *     takeable by EI, LDSR or EIRET is compared, since the translator
 *     declines all three. That matters more here than anywhere: the
 *     JIT takes an interrupt through its own copy of the run loop's
 *     code (g4mh_jit_take_irq), which has drifted from the interpreter's
 *     three times, so the two copies are what this compares.
 *   - No timer is mapped: the OSTM and TPTM count time, which does not
 *     pass here.
 *
 * A second PE, in a build with more than one (scripts/fuzz.sh SECS
 * g4mh-smp), when the configuration asks for it: both PEs run the same
 * program, PE1 once PE0 parks, each with its own bus, MPU, INTC1 and
 * local RAM over shared memory, and the stack pointer in its local RAM
 * through the SELF window -- where a U2B program keeps it, and the one
 * address that means different memory on each PE. The JIT's blocks are
 * the thread's, so a block PE0 built is what PE1 runs; anything it took
 * from the PE that built it is the thing to find. PE0 can wake a halted
 * PE1, or the other way round, through the IPIR.
 *
 * Built only with -DEMU_FUZZ=ON; see scripts/fuzz.sh, docs/fuzzing.md
 * and fuzz_harness.h.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "emu/emu_bus.h"
#include "g4mh/g4mh_backend.h"
#include "g4mh/g4mh_cpu.h"
#include "g4mh/g4mh_decode.h"
#include "g4mh/g4mh_disasm.h"
#include "g4mh/g4mh_intc.h"
#include "g4mh/g4mh_intercpu.h"
#include "g4mh/g4mh_memmap.h"
#include "g4mh/g4mh_mpu.h"

#include "fuzz_harness.h"

#if G4MH_PE_COUNT > 1
#define NCORES 2u
#else
#define NCORES 1u
#endif

#define CODE_BASE 0x20000000u
#define CODE_SIZE 0x1000u /* the program, then HALTs */
#define TRAP_BASE 0x20010000u
#define TRAP_SIZE 0x200u /* the whole table: every vector is a HALT */
#define RAM_BASE 0x80000000u
#define RAM_SIZE 0x4000u
#define REG_BASE (RAM_BASE + 0x2000u) /* r1-r31 start pointing here */
#define LRAM_SIZE 0x1000u /* of each PE's, at the SELF window and its own */

#define HALT_W0 0x07E0u
#define HALT_W1 0x0120u

/* The configuration word: the input's first four bytes. */
#define CFG_UM (1u << 0) /* user mode                         */
#define CFG_NOFPU (1u << 1) /* PSW.CU0 clear: FP raises UCPOP    */
#define CFG_RM_SHIFT 2 /* FPSR.RM, two bits                 */
#define CFG_FS (1u << 4) /* FPSR.FS: flush subnormals         */
#define CFG_FN (1u << 5) /* FPSR.FN: flush to nearest         */
#define CFG_XE_SHIFT 6 /* FPSR.XE: five exception enables   */
#define CFG_PE1 (1u << 11) /* a second PE, if the build has one */

typedef struct side {
    fz_side_t run; /* first: before_run is handed this */
    g4mh_cpu_t cpu[NCORES];
    g4mh_mpu_t mpu[NCORES]; /* its own: the frontend's is one per PE id */
    g4mh_intc_t intc[NCORES]; /* and its own controllers, likewise */
    g4mh_intc2_t intc2;
    g4mh_ipir_t ipir;
    g4mh_intercpu_port_t ipir_port[NCORES];
    /* One per mapping, so each knows whose bus it is on. */
    fz_dev_t intc1_self_dev[NCORES];
    fz_dev_t intc1_pe_dev[NCORES][NCORES];
    fz_dev_t intc2_dev[NCORES];
    fz_dev_t ipir_dev[NCORES];
    emu_bus_t bus[NCORES];
    uint8_t ram[RAM_SIZE];
    uint8_t lram[NCORES][LRAM_SIZE];
} side_t;

static uint8_t g_code[CODE_SIZE];
static size_t g_len;
static uint32_t g_cfg;
static uint8_t g_trap[TRAP_SIZE];
static uint8_t g_ram_init[RAM_SIZE];
static uint8_t g_lram_init[NCORES][LRAM_SIZE];
static unsigned g_ncores; /* this input's */

static side_t g_interp = {
    .run = {.name = "interp", .backend = &g4mh_backend_interp}};
static side_t g_jit = {.run = {.name = "jit", .backend = &g4mh_backend_jit}};

static void dump_program(void);

/*
 * The cross-core reservation tracker is the frontend's, by PE id, and a
 * store breaks the reservations of the cores registered there -- its
 * own core's included, which is what STC.W after a store to the linked
 * word depends on. So the side about to run is PE0 and PE1, and the
 * other is out of reach of its stores.
 */
static void before_run(fz_side_t *run)
{
    side_t *const sd = (side_t *)(void *)run;

    for (unsigned k = 0; k < NCORES; k++) {
        g4mh_ll_register(&sd->cpu[k]);
    }
}

static const fz_target_t g_target = {
    .name = "fuzz-g4mh-jit",
    .active = &g4mh_backend,
    .budget = 20000u,
    .slice = 1024u,
    .before_run = before_run,
    .after_run = NULL,
    .dump = dump_program,
};

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void fill_halt(uint8_t *p, size_t n)
{
    for (size_t i = 0; i + 4u <= n; i += 4u) {
        put16(&p[i], HALT_W0);
        put16(&p[i + 2u], HALT_W1);
    }
}

static void once(void)
{
    static bool done;

    if (done) {
        return;
    }
    done = true;
    fill_halt(g_trap, TRAP_SIZE);
    fz_fill(g_ram_init, RAM_SIZE, 0x9E3779B9u);
    for (unsigned k = 0; k < NCORES; k++) {
        fz_fill(g_lram_init[k], LRAM_SIZE, 0x85EBCA6Bu + k);
        g_interp.run.cores[k] = (emu_cpu_t *)(void *)&g_interp.cpu[k];
        g_jit.run.cores[k] = (emu_cpu_t *)(void *)&g_jit.cpu[k];
    }
    g_interp.run.cpu = g_interp.run.cores[0];
    g_jit.run.cpu = g_jit.run.cores[0];
    fz_start(&g_target, &g4mh_backend_jit, g_jit.run.cpu);
}

/*
 * After a write to a controller: whether the core would now take an
 * interrupt before its next instruction. See fuzz_harness.h.
 */
static unsigned intc_after_write(fz_dev_t *d, uint32_t off, uint32_t size)
{
    const g4mh_cpu_t *const c = (const g4mh_cpu_t *)(void *)fz_dev_core(d);

    (void)off;
    (void)size;
    return (g4mh_cpu_pending_fe(c) || g4mh_cpu_pending_irq(c) >= 0)
               ? FZ_ASIDE_ASYNC
               : 0u;
}

/* A device on PE k's bus, watched by the wrapper. */
static bool map_device(side_t *sd, unsigned k, fz_dev_t *d, const char *name,
                       uint32_t base, uint32_t size, const emu_dev_ops_t *ops,
                       void *ctx)
{
    *d = (fz_dev_t){
        .ops = ops,
        .ctx = ctx,
        .side = &sd->run,
        .cpu = (emu_cpu_t *)(void *)&sd->cpu[k],
        .after_write = intc_after_write,
    };
    return emu_bus_add_mmio(&sd->bus[k], name, base, size, &fz_dev_ops, d);
}

/*
 * PE k's bus: the program and the vectors, shared RAM, its own local RAM
 * at the SELF window and every PE's at its absolute one, and the
 * controllers the same way -- the frontend's map, at the size of this
 * one.
 */
static void build_bus(side_t *sd, unsigned k)
{
    static const char *const lnm[2] = {"lram-pe0", "lram-pe1"};
    static const char *const inm[2] = {"intc1-pe0", "intc1-pe1"};
    emu_bus_t *const bus = &sd->bus[k];
    const emu_region_t ram = {
        .base = RAM_BASE,
        .size = RAM_SIZE,
        .host = sd->ram,
        .name = "ram",
        .kind = EMU_MEM_RAM,
        .perm = EMU_PERM_RW, /* not X: see the header */
        .widths = EMU_WANY,
    };
    bool ok;

    emu_bus_init(bus);
    ok = emu_bus_add(bus, &ram) &&
         emu_bus_add_rom(bus, "code", CODE_BASE, g_code, CODE_SIZE) &&
         emu_bus_add_rom(bus, "trap", TRAP_BASE, g_trap, TRAP_SIZE) &&
         emu_bus_add_ram(bus, "lram-self", G4MH_LRAM_SELF_BASE, sd->lram[k],
                         LRAM_SIZE) &&
         map_device(sd, k, &sd->intc1_self_dev[k], "intc1-self",
                    G4MH_INTC1_SELF_BASE, G4MH_INTC1_SIZE, &g4mh_intc1_ops,
                    &sd->intc[k]) &&
         map_device(sd, k, &sd->intc2_dev[k], "intc2", G4MH_INTC2_BASE,
                    G4MH_INTC2_SIZE, &g4mh_intc2_ops, &sd->intc[0]) &&
         map_device(sd, k, &sd->ipir_dev[k], "ipir", G4MH_IPIR_BASE,
                    G4MH_IPIR_SIZE, &g4mh_ipir_ops, &sd->ipir_port[k]);
    for (unsigned pe = 0; ok && pe < NCORES; pe++) {
        ok = emu_bus_add_ram(bus, lnm[pe], G4MH_LRAM_PE_BASE(pe), sd->lram[pe],
                             LRAM_SIZE) &&
             map_device(sd, k, &sd->intc1_pe_dev[k][pe], inm[pe],
                        G4MH_INTC1_PE0_BASE + pe * G4MH_INTC1_PE_STRIDE,
                        G4MH_INTC1_SIZE, &g4mh_intc1_ops, &sd->intc[pe]);
    }
    if (!ok) {
        fprintf(stderr, "%s: could not build PE%u's bus\n", g_target.name, k);
        abort();
    }
}

/* The same machine on each side, from the same state. */
static void build(side_t *sd)
{
    memcpy(sd->ram, g_ram_init, RAM_SIZE);
    memcpy(sd->lram, g_lram_init, sizeof(sd->lram));
    g4mh_ipir_init(&sd->ipir);

    /*
     * Every PE the build has, whether this input runs one or two, so
     * that the reservation tracker -- which keeps the cores by id -- never
     * holds a core from an earlier input.
     */
    for (unsigned k = 0; k < NCORES; k++) {
        g4mh_cpu_t *const c = &sd->cpu[k];

        sd->ipir_port[k] = (g4mh_intercpu_port_t){.state = &sd->ipir, .pe = k};
        build_bus(sd, k);

        /*
         * Each side its own MPU, which the frontend allocates one per PE
         * id; two cores with id 0 would otherwise share one, and the
         * second run would start from what the first configured.
         */
        g4mh_ll_drop(c); /* the last input's: the tracker's count stays true */
        g4mh_cpu_init(c, &sd->bus[k], k);
        c->mpu = &sd->mpu[k];
        /* PE0's controller holds INTC2; the others point at it. */
        g4mh_intc_init(&sd->intc[k], c, (k == 0u) ? NULL : &sd->intc[0],
                       &sd->intc2);
        g4mh_ipir_bind(&sd->ipir, k, &sd->intc[k]);
        g4mh_cpu_reset(c, CODE_BASE);
        c->state = EMU_STATE_RUNNING;
        c->sr[1][G4MH_SR_RBASE] = TRAP_BASE;
        c->sr[1][G4MH_SR_EBASE] = TRAP_BASE;

        for (uint32_t i = 1; i < 32u; i++) {
            c->r[i] = REG_BASE + i * 32u;
        }
        c->r[31] = TRAP_BASE + 0x1F0u; /* lp: a HALT */
        if (g_ncores > 1u) {
            /* The stack in this PE's own local RAM; see the header. */
            c->r[3] = G4MH_LRAM_SELF_BASE + LRAM_SIZE - 0x40u;
        }

        c->psw = G4MH_PSW_ID | ((g_cfg & CFG_NOFPU) ? 0u : G4MH_PSW_CU0) |
                 ((g_cfg & CFG_UM) ? G4MH_PSW_UM : 0u);
        c->sr[0][G4MH_SR_PSW] = c->psw;
        c->sr[0][G4MH_SR_FPSR] =
            (((g_cfg >> CFG_RM_SHIFT) & 3u) << G4MH_FPSR_RM_SHIFT) |
            ((g_cfg & CFG_FS) ? G4MH_FPSR_FS : 0u) |
            ((g_cfg & CFG_FN) ? G4MH_FPSR_FN : 0u) |
            (((g_cfg >> CFG_XE_SHIFT) & 0x1Fu) << G4MH_FPSR_XE_SHIFT);
        g4mh_jit_ctx_refresh(c);
    }
    sd->run.ncores = g_ncores;
}

/* PE k's field, named pe<k>.<field> when there is a second PE. */
#define SAME(field)                                                            \
    fz_diff(&g_target, pe(k, #field), 0u, g_interp.cpu[k].field,               \
            g_jit.cpu[k].field)

static const char *pe(unsigned k, const char *field)
{
    static char buf[2][48];

    if (g_ncores == 1u) {
        return field;
    }
    snprintf(buf[k & 1u], sizeof(buf[0]), "pe%u.%s", k, field);
    return buf[k & 1u];
}

static void dump_program(void)
{
    char buf[96];

    fprintf(stderr, "  (ram[n] is the word at 0x%08x + 4n)\n", RAM_BASE);
    fprintf(stderr, "  config 0x%08x, %u PE%s; program, %zu bytes at 0x%08x:\n",
            g_cfg, g_ncores, (g_ncores == 1u) ? "" : "s", g_len, CODE_BASE);
    for (size_t off = 0; off < g_len;) {
        uint16_t hw[4] = {0u, 0u, 0u, 0u};
        unsigned n;
        uint64_t insn = 0u;

        for (unsigned i = 0; i < 4u && off + 2u * i + 1u < CODE_SIZE; i++) {
            hw[i] = (uint16_t)(g_code[off + 2u * i] |
                               (g_code[off + 2u * i + 1u] << 8));
        }
        n = g4mh_insn_len(hw[0]);
        if (n == 4u && g4mh_insn_is_48(hw[0], hw[1])) {
            n = g4mh_insn_is_64(hw[0], hw[1]) ? 8u : 6u;
        }
        for (unsigned i = 0; i < n / 2u; i++) {
            insn |= (uint64_t)hw[i] << (16u * i);
        }
        g4mh_disasm(buf, sizeof(buf), CODE_BASE + (uint32_t)off, insn, n);
        fprintf(stderr, "    %08x:", CODE_BASE + (unsigned)off);
        for (unsigned i = 0; i < 4u; i++) {
            if (i < n / 2u) {
                fprintf(stderr, " %04x", hw[i]);
            } else {
                fprintf(stderr, "     ");
            }
        }
        fprintf(stderr, "  %s\n", buf);
        off += n;
    }
}

static void compare_pe(unsigned k)
{
    for (unsigned i = 1; i < 32u; i++) {
        fz_diff(&g_target, pe(k, "r"), i, g_interp.cpu[k].r[i],
                g_jit.cpu[k].r[i]);
    }
    SAME(pc);
    SAME(psw);
    SAME(state);
    for (unsigned b = 0; b < G4MH_SR_BANKS; b++) {
        for (unsigned i = 0; i < G4MH_SR_PER_BANK; i++) {
            fz_diff(&g_target, pe(k, "sr"), b * 100u + i,
                    g_interp.cpu[k].sr[b][i], g_jit.cpu[k].sr[b][i]);
        }
    }
    SAME(retired); /* the PMU counts from it, into PMCOUNTn, above */
    SAME(ll_valid);
    if (g_interp.cpu[k].ll_valid) {
        SAME(ll_addr);
    }
    SAME(mpu_active);
    fz_diff_mem(
        &g_target, pe(k, "intc.chan"), (const uint8_t *)g_interp.intc[k].chan,
        (const uint8_t *)g_jit.intc[k].chan, sizeof(g_jit.intc[k].chan));
    fz_diff_mem(
        &g_target, pe(k, "intc.eibd"), (const uint8_t *)g_interp.intc[k].eibd,
        (const uint8_t *)g_jit.intc[k].eibd, sizeof(g_jit.intc[k].eibd));
    SAME(intc->pend);
    SAME(intc->fibd);
    SAME(intc->eibg);
    SAME(intc->fibg);
    SAME(intc->ihvcfg);
    SAME(intc->feint);
    fz_diff_mem(&g_target, pe(k, "lram"), g_interp.lram[k], g_jit.lram[k],
                LRAM_SIZE);
}

static void compare(void)
{
    fz_diff(&g_target, "reason", 0u, g_interp.run.reason, g_jit.run.reason);
    for (unsigned k = 0; k < g_ncores; k++) {
        compare_pe(k);
    }
    fz_diff_mem(&g_target, "intc2.chan", (const uint8_t *)g_interp.intc2.chan,
                (const uint8_t *)g_jit.intc2.chan, sizeof(g_jit.intc2.chan));
    fz_diff_mem(&g_target, "intc2.eibd", (const uint8_t *)g_interp.intc2.eibd,
                (const uint8_t *)g_jit.intc2.eibd, sizeof(g_jit.intc2.eibd));
    fz_diff_mem(&g_target, "ipir", (const uint8_t *)&g_interp.ipir,
                (const uint8_t *)&g_jit.ipir,
                offsetof(g4mh_ipir_t, intc) & ~(size_t)3);
    fz_diff_mem(&g_target, "ram", g_interp.ram, g_jit.ram, RAM_SIZE);
    if (fz_diffs != 0u) {
        fprintf(stderr, "  (sr[100b + n] is selID b, regID n)\n");
    }
    fz_verdict(&g_target, &g_interp.run, &g_jit.run);
    /* EIIC names an EIINT channel from 0x1000; FEIC 0xF0 is FEINT. */
    for (unsigned k = 0; k < g_ncores; k++) {
        if (g_interp.cpu[k].sr[0][G4MH_SR_EIIC] >= 0x1000u ||
            g_interp.cpu[k].sr[0][G4MH_SR_FEIC] == 0xF0u) {
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
    g_ncores = (NCORES > 1u && (g_cfg & CFG_PE1) != 0u) ? NCORES : 1u;
    g_len = (size < CODE_SIZE - 8u) ? size & ~(size_t)1 : CODE_SIZE - 8u;
    /* The HALTs begin where the program ends, whatever its length. */
    memset(g_code, 0, CODE_SIZE);
    memcpy(g_code, data, g_len);
    fill_halt(g_code + g_len, CODE_SIZE - g_len);

    build(&g_interp);
    build(&g_jit);
    if (fz_pair(&g_target, &g_interp.run, &g_jit.run)) {
        compare();
    }
    return 0;
}
