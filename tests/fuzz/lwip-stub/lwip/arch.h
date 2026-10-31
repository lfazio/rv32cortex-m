/* SPDX-License-Identifier: Apache-2.0 */
/* lwIP's integer types and LWIP_UNUSED_ARG; see ../README.md. */
#ifndef FUZZ_LWIP_ARCH_H
#define FUZZ_LWIP_ARCH_H
#include <stdint.h>
typedef uint8_t u8_t;
typedef uint16_t u16_t;
typedef uint32_t u32_t;
typedef int8_t err_t;
#define ERR_OK 0
#define LWIP_UNUSED_ARG(x) (void)(x)
#endif
