/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * stm32f4xx_hal_conf.h - STM32Cube HAL configuration.
 *
 * Derived from ST's stm32f4xx_hal_conf_template.h, trimmed to the modules
 * this firmware actually uses. Every unlisted HAL_*_MODULE_ENABLED is left
 * undefined so the corresponding driver is not compiled in at all, which
 * matters on a 512 KiB part that also has to hold the emulator.
 */
#ifndef STM32F4xx_HAL_CONF_H
#define STM32F4xx_HAL_CONF_H

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
#define HAL_PWR_MODULE_ENABLED       /* over-drive mode for 180 MHz */
#define HAL_RCC_MODULE_ENABLED
#define HAL_UART_MODULE_ENABLED

/* ---------------------------------------------------------------- */
/* Oscillators                                                       */
/* ---------------------------------------------------------------- */

/*
 * The Nucleo-F446RE has no populated HSE crystal by default; the clock
 * tree is built from the internal 16 MHz HSI, so HSE_VALUE only matters if
 * the board is rewired.
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
#define INSTRUCTION_CACHE_ENABLE     1U
#define DATA_CACHE_ENABLE            1U
#define USE_SPI_CRC                  0U

/* Static callbacks: the dynamic registration machinery costs RAM we would
 * rather give to the guest. */
#define USE_HAL_UART_REGISTER_CALLBACKS 0U

/* ---------------------------------------------------------------- */
/* Includes                                                          */
/* ---------------------------------------------------------------- */

#ifdef HAL_RCC_MODULE_ENABLED
#include "stm32f4xx_hal_rcc.h"
#endif
#ifdef HAL_GPIO_MODULE_ENABLED
#include "stm32f4xx_hal_gpio.h"
#endif
#ifdef HAL_EXTI_MODULE_ENABLED
#include "stm32f4xx_hal_exti.h"
#endif
#ifdef HAL_DMA_MODULE_ENABLED
#include "stm32f4xx_hal_dma.h"
#endif
#ifdef HAL_CORTEX_MODULE_ENABLED
#include "stm32f4xx_hal_cortex.h"
#endif
#ifdef HAL_FLASH_MODULE_ENABLED
#include "stm32f4xx_hal_flash.h"
#endif
#ifdef HAL_PWR_MODULE_ENABLED
#include "stm32f4xx_hal_pwr.h"
#endif
#ifdef HAL_UART_MODULE_ENABLED
#include "stm32f4xx_hal_uart.h"
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

#endif /* STM32F4xx_HAL_CONF_H */
