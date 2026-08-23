/* SPDX-License-Identifier: Apache-2.0 */
/*
 * runner.c - the STM32 half of the runner, shared by the F446 and F746.
 *
 * Bring the part up, build a guest address space out of its memory and
 * its peripherals, open a core, hand it the console and the cache ops,
 * run it in slices, report. **None of that is about a part**, and what is
 * about a part is in emu_board.h and is short.
 *
 * This was two files of 645 and 1108 lines running the same sequence.
 * They agreed because someone kept them agreeing: the F446 still carried
 * its own copy of the run summary and the JIT statistics long after the
 * shared ones existed, and its guest-RAM handling still memcpy'd a
 * writable half in after the F746 had stopped -- harmless, because the
 * guest overwrote it with the same bytes, and exactly the kind of
 * divergence a shared runner makes impossible rather than unlikely.
 *
 * The division is *not* "what differs between the F446 and the F746",
 * which would bake two boards into an interface meant for three. It is
 * "what only a board can answer", and the two capabilities that vary are
 * asked at run time rather than compiled around:
 *
 *   the network      EMU_NET, a build option rather than a board fact
 *   the flash arena  board_flash_arena_size() == 0 means "no uploads"
 *
 * The second used to be an #if plus three stub callbacks per board. A
 * board with no arena returns a constant zero, the compiler folds every
 * branch on it, and the runner needs no case for which board it is on.
 */

#include "board.h"
#include "emu_board.h"
#include "emu_console.h"
#include "emu_run.h"
#include "emu_session.h"

extern const emu_cache_ops_t board_cache_ops;

#include "emu/emu_cpu.h"
#include "emu/emu_dev.h"
#include "emu/emu_jit.h"
#include "emu/emu_elf.h"
#include "emu/emu_memmap.h"

#if EMU_NET
#  include "emu_net.h"
#  include "emu/emu_gdb.h"
#endif

#include <stdio.h>
#include <string.h>

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



/*
 * Where the guest image currently lives. The one guest_image.S baked into
 * the firmware, until an upload repoints it at the flash arena.
 *
 * A variable rather than the .incbin symbols directly, which is what
 * makes "which image is running" a run-time fact instead of a link-time
 * one. Everything downstream reads emu_board_img, so the two cases are
 * the same case.
 */
static const uint8_t *g_img = emu_guest_image;
static uint32_t       g_img_size;



/* ------------------------------------------------------------------ */
/* Console                                                             */

#define console_printf emu_console_printf

/* Transport hook for the guest's virtual UART. */


/*
 * A board's ISR has masked the line and is handing it over. The core is
 * this file's, which is the whole reason this is not in the board's own
 * file with the handler that calls it.
 */
void emu_raise_irq(uint32_t source, bool level)
{
    emu_core_set_irq(&emu_main_system()->core[0], source, level);
}

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

uint64_t emu_board_time_now(void)
{
    return (uint64_t)(board_cycles() - g_start_cycles) / g_cycles_per_tick;
}

/*
 * The run loop's clock hook. A board reads a *real* cycle counter, not
 * the retired count: its guest drives real peripherals, so a timer
 * interrupt has to bear some relation to the wall clock. The host runner
 * answers the opposite way, and both are right for what they are.
 */
static void advance_guest_time(uint64_t retired_total, uint32_t did)
{
    (void)retired_total;
    (void)did;
    emu_system_t *const sys = emu_main_system();

    sys->ops->set_time(sys->core[0].cpu, emu_board_time_now());
}

#if EMU_NET
static void run_poll(void)
{
    emu_net_poll();
}
#endif

/* ------------------------------------------------------------------ */
/* Starting a guest                                                    */
/* ------------------------------------------------------------------ */


#if EMU_NET
/* ------------------------------------------------------------------ */
/* Images arriving over TFTP                                           */
/* ------------------------------------------------------------------ */

