/* SPDX-License-Identifier: Apache-2.0 */
/*
 * main.c - Host runner.
 *
 * Runs the same core the firmware runs, against the same guest memory map,
 * on a development machine. This is where the instruction-level tests run:
 * iterating here is far faster than reflashing, and any divergence between
 * host and target is a bug in the platform layer, not the frontend.
 *
 * Nothing below names a guest architecture. It builds a bus, opens a core
 * through emu_cpu_ops_t, and runs it; which ISA that is comes from
 * --frontend, or from the ELF header of the image, or from whichever
 * frontend was compiled in first.
 *
 * The primary input is a flat binary, which is what the target consumes.
 * ELF is accepted as a convenience because the RISC-V test suites ship
 * that way; the loader for it is host-only and is never built into the
 * firmware, where flash is scarce.
 */

#include "emu/emu_cpu.h"
#include "emu/emu_gdb.h"
#include "emu/emu_dev.h"
#include "emu/emu_elf.h"
#include "emu/emu_memmap.h"
#include "emu/emu_jit.h"

#if EMU_FRONTEND_RV32
/* --jit selects the backend, and the summary reports what it did. */
#  include "rv32/rv_backend.h"
#  include "rv32/rv_jit.h"
#endif

#if RV_PAIR_STATS
#  include "rv32/rv_pairstats.h"
#endif

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Guest memory                                                        */
/* ------------------------------------------------------------------ */

#define DEFAULT_RAM_SIZE  (1u << 20)   /* 1 MiB */

/*
 * The host has no ARM peripherals to pass through to, so the peripheral
 * window is backed by plain memory. Guest drivers still run; they just talk
 * to nothing. This keeps guest images identical across host and target.
 *
 * Sized to reach 0x40024000, the end of the STM32F446's APB1/APB2/AHB1
 * block, because that is where RCC lives at 0x40023800. A driver's first
 * act is to ungate its own peripheral's clock, so a window that stops short
 * of RCC faults on the first store every real guest driver makes -- which
 * is exactly what a 64 KiB window did.
 */
#define PERIPH_SIM_SIZE   0x24000u

static uint8_t *g_ram;
static uint8_t *g_periph;

/*
 * The machine: a frontend, its cores, and a bus each.
 *
 * One bus per core rather than one shared: a core-relative window like the
 * RH850 INTC1 SELF alias means the same guest address resolves to
 * different memory depending on who is executing, and giving each core its
 * own region table expresses that with no cost on the access path. It also
 * means each core gets its own fast-path caches, which one shared bus
 * would have them thrashing on every switch.
 *
 * `g_core` is core 0, kept as a name because the syscall and trace hooks
 * only ever need the bus, and every core's bus maps the shared regions
 * identically.
 */
static emu_system_t g_sys;
static emu_bus_t    g_bus[EMU_MAX_CORES];
#define g_core (g_sys.core[0])

/* ------------------------------------------------------------------ */
/* Console transport                                                   */
/* ------------------------------------------------------------------ */

static void host_tx(void *ctx, uint8_t c)
{
    (void)ctx;
    fputc(c, stdout);
    /* Unbuffered so output survives a guest that faults straight after. */
    fflush(stdout);
}

static int host_rx(void *ctx)
{
    (void)ctx;
    return -1;   /* no interactive input in the batch runner */
}

/* ------------------------------------------------------------------ */
/* System-call services                                                */
/* ------------------------------------------------------------------ */

/*
 * A tiny subset of the newlib syscall ABI, which is what a bare-metal
 * cross-gcc's crt0 and the standard test harnesses emit:
 *
 *   nr = 64  write(fd, buf, len)
 *   nr = 93  exit(code)
 *
 * Anything else falls through to the architectural trap, so guest software
 * with its own handler keeps working.
 *
 * The frontend unpacks its own calling convention into emu_syscall_t --
 * a7 and a0-a3 on RISC-V -- so this handler is written once and serves any
 * of them.
 */
static int g_exit_code = -1;

