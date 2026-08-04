/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * stm32f7xx_hal_conf.h - STM32Cube HAL configuration.
 *
 * Derived from ST's stm32f7xx_hal_conf_template.h, trimmed to the modules
 * this firmware actually uses. Every unlisted HAL_*_MODULE_ENABLED is left
 * undefined so the corresponding driver is not compiled in at all.
 *
 * This is *not* the F4 file with the names changed. The system
 * configuration section differs where the parts differ, and the difference
 * is silent: F4 has ART with separate instruction and data caches, spelled
 * INSTRUCTION_CACHE_ENABLE and DATA_CACHE_ENABLE, while F7 has a single
 * ART accelerator spelled ART_ACCELERATOR_ENABLE. HAL_Init consults
 * whichever name its own family uses, so carrying the F4 spelling over
 * leaves the accelerator switched off and costs flash wait states on every
 * fetch that misses the M7's instruction cache, with nothing to indicate
 * it. The core's own caches are a separate matter and are enabled directly
 * in main().
 */
#ifndef STM32F7xx_HAL_CONF_H
#define STM32F7xx_HAL_CONF_H

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------- */
/* Module selection                                                  */
/* ---------------------------------------------------------------- */

#define HAL_MODULE_ENABLED
#define HAL_CORTEX_MODULE_ENABLED
#define HAL_DMA_MODULE_ENABLED       /* UART driver references DMA types */
#define HAL_EXTI_MODULE_ENABLED
#define HAL_FLASH_MODULE_ENABLED
#define HAL_GPIO_MODULE_ENABLED
#define HAL_PWR_MODULE_ENABLED       /* over-drive mode for 216 MHz */
#define HAL_RCC_MODULE_ENABLED
#define HAL_UART_MODULE_ENABLED

/* ---------------------------------------------------------------- */
/* Oscillators                                                       */
/* ---------------------------------------------------------------- */

/*
 * The Nucleo-F746ZG ships with X3 unpopulated, so HSE comes from the
 * ST-LINK's 8 MHz MCO and only when the solder bridges select it. The
 * clock tree here is built from the internal 16 MHz HSI, so this value
 * matters only if the board is rewired -- but ST's template defaults it to
 * 25 MHz, which is right for their evaluation boards and wrong for this
 * one, so it is set explicitly rather than left to default.
 */
#if !defined(HSE_VALUE)
#define HSE_VALUE            8000000U
#endif

#if !defined(HSE_STARTUP_TIMEOUT)
#define HSE_STARTUP_TIMEOUT  100U
#endif

#if !defined(HSI_VALUE)
#define HSI_VALUE            16000000U
#endif

#if !defined(LSI_VALUE)
#define LSI_VALUE            32000U
#endif

#if !defined(LSE_VALUE)
#define LSE_VALUE            32768U
#endif

#if !defined(LSE_STARTUP_TIMEOUT)
#define LSE_STARTUP_TIMEOUT  5000U
#endif

#if !defined(EXTERNAL_CLOCK_VALUE)
#define EXTERNAL_CLOCK_VALUE 12288000U
#endif

/* ---------------------------------------------------------------- */
/* System configuration                                              */
/* ---------------------------------------------------------------- */

#define VDD_VALUE                    3300U
#define TICK_INT_PRIORITY            0x0FU
#define USE_RTOS                     0U
#define PREFETCH_ENABLE              1U

/*
 * The F7 flash accelerator. One bit, unlike the F4's pair, and the reason
 * this file cannot be the F4 one renamed.
 */
#define ART_ACCELERATOR_ENABLE       1U

#define USE_SPI_CRC                  0U

/* Static callbacks: the dynamic registration machinery costs RAM we would
 * rather give to the guest. */
#define USE_HAL_UART_REGISTER_CALLBACKS 0U

/* ---------------------------------------------------------------- */
/* Includes                                                          */
/* ---------------------------------------------------------------- */

#ifdef HAL_RCC_MODULE_ENABLED
#include "stm32f7xx_hal_rcc.h"
#endif
#ifdef HAL_GPIO_MODULE_ENABLED
#include "stm32f7xx_hal_gpio.h"
#endif
#ifdef HAL_EXTI_MODULE_ENABLED
#include "stm32f7xx_hal_exti.h"
#endif
#ifdef HAL_DMA_MODULE_ENABLED
#include "stm32f7xx_hal_dma.h"
#endif
#ifdef HAL_CORTEX_MODULE_ENABLED
#include "stm32f7xx_hal_cortex.h"
#endif
#ifdef HAL_FLASH_MODULE_ENABLED
#include "stm32f7xx_hal_flash.h"
#endif
#ifdef HAL_PWR_MODULE_ENABLED
#include "stm32f7xx_hal_pwr.h"
#endif
#ifdef HAL_UART_MODULE_ENABLED
#include "stm32f7xx_hal_uart.h"
#endif

/* ---------------------------------------------------------------- */
/* Assertions                                                        */
/* ---------------------------------------------------------------- */

#ifdef USE_FULL_ASSERT
#define assert_param(expr) ((expr) ? (void)0U : assert_failed((uint8_t *)__FILE__, __LINE__))
void assert_failed(uint8_t *file, uint32_t line);
#else
#define assert_param(expr) ((void)0U)
#endif

#ifdef __cplusplus
}
#endif

#endif /* STM32F7xx_HAL_CONF_H */
