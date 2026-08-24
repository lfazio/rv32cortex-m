/* SPDX-License-Identifier: Apache-2.0 */
/*
 * board_api.h - the `board_*` contract, stated once.
 *
 * Two namespaces live in src/platform/ and they are layers rather than a
 * mixture:
 *
 *   board_*       what a platform provides. *This* file -- and it is
 *                 mostly common, which is the point: every platform has a
 *                 console, a clock and a name, so saying so three times
 *                 was three chances to say it differently.
 *   emu_*         what the common layer provides *to* a platform, in
 *                 emu_board.h: emu_raise_irq, emu_build_address_space,
 *                 emu_start_guest.
 *
 * The direction is one way, and the prefix is how you can tell which way
 * you are looking.
 *
 * **It did not used to be.** `emu_board_*` named both halves: the runner
 * called emu_board_add_regions and emu_board_irqs_init, but every
 * platform *defined* them, so a name that was supposed to mean "the
 * runner's side" appeared as a definition in board.c. That is why a
 * reader of host/board.c found `emu_` all through a file that is
 * supposed to be the bottom of the stack. They are board_regions,
 * board_irqs_init, board_irq_unmask, board_core_name, board_img and
 * board_ram now, declared here with the rest of what a platform owes.
 *
 * What normalising still means: board_perf_cycles() is board_cycles() on
 * a part with a cycle counter and 0 on one without, and the runner never
 * learns which it got.
 *
 * A platform includes this from its own board.h and adds whatever else it
 * has -- the STM32s' flash arena, the host's pty. Those are genuinely
 * per-part and stay per-part.
 *
 * **How a platform declines something.** Not by leaving a function out,
 * which is a link error naming a symbol rather than a capability. Either
 * it answers with nothing -- board_poll() empty, board_gdb_poll() empty
 * -- or it answers zero where a size is asked for, which the caller tests
 * at run time and the compiler folds away. board_flash_arena_size()
 * returning 0 is how a board says it takes no uploads, and it costs a
 * board that has one nothing.
 */
#ifndef EMU_PLATFORM_BOARD_API_H
#define EMU_PLATFORM_BOARD_API_H

/*
 * The gdb transport below names a core and a target description, so this
 * header depends on the emulator's types -- which is the right direction:
 * a platform serves *this* emulator's debugger, not a debugger in
 * general.
 */
#include "emu/emu_bus.h"
#include "emu/emu_cpu.h"
#include "emu/emu_gdb.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* board_ -- identity, image and RAM the platform supplies             */
/* ------------------------------------------------------------------ */

/*
 * The core this firmware runs on, for the banner: "Cortex-M4",
 * "Cortex-M7", "Cortex-M55". A string rather than a macro because the
 * runner prints it and nothing branches on it -- the moment something
 * does, that belongs in one of the hooks below instead.
 */
extern const char *const board_core_name;

/* ------------------------------------------------------------------ */
/* The guest image                                                     */
/* ------------------------------------------------------------------ */

/*
 * The image in force, as variables rather than constants.
 *
 * A board that can take an upload moves these when one arrives; a board
 * that cannot points them at the linked-in image once and never touches
 * them again. The runner reads them and does not care which -- which is
 * what lets the address space be rebuilt identically in both cases.
 *
 * There is no read-only *boundary* any more. The guest links .text and
 * .rodata into flash and .data into RAM, so the platform serves one
 * region as each and never has to know where one ends -- which is what
 * removed the two-piece upload.
 */
extern const uint8_t *board_img;
extern uint32_t board_img_size;

/*
 * Where the guest's RAM is and how much of it there is.
 *
 * Variables, not constants, and not only for symmetry with the image
 * extents: on a board that carves guest RAM out of whatever the link
 * left over, the size is a *difference of two linker symbols*, which C
 * will not accept in a static initialiser however constant it is at run
 * time. The board assigns both before building the address space.
 */
extern uint8_t *board_ram;
extern uint32_t board_ram_size;

/* ------------------------------------------------------------------ */
/* board_ -- the bus and the interrupt bridge                          */
/* ------------------------------------------------------------------ */

/*
 * The windows this platform offers the guest, beyond RAM and the image.
 *
 * A *table*, not a function that adds them, and that is the whole change:
 * the three STM32 platforms had byte-identical loops over their own
 * tables, so what varied was never the adding. emu_build_address_space
 * walks this.
 *
 * `host` is what distinguishes the two kinds. NULL means passthrough --
 * the region is identity-mapped onto the real bus at `base`, which is how
 * a guest driver reaches actual hardware using the addresses its
 * datasheet prints, and is the entire point of this emulator. Non-NULL
 * means memory the emulator owns, which is what a host uses to *simulate*
 * a peripheral window it does not have.
 *
 * `perm` is EMU_PERM_*; a read-only span is how a board withholds
 * something a guest could take the emulator down with.
 *
 * NULL, or a count of zero, is a platform with nothing to add.
 */