/*
 * The emulator is already suspended whenever these run: they are reached
 * from emu_net_poll(), which the run loop calls between guest slices, so
 * no guest instruction is in flight. Nothing has to be stopped -- but the
 * restart does have to be explicit, because the bus regions and the reset
 * vector were built from the old image.
 *
 * One file, whole, into the arena. It used to be two -- "rom" then "ram"
 * -- because the guest was linked entirely in RAM and the board learned
 * where its read-only part ended from the size of the first transfer. The
 * guest executes in place from flash now and copies its own .data, so
 * there is no boundary and no ordering contract.
 */
static uint32_t g_up_addr;
static bool     g_reload;

bool emu_net_image_begin(void)
{
    if (board_flash_arena_size() == 0u) {
        return false;           /* this board takes no uploads */
    }
    g_up_addr = board_flash_arena_begin();
    return g_up_addr != 0u;
}

bool emu_net_image_data(const void *data, uint32_t len, uint32_t off)
{
    if (g_up_addr == 0u) {
        return false;
    }
    return board_flash_write(g_up_addr + off, data, len);
}

void emu_net_image_end(uint32_t len, bool ok)
{
    if (g_up_addr == 0u) {
        return;
    }
    if (!ok) {
        /*
         * The arena filling up is the *expected* failure, not an
         * exceptional one: TFTP carries no length, so running out is how
         * the end is discovered. Erasing here is what makes the client's
         * retry succeed rather than fail identically for ever.
         */
        (void)board_flash_arena_reset();
        g_up_addr = 0u;
        emu_console_printf("\nemu: upload failed\n");
        return;
    }

    board_flash_arena_commit(len);
    g_img      = (const uint8_t *)g_up_addr;
    g_img_size = len;
    g_up_addr  = 0u;

    /*
     * Flagged, not acted on. The address space has to be rebuilt around
     * the new image and the core reset, and neither can happen from
     * inside a TFTP callback -- which runs from emu_net_poll(), called
     * between guest slices, with the current guest's regions live.
     */
    g_reload = true;
}

/* ------------------------------------------------------------------ */
/* Images arriving through gdb's `load`                                */
/* ------------------------------------------------------------------ */

/*
 * The same arena, driven by vFlashErase / vFlashWrite / vFlashDone.
 *
 * Worth having because it collapses the whole upload dance into one
 * command: `load` puts the image where it actually lives and leaves the
 * debugger attached and in control, which is exactly the position from
 * which a guest bug is worth looking at.
 *
 * gdb addresses these in *guest* space, so the arena offset is applied
 * here; the guest's view is what its ELF says and the arena is an
 * implementation detail of where that lands.
 */
static uint8_t  g_gf_carry[4];
static uint32_t g_gf_carry_len;
static uint32_t g_gf_carry_off;   /* guest offset of g_gf_carry[0] */
static uint32_t g_gf_len;         /* highest byte gdb has written  */

static bool gdb_flash_erase(uint32_t addr, uint32_t len)
{
    (void)len;

    if (board_flash_arena_size() == 0u || addr < EMU_GUEST_ROM_BASE) {
        return false;
    }
    /*
     * gdb erases before writing, and it is the first erase that decides
     * where this image starts. Later ones inside the same load are
     * already covered: the arena is handed out erased.
     */
    if (g_up_addr == 0u) {
        g_up_addr = board_flash_arena_begin();
        g_gf_carry_len = 0u;
    }
    return g_up_addr != 0u;
}

/*
 * gdb does not send word-aligned chunks, and board_flash_write requires
 * them.
 *
 * Its contract is "sequential and word aligned in length except for the
 * last", which the TFTP path satisfies for free -- 512-byte blocks. gdb
 * sends whatever fits its packet, ~975 bytes. Each chunk had its tail
 * padded to a word with 0xFF and the next then began at a non-aligned
 * flash address, so `load` reported success, the image landed corrupted,
 * and the guest ran away without reaching the first breakpoint. The
 * transfer looks perfect from both ends; only the guest disagrees.
 *
 * So carry the 1-3 byte remainder into the next call and hand the flash
 * only whole words. The carry is flushed when a write arrives that is not
 * contiguous with it -- gdb moves between sections, and the gap between
 * .text and .data is exactly that -- and again at vFlashDone.
 */
