/* SPDX-License-Identifier: Apache-2.0 */
/*
 * main.c - Host runner for the RV32 core.
 *
 * Runs the same core the firmware runs, against the same guest memory map,
 * on a development machine. This is where the instruction-level tests run:
 * iterating here is far faster than reflashing, and any divergence between
 * host and target is a bug in the platform layer, not the core.
 *
 * The primary input is a flat binary, which is what the target consumes.
 * ELF is accepted as a convenience because the RISC-V test suites ship
 * that way; the loader for it is host-only and is never built into the
 * firmware, where flash is scarce.
 */

#include "rv32/rv_backend.h"
#include "rv32/rv_dev.h"
#include "rv32/rv_aplic.h"
#include "rv32/rv_hart.h"
#include "rv32/rv_memmap.h"
#include "rv32/rv_disasm.h"
#include "rv_elf.h"

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
/* ECALL services                                                      */
/* ------------------------------------------------------------------ */

/*
 * A tiny subset of the RISC-V newlib syscall ABI, which is what
 * riscv64-unknown-elf-gcc's crt0 and the standard test harnesses emit:
 *
 *   a7 = 64  write(fd, buf, len)
 *   a7 = 93  exit(code)
 *
 * Anything else falls through to a normal M-mode trap so guest software
 * with its own handler keeps working.
 */
#define REG_A0  10
#define REG_A1  11
#define REG_A2  12
#define REG_A7  17

static int g_exit_code = -1;

#if RV_ENABLE_TRACE
/*
 * Instruction trace. Prints pc, the encoding, the disassembly and the
 * registers the instruction touched, which is what is needed to find where
 * execution diverges from a reference model.
 */
static uint64_t g_trace_skip;
static uint64_t g_trace_count = 64;

static void host_trace(rv_hart_t *h, uint32_t pc, uint32_t insn, void *user)
{
    (void)user;
    if (h->minstret < g_trace_skip) {
        return;
    }
    if (h->minstret >= g_trace_skip + g_trace_count) {
        return;
    }
    char buf[64];
    rv_disasm(buf, sizeof(buf), pc, insn);
    fprintf(stderr, "%8llu %08x  %08x  %-28s",
            (unsigned long long)h->minstret, pc, insn, buf);
    /* gp, sp and the operand registers cover most divergence hunts. */
    fprintf(stderr, " gp=%08x sp=%08x a1=%08x a2=%08x t2=%08x\n",
            h->x[3], h->x[2], h->x[11], h->x[12], h->x[7]);
}
#endif

static bool host_ecall(rv_hart_t *h, void *user)
{
    (void)user;

    switch (h->x[REG_A7]) {
    case 64: {   /* write */
        const uint32_t buf = h->x[REG_A1];
        const uint32_t len = h->x[REG_A2];
        for (uint32_t i = 0; i < len; i++) {
            uint32_t byte;
            if (rv_bus_read(h->bus, buf + i, 1u, &byte) != RV_EXC_NONE) {
                break;
            }
            fputc((int)byte, stdout);
        }
        fflush(stdout);
        h->x[REG_A0] = len;
        return true;
    }

    case 93:     /* exit */
        g_exit_code = (int)h->x[REG_A0];
        h->state = RV_STATE_HALTED;
        return true;

    default:
        return false;
    }
}

/* ------------------------------------------------------------------ */
/* Image loading                                                       */
/* ------------------------------------------------------------------ */

