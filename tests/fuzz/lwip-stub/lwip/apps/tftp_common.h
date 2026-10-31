/* SPDX-License-Identifier: Apache-2.0 */
/* lwIP 2.2.1's tftp_context, member for member; see ../../README.md. */
#ifndef FUZZ_LWIP_TFTP_COMMON_H
#define FUZZ_LWIP_TFTP_COMMON_H
#include "lwip/arch.h"
#include "lwip/pbuf.h"
struct tftp_context {
    void *(*open)(const char *fname, const char *mode, u8_t write);
    void (*close)(void *handle);
    int (*read)(void *handle, void *buf, int bytes);
    int (*write)(void *handle, struct pbuf *p);
    void (*error)(void *handle, int err, const char *msg, int size);
};
void tftp_cleanup(void);
#endif
