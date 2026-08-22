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
#include "emu_console.h"   /* the shared syscall handler and its context */
#include "emu_run.h"

#if EMU_NET
#  include "board.h"
#  include "emu_net.h"
#endif
#include "emu/emu_gdb.h"
#include "emu/emu_dev.h"
#include "emu/emu_elf.h"
#include "emu/emu_memmap.h"
#include "emu/emu_jit.h"
#include "emu/emu_ir.h"

/*
 * No frontend headers here, and that is the property to keep.
 *
 * This file used to include both frontends' backend headers so that
 * --jit could assign rv_backend or g4mh_backend directly, guarded by
 * EMU_GUEST_ARCH_*. Every one of those guards was a defect waiting: --jit
 * was parsed inside the RV32 block, so a G4MH-only build rejected it and
 * ran translated whatever was asked; --gdb was still inside it after
 * --jit was moved out, so a G4MH-only build rejected *that* while its
 * register description sat unreachable; and the RV32 assignment ran
 * unconditionally, calling rv_backend->init with a G4MH core pointer in a
 * build with both.
 *
 * ops->select_backend and ops->gdb_target replace all of it. The runner
 * now knows what a frontend *is*, not which ones exist.
 */

/*
 * Whether `--jit` means anything in this build.
 *
 * The host half of the question, and now the only half: whether the
 * *frontend* has a second backend is `ops->select_backend != NULL`, which
 * is a property of the frontend answered at run time rather than a macro
 * naming the frontends that exist. This used to be
 * `EMU_HAVE_JIT && (EMU_GUEST_ARCH_RV32 || EMU_GUEST_ARCH_G4MH)` -- a
 * list that a third frontend would have had to be added to, and that
 * nothing would have failed for omitting.
 */
#define EMU_JIT_SELECTABLE EMU_HAVE_JIT

#if EMU_PAIR_STATS
#  include "emu/emu_pairstats.h"
#endif

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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
#if EMU_NET
    /*
     * Once the link is up the guest's console is a telnet connection,
     * which is the board's arrangement and the point of running this
     * here. The *runner's* own diagnostics still go to stderr, because a
     * host has a terminal as well as a wire and giving one up buys
     * nothing -- see the note in board.h.
     */
    if (emu_net_active()) {
        emu_net_console_putc(c);
        return;
    }
#endif
    fputc(c, stdout);
    /* Unbuffered so output survives a guest that faults straight after. */
    fflush(stdout);
}

/*
 * A byte of *guest* output, for the shared syscall handler.
 *
 * Deliberately not the firmware's emu_console_putb, which expands LF to
 * CRLF: that is a property of a terminal on a serial line, not of a
 * console. Doing it here would put a \r into every line of every guest's
 * output on stdout, which two test suites compare and one figure script
 * parses.
 *
 * The same byte as host_tx, which is the guest's virtual UART -- one
 * console, so a guest that writes through the UART and one that writes
 * through the syscall interleave in the order they happened.
 */
static int host_rx(void *ctx)
{
    (void)ctx;
#if EMU_NET
    if (emu_net_active()) {
        return emu_net_console_getc();
    }
#endif
    return -1;   /* no interactive input in the batch runner */
}

/*
 * The runner's own output: the trace, the register dump, the summary.
 *
 * To stderr, and *also* to the telnet ring when the link is up. Both,
 * not either: the terminal is where a person watching this process
 * expects to see it, and a telnet session that carries the guest's
 * output but not the trace beside it is the wrong half -- reading a
 * trace against the output it produced is the whole point of having one.
 *
 * The board cannot do this. It has one wire and gives it away, so its
 * diagnostics go to the ring or nowhere. A host has both, so it uses
 * both.
 */
static void host_diag(const char *s)
{
    fputs(s, stderr);
#if EMU_NET
    if (emu_net_active()) {
        for (const char *p = s; *p != '\0'; p++) {
            emu_net_console_putc((uint8_t)*p);
        }
    }
#endif
}

