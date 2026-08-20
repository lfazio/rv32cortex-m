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

#include <string.h>

bool emu_build_address_space(emu_bus_t *bus, emu_uart_t *uart)
{
    emu_bus_init(bus);

    /*
     * The image, read-only, as the guest's flash. Its .text and .rodata
     * are *linked* here and execute in place, so however large they are
     * they cost the guest no RAM -- which is what lets an architecture
     * test needing 345 KiB run on a part with 264 KiB of it. The .data
     * initialiser is in here too, and start.S copies it across.
     */
    if (!emu_bus_add_rom(bus, "flash", EMU_GUEST_ROM_BASE,
                         emu_board_img, emu_board_img_size)) {
        return false;
    }

    /*
     * Guest RAM, whole and starting at its base.
     *
     * This used to be two regions with a *boundary* between them: the
     * image was linked entirely in RAM, so the platform served the
     * read-only part from flash up to __guest_ro_end and RAM after it,
     * and an upload had to arrive in two pieces for the board to learn
     * where that was. With the run addresses in separate regions there
     * is nothing to infer -- flash is flash and RAM is RAM -- and the
     * split, the two-piece upload and the exactness they depended on are
     * all gone.
     */
    if (!emu_bus_add_ram(bus, "ram", EMU_GUEST_RAM_BASE,
                         emu_board_ram, emu_board_ram_size)) {
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

/*
 * Bring a guest up: address space, the frontend's own devices, cleared
 * RAM and state, reset and boot.
 *
 * Split out because an upload has to repeat all of it. The bus is rebuilt
 * rather than patched, because the read-only region's base *and* length
 * both move when a different image arrives and emu_bus cannot resize a
 * region in place -- and that is why the frontend's devices go back on
 * every time: emu_bus_init clears the table, so a rebuild that skipped
 * them would take the interrupt controller away from a guest that had it
 * a moment earlier.
 *
 * The guest RAM is zeroed rather than left: without that, one test's
 * leftovers become the next test's initial state and a suite's results
 * start depending on the order it ran in. The exit status is cleared for
 * the same reason -- a guest that halts without calling exit() otherwise
 * reports whatever the last one returned, so every test after the first
 * passing one looks like it passed.
 */
bool emu_start_guest(emu_core_t *core, emu_bus_t *bus, emu_uart_t *uart,
                     emu_guest_exit_t *exit_state)
{
    if (!emu_build_address_space(bus, uart)) {
        return false;
    }

    if (core->cpu != NULL) {
        const emu_cpu_ops_t *const ops = core->ops;

        emu_board_image_published();

        if ((ops->add_shared_devices != NULL &&
             !ops->add_shared_devices(bus)) ||
            (ops->add_core_devices != NULL &&
             !ops->add_core_devices(core->cpu, bus, 0u))) {
            return false;
        }
    }

    memset(emu_board_ram, 0, emu_board_ram_size);

    if (exit_state != NULL) {
        exit_state->code = 0u;
        exit_state->exited = false;
    }

    emu_core_reset(core, EMU_GUEST_RESET_PC);
    emu_core_boot(core, EMU_GUEST_RAM_BASE, emu_board_ram_size);
    return true;
}
