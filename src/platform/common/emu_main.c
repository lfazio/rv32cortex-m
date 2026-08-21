/* SPDX-License-Identifier: Apache-2.0 */
/*
 * emu_main.c - the firmware runner, shared by every board.
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

#include "emu/emu_cpu.h"
#include "emu/emu_dev.h"
#include "emu/emu_jit.h"
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

static emu_bus_t  g_bus;
static emu_core_t g_core;
static emu_uart_t g_uart;
static emu_guest_exit_t g_exit;

static emu_syscall_ctx_t g_sc_ctx = {
    .bus = &g_bus, .core = &g_core, .exit = &g_exit,
};

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
/* ------------------------------------------------------------------ */

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
static void fatal_halt(void)
{
#if EMU_NET
    if (emu_net_active()) {
        for (;;) {
            emu_net_poll();
        }
    }
#endif
    board_fatal_halt();
}

/*
 * One console, two possible sinks. Before emu_net_init() succeeds it is
 * the UART; after, the UART carries SLIP or PPP and cannot carry text as
 * well, so everything goes to the telnet buffer instead.
 *
 * The branch is a load and a test per character, which is nothing: the
 * console is written by human-readable output and by the guest's virtual
 * UART, neither of which is on any measured hot path. The same is not
 * true of the run loop, which is why emu_net_poll() below is the thing
 * that had to be thought about.
 */
void emu_console_putc(uint8_t c)
{
#if EMU_NET
    if (emu_net_active()) {
        emu_net_console_putc(c);
        return;
    }
#endif
    board_console_putc(c);
}

static int console_getc(void)
{
#if EMU_NET
    if (emu_net_active()) {
        return emu_net_console_getc();
    }
#endif
    return board_console_getc();
}

#define console_printf emu_console_printf

/* Transport hook for the guest's virtual UART. */
static int guest_uart_rx(void *ctx)
{
    (void)ctx;
    return console_getc();
}

#ifdef EMU_JIT_DIFF
/*
 * A block whose compiled code disagreed with the IR interpreter.
 *
 * `off` is a byte offset into the guest state, so the register file
 * starts at zero and the number is the register times four. Only the
 * first few are printed: everything after the first divergence is
 * downstream of the same bug, and a UART at 921600 is not a debugger.
 */
void emu_jit_diff_report(uint32_t pc, uint32_t off, uint32_t want,
                         uint32_t got);
void emu_jit_diff_report(uint32_t pc, uint32_t off, uint32_t want,
                         uint32_t got)
{
    static unsigned reported;

    if (reported++ >= 12u) {
        return;
    }
    console_printf("jit-diff pc 0x%08x +%u want 0x%08x got 0x%08x\n",
                   (unsigned)pc, (unsigned)off, (unsigned)want,
                   (unsigned)got);
}
#endif

/*
 * A board's ISR has masked the line and is handing it over. The core is
 * this file's, which is the whole reason this is not in the board's own
 * file with the handler that calls it.
 */
void emu_raise_irq(uint32_t source, bool level)
{
    emu_core_set_irq(&g_core, source, level);
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

/* ------------------------------------------------------------------ */
/* Starting a guest                                                    */
/* ------------------------------------------------------------------ */

static bool start_guest(void)
{
    emu_board_img      = g_img;
    emu_board_img_size = g_img_size;
    return emu_start_guest(&g_core, &g_bus, &g_uart, &g_exit);
}

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
        console_printf("\nemu: upload failed\n");
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
    if (!start_guest()) {
        console_printf("emu: uploaded image does not fit guest RAM\n");
        return false;
    }

    console_printf("\nemu: running uploaded image, %u bytes\n",
                   (unsigned)g_img_size);
    return true;
}
#endif /* EMU_NET */

/* ------------------------------------------------------------------ */
/* Entry                                                               */
/* ------------------------------------------------------------------ */

int main(void)
{
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

        console_printf("\n\nrv32cortex-m: NATIVE CoreMark on %s @ %u MHz\n\n",
                       emu_board_core_name,
                       (unsigned)(board_clock_hz() / 1000000u));
        const uint32_t c0 = board_cycles();
        (void)coremark_native_main();
        console_printf("\n-- native --\n  host     %u cycles\n",
                       (unsigned)(board_cycles() - c0));
        for (;;) {
            board_idle();
        }
    }
