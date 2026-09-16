/* SPDX-License-Identifier: Apache-2.0 */
/*
 * emu_main.c - the runner. One of them, for every platform.
 *
 * A board and a development machine differ in two things, and this file
 * contains neither: where the guest image comes from, and what happens
 * when the guest stops. Everything between -- build the address space,
 * open the cores, run in slices, report -- is one sequence and is here.
 *
 * The two ends are board_init() and emu_board_after_run(), and
 * naming them that way is what makes a host a *board*: it brings its own
 * "hardware" up (malloc'd RAM, a pty, stdout), obtains an image (argv and
 * a file rather than an incbin), and at the end returns an exit status
 * instead of parking. Same shape, different contents.
 *
 * **They had drifted, in every way this arrangement prevents.** The host
 * carried a second copy of the newlib syscall ABI. emu_jit_diff_report
 * existed twice with different formats *and* different limits, so a
 * divergence reported from a board could not be diffed against one from a
 * host. The instruction trace existed only on the host -- the platform
 * where a guest is *easiest* to observe -- so a guest that only
 * misbehaved on hardware had nothing. And the state dump printed core 0
 * alone on the board, so a three-core G4MH guest reported a third of
 * itself.
 */

#include "emu_board.h"
#include "emu_console.h"
#include "emu_debug.h"
#include "emu_args.h"

void host_rate_init(bool quiet, bool force);
#include "emu/emu_virtio.h"
#include "emu_image.h"
#include "emu/emu_elf.h"
#include "emu_run.h"
#include "emu_session.h"

#include "emu/emu_cpu.h"
#include "emu/emu_dev.h"
#include "emu/emu_memmap.h"

#include <stdbool.h>
#include <stdint.h>

/*
 * The machine. EMU_MAX_CORES buses, because the frontend decides the
 * count at build time -- G4MH's is G4MH_PE_COUNT -- and a platform that
 * cannot afford the tables says so by building a frontend that reports
 * one core.
 */
static emu_bus_t g_buses[EMU_MAX_CORES];
static emu_system_t g_sys;
/*
 * The console UART's interrupt.
 *
 * Above the virtio range on purpose: those are handed out from 1 in the
 * order the options ask for, so a fixed number below them would collide
 * as soon as a fourth device was added. 10 is what boot/rv32-emu.dts
 * names, and **nothing checks that the two agree** -- the device tree
 * and the emulator are two descriptions of one machine.
 */
#define EMU_UART_IRQ 10u

static emu_uart_t g_uart;

static emu_guest_exit_t g_exit;
static emu_session_cfg_t g_cfg;

static emu_syscall_ctx_t g_sc_ctx = {
    .bus = &g_buses[0],
    .core = &g_sys.core[0],
    .exit = &g_exit,
};

/*
 * A byte of guest output, and one in.
 *
 * Through the platform's emu_console_putchar/getchar, which is where a
 * board's UART, a host's stdout and a telnet ring all live. The guest's
 * virtual UART and everything the runner prints share one stream, so a
 * guest's output and a panic interleave in the order they happened.
 */
static void guest_tx(void *ctx, uint8_t c)
{
    (void)ctx;
    emu_console_putchar(c);
}

static int guest_rx(void *ctx)
{
    (void)ctx;
    return emu_console_getchar();
}

static void session_fail(const char *msg, const char *detail)
{
    if (detail != NULL) {
        emu_console_printf("fatal: %s: %s\n", msg, detail);
    } else {
        emu_console_printf("fatal: %s\n", msg);
    }
}

/* The image extents move when one is uploaded; everything else is fixed
 * at start-up. */
static void cfg_refresh(void)
{
    g_cfg.image = board_img;
    g_cfg.image_size = board_img_size;
    g_cfg.ram_base = EMU_GUEST_RAM_BASE;
    g_cfg.ram_size = board_ram_size;
    g_cfg.ram_host = board_ram;
}

/*
 * Rebuild the address space around a new image and restart the guest.
 *
 * The whole bring-up, not just the bus: a new image needs the frontend's
 * devices re-added, RAM cleared and the cores reset, and skipping any of
 * it leaves the previous guest's state in place -- which presents as the
 * new guest retiring zero instructions.
 */