typedef struct board_region {
    const char *name;
    uint32_t base;
    uint32_t size;
    uint8_t perm;
    void *host;
} board_region_t;

const board_region_t *board_regions(unsigned *count);

/*
 * Route a real interrupt line to the guest.
 *
 * `unmask` is handed to the frontend, which calls it when the guest
 * enables a source; `init` enables at the NVIC whatever lines this board
 * bridges. Both are per-board because the set of bridged lines is, and
 * because IRQn_Type is a device enumeration.
 */
void board_irqs_init(void);
void board_irq_unmask(void *ctx, uint32_t source);

/* ------------------------------------------------------------------ */
/* board_ -- identity and lifecycle                                    */
/* ------------------------------------------------------------------ */

/* This part, for the banner: "Cortex-M7", "x86-64". */
const char *board_name(void);

/*
 * The core clock, in Hz. Two callers with different needs: the run
 * summary divides retired instructions by it for KIPS, and lwIP's
 * sys_now() divides board_cycles() by it for milliseconds. A platform
 * whose "cycles" are microseconds says 1000000 and both come out right.
 */
uint32_t board_clock_hz(void);

/*
 * A free-running counter at board_clock_hz().
 *
 * **Real time, and it has to be.** This is lwIP's time base: a version
 * that returned a constant would freeze every timeout in the stack --
 * TFTP sessions never reclaimed, retransmissions never fired. That is the
 * __WFI defect CLAUDE.md records on the board, where the clock ran at 6%
 * of real time, in its absolute form.
 */
uint32_t board_cycles(void);

/*
 * The platform's own work between guest slices, or nothing.
 *
 * An IP stack advances only when called, so this is its entire schedule.
 * The debugger is *not* here: that is emu_debug_poll, one layer up, and
 * they are two questions.
 */
void board_poll(void);

/*
 * Host cycles for the *performance* figure, which is **not**
 * board_cycles().
 *
 * Two clocks, and collapsing them into one name produces a wrong number
 * that looks measured. board_cycles() must be real time because lwIP's
 * sys_now() divides it; this one is whatever the part executes at. A host
 * reporting the first as the second gave "ratio 2.01 host cycles per
 * guest instruction" for a board that really spends 429.
 *
 * A platform with no meaningful answer returns 0, and the ratio is
 * suppressed rather than computed from a clock that means something else.
 */
uint32_t board_perf_cycles(void);

/*
 * Guest time, in the units the frontend's timer expects.
 *
 * board_cycles() divided by a rate the platform knows -- the
 * normalisation this layer exists to do.
 */
uint64_t board_time_now(void);

/* ------------------------------------------------------------------ */
/* The two ends of a run                                               */
/* ------------------------------------------------------------------ */

/*
 * This platform's equivalent command line, or NULL to use the real one.
 *
 * **A board has no argv and still has options.** It wants the JIT, a
 * particular quantum, an instruction cap and a register dump on exit --
 * exactly the things the runner's command line names -- and it used to
 * set the corresponding struct fields by hand, in parallel with a parser
 * that understood the same settings by name. Two ways to say one thing,
 * and only one of them was documented by a --help.
 *
 * So a board hands over the argv it *would* have been given, and one
 * parser reads both. Beyond the deduplication, a board's policy is now
 * legible as a command line, and anything added to the parser reaches the
 * boards without a second edit.
 *
 * The array must outlive the call; a static is the obvious thing. argv[0]
 * is skipped as a program name, so it must be present.
 */
char *const *board_argv(int *argc);

/*
 * Bring this platform up, and state how it wants the run done.
 *
 * *Acquisition*, and only that. A board brings up its clocks and console
 * and has its image linked in; a runner allocates a heap and reads the
 * file its command line names. On return `cfg->image`/`image_size` name
 * the image and board_ram/_size the guest's memory, and anything only
 * this platform knows must be in `cfg` and `env` -- the cache ops, the
 * interrupt unmask hook, how guest time advances.
 *
 * A platform *finds* the image; installing it is emu_main's, through
 * emu_image_set, because which image is in force is a run-time fact the
 * upload path also changes.
 *
 * **Everything after this is common**, which is what makes this the whole
 * of a platform's start-up rather than most of it. Choosing the frontend,
 * printing the banner, handing the wire to the IP stack, and the cfg and
 * env fields that come straight from the command line are all emu_main's
 * -- they were written out per platform and the two copies had already
 * drifted, to the point that a board could not honour a --frontend its
 * own board_argv named.
 *
 * False means stop.
 */
struct emu_args;
struct emu_session_cfg;
struct emu_run_env;
struct emu_guest_exit;
bool board_init(const struct emu_args *args, struct emu_session_cfg *cfg,
                struct emu_run_env *env);

