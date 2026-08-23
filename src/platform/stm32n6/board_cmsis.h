/* SPDX-License-Identifier: Apache-2.0 */
/*
 * board_cmsis.h - this part's CMSIS device header, under a fixed name.
 *
 * One line, and it exists because the shared STM32 sources cannot spell
 * the family: src/platform/stm32/cache.c is compiled for all three parts
 * and needs SCB_* and __DCACHE_PRESENT, which live behind a header whose
 * name changes with the family. Each platform directory is first on the
 * include path, so this resolves to the right one.
 *
 * **What went wrong without it.** cache.c guarded its maintenance on
 * `#if defined(__DCACHE_PRESENT)` while including nothing that defines
 * it, so the guest's cbo.clean/inval/flush compiled to an empty function
 * on every part -- including the M7, where they are the whole point of
 * the file. Nothing failed and no warning fired: -Wundef does not look at
 * `defined()`. See the #error in cache.c, which is the part that stops
 * it happening again.
 *
 * This part: Cortex-M55, both caches.
 */
#ifndef BOARD_CMSIS_H_
#define BOARD_CMSIS_H_

#include "stm32n6xx.h"

#endif /* BOARD_CMSIS_H_ */
