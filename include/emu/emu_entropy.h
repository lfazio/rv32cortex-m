/* SPDX-License-Identifier: Apache-2.0 */
/*
 * emu_entropy.h - the platform's entropy source, for RISC-V's `seed` CSR
 * (Zkr) or any frontend that asks.
 *
 * A frontend calls emu_entropy16() and knows nothing of where the bits
 * come from; the platform hands its source over once at start-up
 * (emu_main.c, from board_entropy16): on the host the CPU's RDSEED where
 * it has one and getrandom(2) otherwise, on an STM32 its RNG peripheral
 * through the vendor HAL. A harness that needs two runs to see the same
 * bits -- fuzz-rv32-jit's interpreter against its JIT -- sets a source
 * of its own and restarts it for each.
 *
 * The four answers are the four Zkr's OPST field reports, so a guest
 * can be told the truth: sixteen bits; none yet (ask again); the source
 * testing itself, as an STM32 RNG recovering from a seed error does
 * (ask again); or none ever. With no source set the answer is DEAD --
 * never bits that only look random.
 */
#ifndef EMU_ENTROPY_H
#define EMU_ENTROPY_H

#include <stdint.h>

typedef enum {
    EMU_ENTROPY_OK,      /* *out holds 16 fresh bits      */
    EMU_ENTROPY_WAIT,    /* none ready now                */
    EMU_ENTROPY_TESTING, /* the source is checking itself */
    EMU_ENTROPY_DEAD     /* none, and none will come      */
} emu_entropy_t;

typedef emu_entropy_t (*emu_entropy_fn)(uint16_t *out);

/* The source; NULL takes it away. Set before any core runs. */
void emu_entropy_set(emu_entropy_fn fn);

/*
 * Sixteen bits, or why not. Each call takes bits no earlier call was
 * given: a source must not hand the same bits out twice.
 */
emu_entropy_t emu_entropy16(uint16_t *out);

#endif /* EMU_ENTROPY_H */