bool emu_main_reload(void)
{
    cfg_refresh();

    for (unsigned i = 0; i < g_sys.ncores; i++) {
        if (!emu_build_address_space(&g_buses[i], &g_uart)) {
            return false;
        }
    }
    return emu_session_reload(&g_sys, &g_cfg);
}

/*
 * A board's ISR has masked a real interrupt line and is handing it to the
 * guest.
 *
 * Here because the cores are here. It lived in stm32/board.c carrying a
 * comment that said exactly that -- "the core is this file's" -- while
 * reaching back through emu_main_system() to get at it, which is the
 * reasoning being right about the wrong file. A platform's ISR calls
 * this; nothing about it is per-part.
 */
/*
 * Weak, so a platform with no terminal needs no opinion about it. The
 * host overrides it; a board links this and does nothing.
 */
__attribute__((weak)) void host_rate_init(bool quiet, bool force)
{
    (void)quiet;
    (void)force;
}

void emu_raise_irq(uint32_t source, bool level)
{
    emu_core_set_irq(&g_sys.core[0], source, level);
}

#if EMU_HAVE_VIRTIO
/*
 * virtio's interrupt line, which is the same line any other device
 * raises -- emu_raise_irq is what a platform's ISR already calls, and a
 * virtio device is not special about it.
 *
 * **Level triggered**, which is the part that is easy to get wrong: the
 * device holds the line up until the guest writes InterruptACK, so this
 * must pass `level` through rather than pulsing. A driver that acks and
 * still sees the line asserted takes another interrupt, which is
 * correct; one that never sees it asserted at all hangs waiting for a
 * queue that is already done.
 */
static void virtio_irq(void *ctx, int irq_num, int level)
{
    (void)ctx;
    emu_raise_irq((uint32_t)irq_num, level != 0);
}
#endif /* EMU_HAVE_VIRTIO */

/*
 * The console UART's line, which is the same mechanism and is wanted
 * whether or not this build has virtio -- hence outside the block
 * above.
 */
static void uart_irq(void *ctx, int level)
{
    (void)ctx;
    emu_raise_irq(EMU_UART_IRQ, level != 0);
}

#if EMU_HAVE_VIRTIO

/*
 * Where the devices live.
 *
 * Chosen to match what a device tree for this machine would say, and
 * **nothing checks that they agree** -- they are two descriptions of one
 * machine, and the usual failure is a driver that probes and finds
 * nothing. 0x1000_1000 upwards at 0x1000 apart is the layout the
 * `virt` machines use, so a device tree written for one of those needs
 * the fewest changes.
 */
#define EMU_VIRTIO_BASE 0x10001000u
#define EMU_VIRTIO_STRIDE 0x1000u
#define EMU_VIRTIO_IRQ_BASE 1

