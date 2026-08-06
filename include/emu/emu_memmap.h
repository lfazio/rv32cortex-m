/* SPDX-License-Identifier: Apache-2.0 */
/*
 * emu_memmap.h - Guest physical memory map, the parts every frontend shares.
 *
 * Shared by every platform so guest binaries are portable between the host
 * simulator and the STM32 firmware.
 *
 *   0x1000_0000  UART0          virtual   NS16550 console
 *   0x2000_0000  ROM            guest image, execute-only from host flash
 *   0x4000_0000  PERIPH         passthru  1:1 onto the ARM peripheral space
 *   0x8000_0000  RAM            guest RAM carved out of ARM SRAM
 *
 * Each frontend adds the interrupt controller and timer its architecture
 * defines, in its own memmap header: rv32/rv_memmap.h places the RISC-V
 * CLINT and APLIC, g4mh/g4mh_memmap.h the RH850 INTC.
 *
 * The peripheral window is an identity map and that is not a coincidence:
 * the STM32 places its APB1/APB2/AHB1/AHB2 peripherals across
 * 0x4000_0000..0x5FFF_FFFF, exactly the range a RISC-V platform leaves
 * free for memory-mapped I/O. A guest driver written against the STM32
 * reference manual therefore uses the same addresses the manual lists,
 * with no translation to reason about.
 */
#ifndef EMU_MEMMAP_H
#define EMU_MEMMAP_H

#define EMU_GUEST_UART_BASE     0x10000000u
#define EMU_GUEST_ROM_BASE      0x20000000u

#define EMU_GUEST_PERIPH_BASE   0x40000000u
#define EMU_GUEST_PERIPH_SIZE   0x20000000u

#define EMU_GUEST_RAM_BASE      0x80000000u

/* Where a guest starts executing unless the platform says otherwise. */
#define EMU_GUEST_RESET_PC      EMU_GUEST_RAM_BASE

/*
 * An interrupt controller source number is the host's interrupt number: on
 * the STM32 platform, the NVIC line. So a guest names TIM6's interrupt by
 * the same 54 that TIM6_DAC_IRQn has on the ARM side, and no translation
 * table exists on either side of the bridge.
 */
#define EMU_IRQ_TIM6_DAC        54u

#endif /* EMU_MEMMAP_H */
