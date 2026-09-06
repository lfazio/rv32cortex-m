/* SPDX-License-Identifier: Apache-2.0 */
/*
 * board.c - the Nucleo-N657X0-Q (MB1940), a Cortex-M55 with no flash.
 *
 * The third platform, and the first that is neither ARMv7E-M nor
 * flash-based. What it shares with the F446 and F746 is in
 * src/platform/stm32/board.c -- the image store, the two ends of a run,
 * the gdb transport; this is the part-specific half that board_api.h
 * asks for.
 *
 * Three things make it different, and only the third took any work.
 *
 * **No internal user flash.** The MCU is an STM32N657X0H3Q: it boots from
 * the boot ROM, which loads an image into AXISRAM and jumps to it. So
 * there is no sector-erasable arena and this board takes no TFTP uploads
 * -- board_flash_arena_size() returns 0, which is how a platform declines
 * and costs the ones that do nothing. A RAM-backed arena would give it
 * uploads back and is a later, small change.
 *
 * **1536 KiB of guest RAM**, against the F746's 243. Everything is
 * AXISRAM here, so the F746's reason for serving read-only guest bytes
 * out of flash does not apply and the guest simply gets more.
 *
 * **The DWT software lock, again.** Like the M7 and unlike the M4, this
 * core implements it: `DWT->LAR = 0xC5ACCE55` has to come before enabling
 * CYCCNT or the writes are discarded *silently*, the cycle counter never
 * runs, and every guest timer interrupt stops. On the F746 that presented
 * as `timer-fired` and `timer-cause` and nothing else, and cost a session.
 */

#include "board.h" /* and board_api.h, the contract, through it */

#include "emu_console.h"

#include "stm32n6xx_hal.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

static UART_HandleTypeDef g_console;

static void Error_Handler(void)
{
    /*
     * Bring-up failed before there is a console to say so on. Halting
     * leaves the state intact for a debugger, which is the only thing
     * that can read it at this point.
     */
    __disable_irq();
    for (;;) {
    }
}

/* ------------------------------------------------------------------ */
/* Clocks                                                              */
/* ------------------------------------------------------------------ */

/*
 * 600 MHz, from ST's own Template FSBL for this board.
 *
 * The boot ROM leaves the CPU on HSI at 64 MHz, and this port stayed
 * there while it was being brought up -- deliberately, because a PLL
 * configuration is a page of register writes whose only test is the
 * board, and there was enough else that was new. It is 9.4x on the table
 * and the emulator is the one thing here that spends every cycle it is
 * given, so it does not stay off.
 *
 * HSI 64 MHz / PLLM 4 = 16 MHz -> x PLLN 75 = 1200 MHz, and IC1 divides
 * that by 2 for the CPU. SYSCLK comes off IC2 at /3 = 400 MHz with AHB
 * at /2.
 *
 * **Three things about the sequence are not obvious and are ST's, not
 * mine.** The supply and the voltage scaling come first, before any
 * oscillator is touched -- leaving them out is what stopped this port
 * running at all for several rounds. The CPU and system clocks are
 * parked back on HSI before the PLL is reconfigured, because a running
 * core cannot have the clock it is executing from moved underneath it.
 * And the "IC" dividers are this family's own layer between a PLL and a
 * domain; there is no direct PLL-to-CPU path to configure.
 */