/*
 * Is there anywhere to go when the guest stops?
 *
 * A runner returns an exit status a suite reads; a board has nowhere to
 * return *to* and parks serving its link. That one bit is the whole of
 * what the two platforms disagreed about -- the park loop itself was
 * written out twice, identically, and is emu_board_after_run's now.
 *
 * A platform that answers false still parks while a link is up, because
 * then there is a client that may yet want the report or may push another
 * image.
 */
bool board_parks_after_run(void);

/*
 * Wait a short while inside the park loop.
 *
 * Per-part because the right way to wait is: a sleep instruction on a
 * board, a millisecond of real sleep on a machine with other work to do.
 * Whatever it does must not stop anything the runtime depends on -- see
 * the note on board_wfi in the STM32s' board.h, where sleeping stops the
 * clock lwIP tells the time by.
 */
void board_idle(void);

/*
 * The platform's last word, once the runner cannot continue.
 *
 * A host returns and lets the shell see a status; a board has nowhere to
 * return *to* and halts. That is all this is -- one part-specific fact.
 *
 * **The interesting half is not here.** If the link is up, the reason is
 * in a ring only a telnet client can drain, so halting writes the
 * diagnosis into memory nobody can reach and the board presents as a dead
 * link. Serving the stack instead is reasoning about the *link* rather
 * than the silicon, so it lives in emu_board_fatal(), which every caller
 * goes through and which reaches this afterwards.
 *
 * Never returns on a board.
 */
void board_fatal(int *status);

/* ------------------------------------------------------------------ */
/* board_console_ -- one byte out, one byte in                         */
/* ------------------------------------------------------------------ */

/*
 * The console wire, and on a platform with a network it is also the link:
 * src/net's sio layer moves PPP and SLIP bytes through these. That is why
 * emu_console.c decides *whether* the wire is still a console -- after the
 * handover it is not -- and these two only move the byte.
 */
void board_console_putc(uint8_t c);
int board_console_getc(void);

/*
 * Arm receive interrupts, where reception needs them.
 *
 * A USART with one byte of holding register and a run loop that reaches
 * it once per guest slice drops most of every frame at 921600 baud; a
 * kernel-buffered device has nothing to arm and defines this empty.
 */
void board_console_rx_irq_enable(void);

/* Bytes the link delivered and nothing collected. Reported beside the
 * guest's own numbers, because a dropped byte is a dropped frame. */
uint32_t board_console_rx_overruns(void);

/* ------------------------------------------------------------------ */
/* board_led_ -- activity, where there is somewhere to show it          */
/* ------------------------------------------------------------------ */

typedef enum { BOARD_LED_RX, BOARD_LED_TX } board_led_t;

/*
 * Per *frame*, not per byte: at 921600 a byte is 10.8 us, so a per-byte
 * toggle is a 45 kHz square wave and the LED reads as half-brightness
 * whatever the traffic.
 */
void board_led_toggle(board_led_t led);

/* ------------------------------------------------------------------ */
/* board_gdb_ -- the transport a debugger is served over               */
/* ------------------------------------------------------------------ */

/*
 * Whether this run wants a stub at all.
 *
 * A board always does -- it costs a listening socket on a link that is
 * already up, and the usual way to arrive at a guest bug there is to
 * watch it fail and then attach. A runner does it under --gdb, because
 * waiting for a debugger that is not coming is indistinguishable from a
 * hang.
 */
bool board_gdb_wanted(void);

/*
 * Start listening. False if the transport would not come up, having said
 * nothing -- emu_debug_start reports it, so that the message is the same
 * on every platform.
 *
 * `flash` is how gdb's `load` reaches an image store, or NULL where there
 * is none to write.
 */
bool board_gdb_start(emu_core_t *core, const emu_gdb_target_t *target,
                     const emu_gdb_flash_ops_t **flash);

/*
 * Where to connect, for the line this prints. A host says
 * "localhost:1234" and a board says its address; both are the thing a
 * person pastes after `target remote`.
 */
const char *board_gdb_where(void);

/*
 * Wait for a client before the guest runs, where that is wanted.
 *
 * A runner's guest is over in milliseconds, so a debugger that connects
 * "immediately" arrives after the run: without this, `--gdb` attaches to
 * a finished guest. A board's guest is still going, and its link may not
 * even be negotiated yet, so it does not wait.
 */
void board_gdb_wait(void);

/*
 * Run control, handed to the run loop. With a debugger attached the
 * *stub* drives the guest -- it owns stepping and breakpoints, and
 * running the cores as well would execute instructions the debugger
 * believes are still ahead of it.
 */
bool board_gdb_attached(void);
uint32_t board_gdb_run(uint32_t budget, uint32_t *retired);

