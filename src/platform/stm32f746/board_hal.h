/* SPDX-License-Identifier: Apache-2.0 */
/*
 * board_hal.h - this part's HAL header, under a fixed name.
 *
 * As board_cmsis.h is for the CMSIS one: src/platform/stm32/board.c is
 * compiled for all three parts and drives the RNG through the HAL for
 * Zkr's seed, behind a header whose name changes with the family. Each
 * platform directory is first on the include path, so this resolves to
 * the right one.
 *
 * This part: Cortex-M7, RM0385.
 */
#ifndef BOARD_HAL_H_
#define BOARD_HAL_H_

#include "stm32f7xx_hal.h"

#endif /* BOARD_HAL_H_ */
