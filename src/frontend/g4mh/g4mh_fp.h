/* SPDX-License-Identifier: Apache-2.0 */
/*
 * g4mh_fp.h - one floating-point element, as the FPU and each FXU way
 * compute it. Private to the frontend: g4mh_fpu.c and g4mh_fxu.c.
 *
 * The FPU's scalar instructions and the FXU's four ways follow the same
 * rules, stated twice in R01UH0923EJ0130 -- 3.2.6.1 for FPSR, 3.2.6.2 for
 * FXSR -- and both statements say the same thing: subnormals flushed or
 * an unimplemented-operation exception (E) by FS, a result judged
 * subnormal *before* rounding, flush-to-nearest under FN, and the
 * product's own NaN results (tables 3.128 and 3.134), which are not
 * IEEE's: an invalid result is 7FFF FFFFH, not a canonical quiet NaN.
 * Written once, so the two units cannot disagree.
 *
 * An element knows nothing of FPSR or FXSR: it reads the three settings
 * it is given and reports what it raised. Committing the flags -- cause
 * bits only on an exception, preservation bits otherwise -- is each
 * unit's, because the FXU keeps them per way as well.
 */
#ifndef G4MH_FP_H
#define G4MH_FP_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint8_t rm; /* G4MH_RM_*: FPSR/FXSR bits 19..18                 */
    bool fs; /* flush subnormals: bit 17                            */
    bool fn; /* flush to nearest, in RN only: bit 23                */
    uint32_t flags; /* raised, G4MH_FPX_* with E: accumulates       */
    bool flushed; /* an input operand was flushed: IF               */
} g4mh_fp_t;

/* The settings, from FPSR or FXSR: the bits are in the same places. */
static inline void g4mh_fp_begin(g4mh_fp_t *e, uint32_t sr)
{
    e->rm = (uint8_t)((sr >> 18) & 3u);
    e->fs = (sr & (1u << 17)) != 0u;
    e->fn = (sr & (1u << 23)) != 0u;
    e->flags = 0u;
    e->flushed = false;
}

/* The operations whose result is a float of the operands' format. */
enum {
    G4MH_FOP_ADD,
    G4MH_FOP_SUB,
    G4MH_FOP_MUL,
    G4MH_FOP_DIV,
    G4MH_FOP_SQRT, /* one operand: x                                */
    G4MH_FOP_RECIP, /* 1/x, as DIVF computes it (table 3.129 (d))  */
    G4MH_FOP_RSQRT,
    G4MH_FOP_FMA, /* x*y + z                                        */
    G4MH_FOP_FMS, /* x*y - z                                        */
    G4MH_FOP_FNMA, /* -(x*y + z)                                    */
    G4MH_FOP_FNMS, /* -(x*y - z)                                    */
};

/*
 * x is reg2 (wreg2), y reg1, z reg3: the order the manual gives quiet
 * NaNs priority in, and reg3 <- reg2 OP reg1 for the two-operand forms.
 */
uint32_t g4mh_fp32_op(g4mh_fp_t *e, unsigned op, uint32_t x, uint32_t y,
                      uint32_t z);
uint64_t g4mh_fp64_op(g4mh_fp_t *e, unsigned op, uint64_t x, uint64_t y);

/*
 * MAXF/MINF: no flushing and no E, a quiet NaN gives the other operand,
 * and two zeros give `first` for a maximum and `second` for a minimum
 * (tables 3.129 (c) and 3.135 (c)). `first` is reg2 for MAXF and MINF.
 */
uint32_t g4mh_fp32_maxmin(g4mh_fp_t *e, bool max, uint32_t first,
                          uint32_t second);
uint64_t g4mh_fp64_maxmin(g4mh_fp_t *e, bool max, uint64_t first,
                          uint64_t second);

/* CMPF: fcond as Table 2.10, `x` (reg2) against `y` (reg1); V only. */
bool g4mh_fp32_cmp(g4mh_fp_t *e, uint32_t fcond, uint32_t x, uint32_t y);
bool g4mh_fp64_cmp(g4mh_fp_t *e, uint32_t fcond, uint64_t x, uint64_t y);

/*
 * To an integer. `mode` is the rounding the opcode names, or
 * G4MH_CVT_CUR for the current one (CVTF). `wide` is a 64-bit result.
 */
enum {
    G4MH_CVT_RN, /* ROUNDF */
    G4MH_CVT_RZ, /* TRNCF  */
    G4MH_CVT_RP, /* CEILF  */
    G4MH_CVT_RM, /* FLOORF */
    G4MH_CVT_CUR, /* CVTF  */
};
uint64_t g4mh_fp32_to_int(g4mh_fp_t *e, uint32_t x, unsigned mode, bool uns,
                          bool wide);
uint64_t g4mh_fp64_to_int(g4mh_fp_t *e, uint64_t x, unsigned mode, bool uns,
                          bool wide);

/* From an integer: `wide` reads all 64 bits of v. */
uint32_t g4mh_int_to_fp32(g4mh_fp_t *e, uint64_t v, bool uns, bool wide);
uint64_t g4mh_int_to_fp64(g4mh_fp_t *e, uint64_t v, bool uns, bool wide);

/* Between formats. */
uint64_t g4mh_fp32_to_fp64(g4mh_fp_t *e, uint32_t x);
uint32_t g4mh_fp64_to_fp32(g4mh_fp_t *e, uint64_t x);
uint32_t g4mh_fp16_to_fp32(g4mh_fp_t *e, uint32_t h);
uint32_t g4mh_fp32_to_fp16(g4mh_fp_t *e, uint32_t x);

#endif /* G4MH_FP_H */
