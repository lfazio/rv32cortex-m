/* SPDX-License-Identifier: Apache-2.0 */
/*
 * board.h - What changes when the board does.
 *
 * Everything behind this interface is a fact about the silicon and the
 * PCB: which oscillator, which PLL dividers, how many flash wait states,
 * which USART the ST-LINK's virtual COM port is wired to, and whether the
 * core has caches to turn on. Nothing here knows the emulator exists.
 *
 * The split exists because those facts are the entire difference between
 * the supported boards, and they were previously interleaved with the
 * emulator glue in main.c -- so porting meant reading 800 lines to find
 * the forty that mattered. A Nucleo-144 putting its VCP on USART3 rather
 * than USART2 is exactly the kind of detail that hides there and produces
 * a board that runs and says nothing.
 *
 * **One copy, for all three STM32 platforms.** There were two, character
 * for character the same apart from where a blank line fell, and the N6
 * port was about to make a third. A header describing what a board owes
 * the runner is not per-board by nature -- board_api.h already says so
 * for the part every platform shares, and this is the STM32 half of the
 * same idea: the flash arena, and bring-up in the order these parts want
 * it. What is genuinely per-part is the *implementation*, which is what
 * each platform's board.c is.
 */
#ifndef BOARD_H_
#define BOARD_H_

#include <stdbool.h>
#include <stdint.h>

#include "emu/emu_cache.h"
#include "board_api.h"

/*
 * Bring the part up: caches, clock tree, console. Called first, before
 * anything reads the clock or prints.
 */
void board_init(void);






/*
 * Park until an interrupt, and stop for good.
 *
 * Here rather than __WFI() in the runner so that emu_main.c needs no
 * CMSIS header, and because the right way to wait is a property of the
 * part: on a core with no sleep instruction, or one where sleeping
 * gates a clock something else depends on, this is where that is
 * decided.
 */
/*
 * Park until an interrupt: this part's sleep instruction, and nothing
 * else.
 *
 * **Not board_idle**, which is the contract emu_main parks with and which
 * stm32/board.c implements in terms of this. The difference is one hard-won
 * rule: __WFI gates the processor clock, and CYCCNT counts *processor*
 * cycles -- so sleeping while the IP stack is up nearly stops lwIP's clock.
 * Measured at about 6% of real time over 29 seconds parked, which froze
 * every timeout in the stack and made TFTP sessions unreclaimable. That
 * rule is the same on all three parts, so it is written once, one layer
 * up.
 */
void board_wfi(void);

/* board_fatal is board_api.h's -- every platform has one, and on these
 * parts it is the per-part halt that the shared board.c used to wrap. */


/* ------------------------------------------------------------------ */
/* Link activity                                                       */
/* ------------------------------------------------------------------ */


/*
 * Set board_ram / board_ram_size from the link script's symbols, before
 * anything uses the guest's memory.
 *
 * STM32-only, so it is here and not in board_api.h: on these parts guest
 * RAM is whatever the link left between .bss and the stack, which is a
 * *difference of two linker symbols* -- something C will not accept in a
 * static initialiser however constant it is at run time. A host mallocs
 * its guest memory and has nothing to do here.
 */
void board_ram_init(void);

/* ------------------------------------------------------------------ */
/* Cache maintenance -- two of them, and they are not the same           */
/* ------------------------------------------------------------------ */

/*
 * The guest's own cache-block operations onto this part's D-cache:
 * RISC-V's `cbo.clean`/`inval`/`flush` handed the *host* address backing
 * the guest block, so a guest driver cleaning a DMA buffer cleans the ARM
 * lines that hold it. Defined in cache.c; the runner hands it to the
 * frontend through emu_session_cfg_t.
 *
 * Declared here rather than as a bare `extern` inside board.c, for the
 * reason board_sync_icache moved out of the Thumb-2 backend: a `board_*`
 * symbol's contract belongs in a board header, and a prototype written at
 * its use site is a prototype nothing checks against the definition.
 *
 * **Not the same thing as board_sync_icache**, which is the other cache
 * maintenance on this part and looks close enough to merge. They share
 * one line -- the clean -- and differ everywhere that matters:
 *
 *   board_cache_ops     guest data, D-cache only. emu_cache_op_t has
 *                       exactly INVAL/CLEAN/FLUSH and no I-side member.
 *                       Called by a guest instruction, per guest block.
 *   board_sync_icache   the JIT's *own* emitted code: clean to the point
 *                       of unification and then invalidate the
 *                       **instruction** lines. Called by the framework
 *                       after every translation, on memory no guest can
 *                       name.
 *
 * Routing the second through the first would maintain a D-cache for bytes
 * the instruction side is about to fetch -- a plausible-looking no-op,
 * and the exact mistake G4MH's CACHE instruction is documented as
 * avoiding.
 */
extern const emu_cache_ops_t board_cache_ops;


#endif /* BOARD_H_ */