static void host_diagf(const char *fmt, ...)
    __attribute__((format(printf, 1, 2)));

static void host_diagf(const char *fmt, ...)
{
    /*
     * 512, and the one caller that does not fit goes to stderr directly.
     *
     * Truncation here is silent, which is the right trade for a stats
     * line and the wrong one for a document: converting usage() to this
     * cut it off mid-option list, and the only symptom was that --jit and
     * --gdb stopped being advertised -- which reads exactly like the
     * build-time gate that had just been removed from them.
     */
    char buf[512];
    va_list ap;

    va_start(ap, fmt);
    (void)vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    host_diag(buf);
}

/* ------------------------------------------------------------------ */
/* System-call services                                                */
/* ------------------------------------------------------------------ */

/*
 * The guest's exit status. `exited` distinguishes a guest that called
 * exit() from one that halted or hit the cap, because a code of 0 means
 * nothing if the syscall was never reached.
 */
static emu_guest_exit_t g_exit;

/*
 * The syscall handler is emu_guest_syscall, shared with the firmware.
 *
 * This file used to carry its own -- the same newlib write(64)/exit(93)
 * pair, differing only in how it reached the bus. Two implementations of
 * one ABI, and the drift had already started: the shared one reads the
 * buffer a byte at a time *through the bus* so a buffer spanning two
 * regions works and a bad pointer faults rather than reaching into host
 * memory, and this copy did the same thing without the reasoning and
 * would not have kept doing it.
 */
static emu_syscall_ctx_t g_sc_ctx;

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

static void host_trace(emu_cpu_t *cpu, uint32_t pc, uint64_t insn,
                       unsigned len, void *user)
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
        g_core.ops->disasm(buf, sizeof(buf), pc, insn, len);
    }
    /*
     * The encoding, only as wide as it is. Printing a fixed eight digits
     * pads a 16-bit instruction with four zeros that look like part of
     * it -- which on an ISA where a shared opcode holds two widths is
     * exactly the thing the reader is trying to tell apart.
     */
    host_diagf("%8llu %08x  %0*llx%*s  %-28s",
               (unsigned long long)st.retired, pc,
               (int)(len * 2u), (unsigned long long)insn,
               (int)(16u - len * 2u), "", buf);

    for (unsigned r = 1; r < 8u && r < g_core.ops->nregs; r++) {
        host_diagf(" %s=%08x", g_core.ops->reg_name(r),
                   g_core.ops->reg_read(cpu, r));
    }
    host_diag("\n");
}
#endif

/* ------------------------------------------------------------------ */
/* Image loading                                                       */
/* ------------------------------------------------------------------ */

