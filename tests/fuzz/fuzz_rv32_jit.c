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
 * A JIT can only stop between blocks, so a run that is still going at
 * the budget stops at a different instruction on each side. Those are
 * not compared -- only a run both sides finished is -- and a program
 * that loops can still finish, by counting.
 *
 * Built only with -DEMU_FUZZ=ON, by clang, on the host; see
 * scripts/fuzz.sh and docs/fuzzing.md.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "emu/emu_bus.h"
#include "emu/emu_jit.h"
#include "rv32/rv_backend.h"
#include "rv32/rv_decode.h"
#include "rv32/rv_disasm.h"
#include "rv32/rv_hart.h"
#include "rv32/rv_jit.h"

#define CODE_BASE 0x20000000u
#define CODE_SIZE 0x1000u /* the program, then ECALLs */
#define TRAP_BASE 0x20010000u
#define RAM_BASE 0x80000000u
#define RAM_SIZE 0x4000u
#define REG_BASE (RAM_BASE + 0x2000u) /* registers start pointing here */

#define BUDGET 20000u
#define SLICE 1024u

#define ECALL 0x00000073u
#define FS_INITIAL 0x00002000u

typedef struct side {
    const char *name;
    const emu_backend_t *backend;
    rv_hart_t hart;
    emu_bus_t bus;
    uint8_t ram[RAM_SIZE];
    emu_run_reason_t reason;
    uint64_t retired;
} side_t;

static uint8_t g_code[CODE_SIZE];
static uint32_t g_trap[4];
static uint8_t g_ram_init[RAM_SIZE];
static const uint64_t g_mtime = 0x0000000123456789ull;

static side_t g_interp = {.name = "interp", .backend = &rv_backend_interp};
static side_t g_jit = {.name = "jit", .backend = &rv_backend_jit};

/* What the runs did, printed at exit when FUZZ_STATS is set. */
static struct {
    uint64_t inputs;
    uint64_t compared;
    uint64_t unfinished;
    uint64_t retired;
    uint64_t translations;
    uint64_t block_entries;
    uint64_t interp_fallbacks;
} g_stats;

static bool halt_hook(emu_cpu_t *cpu, emu_syscall_t *sc, void *user)
{
    (void)sc;
    (void)user;
    ((rv_hart_t *)cpu)->state = EMU_STATE_HALTED;
    return true;
}

/* xorshift32, so data RAM's initial bytes are arbitrary but fixed. */
static uint32_t prng(uint32_t *s)
{
    *s ^= *s << 13;
    *s ^= *s >> 17;
    *s ^= *s << 5;
    return *s;
}

static void print_stats(void)
{
    fprintf(stderr,
            "fuzz-rv32-jit: %llu inputs, %llu compared, %llu unfinished; "
            "%llu retired, %llu translations, %llu block entries, "
            "%llu interpreted under the JIT\n",
            (unsigned long long)g_stats.inputs,
            (unsigned long long)g_stats.compared,
            (unsigned long long)g_stats.unfinished,
            (unsigned long long)g_stats.retired,
            (unsigned long long)g_stats.translations,
            (unsigned long long)g_stats.block_entries,
            (unsigned long long)g_stats.interp_fallbacks);
}

