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

/*
 * VLMAX for a vtype, or 0 for one this unit cannot hold -- which vset
 * answers with vill. One definition for the vector unit and for the
 * translator, which resolves a vsetvli's at translation.
 */
uint32_t rv_vec_vlmax(uint32_t vtype);

#if RV_EXT_F
/*
 * rv_fpu.c's single precision, an element at a time, for Zve32f: the
 * scalar instructions' NaN, flag and rounding rules, through the same
 * functions. rm is a rounding mode 0-4; flags accrue. With F alone too,
 * where the JIT's backends redo an operation in it (the target's vfop):
 * the 16-bit conversions and the estimates are Zve32f's only.
 */
typedef enum {
    RV_VF_ADD,
    RV_VF_SUB,
    RV_VF_MUL,
    RV_VF_DIV,
    RV_VF_SQRT,
    RV_VF_MIN,
    RV_VF_MAX,
    RV_VF_FMA, /* a * b + c, one rounding */
    RV_VF_EQ,  /* 1 or 0, quiet */
    RV_VF_LT,  /* signalling */
    RV_VF_LE,
    RV_VF_CLASS,
    RV_VF_TO_I32,
    RV_VF_TO_U32,
    RV_VF_FROM_I32,
    RV_VF_FROM_U32,
    RV_VF_TO_I16, /* saturating, as Spike's f32_to_i16 */
    RV_VF_TO_U16,
    RV_VF_REC7,   /* the 7-bit estimates */
    RV_VF_RSQRT7,
} rv_vf_op_t;

uint32_t rv_vf32(rv_vf_op_t op, uint32_t a, uint32_t b, uint32_t c, uint32_t rm,
                 uint32_t *flags);
#endif

#if RV_EXT_ZVE32F
/* f[r] as a single (the canonical NaN if not NaN-boxed), and its write. */
uint32_t rv_vf32_reg(const rv_hart_t *h, uint32_t r);
void rv_vf32_set_reg(rv_hart_t *h, uint32_t r, uint32_t v);
/* An instruction's flags, into fflags, FS Dirty if they change it. */
void rv_vf32_accrue(rv_hart_t *h, uint32_t flags);
extern const uint8_t rv_vf_rec7_table[128];
extern const uint8_t rv_vf_rsqrt7_table[128];
#endif

#endif /* RV32_RV_VEC_H */
