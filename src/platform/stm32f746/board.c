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

#include <stdbool.h>
#include <string.h>

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

/* ------------------------------------------------------------------ */
/* Interrupt-driven receive                                            */
/* ------------------------------------------------------------------ */

/*
 * Polling RDR is adequate for a human typing at a guest and hopeless for
 * a protocol. At 921600 baud a byte lands every 10.8 us and this family's
 * USART holds exactly one -- there is no receive FIFO -- so the next
 * arrival overruns it. The caller reaches board_console_getc() once per
 * guest slice, thousands of instructions apart, which is orders of
 * magnitude too slow: SLIP would lose most of every frame and present as
 * a link that passes nothing.
 *
 * So reception moves into an interrupt that does nothing but store a
 * byte, and the ring is what the run loop drains at its own pace. 2 KiB
 * covers 22 ms of wire at this rate, against a slice of a few hundred
 * microseconds.
 */
#define RX_RING_SIZE 2048u
#define RX_RING_MASK (RX_RING_SIZE - 1u)

static uint8_t  g_rx_ring[RX_RING_SIZE];
static volatile uint32_t g_rx_head;     /* written by the ISR only  */
static uint32_t g_rx_tail;              /* written by the loop only */
static volatile uint32_t g_rx_overrun;
static bool     g_rx_irq;

/*
 * The console USART's interrupt, and the reason it lives here rather
 * than in stm32f7xx_it.c with the other handlers: which USART the
 * ST-LINK's virtual COM port is wired to is precisely the fact board.c
 * exists to own. Naming USART3 in the interrupt file would put half that
 * knowledge in a file that is otherwise identical between the two
 * boards.
 */
void USART3_IRQHandler(void)
{
    USART_TypeDef *const u = g_console.Instance;
    const uint32_t isr = u->ISR;

    if ((isr & USART_ISR_RXNE) != 0u) {
        const uint8_t c = (uint8_t)(u->RDR & 0xFFu);

        /*
         * Single producer, single consumer, and the indices are word
         * sized and free running -- so the ISR and the loop each write
         * one of them and read the other, with no update that could be
         * seen half done. That is what makes this safe without disabling
         * interrupts around it.
         */
        if ((g_rx_head - g_rx_tail) < RX_RING_SIZE) {
            g_rx_ring[g_rx_head & RX_RING_MASK] = c;
            g_rx_head++;
        } else {
            g_rx_overrun++;
        }
    }

    /*
     * An overrun latches ORE and, until it is cleared, RXNE never sets
     * again -- so a single lost byte would silently stop reception for
     * good. The same is true of the framing and noise flags at this baud
     * rate. Clearing them costs one write and turns a dead link into a
     * dropped packet the protocol above can retransmit.
     */
    if ((isr & (USART_ISR_ORE | USART_ISR_FE | USART_ISR_NE | USART_ISR_PE))
        != 0u) {
        u->ICR = USART_ICR_ORECF | USART_ICR_FECF | USART_ICR_NCF |
                 USART_ICR_PECF;
        g_rx_overrun++;
    }
}

void board_console_rx_irq_enable(void)
{
    if (g_rx_irq) {
        return;
    }
    g_rx_irq = true;

    /*
     * Above the HAL's SysTick (which sits at the lowest urgency by
     * default) so a byte is never delayed by a tick, and below nothing
     * else -- this firmware runs the guest in thread mode and has only
     * the bridged guest interrupts besides.
     */
    HAL_NVIC_SetPriority(USART3_IRQn, 5, 0);
    HAL_NVIC_EnableIRQ(USART3_IRQn);
    __HAL_UART_ENABLE_IT(&g_console, UART_IT_RXNE);
}

