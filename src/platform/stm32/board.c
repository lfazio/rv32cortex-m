/* SPDX-License-Identifier: Apache-2.0 */
/*
 * board.c - the STM32s' half of the board contract, shared by both parts.
 *
 * board_api.h says what a platform provides; this provides the parts that
 * are the same on the F446 and the F746, which is everything reached
 * through the network: the IP stack is polled the same way and the gdb
 * stub is served over it the same way. What differs between the two
 * parts -- the UART, the clocks, the caches, the flash -- is in each
 * part's own board.c beside it.
 *
 * **Here rather than in runner.c**, which is where these were. A runner
 * implementing board_* is the two namespaces mixed again: the runner is
 * the thing that *calls* the contract, and a file that both calls it and
 * fulfils it gives a reader no way to tell which half they are reading.
 */

#include "board.h"
#include "board_api.h"
#include "emu_console.h"

#include <stdio.h>
#include "emu_board.h"
#include "emu_run.h"
#include "emu_debug.h"
#include "emu_image.h"
#include "emu_args.h"
#include "emu_session.h"
#include "emu/emu_cpu.h"
#include "emu/emu_dev.h"
#include "emu/emu_jit.h"
#include "emu/emu_elf.h"
#include "emu/emu_memmap.h"
#include <string.h>

/*
 * **A board has no gdb transport of its own**, and that is not the same
 * as having no debugger.
 *
 * It serves gdb over the IP stack, which is emu_debug.c's now: the same
 * emu_net_gdb_init on every platform that has a link, behind a build
 * option rather than a fact about the part. All seven of these used to be
 * that transport spelled out here behind `#if EMU_NET`, in the file whose
 * whole job is what changes when the silicon does.
 *
 * So they answer with nothing, the way board_api.h says a platform
 * declines anything else. A board that grew a second debug port -- a
 * real one on a spare UART, say -- would fill them in, and the network
 * would still take precedence while the link is up.
 */
bool board_gdb_wanted(void) { return false; }

bool board_gdb_start(emu_core_t *core, const emu_gdb_target_t *target,
                     const emu_gdb_flash_ops_t **flash)
{
    (void)core; (void)target; (void)flash;
    return false;
}

const char *board_gdb_where(void)   { return "(no local transport)"; }
void        board_gdb_wait(void)    { }
void        board_gdb_poll(void)    { }
bool        board_gdb_attached(void) { return false; }

uint32_t board_gdb_run(uint32_t budget, uint32_t *retired)
{
    (void)budget; (void)retired;
    return 0u;
}

/*
 * Nothing of the board's own between slices. The IP stack is polled by
 * emu_board_poll, which every platform shares.
 */
void board_poll(void) { }

/* ------------------------------------------------------------------ */
/* The image store, the clocks, and the two ends of a run             */
/* ------------------------------------------------------------------ */
/* The guest binary, embedded by guest_image.S. */
extern const uint8_t  emu_guest_image[];
extern const uint32_t emu_guest_image_size;

/* ------------------------------------------------------------------ */
/* Configuration                                                       */
/* ------------------------------------------------------------------ */

/* Guest time runs at 1 MHz, derived from the part's cycle counter. */
#define EMU_TIMER_HZ        1000000u

/*
 * A cap on how long a guest may run before the firmware gives up.
 *
 * The host runner has --max-insn and two of the Berkeley tests depend on
 * it to terminate at all -- they are *meant* to run away, and the cap is
 * what turns that into a reported failure. Without one the same guest
 * hangs the board: no output, no prompt, and the only way out is a reset,
 * which is indistinguishable from a firmware crash. That is the
 * difference between a suite that reports 273 passed and 1 failed and one
 * that stops after test 47 and needs a human.
 *
 * Zero disables it, which is what an interactive session or a benchmark
 * wants. The default is generous: CoreMark retires about 1.3 million
 * instructions a run and the architecture tests far fewer, so anything
 * reaching this is not making progress.
 */
#ifndef EMU_MAX_INSN
#  define EMU_MAX_INSN      100000000u
#endif

#ifndef EMU_RUN_SLICE
#  define EMU_RUN_SLICE     4096u
#endif

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

