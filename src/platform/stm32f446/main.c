/* SPDX-License-Identifier: Apache-2.0 */
/*
 * main.c - RV32IMAC emulator firmware for the Nucleo-F446RE.
 *
 * Bring-up is done entirely with ST's own driver pack: the CMSIS device
 * headers for register definitions, ST's startup file and
 * system_stm32f4xx.c, and the STM32Cube HAL for the clock tree, GPIO and
 * USART. Nothing here reimplements a peripheral the vendor already
 * supports.
 *
 * What this file adds is the glue: a guest address space built out of ARM
 * memory and ARM peripherals, and a run loop that hands control back often
 * enough for the ARM side to keep servicing its own interrupts.
 */

#include "stm32f4xx_hal.h"

#include "rv32/rv_backend.h"
#include "rv32/rv_dev.h"
#include "rv32/rv_aplic.h"
#include "rv32/rv_hart.h"
#include "rv32/rv_jit.h"
#include "rv32/rv_memmap.h"

#include <string.h>

/* The guest binary, embedded by guest_image.S. */
extern const uint8_t rv_guest_image[];
extern const uint32_t rv_guest_image_size;

/* ------------------------------------------------------------------ */
/* Configuration                                                       */
/* ------------------------------------------------------------------ */

/*
 * Guest RAM is not a fixed-size array: the link script places it between
 * the firmware's .bss and the stack and gives it everything in between, so
 * the guest automatically receives all the SRAM the ARM side is not using
 * (see .guest_ram in stm32f446retx.ld).
 */
extern uint8_t __guest_ram_start[];
extern uint8_t __guest_ram_end[];

#define GUEST_RAM_BASE_PTR  (__guest_ram_start)
#define GUEST_RAM_SIZE      ((uint32_t)(__guest_ram_end - __guest_ram_start))

/* CLINT mtime runs at 1 MHz, derived from the DWT cycle counter. */
#define RV_CLINT_HZ         1000000u

/* Instructions executed between returns to the ARM side. */
#ifndef RV_RUN_SLICE
#define RV_RUN_SLICE        4096u
#endif

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

#if RV_ENABLE_JIT
/*
 * Code cache for translated blocks. Ordinary .bss: the ARMv7-M default
 * memory map makes SRAM executable, so no MPU work is needed. This comes
 * out of what the guest would otherwise get, which is the trade the JIT
 * asks for.
 */
static uint8_t g_jit_code[RV_JIT_CODE_SIZE] __attribute__((aligned(8)));
#endif

static rv_bus_t    g_bus;
static rv_hart_t   g_hart;
static rv_clint_t  g_clint;
static rv_aplic_t  g_aplic;
static rv_uart_t   g_uart;
static UART_HandleTypeDef g_console;

/* ------------------------------------------------------------------ */
/* Console                                                             */
/* ------------------------------------------------------------------ */

void rv_console_putc(uint8_t c)
{
    /* Blocking, with a generous timeout: output is for humans, and losing
     * a byte is worse than stalling briefly. */
    HAL_UART_Transmit(&g_console, &c, 1u, 100u);
}

#define console_putc rv_console_putc

static void console_puts(const char *s)
{
    while (*s != '\0') {
        if (*s == '\n') {
            console_putc('\r');   /* terminals expect CRLF */
        }
        console_putc((uint8_t)*s++);
    }
}

static void console_puthex(uint32_t v)
{
    static const char hex[] = "0123456789abcdef";
    console_puts("0x");
    for (int i = 28; i >= 0; i -= 4) {
        console_putc((uint8_t)hex[(v >> i) & 0xFu]);
    }
}

static void console_putu(uint32_t v)
{
    char tmp[10];
    unsigned n = 0;
    do {
        tmp[n++] = (char)('0' + (v % 10u));
        v /= 10u;
    } while (v != 0u);
    while (n != 0u) {
        console_putc((uint8_t)tmp[--n]);
    }
}

/* Transport hooks for the guest's virtual UART. */
static void guest_uart_tx(void *ctx, uint8_t c)
{
    (void)ctx;
    if (c == '\n') {
        console_putc('\r');
    }
    console_putc(c);
}

