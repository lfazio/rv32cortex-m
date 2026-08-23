/* SPDX-License-Identifier: Apache-2.0 */
/*
 * platform.c - what the stm32f746 owes the shared runner.
 *
 * The runner is emu_main.c and is the same on every board. This is the
 * rest: where guest RAM is, which peripheral addresses the guest may
 * reach, and which real interrupt lines are bridged to it. See
 * emu_board.h for why the division falls here.
 */

#include "stm32f7xx_hal.h"
#include "board.h"
#include "emu_board.h"

#include "emu/emu_bus.h"
#include "emu/emu_cpu.h"
#include "emu/emu_memmap.h"

const char *const board_core_name = "Cortex-M7";

/*
 * Guest RAM is not a fixed-size array: the link script places it between
 * the firmware's .bss and the stack and gives it everything in between,
 * so the guest automatically receives all the SRAM the ARM side is not
 * using (see .guest_ram in the link script).
 */
extern uint8_t __guest_ram_start[];
extern uint8_t __guest_ram_end[];

/*
 * Mutable, because a board that can be reloaded moves these: an image
 * arriving over TFTP or through gdb's `load` repoints them and the
 * address space is rebuilt around the new numbers.
 */
const uint8_t *board_img      = NULL;
uint32_t       board_img_size = 0u;

uint8_t *board_ram      = NULL;
uint32_t board_ram_size = 0u;

/*
 * A difference of two linker symbols, which C will not accept in a static
 * initialiser however constant it is at run time -- which is why the
 * runner reads variables here rather than a macro.
 */
void board_ram_init(void)
{
    board_ram      = __guest_ram_start;
    board_ram_size = (uint32_t)(__guest_ram_end - __guest_ram_start);
}

/*
 * Peripheral passthrough policy.
 *
 * Guest 0x40000000..0x5FFFFFFF maps one-to-one onto the STM32's own
 * peripheral space, so a guest driver uses exactly the addresses printed
 * in RM0385 with no translation to reason about. This is what keeps
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
 *
 * This table is the same as the F446's, and that is a checked result
 * rather than an assumption: all three withheld blocks sit at identical
 * addresses in RM0385 and RM0390 -- PWR at 0x40007000, RCC at 0x40023800,
 * the flash interface at 0x40023C00. What this part adds (LPTIM1, SPDIFRX,
 * I2C4, CAN2, HDMI-CEC, UART7/8, SAI2, LCD-TFT, Ethernet, CRYP/HASH/RNG)
 * all falls inside spans that were already read-write, and none of it can
 * take the emulator down with the guest.
 *
 * The console moved to USART3 for this board, and USART3 at 0x40004800 is
 * inside the guest's read-write span -- exactly as USART2 was on the F446.
 * A guest that reprograms it silences the console, which is the same
 * bargain as everywhere else here: the guest owns the peripherals.
 */
static const struct {
    const char *name;
    uint32_t    base;
    uint32_t    size;
    uint8_t     perm;
} g_periph_map[] = {
    /* APB1 up to PWR: timers, RTC, WWDG, SPI2/3, USART2/3, UART4/5, I2C */
    { "apb1",       0x40000000u, 0x00007000u, EMU_PERM_RW },
    { "pwr",        0x40007000u, 0x00000400u, EMU_PERM_R  },
    /* Rest of APB1, all of APB2, GPIO and CRC */
    { "apb1b+apb2", 0x40007400u, 0x0001C400u, EMU_PERM_RW },
    /* RCC clock tree: CR, PLLCFGR, CFGR, CIR */
    { "rcc-clock",  0x40023800u, 0x00000010u, EMU_PERM_R  },
    /* RCC resets and peripheral clock enables: the guest's to drive */
    { "rcc-periph", 0x40023810u, 0x000003F0u, EMU_PERM_RW },
    /* Flash interface: ACR, keys, control, option bytes */
    { "flash-ctl",  0x40023C00u, 0x00000400u, EMU_PERM_R  },
    /* BKPSRAM, DMA1/2, USB OTG HS, and AHB2 up to 0x5FFFFFFF */
    { "ahb1b+ahb2", 0x40024000u, 0x1FFDC000u, EMU_PERM_RW },
};

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
 * with -- see board_irq_unmask, reached through the APLIC's eoi hook.
 *
 * Adding a peripheral is one table entry and one handler; the table is the
 * policy, the same way g_periph_map is for addresses.
 */
/*
 * An APLIC source number *is* the NVIC line number, so there is no mapping
 * table -- and, more to the point, none in the guest either. A driver that
 * would call HAL_NVIC_EnableIRQ(TIM6_DAC_IRQn) writes that same 54 to the
 * APLIC's setienum, and the two numbering spaces never have to be
 * reconciled. It is the reason RV_APLIC_SOURCES is 128 rather than 32.
 */
static const IRQn_Type g_bridged[] = {
    TIM6_DAC_IRQn,
};

static bool irq_is_bridged(uint32_t source)
{
    for (unsigned i = 0; i < sizeof(g_bridged) / sizeof(g_bridged[0]); i++) {
        if ((uint32_t)g_bridged[i] == source) {
            return true;
        }
    }
    return false;
}

static void irq_line_entry(IRQn_Type irqn)
{
    NVIC_DisableIRQ(irqn);
    emu_raise_irq((uint32_t)irqn, true);
}

void board_irq_unmask(void *ctx, uint32_t source)
{
    (void)ctx;
    if (irq_is_bridged(source)) {
        NVIC_ClearPendingIRQ((IRQn_Type)source);
        NVIC_EnableIRQ((IRQn_Type)source);
    }
}

/*
 * Enable the bridged lines at the NVIC. Priority is left at the default:
 * these handlers do almost nothing, and the emulator has no other interrupt
 * to rank them against.
 */
void board_irqs_init(void)
{
    for (unsigned i = 0; i < sizeof(g_bridged) / sizeof(g_bridged[0]); i++) {
        NVIC_EnableIRQ(g_bridged[i]);
    }
}

void TIM6_DAC_IRQHandler(void)
{
    irq_line_entry(TIM6_DAC_IRQn);
}
/*
 * The passthrough windows. Identity-mapped, so a guest driver writing
 * what its datasheet says reaches the real peripheral -- the point of
 * this emulator, and necessarily per-part.
 */
bool board_add_regions(emu_bus_t *bus)
{
    for (unsigned i = 0; i < sizeof(g_periph_map) / sizeof(g_periph_map[0]);
         i++) {
        if (!emu_bus_add_passthru(bus, g_periph_map[i].name,
                                  g_periph_map[i].base,
                                  g_periph_map[i].size,
                                  (uintptr_t)g_periph_map[i].base,
                                  g_periph_map[i].perm, EMU_WANY)) {
            return false;
        }
    }
    return true;
}
