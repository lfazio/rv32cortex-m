/* SPDX-License-Identifier: Apache-2.0 */
/*
 * emu_arm_cache.c - guest cache maintenance onto ARMv7-M's, shared.
 *
 * This is what makes a guest's cache-block operations mean something:
 * RISC-V's `cbo.clean`/`inval`/`flush` and their equivalents are handed
 * the *host* address backing the guest block, so a guest driver cleaning
 * a DMA buffer cleans the very ARM cache lines that hold it.
 *
 * Identical on every ARM platform here and duplicated in two of them,
 * because the only thing that varies is whether the part has a D-cache
 * at all -- and CMSIS already answers that with __DCACHE_PRESENT, which
 * the device header defines. A Cortex-M4 compiles this to nothing; a
 * Cortex-M7 or M55 gets the real maintenance.
 *
 * Note what is *not* here: the JIT's own emitted code needs a clean to
 * the point of unification and an I-cache invalidate, which is a
 * different operation on different memory and lives in t2_sync_code and
 * board_sync_icache. Confusing the two would maintain a D-cache for
 * bytes the instruction side is about to fetch.
 */

#include "emu_console.h"        /* for the platform's board.h chain */
#include "emu/emu_cache.h"

#include "board.h"

static void arm_cache_maint(void *ctx, void *host, uint32_t len,
                            emu_cache_op_t op)
{
    (void)ctx;

#if defined(__DCACHE_PRESENT) && (__DCACHE_PRESENT == 1U)
    switch (op) {
    case EMU_CACHE_CLEAN:
        SCB_CleanDCache_by_Addr((uint32_t *)host, (int32_t)len);
        break;
    case EMU_CACHE_INVAL:
        SCB_InvalidateDCache_by_Addr((uint32_t *)host, (int32_t)len);
        break;
    case EMU_CACHE_FLUSH:
        SCB_CleanInvalidateDCache_by_Addr((uint32_t *)host, (int32_t)len);
        break;
    }
#else
    (void)host;
    (void)len;
    (void)op;
    /* No data cache on this part: nothing to maintain. */
#endif
}

const emu_cache_ops_t emu_arm_cache_ops = {
    .maint = arm_cache_maint,
    .ctx = NULL,
};
