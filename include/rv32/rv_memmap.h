/* SPDX-License-Identifier: Apache-2.0 */
/*
 * rv_memmap.h - Guest physical memory map.
 *
 * Shared by every platform so guest binaries are portable between the host
 * simulator and the STM32 firmware.
 *
 *   0x0200_0000  CLINT          virtual   timer + software interrupt
 *   0x1000_0000  UART0          virtual   NS16550 console
 *   0x2000_0000  ROM            guest image, execute-only from host flash
 *   0x4000_0000  PERIPH         passthru  1:1 onto the ARM peripheral space
 *   0x8000_0000  RAM            guest RAM carved out of ARM SRAM
 *
 * The peripheral window is an identity map and that is not a coincidence:
 * the STM32 places its APB1/APB2/AHB1/AHB2 peripherals across
 * 0x4000_0000..0x5FFF_FFFF, exactly the range a RISC-V platform leaves
 * free for memory-mapped I/O. A guest driver written against the STM32
 * reference manual therefore uses the same addresses the manual lists,
 * with no translation to reason about.
 */
#ifndef RV32_RV_MEMMAP_H
#define RV32_RV_MEMMAP_H

#define RV_GUEST_CLINT_BASE     0x02000000u
#define RV_GUEST_UART_BASE      0x10000000u
#define RV_GUEST_ROM_BASE       0x20000000u

#define RV_GUEST_PERIPH_BASE    0x40000000u
#define RV_GUEST_PERIPH_SIZE    0x20000000u

#define RV_GUEST_RAM_BASE       0x80000000u

/* Where a guest starts executing unless the platform says otherwise. */
#define RV_GUEST_RESET_PC       RV_GUEST_RAM_BASE

#endif /* RV32_RV_MEMMAP_H */
