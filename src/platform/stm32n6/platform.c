/* SPDX-License-Identifier: Apache-2.0 */
/*
 * platform.c - what the stm32n6 owes the shared runner.
 *
 * The runner is emu_main.c and is the same on every board. This is the
 * rest: where guest RAM is, which peripheral addresses the guest may
 * reach, and which real interrupt lines are bridged to it. See
 * emu_board.h for why the division falls here.
 *
 * This is the third board and the first where the *policy table* had to
 * be rewritten rather than copied. The F446's table survived the move to
 * the F746 unchanged, and that was a checked result -- PWR, RCC and the
 * flash interface sit at identical addresses in RM0390 and RM0385. None
 * of it transfers here: the N6's peripheral space is a different map at
 * different addresses with a different set of things that can take the
 * emulator down. The check that entry recommended is what said so.
 */

#include "stm32n6xx_hal.h"
#include "board.h"
#include "emu_board.h"

#include "emu/emu_bus.h"
#include "emu/emu_cpu.h"
#include "emu/emu_memmap.h"

const char *const board_core_name = "Cortex-M55";

/*
 * Guest RAM is not a fixed-size array: the link script places it between
 * the firmware's .bss and the stack and gives it everything in between,
 * so the guest automatically receives all the AXISRAM the ARM side is not
 * using (see .guest_ram in the link script).
 *
 * It is about six times what the F746 can offer, and for a reason worth
 * stating: there is no internal flash on this part, so the firmware's
 * read-only half is in RAM too and the guest still comes out ahead --
 * everything here is one 4 MiB pool rather than 1 MiB of flash and 320 KiB
 * of SRAM.
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
 * Guest 0x50000000..0x5FFFFFFF maps one-to-one onto the STM32N6's own
 * peripheral space, so a guest driver uses exactly the addresses printed
 * in RM0486 with no translation to reason about. This is what keeps
 * peripheral drivers in the guest and the ARM-side port thin.
 *
 * **The window moved, and only this part explains why.** The F446 and
 * F746 map 0x40000000..0x5FFFFFFF, because on those parts that whole span
 * *is* the peripheral space. Here it is two aliases of one peripheral
 * space: 0x4xxx_xxxx is the non-secure view and 0x5xxx_xxxx the secure
 * one. This firmware runs secure -- the boot ROM enters the FSBL there,
 * and the device header notices, so `USART1` in the C below already means
 * 0x5200_1000 -- so the secure alias is the one that works and it is the
 * only one mapped.
 *
 * Leaving the non-secure half *unmapped* is deliberate rather than
 * unfinished. An access to it is subject to the resource-isolation
 * framework, which by default answers a secure-only peripheral with a bus
 * error -- and CLAUDE.md records what a bus error is here: a HardFault in
 * the *emulator*, so the firmware dies rather than the guest's test
 * failing. Unmapped, the same access is a clean guest fault, which is the
 * report the design promises. Same reasoning as the holes in the STM32
 * tables, arrived at from the other direction.
 *
 * What is withheld, and why each one would take the emulator down with
 * the guest rather than merely misbehave:
 *
 *   debug         a guest that turns the debug port off makes the board
 *                 unreflashable without a boot-mode change
 *   RIFSC/RISAF   the isolation units. A guest writing them can revoke
 *                 the *firmware's* access to its own RAM and console
 *   BSEC          one-time-programmable fuses. Irreversible, and one of
 *                 the few ways to permanently brick this part
 *   PWR           as on the other two: dropping the supply the core runs
 *                 from stalls it
 *   RCC clocks    reconfiguring the PLL kills the clock the emulator
 *                 runs on
 *
 * Note what is deliberately *not* withheld: the RCC reset and clock-enable
 * registers. A guest driver has to be able to ungate its own peripheral's
 * clock, and denying that would push every driver back into the firmware
 * -- the whole premise of this emulator.
 *
 * **The RCC needed five entries where the F7 needed two**, because this
 * part's is a different shape in two ways. The enable and reset registers
 * are mirrored by a *set* alias at +0x800 and a *clear* alias at +0x1000,
 * so withholding the direct block alone would leave the same bits
 * reachable twice over; the aliases are read-only here and a guest does
 * read-modify-write on the direct registers exactly as an F4 driver does
 * on AHB1ENR. And DIVENR sits at 0x240, in the middle of the enable
 * block rather than with the rest of the clock tree, so it is carved out
 * on its own: it gates the dividers, one of which feeds the CPU.
 *
 * The console is USART1 at 0x5200_1000, inside the guest's read-write
 * span -- exactly as USART3 was on the F746 and USART2 on the F446. A
 * guest that reprograms it silences the console, which is the same
 * bargain as everywhere else here: the guest owns the peripherals.
 */