/*
 * One core, and the array is sized for it.
 *
 * The runner drives an emu_system_t because the host schedules several
 * and the loop is shared, but a board is a system of one on purpose: a
 * second core needs a second bus and, for G4MH, 64 KiB of its own local
 * RAM, and this part has 320 KiB in total. emu_system_open is therefore
 * told 1 rather than asked -- passing 0 would take the frontend's answer,
 * which for -DG4MH_PE_COUNT=3 is three cores that do not fit.
 */
#define EMU_BOARD_CORES 1u

/* ------------------------------------------------------------------ */
/* Guest time                                                          */
/* ------------------------------------------------------------------ */

/*
 * The divisor is fixed at start-up because the core clock is, and the
 * epoch is the first slice rather than reset so a guest's clock starts
 * near zero.
 */
static uint32_t g_cycles_per_tick;
static uint32_t g_start_cycles;

uint64_t board_time_now(void)
{
    return (uint64_t)(board_cycles() - g_start_cycles) / g_cycles_per_tick;
}

/*
 * The run loop's clock hook. A board reads a *real* cycle counter, not
 * the retired count: its guest drives real peripherals, so a timer
 * interrupt has to bear some relation to the wall clock. The host runner
 * answers the opposite way, and both are right for what they are.
 */
static void advance_guest_time(emu_system_t *sys, uint64_t retired_total,
                               uint32_t did)
{
    (void)retired_total;
    (void)did;

    sys->ops->set_time(sys->core[0].cpu, board_time_now());
}

/* ------------------------------------------------------------------ */
/* Starting a guest                                                    */
/* ------------------------------------------------------------------ */



/* ------------------------------------------------------------------ */
/* Entry                                                               */
/* ------------------------------------------------------------------ */


/* ------------------------------------------------------------------ */
/* The two ends of a run -- see emu_board.h                            */
/* ------------------------------------------------------------------ */

/*
 * Bring the part up, hand the console to the network, and say what is
 * about to run.
 *
 * argc/argv are a hosted platform's and this one has neither; a board is
 * defined by what is soldered to it, which is the whole difference this
 * hook exists to hold.
 */

/*
 * This board's command line, which it would have been given if it had
 * one.
 *
 * Every entry here used to be a struct field assigned by hand below, in
 * parallel with a parser that understood the same setting by name --
 * `--jit` and `cfg->want_jit`, `--quantum` and `env->slice`. Writing it
 * as argv is what collapses the two, and it has a second effect worth
 * more than the deduplication: this board's policy is now readable as the
 * command line it corresponds to, so "what does the firmware run with"
 * and "what would I type" have one answer.
 *
 * Static, because emu_main keeps the pointer. argv[0] is skipped as a
 * program name and so must be there.
 *
 * No image is named: this board's is linked in with .incbin, which is why
 * the shared parser cannot require one.
 */
char *const *board_argv(int *argc)
{
    /*
     * **Formatted, not stringified**, and that is not a style choice.
     *
     * The obvious `#x` produces the *token*, and these constants carry a
     * `u` suffix -- so `--quantum 4096u` reached a parser whose parse_u32
     * insists on `*end == '\0'` and rejected it. The board would have
     * printed the usage text over its console and never started a guest.
     * Confirmed on the host, where the same argument gives usage and exit
     * 2, because a board cannot be asked without flashing it.
     *
     * snprintf is also immune to the form a -D takes: -DEMU_RUN_SLICE=4096
     * and =4096u come out the same here, where stringifying would put the
     * caller's typing straight into an argv.
     */
    static char slice[16];
    static char maxi[24];

    (void)snprintf(slice, sizeof(slice), "%u", (unsigned)EMU_RUN_SLICE);
    (void)snprintf(maxi,  sizeof(maxi),  "%u", (unsigned)EMU_MAX_INSN);

    static char *av[] = {
        "emu",
        "--jit",                        /* a board wants speed; a runner
                                         * chooses, because there it is a
                                         * coverage question */
        "--dump",                       /* the register state on exit is
                                         * most of what a person reading a
                                         * telnet session came for */
        "--quantum",  NULL,
        "--max-insn", NULL,
    };

    av[4] = slice;
    av[6] = maxi;

    *argc = (int)(sizeof(av) / sizeof(av[0]));
    return av;
}

