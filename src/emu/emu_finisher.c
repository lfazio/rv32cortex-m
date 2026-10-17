/* SPDX-License-Identifier: Apache-2.0 */
/*
 * emu_finisher.c - the test finisher: see emu_finisher.h.
 *
 * Reads answer zero, at any width, because OpenSBI's syscon regmap
 * updates the register with a read-modify-write: a read that faulted
 * would turn the power-off into a bus error in the firmware.
 */

#include "emu/emu_finisher.h"

static emu_fault_t finisher_read(void *ctx, uint32_t off, uint32_t size,
                                 uint32_t *out)
{
    (void)ctx;
    (void)off;
    (void)size;
    *out = 0u;
    return EMU_FAULT_NONE;
}

static emu_fault_t finisher_write(void *ctx, uint32_t off, uint32_t size,
                                  uint32_t val)
{
    const emu_finisher_t *const f = (const emu_finisher_t *)ctx;

    if (off != 0u || size != 4u || f->finish == NULL) {
        return EMU_FAULT_NONE; /* only the word at 0 means anything */
    }
    switch (val & 0xFFFFu) {
    case EMU_FINISHER_PASS:
        f->finish(f->ctx, EMU_FINISH_EXIT, 0u);
        break;
    case EMU_FINISHER_FAIL:
        /* The code is the high half -- and a failure never reads as 0,
         * which is a pass. */
        f->finish(f->ctx, EMU_FINISH_EXIT,
                  ((val >> 16) != 0u) ? (val >> 16) : 1u);
        break;
    case EMU_FINISHER_RESET:
        f->finish(f->ctx, EMU_FINISH_RESET, 0u);
        break;
    default:
        break;
    }
    return EMU_FAULT_NONE;
}

const emu_dev_ops_t emu_finisher_ops = {
    .read = finisher_read,
    .write = finisher_write,
    .tick = NULL,
};