static void virtio_attach(emu_bus_t *bus, const emu_args_t *args)
{
    unsigned n = 0;

    if (!emu_virtio_init(bus, virtio_irq, NULL)) {
        return;
    }

    if (args->disk_path != NULL) {
        const uint32_t base = EMU_VIRTIO_BASE + n * EMU_VIRTIO_STRIDE;

        if (emu_virtio_add_block(base, EMU_VIRTIO_IRQ_BASE + (int)n,
                                 args->disk_path, !args->disk_ro)) {
            emu_console_printf("virtio-blk  '%s' (%s) at 0x%08x irq %d\n",
                               args->disk_path,
                               args->disk_ro ? "ro" : "rw", (unsigned)base,
                               EMU_VIRTIO_IRQ_BASE + (int)n);
            n++;
        }
    }

    if (args->net_spec != NULL) {
        const uint32_t base = EMU_VIRTIO_BASE + n * EMU_VIRTIO_STRIDE;

        if (emu_virtio_add_net(base, EMU_VIRTIO_IRQ_BASE + (int)n,
                               args->net_spec)) {
            emu_console_printf("virtio-net  '%s' at 0x%08x irq %d, "
                               "mac 02:00:00:00:00:01\n",
                               args->net_spec, (unsigned)base,
                               EMU_VIRTIO_IRQ_BASE + (int)n);
            n++;
        }
    }

    if (args->virtio_console) {
        const uint32_t base = EMU_VIRTIO_BASE + n * EMU_VIRTIO_STRIDE;

        if (emu_virtio_add_console(base, EMU_VIRTIO_IRQ_BASE + (int)n)) {
            emu_console_printf("virtio-con  at 0x%08x irq %d\n",
                               (unsigned)base, EMU_VIRTIO_IRQ_BASE + (int)n);
            n++;
        }
    }

    if (args->virtio_input) {
        /*
         * Keyboard then mouse, in that order, so their addresses and
         * interrupts are predictable from the command line rather than
         * from which one happened to be created first -- the device
         * tree has to name them and cannot ask.
         */
        const uint32_t kb = EMU_VIRTIO_BASE + n * EMU_VIRTIO_STRIDE;

        if (emu_virtio_add_keyboard(kb, EMU_VIRTIO_IRQ_BASE + (int)n)) {
            emu_console_printf("virtio-kbd  at 0x%08x irq %d\n",
                               (unsigned)kb, EMU_VIRTIO_IRQ_BASE + (int)n);
            n++;
        }

        {
            const uint32_t ms = EMU_VIRTIO_BASE + n * EMU_VIRTIO_STRIDE;

            if (emu_virtio_add_mouse(ms, EMU_VIRTIO_IRQ_BASE + (int)n)) {
                emu_console_printf("virtio-mouse at 0x%08x irq %d\n",
                                   (unsigned)ms,
                                   EMU_VIRTIO_IRQ_BASE + (int)n);
                n++;
            }
        }
    }

    if (args->p9_root != NULL) {
        const uint32_t base = EMU_VIRTIO_BASE + n * EMU_VIRTIO_STRIDE;

        if (emu_virtio_add_9p(base, EMU_VIRTIO_IRQ_BASE + (int)n,
                              args->p9_tag, args->p9_root)) {
            emu_console_printf("virtio-9p  '%s' as '%s' at 0x%08x irq %d\n",
                               args->p9_root, args->p9_tag,
                               (unsigned)base, EMU_VIRTIO_IRQ_BASE + (int)n);
            n++;
        }
    }
}
#endif /* EMU_HAVE_VIRTIO */

/*
 * The native baseline: the same CoreMark sources compiled for the host
 * this emulator runs on, run directly, so the interpreter and JIT figures
 * have something absolute to be read against.
 *
 * Here rather than in a board, because nothing about it is per-part -- it
 * needs a core name, a clock and a cycle counter, and board_api.h
 * promises all three. It sat in stm32/board.c behind `#ifdef`, so the
 * host could build the option and silently never run it.
 *
 * True when it ran, and the caller then returns rather than falling
 * through: this is a measurement, not a mode, and continuing would report
 * the guest's figures under a banner that says "native".
 *
 * **Returning, not spinning.** The first version ended in `for (;;)`,
 * which is what a board wants and made every host run look like a hang --
 * CoreMark printed its whole result and the process then spun until the
 * timeout killed it, so three separate runs were read as "too slow" when
 * they had already finished. A board returning from main lands in the
 * startup's own loop, which is the same outcome by the normal route.
 */
static bool native_coremark_baseline(void)
{
#ifdef EMU_NATIVE_COREMARK
    extern int coremark_native_main(void);

    emu_console_printf("\n\nemu: NATIVE CoreMark on %s @ %u MHz\n\n",
                       board_core_name,
                       (unsigned)(board_clock_hz() / 1000000u));

    const uint32_t c0 = board_cycles();

    (void)coremark_native_main();

    emu_console_printf("\n-- native --\n  host     %u cycles\n",
                       (unsigned)(board_cycles() - c0));
    return true;
#else
    return false;
#endif
}

/*
 * Which frontend runs this image.
 *
 * Common, and it was the host's alone: a board took emu_frontend_default()
 * and nothing else. That stopped being defensible when boards gained a
 * command line -- a board_argv saying `--frontend g4mh` was parsed and
 * then ignored.
 *
 * Three sources, most specific first: what was asked for, what the ELF
 * header says, and the first one compiled in. A flat binary carries no
 * machine type, which is why the last is not a fallback for failure but
 * the answer for an image that cannot say.
 */
