/* SPDX-License-Identifier: Apache-2.0 */
/*
 * net.c - the stack, its clock, and the handover of the UART.
 *
 * lwIP with NO_SYS=1 is a library, not a thread: it advances only when
 * something calls into it. Everything that would be a task elsewhere is
 * a call from emu_net_poll(), which the run loop makes between guest
 * slices. That is the whole scheduling model, and it is why nothing in
 * src/net/ needs a lock -- the only interrupt in the picture fills the
 * UART receive ring in board.c and touches nothing here.
 */

#include "emu_net.h"

#include "lwip/init.h"
#include "lwip/ip_addr.h"
#include "lwip/netif.h"
#include "lwip/timeouts.h"
#include "netif/slipif.h"

#include "board.h"

/*
 * The two ends of the wire. A SLIP link is point to point and carries no
 * addressing of its own, so these are pure convention -- they only have
 * to match what the host end is configured with, which is what
 * scripts/slip-up.sh passes to slattach.
 *
 * 192.168.7.0/24 rather than the more usual 192.168.1.0/24 because the
 * host almost certainly has that one already, and a route collision
 * presents as traffic silently going out of the wrong interface.
 */
#ifndef EMU_NET_ADDR
#define EMU_NET_ADDR  "192.168.7.2"
#endif
#ifndef EMU_NET_PEER
#define EMU_NET_PEER  "192.168.7.1"
#endif
#ifndef EMU_NET_MASK
#define EMU_NET_MASK  "255.255.255.0"
#endif

static struct netif g_slip;
static bool         g_active;

/* ------------------------------------------------------------------ */
/* The clock lwIP's timers run on                                      */
/* ------------------------------------------------------------------ */

/*
 * board_cycles() is a free-running 32-bit counter at the core clock, so
 * at 216 MHz it wraps every 19.9 seconds. The subtraction below is
 * wrap-correct in unsigned arithmetic and therefore right for any gap
 * shorter than one wrap -- but only for that, so sys_now() has to be
 * reached more often than every 20 seconds. It is: emu_net_poll() calls
 * it once per guest slice, which is a few hundred microseconds.
 *
 * The remainder is carried rather than discarded. Dropping it would lose
 * up to a millisecond per call, which at this call rate is a clock that
 * runs at a small fraction of real time -- and lwIP's retransmission
 * timers would stretch with it, so a lost packet would look like a dead
 * link rather than a slow one.
 */
static uint32_t g_last_cycles;
static uint32_t g_cycle_rem;
static uint32_t g_ms;

u32_t sys_now(void)
{
    const uint32_t per_ms = board_clock_hz() / 1000u;
    const uint32_t now = board_cycles();
    const uint32_t delta = (now - g_last_cycles) + g_cycle_rem;

    g_last_cycles = now;
    g_ms += delta / per_ms;
    g_cycle_rem = delta % per_ms;
    return g_ms;
}

uint32_t emu_net_rand(void)
{
    /*
     * Used for TCP initial sequence numbers and the TFTP transfer id.
     * The cycle counter is genuinely unpredictable across resets in a way
     * a seeded PRNG on deterministic firmware is not, and mixing in the
     * millisecond count keeps two calls in the same instant apart.
     */
    return board_cycles() * 2654435761u + g_ms;
}

void emu_net_assert_fail(const char *msg, const char *file, int line)
{
    /*
     * An assertion inside the stack is a bug in this port. Stopping
     * leaves the state intact for a debugger, which is the same choice
     * the fault handlers make; printing first means the message reaches
     * whichever console is live, which by now is telnet -- so the poll
     * below is what actually gets it out of the door before halting.
     */
    (void)file;
    (void)line;

    for (const char *p = msg; *p != '\0'; p++) {
        emu_net_console_putc((uint8_t)*p);
    }
    emu_net_console_putc('\n');
    emu_net_telnet_poll();

    for (;;) {
    }
}

/* ------------------------------------------------------------------ */

bool emu_net_active(void)
{
    return g_active;
}

const char *emu_net_addr_str(void)
{
    return EMU_NET_ADDR;
}

bool emu_net_init(void)
{
    ip4_addr_t addr, mask, peer;

    /* Seed the clock before anything can ask it the time. */
    g_last_cycles = board_cycles();

    lwip_init();

    if (!ip4addr_aton(EMU_NET_ADDR, &addr) ||
        !ip4addr_aton(EMU_NET_MASK, &mask) ||
        !ip4addr_aton(EMU_NET_PEER, &peer)) {
        return false;
    }

    /*
     * netif->state carries the serial device number for slipif_init,
     * which is why it is a cast integer and not a pointer to anything.
     * Device 0 is the console UART; net_sio.c refuses any other.
     *
     * netif_input rather than ip_input: SLIP has no link layer to strip,
     * but netif_input is the documented entry point and is what keeps
     * the loopback and IPv6 paths correct if either is ever turned on.
     */
    if (netif_add(&g_slip, &addr, &mask, &peer,
                  (void *)0, slipif_init, netif_input) == NULL) {
        return false;
    }

    netif_set_default(&g_slip);
    netif_set_up(&g_slip);
    /*
     * A serial line has no carrier to detect, so nothing will ever raise
     * this on its own -- and with it down, lwIP silently discards
     * everything queued for output.
     */
    netif_set_link_up(&g_slip);

    if (!emu_net_telnet_init()) {
        return false;
    }
    if (!emu_net_tftp_init()) {
        return false;
    }

    /*
     * Last, and only once everything above has succeeded: from here the
     * UART carries SLIP and the console is telnet. A failure before this
     * point leaves the caller with a working serial console to report it
     * on, which is the only reason the flag is not set at the top.
     */
    g_active = true;
    return true;
}

void emu_net_poll(void)
{
    static uint32_t last_ms;

    /*
     * Draining the receive ring is the part that cannot be deferred --
     * it is 2 KiB deep and the wire fills it in 22 ms -- so it happens
     * every call.
     */
    slipif_poll(&g_slip);
    emu_net_telnet_poll();

    /*
     * The timer wheel is not. sys_check_timeouts() walks a list and is
     * cheap, but "cheap" is still paid once per guest slice, and a slice
     * is 4096 instructions -- the same reasoning that keeps the JIT's
     * generation check off the dispatch path. A millisecond is finer
     * than any timeout lwIP schedules.
     */
    const uint32_t now = sys_now();

    if (now != last_ms) {
        last_ms = now;
        sys_check_timeouts();
    }
}