/* Service the transport between slices, where it needs it. */
void board_gdb_poll(void);

/* ------------------------------------------------------------------ */
/* board_flash_ -- the guest-image arena, optional                      */
/* ------------------------------------------------------------------ */

/*
 * Spare storage a guest image can be programmed into at run time, so a
 * new guest arrives over TFTP or through gdb's `load` instead of over
 * SWD.
 *
 * **"Flash" is what it is on the boards, not what it has to be.** The
 * host backs the same calls with a malloc'd buffer, which is why the
 * addresses here are `uintptr_t` and not `uint32_t`: an address on a
 * 64-bit host does not fit in the width an MCU's flash map needs, and the
 * cast that would have made it fit is the kind that truncates in one
 * build and not the other. On an MCU uintptr_t *is* 32 bits, so nothing
 * changes there.
 *
 * What that buys is that everything above this line -- the TFTP server,
 * gdb's vFlashWrite, the commit-on-success rule, the erase-and-retry when
 * the arena fills -- is one implementation rather than one per platform.
 *
 * **A board without one returns 0 from board_flash_arena_size(), and
 * that is the whole of how it declines.** It used to be an #if plus
 * three stub callbacks per board; a run-time size is better here for the
 * reason emu_print_jit_stats takes the same shape -- the capability
 * macro that gated the old arrangement was read in a file that did not
 * include what defined it, which #if quietly treats as 0. A constant
 * zero folds the branch away for a board that has no arena, so the cost
 * is nothing and the failure mode is a wrong answer rather than silence.
 *
 * The arena is append-only and erased only when the next image will not
 * fit. A sector erase stalls flash fetch for seconds and costs one of
 * ten thousand cycles, so erasing per upload would be 274 erases per
 * suite run -- about thirty runs before the sector wears out. Packing
 * images end to end is roughly fifteen times better.
 */
uintptr_t board_flash_arena_base(void);
uint32_t board_flash_arena_size(void);

/*
 * Where the next image will be programmed, erasing first if the arena
 * has never been erased since reset. Returns 0 on failure.
 *
 * No length, because TFTP does not carry one: a transfer ends when a
 * short block arrives, so the size is known only once the whole image is
 * written. So writes run until they hit the end and *fail*, and the
 * caller erases and retries -- one wasted transfer per erase cycle
 * against fifteen times the flash wear.
 */
uintptr_t board_flash_arena_begin(void);

/* Accept `len` bytes at the address begin() returned, so the next image
 * starts after them. Not called when a transfer fails, which is what
 * makes a failed upload leave no trace. */
void board_flash_arena_commit(uint32_t len);

/* Erase unconditionally and restart from the base. */
bool board_flash_arena_reset(void);

/*
 * Program into the arena. Writes must be sequential and word aligned in
 * length except for the last -- which the TFTP path satisfies for free
 * with its 512-byte blocks and gdb does not, so the runner carries the
 * 1-3 byte remainder between calls.
 */
bool board_flash_write(uintptr_t addr, const void *data, uint32_t len);

/* The HAL's error code from the last board_flash_write: a refused
 * program and a full arena are different problems with different
 * recoveries. */
uint32_t board_flash_last_error(void);

/* ------------------------------------------------------------------ */
/* board_sync_ -- making written bytes fetchable                       */
/* ------------------------------------------------------------------ */

/*
 * Clean `len` bytes at `addr` out of the D-cache to the point of
 * unification and invalidate the matching instruction lines.
 *
 * The JIT writes instructions as *data* and then branches to them. On a
 * part with split caches the write sits in the D-cache while the
 * instruction side fetches through its own, and without this the core
 * executes whatever was at those addresses before -- not a wrong answer
 * but arbitrary code, and it fires on *reuse* of the code buffer rather
 * than on first write, so a short run looks perfectly healthy.
 *
 * **Why this is a board_ function and not part of the backend.** The
 * barriers are a property of the host instruction set and live in
 * `t2_sync_code`; whether there are caches to maintain is a property of
 * the *part*, and nothing in the compiler flags decides it --
 * -mcpu=cortex-m4 and -mcpu=cortex-m7 both define __ARM_ARCH_7EM__. So
 * the backend calls and the board answers.
 *
 * A board with nothing to maintain defines this empty, the same way it
 * declines everything else here. It used to be *weak* in the backend
 * instead, which looks equivalent and is not: a new platform then got
 * silent no-op cache maintenance by default. The Cortex-M55 port was
 * written that way and linked without a word, on a part with both caches
 * enabled. Requiring the definition turns that into a link error naming
 * the symbol.
 */
void board_sync_icache(const void *addr, uint32_t len);

#ifdef __cplusplus
}
#endif

#endif /* EMU_PLATFORM_BOARD_API_H */
