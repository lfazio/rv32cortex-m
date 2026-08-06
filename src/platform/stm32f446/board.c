/* SPDX-License-Identifier: Apache-2.0 */
/*
 * board.c - Nucleo-F446RE bring-up.
 *
 * STM32F446RE: Cortex-M4F at 180 MHz, 512 KiB flash, 128 KiB of RAM.
 * References are RM0390 for the part and UM1724 for the board.
 *
 * Everything here is board fact rather than emulator policy, which is what
 * makes it the file to read when porting. Comparing it against the F746's
 * copy is the shortest description of what changes between the two.
 */

#include "board.h"

#include "stm32f4xx_hal.h"

static UART_HandleTypeDef g_console;

static void Error_Handler(void)
{
    __disable_irq();
    for (;;) {
        /* A failure this early cannot be reported; halt so a debugger can
         * see where we stopped. */
    }
}

/* ------------------------------------------------------------------ */
/* Clock tree                                                          */
/* ------------------------------------------------------------------ */

/*
 * 180 MHz from the internal 16 MHz HSI.
 *
 * HSI rather than HSE because this board ships without a populated
 * crystal, and the ST-LINK MCO path depends on solder-bridge options that
 * vary between board revisions; HSI works on an unmodified board.
 *
 *   HSI 16 MHz / PLLM 8 = 2 MHz -> x PLLN 180 = 360 MHz -> / PLLP 2 = 180
 */
static void clock_init(void)
{
    RCC_OscInitTypeDef osc = { 0 };
    RCC_ClkInitTypeDef clk = { 0 };

    __HAL_RCC_PWR_CLK_ENABLE();
    __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

    osc.OscillatorType = RCC_OSCILLATORTYPE_HSI;
    osc.HSIState = RCC_HSI_ON;
    osc.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
    osc.PLL.PLLState = RCC_PLL_ON;
    osc.PLL.PLLSource = RCC_PLLSOURCE_HSI;
    osc.PLL.PLLM = 8;
    osc.PLL.PLLN = 180;
    osc.PLL.PLLP = RCC_PLLP_DIV2;
    osc.PLL.PLLQ = 4;
    osc.PLL.PLLR = 2;
    if (HAL_RCC_OscConfig(&osc) != HAL_OK) {
        Error_Handler();
    }

    /* Over-drive is mandatory above 168 MHz on this part. */
    if (HAL_PWREx_EnableOverDrive() != HAL_OK) {
        Error_Handler();
    }

    clk.ClockType = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK |
                    RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
    clk.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
    clk.AHBCLKDivider = RCC_SYSCLK_DIV1;
    clk.APB1CLKDivider = RCC_HCLK_DIV4;    /* 45 MHz, APB1 max */
    clk.APB2CLKDivider = RCC_HCLK_DIV2;    /* 90 MHz, APB2 max */
    if (HAL_RCC_ClockConfig(&clk, FLASH_LATENCY_5) != HAL_OK) {
        Error_Handler();
    }

    /*
     * No peripheral kernel clock selection here, unlike the F7: on this
     * family a USART simply runs from its APB clock. That is one of the
     * differences the split makes visible.
     */
}

/* ------------------------------------------------------------------ */
/* Console                                                             */
/* ------------------------------------------------------------------ */

static void console_init(void)
{
    /* USART2, which is where a Nucleo-64 wires the ST-LINK VCP. */
    g_console.Instance = USART2;
    g_console.Init.BaudRate = 921600;
    g_console.Init.WordLength = UART_WORDLENGTH_8B;
    g_console.Init.StopBits = UART_STOPBITS_1;
    g_console.Init.Parity = UART_PARITY_NONE;
    g_console.Init.Mode = UART_MODE_TX_RX;
    g_console.Init.HwFlowCtl = UART_HWCONTROL_NONE;
    g_console.Init.OverSampling = UART_OVERSAMPLING_16;
    if (HAL_UART_Init(&g_console) != HAL_OK) {
        Error_Handler();
    }
}

/* Called by HAL_UART_Init. */
void HAL_UART_MspInit(UART_HandleTypeDef *huart)
{
    GPIO_InitTypeDef gpio = { 0 };

    if (huart->Instance != USART2) {
        return;
    }

    __HAL_RCC_USART2_CLK_ENABLE();
    __HAL_RCC_GPIOA_CLK_ENABLE();

    /* PA2 = USART2_TX, PA3 = USART2_RX, both AF7. */
    gpio.Pin = GPIO_PIN_2 | GPIO_PIN_3;
    gpio.Mode = GPIO_MODE_AF_PP;
    gpio.Pull = GPIO_PULLUP;
    gpio.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    gpio.Alternate = GPIO_AF7_USART2;
    HAL_GPIO_Init(GPIOA, &gpio);
}

void board_console_putc(uint8_t c)
{
    /* Blocking, with a generous timeout: output is for humans, and losing
     * a byte is worse than stalling briefly. */
    HAL_UART_Transmit(&g_console, &c, 1u, 100u);
}

int board_console_getc(void)
{
    /* DR, one register for both directions -- the F7 splits it into
     * RDR and TDR. */
    if (__HAL_UART_GET_FLAG(&g_console, UART_FLAG_RXNE)) {
        return (int)(g_console.Instance->DR & 0xFFu);
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* Cycle counter                                                       */
/* ------------------------------------------------------------------ */

static void dwt_init(void)
{
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;

    /*
     * No DWT->LAR write, and that is not an omission. The software lock is
     * optional in ARMv7-M; this part does not implement it, so the write
     * would be harmless but says nothing. The Cortex-M7 does implement it,
     * and without the unlock its cycle counter never starts -- see the
     * F746's copy of this file.
     */
    DWT->CYCCNT = 0u;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

uint32_t board_cycles(void)
{
    return DWT->CYCCNT;
}

/* ------------------------------------------------------------------ */

const char *board_name(void) { return "Cortex-M4"; }

uint32_t board_clock_hz(void) { return SystemCoreClock; }

void board_init(void)
{
    /*
     * No caches to enable: this part has only the ART flash accelerator,
     * which is transparent. That is why RV_ARM_HAS_CACHES is left off here
     * and the JIT's instruction-cache maintenance compiles out.
     */
    HAL_Init();
    clock_init();
    console_init();
    dwt_init();
}
