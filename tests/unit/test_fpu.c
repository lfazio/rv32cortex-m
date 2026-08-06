/* SPDX-License-Identifier: Apache-2.0 */
/*
 * test_fpu.c - Unit tests for the F extension.
 *
 * These go through rv_hart_fp, which is the single entry point both the
 * interpreter and the JIT helpers use, so one test covers every backend
 * that routes F through it.
 *
 * The cases are chosen from where implementations actually differ rather
 * than from the happy path, because that is where this project has been
 * bitten before -- CLAUDE.md records a single FCVT.W.S check of 10.0 with
 * rtz that passed whether or not the NaN fixup existed at all. So: NaN
 * inputs, the sign rules FMIN/FMAX give zero and NaN, out-of-range
 * conversions in both directions, and the exception flags each raises.
 *
 * They also run with either FP backend. Built with
 * -DRV32_FPU_SOFTFLOAT=ON these check SoftFloat; without it they check the
 * host FPU path, and a disagreement between the two runs is exactly the
 * differential the hardware self-test performs with -DRV32_JIT=OFF.
 */

#include "tests.h"

#include "rv32/rv_hart.h"
#include "rv32/rv_csr.h"

#include <string.h>

#if RV_EXT_F

/* Encodings, built rather than hard-coded so the intent stays readable. */
#define OP_FP    0x53u
#define OP_FMADD 0x43u

static uint32_t r_type(uint32_t op, uint32_t rd, uint32_t f3, uint32_t rs1,
                       uint32_t rs2, uint32_t f7)
{
    return op | (rd << 7) | (f3 << 12) | (rs1 << 15) | (rs2 << 20) | (f7 << 25);
}

/* Bit patterns, so a NaN survives being written down. */
#define F_QNAN   0x7FC00000u
#define F_SNAN   0x7F800001u
#define F_INF    0x7F800000u
#define F_NINF   0xFF800000u
#define F_ZERO   0x00000000u
#define F_NZERO  0x80000000u
#define F_ONE    0x3F800000u
#define F_TWO    0x40000000u
#define F_THREE  0x40400000u

static rv_hart_t g_hart;
static emu_bus_t g_bus;

static void fp_reset(void)
{
    memset(&g_hart, 0, sizeof(g_hart));
    emu_bus_init(&g_bus);
    rv_hart_init(&g_hart, &g_bus, 0u);
    rv_hart_reset(&g_hart, 0x80000000u);
    /* The FPU must be on, or every one of these is an illegal instruction. */
    g_hart.mstatus |= (1u << MSTATUS_FS_SHIFT);
}

/*
 * Run one OP-FP instruction on f1 and f2, returning the raw bits of f0 and
 * the flags it raised.
 */
static uint32_t fp_op(uint32_t f7, uint32_t rm, uint32_t a, uint32_t b,
                      uint32_t *flags)
{
    uint32_t tval = 0u;

    fp_reset();
    g_hart.f[1] = a;
    g_hart.f[2] = b;
    (void)rv_hart_fp(&g_hart, r_type(OP_FP, 0u, rm, 1u, 2u, f7), &tval);
    if (flags != NULL) {
        *flags = g_hart.fcsr & 0x1Fu;
    }
    return g_hart.f[0];
}

/* Same, but the result is an integer register (comparisons, conversions). */
static uint32_t fp_op_x(uint32_t f7, uint32_t rm, uint32_t rs2,
                        uint32_t a, uint32_t b, uint32_t *flags)
{
    uint32_t tval = 0u;

    fp_reset();
    g_hart.f[1] = a;
    g_hart.f[2] = b;
    (void)rv_hart_fp(&g_hart, r_type(OP_FP, 3u, rm, 1u, rs2, f7), &tval);
    if (flags != NULL) {
        *flags = g_hart.fcsr & 0x1Fu;
    }
    return g_hart.x[3];
}