static int guest_uart_rx(void *ctx)
{
    (void)ctx;
    /* Non-blocking poll of the receive data register. */
    if (__HAL_UART_GET_FLAG(&g_console, UART_FLAG_RXNE)) {
        return (int)(g_console.Instance->DR & 0xFFu);
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* ECALL services                                                      */
/* ------------------------------------------------------------------ */

/*
 * The same newlib-style calls the host runner implements, so a guest built
 * once behaves identically in both places. Anything else falls through to
 * a normal M-mode trap, leaving guests with their own handler unaffected.
 *
 *   a7 = 64  write(fd, buf, len)
 *   a7 = 93  exit(code)
 */
#define REG_A0  10
#define REG_A1  11
#define REG_A2  12
#define REG_A7  17

static uint32_t g_exit_code;
static bool     g_exited;

static bool guest_ecall(rv_hart_t *h, void *user)
{
    (void)user;

    switch (h->x[REG_A7]) {
    case 64: {
        const uint32_t buf = h->x[REG_A1];
        const uint32_t len = h->x[REG_A2];
        for (uint32_t i = 0; i < len; i++) {
            uint32_t byte;
            if (rv_bus_read(h->bus, buf + i, 1u, &byte) != RV_EXC_NONE) {
                break;
            }
            guest_uart_tx(NULL, (uint8_t)byte);
        }
        h->x[REG_A0] = len;
        return true;
    }

    case 93:
        g_exit_code = h->x[REG_A0];
        g_exited = true;
        h->state = RV_STATE_HALTED;
        return true;

    default:
        return false;
    }
}

/* ------------------------------------------------------------------ */
/* Cache maintenance                                                   */
/* ------------------------------------------------------------------ */

/*
 * Zicbom mapped onto ARMv7-M cache maintenance.
 *
 * The Cortex-M4 in the STM32F446 has no data cache -- only the ART flash
 * accelerator, which is transparent and needs no maintenance -- so on this
 * part every operation is a no-op, which the RISC-V spec explicitly
 * permits. The code below is written against __DCACHE_PRESENT so the same
 * platform file does the right thing when built for a Cortex-M7, where
 * these become real cache operations on the lines backing the guest block.
 */
static void arm_cache_maint(void *ctx, void *host, uint32_t len,
                            rv_cbo_op_t op)
{
    (void)ctx;

#if defined(__DCACHE_PRESENT) && (__DCACHE_PRESENT == 1U)
    switch (op) {
    case RV_CBO_CLEAN:
        SCB_CleanDCache_by_Addr((uint32_t *)host, (int32_t)len);
        break;
    case RV_CBO_INVAL:
        SCB_InvalidateDCache_by_Addr((uint32_t *)host, (int32_t)len);
        break;
    case RV_CBO_FLUSH:
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

static const rv_cache_ops_t g_cache_ops = {
    .maint = arm_cache_maint,
    .ctx = NULL,
};

/* ------------------------------------------------------------------ */
/* Cycle counter                                                       */
/* ------------------------------------------------------------------ */

static void dwt_init(void)
{
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0u;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

static inline uint32_t dwt_cycles(void)
{
    return DWT->CYCCNT;
}

/* ------------------------------------------------------------------ */
/* Clock tree                                                          */
/* ------------------------------------------------------------------ */

static void Error_Handler(void)
{
    __disable_irq();
    for (;;) {
        /* A failure this early cannot be reported; halt so a debugger can
         * see where we stopped. */
    }
}

/*
 * 180 MHz from the internal 16 MHz HSI. HSI rather than HSE because the
 * Nucleo-F446RE ships without a populated crystal, and the ST-LINK MCO
 * path depends on solder-bridge options that vary between board revisions;
 * HSI works on an unmodified board.
 *
 *   HSI 16 MHz / PLLM 8 = 2 MHz  -> x PLLN 180 = 360 MHz -> / PLLP 2 = 180 MHz
 */
static void SystemClock_Config(void)
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
}

static void console_init(void)
{
    g_console.Instance = USART2;             /* wired to the ST-LINK VCP */
    g_console.Init.BaudRate = 115200;
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

/* ------------------------------------------------------------------ */
/* Guest address space                                                 */
/* ------------------------------------------------------------------ */

/*
 * Peripheral passthrough policy.
 *
 * Guest 0x40000000..0x5FFFFFFF maps one-to-one onto the STM32's own
 * peripheral space, so a guest driver uses exactly the addresses printed
 * in RM0390 with no translation to reason about. This is what keeps
 * peripheral drivers in the guest and the ARM-side port thin: moving to
 * another STM32 means changing the clock setup and this table, not writing
 * new drivers.
 *
 * Only what would take the emulator down with the guest is withheld:
 *
 *   PWR           dropping over-drive at 180 MHz stalls the core
 *   RCC CR/PLL    reconfiguring the PLL kills the clock the emulator runs on
 *   FLASH         the flash controller can erase this firmware underneath us
 *
 * Note what is deliberately *not* withheld: the rest of RCC, including
 * AHB1ENR/APB1ENR/APB2ENR and the peripheral reset registers. A guest
 * driver has to be able to ungate its own peripheral's clock, and denying
 * that would push every driver back into the firmware.
 */
static const struct {
    const char *name;
    uint32_t    base;
    uint32_t    size;
    uint8_t     perm;
} g_periph_map[] = {
    /* APB1 up to PWR: timers, RTC, WWDG, SPI2/3, USART2/3, UART4/5, I2C */
    { "apb1",       0x40000000u, 0x00007000u, RV_PERM_RW },
    { "pwr",        0x40007000u, 0x00000400u, RV_PERM_R  },
    /* Rest of APB1, all of APB2, GPIO and CRC */
    { "apb1b+apb2", 0x40007400u, 0x0001C400u, RV_PERM_RW },
    /* RCC clock tree: CR, PLLCFGR, CFGR, CIR */
    { "rcc-clock",  0x40023800u, 0x00000010u, RV_PERM_R  },
    /* RCC resets and peripheral clock enables: the guest's to drive */
    { "rcc-periph", 0x40023810u, 0x000003F0u, RV_PERM_RW },
    /* Flash interface: ACR, keys, control, option bytes */
    { "flash-ctl",  0x40023C00u, 0x00000400u, RV_PERM_R  },
    /* BKPSRAM, DMA1/2, USB OTG HS, and AHB2 up to 0x5FFFFFFF */
    { "ahb1b+ahb2", 0x40024000u, 0x1FFDC000u, RV_PERM_RW },
};

/* ------------------------------------------------------------------ */
/* Real interrupt lines                                                */
/* ------------------------------------------------------------------ */

/*
 * Bridging the NVIC to the APLIC.
 *
 * An interrupt is the one thing the passthrough window cannot carry. A
 * guest driver reaches a peripheral by using its address, but when that
 * peripheral raises an interrupt the NVIC vectors here, into the emulator,
 * with the guest nowhere in sight.
 *
 * The handshake is forced by one fact: nothing on this side can service the
 * device. Only the guest's driver knows how, and it will not run until the
 * emulator returns from the ISR. A level-triggered peripheral therefore
 * re-asserts the moment the handler exits, and the emulator would spin in
 * interrupt entry forever without the guest ever making progress. So the
 * line is masked on entry and stays masked until the guest clears the
 * APLIC pending bit, which is its way of saying the device has been dealt
 * with -- see aplic_unmask_line, reached through the APLIC's eoi hook.
 *
 * Adding a peripheral is one table entry and one handler; the table is the
 * policy, the same way g_periph_map is for addresses.
 */
static const IRQn_Type g_irq_map[RV_APLIC_SOURCES] = {
    [RV_IRQ_SRC_TIM6] = TIM6_DAC_IRQn,
};

static void aplic_line_entry(uint32_t source)
{
    NVIC_DisableIRQ(g_irq_map[source]);
    rv_aplic_raise(&g_aplic, source);
}

static void aplic_unmask_line(void *ctx, uint32_t source)
{
    (void)ctx;
    if (source < RV_APLIC_SOURCES && g_irq_map[source] != 0) {
        NVIC_ClearPendingIRQ(g_irq_map[source]);
        NVIC_EnableIRQ(g_irq_map[source]);
    }
}

/*
 * Enable the bridged lines at the NVIC. Priority is left at the default:
 * these handlers do almost nothing, and the emulator has no other interrupt
 * to rank them against.
 */
static void bridged_irqs_init(void)
{
    for (uint32_t i = 1u; i < RV_APLIC_SOURCES; i++) {
        if (g_irq_map[i] != 0) {
            NVIC_EnableIRQ(g_irq_map[i]);
        }
    }
}

void TIM6_DAC_IRQHandler(void)
{
    aplic_line_entry(RV_IRQ_SRC_TIM6);
}

static bool build_address_space(void)
{
    rv_bus_init(&g_bus);

    if (!rv_bus_add_ram(&g_bus, "ram", RV_GUEST_RAM_BASE,
                        GUEST_RAM_BASE_PTR, GUEST_RAM_SIZE)) {
        return false;
    }

    /*
     * The guest image stays in ARM flash and is exposed read-only, so a
     * guest linked for execute-in-place costs no RAM at all.
     */
    if (!rv_bus_add_rom(&g_bus, "rom", RV_GUEST_ROM_BASE,
                        rv_guest_image, rv_guest_image_size)) {
        return false;
    }

    if (!rv_bus_add_mmio(&g_bus, "aplic", RV_GUEST_APLIC_BASE,
                         RV_APLIC_SIZE, &rv_aplic_ops, &g_aplic)) {
        return false;
    }
    /*
     * ACLINT rather than the legacy CLINT window: two devices at the
     * offsets the old layout implied, so guests written for either work.
     */
    if (!rv_bus_add_mmio(&g_bus, "aclint-mswi", RV_GUEST_ACLINT_MSWI_BASE,
                         RV_ACLINT_MSWI_SIZE, &rv_aclint_mswi_ops,
                         &g_clint)) {
        return false;
    }
    if (!rv_bus_add_mmio(&g_bus, "aclint-mtimer", RV_GUEST_ACLINT_MTIMER_BASE,
                         RV_ACLINT_MTIMER_SIZE, &rv_aclint_mtimer_ops,
                         &g_clint)) {
        return false;
    }
    if (!rv_bus_add_mmio(&g_bus, "uart0", RV_GUEST_UART_BASE,
                         RV_UART_SIZE, &rv_uart_ops, &g_uart)) {
        return false;
    }

    for (unsigned i = 0; i < sizeof(g_periph_map) / sizeof(g_periph_map[0]); i++) {
        /* Identity map: host base == guest base. */
        if (!rv_bus_add_passthru(&g_bus, g_periph_map[i].name,
                                 g_periph_map[i].base,
                                 g_periph_map[i].size,
                                 (uintptr_t)g_periph_map[i].base,
                                 g_periph_map[i].perm, RV_WANY)) {
            return false;
        }
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* Diagnostics                                                         */
/* ------------------------------------------------------------------ */

static const char *cause_name(uint32_t mcause)
{
    if (mcause & RV_CAUSE_INTERRUPT) {
        return "interrupt";
    }
    switch (mcause) {
    case RV_EXC_INSN_MISALIGNED:    return "instruction address misaligned";
    case RV_EXC_INSN_ACCESS_FAULT:  return "instruction access fault";
    case RV_EXC_ILLEGAL_INSN:       return "illegal instruction";
    case RV_EXC_BREAKPOINT:         return "breakpoint";
    case RV_EXC_LOAD_MISALIGNED:    return "load address misaligned";
    case RV_EXC_LOAD_ACCESS_FAULT:  return "load access fault";
    case RV_EXC_STORE_MISALIGNED:   return "store address misaligned";
    case RV_EXC_STORE_ACCESS_FAULT: return "store access fault";
    case RV_EXC_ECALL_M:            return "environment call";
    default:                        return "unknown";
    }
}

static void report_state(void)
{
    console_puts("\n-- guest state --\n  pc     ");
    console_puthex(g_hart.pc);

    /*
     * mcause is only meaningful once something has trapped. Decoding it
     * unconditionally reports "instruction address misaligned" for a clean
     * run, because that cause happens to be code 0.
     */
#if RV_ENABLE_STATS
    console_puts("\n  traps  ");
    console_putu(g_hart.trap_count);
    if (g_hart.trap_count == 0u) {
        console_puts("\n  sp     ");
        console_puthex(g_hart.x[2]);
        console_puts("  ra    ");
        console_puthex(g_hart.x[1]);
        console_putc('\n');
        return;
    }
#endif

    console_puts("\n  mcause ");
    console_puthex(g_hart.mcause);
    console_puts("  (");
    console_puts(cause_name(g_hart.mcause));
    console_puts(")\n  mepc   ");
    console_puthex(g_hart.mepc);
    console_puts("  mtval ");
    console_puthex(g_hart.mtval);
    console_puts("\n  sp     ");
    console_puthex(g_hart.x[2]);
    console_puts("  ra    ");
    console_puthex(g_hart.x[1]);
    console_putc('\n');
}

/* ------------------------------------------------------------------ */
/* Entry                                                               */
/* ------------------------------------------------------------------ */

int main(void)
{
    HAL_Init();
    SystemClock_Config();
    console_init();
    dwt_init();

#if RV32_NATIVE_COREMARK
    /*
     * Native baseline: the same CoreMark sources compiled for Cortex-M4
     * and run directly, with no emulation, so the interpreter and JIT
     * numbers can be put against something absolute.
     */
    {
        extern int coremark_native_main(void);
        console_puts("\n\nrv32cortex-m: NATIVE CoreMark on Cortex-M4 @ ");
        console_putu(SystemCoreClock / 1000000u);
        console_puts(" MHz\n\n");
        const uint32_t c0 = dwt_cycles();
        (void)coremark_native_main();
        console_puts("\n-- native --\n  host     ");
        console_putu(dwt_cycles() - c0);
        console_puts(" cycles\n");
        for (;;) { __WFI(); }
    }
#endif
    /* Built from the configured extensions rather than hardcoded, so the
     * banner cannot drift from what the core actually implements. */
    console_puts("\n\nrv32cortex-m: RV32I"
#if RV_EXT_M
                 "M"
#endif
#if RV_EXT_A
                 "A"
#endif
#if RV_EXT_F
                 "F"
#endif
#if RV_EXT_C
                 "C"
#endif
/* B is exactly Zba+Zbb+Zbs, and is what misa reports; Zbc is separate. */
#if RV_EXT_ZBA && RV_EXT_ZBB && RV_EXT_ZBS
                 "B"
#endif
#if RV_EXT_ZBC
                 "_zbc"
#endif
                 " on Cortex-M4 @ ");
    console_putu(SystemCoreClock / 1000000u);
    console_puts(" MHz\n");

    if (!build_address_space()) {
        console_puts("fatal: could not build the guest address space\n");
        Error_Handler();
    }

    rv_hart_init(&g_hart, &g_bus, 0u);
    rv_clint_init(&g_clint, &g_hart);
    rv_aplic_init(&g_aplic, &g_hart);
    rv_aplic_set_eoi(&g_aplic, aplic_unmask_line, NULL);
    bridged_irqs_init();
    rv_uart_init(&g_uart, guest_uart_tx, guest_uart_rx, NULL);
    g_hart.ecall = guest_ecall;
    g_hart.cache = &g_cache_ops;

#if RV_ENABLE_JIT
    rv_jit_set_code_buffer(g_jit_code, sizeof(g_jit_code));
    rv_backend = &rv_backend_jit;
    if (rv_backend->init != NULL && !rv_backend->init(&g_hart)) {
        console_puts("jit init failed; falling back to the interpreter\n");
        rv_backend = &rv_backend_interp;
    }
#endif

    /*
     * The image is linked to run from guest RAM, so copy it out of flash.
     * Guests linked for the ROM window can skip this and reset straight to
     * RV_GUEST_ROM_BASE.
     */
    if (rv_guest_image_size > GUEST_RAM_SIZE) {
        console_puts("fatal: guest image larger than guest RAM\n");
        Error_Handler();
    }
    memcpy(GUEST_RAM_BASE_PTR, rv_guest_image, rv_guest_image_size);

    console_puts("guest  ");
    console_putu(rv_guest_image_size);
    console_puts(" bytes at ");
    console_puthex(RV_GUEST_RESET_PC);
    console_puts("\nram    ");
    console_putu(GUEST_RAM_SIZE / 1024u);
    console_puts(" KiB (");
    console_putu(GUEST_RAM_SIZE);
    console_puts(" bytes)\nbackend ");
    console_puts(rv_backend->name);
    console_puts("\n\n");

    rv_hart_reset(&g_hart, RV_GUEST_RESET_PC);
    rv_hart_boot(&g_hart, RV_GUEST_RAM_BASE, GUEST_RAM_SIZE);

    const uint32_t cycles_per_tick = SystemCoreClock / RV_CLINT_HZ;
    const uint32_t start_cycles = dwt_cycles();
    uint64_t retired_total = 0;

    for (;;) {
        uint32_t retired = 0;
        const rv_run_reason_t why = rv_run(&g_hart, RV_RUN_SLICE, &retired);
        retired_total += retired;

        /* Guest time tracks real time through the DWT cycle counter. */
        rv_clint_set_time(&g_clint,
                          (uint64_t)(dwt_cycles() - start_cycles) / cycles_per_tick);

        if (why == RV_RUN_HALTED) {
            break;
        }
        if (why == RV_RUN_WFI && g_hart.mie == 0u) {
            console_puts("\nguest parked in WFI with no interrupts enabled\n");
            break;
        }
    }

    const uint32_t elapsed = dwt_cycles() - start_cycles;

    console_puts("\n-- done --\n  retired  ");
    console_putu((uint32_t)retired_total);
    console_puts(" instructions\n  host     ");
    console_putu(elapsed);
    console_puts(" cycles\n  ratio    ");
    if (retired_total != 0u) {
        /* Host ARM cycles per emulated RISC-V instruction, x100 so the
         * fractional part survives integer division. */
        const uint32_t x100 = (uint32_t)((uint64_t)elapsed * 100u / retired_total);
        console_putu(x100 / 100u);
        console_putc('.');
        console_putu((x100 % 100u) / 10u);
        console_putu(x100 % 10u);
        console_puts(" host cycles per guest instruction\n  speed    ");
        const uint32_t kips =
            (uint32_t)((uint64_t)retired_total * (SystemCoreClock / 1000u) / elapsed);
        console_putu(kips);
        console_puts(" KIPS\n");
    }

#if RV_ENABLE_JIT
    if (rv_backend == &rv_backend_jit) {
        rv_jit_stats_t js;
        rv_jit_get_stats(&js);
        console_puts("\n-- jit --\n  blocks   ");
        console_putu(js.blocks);
        console_puts("\n  code     ");
        console_putu(js.code_used);
        console_putc('/');
        console_putu(js.code_size);
        console_puts(" bytes\n  blks/xlat ");
        console_putu(js.translations);
        console_puts("\n  compact  ");
        console_putu(js.compactions);
        console_puts(" (");
        console_putu(js.evictions);
        console_puts(" evicted)\n  flushes  ");
        console_putu(js.flushes);
        /*
         * Instructions the translator declined and the interpreter ran.
         * A high share here is the first place to look when the speedup
         * is smaller than expected: it names exactly which encodings are
         * worth teaching the translator next.
         */
        console_puts("\n  interp   ");
        console_putu(js.interp_fallbacks);
        console_puts(" instructions fell back\n  helpers  muldiv ");
        console_putu(js.alu_calls_muldiv);
        console_puts("  clmul ");
        console_putu(js.alu_calls_clmul);
        console_puts("  bit ");
        console_putu(js.alu_calls_bit);
        console_puts("\n  pt hits  ");
        console_putu(js.pt_hits);
        console_puts(" armed ");
        console_putu(js.pt_armed);
        console_puts("\n  blk entr ");
        console_putu(js.block_entries);
        /*
         * Reads per block that uses the register, x100. Below 100 a cache
         * cannot pay: the block would spend a load to save fewer than one.
         */
        {
            static const char *const nm[4] = { "sp", "ra", "a0", "a1" };
            console_puts("\n  reads/blk");
            for (unsigned i = 0; i < 4u; i++) {
                console_putc(' ');
                console_puts(nm[i]);
                console_putc('=');
                if (js.hot_blocks[i] != 0u) {
                    const uint32_t x100 = js.hot_reads[i] * 100u / js.hot_blocks[i];
                    console_putu(x100 / 100u);
                    console_putc('.');
                    console_putu((x100 % 100u) / 10u);
                    console_putu(x100 % 10u);
                } else {
                    console_puts("-");
                }
                console_puts(" in ");
                console_putu(js.hot_blocks[i]);
            }
        }
        console_putc('\n');
    }
#endif

    report_state();

    for (;;) {
        __WFI();
    }
}