static void clock_init(void)
{
    RCC_OscInitTypeDef osc = { 0 };
    RCC_ClkInitTypeDef clk = { 0 };

    if (HAL_PWREx_ConfigSupply(PWR_EXTERNAL_SOURCE_SUPPLY) != HAL_OK) {
        Error_Handler();
    }
    if (HAL_PWREx_ControlVoltageScaling(PWR_REGULATOR_VOLTAGE_SCALE1)
        != HAL_OK) {
        Error_Handler();
    }

    osc.OscillatorType = RCC_OSCILLATORTYPE_HSI;
    osc.HSIState = RCC_HSI_ON;
    osc.HSIDiv = RCC_HSI_DIV1;
    osc.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
    osc.PLL1.PLLState = RCC_PLL_NONE;
    osc.PLL2.PLLState = RCC_PLL_NONE;
    osc.PLL3.PLLState = RCC_PLL_NONE;
    osc.PLL4.PLLState = RCC_PLL_NONE;
    if (HAL_RCC_OscConfig(&osc) != HAL_OK) {
        Error_Handler();
    }

    /* Park on HSI: the PLL about to be reconfigured may be the one this
     * core is running from. */
    HAL_RCC_GetClockConfig(&clk);
    if (clk.CPUCLKSource == RCC_CPUCLKSOURCE_IC1 ||
        clk.SYSCLKSource == RCC_SYSCLKSOURCE_IC2_IC6_IC11) {
        clk.ClockType     = RCC_CLOCKTYPE_CPUCLK | RCC_CLOCKTYPE_SYSCLK;
        clk.CPUCLKSource  = RCC_CPUCLKSOURCE_HSI;
        clk.SYSCLKSource  = RCC_SYSCLKSOURCE_HSI;
        if (HAL_RCC_ClockConfig(&clk) != HAL_OK) {
            Error_Handler();
        }
    }

    /* PLL1: 64 / 4 * 75 = 1200 MHz. */
    osc.OscillatorType   = RCC_OSCILLATORTYPE_NONE;
    osc.PLL1.PLLState    = RCC_PLL_ON;
    osc.PLL1.PLLSource   = RCC_PLLSOURCE_HSI;
    osc.PLL1.PLLM        = 4;
    osc.PLL1.PLLN        = 75;
    osc.PLL1.PLLFractional = 0;
    osc.PLL1.PLLP1       = 1;
    osc.PLL1.PLLP2       = 1;
    osc.PLL2.PLLState    = RCC_PLL_NONE;
    osc.PLL3.PLLState    = RCC_PLL_NONE;
    osc.PLL4.PLLState    = RCC_PLL_NONE;
    if (HAL_RCC_OscConfig(&osc) != HAL_OK) {
        Error_Handler();
    }

    clk.ClockType = RCC_CLOCKTYPE_CPUCLK | RCC_CLOCKTYPE_HCLK |
                    RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_PCLK1 |
                    RCC_CLOCKTYPE_PCLK2 | RCC_CLOCKTYPE_PCLK5 |
                    RCC_CLOCKTYPE_PCLK4;
    clk.CPUCLKSource   = RCC_CPUCLKSOURCE_IC1;      /* PLL1 / 2 = 600 MHz */
    clk.SYSCLKSource   = RCC_SYSCLKSOURCE_IC2_IC6_IC11;
    clk.AHBCLKDivider  = RCC_HCLK_DIV2;
    clk.APB1CLKDivider = RCC_APB1_DIV1;
    clk.APB2CLKDivider = RCC_APB2_DIV1;
    clk.APB4CLKDivider = RCC_APB4_DIV1;
    clk.APB5CLKDivider = RCC_APB5_DIV1;
    clk.IC1Selection.ClockSelection  = RCC_ICCLKSOURCE_PLL1;
    clk.IC1Selection.ClockDivider    = 2;
    clk.IC2Selection.ClockSelection  = RCC_ICCLKSOURCE_PLL1;
    clk.IC2Selection.ClockDivider    = 3;
    clk.IC6Selection.ClockSelection  = RCC_ICCLKSOURCE_PLL1;
    clk.IC6Selection.ClockDivider    = 4;
    clk.IC11Selection.ClockSelection = RCC_ICCLKSOURCE_PLL1;
    clk.IC11Selection.ClockDivider   = 3;
    if (HAL_RCC_ClockConfig(&clk) != HAL_OK) {
        Error_Handler();
    }

    SystemCoreClockUpdate();
}

/* ------------------------------------------------------------------ */
/* Console -- USART1 on PE5/PE6                                        */
/* ------------------------------------------------------------------ */

/*
 * From the board's own BSP (stm32n6xx_nucleo.h: COM1_TX_PIN,
 * COM1_TX_AF), not guessed: USART1 has two alternate-function mappings
 * on this part, AF4 and AF7, and the pins carry AF7. A wrong AF does not
 * fail to compile or configure -- it routes the pin to a different
 * peripheral and the port goes quiet, which is the shape of failure this
 * project has already lost a day to on a serial line.
 */
/*
 * 921600, matching the F446 and F746 rather than the 115200 UM3417 gives
 * as the VCP default. The ST-LINK's virtual COM port carries whatever
 * both ends agree on, and the emulator's own output plus a guest's
 * console is enough traffic that the slower rate is felt -- the F746
 * runs a whole architecture suite over this wire.
 */
#define CONSOLE_BAUD 921600u

