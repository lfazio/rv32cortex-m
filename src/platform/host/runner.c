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
#include "emu_board.h"
#include "host_args.h"
#include "emu_debug.h"
#include "emu_session.h"

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
#define g_sys  (*emu_main_system())

/* ------------------------------------------------------------------ */
/* Console transport                                                   */

/*
 * A byte of *guest* output, for the shared syscall handler.
 *
 * Deliberately not the firmware's emu_console_putchar, which expands LF to
 * CRLF: that is a property of a terminal on a serial line, not of a
 * console. Doing it here would put a \r into every line of every guest's
 * output on stdout, which two test suites compare and one figure script
 * parses.
 *
 * The same byte as host_tx, which is the guest's virtual UART -- one
 * console, so a guest that writes through the UART and one that writes
 * through the syscall interleave in the order they happened.
 */

/*
 * No real interrupt lines to bridge: a host has no peripherals, so
 * nothing raises one and nothing needs unmasking. The hooks exist because
 * a board's do, and answering them with nothing is how a platform says
 * it has none.
 */
void emu_board_irqs_init(void) { }

void emu_board_irq_unmask(void *ctx, uint32_t source)
{
    (void)ctx;
    (void)source;
}




/* ------------------------------------------------------------------ */
/* System-call services                                                */
/* ------------------------------------------------------------------ */

/*
 * The guest's exit status. `exited` distinguishes a guest that called
 * exit() from one that halted or hit the cap, because a code of 0 means
 * nothing if the syscall was never reached.
 */

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


/* ------------------------------------------------------------------ */
/* Image loading                                                       */
/* ------------------------------------------------------------------ */



/* ------------------------------------------------------------------ */
/* Diagnostics                                                         */
/* ------------------------------------------------------------------ */

/* emu_print_fn onto stderr, for the frontend's own state dump. */

/*
 * The diagnostic sink the shared files print through.
 *
 * Two functions rather than linking emu_console.c, because the host has
 * two sinks where a board has one: guest output goes to stdout (above)
 * and diagnostics go to stderr and the telnet ring (below). A board gives
 * its only wire away and has no such choice to make.
 */



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
        emu_console_printf("emu: upload failed\n");
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

/* Where emu_session reports a failure: the same sink as everything else
 * this runner says about itself. */



/* ------------------------------------------------------------------ */
/* Entry                                                               */
/* ------------------------------------------------------------------ */




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
    emu_system_t *const sys = emu_main_system();

    if (g_timer_div != 0u && sys->ops->advance_time != NULL) {
        sys->ops->advance_time(sys->core[0].cpu, did / g_timer_div);
    }
}


#if EMU_NET
/*
 * A complete image waiting to be run, and the one currently running.
 *
 * The rest of what a reload needs -- the ops, the UART, the RAM extents
 * -- is the runner's now and reached through emu_main_reload(), which is
 * what stopped this file and the board's from each having their own idea
 * of how much of the bring-up an upload repeats.
 */
static uint8_t *g_pending;
static uint32_t g_pending_len;
static uint8_t *g_img_buf;

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
    emu_board_img      = img;
    emu_board_img_size = n;

    /*
     * The whole bring-up, through the shared path: rebuild every bus, put
     * the frontend's devices back, clear RAM, reset. This file had its own
     * copy of that sequence and the board had another, which is exactly
     * how one of them came to skip the devices and leave the new guest
     * retiring nothing.
     */
    if (!emu_main_reload()) {
        free(img);
        return false;
    }

    free(g_img_buf);
    g_img_buf = img;

    emu_console_printf("\nemu: running uploaded image, %u bytes\n",
                       (unsigned)n);
    return true;
}
#endif /* EMU_NET */

/*
 * What this platform adds to a guest's address space beyond the four
 * every platform has -- see emu_board.h.
 *
 * A board's is its passthrough windows onto real peripherals. A host has
 * no peripherals, so this is plain RAM standing in for them: a guest
 * driver written against the STM32 reference manual runs here and reads
 * back what it wrote, which is enough to exercise everything except the
 * hardware itself.
 */

bool emu_board_add_regions(emu_bus_t *bus)
{
    return emu_bus_add_ram(bus, "periph-sim", EMU_GUEST_PERIPH_BASE,
                           g_periph, PERIPH_SIM_SIZE);
}

/*
 * The image and RAM extents the shared builder reads. Mutable here for
 * the same reason as on a board: an image arriving over TFTP replaces
 * them and the address space is rebuilt around the new numbers.
 */
const uint8_t *emu_board_img      = NULL;
uint32_t       emu_board_img_size = 0u;
uint8_t       *emu_board_ram      = NULL;
uint32_t       emu_board_ram_size = 0u;



/* ------------------------------------------------------------------ */
/* The two ends of a run -- see emu_board.h                            */
/* ------------------------------------------------------------------ */