#endif

    /*
     * The frontend names itself, and builds its ISA string from the
     * extensions actually compiled in, so the banner cannot drift from
     * what the core implements -- which it could when this file spelled
     * the string out itself.
     */
    const emu_cpu_ops_t *const ops = emu_frontend_default();

    console_printf("\n\nrv32cortex-m: %s on %s @ %u MHz\n",
                   ops->desc, emu_board_core_name,
                   (unsigned)(board_clock_hz() / 1000000u));

#if EMU_NET
    /*
     * The handover, before the rest of the banner, so that everything
     * describing what is about to run reaches a telnet client rather than
     * a serial port nobody is watching. It is buffered until one
     * connects, which is what net_telnet.c's output ring is for.
     *
     * These two lines are the last the UART ever carries as text, and are
     * deliberately the two that matter when nothing works: whether the
     * stack started at all, and what address to connect to. After this,
     * silence on the serial port is expected and silence on the network
     * is the fault.
     */
    console_printf("net    %s on this port; telnet %s 23\n",
                   EMU_NET_LINK_PPP ? "PPP" : "SLIP", emu_net_addr_str());
    if (!emu_net_init()) {
        console_printf("net    failed to start; staying on the serial "
                       "console\n");
    }
#endif

    /*
     * Which image is running, before anything asks. The address space is
     * built from these rather than from the .incbin symbols -- that is
     * what lets an upload replace the image at run time -- so leaving
     * them until after the bus is built publishes a zero-length region,
     * which emu_bus rejects, and the firmware halts before it has run an
     * instruction.
     */
    g_img_size         = emu_guest_image_size;
    emu_board_img      = g_img;
    emu_board_img_size = g_img_size;

    /*
     * The bus only, at this point: emu_core_open() needs one to open
     * onto, and the devices, reset and boot come later through
     * start_guest(), which needs the core to exist.
     */
    if (!emu_build_address_space(&g_bus, &g_uart)) {
        console_printf("fatal: could not build the guest address space\n");
        fatal_halt();
    }

    if (!emu_core_open(&g_core, ops, &g_bus, 0u)) {
        console_printf("fatal: frontend has no core 0\n");
        fatal_halt();
    }

    ops->set_unmask_hook(g_core.cpu, emu_board_irq_unmask, NULL);
    emu_board_irqs_init();
    emu_uart_init(&g_uart, emu_console_uart_tx, guest_uart_rx, NULL);
    ops->set_syscall(g_core.cpu, emu_guest_syscall, &g_sc_ctx);
    ops->set_cache(g_core.cpu, &emu_arm_cache_ops);

#if EMU_NET
    /*
     * The gdb stub, once there is a core for it to describe. Started
     * after the guest exists and before it runs, so a debugger that
     * connects immediately finds the guest at its reset vector rather
     * than somewhere arbitrary.
     *
     * Failure is not fatal: a board that cannot serve gdb is still a
     * board that runs guests, and saying so beats halting.
     */
    {
        /*
         * The frontend states its own register layout. gdb's `g` packet
         * is a fixed per-architecture concatenation it never asks about,
         * so serving the wrong one gives an `info registers` that is
         * entirely wrong and entirely plausible.
         */
        const emu_gdb_target_t *const gt =
            ops->gdb_target != NULL ? ops->gdb_target() : NULL;

        if (gt == NULL) {
            console_printf("gdb    frontend has no target description\n");
        } else if (!emu_net_gdb_init(&g_core, gt, &k_gdb_flash)) {
            console_printf("gdb    stub failed to start\n");
        } else {
            console_printf("gdb    target remote %s:1234\n",
                           emu_net_addr_str());
        }
    }
#endif

    if (!start_guest()) {
        console_printf("fatal: could not start the guest\n");
        fatal_halt();
    }

    /*
     * Everything from here down is one run of one guest, and an uploaded
     * image comes back to it. The banner is inside the loop deliberately:
     * it names the image's size, which is the first thing that differs
     * after a reload and the first thing a harness wants to see.
     */
