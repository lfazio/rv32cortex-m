/* SPDX-License-Identifier: Apache-2.0 */
/* lwIP's TFTP server entry point: the fuzz target keeps the context. */
#ifndef FUZZ_LWIP_TFTP_SERVER_H
#define FUZZ_LWIP_TFTP_SERVER_H
#include "lwip/apps/tftp_common.h"
err_t tftp_init_server(const struct tftp_context *ctx);
#endif