void test_fpu(void)
{
    uint32_t flags;

    /* ---- arithmetic, to establish the harness is wired up ---- */
    CHECK_EQ(fp_op(0x00u, FRM_RNE, F_ONE, F_TWO, NULL), F_THREE);   /* add */
    CHECK_EQ(fp_op(0x04u, FRM_RNE, F_THREE, F_TWO, NULL), F_ONE);   /* sub */
    CHECK_EQ(fp_op(0x08u, FRM_RNE, F_THREE, F_TWO, NULL), 0x40C00000u); /* mul */

    /* Divide by zero sets DZ and gives a correctly signed infinity, which
     * is not the same as raising invalid. */
    CHECK_EQ(fp_op(0x0Cu, FRM_RNE, F_ONE, F_ZERO, &flags), F_INF);
    CHECK_EQ(flags, FFLAG_DZ);
    CHECK_EQ(fp_op(0x0Cu, FRM_RNE, F_ONE, F_NZERO, NULL), F_NINF);

    /* 0/0 is invalid, and produces the canonical NaN rather than any NaN. */
    CHECK_EQ(fp_op(0x0Cu, FRM_RNE, F_ZERO, F_ZERO, &flags), F_QNAN);
    CHECK_EQ(flags, FFLAG_NV);

    /* ---- FMIN/FMAX: the sign-of-zero and NaN rules ---- */

    /*
     * -0 and +0 compare equal, so an implementation that returns "the
     * smaller" by comparison alone may return either. The spec does not
     * leave it open: FMIN must give -0 and FMAX +0.
     */
    CHECK_EQ(fp_op(0x14u, 0u, F_NZERO, F_ZERO, NULL), F_NZERO);  /* fmin */
    CHECK_EQ(fp_op(0x14u, 0u, F_ZERO, F_NZERO, NULL), F_NZERO);
    CHECK_EQ(fp_op(0x14u, 1u, F_NZERO, F_ZERO, NULL), F_ZERO);   /* fmax */
    CHECK_EQ(fp_op(0x14u, 1u, F_ZERO, F_NZERO, NULL), F_ZERO);

    /* A quiet NaN is ignored if the other operand is a number, and that is
     * *not* an invalid operation. */
    CHECK_EQ(fp_op(0x14u, 0u, F_QNAN, F_TWO, &flags), F_TWO);
    CHECK_EQ(flags, 0u);
    CHECK_EQ(fp_op(0x14u, 1u, F_ONE, F_QNAN, &flags), F_ONE);
    CHECK_EQ(flags, 0u);

    /* Two NaNs give the canonical NaN. A signalling NaN raises invalid
     * even though its value is still discarded. */
    CHECK_EQ(fp_op(0x14u, 0u, F_QNAN, F_QNAN, NULL), F_QNAN);
    CHECK_EQ(fp_op(0x14u, 0u, F_SNAN, F_TWO, &flags), F_TWO);
    CHECK_EQ(flags, FFLAG_NV);

    /* ---- FSGNJ family ---- */
    CHECK_EQ(fp_op(0x10u, 0u, F_ONE, F_NZERO, NULL), 0xBF800000u);  /* fsgnj  */
    CHECK_EQ(fp_op(0x10u, 1u, F_ONE, F_NZERO, NULL), F_ONE);        /* fsgnjn */
    CHECK_EQ(fp_op(0x10u, 2u, F_ONE, F_NZERO, NULL), 0xBF800000u);  /* fsgnjx */

    /* ---- comparisons ---- */
    /* An ordered comparison against NaN is false *and* invalid; FEQ is the
     * quiet one and raises invalid only for a signalling NaN. */
    CHECK_EQ(fp_op_x(0x50u, 2u, 2u, F_ONE, F_ONE, NULL), 1u);       /* feq */
    CHECK_EQ(fp_op_x(0x50u, 2u, 2u, F_QNAN, F_ONE, &flags), 0u);
    CHECK_EQ(flags, 0u);
    CHECK_EQ(fp_op_x(0x50u, 2u, 2u, F_SNAN, F_ONE, &flags), 0u);
    CHECK_EQ(flags, FFLAG_NV);
    CHECK_EQ(fp_op_x(0x50u, 1u, 2u, F_QNAN, F_ONE, &flags), 0u);    /* flt */
    CHECK_EQ(flags, FFLAG_NV);

    /* ---- FCLASS ---- */
    CHECK_EQ(fp_op_x(0x70u, 1u, 0u, F_NINF,  0u, NULL), 1u << 0);
    CHECK_EQ(fp_op_x(0x70u, 1u, 0u, F_NZERO, 0u, NULL), 1u << 3);
    CHECK_EQ(fp_op_x(0x70u, 1u, 0u, F_ZERO,  0u, NULL), 1u << 4);
    CHECK_EQ(fp_op_x(0x70u, 1u, 0u, F_INF,   0u, NULL), 1u << 7);
    CHECK_EQ(fp_op_x(0x70u, 1u, 0u, F_SNAN,  0u, NULL), 1u << 8);
    CHECK_EQ(fp_op_x(0x70u, 1u, 0u, F_QNAN,  0u, NULL), 1u << 9);

    /*
     * ---- float to integer ----
     *
     * The case the whole conversion path turns on. ARM and x86 both give
     * 0 or INT_MIN for a NaN; RISC-V gives the *maximum*, signed or
     * unsigned as appropriate, and raises invalid. A backend that maps the
     * host instruction straight through is wrong here and nowhere else.
     */
    CHECK_EQ(fp_op_x(0x60u, FRM_RTZ, 0u, F_QNAN, 0u, &flags), 0x7FFFFFFFu);
    CHECK_EQ(flags, FFLAG_NV);
    CHECK_EQ(fp_op_x(0x60u, FRM_RTZ, 1u, F_QNAN, 0u, &flags), 0xFFFFFFFFu);
    CHECK_EQ(flags, FFLAG_NV);

    /* Saturation, and invalid with it -- but no inexact. */
    CHECK_EQ(fp_op_x(0x60u, FRM_RTZ, 0u, F_INF, 0u, &flags), 0x7FFFFFFFu);
    CHECK_EQ(flags, FFLAG_NV);
    CHECK_EQ(fp_op_x(0x60u, FRM_RTZ, 0u, F_NINF, 0u, &flags), 0x80000000u);
    CHECK_EQ(flags, FFLAG_NV);
    /* A negative value converted to unsigned saturates to zero. */
    CHECK_EQ(fp_op_x(0x60u, FRM_RTZ, 1u, 0xBF800000u, 0u, &flags), 0u);
    CHECK_EQ(flags, FFLAG_NV);

    /* Ordinary conversions, and the rounding modes that distinguish RMM
     * from RNE -- 2.5 goes to 2 under RNE and 3 under RMM, which is the
     * pair that caught the JIT resolving `dyn` wrongly. */
    CHECK_EQ(fp_op_x(0x60u, FRM_RTZ, 0u, 0x40200000u, 0u, NULL), 2u);
    CHECK_EQ(fp_op_x(0x60u, FRM_RNE, 0u, 0x40200000u, 0u, NULL), 2u);
    CHECK_EQ(fp_op_x(0x60u, FRM_RMM, 0u, 0x40200000u, 0u, NULL), 3u);
    CHECK_EQ(fp_op_x(0x60u, FRM_RUP, 0u, 0x40200000u, 0u, NULL), 3u);
    CHECK_EQ(fp_op_x(0x60u, FRM_RDN, 0u, 0x40200000u, 0u, NULL), 2u);

    /* ---- integer to float ---- */
    {
        uint32_t tval = 0u;

        fp_reset();
        g_hart.x[1] = 0xFFFFFFFFu;
        (void)rv_hart_fp(&g_hart, r_type(OP_FP, 0u, FRM_RNE, 1u, 0u, 0x68u),
                         &tval);
        CHECK_EQ(g_hart.f[0], 0xBF800000u);          /* -1 as signed   */

        fp_reset();
        g_hart.x[1] = 0xFFFFFFFFu;
        (void)rv_hart_fp(&g_hart, r_type(OP_FP, 0u, FRM_RNE, 1u, 1u, 0x68u),
                         &tval);
        CHECK_EQ(g_hart.f[0], 0x4F800000u);          /* 2^32 unsigned  */
    }

    /* ---- FSQRT, including the invalid case ---- */
    CHECK_EQ(fp_op(0x2Cu, FRM_RNE, 0x40800000u, 0u, NULL), F_TWO);  /* sqrt 4 */
    CHECK_EQ(fp_op(0x2Cu, FRM_RNE, 0xBF800000u, 0u, &flags), F_QNAN);
    CHECK_EQ(flags, FFLAG_NV);

    /*
     * ---- fused multiply-add ----
     *
     * The point of a *fused* multiply-add is that it rounds once, so it is
     * only testable with operands where rounding twice differs. 1 + 2^-24
     * squared minus 1 is exactly that: the product needs more bits than a
     * float has, and an unfused a*b+c returns zero.
     */
    {
        uint32_t tval = 0u;
        const uint32_t one_plus_ulp = 0x3F800001u;

        fp_reset();
        g_hart.f[1] = one_plus_ulp;
        g_hart.f[2] = one_plus_ulp;
        g_hart.f[3] = 0xBF800000u;               /* -1 */
        /* FMADD.S f0, f1, f2, f3 */
        (void)rv_hart_fp(&g_hart,
                         OP_FMADD | (0u << 7) | (FRM_RNE << 12) | (1u << 15) |
                         (2u << 20) | (3u << 27),
                         &tval);
        CHECK(g_hart.f[0] != 0u);
    }

    /* ---- mstatus.FS gates the whole extension ---- */
    {
        uint32_t tval = 0u;

        fp_reset();
        g_hart.mstatus &= ~MSTATUS_FS_MASK;       /* FS = Off */
        CHECK_EQ(rv_hart_fp(&g_hart, r_type(OP_FP, 0u, FRM_RNE, 1u, 2u, 0x00u),
                            &tval),
                 RV_EXC_ILLEGAL_INSN);
    }
}

#else  /* !RV_EXT_F */

void test_fpu(void) { }

#endif