restart:
    console_printf("guest  %u bytes at 0x%08x\n"
                   "ram    %u KiB (%u bytes)\n",
                   (unsigned)g_img_size, (unsigned)EMU_GUEST_RESET_PC,
                   (unsigned)(emu_board_ram_size / 1024u),
                   (unsigned)emu_board_ram_size);

    {
        emu_cpu_status_t st;

        emu_core_status(&g_core, &st);
        console_printf("backend %s\n\n", st.backend);
    }

    g_cycles_per_tick = board_clock_hz() / EMU_TIMER_HZ;
    const uint32_t start_cycles = board_cycles();

    g_start_cycles = start_cycles;

    uint64_t retired_total = 0;
    bool     capped = false;

    {
        const emu_run_env_t env = {
            .slice       = EMU_RUN_SLICE,
            .max_insn    = EMU_MAX_INSN,
#if EMU_NET
            .take_upload = take_uploaded_image,
#else
            .take_upload = NULL,
#endif
        };
        const emu_run_outcome_t out =
            emu_run_guest(&g_core, ops, &env, &retired_total);

        if (out == EMU_RUN_OUTCOME_RELOAD) {
            goto restart;
        }
        capped = (out == EMU_RUN_OUTCOME_CAPPED);
    }

    const uint32_t elapsed = board_cycles() - start_cycles;

    if (capped) {
        /*
         * Named on its own line and before the statistics, so a harness
         * reading the console can tell "did not terminate" from "ran and
         * failed" without parsing the numbers.
         */
        console_printf("\nemu: instruction cap reached, guest did not "
                       "halt\n");
    }

    emu_print_run_summary(retired_total, elapsed);
    if (retired_total != 0u && elapsed != 0u) {
        /* KIPS needs the core clock, which is the board's to know. */
        const uint32_t kips = (uint32_t)((uint64_t)retired_total *
                                         (board_clock_hz() / 1000u) / elapsed);

        console_printf("  speed    %u KIPS\n", (unsigned)kips);
    }

    /*
     * No #if. emu_print_jit_stats answers "is there a JIT here" from the
     * framework's own code_size, so the caller needs no capability macro
     * -- and the one that was here read EMU_HAVE_JIT without including
     * what defines it, which #if treats as 0 without a word. The whole
     * block silently stopped printing the moment an unrelated include was
     * removed.
     */
    (void)emu_print_jit_stats();

    emu_report_state(g_core.cpu, g_core.ops);

#if EMU_NET
    /*
     * Bytes the wire delivered and nothing collected, reported next to
     * the guest's own numbers because it is the one failure that makes
     * *those* numbers untrustworthy without looking wrong: a dropped byte
     * is a dropped frame, which is a retransmission at best and a
     * truncated image at worst.
     *
     * `tftp reclaims` is how often the server had to be rebuilt under it.
     * Nonzero is normal -- the watchdog re-arms while the link is quiet
     * -- but it climbing during a suite run means transfers are being
     * abandoned, which is worth seeing rather than inferring from a slow
     * harness.
     */
    console_printf("\n-- net --\n  rx drops %u  tftp reclaims %u\n",
                   (unsigned)board_console_rx_overruns(),
                   (unsigned)emu_net_tftp_reclaims());
#endif

    /*
     * The machine-readable terminator, and deliberately the last thing
     * printed for a run.
     *
     * A harness needs two things this report otherwise does not give it:
     * where the output for this guest *ends* -- there is no process to
     * exit, so nothing else says so -- and the guest's exit status, which
     * both test suites judge on and which was being captured and then
     * thrown away. run-riscv-tests.sh reads the runner's exit code as
     * (testnum << 1) | 1, so losing it means every result is "it ran".
     *
     * `exited` distinguishes a guest that called exit() from one that
     * halted or was capped, because exit=0 means nothing if the guest
     * never reached the syscall.
     */
    console_printf("\nemu-result exit=%u exited=%u capped=%u retired=%u\n",
                   (unsigned)g_exit.code, (unsigned)(g_exit.exited ? 1u : 0u),
                   (unsigned)(capped ? 1u : 0u), (unsigned)retired_total);

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
            goto restart;
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
    }
}
