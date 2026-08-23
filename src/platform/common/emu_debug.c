/* SPDX-License-Identifier: Apache-2.0 */
/*
 * emu_debug.c - see emu_debug.h for the three layers this is the middle
 * of.
 *
 * **The network transport lives here, not in a board.** Serving gdb over
 * the IP stack is the same code on every platform -- emu_net_gdb_init,
 * emu_net_gdb_attached, emu_net_gdb_run and an address to print -- and it
 * was written out in both boards behind `#if EMU_NET`, which made a
 * *build option* look like a property of the silicon. The STM32 half was
 * the clearest case: all seven of its board_gdb_* functions were the
 * network and nothing else, so a file whose entire job is part-specific
 * facts carried a transport that has nothing to do with the part.
 *
 * So this layer decides which transport is in force, and board_gdb_*
 * means what its name says: the transport *this platform* has of its own.
 * On the host that is the loopback socket behind --gdb; on the boards
 * there is none, and they answer with nothing the way board_api.h says a
 * platform declines everything else.
 *
 * The network wins when it is up, on both. It is the only transport a
 * board has, and on the host asking for --ppp is asking for the stub over
 * the link -- a loopback socket beside it would be a second way in to one
 * stub.
 */

#include "emu_debug.h"
#include "emu_console.h"

#if EMU_NET
#  include "emu_net.h"
#  include "emu_image.h"
#endif

/*
 * Is the IP stack up and therefore serving the stub?
 *
 * A function rather than `#if EMU_NET` at each site: with the transport
 * compiled out this folds to a constant false and every branch below
 * disappears, which is the same "decline by answering, not by omitting"
 * rule board_api.h states for platforms.
 */
static bool net_serving(void)
{
#if EMU_NET
    return emu_net_active();
#else
    return false;
#endif
}

bool emu_board_link_up(void)
{
    return net_serving();
}

bool emu_board_link_start(void)
{
#if EMU_NET
    emu_console_printf("net    %s on this port; telnet %s 23\n",
                       EMU_NET_LINK_PPP ? "PPP" : "SLIP", emu_net_addr_str());
    if (emu_net_init()) {
        return true;
    }
    emu_console_printf("net    failed to start; staying on the serial "
                       "console\n");
#endif
    return false;
}

void emu_debug_start(emu_system_t *sys, const emu_cpu_ops_t *ops)
{
    if (!net_serving() && !board_gdb_wanted()) {
        return;
    }

    /*
     * The frontend states its own layout. Answering "no" is a normal
     * outcome -- the PowerPC frontend has no description yet -- and is
     * reported rather than treated as a failure, because a platform that
     * cannot serve gdb is still one that runs guests.
     */
    const emu_gdb_target_t *const gt =
        (ops->gdb_target != NULL) ? ops->gdb_target() : NULL;

    if (gt == NULL) {
        emu_console_printf("gdb    frontend %s has no target description\n",
                           ops->name);
        return;
    }

    const emu_gdb_flash_ops_t *flash = NULL;

    if (net_serving()) {
#if EMU_NET
        /*
         * gdb's `load` writes through the same arena a TFTP upload lands
         * in -- one image store, reached two ways. A platform with no
         * arena says so by returning 0 from board_flash_arena_size(), and
         * emu_image's own ops answer accordingly; there is nothing to
         * decide here.
         */
        flash = &emu_image_gdb_flash;
        if (!emu_net_gdb_init(&sys->core[0], gt, flash)) {
            emu_console_printf("gdb    stub failed to start\n");
            return;
        }
        emu_console_printf("gdb    target remote %s:1234\n",
                           emu_net_addr_str());
#endif
        /*
         * No wait over the link: the guest is served for as long as the
         * process or the board runs, so there is no race to lose, and
         * blocking would stop the run before the person has even brought
         * the link up.
         */
        return;
    }

    if (!board_gdb_start(&sys->core[0], gt, &flash)) {
        emu_console_printf("gdb    stub failed to start\n");
        return;
    }

    emu_console_printf("gdb    target remote %s\n", board_gdb_where());

    /*
     * After the line, so a person who is going to attach has read where
     * to attach to before this blocks.
     */
    board_gdb_wait();
}

/*
 * With a debugger attached the *stub* drives the guest: it owns stepping
 * and breakpoints, and stepping the cores here as well would execute
 * instructions the debugger believes are still ahead of it.
 */
uint32_t emu_debug_run(emu_system_t *sys, uint32_t budget, uint32_t *retired,
                       bool *all_idle)
{
    *all_idle = false;

#if EMU_NET
    if (net_serving()) {
        if (emu_net_gdb_attached()) {
            uint32_t n = 0u;

            (void)emu_net_gdb_run(budget, &n);
            *retired = n;
            return n;
        }
        *retired = emu_system_step(sys, budget, all_idle);
        return *retired;
    }
#endif

    if (board_gdb_attached()) {
        uint32_t n = 0u;

        (void)board_gdb_run(budget, &n);
        *retired = n;
        return n;
    }

    *retired = emu_system_step(sys, budget, all_idle);
    return *retired;
}

/*
 * Run control while the guest is parked. True when a debugger drove it.
 *
 * Without this a park loop services the link and nothing else, so a
 * debugger attaching to a *completed* run can read registers and memory
 * and then hangs the moment it resumes: `continue` is accepted, nothing
 * executes, and gdb waits for ever. That is the normal way to arrive
 * there -- push an image, watch it fail, attach to find out why -- and
 * rewinding the pc to re-run under a breakpoint is most of what it is
 * for.
 */
bool emu_debug_parked_step(uint32_t budget)
{
#if EMU_NET
    if (net_serving()) {
        if (!emu_net_gdb_attached()) {
            return false;
        }

        uint32_t n = 0u;

        (void)emu_net_gdb_run(budget, &n);
        return true;
    }
#endif
    if (!board_gdb_attached()) {
        return false;
    }

    uint32_t n = 0u;

    (void)board_gdb_run(budget, &n);
    return true;
}

void emu_debug_poll(void)
{
    /*
     * Nothing over the link: emu_board_poll drives the whole stack once
     * per slice, and the stub is served from inside that.
     */
    if (net_serving()) {
        return;
    }
    board_gdb_poll();
}

/*
 * The platform's own work between guest slices, plus the IP stack's.
 *
 * The stack advances only when called, so this is its entire schedule --
 * once per slice, finer than any timeout lwIP keeps. It is here rather
 * than in each board_poll() for the same reason the transport above is:
 * "is there a network" is a build option, not a fact about the part.
 */
void emu_board_poll(void)
{
#if EMU_NET
    emu_net_poll();
#endif
    board_poll();
}
