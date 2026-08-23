/* SPDX-License-Identifier: Apache-2.0 */
/*
 * emu_main.c - the runner. One of them, for every platform.
 *
 * A board and a development machine differ in two things, and this file
 * contains neither: where the guest image comes from, and what happens
 * when the guest stops. Everything between -- build the address space,
 * open the cores, run in slices, report -- is one sequence and is here.
 *
 * The two ends are board_startup() and board_after_run(), and
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
static emu_bus_t    g_buses[EMU_MAX_CORES];
static emu_system_t g_sys;
static emu_uart_t   g_uart;

static emu_guest_exit_t  g_exit;
static emu_session_cfg_t g_cfg;

static emu_syscall_ctx_t g_sc_ctx = {
    .bus = &g_buses[0], .core = &g_sys.core[0], .exit = &g_exit,
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
    g_cfg.image      = emu_board_img;
    g_cfg.image_size = emu_board_img_size;
    g_cfg.ram_base   = EMU_GUEST_RAM_BASE;
    g_cfg.ram_size   = emu_board_ram_size;
    g_cfg.ram_host   = emu_board_ram;
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

emu_system_t *emu_main_system(void) { return &g_sys; }

int main(int argc, char **argv)
{
    int           status = 0;
    emu_run_env_t env = { 0 };

    /*
     * What the runner owns, before the platform is asked: the buses it
     * allocated, the UART it will pump, the syscall handler both
     * platforms share. The platform fills in the rest -- which backend,
     * where the image goes, what to poll -- in board_startup.
     */
    g_cfg.buses       = g_buses;
    g_cfg.ncores      = 0u;             /* the frontend's count */
    g_cfg.uart        = &g_uart;
    g_cfg.uart_tx     = guest_tx;
    g_cfg.uart_rx     = guest_rx;
    g_cfg.syscall_fn  = emu_guest_syscall;
    g_cfg.syscall_ctx = &g_sc_ctx;
    g_cfg.fail        = session_fail;

    if (!board_startup(argc, argv, &status, &g_cfg, &env)) {
        return status;
    }

    cfg_refresh();
    if (g_cfg.ops == NULL) {
        g_cfg.ops = emu_frontend_default();
    }

    for (unsigned i = 0; i < EMU_MAX_CORES; i++) {
        if (!emu_build_address_space(&g_buses[i], &g_uart)) {
            emu_console_printf("fatal: could not build the guest address "
                               "space\n");
            board_fatal(&status);
            return status;
        }
    }

    if (!emu_session_start(&g_sys, &g_cfg)) {
        board_fatal(&status);
        return status;
    }

    emu_board_irqs_init();
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
                           (unsigned)emu_board_img_size, (unsigned)st.pc,
                           (unsigned)(emu_board_ram_size / 1024u),
                           (unsigned)emu_board_ram_size, st.backend);

        const uint32_t t0 = board_perf_cycles();
        uint64_t retired = 0;
        bool     capped = false;

        {
            const emu_run_outcome_t out =
                emu_run_system(&g_sys, &env, &retired);

            if (out == EMU_RUN_OUTCOME_RELOAD) {
                continue;
            }
            capped = (out == EMU_RUN_OUTCOME_CAPPED);
        }

        const uint32_t elapsed = board_perf_cycles() - t0;

        emu_session_report(&g_sys, retired, elapsed, capped,
                           g_cfg.dump_state);

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
        emu_console_printf("\nemu-result exit=%u exited=%u capped=%u "
                           "retired=%u\n",
                           (unsigned)g_exit.code,
                           (unsigned)(g_exit.exited ? 1u : 0u),
                           (unsigned)(capped ? 1u : 0u), (unsigned)retired);

        if (!board_after_run(&g_exit, capped, &status)) {
            return status;
        }
    }
}