static bool host_syscall(emu_cpu_t *cpu, emu_syscall_t *sc, void *user)
{
    (void)user;

    switch (sc->nr) {
    case 64: {   /* write(fd, buf, len) */
        const uint32_t buf = sc->arg[1];
        const uint32_t len = sc->arg[2];
        for (uint32_t i = 0; i < len; i++) {
            uint32_t byte;
            if (emu_bus_read(g_core.bus, buf + i, 1u, &byte) != EMU_FAULT_NONE) {
                break;
            }
            fputc((int)byte, stdout);
        }
        fflush(stdout);
        sc->ret = len;
        return true;
    }

    case 93:     /* exit(code) */
        g_exit_code = (int)sc->arg[0];
        g_core.ops->halt(cpu);
        return true;

    default:
        return false;
    }
}

#if EMU_ENABLE_TRACE
/*
 * Instruction trace. Prints pc, the encoding, the disassembly and the
 * first few registers, which is what is needed to find where execution
 * diverges from a reference model. Registers by index rather than by name
 * because the frontend decides what they are called, and a divergence hunt
 * wants the same columns on every line.
 */
static uint64_t g_trace_skip;
static uint64_t g_trace_count = 64;

static void host_trace(emu_cpu_t *cpu, uint32_t pc, uint32_t insn, void *user)
{
    (void)user;
    emu_cpu_status_t st;
    g_core.ops->status(cpu, &st);

    if (st.retired < g_trace_skip ||
        st.retired >= g_trace_skip + g_trace_count) {
        return;
    }

    char buf[64];
    buf[0] = '\0';
    if (g_core.ops->disasm != NULL) {
        g_core.ops->disasm(buf, sizeof(buf), pc, insn);
    }
    fprintf(stderr, "%8llu %08x  %08x  %-28s",
            (unsigned long long)st.retired, pc, insn, buf);

    for (unsigned r = 1; r < 8u && r < g_core.ops->nregs; r++) {
        fprintf(stderr, " %s=%08x", g_core.ops->reg_name(r),
                g_core.ops->reg_read(cpu, r));
    }
    fputc('\n', stderr);
}
#endif

/* ------------------------------------------------------------------ */
/* Image loading                                                       */
/* ------------------------------------------------------------------ */

static uint8_t *read_file(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        fprintf(stderr, "emu: %s: %s\n", path, strerror(errno));
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fprintf(stderr, "emu: %s: not seekable\n", path);
        fclose(f);
        return NULL;
    }
    const long n = ftell(f);
    if (n < 0) {
        fprintf(stderr, "emu: %s: %s\n", path, strerror(errno));
        fclose(f);
        return NULL;
    }
    rewind(f);

    uint8_t *buf = malloc((size_t)n ? (size_t)n : 1u);
    if (buf == NULL) {
        fclose(f);
        fprintf(stderr, "emu: out of memory\n");
        return NULL;
    }
    if (fread(buf, 1u, (size_t)n, f) != (size_t)n) {
        fprintf(stderr, "emu: %s: short read\n", path);
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *out_len = (size_t)n;
    return buf;
}

static bool looks_like_elf(const uint8_t *b, size_t n)
{
    return n >= 4u && b[0] == 0x7F && b[1] == 'E' && b[2] == 'L' && b[3] == 'F';
}

/* e_machine of a little-endian ELF32, or 0 if the header is too short. */
static uint16_t elf_machine(const uint8_t *b, size_t n)
{
    return (n >= 20u) ? (uint16_t)(b[18] | ((uint16_t)b[19] << 8)) : 0u;
}

/* ------------------------------------------------------------------ */
/* Diagnostics                                                         */
/* ------------------------------------------------------------------ */

/* emu_print_fn onto stderr, for the frontend's own state dump. */
static void err_puts(void *ctx, const char *s)
{
    (void)ctx;
    fputs(s, stderr);
}

static void list_frontends(FILE *f)
{
    for (const emu_cpu_ops_t *const *p = emu_frontends; *p != NULL; p++) {
        fprintf(f, "%s%s", (p == emu_frontends) ? "" : ", ", (*p)->name);
    }
}

/* ------------------------------------------------------------------ */
/* Entry                                                               */
/* ------------------------------------------------------------------ */

