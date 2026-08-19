/* SPDX-License-Identifier: Apache-2.0 */
/*
 * emu_address_space.c - the guest's memory map, shared.
 *
 * The same four regions on every board, in the same order, differing
 * only in the numbers a board supplies through emu_board.h:
 *
 *   guest-ro   the image's read-only half, served from where it already
 *              lives -- host flash, typically -- so it costs no RAM
 *   ram        guest RAM, starting where that half ends
 *   rom        the whole image again, read-only, at EMU_GUEST_ROM_BASE.
 *              This is where the guest's own .data initialiser lives:
 *              start.S copies from __data_lma, which points here
 *   uart0      the NS16550 the guest prints through
 *
 * then whatever the board adds itself, which is the passthrough windows.
 *
 * **The split point is a placement decision, not a correctness one.**
 * `ro` says where flash stops answering and RAM starts, and the saving is
 * real -- 140 KiB of the 345 the largest architecture tests need. It used
 * to also be load-bearing, because the firmware installed the guest's
 * .data at the boundary; the guest does that itself now, so a board with
 * room to map the whole image writable may pass zero and lose nothing but
 * the saving.
 *
 * Rebuilt rather than patched when an image changes, because the
 * read-only region's base *and* length both move and emu_bus cannot
 * resize a region in place. That is also why the frontend's devices have
 * to go back afterwards: emu_bus_init clears the table, and a rebuild
 * that forgot would take the interrupt controller away from a guest that
 * had it a moment earlier.
 */

#include "emu_board.h"
#include "emu_console.h"

#include "emu/emu_memmap.h"
#include "emu/emu_dev.h"

bool emu_build_address_space(emu_bus_t *bus, emu_uart_t *uart)
{
    emu_bus_init(bus);

    const uint32_t ro = emu_board_img_ro;

    /*
     * Both regions are added even when `ro` is zero or the whole image,
     * because emu_bus rejects a zero-length region -- and a guest with no
     * .data is the common case here, not an edge one.
     */
    if (ro != 0u &&
        !emu_bus_add_rom(bus, "guest-ro", EMU_GUEST_RAM_BASE,
                         emu_board_img, ro)) {
        return false;
    }
    if (!emu_bus_add_ram(bus, "ram", EMU_GUEST_RAM_BASE + ro,
                         emu_board_ram, emu_board_ram_size)) {
        return false;
    }

    /*
     * The whole image, read-only. A guest linked for execute-in-place
     * reads its constants here, and every guest reads its .data
     * initialiser here -- which is why the host runner had to grow this
     * window too when the guest stopped being handed an initialised RAM.
     */
    if (!emu_bus_add_rom(bus, "rom", EMU_GUEST_ROM_BASE,
                         emu_board_img, emu_board_img_size)) {
        return false;
    }

    if (!emu_bus_add_mmio(bus, "uart0", EMU_GUEST_UART_BASE,
                          EMU_UART_SIZE, &emu_uart_ops, uart)) {
        return false;
    }

    /*
     * The board's own windows last, and the frontend's devices after
     * that: the interrupt controller and the timer belong to the guest
     * *architecture* rather than to a board, so the frontend maps them.
     */
    return emu_board_add_regions(bus);
}
