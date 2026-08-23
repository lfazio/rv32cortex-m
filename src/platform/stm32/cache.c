/* SPDX-License-Identifier: Apache-2.0 */
/*
 * cache.c - guest cache maintenance onto this part's, for the STM32s.
 *
 * This is what makes a guest's cache-block operations mean something:
 * RISC-V's `cbo.clean`/`inval`/`flush` and their equivalents are handed
 * the *host* address backing the guest block, so a guest driver cleaning
 * a DMA buffer cleans the very ARM cache lines that hold it.
 *
 * **Not common, and it never was.** This lived in src/platform/common/
 * under the name emu_arm_cache.c, which made "shared between platforms"
 * and "shared between two ARM parts" the same thing. They are not: a host
 * has no guest-visible cache to maintain and answers these by doing
 * nothing, and a third architecture would have neither this code nor a
 * reason to compile it out. What is shared is the *question*, and that is
 * emu_cache_ops_t in the frontend contract.
 *
 * Identical between the F446 and the F746, which is what this directory
 * is for. The only thing that varies is whether the part has a D-cache,
 * and CMSIS already answers that with __DCACHE_PRESENT from the device
 * header: a Cortex-M4 compiles this to nothing and a Cortex-M7 gets the
 * real maintenance.
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

static void board_cache_maint(void *ctx, void *host, uint32_t len,
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

const emu_cache_ops_t board_cache_ops = {
    .maint = board_cache_maint,
    .ctx = NULL,
};
