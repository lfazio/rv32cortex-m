/* SPDX-License-Identifier: Apache-2.0 */
/*
 * lwIP's raw TCP API, as much of it as src/net/net_telnet.c uses; see
 * ../README.md. Signatures as lwIP 2.2.1's lwip/tcp.h, except that
 * tcp_sndbuf and tcp_nagle_disable, macros there reading the pcb, are
 * functions here, and an address is a `const void *`.
 */
#ifndef FUZZ_LWIP_TCP_H
#define FUZZ_LWIP_TCP_H
#include "lwip/arch.h"
#include "lwip/pbuf.h"

#define ERR_MEM -1
#define ERR_VAL -6
#define ERR_ABRT -13
#define TCP_WRITE_FLAG_COPY 0x01u
#define IP_ANY_TYPE ((const void *)0)

struct tcp_pcb;

typedef err_t (*tcp_recv_fn)(void *arg, struct tcp_pcb *tpcb, struct pbuf *p,
                             err_t err);
typedef err_t (*tcp_sent_fn)(void *arg, struct tcp_pcb *tpcb, u16_t len);
typedef void (*tcp_err_fn)(void *arg, err_t err);
typedef err_t (*tcp_accept_fn)(void *arg, struct tcp_pcb *newpcb, err_t err);

struct tcp_pcb *tcp_new(void);
err_t tcp_bind(struct tcp_pcb *pcb, const void *ipaddr, u16_t port);
struct tcp_pcb *tcp_listen(struct tcp_pcb *pcb);
void tcp_accept(struct tcp_pcb *pcb, tcp_accept_fn accept);
void tcp_arg(struct tcp_pcb *pcb, void *arg);
void tcp_recv(struct tcp_pcb *pcb, tcp_recv_fn recv);
void tcp_sent(struct tcp_pcb *pcb, tcp_sent_fn sent);
void tcp_err(struct tcp_pcb *pcb, tcp_err_fn err);
err_t tcp_close(struct tcp_pcb *pcb);
void tcp_recved(struct tcp_pcb *pcb, u16_t len);
err_t tcp_write(struct tcp_pcb *pcb, const void *dataptr, u16_t len,
                u8_t apiflags);
err_t tcp_output(struct tcp_pcb *pcb);
u16_t tcp_sndbuf(struct tcp_pcb *pcb);
void tcp_nagle_disable(struct tcp_pcb *pcb);
u8_t pbuf_free(struct pbuf *p);

#endif