static uint8_t *read_file(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        fprintf(stderr, "rv32: %s: %s\n", path, strerror(errno));
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fprintf(stderr, "rv32: %s: not seekable\n", path);
        fclose(f);
        return NULL;
    }
    const long n = ftell(f);
    if (n < 0) {
        fprintf(stderr, "rv32: %s: %s\n", path, strerror(errno));
        fclose(f);
        return NULL;
    }
    rewind(f);

    uint8_t *buf = malloc((size_t)n ? (size_t)n : 1u);
    if (buf == NULL) {
        fclose(f);
        fprintf(stderr, "rv32: out of memory\n");
        return NULL;
    }
    if (fread(buf, 1u, (size_t)n, f) != (size_t)n) {
        fprintf(stderr, "rv32: %s: short read\n", path);
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

/* ------------------------------------------------------------------ */
/* Diagnostics                                                         */
/* ------------------------------------------------------------------ */

static const char *cause_name(uint32_t mcause)
{
    if (mcause & RV_CAUSE_INTERRUPT) {
        switch (mcause & ~RV_CAUSE_INTERRUPT) {
        case RV_INT_M_SOFT:  return "machine software interrupt";
        case RV_INT_M_TIMER: return "machine timer interrupt";
        case RV_INT_M_EXT:   return "machine external interrupt";
        default:             return "unknown interrupt";
        }
    }
    switch (mcause) {
    case RV_EXC_INSN_MISALIGNED:    return "instruction address misaligned";
    case RV_EXC_INSN_ACCESS_FAULT:  return "instruction access fault";
    case RV_EXC_ILLEGAL_INSN:       return "illegal instruction";
    case RV_EXC_BREAKPOINT:         return "breakpoint";
    case RV_EXC_LOAD_MISALIGNED:    return "load address misaligned";
    case RV_EXC_LOAD_ACCESS_FAULT:  return "load access fault";
    case RV_EXC_STORE_MISALIGNED:   return "store address misaligned";
    case RV_EXC_STORE_ACCESS_FAULT: return "store access fault";
    case RV_EXC_ECALL_M:            return "environment call from M-mode";
    default:                        return "unknown exception";
    }
}

static void dump_state(const rv_hart_t *h)
{
    static const char *const abi[32] = {
        "zero", "ra", "sp", "gp", "tp", "t0", "t1", "t2",
        "s0",   "s1", "a0", "a1", "a2", "a3", "a4", "a5",
        "a6",   "a7", "s2", "s3", "s4", "s5", "s6", "s7",
        "s8",   "s9", "s10","s11","t3", "t4", "t5", "t6",
    };

    fprintf(stderr, "\n  pc      %08x\n", h->pc);
    fprintf(stderr, "  mcause  %08x  (%s)\n", h->mcause, cause_name(h->mcause));
    fprintf(stderr, "  mepc    %08x   mtval %08x\n", h->mepc, h->mtval);
    fprintf(stderr, "  mstatus %08x   mtvec %08x\n", h->mstatus, h->mtvec);
    fprintf(stderr, "  priv    %u\n", (unsigned)h->priv);
#if RV_EXT_S
    fprintf(stderr, "  scause  %08x   sepc  %08x  stval %08x\n",
            h->scause, h->sepc, h->stval);
    fprintf(stderr, "  stvec   %08x   medeleg %08x  mideleg %08x\n",
            h->stvec, h->medeleg, h->mideleg);
#endif
    for (unsigned i = 0; i < 32u; i += 4u) {
        fprintf(stderr, "  ");
        for (unsigned j = 0; j < 4u; j++) {
            fprintf(stderr, "%-4s %08x  ", abi[i + j], h->x[i + j]);
        }
        fprintf(stderr, "\n");
    }
    fprintf(stderr, "  retired %llu  traps %u\n",
            (unsigned long long)h->minstret,
#if RV_ENABLE_STATS
            h->trap_count
#else
            0u
#endif
    );
}

/* ------------------------------------------------------------------ */
/* Entry                                                               */
/* ------------------------------------------------------------------ */

static void usage(void)
{
    fprintf(stderr,
        "usage: rv32-host [options] <image>\n"
        "\n"
        "  <image>              flat binary (preferred) or static RV32 ELF\n"
        "\n"
        "  --load ADDR          load address for a flat binary\n"
        "                       (default 0x%08x)\n"
        "  --entry ADDR         reset pc (default: load address, or the\n"
        "                       ELF entry point)\n"
        "  --ram BYTES          guest RAM size (default %u)\n"
        "  --max-insn N         stop after N instructions (0 = unlimited)\n"
        "  --timer-hz N         CLINT mtime ticks per second of guest time\n"
        "  --quiet              suppress the exit summary\n"
        "  --dump               dump register state on exit\n",
        RV_GUEST_RAM_BASE, DEFAULT_RAM_SIZE);
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

int main(int argc, char **argv)
{
    const char *path = NULL;
    uint32_t load_addr = RV_GUEST_RAM_BASE;
    uint32_t entry = 0;
    bool have_entry = false;
    uint32_t ram_size = DEFAULT_RAM_SIZE;
    uint64_t max_insn = 0;
    /* Instructions per CLINT mtime tick. 1 keeps mtime in step with the
     * cycle counter, which is what the reference model assumes. */
    uint32_t timer_div = 1;
    bool quiet = false;
    bool dump = false;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];

        if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
            usage();
            return 0;
        }
        if (strcmp(a, "--quiet") == 0) { quiet = true; continue; }
        if (strcmp(a, "--dump") == 0)  { dump = true;  continue; }

        if (i + 1 < argc) {
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
#if RV_ENABLE_TRACE
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
        }

        if (a[0] == '-') {
            fprintf(stderr, "rv32: unknown option %s\n", a);
            usage();
            return 2;
        }
        if (path != NULL) {
            fprintf(stderr, "rv32: more than one image given\n");
            return 2;
        }
        path = a;
    }

    if (path == NULL) {
        usage();
        return 2;
    }

    /* --- guest memory ------------------------------------------------ */
    g_ram = calloc(ram_size, 1u);
    g_periph = calloc(PERIPH_SIM_SIZE, 1u);
    if (g_ram == NULL || g_periph == NULL) {
        fprintf(stderr, "rv32: cannot allocate guest memory\n");
        return 1;
    }

    static rv_bus_t bus;
    static rv_hart_t hart;
    static rv_clint_t clint;
    static rv_aplic_t aplic;
    static rv_uart_t uart;

    rv_bus_init(&bus);
    if (!rv_bus_add_ram(&bus, "ram", RV_GUEST_RAM_BASE, g_ram, ram_size) ||
        !rv_bus_add_ram(&bus, "periph-sim", RV_GUEST_PERIPH_BASE,
                        g_periph, PERIPH_SIM_SIZE) ||
        !rv_bus_add_mmio(&bus, "aplic", RV_GUEST_APLIC_BASE, RV_APLIC_SIZE,
                         &rv_aplic_ops, &aplic) ||
        !rv_bus_add_mmio(&bus, "aclint-mswi", RV_GUEST_ACLINT_MSWI_BASE,
                         RV_ACLINT_MSWI_SIZE, &rv_aclint_mswi_ops, &clint) ||
        !rv_bus_add_mmio(&bus, "aclint-mtimer", RV_GUEST_ACLINT_MTIMER_BASE,
                         RV_ACLINT_MTIMER_SIZE, &rv_aclint_mtimer_ops,
                         &clint) ||
        !rv_bus_add_mmio(&bus, "uart0", RV_GUEST_UART_BASE, RV_UART_SIZE,
                         &rv_uart_ops, &uart)) {
        fprintf(stderr, "rv32: failed to build the guest memory map\n");
        return 1;
    }

    rv_hart_init(&hart, &bus, 0u);
    rv_clint_init(&clint, &hart);
    rv_aplic_init(&aplic, &hart);
    rv_uart_init(&uart, host_tx, host_rx, NULL);
