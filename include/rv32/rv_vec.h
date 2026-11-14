/* SPDX-License-Identifier: Apache-2.0 */
/*
 * rv_vec.h - the vector unit (Zve32x, in progress; rv_vec.c).
 */
#ifndef RV32_RV_VEC_H
#define RV32_RV_VEC_H

#include "rv32/rv_hart.h"

#include <stdbool.h>
#include <stdint.h>

/*
 * Whether a LOAD-FP or STORE-FP encoding is a vector access: widths 0,
 * 5, 6 and 7 (EEW 8, 16, 32, 64), where 1, 2 and 3 are the scalar FP
 * ones. Asked before anything about FS, which does not gate them.
 */
static inline bool rv_vec_is_mem(uint32_t insn)
{
    const uint32_t w = (insn >> 12) & 7u;

    return w == 0u || w >= 5u;
}

/*
 * One vector instruction: OP-V, or a vector load or store. Returns the
 * exception it raised, RV_EXC_NONE if none, with *tval its value. Every
 * vector instruction is refused while mstatus.VS is Off, and marks it
 * Dirty when it writes vector state.
 */
rv_exc_t rv_hart_vec(rv_hart_t *h, uint32_t insn, uint32_t *tval);

#endif /* RV32_RV_VEC_H */
