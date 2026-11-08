/* SPDX-License-Identifier: Apache-2.0 */
/*
 * fuzz_telnet.c - the telnet console (src/net/net_telnet.c) against a
 * reading of RFC 854, through the callbacks lwIP would make.
 *
 * The input is a script. Each step is an opcode byte and its operands:
 *
 *   0  a client connects (a new pcb; the old one, if any, is replaced)
 *   1  N bytes arrive, as a pbuf chain of segments of one size
 *   2  the guest reads N bytes from its UART
 *   3  the guest writes N bytes to its UART
 *   4  the stack polls with a send window of 4K bytes
 *   5  an acknowledgement frees a window of 4K bytes
 *   6  the connection fails (tcp_err)
 *   7  the client closes (a NULL pbuf)
 *   8  the guest writes one byte 64N times -- what fills the 4 KiB ring,
 *      which no input short enough to fuzz could spell out
 *
 * What is held:
 *
 *   - What the guest reads is the byte stream with telnet's commands
 *     removed, by RFC 854: IAC IAC is the data byte 255; WILL, WONT, DO
 *     and DONT take an option byte; every other command is two bytes.
 *     Subnegotiation's IAC SB takes an option byte and its payload is
 *     data, as net_telnet.c documents. A command split between two
 *     segments is still one command. A byte arriving with the 256-byte
 *     ring full is dropped.
 *   - What reaches tcp_write over a connection is, in order, the three
 *     options and then what the guest wrote -- whatever the window cut
 *     it into -- and never more than the window at the time.
 *
 * net_telnet.c is #included, as fuzz_upload.c includes net_tftp.c, so
 * that its statics can be reset per input; lwIP is stood in for by
 * lwip-stub/. Built only with -DEMU_FUZZ=ON.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/net/net_telnet.c"

/* ------------------------------------------------------------------ */
/* lwIP, as far as the console uses it                                 */
/* ------------------------------------------------------------------ */

struct tcp_pcb {
    int id;
};

static struct tcp_pcb g_pcbs[3]; /* [0] listening, [1] and [2] clients */
static unsigned g_next_client;
static struct tcp_pcb *g_open; /* the connection the console was given */
static tcp_accept_fn g_accept;
static tcp_recv_fn g_recv;
static tcp_sent_fn g_sent;
static tcp_err_fn g_err;
static u16_t g_window;

struct tcp_pcb *tcp_new(void)
{
    return &g_pcbs[0];
}

err_t tcp_bind(struct tcp_pcb *pcb, const void *ipaddr, u16_t port)
{
    (void)pcb;
    (void)ipaddr;
    (void)port;
    return ERR_OK;
}

struct tcp_pcb *tcp_listen(struct tcp_pcb *pcb)
{
    return pcb;
}

void tcp_accept(struct tcp_pcb *pcb, tcp_accept_fn accept)
{
    (void)pcb;
    g_accept = accept;
}

void tcp_arg(struct tcp_pcb *pcb, void *arg)
{
    (void)pcb;
    (void)arg;
}

void tcp_recv(struct tcp_pcb *pcb, tcp_recv_fn recv)
{
    (void)pcb;
    g_recv = recv;
}

void tcp_sent(struct tcp_pcb *pcb, tcp_sent_fn sent)
{
    (void)pcb;
    g_sent = sent;
}

void tcp_err(struct tcp_pcb *pcb, tcp_err_fn err)
{
    (void)pcb;
    g_err = err;
}

err_t tcp_close(struct tcp_pcb *pcb)
{
    if (pcb == g_open) {
        g_open = NULL;
    }
    return ERR_OK;
}

void tcp_recved(struct tcp_pcb *pcb, u16_t len)
{
    (void)pcb;
    (void)len;
}

u16_t tcp_sndbuf(struct tcp_pcb *pcb)
{
    (void)pcb;
    return g_window;
}

void tcp_nagle_disable(struct tcp_pcb *pcb)
{
    (void)pcb;
}

err_t tcp_output(struct tcp_pcb *pcb)
{
    (void)pcb;
    return ERR_OK;
}

