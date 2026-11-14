/* SPDX-License-Identifier: Apache-2.0 */
/*
 * emu_entropy.c - the platform's entropy source. See emu_entropy.h.
 *
 * One pointer, set before any core runs and read by every one after:
 * the sources themselves are what must be safe to call from two cores
 * at once (the host's RDSEED and getrandom are; an STM32 runs one core).
 */
#include "emu/emu_entropy.h"

#include <stddef.h>

static emu_entropy_fn g_source;

void emu_entropy_set(emu_entropy_fn fn)
{
    g_source = fn;
}

emu_entropy_t emu_entropy16(uint16_t *out)
{
    if (g_source == NULL) {
        return EMU_ENTROPY_DEAD;
    }
    return g_source(out);
}
