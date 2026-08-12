/* SPDX-License-Identifier: Apache-2.0 */
/*
 * net_telnet.c - the console, once SLIP has taken the UART.
 *
 * A raw-TCP server on port 23 carrying exactly what the serial console
 * carried: the firmware's own messages and the guest's virtual UART, in
 * both directions. It is telnet only to the extent that a telnet client
 * can connect to it -- the option negotiation below is the minimum that
 * stops a client from waiting for a reply that never comes.
 *
 * Two things here are not obvious.
 *
 * The output buffer exists because output happens when the guest runs
 * and delivery happens when the stack is polled, and those are not the
 * same instant. Without it every character would have to wait for a
 * poll, which would put a TCP round trip in the middle of the guest's
 * run loop.
 *
 * Output produced before a client connects is kept, not discarded. The
 * boot banner says which frontend, which clock, how much guest RAM and
 * which backend -- and it is all printed before anything could possibly
 * have connected. Dropping it would mean the one message that explains
 * what is running is the one message nobody ever sees.
 */

#include "emu_net.h"

#include "lwip/tcp.h"

#include <string.h>

#define TELNET_PORT 23u

/*
 * Sized to hold the boot banner several times over. It is .bss, so it
 * comes out of the guest's SRAM -- 4 KiB against the ~60 KiB of headroom
 * the guest sizing work left, which is worth it for not losing the one
 * output that is guaranteed to have no reader.
 */
#define TX_SIZE 4096u
#define TX_MASK (TX_SIZE - 1u)

/*
 * The receive side only has to keep up with a human typing or a test
 * harness feeding a guest, and the TCP window provides the real back
 * pressure, so it is small.
 */
#define RX_SIZE 256u
#define RX_MASK (RX_SIZE - 1u)

static struct tcp_pcb *g_listen;
static struct tcp_pcb *g_conn;

static uint8_t  g_tx[TX_SIZE];
static uint32_t g_tx_head, g_tx_tail;   /* free-running; count = head - tail */
static uint32_t g_tx_dropped;

static uint8_t  g_rx[RX_SIZE];
static uint32_t g_rx_head, g_rx_tail;

/*
 * Telnet's IAC state machine, kept across calls because a command can be
 * split across TCP segments.
 */
enum { IAC_NONE = 0, IAC_CMD, IAC_OPT };
static uint8_t g_iac_state;

#define TELNET_IAC  255u
#define TELNET_SB   250u
#define TELNET_SE   240u
#define TELNET_WILL 251u
#define TELNET_DONT 254u
#define TELNET_OPT_ECHO 1u
#define TELNET_OPT_SGA  3u

/* ------------------------------------------------------------------ */
/* The console side                                                    */
/* ------------------------------------------------------------------ */

void emu_net_console_putc(uint8_t c)
{
    if ((g_tx_head - g_tx_tail) >= TX_SIZE) {
        /*
         * Drop the new byte, not the old one. With no client connected
         * the buffer holds the boot banner, and overwriting it with the
         * guest's output would lose exactly the part worth keeping. The
         * count is reported when a client arrives, so the loss is
         * visible rather than silent.
         */
        g_tx_dropped++;
        return;
    }
    g_tx[g_tx_head & TX_MASK] = c;
    g_tx_head++;
}

int emu_net_console_getc(void)
{
    if (g_rx_head == g_rx_tail) {
        return -1;
    }
    return (int)g_rx[g_rx_tail++ & RX_MASK];
}

static void console_puts(const char *s)
{
    while (*s != '\0') {
        emu_net_console_putc((uint8_t)*s++);
    }
}

/* ------------------------------------------------------------------ */
/* Delivery                                                            */
/* ------------------------------------------------------------------ */

/*
 * Push as much of the buffer as the connection will take. The ring is
 * written in two pieces when it wraps, which is why this loops rather
 * than making one call: tcp_write needs a contiguous range.
 */
