/* SPDX-License-Identifier: Apache-2.0 */
/*
 * emu_debug.c - see emu_debug.h for the three layers this is the middle
 * of.
 */

#include "emu_debug.h"
#include "emu_console.h"

void emu_debug_start(emu_system_t *sys, const emu_cpu_ops_t *ops)
{
    if (!board_gdb_wanted()) {
        return;
    }

    /*
     * The frontend states its own layout. Answering "no" is a normal
     * outcome -- the PowerPC frontend has no description yet -- and is
     * reported rather than treated as a failure, because a platform that
     * cannot serve gdb is still one that runs guests.
     */
    const emu_gdb_target_t *const gt =
        (ops->gdb_target != NULL) ? ops->gdb_target() : NULL;

    if (gt == NULL) {
        emu_console_printf("gdb    frontend %s has no target description\n",
                           ops->name);
        return;
    }

    const emu_gdb_flash_ops_t *flash = NULL;

    if (!board_gdb_start(&sys->core[0], gt, &flash)) {
        emu_console_printf("gdb    stub failed to start\n");
        return;
    }

    emu_console_printf("gdb    target remote %s\n", board_gdb_where());

    /*
     * After the line, so a person who is going to attach has read where
     * to attach to before this blocks.
     */
    board_gdb_wait();
}

uint32_t emu_debug_run(emu_system_t *sys, uint32_t budget, uint32_t *retired,
                       bool *all_idle)
{
    *all_idle = false;

    if (board_gdb_attached()) {
        uint32_t n = 0u;

        (void)board_gdb_run(budget, &n);
        *retired = n;
        return n;
    }

    *retired = emu_system_step(sys, budget, all_idle);
    return *retired;
}

void emu_debug_poll(void)
{
    board_gdb_poll();
}
