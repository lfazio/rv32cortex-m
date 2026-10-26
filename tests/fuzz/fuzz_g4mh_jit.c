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
 *   - HALT parks the core, and the run reports WFI. Nothing can wake
 *     it: there is no interrupt controller.
 *   - RBASE and EBASE name a 512-byte table in ROM that is HALTs from
 *     end to end, so every exception -- RIE, the traps, FPE, MAE, MDP
 *     -- lands on one and parks there.
 *   - lp starts at a HALT too, so `jmp [lp]` ends a program that reads
 *     like a function.
 *   - Code is ROM and data RAM is not executable, for the reasons the
 *     RV32 target gives.
 *   - TRAP reaches no syscall hook: it is the architectural trap.
 *
 * Built only with -DEMU_FUZZ=ON; see scripts/fuzz.sh, docs/fuzzing.md
 * and fuzz_harness.h.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "emu/emu_bus.h"
#include "g4mh/g4mh_backend.h"
#include "g4mh/g4mh_cpu.h"
#include "g4mh/g4mh_decode.h"
#include "g4mh/g4mh_disasm.h"
#include "g4mh/g4mh_mpu.h"

#include "fuzz_harness.h"

#define CODE_BASE 0x20000000u
#define CODE_SIZE 0x1000u /* the program, then HALTs */
#define TRAP_BASE 0x20010000u
#define TRAP_SIZE 0x200u /* the whole table: every vector is a HALT */
#define RAM_BASE 0x80000000u
#define RAM_SIZE 0x4000u
#define REG_BASE (RAM_BASE + 0x2000u) /* r1-r31 start pointing here */

#define HALT_W0 0x07E0u
#define HALT_W1 0x0120u

/* The configuration word: the input's first four bytes. */
#define CFG_UM (1u << 0) /* user mode                         */
#define CFG_NOFPU (1u << 1) /* PSW.CU0 clear: FP raises UCPOP    */
#define CFG_RM_SHIFT 2 /* FPSR.RM, two bits                 */
#define CFG_FS (1u << 4) /* FPSR.FS: flush subnormals         */
#define CFG_FN (1u << 5) /* FPSR.FN: flush to nearest         */
#define CFG_XE_SHIFT 6 /* FPSR.XE: five exception enables   */

typedef struct side {
    fz_side_t run;
    g4mh_cpu_t cpu;
    g4mh_mpu_t mpu; /* its own: the frontend's is one per PE id */
    emu_bus_t bus;
    uint8_t ram[RAM_SIZE];
} side_t;

static uint8_t g_code[CODE_SIZE];
static size_t g_len;
static uint32_t g_cfg;
static uint8_t g_trap[TRAP_SIZE];
static uint8_t g_ram_init[RAM_SIZE];

static side_t g_interp = {
    .run = {.name = "interp", .backend = &g4mh_backend_interp}};
static side_t g_jit = {.run = {.name = "jit", .backend = &g4mh_backend_jit}};

static void dump_program(void);

/*
 * The cross-core reservation tracker is the frontend's, by PE id, and a
 * store breaks the reservations of the cores registered there -- its
 * own core's included, which is what STC.W after a store to the linked
 * word depends on. So the side about to run is PE0, and the other is
 * out of reach of its stores.
 */
static void before_run(emu_cpu_t *cpu)
{
    g4mh_ll_register((g4mh_cpu_t *)(void *)cpu);
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
    g_interp.run.cpu = (emu_cpu_t *)(void *)&g_interp.cpu;
    g_jit.run.cpu = (emu_cpu_t *)(void *)&g_jit.cpu;
    fz_start(&g_target, &g4mh_backend_jit, g_jit.run.cpu);
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
    g4mh_cpu_t *c = &sd->cpu;

    emu_bus_init(&sd->bus);
    memcpy(sd->ram, g_ram_init, RAM_SIZE);
    if (!emu_bus_add(&sd->bus, &ram) ||
        !emu_bus_add_rom(&sd->bus, "code", CODE_BASE, g_code, CODE_SIZE) ||
        !emu_bus_add_rom(&sd->bus, "trap", TRAP_BASE, g_trap, TRAP_SIZE)) {
        fprintf(stderr, "%s: could not build the bus\n", g_target.name);
        abort();
    }

    /*
     * Each side its own MPU, which the frontend allocates one per PE id;
     * two cores with id 0 would otherwise share one, and the second run
     * would start from what the first configured.
     */
    g4mh_ll_drop(c); /* the last input's, so the tracker's count is true */
    g4mh_cpu_init(c, &sd->bus, 0u);
    c->mpu = &sd->mpu;
    g4mh_cpu_reset(c, CODE_BASE);
    c->state = EMU_STATE_RUNNING;
    c->sr[1][G4MH_SR_RBASE] = TRAP_BASE;
    c->sr[1][G4MH_SR_EBASE] = TRAP_BASE;

    for (uint32_t i = 1; i < 32u; i++) {
        c->r[i] = REG_BASE + i * 32u;
    }
    c->r[31] = TRAP_BASE + 0x1F0u; /* lp: a HALT */

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

#define SAME(field)                                                            \
    fz_diff(&g_target, #field, 0u, g_interp.cpu.field, g_jit.cpu.field)

static void dump_program(void)
{
    char buf[96];

    fprintf(stderr, "  (ram[n] is the word at 0x%08x + 4n)\n", RAM_BASE);
    fprintf(stderr, "  config 0x%08x; program, %zu bytes at 0x%08x:\n", g_cfg,
            g_len, CODE_BASE);
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

static void compare(void)
{
    fz_diff(&g_target, "reason", 0u, g_interp.run.reason, g_jit.run.reason);
    for (unsigned i = 1; i < 32u; i++) {
        fz_diff(&g_target, "r", i, g_interp.cpu.r[i], g_jit.cpu.r[i]);
    }
    SAME(pc);
    SAME(psw);
    SAME(state);
    for (unsigned b = 0; b < G4MH_SR_BANKS; b++) {
        for (unsigned i = 0; i < G4MH_SR_PER_BANK; i++) {
            fz_diff(&g_target, "sr", b * 100u + i, g_interp.cpu.sr[b][i],
                    g_jit.cpu.sr[b][i]);
        }
    }
    SAME(retired);
    /*
     * Not `cycles`: nothing in the tree reads it. The interpreter counts
     * an exception entry there and the JIT does not, and no guest can
     * tell -- the PMU's cycle event counts in PMCOUNTn, which is a
     * system register and compared above.
     */
    SAME(ll_valid);
    if (g_interp.cpu.ll_valid) {
        SAME(ll_addr);
    }
    SAME(mpu_active);
    fz_diff_mem(&g_target, "ram", g_interp.ram, g_jit.ram, RAM_SIZE);
    if (fz_diffs != 0u) {
        fprintf(stderr, "  (sr[100b + n] is selID b, regID n)\n");
    }
    fz_verdict(&g_target, &g_interp.run, &g_jit.run);
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