void HAL_UART_MspInit(UART_HandleTypeDef *huart)
{
    if (huart->Instance != USART1) {
        return;
    }

    /*
     * **Select the kernel clock before enabling the peripheral.**
     *
     * On this family a USART does not simply run from its APB clock the
     * way an F4's does: the clock source is a separate choice, and left
     * unmade the peripheral has no clock at all. HAL_UART_Init then fails
     * inside UART_SetConfig, before MspInit has written a single
     * register -- which presents as a console that is silent with the
     * pins, the AF and the baud rate all correct.
     *
     * From ST's own UART_Printf example for this board, which is also
     * where PE5/PE6 and GPIO_AF7_USART1 come from.
     */
    RCC_PeriphCLKInitTypeDef pclk = {
        .PeriphClockSelection = RCC_PERIPHCLK_USART1,
        .Usart1ClockSelection = RCC_USART1CLKSOURCE_PCLK2,
    };

    if (HAL_RCCEx_PeriphCLKConfig(&pclk) != HAL_OK) {
        Error_Handler();
    }

    __HAL_RCC_GPIOE_CLK_ENABLE();
    __HAL_RCC_USART1_CLK_ENABLE();

    GPIO_InitTypeDef g = {
        .Pin = GPIO_PIN_5 | GPIO_PIN_6,
        .Mode = GPIO_MODE_AF_PP,
        .Pull = GPIO_PULLUP,
        .Speed = GPIO_SPEED_FREQ_VERY_HIGH,
        .Alternate = GPIO_AF7_USART1,
    };

    HAL_GPIO_Init(GPIOE, &g);
}