u8_t pbuf_free(struct pbuf *p)
{
    (void)p; /* the chain is the input's, on the stack */
    return 1u;
}

/* ------------------------------------------------------------------ */
/* The reference                                                       */
/* ------------------------------------------------------------------ */

static void fail(const char *what, unsigned a, unsigned b)
{
    fprintf(stderr, "fuzz-telnet: %s (%u, %u)\n", what, a, b);
    abort();
}

/* The receive side: RFC 854's filter, and RX_SIZE's drop rule. */
enum { M_DATA, M_CMD, M_OPT };
static unsigned m_state;
static uint8_t m_rx[RX_SIZE];
static unsigned m_rx_n, m_rx_at;

static void m_deliver(uint8_t c)
{
    if (m_rx_n < RX_SIZE) {
        m_rx[(m_rx_at + m_rx_n++) % RX_SIZE] = c;
    }
}

static void m_rx_byte(uint8_t c)
{
    switch (m_state) {
    case M_CMD:
        if (c == TELNET_IAC) {
            m_deliver(c); /* IAC IAC: the data byte 255 */
            m_state = M_DATA;
        } else {
            m_state = (c >= TELNET_SB) ? M_OPT : M_DATA;
        }
        return;
    case M_OPT:
        m_state = M_DATA;
        return;
    default:
        break;
    }
    if (c == TELNET_IAC) {
        m_state = M_CMD;
    } else {
        m_deliver(c);
    }
}

/*
 * The send side: the bytes queued for the client in the order it must
 * see them, kept as the console keeps them -- what the guest wrote, at
 * most TX_SIZE of them, new bytes dropped when full -- with the options
 * put in front on connect when they fit.
 */
static uint8_t m_tx[TX_SIZE + 64u];
static unsigned m_tx_n;
static unsigned m_tx_dropped;

static void m_tx_put(uint8_t c)
{
    if (m_tx_n >= TX_SIZE) {
        m_tx_dropped++;
        return;
    }
    m_tx[m_tx_n++] = c;
}

static void m_connect(void)
{
    static const uint8_t opts[9] = {255, 251, 1, 255, 251, 3, 255, 254, 1};

    m_state = M_DATA;
    if (m_tx_n <= TX_SIZE - sizeof(opts)) {
        memmove(m_tx + sizeof(opts), m_tx, m_tx_n);
        memcpy(m_tx, opts, sizeof(opts));
        m_tx_n += (unsigned)sizeof(opts);
    }
    if (m_tx_dropped != 0u) {
        const char *s =
            "\n[telnet: console output was dropped before connect]\n";

        while (*s != '\0') {
            m_tx_put((uint8_t)*s++);
        }
        m_tx_dropped = 0u;
    }
}

err_t tcp_write(struct tcp_pcb *pcb, const void *dataptr, u16_t len,
                u8_t apiflags)
{
    const uint8_t *const p = (const uint8_t *)dataptr;

    (void)apiflags;
    if (pcb != g_open) {
        fail("a write on a connection the console was not given", 0u, 0u);
    }
    if (len == 0u || len > g_window) {
        fail("a write the window does not allow", len, g_window);
    }
    if (len > m_tx_n) {
        fail("more written than was queued", len, m_tx_n);
    }
    for (unsigned i = 0; i < len; i++) {
        if (p[i] != m_tx[i]) {
            fail("a byte written out of order, at", i, p[i]);
        }
    }
    memmove(m_tx, m_tx + len, m_tx_n - len);
    m_tx_n -= len;
    g_window = (u16_t)(g_window - len);
    return ERR_OK;
}

/* ------------------------------------------------------------------ */
/* The script                                                          */
/* ------------------------------------------------------------------ */

