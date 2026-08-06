/* SPDX-License-Identifier: Apache-2.0 */
/*
 * emu_cache.h - Platform cache maintenance.
 *
 * These map remarkably directly onto ARMv7-M cache maintenance, because
 * both architectures define the same three operations over a cache block
 * identified by an address:
 *
 *   RISC-V            ARMv7-M (CMSIS)                       Cortex-M7
 *   cbo.clean   ->    SCB_CleanDCache_by_Addr               yes
 *   cbo.inval   ->    SCB_InvalidateDCache_by_Addr          yes
 *   cbo.flush   ->    SCB_CleanInvalidateDCache_by_Addr     yes
 *   cbo.zero    ->    store zeros (no ARM instruction)      n/a
 *   prefetch.*  ->    PLD                                   hint only
 *
 * The guest address is translated to the host address that actually backs
 * it before the maintenance call, which is what makes this meaningful: a
 * guest cleaning a DMA buffer in guest RAM ends up cleaning the very ARM
 * cache lines that hold it, so a real DMA engine driven by a guest driver
 * sees coherent data.
 *
 * On a part without caches (Cortex-M0+, and the Cortex-M4 in the
 * STM32F446) the maintenance operations are architecturally permitted to
 * do nothing, and the platform supplies no-ops. cbo.zero still has to
 * write zeros everywhere, because that effect is architecturally visible.
 */
#ifndef EMU_CACHE_H
#define EMU_CACHE_H

#include "emu_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    EMU_CACHE_INVAL = 0,   /* discard the block, do not write it back */
    EMU_CACHE_CLEAN,       /* write the block back, keep it valid     */
    EMU_CACHE_FLUSH,       /* write back and discard                  */
} emu_cache_op_t;

/*
 * Platform cache maintenance. `host` points at the memory backing the
 * guest block and `len` is the frontend's block size. May be NULL on a system
 * without caches.
 */
typedef struct emu_cache_ops {
    void (*maint)(void *ctx, void *host, uint32_t len, emu_cache_op_t op);
    void *ctx;
} emu_cache_ops_t;

#ifdef __cplusplus
}
#endif

#endif /* EMU_CACHE_H */
