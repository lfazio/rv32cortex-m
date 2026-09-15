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

#include "emu_image.h"
#include "emu_session.h"

#if EMU_NET
#include "emu_net.h"
#endif

/*
 * emu_virtio.h, not just the macro. EMU_HAVE_VIRTIO comes from emucore's
 * PUBLIC compile definitions, so it is defined here -- but this file
 * *calls* emu_virtio_net_poll, and a capability macro read without
 * including what declares the function behind it is the shape that
 * already cost this tree a silently-interpreted frontend.
 */
#if EMU_HAVE_VIRTIO
#include "emu/emu_virtio.h"
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

/*
 * The runner cannot continue, and has already said why.
 *
 * What to do about it splits in two, and only the second half is the
 * platform's. **If the link is up, the reason is in a ring nobody can
 * reach** -- the console was handed to the IP stack, so halting here
 * writes the diagnosis into memory and presents as a dead link: no ping,
 * no telnet, no TFTP. That is not hypothetical; a start-up ordering bug
 * halted with "could not build the guest address space" sitting in the
 * ring, and an hour went on the network for a fault that had already
 * diagnosed itself.
 *
 * So with the stack up, keep servicing it for ever. Nothing else runs,
 * which is the point of a halt, and a client can still connect and
 * collect the reason. That reasoning is about the *link*, not about the
 * silicon, so it is here; what a platform is left with is its last word
 * -- a shell to return a status to, or nowhere to go.
 */
void emu_board_fatal(int *status)
{
    *status = 1;

    if (emu_board_link_up()) {
        for (;;) {
            emu_board_poll();
        }
    }
    board_fatal(status);
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
    /*
     * The console's receive line. The transport is polled, so this is
     * the only thing that ever notices a byte has arrived -- without it
     * a guest that enables the receive interrupt waits for one that
     * cannot happen.
     */
    emu_session_poll_uart();

#if EMU_HAVE_VIRTIO
    /*
     * The guest's own network interface, which is a different thing from
     * the one above: emu_net_poll drives the stack *the firmware* speaks
     * over its UART, and this moves frames between the host and the
     * guest's virtio device. A machine can have both, and they share
     * nothing but this line.
     *
     * Once per slice, because the receive path has no other schedule --
     * a tap fd nobody reads fills up and the host starts dropping, which
     * presents as a link that works in one direction.
     */
    emu_virtio_net_poll();
#endif
    board_poll();
}

/*
 * The guest has stopped. Park, or hand back an exit status.
 *
 * **The same four steps on both platforms**, written out twice until now
 * and differing only in two answers a board gives: whether there is
 * anywhere to return to, and what "wait" means.
 *
 *   drain    everything the run produced is still in the output ring --
 *            the run loop stopped, and with it the only thing that was
 *            delivering. Parking without draining first loses the entire
 *            report, which is the part a harness came for.
 *   reload   an image may arrive *after* the guest has finished, and that
 *            is the normal case rather than an edge one: a harness runs a
 *            test, waits for it to halt and report, then pushes the next.
 *            The check inside the run loop never sees those, because that
 *            loop exited when the guest halted -- so an upload completed
 *            successfully, said so, and nothing happened.
 *   debug    run control still works after the guest has finished.
 *            Without it a park loop services the link and nothing else,
 *            so gdb accepts `continue` and waits for ever.
 *   wait     board_idle, which on a board must not stop lwIP's clock.
 *
 * True means run again.
 */
bool emu_board_after_run(const emu_guest_exit_t *exit, bool capped,
                         uint32_t slice, int *status)
{
    (void)capped;

    if (!board_parks_after_run() && !emu_board_link_up()) {
        *status = exit->exited ? (int)exit->code : 0;
        return false;
    }

    if (!board_parks_after_run()) {
        emu_console_printf(
            "emu: guest finished; serving the link (^C to quit)\n");
    }

    for (;;) {
        emu_board_poll();

        if (emu_image_take_pending()) {
            return true;
        }
        if (emu_debug_parked_step(slice)) {
            continue;
        }
        board_idle();
    }
}