static const struct {
    const char *name;
    uint32_t    base;
    uint32_t    size;
    uint8_t     perm;
} g_periph_map[] = {
    /* APB1, AHB1, APB2, AHB2: timers, SPI, I2C, USART, GPDMA, ADC */
    { "apb1..ahb2", 0x50000000u, 0x04000000u, EMU_PERM_RW },
    /* DAP ROM, DBGMCU, DFT: the debug subsystem */
    { "debug",      0x54000000u, 0x00003000u, EMU_PERM_R  },
    /* Rest of APB3 and AHB3 up to the isolation units: RNG, HASH, CRYP,
     * SAES, PKA */
    { "apb3+ahb3",  0x54003000u, 0x00021000u, EMU_PERM_RW },
    /* RIFSC, IAC and RISAF1..23 -- who may reach what, including us */
    { "isolation",  0x54024000u, 0x00014000u, EMU_PERM_R  },
    /* Rest of AHB3 and APB4 up to the fuses */
    { "ahb3b+apb4", 0x54038000u, 0x01FD1000u, EMU_PERM_RW },
    /* One-time-programmable fuses: irreversible */
    { "bsec",       0x56009000u, 0x00001000u, EMU_PERM_R  },
    /* DTS and the AHB4 GPIO ports up to PWR */
    { "dts+gpio",   0x5600A000u, 0x0001A800u, EMU_PERM_RW },
    { "pwr",        0x56024800u, 0x00000400u, EMU_PERM_R  },
    /* CRC and EXTI, up to RCC */
    { "crc+exti",   0x56024C00u, 0x00003400u, EMU_PERM_RW },
    /* RCC: CR, CFGR, the four PLLs, the twenty IC dividers, CCIPRx */
    { "rcc-clock",  0x56028000u, 0x00000208u, EMU_PERM_R  },
    /* RCC peripheral resets: the guest's to drive */
    { "rcc-reset",  0x56028208u, 0x00000038u, EMU_PERM_RW },
    /* RCC DIVENR alone: one of these dividers feeds the CPU */
    { "rcc-diven",  0x56028240u, 0x00000004u, EMU_PERM_R  },
    /* RCC peripheral clock enables: the guest's to drive */
    { "rcc-en",     0x56028244u, 0x0000007Cu, EMU_PERM_RW },
    /* The set and clear aliases of all of the above, and the security
     * configuration. Read-only, or the carve-outs above are decorative. */
    { "rcc-alias",  0x560282C0u, 0x00001D40u, EMU_PERM_R  },
    /* APB5 and AHB5: LTDC, GFXMMU, DCMIPP, SDMMC, XSPI, Ethernet, USB */
    { "apb5+ahb5",  0x5602A000u, 0x09FD6000u, EMU_PERM_RW },
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
 * An APLIC source number *is* the NVIC line number, so there is no mapping
 * table -- and, more to the point, none in the guest either. A driver that
 * would call HAL_NVIC_EnableIRQ(TIM6_IRQn) writes that same 120 to the
 * APLIC's setienum, and the two numbering spaces never have to be
 * reconciled.
 *
 * **This part is the first where that identity has a ceiling in sight.**
 * RV_APLIC_SOURCES is 128 and the F7's bridged line is 54; here TIM6 is
 * 120 and the table runs to 159. Anything above 127 -- USART, SDMMC,
 * Ethernet, most of what a real driver would want -- needs
 * RV_APLIC_SOURCES raised to 256 before it can be bridged at all, and
 * `irq_is_bridged` would answer false rather than fail, so it would
 * present as an interrupt that simply never arrives.
 */
static const IRQn_Type g_bridged[] = {
    TIM6_IRQn,
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

void TIM6_IRQHandler(void)
{
    irq_line_entry(TIM6_IRQn);
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