static void usage(void)
{
    fprintf(stderr,
        "usage: emu-host [options] <image>\n"
        "\n"
        "  <image>              flat binary (preferred) or static ELF32\n"
        "\n"
        "  --frontend NAME      guest ISA (default: from the ELF header,\n"
        "                       else the first compiled in). This build has: ");
    list_frontends(stderr);
    fprintf(stderr,
        "\n"
        "  --load ADDR          load address for a flat binary\n"
        "                       (default 0x%08x)\n"
        "  --entry ADDR         reset pc (default: load address, or the\n"
        "                       ELF entry point)\n"
        "  --ram BYTES          guest RAM size (default %u)\n"
        "  --max-insn N         stop after N instructions (0 = unlimited)\n"
        "  --timer-hz N         timer ticks per second of guest time\n"
        "  --cores N            cores to run (default: the frontend's count)\n"
        "  --quantum N          instructions per core per round (default %u).\n"
        "                       1 is instruction-interleaved lockstep\n"
#if EMU_FRONTEND_RV32
        "  --jit                use the JIT backend instead of the interpreter\n"
        "  --gdb [port]         serve a gdb stub on localhost (default 1234)\n"
#endif
        "  --quiet              suppress the exit summary\n"
        "  --dump               dump register state on exit\n",
        EMU_GUEST_RAM_BASE, DEFAULT_RAM_SIZE,
        (unsigned)EMU_DEFAULT_BUDGET);
}

static bool parse_u32(const char *s, uint32_t *out)
{
    char *end = NULL;
    errno = 0;
    const unsigned long long v = strtoull(s, &end, 0);
    if (errno != 0 || end == s || *end != '\0' || v > 0xFFFFFFFFull) {
        return false;
    }
    *out = (uint32_t)v;
    return true;
}

#ifdef EMU_JIT_DIFF
/*
 * A block whose compiled code disagreed with the IR interpreter.
 *
 * `off` is a byte offset into the guest state, so for both frontends the
 * register file starts at zero and the number is the register times
 * four. The first divergence is the useful one -- everything after it is
 * downstream of the same bug.
 */
void emu_jit_diff_report(uint32_t pc, uint32_t off, uint32_t want,
                         uint32_t got)
{
    static unsigned reported;

    if (reported++ < 20u) {
        fprintf(stderr,
                "jit-diff: block pc=%08x state+%u want=%08x got=%08x\n",
                pc, off, want, got);
    }
}
#endif

bool host_gdb_start(emu_core_t *core, const emu_gdb_target_t *target, int port);
void host_gdb_wait(void);
void host_gdb_poll(void);
bool host_gdb_attached(void);
uint32_t host_gdb_run(uint32_t budget, uint32_t *retired);
const emu_gdb_target_t *rv32_gdb_target(void);