int board_console_getc(void)
{
    if (g_rx_irq) {
        if (g_rx_head == g_rx_tail) {
            return -1;
        }
        return (int)g_rx_ring[g_rx_tail++ & RX_RING_MASK];
    }

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

uint32_t board_console_rx_overruns(void)
{
    return g_rx_overrun;
}

/* ------------------------------------------------------------------ */
/* ITCM                                                                */
/* ------------------------------------------------------------------ */

extern uint8_t __itcm_start[];
extern uint8_t __itcm_end[];
extern uint8_t __itcm_load[];

/*
 * ST's startup copies .data and knows nothing about .itcm, so anything
 * placed there is unreachable until this has run. It is the first thing
 * board_init does after the caches, which is what makes "nothing may be
 * called from ITCM before board_init" the whole of the rule.
 *
 * No cache maintenance is needed on the destination -- a TCM is never
 * cached, which is precisely why code there is immune to the flash bank
 * being busy. The barriers order the writes against the first fetch.
 */
static void itcm_init(void)
{
    const uint32_t len = (uint32_t)(__itcm_end - __itcm_start);

    if (len != 0u) {
        memcpy(__itcm_start, __itcm_load, len);
        __DSB();
        __ISB();
    }
}

/* ------------------------------------------------------------------ */
/* The guest-image arena in flash                                      */
/* ------------------------------------------------------------------ */

/*
 * Sectors 5, 6 and 7: 256 KiB each at 0x08040000, and the firmware is
 * 124 KiB, which fits inside sectors 0 to 3. Sector 4 is left as a gap
 * rather than used, so that growing the firmware past 128 KiB does not
 * silently start overwriting the arena -- it runs out of room and the
 * link fails instead.
 */
#define ARENA_BASE    0x08040000u
#define ARENA_SIZE    (768u * 1024u)
#define ARENA_SECTOR0 FLASH_SECTOR_5
#define ARENA_SECTORS 3u

static uint32_t g_arena_used;
static bool     g_arena_erased;

uint32_t board_flash_arena_base(void) { return ARENA_BASE; }
uint32_t board_flash_arena_size(void) { return ARENA_SIZE; }

/*
 * In ITCM, and this is the whole reason ITCM is declared. A sector erase
 * takes seconds on this part and stalls every fetch from the flash bank
 * while it runs, so a routine polling for completion from flash would be
 * polling instructions it cannot fetch. The core does not fault -- it
 * stops, and comes back when the bank does, which looks like a very slow
 * board rather than a design error.
 */
__attribute__((section(".itcm"), noinline))
static bool arena_erase(void)
{
    FLASH_EraseInitTypeDef e = { 0 };
    uint32_t bad = 0;

    e.TypeErase = FLASH_TYPEERASE_SECTORS;
    e.Sector = ARENA_SECTOR0;
    e.NbSectors = ARENA_SECTORS;
    /*
     * VOLTAGE_RANGE_3 is 2.7-3.6 V, which is what a Nucleo runs at, and
     * it selects x32 parallelism. Declaring a lower range would still
     * work and would take substantially longer; declaring a higher one
     * than the board supplies can leave the erase incomplete.
     */
    e.VoltageRange = FLASH_VOLTAGE_RANGE_3;

    if (HAL_FLASHEx_Erase(&e, &bad) != HAL_OK) {
        return false;
    }
    return true;
}

static bool arena_ensure_erased(void)
{
    if (g_arena_erased) {
        return true;
    }

    HAL_FLASH_Unlock();
    const bool ok = arena_erase();
    HAL_FLASH_Lock();

    if (!ok) {
        return false;
    }
    /*
     * Erasing changed what flash holds behind the caches' backs, and the
     * D-cache may still be holding the old contents from a readback.
     * Invalidating is the same rule the JIT lives by after emitting code.
     */
    SCB_CleanInvalidateDCache();
    SCB_InvalidateICache();

    g_arena_used = 0u;
    g_arena_erased = true;
    return true;
}

uint32_t board_flash_arena_begin(void)
{
    if (!arena_ensure_erased()) {
        return 0u;
    }
    if (g_arena_used >= ARENA_SIZE) {
        return 0u;
    }
    return ARENA_BASE + g_arena_used;
}

void board_flash_arena_commit(uint32_t len)
{
    /* Word-align so the next image can be programmed as whole words. */
    g_arena_used += (len + 3u) & ~3u;
}

bool board_flash_arena_reset(void)
{
    g_arena_erased = false;
    return arena_ensure_erased();
}

__attribute__((section(".itcm"), noinline))
bool board_flash_write(uint32_t addr, const void *data, uint32_t len)
{
    const uint8_t *src = (const uint8_t *)data;
    bool ok = true;

    if (addr < ARENA_BASE || (addr + len) > (ARENA_BASE + ARENA_SIZE)) {
        return false;
    }

    HAL_FLASH_Unlock();

    while (len != 0u && ok) {
        /*
         * Word at a time, and the tail padded with ones rather than
         * zeroes: an erased cell is 1, and programming can only clear
         * bits, so padding with zeroes would make the remainder of the
         * word unprogrammable if it were ever written again.
         */
        uint32_t w = 0xFFFFFFFFu;
        const uint32_t n = (len < 4u) ? len : 4u;

        memcpy(&w, src, n);
        ok = HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, addr, w) == HAL_OK;

        addr += 4u;
        src += n;
        len -= n;
    }

    HAL_FLASH_Lock();

    /*
     * The guest is about to be fetched from here, and on a part with
     * caches that means the same clean-and-invalidate the JIT needs
     * after emitting code. Skipping it does not fail visibly -- it runs
     * the *previous* image, which passes or fails on its own merits and
     * says nothing about the one just uploaded.
     */
    SCB_CleanInvalidateDCache();
    SCB_InvalidateICache();
    return ok;
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

    itcm_init();

    HAL_Init();
    clock_init();
    console_init();
    dwt_init();
}