#if RV_ENABLE_ECALL_HOOK
    hart.ecall = host_ecall;
#endif
#if RV_ENABLE_TRACE
    hart.trace = host_trace;
#endif

    /* --- load -------------------------------------------------------- */
    size_t len = 0;
    uint8_t *image = read_file(path, &len);
    if (image == NULL) {
        return 1;
    }

    if (looks_like_elf(image, len)) {
        uint32_t elf_entry = 0;
        const char *err = rv_elf_load(&bus, image, len, &elf_entry);
        if (err != NULL) {
            fprintf(stderr, "rv32: %s: %s\n", path, err);
            free(image);
            return 1;
        }
        if (!have_entry) {
            entry = elf_entry;
            have_entry = true;
        }
    } else {
        if (!rv_bus_load(&bus, load_addr, image, (uint32_t)len)) {
            fprintf(stderr,
                    "rv32: %s: %zu bytes do not fit at 0x%08x\n",
                    path, len, load_addr);
            free(image);
            return 1;
        }
        if (!have_entry) {
            entry = load_addr;
        }
    }
    free(image);

    rv_hart_reset(&hart, entry);
    /*
     * Hand the guest a stack and its RAM size. The architecture suite's
     * images set up their own stack from their link script and ignore
     * this, which is exactly the fallback the protocol allows for.
     */
    rv_hart_boot(&hart, RV_GUEST_RAM_BASE, ram_size);
    rv_invalidate(&hart, 0u, 0xFFFFFFFFu);

    /* --- run --------------------------------------------------------- */
    uint64_t total = 0;
    for (;;) {
        uint32_t budget = RV_DEFAULT_BUDGET;
        if (max_insn != 0 && (uint64_t)budget > max_insn - total) {
            budget = (uint32_t)(max_insn - total);
        }
        if (budget == 0u) {
            if (!quiet) {
                fprintf(stderr, "rv32: instruction limit reached\n");
            }
            break;
        }

        uint32_t did = 0;
        const rv_run_reason_t why = rv_run(&hart, budget, &did);
        total += did;

        /*
         * Guest time advances with executed instructions: there is no wall
         * clock to track here, and a deterministic time base makes runs
         * reproducible. One tick per instruction matches the rate the
         * cycle counter advances at, which is what the architecture
         * suite's Sail config declares.
         */
        if (timer_div != 0u) {
            rv_clint_advance(&clint, did / timer_div);
        }

        if (why == RV_RUN_HALTED) {
            break;
        }
        if (why == RV_RUN_WFI && hart.mie == 0u) {
            /* Parked with nothing that could ever wake it. */
            if (!quiet) {
                fprintf(stderr, "rv32: WFI with no interrupts enabled\n");
            }
            break;
        }
    }

    if (dump) {
        dump_state(&hart);
    }
    if (!quiet) {
        fprintf(stderr, "rv32: %llu instructions retired\n",
                (unsigned long long)total);
    }

    return (g_exit_code >= 0) ? g_exit_code : 0;
}