/*
 * A runner's "hardware": the command line, the file it names, and RAM
 * from malloc.
 *
 * That is the whole of what makes this a different platform rather than a
 * different program. A board brings up clocks and a UART and has its
 * image linked in; this brings up a heap and a pty and is told where to
 * find one. Everything after is the same sequence, in emu_main.c.
 */
static host_args_t g_opt;

bool emu_board_startup(int argc, char **argv, int *status,
                       emu_session_cfg_t *cfg, emu_run_env_t *env)
{

#if EMU_NET
#endif
    /* Instructions per timer tick. 1 keeps guest time in step with the
     * cycle counter, which is what the reference model assumes. */

    if (!host_args_parse(argc, argv, &g_opt, status)) {
        return false;
    }

    /* --- read the image first, so its ELF header can pick a frontend - */
    size_t len = 0;
    uint8_t *image = host_read_file(g_opt.path, &len);
    if (image == NULL) {
        *status = 1;
        return false;
    }

    const emu_cpu_ops_t *ops;
    if (g_opt.frontend != NULL) {
        ops = emu_frontend_find(g_opt.frontend);
        if (ops == NULL) {
            emu_console_printf("emu: no frontend '%s'; this build has: ",
                    g_opt.frontend);
            host_list_frontends(stderr);
            fputc('\n', stderr);
            free(image);
            *status = 2;
            return false;
        }
    } else if (emu_elf_is_elf(image, len)) {
        const uint16_t m = emu_elf_machine(image, len);
        ops = emu_frontend_for_elf(m);
        if (ops == NULL) {
            emu_console_printf("emu: no frontend for ELF machine %u; this build has: ", m);
            host_list_frontends(stderr);
            fputc('\n', stderr);
            free(image);
            *status = 2;
            return false;
        }
    } else {
        /* A flat binary says nothing about its architecture. */
        ops = emu_frontend_default();
    }


    /* --- guest memory, which is this platform's "hardware" -------- */
    g_ram    = calloc(g_opt.ram_size, 1u);
    g_periph = calloc(PERIPH_SIM_SIZE, 1u);
    if (g_ram == NULL || g_periph == NULL) {
        emu_console_printf("emu: cannot allocate guest memory\n");
        *status = 1;
        return false;
    }

    emu_board_ram      = g_ram;
    emu_board_ram_size = g_opt.ram_size;
    emu_board_img      = image;
    emu_board_img_size = (uint32_t)len;

    /*
     * What only this platform decides. The runner already filled in the
     * buses, the UART and the syscall handler before calling here -- see
     * emu_board.h on why these are the two structs rather than a hook
     * each.
     */
    cfg->ops       = ops;
    cfg->load_addr = g_opt.load_addr;
    cfg->entry     = g_opt.entry;
    cfg->want_jit  = g_opt.want_jit;
    /* 378 architecture tests do not each want a register dump. */
    cfg->dump_state = g_opt.dump;
    /*
     * NULL: this runner calloc'd guest RAM and an ELF's segments are
     * written into it by the loader. Zeroing it again would erase them.
     */
    cfg->ram_host  = NULL;

    env->slice    = g_opt.quantum;
    env->max_insn = (uint32_t)g_opt.max_insn;
    env->advance_time = advance_guest_time;
#if EMU_NET
    /*
     * The link, before the guest runs and after the cores exist -- the
     * gdb stub needs one to describe.
     *
     * Not fatal if it fails. A runner that cannot get a pty is still a
     * runner, and saying so beats refusing to run the guest; this is the
     * same judgement the firmware makes, for the same reason.
     */
    if (g_opt.ppp) {
        char slave[64] = "";

        if (!board_console_open(g_opt.ppp_dev, slave, sizeof(slave))) {
            emu_console_printf("emu: --ppp: no serial device; continuing "
                            "without a network\n");
        } else if (!emu_net_init()) {
            emu_console_printf("emu: --ppp: the IP stack would not start\n");
        } else {
            fprintf(stderr,
                    "emu: ppp on %s (%s <-> %s)\n"
                    "emu:   scripts/ppp-host.sh %s\n"
                    "emu:   then: telnet %s 23   |   tftp %s\n",
                    slave, EMU_NET_PEER, EMU_NET_ADDR, slave,
                    EMU_NET_ADDR, EMU_NET_ADDR);
        }
    }
#endif


    /* --- run --------------------------------------------------------- */
#if EMU_NET
    if (emu_net_active()) {
        env->take_upload = take_uploaded_image;
    }
#endif
    /*
     * Run control comes from board_gdb_*, which picks the transport --
     * this platform has two and a board has one. Set unconditionally
     * because they answer false when no stub is listening, which is one
     * predictable branch per slice against a NULL check that had to be
     * kept in step with three other places.
     */
#if EMU_ENABLE_TRACE
    emu_trace_configure(g_opt.trace_skip, g_opt.trace_count);
#endif
    return true;
}