bool board_startup(const emu_args_t *args, int *status,
                       emu_session_cfg_t *cfg, emu_run_env_t *env)
{
    (void)status;

    board_init();
    board_ram_init();


    /*
     * The frontend names itself and builds its ISA string from the
     * extensions actually compiled in, so the banner cannot drift from
     * what the core implements -- which it could when this file spelled
     * the string out itself.
     */
    const emu_cpu_ops_t *const ops = emu_frontend_default();

    emu_console_printf("\n\nemu: %s on %s @ %u MHz\n",
                       ops->desc, board_core_name,
                       (unsigned)(board_clock_hz() / 1000000u));

    /*
     * The handover: after this the UART is the link and the console is
     * telnet. What it prints and whether it succeeds is the same on every
     * platform, so it is emu_debug.c's -- see emu_board_link_start.
     */
    (void)emu_board_link_start();

    emu_image_set(emu_guest_image, emu_guest_image_size);

    /*
     * The guest's clock: cycles per tick, and the epoch.
     *
     * Set here because board_time_now divides by the first, and the
     * old main() assigned both just before the run. Splitting that main
     * into a runner and this file left the assignment behind, and the
     * board stopped *dead* at the first call -- the banner printed and
     * nothing else, because a divide by zero on this part is a
     * UsageFault and there is no handler to say so.
     *
     * The epoch is the first slice rather than reset, so a guest's clock
     * starts near zero.
     */
    g_cycles_per_tick = board_clock_hz() / EMU_TIMER_HZ;
    g_start_cycles    = board_cycles();

    cfg->ops       = ops;
    cfg->cache_ops = &board_cache_ops;
    cfg->unmask_fn = board_irq_unmask;
    /* A board wants speed; a runner chooses, because there it is a
     * coverage question rather than a performance one. */
    /* No command line to ask on, and the state after a guest stops is
     * most of what a person reading a telnet session came for. */

    env->advance_time = advance_guest_time;
    env->take_upload  = emu_image_take_pending;
    return true;
}

/* ------------------------------------------------------------------ */
/* The gdb transport -- see emu_debug.h                                */
/* ------------------------------------------------------------------ */









/* Host cycles for the performance figure, which on this part is the DWT
 * counter the guest's clock is also derived from -- see
 * board_time_now for the division that separates them. */
uint32_t board_perf_cycles(void)
{
    return board_cycles();
}

/*
 * A board has nowhere to return to, so it always parks.
 *
 * The loop itself is emu_board_after_run's, in emu_debug.c: drain the
 * link, take an image if one arrived, let a debugger drive, wait. It was
 * written out here and in the host's board.c -- the same four steps in
 * the same order, differing only in this answer and in what "wait" means.
 */
bool board_parks_after_run(void) { return true; }

/*
 * **Never sleep while the stack is up**, and the reason is the clock
 * rather than latency.
 *
 * lwIP's time base is sys_now(), derived from board_cycles() -- a counter
 * of *processor* cycles. board_wfi() gates the processor clock, so that
 * counter stops with it and the stack's notion of time stops advancing.
 * Measured: over 29 seconds of wall time parked, lwIP's clock advanced
 * 1.74 seconds, about 6% of real time.
 *
 * Every timeout in the stack is frozen by that, not just one. The visible
 * symptom was TFTP: a client killed mid-transfer leaves a session open,
 * and the 10-second timeout that would reclaim it needs ~3 minutes of
 * wall time to expire, so the board refused every later upload until
 * reset. A TCP retransmission or an ARP entry ageing out is equally late
 * and would present as a link that is mysteriously sluggish rather than
 * as a stopped clock.
 *
 * Nothing here is power-sensitive: this is a bench board waiting to be
 * handed the next test image. It still sleeps when the network is *not*
 * up, which is the plain serial-console case where nothing depends on
 * lwIP's timers. A free-running peripheral timer would be the better
 * answer if this ever needs to sleep again -- one keeps its clock through
 * sleep where a cycle counter does not.
 */
void board_idle(void)
{
    if (emu_board_link_up()) {
        return;
    }
    board_wfi();
}