int main(int argc, char **argv)
{
    const char *path = NULL;
    const char *frontend_name = NULL;
    uint32_t load_addr = EMU_GUEST_RAM_BASE;
    uint32_t entry = 0;
    bool have_entry = false;
    uint32_t ram_size = DEFAULT_RAM_SIZE;
    uint64_t max_insn = 0;
    int gdb_port = 0;
    /* Instructions per timer tick. 1 keeps guest time in step with the
     * cycle counter, which is what the reference model assumes. */
    uint32_t timer_div = 1;
    unsigned ncores = 0;                    /* 0 = ask the frontend */
    uint32_t quantum = EMU_DEFAULT_BUDGET;
    bool quiet = false;
    bool dump = false;

    bool want_jit = false;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];

        if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
            usage();
            return 0;
        }
        if (strcmp(a, "--quiet") == 0) { quiet = true; continue; }
        if (strcmp(a, "--dump") == 0)  { dump = true;  continue; }

        if (i + 1 < argc) {
            if (strcmp(a, "--frontend") == 0) {
                frontend_name = argv[++i];
                continue;
            }
            if (strcmp(a, "--load") == 0) {
                if (!parse_u32(argv[++i], &load_addr)) { usage(); return 2; }
                continue;
            }
            if (strcmp(a, "--entry") == 0) {
                if (!parse_u32(argv[++i], &entry)) { usage(); return 2; }
                have_entry = true;
                continue;
            }
            if (strcmp(a, "--ram") == 0) {
                if (!parse_u32(argv[++i], &ram_size)) { usage(); return 2; }
                continue;
            }
            if (strcmp(a, "--max-insn") == 0) {
                uint32_t v;
                if (!parse_u32(argv[++i], &v)) { usage(); return 2; }
                max_insn = v;
                continue;
            }
#if EMU_FRONTEND_RV32
            if (strcmp(a, "--gdb") == 0) {
                /* Port only; the stub listens on loopback. Waits for a
                 * client before the first instruction, because the whole
                 * guest is over in milliseconds otherwise. */
                gdb_port = 1234;
                if (i + 1 < argc && argv[i + 1][0] != '-') {
                    uint32_t v;
                    if (!parse_u32(argv[++i], &v)) { usage(); return 2; }
                    gdb_port = (int)v;
                }
                continue;
            }
            if (strcmp(a, "--jit") == 0) {
#if RV_ENABLE_JIT
                /*
                 * The whole reason the x86-64 backend exists: with this,
                 * the architecture suite and riscv-tests run against
                 * *translated* code -- coverage the Thumb-2 backend can
                 * only get by flashing a board.
                 */
                want_jit = true;
#else
                fprintf(stderr, "rv32: no JIT backend for this host\n");
                return 2;
#endif
                continue;
            }
#endif
#if EMU_ENABLE_TRACE
            if (strcmp(a, "--trace-skip") == 0) {
                uint32_t v; if (!parse_u32(argv[++i], &v)) { usage(); return 2; }
                g_trace_skip = v; continue;
            }
            if (strcmp(a, "--trace-count") == 0) {
                uint32_t v; if (!parse_u32(argv[++i], &v)) { usage(); return 2; }
                g_trace_count = v; continue;
            }
#endif
            if (strcmp(a, "--timer-hz") == 0) {
                if (!parse_u32(argv[++i], &timer_div)) { usage(); return 2; }
                continue;
            }
            if (strcmp(a, "--cores") == 0) {
                uint32_t v;
                if (!parse_u32(argv[++i], &v) || v == 0u) { usage(); return 2; }
                ncores = (unsigned)v;
                continue;
            }
            if (strcmp(a, "--quantum") == 0) {
                if (!parse_u32(argv[++i], &quantum) || quantum == 0u) {
                    usage(); return 2;
                }
                continue;
            }
        }

        if (a[0] == '-') {
            fprintf(stderr, "emu: unknown option %s\n", a);
            usage();
            return 2;
        }
        if (path != NULL) {
            fprintf(stderr, "emu: more than one image given\n");
            return 2;
        }
        path = a;
    }

    if (path == NULL) {
        usage();
        return 2;
    }

    /* --- read the image first, so its ELF header can pick a frontend - */
    size_t len = 0;
    uint8_t *image = read_file(path, &len);
    if (image == NULL) {
        return 1;
    }

    const emu_cpu_ops_t *ops;
    if (frontend_name != NULL) {
        ops = emu_frontend_find(frontend_name);
        if (ops == NULL) {
            fprintf(stderr, "emu: no frontend '%s'; this build has: ",
                    frontend_name);
            list_frontends(stderr);
            fputc('\n', stderr);
            free(image);
            return 2;
        }
    } else if (looks_like_elf(image, len)) {
        const uint16_t m = elf_machine(image, len);
        ops = emu_frontend_for_elf(m);
        if (ops == NULL) {
            fprintf(stderr,
                    "emu: no frontend for ELF machine %u; this build has: ", m);
            list_frontends(stderr);
            fputc('\n', stderr);
            free(image);
            return 2;
        }
    } else {
        /* A flat binary says nothing about its architecture. */
        ops = emu_frontend_default();
    }

    /* --- guest memory ------------------------------------------------ */
    g_ram = calloc(ram_size, 1u);
    g_periph = calloc(PERIPH_SIM_SIZE, 1u);
    if (g_ram == NULL || g_periph == NULL) {
        fprintf(stderr, "emu: cannot allocate guest memory\n");
        free(image);
        return 1;
    }

    static emu_uart_t uart;

    if (ncores == 0u) {
        ncores = (ops->ncores != 0u) ? ops->ncores : 1u;
    }
    if (ncores > EMU_MAX_CORES) {
        fprintf(stderr, "emu: %u cores exceeds EMU_MAX_CORES (%u)\n",
                ncores, (unsigned)EMU_MAX_CORES);
        free(image);
        return 1;
    }

    /*
     * The shared regions go into every core's bus, pointing at the same
     * backing memory -- so RAM really is shared, and only the frontend's
     * core-relative windows differ between them.
     */
    for (unsigned i = 0; i < ncores; i++) {
        emu_bus_init(&g_bus[i]);
        if (!emu_bus_add_ram(&g_bus[i], "ram", EMU_GUEST_RAM_BASE,
                             g_ram, ram_size) ||
            !emu_bus_add_ram(&g_bus[i], "periph-sim", EMU_GUEST_PERIPH_BASE,
                             g_periph, PERIPH_SIM_SIZE) ||
            !emu_bus_add_mmio(&g_bus[i], "uart0", EMU_GUEST_UART_BASE,
                              EMU_UART_SIZE, &emu_uart_ops, &uart)) {
            fprintf(stderr, "emu: failed to build the guest memory map\n");
            free(image);
            return 1;
        }
    }

    /*
     * Opens the cores and lets the frontend add its own devices -- the
     * shared ones to every bus, the per-core ones to the core they belong
     * to. They are part of its architecture rather than of this platform,
     * and only it knows where in the guest map they go.
     */
    if (!emu_system_open(&g_sys, ops, g_bus, ncores)) {
        fprintf(stderr, "emu: could not bring up %u %s core%s\n",
                ncores, ops->name, (ncores == 1u) ? "" : "s");
        free(image);
        return 1;
    }


