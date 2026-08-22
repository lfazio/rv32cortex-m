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
#include "emu/emu_elf.h"

#include <string.h>

/*
 * Where the guest starts. EMU_GUEST_RESET_PC for a flat binary, and the
 * ELF's own e_entry for an ELF -- set while the address space is built,
 * because that is where the program headers are read.
 */
static uint32_t g_entry = EMU_GUEST_RESET_PC;

uint32_t emu_guest_entry(void)
{
    return g_entry;
}

bool emu_build_address_space(emu_bus_t *bus, emu_uart_t *uart)
{
    emu_bus_init(bus);

    /*
     * Guest RAM first, because an ELF's segments are placed against it:
     * emu_elf_map has to be able to ask whether a segment lands inside
     * this window, and it can only copy one that does.
     */
    if (!emu_bus_add_ram(bus, "ram", EMU_GUEST_RAM_BASE,
                         emu_board_ram, emu_board_ram_size)) {
        return false;
    }

    /*
     * The image.
     *
     * A flat binary is mapped whole, read-only, at EMU_GUEST_ROM_BASE:
     * its .text and .rodata are *linked* there and execute in place, so
     * however large they are they cost the guest no RAM -- which is what
     * lets an architecture test needing 345 KiB run on a part with 243
     * KiB of it. The .data initialiser is in there too, and the guest's
     * own start.S copies it across.
     *
     * An **ELF** is placed by its program headers instead, which is the
     * same arrangement arrived at from the other end: segments outside
     * guest RAM are mapped read-only straight out of the image, and only
     * what lands in RAM is copied. Nothing has to agree about a link
     * address in advance, so the file a person builds and debugs is the
     * file the board runs.
     *
     * The entry point comes from the ELF too, which is why g_entry
     * exists -- a flat binary starts at EMU_GUEST_RESET_PC and an ELF
     * starts wherever it says.
     */
    g_entry = EMU_GUEST_RESET_PC;

    if (emu_elf_is_elf(emu_board_img, emu_board_img_size)) {
        const char *const err =
            emu_elf_map(bus, emu_board_img, emu_board_img_size,
                        EMU_ELF_ANY_MACHINE, 0u,
                        EMU_GUEST_RAM_BASE, emu_board_ram_size,
                        &g_entry, NULL);

        if (err != NULL) {
            emu_console_printf("elf: %s\n", err);
            return false;
        }
    } else if (!emu_bus_add_rom(bus, "flash", EMU_GUEST_ROM_BASE,
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

/*
 * Bring a guest up: address space, the frontend's own devices, cleared
 * RAM and state, reset and boot.
 *
 * Split out because an upload has to repeat all of it. The bus is rebuilt
 * rather than patched, because the image region's base *and* length both
 * move when a different image arrives and emu_bus cannot resize a region
 * in place -- and that is why the frontend's devices go back on every
 * time: emu_bus_init clears the table, so a rebuild that skipped them
 * would take the interrupt controller away from a guest that had it a
 * moment earlier.
 *
 * The cores are *not* re-opened. They already exist, hold state the
 * frontend allocated, and are what the gdb stub is pointed at; a reload
 * replaces the guest, not the machine.
 *
 * The guest RAM is zeroed rather than left: without that, one test's
 * leftovers become the next test's initial state and a suite's results
 * start depending on the order it ran in. The exit status is cleared for
 * the same reason -- a guest that halts without calling exit() otherwise
 * reports whatever the last one returned, so every test after the first
 * passing one looks like it passed.
 */
bool emu_start_guest(emu_system_t *sys, emu_bus_t *buses, emu_uart_t *uart,
                     emu_guest_exit_t *exit_state)
{
    const emu_cpu_ops_t *const ops = sys->ops;

    for (unsigned i = 0; i < sys->ncores; i++) {
        emu_bus_t *const bus = &buses[i];

        if (!emu_build_address_space(bus, uart)) {
            return false;
        }

        /*
         * Hand the image to the frontend before its devices go on, for
         * one that maps it at an architectural address of its own --
         * G4MH's code flash at zero. RV32 leaves the hook NULL, because
         * the region emu_build_address_space added is already where its
         * guests link.
         */
        if (ops->set_image != NULL) {
            ops->set_image(emu_board_img, emu_board_img_size);
        }

        if ((ops->add_shared_devices != NULL &&
             !ops->add_shared_devices(bus)) ||
            (ops->add_core_devices != NULL &&
             !ops->add_core_devices(sys->core[i].cpu, bus, i))) {
            return false;
        }
    }

    memset(emu_board_ram, 0, emu_board_ram_size);

    if (exit_state != NULL) {
        exit_state->code = 0u;
        exit_state->exited = false;
    }

    emu_system_reset(sys, g_entry);
    emu_system_boot(sys, EMU_GUEST_RAM_BASE, emu_board_ram_size);
    return true;
}
