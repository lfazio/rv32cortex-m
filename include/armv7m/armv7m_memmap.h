/* SPDX-License-Identifier: Apache-2.0 */
/*
 * armv7m_memmap.h - where an ARMv7-M guest finds its memory: the M7's
 * map, as the STM32F746 has it (RM0385 section 2.2).
 *
 *   0x0000_0000  ITCM     16 KiB, executable RAM at zero
 *   0x0800_0000  FLASH    the image, read-only, as the part's flash
 *   0x1000_0000  UART0    the shared NS16550 console (emu_memmap.h)
 *   0x2000_0000  RAM      the DTCM's 64 KiB, then SRAM1 and SRAM2,
 *                         contiguous as on the part, for as much as the
 *                         platform gives
 *   0x4000_0000  PERIPH   the passthrough window (emu_memmap.h)
 *   0xE000_0000  SCS      the core's own: NVIC, SysTick, SCB, MPU
 *
 * The other frontends keep emu_memmap.h's shared map, which put this
 * guest's image at 0x2000_0000 -- the DTCM's address -- and its RAM at
 * 0x8000_0000, where the M7 has external memory. A program linked for
 * the F746 now runs as linked: its vector table at the flash, its stack
 * in the DTCM, code it copies into the ITCM executable there.
 *
 * The TCMs are RAM like any other here -- the emulator has no wait
 * states to model -- and the ITCM comes out of the front of the RAM the
 * platform gives (emu_guest_map_t), so the frontend owns no memory of its
 * own. On the F746 itself the guest's ITCM and DTCM are host SRAM, not
 * the host's TCMs, which the emulator's own loop and translated code use.
 */
#ifndef ARMV7M_MEMMAP_H
#define ARMV7M_MEMMAP_H

#define ARMV7M_GUEST_ITCM_BASE 0x00000000u
#define ARMV7M_GUEST_ITCM_SIZE 0x00004000u
#define ARMV7M_GUEST_FLASH_BASE 0x08000000u
#define ARMV7M_GUEST_RAM_BASE 0x20000000u /* the DTCM's, then the SRAMs */
#define ARMV7M_GUEST_DTCM_SIZE 0x00010000u

#endif /* ARMV7M_MEMMAP_H */
