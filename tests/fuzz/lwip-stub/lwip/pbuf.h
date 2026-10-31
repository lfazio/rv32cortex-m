/* SPDX-License-Identifier: Apache-2.0 */
/* lwIP's pbuf, the members net_tftp.c reads, in lwIP's order. */
#ifndef FUZZ_LWIP_PBUF_H
#define FUZZ_LWIP_PBUF_H
#include "lwip/arch.h"
struct pbuf {
    struct pbuf *next;
    void *payload;
    u16_t tot_len;
    u16_t len;
};
#endif
