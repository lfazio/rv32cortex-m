/* SPDX-License-Identifier: Apache-2.0 */
/*
 * fuzz_rv32_jit.c - the RV32 interpreter against the x86-64 JIT, on
 * whatever bytes libFuzzer hands over.
 *
 * The input is the program: its bytes are copied to the start of a
 * read-only code region and both backends run it from there, each on
 * its own hart and bus, from identical state. When both stop the same
 * way, every register, every CSR this compares, the retired count and
 * the whole of data RAM must agree; when they do not, the harness
 * prints both sides and aborts, and libFuzzer keeps the input.
 *
 * The interpreter is the reference. It is what both architecture
 * suites and every board test run against, and the JIT is meant to be
 * the same machine faster -- so a difference is a JIT defect until the
 * interpreter is shown wrong. Nothing here consults a GPL emulator.
 *
 * Every run has to end at the same instruction on both sides, and that
 * is what the layout is for:
 *
 *   - Code is ROM. A store into it faults on both backends alike;
 *     self-modifying code without FENCE.I is allowed to differ between
 *     an interpreter and a translator, and is the one thing that would.
 *   - Data RAM is readable and writable and not executable, so a jump
 *     into data faults rather than running bytes the program wrote.
 *   - mtvec points at an ECALL in ROM, and the ECALL hook halts the
 *     hart. Any trap, an ECALL in the program, and falling off its end
 *     (the rest of the code region is ECALLs) all stop the run there.
 *   - mtime is a constant, so `rdtime` reads the same on both.
 *
 * A run that has not finished at the budget is not compared; the rule,
 * the run loop and the report are in fuzz_harness.h, which every
 * frontend's target shares.
 *
 * Built only with -DEMU_FUZZ=ON, by clang, on the host; see
 * scripts/fuzz.sh and docs/fuzzing.md.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "emu/emu_bus.h"
#include "rv32/rv_backend.h"
#include "rv32/rv_decode.h"
#include "rv32/rv_disasm.h"
#include "rv32/rv_hart.h"
#include "rv32/rv_jit.h"

#include "fuzz_harness.h"

#define CODE_BASE 0x20000000u
#define CODE_SIZE 0x1000u /* the program, then ECALLs */
#define TRAP_BASE 0x20010000u
#define RAM_BASE 0x80000000u
#define RAM_SIZE 0x4000u
#define REG_BASE (RAM_BASE + 0x2000u) /* registers start pointing here */

#define ECALL 0x00000073u
#define FS_INITIAL 0x00002000u

typedef struct side {
    fz_side_t run;
    rv_hart_t hart;
    emu_bus_t bus;
    uint8_t ram[RAM_SIZE];
} side_t;

static uint8_t g_code[CODE_SIZE];
static size_t g_len;
static uint32_t g_trap[4];
static uint8_t g_ram_init[RAM_SIZE];
static const uint64_t g_mtime = 0x0000000123456789ull;

static side_t g_interp = {
    .run = {.name = "interp", .backend = &rv_backend_interp}};
static side_t g_jit = {.run = {.name = "jit", .backend = &rv_backend_jit}};

static void dump_program(void);

static const fz_target_t g_target = {
    .name = "fuzz-rv32-jit",
    .active = &rv_backend,
    .budget = 20000u,
    .slice = 1024u,
    .after_run = NULL,
    .dump = dump_program,
};

static bool halt_hook(emu_cpu_t *cpu, emu_syscall_t *sc, void *user)
{
    (void)sc;
    (void)user;
    ((rv_hart_t *)cpu)->state = EMU_STATE_HALTED;
    return true;
}

static void once(void)
{
    static bool done;

    if (done) {
        return;
    }
    done = true;
    for (uint32_t i = 0; i < 4u; i++) {
        g_trap[i] = ECALL;
    }
    fz_fill(g_ram_init, RAM_SIZE, 0x9E3779B9u);
    g_interp.run.cpu = (emu_cpu_t *)&g_interp.hart;
    g_jit.run.cpu = (emu_cpu_t *)&g_jit.hart;
    /* The JIT's code buffer is per process; map it once. */
    fz_start(&g_target, &rv_backend_jit, g_jit.run.cpu);
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
    rv_hart_t *h = &sd->hart;

    emu_bus_init(&sd->bus);
    memcpy(sd->ram, g_ram_init, RAM_SIZE);
    if (!emu_bus_add(&sd->bus, &ram) ||
        !emu_bus_add_rom(&sd->bus, "code", CODE_BASE, g_code, CODE_SIZE) ||
        !emu_bus_add_rom(&sd->bus, "trap", TRAP_BASE, g_trap, sizeof(g_trap))) {
        fprintf(stderr, "fuzz-rv32-jit: could not build the bus\n");
        abort();
    }

    rv_hart_init(h, &sd->bus, 0u);
    rv_hart_reset(h, CODE_BASE);
    for (uint32_t i = 1; i < 32u; i++) {
        h->x[i] = REG_BASE + i * 32u;
    }
    for (uint32_t i = 0; i < 32u; i++) {
        /* Odd registers hold boxed singles, even ones doubles. */
        const float fs = (float)((int)i - 8) * 1.5f;
        const double fd = (double)((int)i - 16) * 0.3125;
        uint32_t s32;
        uint64_t d64;

        memcpy(&s32, &fs, 4u);
        memcpy(&d64, &fd, 8u);
        h->f[i] = (i & 1u) ? (0xFFFFFFFF00000000ull | s32) : d64;
    }
    h->mstatus |= FS_INITIAL;
    h->mtvec = TRAP_BASE;
    h->mtime = &g_mtime;
    h->ecall = halt_hook;
    h->ecall_user = NULL;
}

