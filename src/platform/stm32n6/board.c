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

#include "stm32n6xx_hal.h"

#include <stdbool.h>
#include <stdint.h>

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
 * The boot ROM leaves the CPU on HSI at 64 MHz, and this port stays
 * there.
 *
 * Deliberate for a first bring-up: a PLL configuration is a page of
 * register writes whose only test is the board, and the port has enough
 * that is new without it. The emulator is slower for it and correct, and
 * the figures this platform reports say 64 MHz beside them so nobody
 * compares them with the F746's 216 by accident.
 *
 * Raising it is the first tuning step, not a correctness one.
 */
static void clock_init(void)
{
    /*
     * **The supply comes first, and skipping it is why nothing ran.**
     *
     * ST's own Template FSBL opens SystemClock_Config with
     * HAL_PWREx_ConfigSupply() and then the voltage scaling, before it
     * touches an oscillator. This port had only SystemCoreClockUpdate(),
     * on the reasoning that staying on the boot ROM's HSI needs no setup
     * -- true of the *clock* and not of the supply it runs from. The
     * regulator is left in whatever state the ROM handed over, and the
     * core does not get far enough to write a UART register.
     *
     * PWR_EXTERNAL_SOURCE_SUPPLY is what the Nucleo wants: the board
     * feeds VDDCORE from an external regulator rather than the internal
     * SMPS, which is a property of the PCB and matches CN9 selecting the
     * 5V source. It is also what ST's template for this exact board
     * passes.
     */
    if (HAL_PWREx_ConfigSupply(PWR_EXTERNAL_SOURCE_SUPPLY) != HAL_OK) {
        Error_Handler();
    }
    if (HAL_PWREx_ControlVoltageScaling(PWR_REGULATOR_VOLTAGE_SCALE1)
        != HAL_OK) {
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
#define CONSOLE_BAUD 115200u

void HAL_UART_MspInit(UART_HandleTypeDef *huart)
{
    if (huart->Instance != USART1) {
        return;
    }

    __HAL_RCC_GPIOE_CLK_ENABLE();
    __HAL_RCC_USART1_CLK_ENABLE();

    GPIO_InitTypeDef g = {
        .Pin = GPIO_PIN_5 | GPIO_PIN_6,
        .Mode = GPIO_MODE_AF_PP,
        .Pull = GPIO_NOPULL,
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

    HAL_Init();
    clock_init();
    cycles_init();
    led_init();
    console_init();
}