static bool gf_flush(void)
{
    bool ok = true;

    if (g_gf_carry_len != 0u) {
        /* board_flash_write pads a short tail with 0xFF, which is the
         * erased state, so a final partial word is safe here. */
        ok = board_flash_write(g_up_addr + g_gf_carry_off,
                               g_gf_carry, g_gf_carry_len);
        g_gf_carry_len = 0u;
    }
    return ok;
}

static bool gdb_flash_write(uint32_t addr, const void *data, uint32_t len)
{
    const uint8_t *const src = (const uint8_t *)data;
    const uint32_t off = addr - EMU_GUEST_ROM_BASE;
    uint32_t pos = 0u;

    if (g_up_addr == 0u || addr < EMU_GUEST_ROM_BASE) {
        return false;
    }

    /* A jump to a new section abandons whatever partial word was held for
     * the old one; it belongs at its own address, not this one. */
    if (g_gf_carry_len != 0u && (g_gf_carry_off + g_gf_carry_len) != off) {
        if (!gf_flush()) {
            return false;
        }
    }

    if (g_gf_carry_len != 0u) {
        while (g_gf_carry_len < 4u && pos < len) {
            g_gf_carry[g_gf_carry_len++] = src[pos++];
        }
        if (g_gf_carry_len < 4u) {
            return true;                /* still short of a word */
        }
        if (!board_flash_write(g_up_addr + g_gf_carry_off, g_gf_carry, 4u)) {
            return false;
        }
        g_gf_carry_len = 0u;
    }

    {
        const uint32_t rest  = len - pos;
        const uint32_t whole = rest & ~3u;
        const uint32_t tail  = rest - whole;

        if (whole != 0u &&
            !board_flash_write(g_up_addr + off + pos, &src[pos], whole)) {
            return false;
        }
        if (tail != 0u) {
            for (uint32_t i = 0; i < tail; i++) {
                g_gf_carry[i] = src[pos + whole + i];
            }
            g_gf_carry_len = tail;
            g_gf_carry_off = off + pos + whole;
        }
    }

    /* The highest byte seen is the image's length: gdb writes segments in
     * whatever order it likes and never says how much there is. */
    if (off + len > g_gf_len) {
        g_gf_len = off + len;
    }
    return true;
}

static bool gdb_flash_done(void)
{
    if (g_up_addr == 0u || g_gf_len == 0u) {
        return false;
    }
    if (!gf_flush()) {              /* the last partial word */
        return false;
    }
    board_flash_arena_commit(g_gf_len);
    g_img      = (const uint8_t *)g_up_addr;
    g_img_size = g_gf_len;
    g_gf_len   = 0u;
    g_up_addr  = 0u;
    g_reload   = true;
    return true;
}

static const emu_gdb_flash_ops_t k_gdb_flash = {
    gdb_flash_erase, gdb_flash_write, gdb_flash_done,
};

/*
 * Take a freshly uploaded image, if one is waiting. True when the guest
 * was restarted from it.
 *
 * One function because there are two callers that must not drift: between
 * guest slices, and after a guest has halted. The second is the one that
 * matters for a test harness and was the one missing -- a harness runs a
 * test, waits for it to halt, then pushes the next, by which time the run
 * loop has exited. Both transfers completed, the server said so, and
 * nothing happened.
 */
static bool take_uploaded_image(void)
{
    if (!g_reload) {
        return false;
    }
    g_reload = false;

    /*
     * The whole bring-up, not just the bus: a new image needs the
     * frontend's devices re-added, RAM cleared and the core reset.
     * Skipping that leaves the previous guest's core state in place,
     * which presents as the new guest retiring zero instructions.
     */
    emu_board_img      = g_img;
    emu_board_img_size = g_img_size;

    if (!emu_main_reload()) {
        emu_console_printf("emu: uploaded image does not fit guest RAM\n");
        return false;
    }

    emu_console_printf("\nemu: running uploaded image, %u bytes\n",
                   (unsigned)g_img_size);
    return true;
}
#endif /* EMU_NET */

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
 * Stop, having said why.
 *
 * The obvious implementation -- mask interrupts and spin -- is right up
 * until emu_net_init() succeeds, and after it is the worst thing this
 * file can do. By then the console is a ring buffer drained by telnet, so
 * masking interrupts writes the message explaining the failure into
 * memory nobody will ever read: the board answers no ping, no telnet and
 * no TFTP, and presents as a dead link rather than as a firmware that
 * knows exactly what went wrong and cannot say so.
 *
 * That is not hypothetical. A start-up ordering bug halted here with
 * "could not build the guest address space" sitting in the ring, and the
 * symptom was a silent link -- an hour spent on the network for a fault
 * that had already diagnosed itself.
 *
 * So with the stack up, keep servicing it forever instead. Nothing else
 * runs, which is the point of a halt; a client can still connect and
 * collect the reason.
 */