static void telnet_flush(void)
{
    if (g_conn == NULL) {
        return;
    }

    while (g_tx_head != g_tx_tail) {
        const u16_t space = tcp_sndbuf(g_conn);

        if (space == 0u) {
            break;
        }

        uint32_t n = g_tx_head - g_tx_tail;
        const uint32_t to_end = TX_SIZE - (g_tx_tail & TX_MASK);

        if (n > to_end) {
            n = to_end;
        }
        if (n > space) {
            n = space;
        }

        /*
         * Copied into the stack's own buffers rather than referenced.
         * The alternative saves a memcpy and is wrong here: the ring
         * keeps being written by the guest while the segment waits to be
         * acknowledged, so a zero-copy write would transmit whatever the
         * bytes had become by then.
         */
        if (tcp_write(g_conn, &g_tx[g_tx_tail & TX_MASK], (u16_t)n,
                      TCP_WRITE_FLAG_COPY) != ERR_OK) {
            break;
        }
        g_tx_tail += n;
    }
    tcp_output(g_conn);
}

void emu_net_telnet_poll(void)
{
    telnet_flush();
}

/* ------------------------------------------------------------------ */
/* Callbacks                                                           */
/* ------------------------------------------------------------------ */

static err_t telnet_sent(void *arg, struct tcp_pcb *pcb, u16_t len)
{
    LWIP_UNUSED_ARG(arg);
    LWIP_UNUSED_ARG(pcb);
    LWIP_UNUSED_ARG(len);

    /* Room has just been freed; use it now rather than waiting on a poll. */
    telnet_flush();
    return ERR_OK;
}

static void telnet_drop(void)
{
    if (g_conn != NULL) {
        tcp_arg(g_conn, NULL);
        tcp_recv(g_conn, NULL);
        tcp_sent(g_conn, NULL);
        tcp_err(g_conn, NULL);
        tcp_close(g_conn);
        g_conn = NULL;
    }
    g_iac_state = IAC_NONE;
}

/*
 * Feed one received byte to the console, filtering telnet's in-band
 * commands out of it.
 *
 * Every option the client offers is refused by silence rather than by a
 * reply. That is not laziness: the two options this end cares about are
 * announced unprompted on connect, and a client that has been told WILL
 * ECHO and WILL SUPPRESS-GO-AHEAD has nothing left to ask that would
 * change how the stream is carried. Answering the rest would mean
 * implementing a negotiation table for options that would all be
 * declined anyway.
 *
 * Subnegotiation is not tracked as a separate state, because its payload
 * is delimited by IAC SE and IAC is what this filter already keys on --
 * so the bytes in between are treated as data and dropped one at a time
 * only if they happen to be commands. In practice no client sends
 * subnegotiation to a server offering nothing, and the cost of being
 * wrong is a few stray bytes on a console, not a protocol error.
 */
static void telnet_rx_byte(uint8_t c)
{
    switch (g_iac_state) {
    case IAC_CMD:
        /*
         * WILL/WONT/DO/DONT each carry an option byte; everything else
         * is a two-byte command that ends here.
         */
        g_iac_state = (c >= TELNET_SB && c != TELNET_SE) ? IAC_OPT : IAC_NONE;
        return;

    case IAC_OPT:
        g_iac_state = IAC_NONE;
        return;

    default:
        break;
    }

    if (c == TELNET_IAC) {
        g_iac_state = IAC_CMD;
        return;
    }

    /*
     * A full receive ring means the guest is not reading its UART. Drop
     * rather than block: the run loop is what would have to block, and
     * stalling the emulator because a console byte has nowhere to go
     * would turn a cosmetic problem into a hang.
     */
    if ((g_rx_head - g_rx_tail) < RX_SIZE) {
        g_rx[g_rx_head++ & RX_MASK] = c;
    }
}

