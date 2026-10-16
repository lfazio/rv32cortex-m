/* SPDX-License-Identifier: Apache-2.0 */
/*
 * net_gdb.c - the gdb stub's TCP transport.
 *
 * Port 1234, which is what `target remote :1234` defaults to in every
 * tutorial, so an attach is one line:
 *
 *   riscv64-unknown-elf-gdb build/f746net/guest/hello.elf
 *   (gdb) target remote 192.168.7.2:1234
 *
 * This file is the only part that knows about lwIP. The protocol is in
 * src/emu/emu_gdb.c and the register map in the frontend, so a platform
 * with a serial line and no network can drive the same stub by calling
 * emu_gdb_rx() from wherever its bytes come from.
 *
 * One connection at a time, and a second is refused rather than queued:
 * two debuggers sharing one target is not a situation with a good
 * outcome, and refusing says so immediately.
 *
 * Sending is direct rather than buffered, unlike the telnet console
 * beside it. The console has to keep talking while nobody is listening
 * -- that is what its ring is for -- but every byte here is a reply to
 * something gdb just asked, so there is never output with no client, and
 * a queue would only add a place for a reply to sit while gdb waits for
 * it.
 */

#include "emu_net.h"

#include "lwip/tcp.h"

#include "emu/emu_gdb.h"

#ifndef EMU_GDB_PORT
#define EMU_GDB_PORT 1234
#endif

static struct tcp_pcb *g_listen;
static struct tcp_pcb *g_conn;
static emu_gdb_t g_gdb;
static bool g_ready;

/* ------------------------------------------------------------------ */

/*
 * Replies go through a ring, not straight to tcp_write. The stub sends
 * from two places -- from the stack's receive path, answering a packet,
 * and from emu_net_gdb_run() on the run loop's task, reporting a stop
 * -- and only the first is on the task that owns lwIP. The ring is
 * drained by emu_net_gdb_poll(), which the stack's poll calls after its
 * input, so a reply to a packet goes out in the same poll it arrived in.
 *
 * The protocol is synchronous and a reply is at most a packet, so the
 * ring does not fill; if it ever did, a reply would be cut, and gdb
 * retransmits a packet whose checksum fails.
 */
#define GDB_TX_SIZE 4096u
#define GDB_TX_MASK (GDB_TX_SIZE - 1u)

static uint8_t g_txq[GDB_TX_SIZE];
static uint32_t g_txq_head, g_txq_tail; /* free-running */

static void gdb_tx(void *ctx, const uint8_t *data, uint32_t len)
{
    (void)ctx;
    if (g_conn == NULL) {
        return;
    }
    for (uint32_t i = 0u; i < len; i++) {
        if ((g_txq_head - g_txq_tail) >= GDB_TX_SIZE) {
            return;
        }
        g_txq[g_txq_head++ & GDB_TX_MASK] = data[i];
    }
}

bool emu_net_gdb_pending(void)
{
    return g_txq_head != g_txq_tail;
}

void emu_net_gdb_poll(void)
{
    if (g_conn == NULL) {
        g_txq_tail = g_txq_head;
        return;
    }
    while (g_txq_head != g_txq_tail) {
        const u16_t space = tcp_sndbuf(g_conn);

        if (space == 0u) {
            break;
        }
        uint32_t n = g_txq_head - g_txq_tail;
        const uint32_t to_end = GDB_TX_SIZE - (g_txq_tail & GDB_TX_MASK);

        if (n > to_end) {
            n = to_end;
        }
        if (n > space) {
            n = space;
        }
        /*
         * TCP_WRITE_FLAG_COPY: the ring is written again before the
         * segment is acknowledged.
         */
        if (tcp_write(g_conn, &g_txq[g_txq_tail & GDB_TX_MASK], (u16_t)n,
                      TCP_WRITE_FLAG_COPY) != ERR_OK) {
            break;
        }
        g_txq_tail += n;
    }
    /*
     * Pushed at once. gdb is synchronous: it sends a packet and waits,
     * so a reply sitting in the send buffer waiting for more data to
     * fill a segment is a debugger that appears to hang.
     */
    (void)tcp_output(g_conn);
}

static void gdb_close(struct tcp_pcb *pcb)
{
    tcp_arg(pcb, NULL);
    tcp_recv(pcb, NULL);
    tcp_err(pcb, NULL);
    if (pcb == g_conn) {
        g_conn = NULL;
        /* Resume the guest. A target left stopped by a debugger that
         * went away is indistinguishable from a hung board. */
        emu_gdb_detach(&g_gdb);
    }
    (void)tcp_close(pcb);
}

static err_t gdb_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
    (void)arg;

    if (p == NULL || err != ERR_OK) {
        if (p != NULL) {
            pbuf_free(p);
        }
        gdb_close(pcb);
        return ERR_OK;
    }

    for (struct pbuf *q = p; q != NULL; q = q->next) {
        emu_gdb_rx(&g_gdb, (const uint8_t *)q->payload, q->len);
    }

    tcp_recved(pcb, p->tot_len);
    pbuf_free(p);
    return ERR_OK;
}

static void gdb_err(void *arg, err_t err)
{
    (void)arg;
    (void)err;
    /* The pcb is already freed by lwIP here; only drop our reference. */
    g_conn = NULL;
    emu_gdb_detach(&g_gdb);
}

static err_t gdb_accept(void *arg, struct tcp_pcb *pcb, err_t err)
{
    (void)arg;

    if (err != ERR_OK || pcb == NULL) {
        return ERR_VAL;
    }
    if (g_conn != NULL) {
        (void)tcp_close(pcb); /* one debugger at a time */
        return ERR_OK;
    }

    g_conn = pcb;
    tcp_recv(pcb, gdb_recv);
    tcp_err(pcb, gdb_err);
    /*
     * Nagle off. Every reply is small and every one is something gdb is
     * blocked waiting for, so coalescing trades the only thing that
     * matters here -- latency -- for bandwidth nobody is using.
     */
    tcp_nagle_disable(pcb);

    emu_gdb_attach(&g_gdb);
    return ERR_OK;
}

/* ------------------------------------------------------------------ */

bool emu_net_gdb_init(emu_core_t *core, const emu_gdb_target_t *target,
                      const emu_gdb_flash_ops_t *flash)
{
    struct tcp_pcb *pcb;

    if (target == NULL) {
        return false; /* frontend offers no gdb description */
    }

    emu_gdb_init(&g_gdb, core, target, gdb_tx, NULL);
    emu_gdb_set_flash(&g_gdb, flash);

    pcb = tcp_new();
    if (pcb == NULL) {
        return false;
    }
    if (tcp_bind(pcb, IP_ANY_TYPE, EMU_GDB_PORT) != ERR_OK) {
        (void)tcp_close(pcb);
        return false;
    }
    g_listen = tcp_listen(pcb);
    if (g_listen == NULL) {
        (void)tcp_close(pcb);
        return false;
    }
    tcp_accept(g_listen, gdb_accept);
    g_ready = true;
    return true;
}

bool emu_net_gdb_attached(void)
{
    return g_ready && emu_gdb_attached(&g_gdb);
}

uint32_t emu_net_gdb_run(uint32_t budget, uint32_t *retired)
{
    /* uint32_t, not emu_run_reason_t, so emu_net.h can declare this
     * without pulling the cpu contract into a header that a
     * network-less platform still parses. The caller casts back. */
    return (uint32_t)emu_gdb_run(&g_gdb, budget, retired);
}