void emu_board_fatal(int *status)
{
    (void)status;
#if EMU_NET
    if (emu_net_active()) {
        for (;;) {
            emu_net_poll();
        }
    }
#endif
    board_fatal_halt();
}

bool emu_board_startup(int argc, char **argv, int *status,
                       emu_session_cfg_t *cfg, emu_run_env_t *env)
{
    (void)argc;
    (void)argv;
    (void)status;

    board_init();
    emu_board_init();

#ifdef EMU_NATIVE_COREMARK
    /*
     * Native baseline: the same CoreMark sources compiled for this core
     * and run directly, with no emulation, so the interpreter and JIT
     * numbers can be put against something absolute.
     */
    {
        extern int coremark_native_main(void);

        emu_console_printf("\n\nemu: NATIVE CoreMark on %s @ %u MHz\n\n",
                           emu_board_core_name,
                           (unsigned)(board_clock_hz() / 1000000u));
        const uint32_t c0 = board_cycles();
        (void)coremark_native_main();
        emu_console_printf("\n-- native --\n  host     %u cycles\n",
                           (unsigned)(board_cycles() - c0));
        for (;;) {
            board_idle();
        }
    }
#endif

    /*
     * The frontend names itself and builds its ISA string from the
     * extensions actually compiled in, so the banner cannot drift from
     * what the core implements -- which it could when this file spelled
     * the string out itself.
     */
    const emu_cpu_ops_t *const ops = emu_frontend_default();

    emu_console_printf("\n\nemu: %s on %s @ %u MHz\n",
                       ops->desc, emu_board_core_name,
                       (unsigned)(board_clock_hz() / 1000000u));

#if EMU_NET
    /*
     * The handover, and the last two lines the UART ever carries as text:
     * whether the stack started, and what address to connect to. After
     * this, silence on the serial port is expected and silence on the
     * network is the fault.
     */
    emu_console_printf("net    %s on this port; telnet %s 23\n",
                       EMU_NET_LINK_PPP ? "PPP" : "SLIP", emu_net_addr_str());
    if (!emu_net_init()) {
        emu_console_printf("net    failed to start; staying on the serial "
                           "console\n");
    }
#endif

    g_img_size         = emu_guest_image_size;
    emu_board_img      = g_img;
    emu_board_img_size = g_img_size;

    /*
     * The guest's clock: cycles per tick, and the epoch.
     *
     * Set here because emu_board_time_now divides by the first, and the
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
    cfg->unmask_fn = emu_board_irq_unmask;
    /* A board wants speed; a runner chooses, because there it is a
     * coverage question rather than a performance one. */
    cfg->want_jit  = true;
    /* No command line to ask on, and the state after a guest stops is
     * most of what a person reading a telnet session came for. */
    cfg->dump_state = true;

    env->slice        = EMU_RUN_SLICE;
    env->max_insn     = EMU_MAX_INSN;
    env->advance_time = advance_guest_time;
#if EMU_NET
    env->poll         = run_poll;
    env->gdb_attached = emu_net_gdb_attached;
    env->gdb_run      = emu_net_gdb_run;
    env->take_upload  = take_uploaded_image;
#endif
    return true;
}