static void once(void)
{
    static bool done;
    uint32_t s = 0x9E3779B9u;

    if (done) {
        return;
    }
    done = true;
    for (uint32_t i = 0; i < 4u; i++) {
        g_trap[i] = ECALL;
    }
    for (uint32_t i = 0; i < RAM_SIZE; i += 4u) {
        const uint32_t v = prng(&s);
        memcpy(&g_ram_init[i], &v, 4u);
    }
    /* The JIT's code buffer is per process; map it once. */
    if (rv_backend_jit.init != NULL &&
        !rv_backend_jit.init((emu_cpu_t *)&g_jit.hart)) {
        fprintf(stderr, "fuzz-rv32-jit: the JIT did not start\n");
        abort();
    }
    if (getenv("FUZZ_STATS") != NULL) {
        atexit(print_stats);
    }
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

static void run(side_t *sd)
{
    emu_run_reason_t r = EMU_RUN_BUDGET;
    uint64_t total = 0;

    rv_backend = sd->backend; /* FENCE.I reaches it through this */
    if (sd->backend->reset != NULL) {
        sd->backend->reset((emu_cpu_t *)&sd->hart);
    }
    while (total < BUDGET) {
        uint32_t n = 0;

        r = sd->backend->run((emu_cpu_t *)&sd->hart, SLICE, &n);
        total += n;
        if (r == EMU_RUN_HALTED || r == EMU_RUN_WFI) {
            break;
        }
    }
    sd->reason = r;
    sd->retired = total;
}

static bool finished(const side_t *sd)
{
    return sd->reason == EMU_RUN_HALTED || sd->reason == EMU_RUN_WFI;
}

static unsigned g_diffs;

static void diff64(const char *what, unsigned idx, uint64_t a, uint64_t b)
{
    if (a == b) {
        return;
    }
    if (g_diffs++ == 0u) {
        fprintf(stderr, "fuzz-rv32-jit: the interpreter and the JIT "
                        "disagree\n");
    }
    fprintf(stderr, "  %-10s[%2u] interp 0x%016llx jit 0x%016llx\n", what, idx,
            (unsigned long long)a, (unsigned long long)b);
}

#define SAME(field) diff64(#field, 0u, g_interp.hart.field, g_jit.hart.field)
#define SAME_AT(field, i)                                                      \
    diff64(#field, (unsigned)(i), g_interp.hart.field[i], g_jit.hart.field[i])

static void dump_program(size_t len)
{
    char buf[96];

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

static void compare(size_t len)
{
    const rv_hart_t *a = &g_interp.hart;

    g_diffs = 0;
    diff64("reason", 0u, g_interp.reason, g_jit.reason);
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

        diff64("mstatus", 0u, a->mstatus & ~(fs | sd),
               g_jit.hart.mstatus & ~(fs | sd));
        if (fa != fb && !(fa != 0u && fb == 3u)) {
            diff64("mstatus.FS", 0u, fa, fb);
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
    for (uint32_t i = 0; i < RAM_SIZE; i += 4u) {
        uint32_t va, vb;

        memcpy(&va, &g_interp.ram[i], 4u);
        memcpy(&vb, &g_jit.ram[i], 4u);
        diff64("ram", (unsigned)(i / 4u), va, vb);
    }

    if (g_diffs != 0u) {
        fprintf(stderr, "  (ram[n] is the word at 0x%08x + 4n)\n", RAM_BASE);
        fprintf(stderr, "  run counts: interp %llu, jit %llu; reason %d\n",
                (unsigned long long)g_interp.retired,
                (unsigned long long)g_jit.retired, (int)g_interp.reason);
        dump_program(len);
        abort();
    }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    const size_t len = (size < CODE_SIZE - 4u) ? size : CODE_SIZE - 4u;
    emu_jit_stats_t before, after;

    once();
    for (uint32_t i = 0; i < CODE_SIZE; i += 4u) {
        memcpy(&g_code[i], &(uint32_t){ECALL}, 4u);
    }
    memcpy(g_code, data, len);

    build(&g_interp);
    build(&g_jit);
    run(&g_interp);
    emu_jit_get_stats(&before);
    run(&g_jit);
    emu_jit_get_stats(&after);

    g_stats.inputs++;
    g_stats.translations += after.translations - before.translations;
    g_stats.block_entries += after.block_entries - before.block_entries;
    g_stats.interp_fallbacks +=
        after.interp_fallbacks - before.interp_fallbacks;

    if (finished(&g_interp) && finished(&g_jit)) {
        g_stats.compared++;
        g_stats.retired += g_interp.retired;
        compare(len);
    } else if (finished(&g_interp) != finished(&g_jit)) {
        /*
         * One stopped and the other reached the budget. That is a
         * divergence only if the one still running went *past* where
         * the other stopped: a JIT returns at a block boundary, so it
         * can reach the budget a little short of an interpreter's halt
         * as easily as a little beyond it.
         */
        const side_t *done = finished(&g_interp) ? &g_interp : &g_jit;
        const side_t *open = (done == &g_interp) ? &g_jit : &g_interp;

        if (open->retired > done->retired) {
            g_diffs = 0;
            diff64("reason", 0u, g_interp.reason, g_jit.reason);
            diff64("retired", 0u, g_interp.retired, g_jit.retired);
            dump_program(len);
            abort();
        }
        g_stats.unfinished++;
    } else {
        g_stats.unfinished++;
    }
    return 0;
}