static void console_init(void)
{
    g_console.Instance = USART1;
    g_console.Init.BaudRate = CONSOLE_BAUD;
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

void board_console_putc(uint8_t c)
{
    /*
     * Polled, and blocking until the byte is gone. The console is written
     * by human-readable output and by the guest's virtual UART, neither
     * of which is on a measured hot path -- and a transmit that returns
     * before the byte has left loses the tail of a panic message, which
     * is the one message worth keeping.
     */
    while ((USART1->ISR & USART_ISR_TXE_TXFNF) == 0u) {
    }
    USART1->TDR = c;
}

/*
 * Receive, into a ring filled by the interrupt.
 *
 * The run loop reaches board_console_getc once per guest slice --
 * thousands of instructions apart -- and this family's USART holds one
 * byte. Polling TDR here drops most of every frame the moment anything
 * faster than a person is talking, which is what the F746 found at
 * 921600.
 */
#define RX_RING_SIZE 512u
#define RX_MASK (RX_RING_SIZE - 1u)

static uint8_t g_rx_ring[RX_RING_SIZE];
static volatile uint32_t g_rx_head; /* written by the ISR only  */
static uint32_t g_rx_tail; /* written by the loop only */
static volatile uint32_t g_rx_overrun;
static bool g_rx_irq;

void USART1_IRQHandler(void)
{
    const uint32_t isr = USART1->ISR;

    if ((isr & USART_ISR_RXNE_RXFNE) != 0u) {
        const uint8_t c = (uint8_t)USART1->RDR;
        const uint32_t next = (g_rx_head + 1u) & RX_MASK;

        if (next != g_rx_tail) {
            g_rx_ring[g_rx_head] = c;
            g_rx_head = next;
        } else {
            g_rx_overrun++; /* the reader is behind */
        }
    }

    /*
     * Dismiss an overrun, or reception stops *permanently* rather than
     * dropping one byte. This family clears it by writing ICR, where the
     * F4 reads SR then DR -- getting that wrong on the F7 was a link that
     * worked until the first burst.
     */
    if ((isr & USART_ISR_ORE) != 0u) {
        USART1->ICR = USART_ICR_ORECF;
        g_rx_overrun++;
    }
}

void board_console_rx_irq_enable(void)
{
    if (g_rx_irq) {
        return;
    }
    g_rx_irq = true;

    USART1->CR1 |= USART_CR1_RXNEIE_RXFNEIE;
    HAL_NVIC_SetPriority(USART1_IRQn, 5, 0);
    HAL_NVIC_EnableIRQ(USART1_IRQn);
}

int board_console_getc(void)
{
    if (!g_rx_irq) {
        /* Before the ring is armed, answer from the register directly so
         * a guest that reads early is not told "nothing, ever". */
        if ((USART1->ISR & USART_ISR_RXNE_RXFNE) != 0u) {
            return (int)(uint8_t)USART1->RDR;
        }
        return -1;
    }
    if (g_rx_tail == g_rx_head) {
        return -1;
    }

    const uint8_t c = g_rx_ring[g_rx_tail];

    g_rx_tail = (g_rx_tail + 1u) & RX_MASK;
    return (int)c;
}

uint32_t board_console_rx_overruns(void)
{
    return g_rx_overrun;
}

/* ------------------------------------------------------------------ */
/* Time                                                                */
/* ------------------------------------------------------------------ */

uint32_t board_clock_hz(void)
{
    return SystemCoreClock;
}

uint32_t board_cycles(void)
{
    return DWT->CYCCNT;
}

/*
 * The DWT software lock: present on the M7, *architecturally removed*
 * here, and the difference is not something to guess at either way.
 *
 * On the F746 the rule is unconditional -- `DWT->LAR = 0xC5ACCE55` before
 * enabling CYCCNT, or every write is discarded silently, the cycle
 * counter never runs, and every guest timer interrupt stops arriving.
 * That cost a session, and the obvious way to port it is to copy the line.
 *
 * It does not compile, which is the lucky outcome: Armv8-M made the
 * CoreSight software lock optional and CMSIS dropped LAR from DWT_Type
 * accordingly, leaving only the read-only Lock Status Register. The
 * unlucky version of the same mistake is poking offset 0xFB0 by address
 * "to be safe" -- a write to a register that may not exist, on a part
 * whose reserved addresses signal an AHB error, which is a HardFault in
 * the emulator rather than a fault delivered anywhere useful.
 *
 * So ask. LSR bit 0 is SLI, "software lock implemented", and it is the
 * architecture's own answer to exactly this question. Zero on this part
 * today; the branch costs one load at start-up and means a future Armv8-M
 * host that does implement the lock is handled rather than mysteriously
 * reporting no time passing.
 */
#define DWT_LSR_SLI_Msk 0x1u
#define DWT_LAR_OFFSET 0xFB0u
#define DWT_UNLOCK_KEY 0xC5ACCE55u

static void cycles_init(void)
{
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;

    if ((DWT->LSR & DWT_LSR_SLI_Msk) != 0u) {
        *(volatile uint32_t *)((uintptr_t)DWT + DWT_LAR_OFFSET) =
            DWT_UNLOCK_KEY;
    }

    DWT->CYCCNT = 0u;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

/* ------------------------------------------------------------------ */
/* LEDs -- LD1 PG8, LD2 PG10, LD3 PG0                                  */
/* ------------------------------------------------------------------ */

void board_led_toggle(board_led_t led)
{
    HAL_GPIO_TogglePin(GPIOG, (led == BOARD_LED_TX) ? GPIO_PIN_10 : GPIO_PIN_8);
}

static void led_init(void)
{
    __HAL_RCC_GPIOG_CLK_ENABLE();

    GPIO_InitTypeDef g = {
        .Pin = GPIO_PIN_8 | GPIO_PIN_10,
        .Mode = GPIO_MODE_OUTPUT_PP,
        .Pull = GPIO_NOPULL,
        .Speed = GPIO_SPEED_FREQ_LOW,
    };

    HAL_GPIO_Init(GPIOG, &g);
}

/* ------------------------------------------------------------------ */
/* The image store: this part has none                                 */
/* ------------------------------------------------------------------ */

/*
 * No internal user flash, so no arena, so no uploads. Answering zero is
 * how board_api.h says a platform declines -- the caller tests it at run
 * time and the compiler folds the branch, so the F746's upload path costs
 * this board nothing and needs no #if anywhere.
 *
 * The 4 MB of AXISRAM makes a RAM-backed arena the obvious next step: it
 * would give this board TFTP uploads with no flash involved at all, which
 * is a thing the flash-based boards cannot do.
 */
uintptr_t board_flash_arena_base(void)
{
    return 0u;
}
uint32_t board_flash_arena_size(void)
{
    return 0u;
}
uintptr_t board_flash_arena_begin(void)
{
    return 0u;
}
void board_flash_arena_commit(uint32_t len)
{
    (void)len;
}
bool board_flash_arena_reset(void)
{
    return false;
}
uint32_t board_flash_last_error(void)
{
    return 0u;
}

bool board_flash_write(uintptr_t addr, const void *data, uint32_t len)
{
    (void)addr;
    (void)data;
    (void)len;
    return false;
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

const char *board_name(void)
{
    return "Cortex-M55";
}

void board_wfi(void)
{
    __WFI();
}

void board_fatal(int *status)
{
    (void)status;

    __disable_irq();
    for (;;) {
    }
}

/* ------------------------------------------------------------------ */
/* ITCM                                                                */
/* ------------------------------------------------------------------ */

extern uint8_t __itcm_start[];
extern uint8_t __itcm_end[];
extern uint8_t __itcm_load[];

/*
 * Bring up the instruction TCM and install the hot path in it.
 *
 * **The enable is written rather than assumed.** MEMSYSCTL->ITCMCR.EN is
 * the Armv8.1-M control (PM0273 / the M55 TRM), and whether the boot ROM
 * leaves it set is not something to infer from a part that boots through
 * a ROM this port has already been surprised by twice. Writing it costs
 * one store and makes the outcome independent of what the ROM did.
 *
 * SZ is read-only and reports what the implementation has -- RM0486
 * table 2 says 64 KiB of baseline at 0x1000_0000, and the linker script
 * declares exactly that. It is reported at boot rather than checked,
 * because a mismatch between the script and the silicon is a link-time
 * question and this is a run-time observation.
 *
 * The copy is the F746's, for the same reason: ST's startup copies .data
 * and knows nothing about .itcm, so **nothing may call into ITCM before
 * this has run**. It is the first thing board_hw_init does after the
 * caches. A TCM is never cached, so only the barriers are needed --
 * there is no clean-to-PoU to do on the destination.
 */
static void itcm_init(void)
{
    const uint32_t len = (uint32_t)(__itcm_end - __itcm_start);

    MEMSYSCTL->ITCMCR |= MEMSYSCTL_ITCMCR_EN_Msk;
    __DSB();
    __ISB();

    if (len != 0u) {
        memcpy(__itcm_start, __itcm_load, len);
        __DSB();
        __ISB();
    }
}

/*
 * What ITCM actually came up as, for the banner.
 *
 * SZ is an encoded size, and the encoding is the one the M55 TRM gives:
 * 0 means absent and n means 1 KiB << (n - 1), so 7 is the 64 KiB this
 * part documents. Reporting the decoded number rather than the field is
 * the difference between a line that can be read and one that has to be
 * looked up -- and a *zero* here says the enable did not take, which is
 * the failure worth being able to see.
 */
uint32_t board_itcm_bytes(void)
{
    const uint32_t sz =
        (MEMSYSCTL->ITCMCR & MEMSYSCTL_ITCMCR_SZ_Msk) >> MEMSYSCTL_ITCMCR_SZ_Pos;

    if ((MEMSYSCTL->ITCMCR & MEMSYSCTL_ITCMCR_EN_Msk) == 0u || sz == 0u) {
        return 0u;
    }
    return 1024u << (sz - 1u);
}

void board_hw_init(void)
{

    /*
     * **Caches first, before anything is written.** ST's own FSBL
     * template does this ahead of HAL_Init() and the clock setup, and the
     * order is the point: enabling a cache after memory has been written
     * through it disabled leaves lines that disagree with RAM.
     *
     * These two lines are what make board_sync_icache load-bearing here.
     * The JIT writes instructions as data and branches to them, and
     * __ARM_ARCH is 8 with __thumb2__ on a Cortex-M55 -- so this part
     * selects the Thumb-2 backend exactly as the M7 does, and needs the
     * same clean-to-PoU and I-cache invalidate. It gets it from
     * src/platform/stm32/cache.c, which every STM32 links and which reads
     * CMSIS's own __DCACHE_PRESENT rather than a platform name.
     */
    SCB_EnableICache();
    SCB_EnableDCache();

    /*
     * Before anything else that might be *in* ITCM is called, and after
     * the caches so the copy's stores go through a configured cache.
     */
    itcm_init();

    HAL_Init();
    clock_init();
    cycles_init();
    led_init();
    console_init();

    /*
     * What ITCM came up as, now that there is a console to say it on.
     *
     * This is not decoration. The JIT's code buffer is linked into ITCM,
     * and if the region is smaller than the linker was told, the buffer
     * runs off the end of real memory -- which does not fault, it
     * executes whatever the truncated address aliases onto, and presents
     * as a guest that starts and never finishes. A *number* here is the
     * difference between diagnosing that in one run and bisecting the
     * translator.
     */
    emu_console_printf("itcm   %u KiB at 0x%08x, jit buffer %u KiB\n",
                       (unsigned)(board_itcm_bytes() / 1024u), 0x10000000u,
                       (unsigned)(EMU_IR_JIT_STATIC_BYTES / 1024u));
}