static const emu_cpu_ops_t *pick_frontend(const emu_args_t *args)
{
    if (args->frontend != NULL) {
        const emu_cpu_ops_t *const ops = emu_frontend_find(args->frontend);

        if (ops == NULL) {
            emu_console_printf("emu: no frontend '%s'; this build has: ",
                               args->frontend);
            emu_args_list_frontends();
            emu_console_printf("\n");
        }
        return ops;
    }

    if (emu_elf_is_elf(board_img, board_img_size)) {
        const uint16_t m = emu_elf_machine(board_img, board_img_size);
        const emu_cpu_ops_t *const ops = emu_frontend_for_elf(m);

        if (ops == NULL) {
            emu_console_printf(
                "emu: no frontend for ELF machine %u; this build has: ", m);
            emu_args_list_frontends();
            emu_console_printf("\n");
        }
        return ops;
    }

    return emu_frontend_default();
}

int main(int argc, char **argv)
{
    int status = 0;
    emu_run_env_t env = {0};

    /*
     * What the runner owns, before the platform is asked: the buses it
     * allocated, the UART it will pump, the syscall handler both
     * platforms share. The platform fills in the rest -- which backend,
     * where the image goes, what to poll -- in board_init.
     */
    g_cfg.buses = g_buses;
    g_cfg.ncores = 0u; /* the frontend's count */
    g_cfg.uart = &g_uart;
    /*
     * Let the console interrupt.
     *
     * Bare-metal guests poll LSR and never enable it, so this costs
     * them nothing. An operating system needs it: Linux writes kernel
     * messages through the driver's *polled* console path and
     * everything userspace writes through the tty layer, which waits
     * for a transmit interrupt to drain. Without one the kernel's own
     * output is perfect and every byte a process writes is queued for
     * ever -- write() returns success and userspace is simply silent.
     */
    g_cfg.uart_irq = uart_irq;
    g_cfg.uart_tx = guest_tx;
    g_cfg.uart_rx = guest_rx;
    g_cfg.syscall_fn = emu_guest_syscall;
    g_cfg.syscall_ctx = &g_sc_ctx;
    g_cfg.fail = session_fail;

    if (native_coremark_baseline()) {
        return 0;
    }

    /*
     * One parser, two sources of argv.
     *
     * A runner gets the real one; a board hands over the command line it
     * *would* have been given -- see board_argv. Before this the boards
     * set the equivalent struct fields by hand, in parallel with a parser
     * that understood the same settings by name, so a new option reached
     * one and not the other.
     */
    int eargc = argc;
    char *const *eargv = argv;
    int bargc = 0;
    char *const *bargv = board_argv(&bargc);

    if (bargv != NULL) {
        eargc = bargc;
        eargv = bargv;
    }

    emu_args_t args;

    if (!emu_args_parse(eargc, (char **)eargv, &args, &status)) {
        return status;
    }

    /*
     * Here rather than in a board: --trace-skip and --trace-count are
     * options like any other, and the trace they configure is
     * emu_diag.c's, which every platform links. It was called from
     * host/board.c, so a board could not be traced by asking for it.
     */
#if EMU_ENABLE_TRACE
    emu_trace_configure(args.trace_skip, args.trace_count);
#endif

    /*
     * What the command line means, applied once.
     *
     * Both boards assigned these four out of their own copy of the same
     * settings -- `cfg->want_jit = true` beside a `--jit` the parser also
     * understood. A platform can still override afterwards; nothing here
     * is a decision, only the translation from an option's name to the
     * field it sets.
     */
    g_cfg.want_jit = args.want_jit;
    g_cfg.dump_state = args.dump;
    env.slice = args.quantum;
    env.max_insn = args.max_insn;

    /*
     * Acquisition: bring the part up, obtain an image, set board_ram and
     * board_img. Everything after this is the same on every platform,
     * which is why it is here rather than repeated in each board.
     */
    if (!board_init(&args, &g_cfg, &env)) {
        return 1;
    }

    /*
     * Installing the image is here, not in a board: an upload changes it
     * too, and having one place that sets board_img is what keeps "which
     * image is running" a single run-time fact.
     */
    emu_image_set(g_cfg.image, g_cfg.image_size);

    g_cfg.ops = pick_frontend(&args);
    if (g_cfg.ops == NULL) {
        return 2;
    }

    emu_console_printf("\n\nemu: %s on %s @ %u MHz\n", g_cfg.ops->desc,
                       board_core_name,
                       (unsigned)(board_clock_hz() / 1000000u));

    /*
     * The handover, after the banner and before anything else is printed:
     * these are the last two lines the wire carries as text.
     */
    (void)emu_board_link_start();

    g_cfg.load_addr = args.load_addr;
    g_cfg.entry = args.entry;
    env.take_upload = emu_image_take_pending;

    cfg_refresh();
    if (g_cfg.ops == NULL) {
        g_cfg.ops = emu_frontend_default();
    }

    for (unsigned i = 0; i < EMU_MAX_CORES; i++) {
        if (!emu_build_address_space(&g_buses[i], &g_uart)) {
            emu_console_printf("fatal: could not build the guest address "
                               "space\n");
            emu_board_fatal(&status);
            return status;
        }
    }

    /*
     * The rate trace, once the options are known. On the host it draws
     * a self-rewriting line on stderr; everywhere else this is a weak
     * no-op, because a board has no terminal to rewrite.
     */
    /*
     * Where external interrupts are delivered, which a frontend with
     * two privilege levels needs before it boots. Set here rather than
     * in the block above because that runs before the options are
     * parsed. See emu_boot_info_t::supervisor.
     */
    g_cfg.supervisor = args.supervisor;

    host_rate_init(args.quiet, args.rate);

#if EMU_HAVE_VIRTIO
    /*
     * After the address space, because the devices are mapped into it,
     * and on core 0's bus only: virtio is not per-core, and every core
     * sees the same devices through the same addresses.
     */
    virtio_attach(&g_buses[0], &args);
#endif

    if (!emu_session_start(&g_sys, &g_cfg)) {
        emu_board_fatal(&status);
        return status;
    }

    board_irqs_init();
    emu_debug_start(&g_sys, g_cfg.ops);

    /*
     * One run of one guest, and an uploaded image comes back to it. The
     * banner is inside the loop deliberately: it names the image's size
     * and its entry, which are the first things that differ after a
     * reload and the first things a harness wants to see.
     *
     * The entry is read back from the core rather than assumed. A flat
     * binary starts at EMU_GUEST_RESET_PC and an ELF starts wherever
     * e_entry says, and printing the constant either way would be a lie
     * exactly when it matters.
     */
    for (;;) {
        emu_cpu_status_t st;

        emu_core_status(&g_sys.core[0], &st);
        emu_console_printf("guest  %u bytes at 0x%08x\n"
                           "ram    %u KiB (%u bytes)\nbackend %s\n\n",
                           (unsigned)board_img_size, (unsigned)st.pc,
                           (unsigned)(board_ram_size / 1024u),
                           (unsigned)board_ram_size, st.backend);

        const uint32_t t0 = board_perf_cycles();
        uint64_t retired = 0;
        bool capped = false;

        {
            const emu_run_outcome_t out =
                emu_run_system(&g_sys, &env, &retired);

            if (out == EMU_RUN_OUTCOME_RELOAD) {
                continue;
            }
            capped = (out == EMU_RUN_OUTCOME_CAPPED);
        }

        const uint32_t elapsed = board_perf_cycles() - t0;

        emu_session_report(&g_sys, retired, elapsed, capped, g_cfg.dump_state);

        /*
         * The machine-readable terminator, and deliberately the last
         * thing printed for a run.
         *
         * A harness needs two things the report otherwise does not give
         * it: where the output for this guest *ends* -- there is no
         * process to exit on a board, so nothing else says so -- and the
         * guest's exit status, which both suites judge on. `exited`
         * distinguishes a guest that called exit() from one that halted
         * or was capped, because exit=0 means nothing if the syscall was
         * never reached.
         */
        char retired_str[21];

        emu_console_printf("\nemu-result exit=%u exited=%u capped=%u "
                           "retired=%s\n",
                           (unsigned)g_exit.code,
                           (unsigned)(g_exit.exited ? 1u : 0u),
                           (unsigned)(capped ? 1u : 0u),
                           emu_u64_str(retired_str,
                                       (unsigned long long)retired));

        if (!emu_board_after_run(&g_exit, capped, env.slice, &status)) {
            return status;
        }
    }
}