#define SAME(field)                                                            \
    fz_diff(&g_target, #field, 0u, g_interp.hart.field, g_jit.hart.field)
#define SAME_AT(field, i)                                                      \
    fz_diff(&g_target, #field, (unsigned)(i), g_interp.hart.field[i],          \
            g_jit.hart.field[i])

static void dump_program(void)
{
    const size_t len = g_len;
    char buf[96];

    fprintf(stderr, "  (ram[n] is the word at 0x%08x + 4n)\n", RAM_BASE);
    fprintf(stderr, "  program, %zu bytes at 0x%08x:\n", len, CODE_BASE);
    for (size_t off = 0; off < len;) {
        uint32_t w = 0;
        unsigned n;

        memcpy(&w, &g_code[off], (len - off >= 4u) ? 4u : len - off);
        n = ((w & 3u) == 3u) ? 4u : 2u;
        if (n == 2u) {
            w &= 0xFFFFu;
        }
        /* The disassembler takes RVC already expanded, as the decoder
         * hands it over; 0 is the expansion of a reserved encoding. */
        rv_disasm(buf, sizeof(buf), CODE_BASE + (uint32_t)off,
                  (n == 2u) ? rv_decode_expand_c((uint16_t)w) : w, n);
        fprintf(stderr, "    %08x: %*s%0*x  %s\n", CODE_BASE + (unsigned)off,
                (n == 2u) ? 4 : 0, "", (int)(n * 2u), w, buf);
        off += n;
    }
}

static void compare(void)
{
    const rv_hart_t *a = &g_interp.hart;

    fz_diff(&g_target, "reason", 0u, g_interp.run.reason, g_jit.run.reason);
    /*
     * Not the run's own count. That is budget bookkeeping, and the two
     * backends keep it differently -- the interpreter charges a trapped
     * instruction to the budget and the JIT framework does not when a
     * block traps on its first instruction, which is why isatest
     * reports 95,812 interpreted and 95,924 translated. minstret is the
     * architectural count, and it is compared below.
     */
    SAME(pc);
    for (unsigned i = 1; i < 32u; i++) {
        SAME_AT(x, i);
    }
    for (unsigned i = 0; i < 32u; i++) {
        SAME_AT(f, i);
    }
    SAME(fcsr);
    SAME(priv);
    SAME(state);
    SAME(trap_count);
    /*
     * mstatus.FS may be dirtier under the JIT, never cleaner. "Imple-
     * mentations may choose to track the dirtiness of the floating-point
     * register state imprecisely by reporting the state to be dirty even
     * when it has not been modified" (the privileged spec; SmF's
     * mstatus_fs_imprecise, which the arch tests call untestable) -- and
     * the JIT marks a block's FP writes dirty before a load in it can
     * fault. Cleaner would be a bug: an OS that reads Clean does not save
     * the registers. SD summarises FS and goes with it.
     */
    {
        const uint32_t fs = 0x6000u, sd = 0x80000000u;
        const uint32_t fa = (a->mstatus & fs) >> 13;
        const uint32_t fb = (g_jit.hart.mstatus & fs) >> 13;

        fz_diff(&g_target, "mstatus", 0u, a->mstatus & ~(fs | sd),
                g_jit.hart.mstatus & ~(fs | sd));
        if (fa != fb && !(fa != 0u && fb == 3u)) {
            fz_diff(&g_target, "mstatus.FS", 0u, fa, fb);
        }
    }
    SAME(mie);
    SAME(mip);
    SAME(mtvec);
    SAME(mscratch);
    SAME(mepc);
    SAME(mcause);
    SAME(mtval);
    SAME(mcountinhibit);
    SAME(minstret);
    SAME(mcycle);
    SAME(mcounteren);
    SAME(menvcfg);
    SAME(senvcfg);
    SAME(stvec);
    SAME(sscratch);
    SAME(sepc);
    SAME(scause);
    SAME(stval);
    SAME(scounteren);
    SAME(medeleg);
    SAME(mideleg);
    SAME(satp);
    SAME(tselect);
    for (unsigned i = 0; i < RV_TRIG_COUNT; i++) {
        SAME_AT(tdata1, i);
        SAME_AT(tdata2, i);
    }
    for (unsigned i = 0; i < RV_PMP_ENTRIES / 4u; i++) {
        SAME_AT(pmpcfg, i);
    }
    for (unsigned i = 0; i < RV_PMP_ENTRIES; i++) {
        SAME_AT(pmpaddr, i);
    }
    SAME(resv_valid);
    if (a->resv_valid) {
        SAME(resv_addr);
    }
    for (unsigned i = 0; i < RV_HPM_COUNT; i++) {
        SAME_AT(mhpmcounter, i);
        SAME_AT(mhpmevent, i);
    }
    fz_diff_mem(&g_target, "ram", g_interp.ram, g_jit.ram, RAM_SIZE);
    fz_verdict(&g_target, &g_interp.run, &g_jit.run);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    g_len = (size < CODE_SIZE - 4u) ? size : CODE_SIZE - 4u;

    once();
    for (uint32_t i = 0; i < CODE_SIZE; i += 4u) {
        memcpy(&g_code[i], &(uint32_t){ECALL}, 4u);
    }
    memcpy(g_code, data, g_len);

    build(&g_interp);
    build(&g_jit);
    if (fz_pair(&g_target, &g_interp.run, &g_jit.run)) {
        compare();
    }
    return 0;
}
