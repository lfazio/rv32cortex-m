/* SPDX-License-Identifier: Apache-2.0 */
/*
 * emu_memmap.h - Guest physical memory map, the parts every frontend shares.
 *
 * Shared by every platform so guest binaries are portable between the host
 * simulator and the STM32 firmware.
 *
 *   0x1000_0000  UART0          virtual   NS16550 console
 *   0x2000_0000  FLASH          guest .text/.rodata, run from host flash
 *   0x4000_0000  PERIPH         passthru  1:1 onto the ARM peripheral space
 *   0x8000_0000  RAM            guest .data/.bss/stack, from ARM SRAM
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

#define EMU_GUEST_UART_BASE 0x10000000u
#define EMU_GUEST_ROM_BASE 0x20000000u

#define EMU_GUEST_PERIPH_BASE 0x40000000u
#define EMU_GUEST_PERIPH_SIZE 0x20000000u

/*
 * The display, when a platform provides one. Two regions, because the
 * pixels are RAM and only the control block is a device -- see
 * emu_dev.h. They sit between the flash window and the peripheral space,
 * which is the gap in this map that nothing else claims.
 */
#define EMU_GUEST_FB_BASE 0x30000000u /* control, EMU_FB_CTRL_SIZE  */
#define EMU_GUEST_FB_PIXELS 0x30100000u /* the buffer itself          */

/*
 * Keyboard and mouse. Two devices rather than one, for the reason a host
 * binds a separate evdev node to each -- see emu_dev.h. Adjacent, so a
 * guest that has one has the other at a known offset.
 */
#define EMU_GUEST_KBD_BASE 0x30001000u
#define EMU_GUEST_MOUSE_BASE 0x30002000u

#define EMU_GUEST_RAM_BASE 0x80000000u

/*
 * A frontend's own map, for one whose guests are linked for a part's
 * fixed addresses rather than for the shared ones above. ARMv7-M's is the
 * M7's -- the image where the F746's flash is, an ITCM at zero, RAM from
 * the DTCM on -- so that a program for the part runs as linked
 * (armv7m/armv7m_memmap.h). emu_cpu_ops_t::guest_map; NULL is the shared
 * map, which every other frontend uses.
 */
#ifndef __ASSEMBLER__ /* guests' start-up includes this file */
#include <stddef.h>
#include <stdint.h>

typedef struct emu_guest_map {
    uint32_t rom_base; /* the image, served read-only             */
    uint32_t ram_base; /* guest RAM: the platform's, less the TCM */
    /*
     * Executable RAM at a fixed address -- a core's ITCM -- taken from the
     * front of the RAM the platform gives, so that a frontend owns no
     * memory of its own (docs/memory.md). tcm_size 0: none.
     */
    uint32_t tcm_base;
    uint32_t tcm_size;
} emu_guest_map_t;

/* `map`, or the shared one when it is NULL. */
static inline const emu_guest_map_t *
emu_guest_map_or_shared(const emu_guest_map_t *map)
{
    static const emu_guest_map_t shared = {EMU_GUEST_ROM_BASE,
                                           EMU_GUEST_RAM_BASE, 0u, 0u};

    return (map != NULL) ? map : &shared;
}
#endif /* __ASSEMBLER__ */

/* Where a guest starts executing unless the platform says otherwise. */
/*
 * Reset into flash, not RAM.
 *
 * A guest's .text and .rodata are linked at EMU_GUEST_ROM_BASE and served
 * read-only from wherever the image already lives -- host flash on the
 * firmware -- so they cost the guest no RAM. Only .data, .bss and the
 * stack are in RAM, and start.S copies .data across at start-up from a
 * load address in flash.
 *
 * This used to reset into RAM with the whole image linked there, which
 * meant the platform had to know where the read-only part ended in order
 * to serve that much from flash and the rest from RAM. Nothing needs that
 * boundary now: flash is flash and RAM is RAM.
 */
#define EMU_GUEST_RESET_PC EMU_GUEST_ROM_BASE

/*
 * An interrupt controller source number is the host's interrupt number: on
 * the STM32 platform, the NVIC line. So a guest names TIM6's interrupt by
 * the same 54 that TIM6_DAC_IRQn has on the ARM side, and no translation
 * table exists on either side of the bridge.
 */
#define EMU_IRQ_TIM6_DAC 54u

#endif /* EMU_MEMMAP_H */
