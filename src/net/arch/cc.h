/* SPDX-License-Identifier: Apache-2.0 */
/*
 * cc.h - lwIP's compiler and platform abstraction, for this firmware.
 *
 * lwIP includes <arch/cc.h> from an include path the port supplies, and
 * ST's middleware ships one under system/arch/. It is not used here: it
 * routes LWIP_PLATFORM_ASSERT through printf and defines LWIP_RAND as
 * rand(), and the ARM toolchain file records that the only libc symbols
 * this firmware pulls in are memcpy, memset and __libc_init_array --
 * deliberately, because --specs=nano.specs is not present in Debian's
 * arm-none-eabi packaging and requiring it makes the build unportable.
 * One printf would put a few KiB of formatting code in flash and a heap
 * dependency in a firmware that has no heap, to serve an assertion that
 * is compiled out.
 *
 * So this file is the whole port layer that is not sio or the netif.
 */
#ifndef EMU_LWIP_ARCH_CC_H
#define EMU_LWIP_ARCH_CC_H

#include <stddef.h>
#include <stdint.h>

/*
 * Every ARM Cortex-M this builds for is little-endian, and lwIP wants the
 * BSD spelling. Getting it wrong is not a compile error -- it byte-swaps
 * every header field, so the board answers nothing and looks unplugged.
 */
#ifndef LITTLE_ENDIAN
#define LITTLE_ENDIAN 1234
#endif
#ifndef BIG_ENDIAN
#define BIG_ENDIAN    4321
#endif
#ifndef BYTE_ORDER
#define BYTE_ORDER LITTLE_ENDIAN
#endif

/* newlib has an errno; lwIP need not define its own. */
#define LWIP_ERRNO_INCLUDE <errno.h>

typedef int sys_prot_t;

#define PACK_STRUCT_BEGIN
#define PACK_STRUCT_STRUCT __attribute__((packed))
#define PACK_STRUCT_END
#define PACK_STRUCT_FIELD(x) x

/*
 * Diagnostics go nowhere unless LWIP_DEBUG is on, and it is off in
 * lwipopts.h. Defining this as a no-op rather than leaving it to lwIP's
 * default is what keeps the default (printf) from being reached.
 */
#define LWIP_PLATFORM_DIAG(x) do { } while (0)

/*
 * An assertion failure inside the stack is a bug in the port, not a
 * condition to recover from, so it stops with the message on whatever
 * console is currently live -- which by then is usually telnet, since
 * SLIP owns the UART. Implemented in net.c.
 */
void emu_net_assert_fail(const char *msg, const char *file, int line);
#define LWIP_PLATFORM_ASSERT(x) emu_net_assert_fail((x), __FILE__, __LINE__)

/*
 * Used for the TCP initial sequence number and the TFTP transfer
 * identifier. The cycle counter is a better source here than rand()
 * would be -- it is genuinely unpredictable across resets, where a
 * freshly seeded PRNG on a deterministic firmware is not.
 */
uint32_t emu_net_rand(void);
#define LWIP_RAND() emu_net_rand()

#endif /* EMU_LWIP_ARCH_CC_H */
