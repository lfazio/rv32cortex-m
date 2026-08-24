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

#include "emu_console.h" /* for the platform's board.h chain */
#include "emu/emu_cache.h"

#include "board.h"
#include "board_cmsis.h" /* this part's SCB_* and __*CACHE_PRESENT */

/*
 * **The guard, not the include, is the fix.**
 *
 * Both functions below are written as `#if defined(__DCACHE_PRESENT)`, so
 * a translation unit that has not seen CMSIS compiles them to empty
 * bodies -- silently, because -Wundef does not inspect `defined()` and an
 * empty function is not an error. That is what this file did for its
 * whole life: it includes emu_console.h and board.h, neither of which
 * reaches a device header, so the guest's cbo.* maintenance was a no-op
 * on the M7 as well as the M4.
 *
 * __CORTEX_M is defined by every core_cmN.h and is therefore a test of
 * "did a device header get here", which is a different question from "has
 * this part got a cache" -- the M4 answers no to the second and must
 * still compile. Testing the cache macros instead would fail on the F446
 * for being right.
 */
#if !defined(__CORTEX_M)
#error "board_cmsis.h did not bring in a CMSIS core header: the cache \
maintenance below would compile to nothing without saying so"
#endif

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

/* ------------------------------------------------------------------ */
/* The JIT's code buffer                                               */
/* ------------------------------------------------------------------ */

/*
 * The platform half of t2_sync_code: the JIT's code buffer is ordinary
 * .bss, so a translated block is written through the D-cache and then
 * branched to through the instruction side.
 *
 * This was **missing entirely** on the F746 for a while -- the IR backend
 * inherited `.sync = NULL` from the x86 host, whose caches are coherent
 * with instruction fetch. The failure it produces is not a wrong answer,
 * it is executing whatever occupied those addresses before, and it needs
 * the buffer to be *reused* before it can fire, so a short run looks
 * healthy.
 *
 * Clean to the point of unification, then invalidate the instruction
 * lines: the data has to reach a level the I-side can see before the
 * stale I-lines are dropped, or the invalidate races the clean.
 *
 * **One copy for all three STM32s, and it is the device header that
 * decides.** __DCACHE_PRESENT and __ICACHE_PRESENT come from CMSIS, which
 * knows the part -- so a Cortex-M4 compiles this to an empty function and
 * a Cortex-M7 or Cortex-M55 gets the real maintenance, with no #if on a
 * platform name anywhere. It lived in stm32f746/board.c, where it was
 * already written in exactly this guarded form; the M55 port needed it
 * too and would have got a silent no-op by copying the F446's comment
 * about not needing one.
 *
 * Note what this is *not*: board_cache_ops above is the guest's
 * cbo.clean/inval/flush onto this part's D-cache, which is different
 * memory for a different reason. Confusing the two maintains a D-cache
 * for bytes the instruction side is about to fetch.
 */
void board_sync_icache(const void *addr, uint32_t len)
{
#if defined(__DCACHE_PRESENT) && (__DCACHE_PRESENT == 1U)
    SCB_CleanDCache_by_Addr((uint32_t *)(uintptr_t)addr, (int32_t)len);
#else
    (void)addr;
    (void)len;
#endif
#if defined(__ICACHE_PRESENT) && (__ICACHE_PRESENT == 1U)
    SCB_InvalidateICache_by_Addr((void *)(uintptr_t)addr, (int32_t)len);
#endif
}