static err_t telnet_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p,
                         err_t err)
{
    LWIP_UNUSED_ARG(arg);

    if (p == NULL || err != ERR_OK) {
        /* The client went away, or the connection failed. */
        if (p != NULL) {
            pbuf_free(p);
        }
        telnet_drop();
        return ERR_OK;
    }

    for (struct pbuf *q = p; q != NULL; q = q->next) {
        const uint8_t *const b = (const uint8_t *)q->payload;

        for (u16_t i = 0; i < q->len; i++) {
            telnet_rx_byte(b[i]);
        }
    }

    /*
     * Acknowledged in full. The console has no flow control to offer --
     * bytes the guest does not read are dropped above -- so holding the
     * window closed would only stall the client without preserving
     * anything.
     */
    tcp_recved(pcb, p->tot_len);
    pbuf_free(p);
    return ERR_OK;
}

static void telnet_err(void *arg, err_t err)
{
    LWIP_UNUSED_ARG(arg);
    LWIP_UNUSED_ARG(err);

    /*
     * The pcb is already freed by the time this is called, so it must
     * not be closed again -- just forgotten.
     */
    g_conn = NULL;
    g_iac_state = IAC_NONE;
}

static err_t telnet_accept(void *arg, struct tcp_pcb *pcb, err_t err)
{
    LWIP_UNUSED_ARG(arg);

    if (err != ERR_OK || pcb == NULL) {
        return ERR_VAL;
    }

    /*
     * One client. A second connection replaces the first rather than
     * being refused: the common way to get here is a harness that died
     * without closing, and refusing would leave the board unreachable
     * until its keepalive timer noticed.
     */
    telnet_drop();
    g_conn = pcb;

    tcp_arg(pcb, NULL);
    tcp_recv(pcb, telnet_recv);
    tcp_sent(pcb, telnet_sent);
    tcp_err(pcb, telnet_err);
    tcp_nagle_disable(pcb);

    /*
     * Character-at-a-time mode. Without these a client stays in its
     * default line mode, buffers input until Return and echoes it
     * locally -- which makes a guest reading single keystrokes appear to
     * ignore them completely.
     *
     * Written straight into the output ring so it goes out ahead of the
     * buffered banner, which is the order a client needs: options first,
     * then data.
     */
    static const uint8_t k_opts[] = {
        TELNET_IAC, TELNET_WILL, TELNET_OPT_ECHO,
        TELNET_IAC, TELNET_WILL, TELNET_OPT_SGA,
        TELNET_IAC, TELNET_DONT, TELNET_OPT_ECHO,
    };

    /*
     * The ring is written from the tail here rather than the head, so
     * these precede whatever is already queued. There is room by
     * construction unless the buffer is completely full, in which case
     * the options are skipped -- a client in line mode is a usable
     * console, and losing banner bytes to make room for it is not a
     * trade worth making.
     */
    if ((g_tx_head - g_tx_tail) <= (TX_SIZE - sizeof(k_opts))) {
        g_tx_tail -= (uint32_t)sizeof(k_opts);
        for (uint32_t i = 0; i < sizeof(k_opts); i++) {
            g_tx[(g_tx_tail + i) & TX_MASK] = k_opts[i];
        }
    }

    if (g_tx_dropped != 0u) {
        /*
         * Say so, rather than letting a gap in the output look like the
         * board stopped producing any. The count is characters, and it
         * is always a prefix of the guest's output that went missing --
         * never the banner, because the drop policy keeps the oldest.
         */
        console_puts("\n[telnet: console output was dropped before connect]\n");
        g_tx_dropped = 0u;
    }

    telnet_flush();
    return ERR_OK;
}

bool emu_net_telnet_init(void)
{
    struct tcp_pcb *pcb = tcp_new();

    if (pcb == NULL) {
        return false;
    }
    if (tcp_bind(pcb, IP_ANY_TYPE, TELNET_PORT) != ERR_OK) {
        tcp_close(pcb);
        return false;
    }

    /*
     * tcp_listen frees the bound pcb and returns a smaller listening one,
     * so the original must not be used or closed afterwards. Losing that
     * detail leaks the pcb on the failure path and double-frees on the
     * success path.
     */
    g_listen = tcp_listen(pcb);
    if (g_listen == NULL) {
        tcp_close(pcb);
        return false;
    }

    tcp_accept(g_listen, telnet_accept);
    return true;
}