static uint8_t *read_file(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        host_diagf("emu: %s: %s\n", path, strerror(errno));
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        host_diagf("emu: %s: not seekable\n", path);
        fclose(f);
        return NULL;
    }
    const long n = ftell(f);
    if (n < 0) {
        host_diagf("emu: %s: %s\n", path, strerror(errno));
        fclose(f);
        return NULL;
    }
    rewind(f);

    uint8_t *buf = malloc((size_t)n ? (size_t)n : 1u);
    if (buf == NULL) {
        fclose(f);
        host_diagf("emu: out of memory\n");
        return NULL;
    }
    if (fread(buf, 1u, (size_t)n, f) != (size_t)n) {
        host_diagf("emu: %s: short read\n", path);
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
void emu_console_putb(uint8_t c);
void emu_console_putb(uint8_t c)
{
    host_tx(NULL, c);
}

/*
 * The diagnostic sink the shared files print through.
 *
 * Two functions rather than linking emu_console.c, because the host has
 * two sinks where a board has one: guest output goes to stdout (above)
 * and diagnostics go to stderr and the telnet ring (below). A board gives
 * its only wire away and has no such choice to make.
 */
void emu_console_puts(const char *s);
void emu_console_puts(const char *s)
{
    host_diag(s);
}

void emu_console_printf(const char *fmt, ...);
void emu_console_printf(const char *fmt, ...)
{
    char buf[512];
    va_list ap;

    va_start(ap, fmt);
    (void)vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    host_diag(buf);
}


#if EMU_NET
/* ------------------------------------------------------------------ */
/* Images arriving over TFTP                                           */
/* ------------------------------------------------------------------ */

/*
 * The board programs an uploaded image into a flash arena; here it is a
 * buffer, which is the whole difference. Everything above it -- the TFTP
 * server, the one-file contract, the commit-on-success rule -- is the
 * same code.
 *
 * The buffer grows as blocks arrive because TFTP carries no length: a
 * transfer ends when a short block does, so the size is known only at the
 * end. The board discovers the same thing by running out of arena.
 */
static uint8_t *g_up;
static uint32_t g_up_len;
static uint32_t g_up_cap;

static uint8_t *g_pending;      /* a complete image waiting to be run */
static uint32_t g_pending_len;

bool emu_net_image_begin(void)
{
    free(g_up);
    g_up = NULL;
    g_up_len = 0u;
    g_up_cap = 0u;
    return true;
}

bool emu_net_image_data(const void *data, uint32_t len, uint32_t off)
{
    if (off != g_up_len) {
        return false;           /* TFTP is sequential; a gap is a bug */
    }
    if (off + len > g_up_cap) {
        const uint32_t want = (g_up_cap == 0u) ? 65536u : g_up_cap * 2u;
        const uint32_t cap = (want > off + len) ? want : off + len;
        uint8_t *const p = realloc(g_up, cap);

        if (p == NULL) {
            return false;
        }
        g_up = p;
        g_up_cap = cap;
    }
    memcpy(g_up + off, data, len);
    g_up_len = off + len;
    return true;
}

void emu_net_image_end(uint32_t len, bool ok)
{
    if (!ok) {
        free(g_up);
        g_up = NULL;
        g_up_len = 0u;
        g_up_cap = 0u;
        host_diagf("emu: upload failed\n");
        return;
    }

    /*
     * Handed over rather than installed. The bus regions and the reset
     * vector are built from the current image, and rebuilding them from
     * inside a TFTP callback would pull the ground out from under the
     * guest whose slice is being serviced.
     */
    free(g_pending);
    g_pending = g_up;
    g_pending_len = len;
    g_up = NULL;
    g_up_len = 0u;
    g_up_cap = 0u;
}
#endif /* EMU_NET */

static void err_puts(void *ctx, const char *s)
{
    (void)ctx;
    host_diag(s);
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
        "                       (default 0x%08x, the flash window; a binary\n"
        "                        linked to run from RAM wants 0x%08x)\n"
        "  --entry ADDR         reset pc (default: load address, or the\n"
        "                       ELF entry point)\n"
        "  --ram BYTES          guest RAM size (default %u)\n"
        "  --max-insn N         stop after N instructions (0 = unlimited)\n"
        "  --timer-hz N         timer ticks per second of guest time\n"
        "  --cores N            cores to run (default: the frontend's count)\n"
        "  --quantum N          instructions per core per round (default %u).\n"
        "                       1 is instruction-interleaved lockstep\n"
#if EMU_JIT_SELECTABLE
        "  --jit                use the JIT backend instead of the interpreter\n"
#endif
        "  --gdb [port]         serve a gdb stub on localhost (default 1234)\n"
        "  --quiet              suppress the exit summary\n"
        "  --dump               dump register state on exit\n",
        EMU_GUEST_ROM_BASE, EMU_GUEST_RAM_BASE, DEFAULT_RAM_SIZE,
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


bool host_gdb_start(emu_core_t *core, const emu_gdb_target_t *target, int port);
void host_gdb_wait(void);
void host_gdb_poll(void);
bool host_gdb_attached(void);
uint32_t host_gdb_run(uint32_t budget, uint32_t *retired);
/*
 * Guest time advances with instructions retired: there is no wall clock
 * worth tracking here, and a deterministic time base is what makes two
 * runs of the same guest comparable -- which is the whole point of the
 * architecture suite. One tick per instruction matches the rate the cycle
 * counter advances at, which is what its Sail config declares.
 *
 * Once per round, not once per core, or it would run N times fast. The
 * firmware answers this hook from a real cycle counter instead, because
 * its guest drives real peripherals.
 */
static uint32_t g_timer_div = 1u;

static void advance_guest_time(uint64_t retired_total, uint32_t did)
{
    (void)retired_total;
    if (g_timer_div != 0u && g_sys.ops->advance_time != NULL) {
        g_sys.ops->advance_time(g_sys.core[0].cpu, did / g_timer_div);
    }
}

static bool build_buses(unsigned ncores, const uint8_t *img, uint32_t img_len,
                        uint32_t ram_bytes, emu_uart_t *uart);

#if EMU_NET
/*
 * State the reload needs. The firmware keeps the same three and calls
 * them the same thing; here they are file-scope because main() is where
 * they are set up and the hook is called from the run loop.
 */
static const emu_cpu_ops_t *g_ops;
static emu_uart_t          *g_uart_p;
static uint32_t             g_ram_bytes;
static uint32_t             g_img_len;
static uint8_t             *g_img_buf;

static void net_poll_hook(void)
{
    emu_net_poll();
}

/*
 * Take a freshly uploaded image, if one is waiting.
 *
 * The same shape as the firmware's: rebuild the address space, put the
 * frontend's devices back, clear RAM, reset. Skipping any of it leaves
 * the previous guest's state in place, which presents as the new guest
 * retiring nothing.
 */
static bool take_uploaded_image(void)
{
    if (g_pending == NULL) {
        return false;
    }

    uint8_t *const img = g_pending;
    const uint32_t n = g_pending_len;

    g_pending = NULL;

    if (!build_buses(g_sys.ncores, img, n, g_ram_bytes, g_uart_p)) {
        free(img);
        return false;
    }
    for (unsigned i = 0; i < g_sys.ncores; i++) {
        if ((g_ops->add_shared_devices != NULL &&
             !g_ops->add_shared_devices(&g_bus[i])) ||
            (g_ops->add_core_devices != NULL &&
             !g_ops->add_core_devices(g_sys.core[i].cpu, &g_bus[i], i))) {
            free(img);
            return false;
        }
    }

    memset(g_ram, 0, g_ram_bytes);
    free(g_img_buf);
    g_img_buf = img;
    g_img_len = n;

    emu_system_reset(&g_sys, EMU_GUEST_RESET_PC);
    emu_system_boot(&g_sys, EMU_GUEST_RAM_BASE, g_ram_bytes);

    host_diagf("emu: running uploaded image, %u bytes\n", (unsigned)n);
    return true;
}
#endif /* EMU_NET */

/*
 * The guest's address space, for every core.
 *
 * emu_bus_init clears the region table, so everything goes back on --
 * including, on a reload, the frontend's own devices, which the caller
 * re-adds afterwards exactly as the firmware's emu_start_guest does.
 */
static bool build_buses(unsigned ncores, const uint8_t *img, uint32_t img_len,
                        uint32_t ram_bytes, emu_uart_t *uart)
{
    for (unsigned i = 0; i < ncores; i++) {
        emu_bus_init(&g_bus[i]);
        /*
         * The image, read-only, where the guest's .data initialiser
         * lives -- __data_lma points into this window and start.S copies
         * from it.
         *
         * The firmware has had this window since it existed, because a
         * guest linked for execute-in-place reads its constants there.
         * The host never needed it while the emulator installed .data
         * for the guest, and adding it is what makes the same image run
         * unchanged on both: without it the guest faults in its own
         * first loop, before anything it could report with.
         */
        if (!emu_bus_add_ram(&g_bus[i], "ram", EMU_GUEST_RAM_BASE,
                             g_ram, ram_bytes) ||
            !emu_bus_add_rom(&g_bus[i], "rom", EMU_GUEST_ROM_BASE,
                             img, img_len) ||
            !emu_bus_add_ram(&g_bus[i], "periph-sim", EMU_GUEST_PERIPH_BASE,
                             g_periph, PERIPH_SIM_SIZE) ||
            !emu_bus_add_mmio(&g_bus[i], "uart0", EMU_GUEST_UART_BASE,
                              EMU_UART_SIZE, &emu_uart_ops, uart)) {
            host_diagf("emu: failed to build the guest memory map\n");
            return false;
        }
    }
    return true;
}

int main(int argc, char **argv)
{
    const char *path = NULL;
    const char *frontend_name = NULL;
    uint32_t load_addr = EMU_GUEST_ROM_BASE;
    uint32_t entry = 0;
    bool have_entry = false;
    uint32_t ram_size = DEFAULT_RAM_SIZE;
    uint64_t max_insn = 0;
    int gdb_port = 0;
#if EMU_NET
    bool        ppp = false;
    const char *ppp_dev = NULL;
#endif
    /* Instructions per timer tick. 1 keeps guest time in step with the
     * cycle counter, which is what the reference model assumes. */
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
#if EMU_NET
            if (strcmp(a, "--ppp") == 0) {
                /*
                 * The same IP stack the board runs, over a pty instead of
                 * a UART. An optional argument names an existing device
                 * for a caller that has already arranged one end.
                 */
                ppp = true;
                if (i + 1 < argc && argv[i + 1][0] != '-') {
                    ppp_dev = argv[++i];
                }
                continue;
            }
#endif
            /*
             * Not inside an RV32 block, for the same reason --jit is not:
             * the stub is in emucore, the register layout comes from
             * ops->gdb_target, and every frontend that has one gets it.
             * Guarded on RV32 this option was *rejected outright* by a
             * G4MH-only build, whose gdb target description existed and
             * was unreachable -- the identical defect --jit had, in the
             * same file, left behind when that one was fixed.
             *
             * A frontend with no description is caught where the stub is
             * started, by name, rather than by the option not existing.
             */
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
            /*
             * Not inside the RV32 block: both frontends have an x86-64
             * backend, and while this option was guarded on RV32 a
             * G4MH-only build rejected it outright -- so the only reachable
             * backend was whichever one the frontend happened to prefer.
             */
            if (strcmp(a, "--jit") == 0) {
#if EMU_JIT_SELECTABLE
                /*
                 * The whole reason the x86-64 backends exist: with this,
                 * the architecture suite and riscv-tests run against
                 * *translated* code -- coverage the Thumb-2 backend can
                 * only get by flashing a board.
                 */
                want_jit = true;
#else
                host_diagf("emu: no JIT backend for this host\n");
                return 2;
#endif
                continue;
            }
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
                if (!parse_u32(argv[++i], &g_timer_div)) { usage(); return 2; }
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
            host_diagf("emu: unknown option %s\n", a);
            usage();
            return 2;
        }
        if (path != NULL) {
            host_diagf("emu: more than one image given\n");
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
            host_diagf("emu: no frontend '%s'; this build has: ",
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
            host_diagf("emu: no frontend for ELF machine %u; this build has: ", m);
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
        host_diagf("emu: cannot allocate guest memory\n");
        return 1;
    }

    static emu_uart_t uart;

    if (ncores == 0u) {
        ncores = (ops->ncores != 0u) ? ops->ncores : 1u;
    }
    if (ncores > EMU_MAX_CORES) {
        host_diagf("emu: %u cores exceeds EMU_MAX_CORES (%u)\n",
                ncores, (unsigned)EMU_MAX_CORES);
        return 1;
    }

    /*
     * The shared regions go into every core's bus, pointing at the same
     * backing memory -- so RAM really is shared, and only the frontend's
     * core-relative windows differ between them.
     *
     * A function rather than inline, because an image arriving over TFTP
     * has to repeat all of it: the image region's base and length both
     * move and emu_bus cannot resize a region in place.
     */
    if (!build_buses(ncores, image, (uint32_t)len, ram_size, &uart)) {
        return 1;
    }

    /*
     * Opens the cores and lets the frontend add its own devices -- the
     * shared ones to every bus, the per-core ones to the core they belong
     * to. They are part of its architecture rather than of this platform,
     * and only it knows where in the guest map they go.
     */
    if (!emu_system_open(&g_sys, ops, g_bus, ncores)) {
        host_diagf("emu: could not bring up %u %s core%s\n",
                ncores, ops->name, (ncores == 1u) ? "" : "s");
        /*
         * Almost always the region table, and the message above says
         * "cores" -- which sent a whole debugging session after the
         * scheduler. Every device a frontend adds costs a region *per
         * bus*, so the count grows with the core count: G4MH at three PEs
         * needs about 20 against a default of 16, and the only symptom
         * was this line.
         *
         * Printed unconditionally rather than only when the table is
         * full, because emu_system_open reports one boolean and the
         * platform cannot tell which of its callees failed. Saying what
         * to check is cheap; guessing wrong is what cost the time.
         */
        host_diagf("emu:   %u of %u bus regions used on core 0 -- if that is "
                "the limit, rebuild with -DEMU_MAX_REGIONS=%u\n",
                emu_bus_region_count(&g_bus[0]), (unsigned)EMU_MAX_REGIONS,
                (unsigned)EMU_MAX_REGIONS + 8u);
        return 1;
    }


    /*
     * Which backend, stated rather than inherited -- see
     * emu_cpu_ops_t.select_backend for why the runner decides and why
     * this is a hook instead of a block per frontend.
     */
    if (ops->select_backend != NULL &&
        !ops->select_backend(g_core.cpu, want_jit)) {
        host_diagf("emu: backend init failed\n");
        return 1;
    }

#if !EMU_JIT_SELECTABLE
    (void)want_jit;
#endif

#if EMU_NET
    /*
     * The link, before the guest runs and after the cores exist -- the
     * gdb stub needs one to describe.
     *
     * Not fatal if it fails. A runner that cannot get a pty is still a
     * runner, and saying so beats refusing to run the guest; this is the
     * same judgement the firmware makes, for the same reason.
     */
    if (ppp) {
        char slave[64] = "";

        if (!board_console_open(ppp_dev, slave, sizeof(slave))) {
            host_diagf("emu: --ppp: no serial device; continuing "
                            "without a network\n");
        } else if (!emu_net_init()) {
            host_diagf("emu: --ppp: the IP stack would not start\n");
        } else {
            const emu_gdb_target_t *const gt =
                ops->gdb_target != NULL ? ops->gdb_target() : NULL;

            fprintf(stderr,
                    "emu: ppp on %s (%s <-> %s)\n"
                    "emu:   scripts/ppp-host.sh %s\n"
                    "emu:   then: telnet %s 23   |   tftp %s\n",
                    slave, EMU_NET_PEER, EMU_NET_ADDR, slave,
                    EMU_NET_ADDR, EMU_NET_ADDR);
            if (gt != NULL && emu_net_gdb_init(&g_core, gt, NULL)) {
                host_diagf("emu:   gdb: target remote %s:1234\n",
                        EMU_NET_ADDR);
            }
        }
    }
#endif

    g_sc_ctx.bus  = g_core.bus;
    g_sc_ctx.core = &g_core;
    g_sc_ctx.exit = &g_exit;

#if EMU_NET
    /* What a reload has to redo, recorded once here. */
    g_ops       = ops;
    g_uart_p    = &uart;
    g_ram_bytes = ram_size;
    g_img_len   = (uint32_t)len;
    g_img_buf   = NULL;         /* the initial image is main()'s to own */
#endif

    emu_uart_init(&uart, host_tx, host_rx, NULL);
    /* Every core gets the hooks: any of them may make a system call. */
    for (unsigned i = 0; i < g_sys.ncores; i++) {
        if (ops->set_syscall != NULL) {
            ops->set_syscall(g_sys.core[i].cpu, emu_guest_syscall,
                             &g_sc_ctx);
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
            host_diagf("emu: %s: %s\n", path, err);
            return 1;
        }
        if (!have_entry) {
            entry = elf_entry;
            have_entry = true;
        }
    } else if (load_addr != EMU_GUEST_ROM_BASE) {
        /*
         * A flat binary somewhere other than flash: written into the bus
         * as before. At the default address there is nothing to write --
         * see below.
         */
        if (!emu_bus_load(g_core.bus, load_addr, image, (uint32_t)len)) {
            host_diagf("emu: %s: %zu bytes do not fit at 0x%08x\n",
                    path, len, load_addr);
            return 1;
        }
        if (!have_entry) {
            entry = load_addr;
        }
    } else if (!have_entry) {
        /*
         * The common case, and nothing to do: the flash window *is* this
         * buffer, added read-only when the bus was built, so the image is
         * already where the guest will fetch it from.
         */
        entry = load_addr;
    }

    /*
     * `image` is deliberately not freed: the flash region points into it
     * for the lifetime of the run, and an ELF's segments were copied out
     * of it but the window still refers to it. Freeing it here left the
     * guest executing out of a freed buffer -- which read correctly,
     * because nothing had reused the allocation yet, and is exactly the
     * kind of bug that surfaces later under a different allocator.
     */

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
        /* The frontend states its own layout -- see
         * emu_cpu_ops_t.gdb_target. */
        const emu_gdb_target_t *gt =
            ops->gdb_target != NULL ? ops->gdb_target() : NULL;

        if (gt == NULL) {
            host_diagf("emu: no gdb target for frontend %s\n", ops->name);
            return 1;
        }
        if (!host_gdb_start(&g_core, gt, gdb_port)) {
            host_diagf("gdb: could not listen on port %d\n", gdb_port);
            return 2;
        }
        host_gdb_wait();        /* the guest is milliseconds long */
    }

#if EMU_NET
restart:
#endif
    {
        /*
         * The same loop the firmware runs -- see
         * src/platform/common/emu_run.c. What differs between a CLI and a
         * board is four hooks, and this is three of them; the fourth,
         * take_upload, is a board that can be handed a new image over the
         * wire and has no equivalent here.
         */
        emu_run_env_t env = {
            .slice        = quantum,
            .max_insn     = (uint32_t)max_insn,
            .advance_time = advance_guest_time,
            .poll         = (gdb_port != 0) ? host_gdb_poll : NULL,
            .gdb_attached = (gdb_port != 0) ? host_gdb_attached : NULL,
            .gdb_run      = (gdb_port != 0) ? host_gdb_run : NULL,
        };

#if EMU_NET
        /*
         * With the link up the stack needs servicing every slice, and it
         * also owns run control through its own gdb stub -- a different
         * transport from --gdb, which is the loopback-socket one. Both
         * cannot drive the guest, so the link wins when it is up.
         */
        if (emu_net_active()) {
            env.poll         = net_poll_hook;
            env.gdb_attached = emu_net_gdb_attached;
            env.gdb_run      = emu_net_gdb_run;
            env.take_upload  = take_uploaded_image;
        }
#endif

        const emu_run_outcome_t out = emu_run_system(&g_sys, &env, &total);

#if EMU_NET
        /*
         * An uploaded image restarts the run rather than ending it, which
         * is what makes one process serve a whole suite -- the same reason
         * the firmware's banner sits inside its restart loop.
         */
        if (out == EMU_RUN_OUTCOME_RELOAD) {
            goto restart;
        }
#endif
        if (out == EMU_RUN_OUTCOME_CAPPED && !quiet) {
            host_diagf("emu: instruction limit reached\n");
        }
    }

#if EMU_PAIR_STATS
    emu_pair_report(40u);
#endif
    if (dump) {
        for (unsigned i = 0; i < g_sys.ncores; i++) {
            if (g_sys.ncores > 1u) {
                host_diagf("\n--- core %u ---", i);
            }
            g_sys.ops->dump(g_sys.core[i].cpu, err_puts, NULL);
        }
    }
    if (!quiet) {
        host_diagf("emu: %llu instructions retired\n",
                (unsigned long long)total);
        if (g_sys.ncores > 1u) {
            /* Per-core counts, because that is what a determinism check
             * compares between two runs. */
            for (unsigned i = 0; i < g_sys.ncores; i++) {
                emu_cpu_status_t st;
                emu_core_status(&g_sys.core[i], &st);
                host_diagf("emu:   core %u: %llu\n", i,
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
                host_diagf("emu: jit blocks %u  xlat %u  entries %u  interp %u  "
                        "code %u/%u  flushes %u\n"
                        "emu: jit declined %u  overflow %u\n",
                        st.blocks, st.translations, st.block_entries,
                        st.interp_fallbacks, st.code_used, st.code_size,
                        st.flushes, st.declined, st.overflowed);

                /*
                 * What the IR optimiser did, which nothing could observe
                 * until it was reported: a pass that never fires and a
                 * pass that does not pay look identical from the outside,
                 * and this tree's own rule is that identical counters
                 * mean the code never ran.
                 */
                emu_ir_opt_stats_t o;

                emu_ir_opt_totals(&o);
                host_diagf("emu: ir  optimised %u blocks\n"
                           "emu: ir  gets-elided %u  puts-dropped %u  "
                           "flags-dropped %u  dead %u\n"
                           "emu: ir  const->imm %u  addr-fold %u  "
                           "identities %u\n",
                           o.blocks,
                           o.gets_removed, o.puts_removed, o.flags_removed,
                           o.dead_removed, o.folded, o.addr_folded,
                           o.identities);
#ifdef EMU_JIT_DIFF
                host_diagf("emu: jit-diff checked %u  declined %u\n",
                        st.diff_checked, st.diff_declined);
#endif
            }
        }
    }

#if EMU_NET
    /*
     * With a link up, do not exit: serve it.
     *
     * This is the board's park loop, and it is what makes --ppp useful
     * rather than a demonstration. A guest is over in milliseconds; the
     * report is sitting in the telnet ring with nobody connected, and the
     * next image has not been uploaded yet. The board stays up because it
     * has nowhere to go, and here it is a deliberate choice with the same
     * consequence: a harness can push image after image at one process.
     *
     * ^C is the way out, which is why there is no clever exit condition.
     * Anything cleverer would have to guess whether a client that has not
     * connected yet is coming.
     */
    if (emu_net_active()) {
        fprintf(stderr, "emu: guest finished; serving the link (^C to quit)\n");
        for (;;) {
            emu_net_poll();

            if (take_uploaded_image()) {
                goto restart;
            }
            if (emu_net_gdb_attached()) {
                uint32_t n = 0;

                (void)emu_net_gdb_run(quantum, &n);
                continue;
            }
            /*
             * A millisecond. The board spins because it has nothing else
             * to do with the cycles; a process on a shared machine does,
             * and lwIP's finest timeout is coarser than this by orders of
             * magnitude. Sleeping any longer would slow the stack's clock
             * the way __WFI did on the board.
             */
            struct timespec ts = { 0, 1000000L };

            (void)nanosleep(&ts, NULL);
        }
    }
#endif

    return g_exit.exited ? (int)g_exit.code : 0;
}
