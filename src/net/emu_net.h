/* SPDX-License-Identifier: Apache-2.0 */
/*
 * emu_net.h - the board's network transport, as the firmware sees it.
 *
 * Why this exists: running the architecture suite on hardware means one
 * guest image per test, and there are 274 of them. Rebuilding and
 * reflashing the firmware for each is minutes per test and wears the
 * part; pushing the image over a wire is seconds. The Nucleo already has
 * a wire that works -- the ST-LINK's virtual COM port -- so the stack
 * runs SLIP over it rather than bringing up the Ethernet PHY.
 *
 * The consequence, and the thing to understand before using this: the
 * UART stops being a console the moment emu_net_init() succeeds. SLIP
 * frames and printable text cannot share a serial line, so from that
 * point the console is a telnet connection and the UART carries nothing
 * but IP. Everything printed before then went to the UART and can be
 * read with a terminal; everything after is buffered until a telnet
 * client arrives.
 *
 * That is deliberate rather than incidental. The alternative -- keeping
 * text on the UART and framing around it -- makes a corrupted SLIP
 * stream indistinguishable from a working one, and the failure it
 * produces (a host that never answers a ping) says nothing about why.
 * With a hard handover, the banner on the UART is the last thing the
 * board says before the link is the only channel, so "the banner printed
 * and then nothing" localises the fault to the link and not to the
 * firmware.
 */
#ifndef EMU_NET_H
#define EMU_NET_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Bring the stack up and hand the console UART to SLIP. Nothing may write
 * to the UART afterwards.
 *
 * Returns false if the interface would not start, in which case the UART
 * is left alone and the caller should carry on with a serial console --
 * a board that cannot reach the network is still a board that can run a
 * guest and say so.
 */
bool emu_net_init(void);

/* True once the handover has happened. */
bool emu_net_active(void);

/*
 * Service the stack. Called from the run loop between guest slices,
 * which at the default slice is every few hundred microseconds -- far
 * more often than the receive ring can fill, and often enough that no
 * separate timer is needed.
 */
void emu_net_poll(void);

/*
 * The console, once the UART is gone. Output is buffered and delivered
 * to whichever telnet client is connected; input comes from the same
 * connection, and is what the guest's virtual UART reads.
 *
 * Output produced with no client connected is kept until the buffer
 * fills and then dropped, with a count reported when one connects. The
 * boot banner therefore survives, which is what it is for.
 */
void emu_net_console_putc(uint8_t c);
int  emu_net_console_getc(void);

/* The board's address, as dotted quad, for the banner. */
const char *emu_net_addr_str(void);

/* ------------------------------------------------------------------ */
/* Internal to src/net/                                                */
/* ------------------------------------------------------------------ */

bool emu_net_telnet_init(void);
void emu_net_telnet_poll(void);

#endif /* EMU_NET_H */
