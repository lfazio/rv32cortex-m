/* SPDX-License-Identifier: Apache-2.0 */
/*
 * board.c - Nucleo-F746ZG bring-up.
 *
 * STM32F746ZG: Cortex-M7 at 216 MHz, 1 MiB flash, 320 KiB of RAM in three
 * contiguous banks. References are RM0385 for the part and UM1974 for the
 * board.
 *
 * Everything here is board fact rather than emulator policy, which is what
 * makes it the file to read when porting.
 */

#include "board.h"

#include "stm32f7xx_hal.h"

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
 * 216 MHz from the internal 16 MHz HSI.
 *
 * HSI rather than HSE because X3 is unpopulated on this board: HSE would
 * come from the ST-LINK's 8 MHz MCO and only with the right solder
 * bridges, which vary by board revision. HSI works on an unmodified board.
 *
 *   HSI 16 MHz / PLLM 8 = 2 MHz -> x PLLN 216 = 432 MHz -> / PLLP 2 = 216
 *
 * PLLQ is 9 rather than the F446's 4 because the 48 MHz domain comes off
 * the same 432 MHz VCO here (432/9), not off a 360 MHz one.
 */
static void clock_init(void)
{
    RCC_OscInitTypeDef osc = { 0 };
    RCC_ClkInitTypeDef clk = { 0 };
    RCC_PeriphCLKInitTypeDef pclk = { 0 };

    __HAL_RCC_PWR_CLK_ENABLE();
    __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

    osc.OscillatorType = RCC_OSCILLATORTYPE_HSI;
    osc.HSIState = RCC_HSI_ON;
    osc.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
    osc.PLL.PLLState = RCC_PLL_ON;
    osc.PLL.PLLSource = RCC_PLLSOURCE_HSI;
    osc.PLL.PLLM = 8;
    osc.PLL.PLLN = 216;
    osc.PLL.PLLP = RCC_PLLP_DIV2;
    osc.PLL.PLLQ = 9;
    if (HAL_RCC_OscConfig(&osc) != HAL_OK) {
        Error_Handler();
    }

    /* Over-drive is mandatory above 180 MHz on this part. */
    if (HAL_PWREx_EnableOverDrive() != HAL_OK) {
        Error_Handler();
    }

    clk.ClockType = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK |
                    RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
    clk.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
    clk.AHBCLKDivider = RCC_SYSCLK_DIV1;
    clk.APB1CLKDivider = RCC_HCLK_DIV4;    /* 54 MHz, APB1 max  */
    clk.APB2CLKDivider = RCC_HCLK_DIV2;    /* 108 MHz, APB2 max */
    /* 7 wait states: 216 MHz at the 2.7-3.6 V range of RM0385 Table 7. */
    if (HAL_RCC_ClockConfig(&clk, FLASH_LATENCY_7) != HAL_OK) {
        Error_Handler();
    }

    /*
     * Peripheral kernel clocks, which the F4 clock tree does not have.
     *
     * On this family each USART selects its own source in DCKCFGR2 rather
     * than simply running from its APB clock, and the HAL computes the
     * baud-rate divisor from whichever source is selected. The reset value
     * is PCLK1, so the console would come up correctly without this -- and
     * would silently produce garbage the moment anything else touched
     * DCKCFGR2. Saying it costs one register write.
     */
    pclk.PeriphClockSelection = RCC_PERIPHCLK_USART3;
    pclk.Usart3ClockSelection = RCC_USART3CLKSOURCE_PCLK1;
    if (HAL_RCCEx_PeriphCLKConfig(&pclk) != HAL_OK) {
        Error_Handler();
    }
}

/* ------------------------------------------------------------------ */
/* Console                                                             */
/* ------------------------------------------------------------------ */

static void console_init(void)
{
    /*
     * USART3, not USART2. On a Nucleo-144 the ST-LINK virtual COM port is
     * wired to PD8/PD9 (UM1974, Table 9); USART2 goes to the Zio header
     * instead, so using it produces a board that runs and says nothing.
     */
    g_console.Instance = USART3;
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

    if (huart->Instance != USART3) {
        return;
    }

    __HAL_RCC_USART3_CLK_ENABLE();
    __HAL_RCC_GPIOD_CLK_ENABLE();

    /* PD8 = USART3_TX, PD9 = USART3_RX, both AF7. */
    gpio.Pin = GPIO_PIN_8 | GPIO_PIN_9;
    gpio.Mode = GPIO_MODE_AF_PP;
    gpio.Pull = GPIO_PULLUP;
    gpio.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    gpio.Alternate = GPIO_AF7_USART3;
    HAL_GPIO_Init(GPIOD, &gpio);
}

void board_console_putc(uint8_t c)
{
    /* Blocking, with a generous timeout: output is for humans, and losing
     * a byte is worse than stalling briefly. */
    HAL_UART_Transmit(&g_console, &c, 1u, 100u);
}

int board_console_getc(void)
{
    /*
     * RDR, not DR: this family splits the F4's single data register into
     * separate receive and transmit halves, so the F4 spelling does not
     * compile rather than quietly reading the wrong thing -- the one kind
     * of porting difference that costs nothing to find.
     */
    if (__HAL_UART_GET_FLAG(&g_console, UART_FLAG_RXNE)) {
        return (int)(g_console.Instance->RDR & 0xFFu);
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
     * Unlock the DWT before touching it. ARMv7-M gives the trace blocks an
     * optional software lock, and the Cortex-M7 implements it where the
     * Cortex-M4 does not: with the lock engaged, writes to CTRL and CYCCNT
     * are discarded silently, so the cycle counter never runs and
     * everything built on it -- the CLINT's mtime, and therefore every
     * guest timer interrupt -- stops with nothing to show for it.
     *
     * The constant is architectural. Writing it to a part with no lock, or
     * to one a debugger already unlocked, does nothing, which is why it is
     * unconditional.
     */
    DWT->LAR = 0xC5ACCE55u;

    DWT->CYCCNT = 0u;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

uint32_t board_cycles(void)
{
    return DWT->CYCCNT;
}

/* ------------------------------------------------------------------ */

const char *board_name(void) { return "Cortex-M7"; }

uint32_t board_clock_hz(void) { return SystemCoreClock; }

void board_init(void)
{
    /*
     * Caches first, before anything is written that will later be read
     * back through them.
     *
     * Not an optional speed-up: at 216 MHz with seven flash wait states an
     * uncached fetch costs seven cycles, so leaving the I-cache off would
     * make this part slower than the 180 MHz M4 it replaces. The D-cache
     * is what makes the JIT's code buffer need real maintenance -- see
     * sync_icache in the backend, and RV_ARM_HAS_CACHES, which this
     * platform defines.
     *
     * There is no DMA in this firmware, so the usual coherency hazard of
     * enabling the D-cache does not arise. A guest driving DMA through the
     * passthrough window is a different matter: peripheral space is Device
     * memory and never cached, but a DMA buffer in guest RAM is, and the
     * guest is expected to use Zicbom for it.
     */
    SCB_EnableICache();
    SCB_EnableDCache();

    HAL_Init();
    clock_init();
    console_init();
    dwt_init();
}