void emu_board_debug_start(emu_system_t *sys, const emu_cpu_ops_t *ops)
{
#if EMU_NET
    /*
     * Failure is not fatal: a board that cannot serve gdb is still a
     * board that runs guests, and saying so beats halting.
     *
     * The frontend states its own register layout -- gdb's `g` packet is
     * a fixed per-architecture concatenation it never asks about, so
     * serving the wrong one gives an `info registers` that is entirely
     * wrong and entirely plausible.
     */
    const emu_gdb_target_t *const gt =
        ops->gdb_target != NULL ? ops->gdb_target() : NULL;

    if (gt == NULL) {
        emu_console_printf("gdb    frontend has no target description\n");
    } else if (!emu_net_gdb_init(&sys->core[0], gt, &k_gdb_flash)) {
        emu_console_printf("gdb    stub failed to start\n");
    } else {
        emu_console_printf("gdb    target remote %s:1234\n",
                           emu_net_addr_str());
    }
#else
    (void)sys;
    (void)ops;
#endif
}

uint32_t emu_board_host_cycles(void)
{
    return board_cycles();
}


/*
 * A board has nowhere to exit to, so it parks serving its link -- and an
 * image arriving while parked is the *normal* case rather than an edge
 * one: a harness runs a test, waits for it to halt and report, then
 * pushes the next. The reload check inside the run loop never sees those,
 * because that loop exited when the guest halted.
 */
bool emu_board_after_run(const emu_guest_exit_t *exit, bool capped,
                         int *status)
{
    (void)exit;
    (void)capped;
    (void)status;

    for (;;) {
#if EMU_NET
        /*
         * Everything above is still sitting in the output ring: the run
         * loop stopped, and with it the only thing that was delivering.
         * Parking without draining first would lose the entire report,
         * which is the part a harness came for.
         */
        emu_net_poll();

        /*
         * An image may arrive after the guest has finished, and that is
         * the *normal* case rather than an edge one: a harness runs a
         * test, waits for it to halt and report, then pushes the next.
         * The reload check inside the run loop never sees those, because
         * that loop exited when the guest halted -- so an upload
         * completed successfully, said so, and nothing happened.
         */
        if (take_uploaded_image()) {
            return true;
        }

        /*
         * Run control still works after the guest has finished.
         *
         * Without this the park loop services the network and nothing
         * else, so a debugger attaching to a completed run can read
         * registers and memory and then hangs the moment it resumes:
         * `continue` is accepted, nothing executes, and gdb waits for
         * ever. That is the normal way to arrive here -- push an image,
         * watch it fail, attach to find out why -- and being able to
         * rewind the pc and re-run under a breakpoint is most of what
         * that is for.
         */
        if (emu_net_gdb_attached()) {
            uint32_t n = 0;

            (void)emu_net_gdb_run(EMU_RUN_SLICE, &n);
            continue;
        }

        /*
         * Do not sleep while the stack is up, and the reason is the clock
         * rather than latency.
         *
         * lwIP's time base is sys_now(), derived from board_cycles() --
         * a counter of *processor* cycles. Sleeping gates the processor
         * clock, so that counter stops with it and the stack's notion of
         * time stops advancing. Measured: over 29 seconds of wall time
         * parked here, lwIP's clock advanced 1.74 seconds, about 6% of
         * real time.
         *
         * Every timeout in the stack is frozen by that, not just one. The
         * visible symptom was TFTP: a client killed mid-transfer leaves a
         * session open, and the 10-second timeout that would reclaim it
         * needs ~3 minutes of wall time to expire, so in practice the
         * board refused every later upload until reset. A TCP
         * retransmission or an ARP entry ageing out is equally late and
         * would present as a link that is mysteriously sluggish rather
         * than as a stopped clock.
         *
         * Nothing here is power-sensitive: this loop is a bench board
         * waiting to be handed the next test image. The board still
         * sleeps when the network is *not* up, which is the plain
         * serial-console case where nothing depends on lwIP's timers.
         *
         * Fixing it in the time base instead would mean a free-running
         * peripheral timer -- one keeps its clock through sleep where a
         * cycle counter does not -- which is the better answer if this
         * loop ever needs to sleep again.
         */
        if (emu_net_active()) {
            continue;
        }
#endif
        board_idle();
        board_idle();
    }
}