#if EMU_FRONTEND_RV32 && RV_ENABLE_JIT
    /*
     * The frontend prefers the JIT wherever it is compiled in, which is
     * right for firmware: there it is a speed choice, and the backend
     * falls back per instruction for anything it cannot translate.
     *
     * On the host it is a *coverage* choice, and the two must be runnable
     * against each other -- the architecture suite passing interpreted and
     * passing through translated code are different claims, and the README
     * distinguishes them. So the host states which it wants rather than
     * inheriting a default that could move under it.
     */
    rv_backend = want_jit ? &rv_backend_jit : &rv_backend_interp;
    if (rv_backend->init != NULL && !rv_backend->init(g_core.cpu)) {
        fprintf(stderr, "emu: backend init failed\n");
        free(image);
        return 1;
    }
#else
    (void)want_jit;
#endif

    emu_uart_init(&uart, host_tx, host_rx, NULL);
    /* Every core gets the hooks: any of them may make a system call. */
    for (unsigned i = 0; i < g_sys.ncores; i++) {
        if (ops->set_syscall != NULL) {
            ops->set_syscall(g_sys.core[i].cpu, host_syscall, NULL);
        }
#if EMU_ENABLE_TRACE
        if (ops->set_trace != NULL) {
            ops->set_trace(g_sys.core[i].cpu, host_trace, NULL);
        }
#endif
    }

    /* --- load -------------------------------------------------------- */
    if (looks_like_elf(image, len)) {
        uint32_t elf_entry = 0;
        const char *err = emu_elf_load(g_core.bus, image, len,
                                       ops->elf_machine, ops->elf_machine_alt,
                                       &elf_entry, NULL);
        if (err != NULL) {
            fprintf(stderr, "emu: %s: %s\n", path, err);
            free(image);
            return 1;
        }
        if (!have_entry) {
            entry = elf_entry;
            have_entry = true;
        }
    } else {
        if (!emu_bus_load(g_core.bus, load_addr, image, (uint32_t)len)) {
            fprintf(stderr,
                    "emu: %s: %zu bytes do not fit at 0x%08x\n",
                    path, len, load_addr);
            free(image);
            return 1;
        }
        if (!have_entry) {
            entry = load_addr;
        }
    }
    free(image);

    emu_system_reset(&g_sys, entry);
    /*
     * Hand the guest a stack and its RAM size. The architecture suite's
     * images set up their own stack from their link script and ignore
     * this, which is exactly the fallback the protocol allows for.
     */
    emu_system_boot(&g_sys, EMU_GUEST_RAM_BASE, ram_size);
    emu_system_invalidate(&g_sys, 0u, 0xFFFFFFFFu);

    /* --- run --------------------------------------------------------- */
    /*
     * A round-robin scheduler, which for a single-core frontend is exactly
     * the loop it replaces -- one core, one budget, repeat.
     *
     * `quantum` is the interleaving knob and the whole reason this shape
     * was chosen: 1 is instruction-interleaved lockstep, which is what
     * finds guest races; larger is faster. Fully deterministic either way,
     * so a failure is reproducible -- which matters most for the frontend
     * that has no reference model to check against.
     */
    uint64_t total = 0;
    if (gdb_port != 0) {
        if (!host_gdb_start(&g_core, rv32_gdb_target(), gdb_port)) {
            fprintf(stderr, "gdb: could not listen on port %d\n", gdb_port);
            return 2;
        }
        host_gdb_wait();        /* the guest is milliseconds long */
    }

    for (;;) {
        uint32_t q = quantum;

        /*
         * `total >= max_insn` rather than a budget that reaches zero.
         *
         * A backend may retire *more* than the budget it was given: the
         * JIT executes whole translated blocks and can only stop between
         * them, so the last one overshoots. `max_insn - total` is then an
         * unsigned subtraction below zero, which is a very large number,
         * so the comparison below leaves the quantum at its default and
         * this loop never ends -- the guest keeps running correctly and
         * the cap silently stops existing. The interpreter retires one
         * instruction at a time and lands exactly on the cap, which is why
         * this was invisible until there was a second backend.
         */
        if (max_insn != 0) {
            if (total >= max_insn) {
                if (!quiet) {
                    fprintf(stderr, "emu: instruction limit reached\n");
                }
                break;
            }
            if ((uint64_t)q > max_insn - total) {
                q = (uint32_t)(max_insn - total);
            }
        }

        bool all_idle = false;
        uint32_t did;

        if (gdb_port != 0) {
            /*
             * With a debugger attached the stub owns run control. Polled
             * once per slice, exactly as the firmware does it, so a run
             * with no --gdb pays one predictable branch.
             */
            host_gdb_poll();
            if (host_gdb_attached()) {
                uint32_t n = 0;
                (void)host_gdb_run(q, &n);
                did = n;
            } else {
                did = emu_system_step(&g_sys, q, &all_idle);
            }
        } else {
            did = emu_system_step(&g_sys, q, &all_idle);
        }
        total += did;

        /*
         * Guest time advances with executed instructions: there is no wall
         * clock to track here, and a deterministic time base makes runs
         * reproducible. One tick per instruction matches the rate the
         * cycle counter advances at, which is what the architecture
         * suite's Sail config declares.
         *
         * Once per round, not once per core, or it would run N times fast.
         */
        if (timer_div != 0u && ops->advance_time != NULL) {
            ops->advance_time(g_core.cpu, did / timer_div);
        }

        if (all_idle) {
            if (!quiet && did == 0u) {
                fprintf(stderr, "emu: all cores idle\n");
            }
            break;
        }
    }