static void reset(void)
{
    g_listen = NULL;
    g_conn = NULL;
    g_tx_head = g_tx_tail = 0u;
    g_tx_dropped = 0u;
    g_rx_head = g_rx_tail = 0u;
    g_iac_state = IAC_NONE;
    g_open = NULL;
    g_next_client = 0u;
    g_accept = NULL;
    g_recv = NULL;
    g_sent = NULL;
    g_err = NULL;
    g_window = 0u;
    m_state = M_DATA;
    m_rx_n = m_rx_at = 0u;
    m_tx_n = 0u;
    m_tx_dropped = 0u;
    if (!emu_net_telnet_init() || g_accept == NULL) {
        fail("the console did not listen", 0u, 0u);
    }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    size_t at = 0u;

#define NEXT() ((at < size) ? data[at++] : 0u)

    reset();
    while (at < size) {
        const unsigned op = NEXT() % 9u;

        switch (op) {
        case 0: { /* connect */
            struct tcp_pcb *const pcb = &g_pcbs[1u + (g_next_client++ & 1u)];

            g_open = pcb;
            g_window = 0u; /* so the options wait for a poll */
            m_connect();
            if (g_accept(NULL, pcb, ERR_OK) != ERR_OK) {
                fail("a connection refused", 0u, 0u);
            }
            break;
        }
        case 1: { /* bytes arrive */
            const unsigned n = NEXT();
            const unsigned seg = (NEXT() % 32u) + 1u;
            struct pbuf chain[256];
            unsigned k = 0u;

            if (n > size - at) {
                at = size; /* a chunk the input cannot finish */
                break;
            }
            if (g_open == NULL) {
                at += n; /* nobody connected to send it */
                break;
            }
            for (unsigned off = 0; off < n; off += seg, k++) {
                const unsigned len = (n - off < seg) ? n - off : seg;

                chain[k].next = NULL;
                chain[k].payload = (void *)(uintptr_t)(data + at + off);
                chain[k].len = (u16_t)len;
                chain[k].tot_len = (u16_t)(n - off);
                if (k != 0u) {
                    chain[k - 1u].next = &chain[k];
                }
            }
            for (unsigned i = 0; i < n; i++) {
                m_rx_byte(data[at + i]);
            }
            at += n;
            if (k != 0u) {
                (void)g_recv(NULL, g_open, &chain[0], ERR_OK);
            }
            break;
        }
        case 2: { /* the guest reads */
            const unsigned n = NEXT();

            for (unsigned i = 0; i < n; i++) {
                const int got = emu_net_console_getc();
                const int want =
                    (m_rx_n != 0u) ? (int)m_rx[m_rx_at % RX_SIZE] : -1;

                if (got != want) {
                    fail("the guest read the wrong byte (got, want)",
                         (unsigned)got, (unsigned)want);
                }
                if (m_rx_n != 0u) {
                    m_rx_at++;
                    m_rx_n--;
                }
            }
            break;
        }
        case 3: { /* the guest writes */
            const unsigned n = NEXT();

            for (unsigned i = 0; i < n && at < size; i++) {
                const uint8_t c = (uint8_t)NEXT();

                emu_net_console_putc(c);
                m_tx_put(c);
            }
            break;
        }
        case 8: { /* the guest writes one byte many times */
            const unsigned n = NEXT() * 64u;
            const uint8_t c = (uint8_t)NEXT();

            for (unsigned i = 0; i < n; i++) {
                emu_net_console_putc(c);
                m_tx_put(c);
            }
            break;
        }
        case 4: /* a poll */
            g_window = (u16_t)(NEXT() * 4u);
            emu_net_telnet_poll();
            break;
        case 5: /* an acknowledgement */
            g_window = (u16_t)(NEXT() * 4u);
            if (g_open != NULL && g_sent != NULL) {
                (void)g_sent(NULL, g_open, 0u);
            }
            break;
        case 6: /* the connection fails: the pcb is gone */
            if (g_open != NULL) {
                g_open = NULL;
                m_state = M_DATA;
                g_err(NULL, ERR_ABRT);
            }
            break;
        case 7: /* the client closes */
            if (g_open != NULL) {
                m_state = M_DATA;
                (void)g_recv(NULL, g_open, NULL, ERR_OK);
                if (g_open != NULL) {
                    fail("a close left the connection open", 0u, 0u);
                }
            }
            break;
        }
    }
#undef NEXT
    return 0;
}
