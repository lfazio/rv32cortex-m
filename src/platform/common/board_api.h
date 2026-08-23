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
 * supposed to be the bottom of the stack. They are board_add_regions,
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
extern uint32_t       board_img_size;

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
 * Add this board's own regions to the bus, after the runner has added
 * the guest image and RAM and before the frontend adds its devices.
 *
 * This is where the passthrough windows go -- the identity-mapped
 * peripheral space that lets a guest driver reach real hardware, which is
 * the entire point of this emulator and is necessarily per-part: the
 * windows differ, and so does which of them a guest may write.
 */
bool board_add_regions(emu_bus_t *bus);

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
 * Everything before a guest can be brought up: bring the part up, obtain
 * an image, say hello -- and state how this platform wants the run done.
 *
 * *Acquisition*, the first of the two halves emu_session.h says a
 * platform cannot share. A board brings up its clocks and peripherals and
 * has its image linked in; a runner parses argv and reads the file it
 * names. argc/argv are what a hosted platform gets and a bare-metal one
 * ignores.
 *
 * The runner fills in what it owns -- the buses, the UART, the syscall
 * handler -- before calling this, and the platform fills in the rest. Two
 * structs rather than a hook each: what a platform decides about a run is
 * already what emu_session_cfg_t and emu_run_env_t describe.
 *
 * On return, board_img/_size and board_ram/_size must be set.
 * False means stop, with *status as the exit code.
 */
struct emu_session_cfg;
struct emu_run_env;
struct emu_guest_exit;
bool board_startup(int argc, char **argv, int *status,
                   struct emu_session_cfg *cfg, struct emu_run_env *env);

/*
 * The run is over. *Termination*, the second half: a runner returns an
 * exit status a suite reads, and a board has nowhere to go and parks
 * serving its link.
 *
 * True means run again -- an image arrived while parked, which is the
 * normal way a board is used by a harness, because a harness uploads
 * *between* runs when the run loop has already exited.
 */
bool board_after_run(const struct emu_guest_exit *exit, bool capped,
                     int *status);

/*
 * The runner cannot continue, and has already said why.
 *
 * A host returns and lets the shell see a status. A board has nowhere to
 * return *to* -- and by this point it may have given its console to the
 * network, so the reason is in a ring only a telnet client can drain.
 * Halting with interrupts masked writes it into memory nobody can reach:
 * the board answers no ping, no telnet and no TFTP, and presents as a
 * dead link rather than as a firmware that knows what went wrong. So it
 * keeps servicing the stack instead, for ever.
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
int  board_console_getc(void);

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

typedef enum {
    BOARD_LED_RX,
    BOARD_LED_TX
} board_led_t;

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
bool     board_gdb_attached(void);
uint32_t board_gdb_run(uint32_t budget, uint32_t *retired);

/* Service the transport between slices, where it needs it. */
void board_gdb_poll(void);

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