#if RV_PAIR_STATS
    rv_pair_report(40u);
#endif
    if (dump) {
        for (unsigned i = 0; i < g_sys.ncores; i++) {
            if (g_sys.ncores > 1u) {
                fprintf(stderr, "\n--- core %u ---", i);
            }
            g_sys.ops->dump(g_sys.core[i].cpu, err_puts, NULL);
        }
    }
    if (!quiet) {
        fprintf(stderr, "emu: %llu instructions retired\n",
                (unsigned long long)total);
        if (g_sys.ncores > 1u) {
            /* Per-core counts, because that is what a determinism check
             * compares between two runs. */
            for (unsigned i = 0; i < g_sys.ncores; i++) {
                emu_cpu_status_t st;
                emu_core_status(&g_sys.core[i], &st);
                fprintf(stderr, "emu:   core %u: %llu\n", i,
                        (unsigned long long)st.retired);
            }
        }
        /*
         * Printed whenever anything was translated, for any frontend --
         * the framework owns these now, so there is one place to read them
         * from rather than one per backend.
         *
         * `interp` against the retired count is what matters when reading
         * a suite result: a backend that translated nothing and fell back
         * for everything passes every test while proving nothing about the
         * translator, which has already happened here once.
         */
        {
            emu_jit_stats_t st;

            emu_jit_get_stats(&st);
            if (st.translations != 0u || st.block_entries != 0u) {
                fprintf(stderr,
                        "emu: jit blocks %u  xlat %u  entries %u  interp %u  "
                        "code %u/%u  flushes %u\n",
                        st.blocks, st.translations, st.block_entries,
                        st.interp_fallbacks, st.code_used, st.code_size,
                        st.flushes);
            }
        }
    }

    return (g_exit_code >= 0) ? g_exit_code : 0;
}
