/* SPDX-License-Identifier: Apache-2.0 */
/* lwIP's clock: the fuzz target's own, which its input advances. */
#ifndef FUZZ_LWIP_SYS_H
#define FUZZ_LWIP_SYS_H
#include "lwip/arch.h"
u32_t sys_now(void);
#endif