/* ------------------------------------------------------------------ */
/* The gdb transport -- see emu_debug.h                                */
/* ------------------------------------------------------------------ */

/*
 * Under --gdb only. A runner's guest is over in milliseconds, so a stub
 * nobody asked for would be a socket nobody connects to; a board's is
 * always worth having because its guest is still going.
 */
bool board_gdb_wanted(void)
{
    /*
     * Either transport counts. --gdb is a loopback socket; --ppp brings
     * up the same stub the board serves, over the link, and a person who
     * asked for a network debugging session asked for the stub with it.
     */
#if EMU_NET
    if (emu_net_active()) {
        return true;
    }
#endif
    return g_opt.gdb_port != 0;
}

bool board_gdb_start(emu_core_t *core, const emu_gdb_target_t *target,
                     const emu_gdb_flash_ops_t **flash)
{
    /*
     * No flash ops either way: this platform's image is a malloc'd buffer
     * the ELF loader writes into directly, so gdb's `load` has nothing to
     * program. A board's writes to its arena.
     */
    *flash = NULL;

#if EMU_NET
    if (emu_net_active()) {
        return emu_net_gdb_init(core, target, NULL);
    }
#endif
    return host_gdb_start(core, target, g_opt.gdb_port);
}

const char *board_gdb_where(void)
{
    static char buf[32];

#if EMU_NET
    if (emu_net_active()) {
        (void)snprintf(buf, sizeof(buf), "%s:1234", EMU_NET_ADDR);
        return buf;
    }
#endif
    (void)snprintf(buf, sizeof(buf), "localhost:%d", g_opt.gdb_port);
    return buf;
}

/*
 * Wait, and this is the platform that must.
 *
 * The whole guest is over in milliseconds, so a debugger that connects
 * "immediately" still arrives after the run finished: without this,
 * --gdb attaches to a guest that has already stopped and every breakpoint
 * is behind it.
 */
void board_gdb_wait(void)
{
#if EMU_NET
    /*
     * Not over the link: there the guest is served for as long as the
     * process runs, so there is no race to lose, and blocking would stop
     * the run before the person has even brought pppd up.
     */
    if (emu_net_active()) {
        return;
    }
#endif
    host_gdb_wait();
}

void board_gdb_poll(void)
{
#if EMU_NET
    if (emu_net_active()) {
        return;                 /* emu_net_poll covers it */
    }
#endif
    host_gdb_poll();
}

bool board_gdb_attached(void)
{
#if EMU_NET
    if (emu_net_active()) {
        return emu_net_gdb_attached();
    }
#endif
    return g_opt.gdb_port != 0 && host_gdb_attached();
}

uint32_t board_gdb_run(uint32_t budget, uint32_t *retired)
{
    return host_gdb_run(budget, retired);
}

/*
 * No cycle counter worth quoting. Returning 0 suppresses the ratio rather
 * than printing one derived from a clock that means something else --
 * which is exactly the mistake the board made when this was wired to
 * guest time.
 */
/*
 * A runner can simply stop: there is a shell to report to, and the
 * message has already gone to stderr. The board's version never returns.
 */
void emu_board_fatal(int *status)
{
    *status = 1;
}

/*
 * This platform's own work between slices: the IP stack, when there is
 * one. The debugger is emu_debug_poll's, which reaches board_gdb_poll
 * below -- two questions, two calls.
 */
void board_poll(void)
{
#if EMU_NET
    if (emu_net_active()) {
        emu_net_poll();
    }
#endif
}

/*
 * No cycle counter worth quoting for the *ratio*.
 *
 * Returning 0 suppresses it rather than deriving one from a clock that
 * means something else -- board_cycles() here is wall time for lwIP, and
 * reporting that as "host cycles per guest instruction" gave 2.01 for a
 * board that spends 429.
 */
uint32_t emu_board_host_cycles(void)
{
    return 0u;
}

/*
 * A runner exits; a board parks. With --ppp it does both: the link is
 * still worth serving after the guest stops, because the report is
 * sitting in a ring with nobody connected and the next image has not
 * arrived yet.
 */
bool emu_board_after_run(const emu_guest_exit_t *exit, bool capped,
                         int *status)
{
    (void)capped;

#if EMU_NET
    /*
     * With a link up, do not exit: serve it.
     *
     * This is the board's park loop, and it is what makes --opt.ppp useful
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
                return true;            /* run the new image */
            }
            if (emu_net_gdb_attached()) {
                uint32_t n = 0;

                (void)emu_net_gdb_run(g_opt.quantum, &n);
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

    *status = exit->exited ? (int)exit->code : 0;
    return false;
}
