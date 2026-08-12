/* SPDX-License-Identifier: Apache-2.0 */
/*
 * host_gdb.c - the gdb stub's transport on the host.
 *
 * The same stub the board runs: src/emu/emu_gdb.c is the protocol and
 * the frontend supplies the register map, so this file is only sockets.
 * It exists because debugging the stub itself on hardware costs a
 * reflash and a network round trip per experiment, and every one of
 * those answers exactly one question.
 *
 * It is also the only way to exercise the stub against the x86-64
 * backend. The stub is indifferent to which JIT lowering runs
 * underneath -- that is what "no ISA and no backend knowledge" in
 * emu_gdb.c buys -- but indifferent is a claim, and this is what tests
 * it.
 *
 * Non-blocking throughout: the run loop calls host_gdb_poll() between
 * slices exactly as the firmware calls emu_net_poll(), so nothing here
 * may wait. A blocking accept would hang a runner that nobody is
 * debugging, which is every run the test suites make.
 */

#include "emu/emu_gdb.h"

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

void host_gdb_poll(void);       /* used by host_gdb_wait below */

static int        g_listen = -1;
static int        g_conn = -1;
static emu_gdb_t  g_gdb;
static bool       g_ready;

static void set_nonblock(int fd)
{
    const int fl = fcntl(fd, F_GETFL, 0);

    if (fl >= 0) {
        (void)fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    }
}

static void host_gdb_tx(void *ctx, const uint8_t *data, uint32_t len)
{
    (void)ctx;

    if (g_conn < 0) {
        return;
    }
    /*
     * Written in a loop because a short write is legal and losing the
     * tail of a reply is indistinguishable, from gdb's side, from the
     * target having hung.
     */
    while (len > 0u) {
        const ssize_t n = send(g_conn, data, len, MSG_NOSIGNAL);

        if (n <= 0) {
            if (n < 0 && (errno == EINTR || errno == EAGAIN)) {
                continue;
            }
            (void)close(g_conn);
            g_conn = -1;
            emu_gdb_detach(&g_gdb);
            return;
        }
        data += n;
        len -= (uint32_t)n;
    }
}

bool host_gdb_start(emu_core_t *core, const emu_gdb_target_t *target,
                    int port)
{
    struct sockaddr_in a;
    int one = 1;

    if (target == NULL) {
        return false;
    }
    emu_gdb_init(&g_gdb, core, target, host_gdb_tx, NULL);

    g_listen = socket(AF_INET, SOCK_STREAM, 0);
    if (g_listen < 0) {
        return false;
    }
    (void)setsockopt(g_listen, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons((uint16_t)port);

    if (bind(g_listen, (struct sockaddr *)&a, sizeof(a)) != 0 ||
        listen(g_listen, 1) != 0) {
        (void)close(g_listen);
        g_listen = -1;
        return false;
    }
    set_nonblock(g_listen);
    g_ready = true;

    fprintf(stderr, "gdb: waiting on localhost:%d "
                    "(target remote :%d)\n", port, port);
    return true;
}

/*
 * Wait for the first client, once.
 *
 * Without this the guest has usually run to completion before a
 * debugger can type the connect command -- the whole program is
 * milliseconds. The board does not need it because a human is already
 * late by the time it boots.
 */
void host_gdb_wait(void)
{
    if (!g_ready || g_conn >= 0) {
        return;
    }
    fprintf(stderr, "gdb: waiting for a connection...\n");
    for (;;) {
        host_gdb_poll();
        if (g_conn >= 0) {
            return;
        }
        usleep(1000);
    }
}

void host_gdb_poll(void)
{
    uint8_t buf[4096];

    if (!g_ready) {
        return;
    }

    if (g_conn < 0) {
        const int fd = accept(g_listen, NULL, NULL);

        if (fd >= 0) {
            int one = 1;

            /* Nagle off: every reply is something gdb is blocked on. */
            (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
            set_nonblock(fd);
            g_conn = fd;
            emu_gdb_attach(&g_gdb);
        }
        return;
    }

    for (;;) {
        const ssize_t n = recv(g_conn, buf, sizeof(buf), 0);

        if (n > 0) {
            emu_gdb_rx(&g_gdb, buf, (uint32_t)n);
            continue;
        }
        if (n == 0) {                   /* client went away */
            (void)close(g_conn);
            g_conn = -1;
            emu_gdb_detach(&g_gdb);
        }
        return;                         /* EAGAIN, or closed */
    }
}

bool host_gdb_attached(void)
{
    return g_ready && emu_gdb_attached(&g_gdb);
}

uint32_t host_gdb_run(uint32_t budget, uint32_t *retired)
{
    return (uint32_t)emu_gdb_run(&g_gdb, budget, retired);
}
