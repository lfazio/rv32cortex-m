/* SPDX-License-Identifier: Apache-2.0 */
/*
 * emu_ir.h - The intermediate representation between a frontend and a host.
 *
 *   frontend -> IR -> common optimisation -> backend
 *
 * A frontend is then unique per emulated machine and a backend unique per
 * host, and neither knows the other exists. That is the point of the
 * layer: with three frontends (RV32, G4MH, e200z7) and three hosts
 * (x86-64, ARMv7-M, ARMv8-M) the direct arrangement is nine translators,
 * and every optimisation has to be written nine times or not at all.
 *
 * What this buys that the direct translators could not have
 * ---------------------------------------------------------
 * The framework in emu_jit.h already shares the block cache, the dispatch
 * loop and staleness tracking, and emu_x86_64.h already shares the host
 * encoding. What was left unshared -- and what this adds -- is everything
 * that needs to see *more than one instruction at a time*:
 *
 *   - dead flag elimination. G4MH and e200z7 set condition flags on
 *     almost every arithmetic instruction, and on x86-64 materialising
 *     them costs a `seto`/`lahf` pair plus masking. Most of those
 *     definitions are dead, because the next flag-setting instruction
 *     overwrites them before any conditional reads them. Seeing one
 *     instruction ahead deletes the whole sequence. RISC-V has no flags,
 *     so this never came up while RV32 was the only frontend.
 *
 *   - guest register traffic. With the register file in memory, a
 *     dependent pair emits a store and then an immediate reload of the
 *     same slot. Measured on this project's own guests, a quarter to a
 *     third of adjacent instruction pairs are data dependent, and the
 *     round trip is 5-7% of executed host instructions and a similar
 *     share of code size -- which counts twice, because code size is what
 *     sets JIT performance when the cache is 12 KB.
 *
 *   - fusion. Not the textbook RISC-V pairs, which this project measured
 *     and found absent (lui+addi is 0.2% of CoreMark pairs), but the ones
 *     a host actually offers: shift-and-add into one ARM operand, or one
 *     x86 LEA.
 *
 * Deliberate limits
 * -----------------
 * Every value is 32 bits. Both current guests are 32-bit and the third
 * is too, and a type lattice that nothing needs is a cost paid on every
 * translation -- so a float is its IEEE-754 bit pattern in an ordinary
 * temp, and the *operation* says how to read it. That is what lets the
 * optimiser, the register allocator and the dead-store rule apply to
 * floating-point values without knowing they are floats.
 *
 * There is no allocation. Blocks are fixed-size arrays sized for the
 * target, because on a microcontroller these bytes are the guest's.
 *
 * The IR is *not* SSA. Values are numbered temporaries written once by
 * construction -- the builder never reuses a temp within a block -- which
 * gives the passes what SSA would without a phi node or a dominance
 * computation, since a block has one entry and no joins.
 */
#ifndef EMU_IR_H
#define EMU_IR_H

#include "emu_jit.h"

#include <stdbool.h>
#include <stdint.h>

#include "emu/emu_dtlb.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Sizes                                                               */
/* ------------------------------------------------------------------ */

/*
 * A block's worth of IR. Sized from the block length the translators
 * already use (64 guest instructions) times the widest lowering, and
 * kept small enough on a target that the buffer is not the reason the
 * guest runs out of RAM.
 */
#if defined(EMU_HOST_JIT_THUMB2)
#define EMU_IR_MAX_INSNS 512u
#define EMU_IR_MAX_TEMPS 256u
#else
#define EMU_IR_MAX_INSNS 2048u
#define EMU_IR_MAX_TEMPS 1024u
#endif

/* ------------------------------------------------------------------ */
/* Guest vector registers: EMU_IR_VOP                                  */
/* ------------------------------------------------------------------ */

/*
 * Whether this build has EMU_IR_VOP and EMU_IR_VMEM's implementations --
 * the reference, the interpreter's cases, each backend's lowering. Set
 * by CMake: on for a host, on a board only with a vector unit. Without
 * them both backends answer emu_ir_can_lower no, and a frontend keeps its
 * helper, which no frontend then has a vector instruction for.
 */
#ifndef EMU_IR_VECTOR
#define EMU_IR_VECTOR 0
#endif

/* Bytes a guest vector register holds: RISC-V's VLEN 128. */
#define EMU_IR_VREG_BYTES 16u

/* EMU_IR_VOP's operations, element by element: d = s2 op s1. */
#define EMU_IR_V_ADD 0u
#define EMU_IR_V_SUB 1u
#define EMU_IR_V_RSUB 2u /* s1 - s2 */
#define EMU_IR_V_AND 3u
#define EMU_IR_V_OR 4u
#define EMU_IR_V_XOR 5u
#define EMU_IR_V_MINU 6u
#define EMU_IR_V_MIN 7u
#define EMU_IR_V_MAXU 8u
#define EMU_IR_V_MAX 9u
#define EMU_IR_V_MUL 10u /* the low half */
#define EMU_IR_V_SLL 11u /* by s1's low log2(bits) bits */
#define EMU_IR_V_SRL 12u
#define EMU_IR_V_SRA 13u
#define EMU_IR_V_MV 14u /* d = s1 */
/*
 * The averaging adds: the sum in one more bit than the element, halved
 * and rounded by imm's mode (EMU_IR_VOP_RM). The narrowing clips below
 * round by it too. Every backend that lowers one of these lowers all
 * four modes, which is why the mode can live in imm, where
 * emu_ir_can_lower does not look.
 */
#define EMU_IR_V_AADDU 15u
#define EMU_IR_V_AADD 16u
/* The multiply-adds, which read d as well, the product's low half. */
#define EMU_IR_V_MACC 17u  /* d + s1 * s2 */
#define EMU_IR_V_NMSAC 18u /* d - s1 * s2 */
#define EMU_IR_V_MADD 19u  /* s1 * d + s2 */
#define EMU_IR_V_NMSUB 20u /* s2 - s1 * d */
/*
 * The widening multiply-adds: s2 and s1 at the element width in a group
 * of 2^lg registers, d at twice it in a group of twice as many -- lg at
 * most 2, d's group aligned to its size and disjoint from both sources --
 * and d[i] += ext(s1[i]) * ext(s2[i]), each extended as named. vl counts
 * elements, of either width: the tail is d's past vl.
 */
#define EMU_IR_V_WMACCU 21u  /* unsigned * unsigned */
#define EMU_IR_V_WMACC 22u   /* signed * signed */
#define EMU_IR_V_WMACCSU 23u /* signed s1 * unsigned s2 */
#define EMU_IR_V_WMACCUS 24u /* unsigned s1 * signed s2 */
/*
 * The narrowing shifts and clips: s2 at twice the element width in a
 * group of 2^(lg+1) registers -- lg at most 2 -- d at the element width in
 * 2^lg, and the shift s1 (or the scalar) taken modulo 2 * SEW. The clips
 * round by imm's mode and saturate to d's width, and any element they
 * saturate sets the target's saturation word (vsat_offset) to 1; the
 * plain shifts truncate. d's group is disjoint from s2's or starts at it.
 */
#define EMU_IR_V_NSRL 25u
#define EMU_IR_V_NSRA 26u
#define EMU_IR_V_NCLIPU 27u
#define EMU_IR_V_NCLIP 28u
/* EMU_IR_VOPM only: d = the mask's bit ? s1 : s2, every element below vl. */
#define EMU_IR_V_MERGE 29u
#define EMU_IR_V_COUNT 30u
#define EMU_IR_V_IS_WIDE(op) ((op) >= EMU_IR_V_WMACCU && (op) <= EMU_IR_V_WMACCUS)
#define EMU_IR_V_IS_NARROW(op) ((op) >= EMU_IR_V_NSRL && (op) <= EMU_IR_V_NCLIP)

#define EMU_IR_VOP_AUX(op, sew, vx)                                            \
    ((uint8_t)((op) | ((uint32_t)(sew) << 5) | ((vx) ? 0x80u : 0u)))
#define EMU_IR_VOP_OP(aux) ((uint32_t)(aux) & 31u)
#define EMU_IR_VOP_SEW(aux) (((uint32_t)(aux) >> 5) & 3u) /* log2 bytes */
#define EMU_IR_VOP_VX(aux) (((uint32_t)(aux) & 0x80u) != 0u)

#define EMU_IR_VOP_IMM(vd, vs2, vs1, lg)                                       \
    ((uint32_t)(vd) | ((uint32_t)(vs2) << 5) | ((uint32_t)(vs1) << 10) |       \
     ((uint32_t)(lg) << 15))
#define EMU_IR_VOP_VD(imm) ((uint32_t)(imm) & 31u)
#define EMU_IR_VOP_VS2(imm) (((uint32_t)(imm) >> 5) & 31u)
#define EMU_IR_VOP_VS1(imm) (((uint32_t)(imm) >> 10) & 31u)
#define EMU_IR_VOP_NREGS(imm) (1u << (((uint32_t)(imm) >> 15) & 3u))
/* EMU_IR_VOPM's mask register, in bits 21:17. */
#define EMU_IR_VOPM_IMM(vd, vs2, vs1, lg, vm)                                  \
    (EMU_IR_VOP_IMM(vd, vs2, vs1, lg) | ((uint32_t)(vm) << 17))
#define EMU_IR_VOP_MREG(imm) (((uint32_t)(imm) >> 17) & 31u)
/*
 * The rounding mode, in bits 23:22, for the averages and the clips:
 * RISC-V's vxrm -- round to nearest up, to nearest even, down, to odd.
 * A shift right by d of v adds r to v >> d: rnu v[d-1]; rne v[d-1] and
 * (v[d-2:0] != 0 or v[d]); rdn 0; rod !v[d] and v[d-1:0] != 0.
 */
#define EMU_IR_RM_RNU 0u
#define EMU_IR_RM_RNE 1u
#define EMU_IR_RM_RDN 2u
#define EMU_IR_RM_ROD 3u
#define EMU_IR_VOP_RM(imm) (((uint32_t)(imm) >> 22) & 3u)
#define EMU_IR_VOP_IMM_RM(rm) ((uint32_t)(rm) << 22)

/*
 * EMU_IR_VRED's operations: vd[0] = vs1[0] op every active element of
 * vs2's group, at the element's width -- or, for the two widening sums,
 * at twice it, each element zero- or sign-extended.
 */
#define EMU_IR_R_SUM 0u
#define EMU_IR_R_AND 1u
#define EMU_IR_R_OR 2u
#define EMU_IR_R_XOR 3u
#define EMU_IR_R_MINU 4u
#define EMU_IR_R_MIN 5u
#define EMU_IR_R_MAXU 6u
#define EMU_IR_R_MAX 7u
#define EMU_IR_R_WSUMU 8u
#define EMU_IR_R_WSUM 9u
#define EMU_IR_R_COUNT 10u
#define EMU_IR_R_IS_WIDE(rop) ((rop) >= EMU_IR_R_WSUMU)

#define EMU_IR_VRED_AUX(rop, sew, masked)                                      \
    ((uint8_t)((rop) | ((masked) ? 0x10u : 0u) | ((uint32_t)(sew) << 5)))
#define EMU_IR_VRED_OP(aux) ((uint32_t)(aux) & 15u)
#define EMU_IR_VRED_MASKED(aux) (((uint32_t)(aux) & 0x10u) != 0u)
#define EMU_IR_VRED_SEW(aux) (((uint32_t)(aux) >> 5) & 3u) /* log2 bytes */

/*
 * EMU_IR_VFOP's operations on single-precision elements, element i of vd
 * from a = vs2[i], b = vs1[i] or the scalar, and c = vd[i] as it was.
 */
#define EMU_IR_VF_ADD 0u    /* a + b */
#define EMU_IR_VF_SUB 1u    /* a - b */
#define EMU_IR_VF_RSUB 2u   /* b - a */
#define EMU_IR_VF_MUL 3u    /* a * b */
#define EMU_IR_VF_DIV 4u    /* a / b */
#define EMU_IR_VF_RDIV 5u   /* b / a */
#define EMU_IR_VF_SQRT 6u   /* sqrt(a) */
#define EMU_IR_VF_MACC 7u   /* b * a + c, one rounding */
#define EMU_IR_VF_NMACC 8u  /* -(b * a) - c */
#define EMU_IR_VF_MSAC 9u   /* b * a - c */
#define EMU_IR_VF_NMSAC 10u /* -(b * a) + c */
#define EMU_IR_VF_MADD 11u  /* b * c + a */
#define EMU_IR_VF_NMADD 12u /* -(b * c) - a */
#define EMU_IR_VF_MSUB 13u  /* b * c - a */
#define EMU_IR_VF_NMSUB 14u /* -(b * c) + a */
#define EMU_IR_VF_COUNT 15u
#define EMU_IR_VF_IS_FMA(op) ((op) >= EMU_IR_VF_MACC)

#define EMU_IR_VFOP_AUX(op, rm) ((uint8_t)((op) | ((uint32_t)(rm) << 5)))
#define EMU_IR_VFOP_OP(aux) ((uint32_t)(aux) & 31u)
#define EMU_IR_VFOP_RM(aux) (((uint32_t)(aux) >> 5) & 7u) /* EMU_IR_FRM_* */
/* imm: EMU_IR_VOPM_IMM's registers, and whether a mask applies (bit 22)
 * and the second operand is the scalar `a` (bit 23). */
#define EMU_IR_VFOP_IMM(vd, vs2, vs1, lg, vm, masked, vf)                      \
    (EMU_IR_VOPM_IMM(vd, vs2, vs1, lg, vm) | ((masked) ? (1u << 22) : 0u) |   \
     ((vf) ? (1u << 23) : 0u))
#define EMU_IR_VFOP_MASKED(imm) ((((uint32_t)(imm)) >> 22) & 1u)
#define EMU_IR_VFOP_VF(imm) ((((uint32_t)(imm)) >> 23) & 1u)

/*
 * EMU_IR_VMEMS's imm: the layout EMU_IR_VMEM's has, less the group (bits
 * 0-9), and in bits 10-31 the frontend's own: the whole imm is the
 * helper's argument, which is how it names the access without a fourth
 * operand.
 */
#define EMU_IR_VMEMS_IMM(vd, eew, store, arg)                                  \
    ((uint32_t)(vd) | ((uint32_t)(eew) << 7) | ((store) ? 0x200u : 0u) |      \
     ((uint32_t)(arg) << 10))
#define EMU_IR_VMEMS_ARG(imm) ((uint32_t)(imm) >> 10)
/*
 * The strides the inline path takes: |stride| below 2^24, so that vl - 1
 * times it fits a signed word on every host. Any other is the helper's.
 */
#define EMU_IR_VMEMS_STRIDE_MAX 0x01000000u

#define EMU_IR_VMEM_IMM(vd, glg, eew, store)                                   \
    ((uint32_t)(vd) | ((uint32_t)(glg) << 5) | ((uint32_t)(eew) << 7) |        \
     ((store) ? 0x200u : 0u))
#define EMU_IR_VMEM_VD(imm) ((uint32_t)(imm) & 31u)
#define EMU_IR_VMEM_BYTES(imm) (16u << (((uint32_t)(imm) >> 5) & 3u))
#define EMU_IR_VMEM_EEW(imm) (((uint32_t)(imm) >> 7) & 3u) /* log2 bytes */
#define EMU_IR_VMEM_STORE(imm) (((uint32_t)(imm) & 0x200u) != 0u)

/*
 * EMU_IR_VOP in C: the group at vd, from the groups at vs2 and vs1 (vs1
 * ignored for a .vx form, whose operand is `scalar`), its first vl
 * elements. The registers are little-endian bytes, as every host here
 * holds them. The reference every lowering is held to, and the IR
 * interpreter's implementation.
 */
void emu_ir_vop_ref(uint8_t *vd, const uint8_t *vs2, const uint8_t *vs1,
                    uint32_t scalar, uint8_t aux, uint32_t nregs, uint32_t vl,
                    uint32_t rm);

/*
 * EMU_IR_VOPM's meaning: as emu_ir_vop_ref, but element i of vd -- at
 * vd's width, twice the element's for a widening operation -- is written
 * only where bit i of `mask` (a register, bit i in byte i / 8) is set,
 * and for EMU_IR_V_MERGE every element below vl, from s1 where the bit
 * is set and s2 where it is not. Returns whether an active element of a
 * clip saturated.
 */
bool emu_ir_vopm_ref(uint8_t *vd, const uint8_t *vs2, const uint8_t *vs1,
                     const uint8_t *mask, uint32_t scalar, uint8_t aux,
                     uint32_t nregs, uint32_t vl, uint32_t rm);

/*
 * EMU_IR_VRED's meaning: vd (a register) element 0, at the result's
 * width, from vs1's element 0 and the first vl elements of the group at
 * vs2 -- those whose bit in `mask` is set, if it is not NULL. Nothing is
 * written when vl is 0, and nothing but element 0 ever: every source is
 * read before it, so vd may be any of them.
 */
void emu_ir_vred_ref(uint8_t *vd, const uint8_t *vs2, const uint8_t *vs1,
                     const uint8_t *mask, uint8_t aux, uint32_t nregs,
                     uint32_t vl);

struct emu_ir_target;

/*
 * EMU_IR_VFOP's meaning: t->vfop on each active element below vl, imm's
 * registers in the cpu at `base`, the flags ORed into *flags.
 */
void emu_ir_vfop_ref(const struct emu_ir_target *t, uint8_t *base, uint8_t aux,
                     uint32_t imm, uint32_t scalar, uint32_t vl, uint32_t *flags);

/*
 * EMU_IR_VFOP run in C on the cpu, the flags handed to t->fp_flags: the
 * IR interpreter's, and what a backend calls for an operation or a
 * rounding mode it has no instructions for -- six arguments, which
 * System V passes in registers.
 */
void emu_ir_vfop_call(void *cpu, const struct emu_ir_target *t, uint32_t aux,
                      uint32_t imm, uint32_t scalar, uint32_t vl);

/*
 * The narrowing operations' reference, which also says whether an
 * element saturated -- the caller sets the target's saturation word.
 * Under a mask (not NULL) only the elements whose bit is set are
 * computed, written, and can saturate.
 */
bool emu_ir_vop_narrow_ref(uint8_t *vd, const uint8_t *vs2, const uint8_t *vs1,
                           const uint8_t *mask, uint32_t scalar, uint8_t aux,
                           uint32_t nregs, uint32_t vl, uint32_t rm);

/* ------------------------------------------------------------------ */
/* Guest condition flags                                               */
/* ------------------------------------------------------------------ */

/*
 * The flags a guest defines and uses, as a bitmask, in *architecture
 * neutral* terms. G4MH's Z/S/OV/CY and PowerPC's CR0 LT/GT/EQ/SO both map
 * onto these; RISC-V defines none of them and its lowering never sets a
 * flag mask at all, which is what makes the dead-flag pass free for it.
 *
 * The mapping is the frontend's business. What the IR guarantees is only
 * that a definition of a flag is dead if every later instruction in the
 * block redefines it before reading it, and that the block boundary
 * counts as a read of everything -- a flag can always be examined by the
 * next block or by an interrupt handler.
 */
#define EMU_IR_F_Z (1u << 0) /* result was zero          */
#define EMU_IR_F_S (1u << 1) /* result was negative      */
#define EMU_IR_F_V (1u << 2) /* signed overflow          */
#define EMU_IR_F_C (1u << 3) /* carry or borrow          */
#define EMU_IR_F_ALL (EMU_IR_F_Z | EMU_IR_F_S | EMU_IR_F_V | EMU_IR_F_C)

/* ------------------------------------------------------------------ */
/* Operations                                                          */
/* ------------------------------------------------------------------ */

typedef enum emu_ir_op {
    EMU_IR_NOP = 0,

    /*
     * Guest register file access, explicit so the optimiser can see it.
     * GET reads guest register `a` into `dst`; PUT writes `a`'s value
     * into guest register `imm`. Making these ordinary instructions
     * rather than an implicit part of every operand is the whole reason
     * the redundant round trip is visible at all.
     */
    EMU_IR_GET,
    EMU_IR_PUT,

    EMU_IR_CONST, /* dst = imm                                */
    EMU_IR_MOV, /* dst = a                                  */

    /* Arithmetic and logic: dst = a op b. */
    EMU_IR_ADD,
    EMU_IR_SUB,
    EMU_IR_AND,
    EMU_IR_OR,
    EMU_IR_XOR,
    EMU_IR_SHL,
    EMU_IR_SHR,
    EMU_IR_SAR,
    EMU_IR_ROTL,
    EMU_IR_MUL,
    EMU_IR_MULHS,
    EMU_IR_MULHU,

    /*
     * Integer divide and remainder, with the *host's* semantics and not
     * a guest's.
     *
     * **The divisor must be non-zero, and the signed forms must not be
     * asked for INT_MIN / -1.** Both are the caller's responsibility,
     * because there is no answer a backend could give that suits every
     * guest: RISC-V defines those cases as producing values, x86 raises
     * #DE, and ARM's SDIV returns zero. A frontend that emits these
     * without guaranteeing the precondition has written a guest-visible
     * fault into translated code that the interpreter would not take.
     *
     * Normalising in the frontend rather than here is what keeps it to
     * one copy: the alternative is every backend reproducing the same
     * two special cases, which is the arrangement that made the FP unit
     * disagree with itself until it was routed through one
     * implementation.
     */
    EMU_IR_DIVS,
    EMU_IR_DIVU,
    EMU_IR_REMS,
    EMU_IR_REMU,

    /*
     * dst = c +/- (a * b), the multiply-accumulate. ARM's MLA and MLS,
     * and the second user of the IR's third operand.
     *
     * `aux` is EMU_IR_MAC_SUB for the subtracting form. There is no
     * rounding question here, unlike EMU_IR_FMA -- integer arithmetic
     * wraps identically whether it is fused or not -- so this is purely
     * an instruction-count saving, and a host without a multiply-
     * accumulate can decline it and lose nothing but size.
     *
     * **The frontend does not emit this; pass_fuse creates it.** RISC-V
     * has no multiply-accumulate instruction, so the pattern only exists
     * as a MUL whose single consumer is an ADD or SUB. That it fires at
     * all was measured before it was written: CoreMark retires 6,156
     * `mul` followed immediately by a dependent `add` in a
     * 400,000-instruction sample, against 9,470 multiplies in total.
     */
    EMU_IR_MAC,

    /* Same, with `imm` as the right operand. */
    EMU_IR_ADDI,
    EMU_IR_ANDI,
    EMU_IR_ORI,
    EMU_IR_XORI,
    EMU_IR_SHLI,
    EMU_IR_SHRI,
    EMU_IR_SARI,
    EMU_IR_ROTLI,

    EMU_IR_NEG, /* dst = -a                                 */
    EMU_IR_NOT, /* dst = ~a                                 */

    /*
     * Bit and byte manipulation. Present as first-class operations
     * rather than as open-coded shift-and-mask sequences because every
     * host has them -- x86 has bswap/bsf/bsr, ARMv7-M has REV/REV16/CLZ
     * and RBIT -- and recovering them from a shift sequence afterwards
     * is pattern matching that would have to be written per host.
     */
    EMU_IR_BSWAP32, /* reverse all four bytes                   */
    EMU_IR_BSWAP16, /* reverse bytes within each halfword       */
    EMU_IR_HSWAP, /* exchange the two halfwords               */
    EMU_IR_CLZ, /* count leading zeros, 32 for a zero input */
    EMU_IR_CTZ, /* count trailing zeros, 32 for zero        */
    EMU_IR_POPCNT,

    /*
     * Two cryptographic primitives (emu_crypto.h), for the hosts that have
     * them as instructions: x86-64's PCLMULQDQ and AES-NI, used only when
     * the CPU reports them. Neither is anywhere near every host -- a
     * Cortex-M has neither -- so a frontend asks emu_ir_can_lower first
     * and calls its helper when told no; both run in time independent of
     * their operands, as RISC-V's Zkt asks of what they serve.
     *
     *   CLMUL  a carry-less multiply of a and b, 64 bits wide: aux 0 the
     *          low 32 bits, 1 the high 32, 2 bits 62:31 (RISC-V's clmul,
     *          clmulh and clmulr)
     *   AESSB  the AES S-box of a's low byte, aux 0, or its inverse,
     *          aux 1; zero above
     *   SM4SB  the SM4 S-box of a's low byte, zero above: on x86-64 AES's
     *          between two affine maps (scripts/sm4-affine.py)
     */
    EMU_IR_CLMUL,
    EMU_IR_AESSB,
    EMU_IR_SM4SB,

    /*
     * Extract or deposit one bit of a *value*. `b` holds the bit number;
     * using a value rather than an immediate keeps the register and
     * immediate forms of a guest's bit instructions on one path.
     */
    EMU_IR_BEXT, /* dst = (a >> b) & 1                       */
    EMU_IR_BSET, /* dst = a | (1 << b)                       */
    EMU_IR_BCLR, /* dst = a & ~(1 << b)                      */
    EMU_IR_BINV, /* dst = a ^ (1 << b)                       */

    /*
     * Bit operations on *memory*: the read-modify-write class.
     *
     * A separate class from the value forms above rather than a LOAD, a
     * BSET and a STORE, for three reasons that all come from what the
     * guests actually define:
     *
     *   - it is one access as far as the guest is concerned. Spelling it
     *     as three IR instructions invites a later pass to reorder or
     *     eliminate one half of it, and on an address that is a
     *     peripheral register a spurious extra read is a side effect.
     *   - the Z flag reports the bit as it was *before* the change, which
     *     is a value that exists nowhere in a load/modify/store
     *     decomposition once the modify has happened.
     *   - hosts implement it directly. x86 has bts/btr/btc with a memory
     *     operand and ARM has the load-exclusive pair, so a backend that
     *     was handed three instructions would have to pattern-match them
     *     back into one.
     *
     * `a` is the address, `b` the bit number, `imm` a byte displacement
     * added to the address, and `aux` the access width. All four define
     * EMU_IR_F_Z and nothing else. TST alone does not write memory --
     * which also means it alone cannot raise a store-side fault.
     *
     * G4MH's SET1/CLR1/NOT1/TST1 are exactly this, in both their
     * immediate-bit and register-bit encodings.
     */
    EMU_IR_BITOP_SET,
    EMU_IR_BITOP_CLR,
    EMU_IR_BITOP_INV,
    EMU_IR_BITOP_TST,

    /* Sign and zero extension from a narrower width. */
    EMU_IR_SEXT8,
    EMU_IR_SEXT16,
    EMU_IR_ZEXT8,
    EMU_IR_ZEXT16,

    /*
     * Memory. `imm` is a byte displacement added to `a`; `b` is the data
     * for a store. Width and signedness travel in `aux`.
     *
     * **`c`, when set, is a predicate**: the access happens only if that
     * temp is non-zero. When it is zero nothing is read or written,
     * nothing can fault, and a LOAD's result is unspecified -- the
     * frontend selects against the old value, which is what it does for
     * every other instruction under a condition. See emu_ir_mem_if.
     */
    EMU_IR_LOAD,
    EMU_IR_STORE,

    /*
     * Flags, split from the operation that produced the value.
     *
     * This split is what makes the dead-flag pass possible: an ADD that
     * also sets flags is an EMU_IR_ADD followed by an EMU_IR_SETF, and
     * deleting the SETF leaves a perfectly good ADD. Fusing them into
     * one flag-setting opcode would mean the pass had to rewrite the
     * opcode instead, once per opcode per host.
     *
     * `aux` says how to derive them (see emu_ir_flagsrc_t), `a` is the
     * result and `b` and `imm` the operands where the derivation needs
     * them.
     */
    EMU_IR_SETF,

    /*
     * dst = condition `aux` evaluated against the current flags. The
     * backend may fold this into a host setcc.
     */
    EMU_IR_GETCOND,

    /*
     * dst = (a `aux` b) ? 1 : 0, comparing two values directly rather
     * than consulting the guest's flags.
     *
     * Separate from GETCOND because a guest without condition flags has
     * no flags to consult: RISC-V's SLT and SLTU are exactly this, and
     * expressing them as "set flags, then read them" would invent a flag
     * word the architecture does not have and defeat the dead-flag pass
     * for the one frontend that should pay nothing for it.
     */
    EMU_IR_SETCC,

    /* dst = c ? a : b, with the condition in `aux`. */
    EMU_IR_SELECT,

    /*
     * Floating point, single precision.
     *
     * The register file is separate from the integer one on both guests
     * that have floats, so it gets its own accessors rather than an
     * offset trick -- and everything between them is an ordinary temp,
     * which is what makes FMV.X.W an FGET and a PUT with nothing in the
     * middle.
     *
     * `aux` carries the rounding mode (EMU_IR_FRM_*) on every operation
     * whose result depends on one, resolved by the *frontend* at
     * translation: a guest's "dynamic" mode is a register read, and
     * baking it in is what lets a backend with no equivalent for a
     * particular mode decline that block instead of quietly rounding
     * differently. This project has made that mistake once already, on
     * ARM, where the old run-time table mapped RISC-V's ties-away to
     * ties-even and no test noticed.
     *
     * **A NaN result is the canonical one, 0x7FC00000**, whatever the
     * operands were -- the rule both guests with floats have. x86-64
     * tests each arithmetic result for it, Thumb-2 runs with FPSCR.DN
     * set, and emu_ir_interp canonicalises too, where host C would not.
     *
     * A block that contains any of these accumulates the host's sticky
     * exception flags and hands them to the frontend once, at the block
     * epilogue -- see emu_ir_target_t.fp_flags. Per-operation would be
     * correct and is what the flags mean, but every path out of a block
     * already passes through the epilogue, so once is enough and the
     * difference is invisible to a guest that can only read them with a
     * CSR instruction no backend translates.
     */
    EMU_IR_FGET, /* dst = freg[imm]                          */
    EMU_IR_FPUT, /* freg[imm] = a                            */

    EMU_IR_FADD,
    EMU_IR_FSUB,
    EMU_IR_FMUL,
    EMU_IR_FDIV,
    EMU_IR_FSQRT, /* dst = sqrt(a)                            */

    /*
     * The fused multiply-adds: dst = +/-(a * b) +/- c, rounded *once*.
     *
     * The only operation in the IR that reads three temps, which is why
     * emu_ir_insn_t has a `c` at all. `aux` carries the two signs as
     * EMU_IR_FMA_NEG_MUL and EMU_IR_FMA_NEG_ADD, covering RISC-V's
     * FMADD/FMSUB/FNMADD/FNMSUB with one opcode.
     *
     * **Rounding once is the whole point and the whole risk.** A backend
     * that lowers this to a multiply and an add rounds twice and is
     * wrong in the last bit; one that declines it gets a helper call
     * whose result is right. Declining is therefore always safe here,
     * which is not true of every operation.
     */
    EMU_IR_FMA,

    /*
     * Minimum and maximum, which are *not* a compare and a select: both
     * guests define the result for a NaN operand as the other operand,
     * where every host's instruction of that name returns its second.
     */
    EMU_IR_FMIN,
    EMU_IR_FMAX,

    /* Sign injection; `aux` is EMU_IR_FSGNJ_*. Pure bit manipulation. */
    EMU_IR_FSGNJ,

    /*
     * dst = (a `aux` b) ? 1 : 0 with `aux` an EMU_IR_C_EQ/LT/LE, and
     * unordered always false. Separate from EMU_IR_SETCC because the
     * unordered case has no integer analogue.
     */
    EMU_IR_FCMP,

    /* Conversions. `aux` is a rounding mode plus EMU_IR_F_UNSIGNED. */
    EMU_IR_FCVT_TO_I,
    EMU_IR_FCVT_FROM_I,

    /* The ten-bit classification both guests spell the same way. */
    EMU_IR_FCLASS,

    /*
     * Anything the IR does not model: a call to a frontend helper with
     * the cpu pointer and up to three arguments. The escape hatch that
     * keeps a block whole -- this project has measured that declining an
     * instruction costs more than translating it badly, because ending
     * the block fragments hot code.
     *
     * The arguments are `a`, `b` and, **only when it is non-zero**,
     * `aux`: helper(cpu, a, b, aux). A helper emitted with aux 0 gets
     * whatever the third argument register held and must not read it;
     * one that takes a constant that can be 0 sets a bit of its own
     * (PowerPC's FP helpers set bit 7). The load is left out at 0 so that
     * a helper which takes two arguments costs not a byte more.
     */
    EMU_IR_HELPER,

    /*
     * A helper that may take a guest trap.
     *
     * Same call as EMU_IR_HELPER, but a non-zero return means the helper
     * entered a trap and the block must stop *there*: pc already points
     * at the handler, and running the rest of the block would execute
     * instructions the trap preempted. `dst` receives the return value.
     *
     * This is why loads and stores cannot simply be EMU_IR_HELPER. The
     * distinction is invisible until a guest actually faults, which is
     * the sort of thing that survives a whole test suite.
     */
    EMU_IR_HELPER_TRAP,

    /*
     * Control flow. All of these end a block.
     *
     * EXIT writes the guest pc from `a` (or `imm` when `a` is
     * EMU_IR_NO_TEMP) and returns. EXIT_IF does the same under a
     * condition, falling through otherwise.
     */
    /*
     * Write the guest pc without ending the block, from `a` or from
     * `imm` when `a` is EMU_IR_NO_TEMP.
     *
     * Distinct from EMU_IR_EXIT because a block that folds several guest
     * instructions still has to leave the pc right after each one: a
     * trap, an interrupt or the interpreter picking up where the block
     * stopped all read it, and a pc that only became correct at the end
     * of the block would send every one of them to the wrong address.
     */
    /*
     * One guest instruction retired -- or `imm` of them, once
     * pass_merge_retire has folded a run into one; 0 means 1, which is
     * what every frontend emits.
     *
     * Emitted by the frontend after each instruction it lowers, and
     * lowered to whatever the host uses to bump the block's retired
     * count. It has to be per instruction rather than a total added at
     * the end of the block, because a block can leave early -- a taken
     * branch, a trapping helper -- and a count placed after the exit
     * label charges the guest for instructions it never ran. That reads
     * as the guest retiring nearly twice what it executed, while
     * computing entirely correct answers.
     */
    EMU_IR_RETIRE,

    EMU_IR_SETPC,

    EMU_IR_EXIT,
    EMU_IR_EXIT_IF,

    /*
     * An operation on whole guest vector registers, element by element.
     *
     * **A vector register is a place, not a value**: every IR value is
     * 32 bits, and these live in the guest's state at
     * emu_ir_target_t::vreg_offset, EMU_IR_VREG_BYTES each, where the
     * backend reads and writes them -- with the host's SIMD where it has
     * the operation (emu_ir_can_lower(EMU_IR_VOP, aux)), and a frontend
     * whose host has not keeps its helper.
     *
     *   aux  EMU_IR_VOP_AUX(op, sew, vx): the operation (EMU_IR_V_*),
     *        log2 of the element's bytes (0-2), and whether the second
     *        operand is the scalar `a` rather than the register vs1
     *   imm  EMU_IR_VOP_IMM(vd, vs2, vs1, lg): the registers, and log2 of
     *        how many a group holds (0-3); vd, vs2 and vs1 are each the
     *        first of a group, and two groups are the same or disjoint
     *   a    the scalar operand, taken at the element's width (and for a
     *        shift, its low log2(bits) bits), or EMU_IR_NO_TEMP
     *   b    vl: element i is written only for i < vl, every other
     *        element of vd left as it was. vl is at most the group's
     *        element count. The widening operations (EMU_IR_V_IS_WIDE)
     *        write a vd of twice the element width in twice the
     *        registers.
     *
     * Reads and writes nothing else -- no guest register, no flag, no
     * memory -- so no pass need treat it as an observer: it is ordered
     * against the helpers that read vector state by being before them.
     * emu_ir_vop_ref is its meaning, in C.
     */
    EMU_IR_VOP,

    /*
     * EMU_IR_VOP under a mask: the same aux and operands, and in imm the
     * mask register (EMU_IR_VOPM_IMM), outside vd's group. An element is
     * written only where its mask bit is set, the others left as they
     * were; EMU_IR_V_MERGE writes every element below vl, choosing its
     * source by the bit. Single-width operations only. A separate
     * operation, so that a backend answers for the masked forms apart
     * (emu_ir_can_lower(EMU_IR_VOPM, aux)). emu_ir_vopm_ref is its
     * meaning.
     */
    EMU_IR_VOPM,

    /*
     * A unit-stride vector load or store that fills or empties a whole
     * register group, inline where it can be, the frontend's helper
     * where it cannot.
     *
     *   imm  EMU_IR_VMEM_IMM(vd, glg, eew, store): the group's first
     *        register, log2 of its bytes less 4 (16 to 128), log2 of
     *        the element's bytes, and the direction
     *   a    the guest address
     *   b    vl
     *   c    the helper's first argument (the instruction, for RV32)
     *   aux  the helper, helpers[aux]
     *
     * **When vl's bytes are the whole group and every byte of it lies in
     * the block's RAM window** (b->has_fast; for a store, the store
     * guard clear), the group is copied between that RAM and the
     * registers. Otherwise -- a shorter vl, an address outside, no
     * window -- r = helpers[aux](cpu, c, a, vl), and a non-zero r is a
     * trap, as EMU_IR_HELPER_TRAP's: the block stops there. The two are
     * the same access, so the helper is the meaning; the IR interpreter
     * always calls it. A guest register, flag or memory may be touched
     * by the helper, so every pass treats this one as it treats a helper.
     */
    EMU_IR_VMEM,

    /*
     * A reduction of a register group to one element.
     *
     *   aux  EMU_IR_VRED_AUX(rop, sew, masked): the operation
     *        (EMU_IR_R_*), log2 of the element's bytes, and whether
     *        the mask register in imm selects the elements
     *   imm  EMU_IR_VOPM_IMM(vd, vs2, vs1, lg, vm): vd and vs1 are single
     *        registers, vs2 the first of a group of 1 << lg; the mask
     *        register is read only when aux says masked
     *   b    vl, at most the group's element count
     *
     * vd's element 0 = vs1's element 0 op each active element of vs2,
     * below vl; the rest of vd is left as it was, and when vl is 0 all of
     * it. As EMU_IR_VOP, it reads and writes only the vector registers.
     * emu_ir_vred_ref is its meaning.
     */
    EMU_IR_VRED,

    /*
     * A strided vector load or store: element i of EEW at a + i * c, for
     * i below vl, into or out of the group at vd -- the rest of the
     * group left as it was.
     *
     *   imm  EMU_IR_VMEMS_IMM(vd, eew, store, arg)
     *   a    the guest address
     *   b    vl
     *   c    the stride, a signed byte count
     *   aux  the helper, helpers[aux]
     *
     * **Inline when every element lies in the block's RAM window** (and
     * for a store, the store guard is clear) and the stride is below
     * EMU_IR_VMEMS_STRIDE_MAX either way; otherwise r = helpers[aux](cpu,
     * imm, a, vl), a non-zero r a trap, as EMU_IR_VMEM's. The helper is
     * the meaning, and the IR interpreter always calls it; every pass
     * treats this as it treats a helper.
     */
    EMU_IR_VMEMS,

    /*
     * Single-precision arithmetic on a group of guest vector registers,
     * 32-bit elements (EMU_IR_VF_*): element i of vd, for i below vl --
     * and under a mask only where its bit is set -- from vs2, vs1 or the
     * scalar, and vd as it was; the rest of vd left alone.
     *
     *   aux  EMU_IR_VFOP_AUX(op, rm): the operation and its rounding mode
     *   imm  EMU_IR_VFOP_IMM(vd, vs2, vs1, lg, vm, masked, vf), the mask
     *        register vm outside vd's group when masked, as EMU_IR_VOPM's
     *   a    the scalar operand for a .vf form, or EMU_IR_NO_TEMP
     *   b    vl, at most the group's element count
     *
     * An element's value and exception flags are the target's `vfop`,
     * which is the meaning: a NaN result is the canonical one, and the
     * flags accrue as EMU_IR_FADD's do, into the block's -- so a block
     * holding one is an FP block. A masked-off or tail element raises
     * nothing. emu_ir_vfop_ref applies `vfop` to every element, and
     * emu_ir_vfop_call is that with the flags handed over: what a
     * backend calls for what it does not lower natively (x86-64: the
     * fused forms and every mode but RNE).
     */
    EMU_IR_VFOP,
    /*
     * A three-way compare: dst = 8 if a < b, 4 if a > b, 2 if equal --
     * signed for aux EMU_IR_C_LT, unsigned for EMU_IR_C_LTU. PowerPC's
     * CR field less SO, in one operation: two compares, an add and a
     * shift in IR (`2 << (lt + ne)`), one compare and two predicated
     * moves on a host. Pure. A backend says so (emu_ir_can_lower).
     */
    EMU_IR_CMP3,
    /*
     * A loop kernel (emu_ir_kernel.c; docs/jit/autovectorisation.md):
     * r = emu_ir_kernel_run(cpu, imm, count), which runs as many of the
     * block's loop iterations as it can prove safe and the scalar loop
     * would have run before the loop cap ended this entry -- all but the
     * last -- on the guest registers and RAM directly, and returns the
     * guest instructions they retired, which are added to the count.
     * aux is the kernel's kind (EMU_IR_LOOP_*). Only first in a block,
     * ahead of the body a back edge returns to, so it runs once an
     * entry. It reads and writes guest registers: every pass treats it
     * as a helper.
     */
    EMU_IR_KERNEL,

    EMU_IR_OP_COUNT
} emu_ir_op_t;

/*
 * EMU_IR_EXIT's aux: a computed exit that is a plain jump -- a return,
 * a call or a jump through a register or a table -- which a backend may
 * send on to the next block itself (emu_jit_ibtc) rather than back to
 * the dispatcher.
 *
 * **Opt in, never out.** A computed exit without it returns to the
 * dispatcher, which is what every translator meant by one before this
 * existed: G4MH's EI and ARMv7-M's ISB leave through a computed exit to
 * the next instruction precisely so that an interrupt they let in is
 * taken before it, and a translator that reads the pc back after the
 * interpreter ran an instruction cannot know what that instruction did.
 * Marked as jumps and sent on, EI's interrupt was taken a block late
 * (test_g4mh's "EI four times").
 */
#define EMU_IR_EXIT_JUMP 1u

/* How EMU_IR_SETF derives the flags it defines. */
typedef enum emu_ir_flagsrc {
    EMU_IR_FS_LOGIC = 0, /* Z and S from the result; V and C cleared */
    EMU_IR_FS_ADD, /* full add semantics, needs both operands  */
    EMU_IR_FS_SUB, /* full subtract; C is a borrow             */
    EMU_IR_FS_ZS, /* Z and S only, leaving V and C alone      */
    /*
     * Shift semantics: Z and S from the result in `a`, V cleared, and
     * **C taken from bit 0 of `b`**.
     *
     * The carry a shift defines is the last bit shifted out, which is
     * not recoverable from the result -- so the frontend computes it as
     * an ordinary value and hands it over, rather than this enum growing
     * a direction and an amount. That keeps the shift itself an ordinary
     * SHLI/SHRI/SARI that every backend already lowers, and it means a
     * shift by zero is expressed by passing a zero rather than by a
     * special case here.
     *
     * Only bit 0 of `b` is read. Handing it the whole shifted-out word
     * and expecting the flag to mean "any bit" would be a different
     * operation, and a plausible one, which is why the bit is stated.
     */
    EMU_IR_FS_SHIFT
} emu_ir_flagsrc_t;

/* Conditions, for GETCOND, SELECT and EXIT_IF. */
typedef enum emu_ir_cond {
    EMU_IR_C_EQ = 0,
    EMU_IR_C_NE,
    EMU_IR_C_LT,
    EMU_IR_C_GE, /* signed   */
    EMU_IR_C_LTU,
    EMU_IR_C_GEU, /* unsigned */
    EMU_IR_C_LE,
    EMU_IR_C_GT,
    EMU_IR_C_LEU,
    EMU_IR_C_GTU,
    EMU_IR_C_ALWAYS,
    /*
     * The sign and the overflow alone -- G4MH's BN/BP and BV/BNV, which
     * declined without them. As flags: S set, S clear, V set, V clear.
     * Comparing two values: the sign, or the signed overflow, of a - b,
     * which is what a host's compare leaves. After ALWAYS so that the
     * ten older values keep theirs; each pair is still positive then
     * negated.
     */
    EMU_IR_C_MI,
    EMU_IR_C_PL,
    EMU_IR_C_VS,
    EMU_IR_C_VC,
    EMU_IR_C_COUNT
} emu_ir_cond_t;

/*
 * Rounding modes, in the `aux` of every FP operation that has one. The
 * numbering is RISC-V's because it is the guest that has to name them;
 * a host without an equivalent for one declines the operation.
 */
#define EMU_IR_FRM_RNE 0u /* to nearest, ties to even             */
#define EMU_IR_FRM_RTZ 1u /* toward zero                          */
#define EMU_IR_FRM_RDN 2u /* toward -inf                          */
#define EMU_IR_FRM_RUP 3u /* toward +inf                          */
#define EMU_IR_FRM_RMM                                                         \
    4u /* to nearest, ties away -- x86 and ARM
                                * have no mode for this               */
#define EMU_IR_FRM(aux) ((aux) & 7u)

/* Conversion is to or from an unsigned integer. */
#define EMU_IR_F_UNSIGNED (1u << 3)

/* EMU_IR_FSGNJ variants, in `aux`. */
#define EMU_IR_FSGNJ_J 0u /* take b's sign            */
#define EMU_IR_FSGNJ_N 1u /* take b's sign, negated   */
#define EMU_IR_FSGNJ_X 2u /* xor the two signs        */

/*
 * EMU_IR_FGET/FPUT: the 32 bits named are a single-precision value in a
 * register file wider than 32 bits, and carry a box.
 *
 * The rule is RISC-V's NaN boxing, and it exists because adding D widens
 * the register file without widening what a single-precision instruction
 * writes. A single stored into a 64-bit register must carry all-ones in
 * its upper half; a single *read* from one whose upper half is anything
 * else is not a single at all and reads as the canonical NaN.
 *
 * It is in `aux` rather than implied by the register width because the
 * frontend is the only thing that knows which of its FP instructions are
 * single-precision -- and because the same file, built without D, has a
 * 32-bit register file where the box does not exist and the plain form
 * is what is wanted. A backend that ignores this flag moves the low half
 * and is wrong in the direction that does not show up here: the failure
 * lands on the *next* reader of the register, not on this instruction.
 */
#define EMU_IR_FP_BOX (1u << 4)

/*
 * The host's sticky exception flags, as emu_ir_target_t.fp_flags
 * receives them. Architecture-neutral, because x86 and ARM order their
 * own differently from each other and from both guests -- and this
 * project has already spent a debugging session on exactly that: ARM is
 * IOC,DZC,OFC,UFC,IXC from bit 0 and RISC-V is NX,UF,OF,DZ,NV, so a
 * backend that passed its own encoding through would be right on one
 * host and wrong on the other.
 */
#define EMU_IR_FE_INEXACT (1u << 0)
#define EMU_IR_FE_UNDERFLOW (1u << 1)
#define EMU_IR_FE_OVERFLOW (1u << 2)
#define EMU_IR_FE_DIVBYZERO (1u << 3)
#define EMU_IR_FE_INVALID (1u << 4)

/*
 * Access widths for LOAD and STORE, in `aux`. Bits 4-6 are the
 * frontend's own, passed through to its accessor (ARMv7-M marks an
 * access that must be aligned); bit 7 is the backend's, and Thumb-2
 * uses it to tell its shared slow path a store from a load.
 */
#define EMU_IR_MEM_SIZE(aux) ((aux) & 7u)
#define EMU_IR_MEM_SIGNED (1u << 3)
#define EMU_IR_MEM_AUX(sz, sg) (uint8_t)((sz) | ((sg) ? EMU_IR_MEM_SIGNED : 0u))

/* No operand in this slot. */
#define EMU_IR_NO_TEMP 0xFFFFu

/*
 * EMU_IR_FMA's `aux`: which of the two terms is negated.
 *
 * RISC-V's four opcodes map on directly -- FMADD is neither, FMSUB is
 * NEG_ADD, FNMSUB is NEG_MUL, FNMADD is both. Note that RISC-V's naming
 * negates the *product* in the "n" forms, which is the opposite of what
 * the mnemonic suggests to a reader coming from ARM.
 */
/*
 * **Bits 4 and 5, not 0 and 1.** EMU_IR_FRM(aux) is `aux & 7` and
 * EMU_IR_F_UNSIGNED is bit 3, so the low nibble is spoken for. The first
 * version used 0x01/0x02 and every FMA therefore presented its sign bits
 * to can_lower *as a rounding mode* -- FMADD read as RNE and lowered,
 * FNMSUB read as RTZ and was declined, and the ones that did lower were
 * checked against the wrong mode. fptest's mixed kernel came back
 * 0x00000000 against a reference of 0x49370308.
 */
/* EMU_IR_MAC's `aux`: subtract the product rather than add it. */
#define EMU_IR_MAC_SUB 0x01u

#define EMU_IR_FMA_NEG_MUL (1u << 4)
#define EMU_IR_FMA_NEG_ADD (1u << 5)

typedef struct emu_ir_insn {
    uint8_t op; /* emu_ir_op_t                                 */
    uint8_t aux; /* op-specific: width, flag source, condition  */
    uint16_t dst; /* temp written, or EMU_IR_NO_TEMP             */
    uint16_t a, b; /* temps read, or EMU_IR_NO_TEMP               */

    /*
     * A third operand, for the classes of instruction that need one: the
     * fused multiply-adds and multiply-accumulates, and the predicate of
     * a conditional LOAD or STORE. EMU_IR_NO_TEMP everywhere else.
     *
     * **It is not free, and the cost is the reason it went unmodelled
     * for so long.** Every pass that walks operands has to walk this one
     * too -- the use counter, the dead-code sweep, the copy forwarding,
     * the register allocator -- and a pass that misses it does not fail
     * loudly: it computes a wrong use count, and the allocator then
     * reuses a register still being read. That is a wrong answer, not a
     * declined block.
     *
     * The alternative was what this did before: send all four FMA
     * opcodes to the helper. That keeps the block whole and gets the
     * single rounding right, but it is a call per FMA on a workload that
     * has 6.71% of its adjacent pairs led by one -- measured on fptest
     * with EMU_PAIR_STATS.
     */
    uint16_t c;
    uint32_t imm;

    /*
     * Which guest flags this instruction defines, and -- filled in by
     * the optimiser, not by the frontend -- which of those any later
     * instruction actually reads. A SETF whose `live` is zero is dead.
     */
    uint8_t defs;
    uint8_t live;

    /*
     * How many surviving instructions read this one's result, saturating
     * at 255. Filled in by the optimiser, after everything that deletes
     * code has run, so it counts real readers rather than ones about to
     * disappear.
     *
     * A backend needs this to fuse safely. Folding a shift into the
     * operand of the instruction that consumes it only pays if the shift
     * itself is then not emitted, and dropping it is sound exactly when
     * nothing else reads it -- `uses == 1`. Without the count a fusion
     * computes the value twice, which is larger and slower than not
     * fusing at all.
     */
    uint8_t uses;

    /* Set by the optimiser; the backend skips these. */
    bool dead;
} emu_ir_insn_t;

/*
 * Does this exit go somewhere known at translation time?
 *
 * **The question a backend has to ask before it can chain.** An exit to
 * a constant can have its jump patched to land in the successor's code;
 * one to a computed address cannot, and has to go back to the
 * dispatcher so the target can be looked up.
 *
 * The information was always in the IR -- `a` unset means the target is
 * the immediate -- but both backends rediscovered it by testing for
 * EMU_IR_NO_TEMP, which reads as a register check rather than as the
 * property it stands for. Naming it puts the rule in one place and lets
 * a backend say what it means.
 *
 * EMU_IR_EXIT_IF is always constant: its target is the branch
 * displacement, and the *condition* is what varies.
 */
static inline bool emu_ir_exit_is_const(const emu_ir_insn_t *in)
{
    if (in->op == (uint8_t)EMU_IR_EXIT_IF) {
        return true;
    }
    return in->op == (uint8_t)EMU_IR_EXIT && in->a == EMU_IR_NO_TEMP;
}

/*
 * A window of guest memory a backend may read and write directly.
 *
 * **Every guest load and store is otherwise a C call**, and on the
 * guests measured roughly 40% of instructions are one: 3,728,397 calls
 * in a 9.4M-instruction Dhrystone. That is the largest single cost in
 * translated code -- far larger than the dispatcher -- because the
 * helper walks the region table, checks permissions and translates,
 * none of which a plain access to guest RAM needs.
 *
 * The frontend decides when there is such a window, and says no
 * whenever anything could make an access mean more than a move: PMP,
 * paging, execute triggers, a big-endian guest. A block that inlines
 * one is therefore specialised on that answer, and is invalidated by
 * the same generation and context keys that already cover PMP, paging
 * and privilege.
 *
 * `store_guard_offset` names a byte in emu_cpu_t that must be zero for
 * an inlined *store* to be legal -- for RISC-V that is the LR/SC
 * reservation flag, because a store to the reserved word has to break
 * the reservation and an inlined one would not. Falling back while one
 * is outstanding costs nothing: they last from an LR to its SC.
 * EMU_IR_NO_GUARD means stores need no such test.
 */
#define EMU_IR_NO_GUARD 0xFFFFFFFFu

typedef struct emu_ir_fastmem {
    uint32_t base; /* guest address of the first byte   */
    uint32_t size; /* bytes in the window               */
    void *host; /* host address of `base`            */
    uint32_t store_guard_offset;
} emu_ir_fastmem_t;

/*
 * A data TLB a backend may probe inline -- see emu_dtlb.h.
 *
 * The other way to reach guest RAM without a call, for when there is no
 * window because paging or PMP is armed. One table, chosen by the
 * frontend at translation from state the block's context already
 * carries (for RISC-V, the privilege a load or store is made at), so
 * the probe is a constant offset from the cpu pointer. A miss, a
 * misaligned access, or a store while the guard byte is set, goes to the
 * helper -- which is also what fills the table.
 *
 * The frontend owns every invalidation. A backend has nothing to keep
 * consistent: it reads the table each time, so an entry cleared between
 * two block entries is a miss at the next access.
 */
typedef struct emu_ir_dtlb {
    uint32_t table_offset; /* of entry 0, from the emu_cpu_t pointer */
    uint32_t index_mask;   /* entries - 1; entries a power of two    */
    uint32_t store_guard_offset;
} emu_ir_dtlb_t;

/*
 * The peripheral window, which a backend may also access inline.
 *
 * Identity-mapped -- the guest's address *is* the host's -- so it is the
 * guest-RAM fast path with `host` equal to `base`, and that is why it is
 * cheap. What differs is the holes: spans a board made read-only because
 * a guest could take the emulator down through them (the PLL, the power
 * controller, the flash interface). A store into one has to fault, so an
 * inlined store tests each hole and leaves for the helper on a match;
 * a load does not, because every span in the window is readable.
 *
 * Only ever filled while armed -- see EMU_JIT_PT_ARM_AT -- and only
 * where the RAM window is, since the same things that refuse that one
 * (PMP, paging, triggers) refuse this.
 */
#ifndef EMU_IR_PT_HOLES
#define EMU_IR_PT_HOLES 4u
#endif

typedef struct emu_ir_ptwin {
    uint32_t base; /* guest == host address of the first byte */
    uint32_t size;
    uint32_t nholes;
    struct {
        uint32_t off; /* from base; word aligned */
        uint32_t len; /* bytes; a multiple of 4  */
    } hole[EMU_IR_PT_HOLES];
} emu_ir_ptwin_t;

/*
 * How many passthrough accesses a guest makes through the bus before its
 * blocks inline the peripheral window. Always-on cost the hand-written
 * translator a CoreMark image past its cache, for a window CoreMark never
 * touches; armed at 64, CoreMark paid 0.66% and drivers gained 2.2-3.1x.
 */
#ifndef EMU_JIT_PT_ARM_AT
#define EMU_JIT_PT_ARM_AT 64u
#endif

struct emu_bus;

/*
 * The peripheral window `bus` offers: the longest run of adjacent,
 * identity-mapped passthrough regions, every one readable at every
 * width, with the read-only ones as holes. False if there is none, or
 * if it has more holes than EMU_IR_PT_HOLES. Cached per core against the
 * bus's epoch, so it is a compare per translation after the first.
 */
bool emu_ir_pt_window(const struct emu_bus *bus, emu_ir_ptwin_t *out);

/*
 * The guest's image, which a backend may *load* from without the helper:
 * `.text` and `.rodata` of a guest linked execute-in-place, in a ROM
 * region. A block tries it after the RAM window misses, so the order
 * costs a RAM access nothing, and never for a store -- a store to the
 * image must fault, and the helper is what faults it. x86-64 tests it in
 * line, 40 bytes on every load; Thumb-2 in its shared memory stub, no
 * bytes per load, because 26 in line pushed ARMv7-M CoreMark out of its
 * 96 KB cache (34.65 host cycles an instruction to 49.19).
 *
 * Armed by use, as the peripheral window is: a guest whose blocks have
 * left EMU_JIT_ROM_ARM_AT reads of the image to the load helper is one
 * that reads constants in its loops. The frontend counts them in its
 * helper (emu_bus_in_rodata), only where the backend lowers the window
 * (EMU_JIT_HAS_ROM_WINDOW). Dhrystone reads its image 31 times a run,
 * RV32 CoreMark 1,143 times in a whole run of any length -- start-up and
 * printing -- and `bench` 41, so the threshold arms the first within a
 * fraction of a percent of its run and the others never. On x86-64 that
 * keeps the guard's bytes out of guests that would not use it.
 *
 * On Thumb-2 the window costs no bytes, and arming was kept for what its
 * one flush did: RV32 Dhrystone on the F746 read 19.19 host cycles an
 * instruction with the window from the start, 16.41 armed by use. The
 * flush put the steady-state loop at the start of the buffer, which was
 * the 9 KB of it then in DTCM; with the buffer first in DTCM, 14.33 from
 * the start and 14.41 armed (docs/performance.md#the-jit-buffer-in-dtcm),
 * and arming stays as on x86-64.
 *
 * Only where the RAM window is, for the same reason as the peripheral
 * window: the things that refuse that one (PMP, paging, triggers) refuse
 * this.
 */
#ifndef EMU_JIT_ROM_ARM_AT
#define EMU_JIT_ROM_ARM_AT 4096u
#endif

/*
 * The ROM region `bus` holds at `base`, as a window: readable at every
 * width, with a host address. False if there is none. Cached per core
 * against the bus's epoch, as the peripheral window is.
 */
bool emu_ir_rom_window(const struct emu_bus *bus, uint32_t base,
                       emu_ir_fastmem_t *out);

typedef struct emu_ir_block {
    emu_ir_insn_t insn[EMU_IR_MAX_INSNS];
    uint32_t count;
    uint32_t next_temp;

    /* Guest instructions folded in so far, for the retired count. */
    uint32_t guest_insns;

    /*
     * The guest address this block starts at.
     *
     * A backend needs it to recognise its own back edge: an EXIT whose
     * constant target is this address is a loop closing on the block
     * itself, and can be a jump rather than a return to the dispatcher.
     * Nothing else in the IR depends on where the block came from.
     */
    uint32_t start_pc;

    /*
     * The window a backend may access without calling the target's
     * load/store, and whether there is one. Filled by the frontend at
     * translation, because only it can say whether an access means more
     * than a move -- see emu_ir_fastmem_t. A frontend that leaves
     * has_fast false gets the helpers, which are always correct.
     */
    emu_ir_fastmem_t fast;
    bool has_fast;

    /* Otherwise, perhaps, a TLB to probe. Never both. */
    emu_ir_dtlb_t dtlb;
    bool has_dtlb;

    /* The peripheral window, likewise, and only while armed. */
    emu_ir_ptwin_t pt;
    bool has_pt;

    /* The image, for loads, likewise: see emu_ir_rom_window. */
    emu_ir_fastmem_t rom;
    bool has_rom;

    /* Set when the block ran out of room; it is then discarded. */
    bool overflow;
} emu_ir_block_t;

/* ------------------------------------------------------------------ */
/* Building                                                            */
/* ------------------------------------------------------------------ */

void emu_ir_reset(emu_ir_block_t *b);
uint16_t emu_ir_temp(emu_ir_block_t *b);

/*
 * Append an instruction and return the temp it writes (or
 * EMU_IR_NO_TEMP). `dst` of EMU_IR_NO_TEMP asks for a fresh temp for the
 * ops that produce a value.
 */
uint16_t emu_ir_emit(emu_ir_block_t *b, emu_ir_op_t op, uint8_t aux, uint16_t a,
                     uint16_t bb, uint32_t imm, uint8_t defs);

/*
 * A three-operand emit, for EMU_IR_FMA. Separate from emu_ir_emit rather
 * than a seventh parameter on it: every other caller would have to pass
 * EMU_IR_NO_TEMP, and the one place a third operand is legal is worth
 * naming.
 */
uint16_t emu_ir_emit3(emu_ir_block_t *b, emu_ir_op_t op, uint8_t aux,
                      uint16_t a, uint16_t bb, uint16_t c);

/* EMU_IR_VMEM, which has three operands and no result (emu_ir.h above). */
void emu_ir_vmem(emu_ir_block_t *b, uint8_t helper, uint16_t addr, uint16_t vl,
                 uint16_t arg, uint32_t imm);

/* EMU_IR_VMEMS: four values and no result; the helper's argument is imm. */
void emu_ir_vmems(emu_ir_block_t *b, uint8_t helper, uint16_t addr,
                  uint16_t vl, uint16_t stride, uint32_t imm);

/*
 * A LOAD or STORE that happens only when `pred` is non-zero; `pred` of
 * EMU_IR_NO_TEMP is an ordinary access. Returns what emu_ir_emit would.
 *
 * For a guest whose conditional instructions include memory accesses --
 * ARMv7-M inside an IT block. Everything else under a condition is
 * computed and then selected against the old value, which an access
 * cannot be: it may fault, and it may be to a device that a read
 * changes. So the backend branches round it instead, inside the one IR
 * instruction, where no pass has to know there is a branch at all.
 */
uint16_t emu_ir_mem_if(emu_ir_block_t *b, emu_ir_op_t op, uint8_t aux,
                       uint16_t addr, uint16_t val, uint32_t disp,
                       uint16_t pred);

/* Shorthands for the shapes that appear most. */
uint16_t emu_ir_get(emu_ir_block_t *b, uint32_t guest_reg);
void emu_ir_put(emu_ir_block_t *b, uint32_t guest_reg, uint16_t val);
uint16_t emu_ir_const(emu_ir_block_t *b, uint32_t v);
uint16_t emu_ir_alu(emu_ir_block_t *b, emu_ir_op_t op, uint16_t a, uint16_t bb);

/*
 * A memory bit operation. `op` must be one of the EMU_IR_BITOP_* class.
 * Defines EMU_IR_F_Z from the bit's value before the operation.
 */
void emu_ir_bitop(emu_ir_block_t *b, emu_ir_op_t op, uint16_t addr,
                  uint16_t bit, uint32_t disp, uint8_t width);

/* ------------------------------------------------------------------ */
/* The loop census (-DEMU_JIT_LOOP_CENSUS=ON, x86-64 hosts)            */
/* ------------------------------------------------------------------ */

/*
 * What a block that branches back to its own start does and how often:
 * the measurement docs/jit/autovectorisation.md's P0 asks for before any
 * kernel is built. A record per translation of such a block; the x86-64
 * lowering counts its entries (from the dispatcher, a chain, or after
 * the loop cap sent it out), the back edges taken, and the cap's exits,
 * so a loop's own trip count is (entries + iters) / (entries - caps).
 * Printed at exit, one `census` line a record, for
 * scripts/loop-census.py.
 */
/*
 * What a block that loops on itself is, for the kernels of
 * docs/jit/autovectorisation.md: one of these, a kernel or a refusal.
 */
typedef enum emu_ir_loop_kind {
    EMU_IR_LOOP_DOT16 = 0,  /* sum += a[i] * b[i * s], 16-bit signed */
    EMU_IR_LOOP_DOT16_F,    /* sum += f(a[i] * b[i * s]): the product mapped */
    EMU_IR_LOOP_COPY_NUL,   /* *d++ = c = *s++ while c, bytes */
    EMU_IR_LOOP_COPY,       /* *d++ = *s++, n times */
    EMU_IR_LOOP_FILL,       /* *d++ = v, n times */
    EMU_IR_LOOP_AFFINE,     /* recognised throughout, no kernel's shape */
    EMU_IR_LOOP_NO_CALL,    /* a helper or a vector operation in the body */
    EMU_IR_LOOP_NO_FP,      /* floating point in the body */
    EMU_IR_LOOP_NO_SIDE,    /* an exit before the back edge */
    EMU_IR_LOOP_NO_CARRIED, /* a register carried neither as an induction nor a sum */
    EMU_IR_LOOP_NO_ADDR,    /* an access not at an induction register plus a constant */
    EMU_IR_LOOP_NO_EXIT,    /* a back edge on neither a count nor a loaded value */
    EMU_IR_LOOP_NO_FLAGS,   /* the flag word read before the body sets it */
    EMU_IR_LOOP_KINDS
} emu_ir_loop_kind_t;

/* Guest register file indices the analysis tracks: GET/PUT's imm below this. */
#define EMU_IR_LOOP_MAX_REGS 64u

/* The analysis, for a block whose back edge is to its own start. */
/*
 * A 16-bit dot product as a kernel runs it (docs/jit/autovectorisation.md,
 * P2): guest register numbers, as GET and PUT name them.
 */
#define EMU_IR_DOT_FN_OPS 4u

typedef struct emu_ir_dot {
    bool ok;            /* every field below is filled */
    uint8_t xreg, yreg; /* the two elements' induction registers */
    bool xswap, yswap;  /* a halfword byte-swapped before its sign (PowerPC) */
    int32_t xoff, yoff; /* each element at its register plus this */
    int32_t xstride;    /* bytes an iteration, a constant */
    uint8_t ysreg;      /* y's stride is r[ysreg] << ysshift, or ysconst if 0xFF */
    uint8_t ysshift;
    int32_t ysconst;
    uint8_t ereg;       /* the back edge's induction: on while r[ereg] + ec != L */
    int32_t estep, ec;
    uint8_t lreg;       /* L is r[lreg], or lconst if 0xFF */
    uint32_t lconst;
    uint8_t acc;        /* the sum */
    uint32_t insns;     /* guest instructions an iteration retires */
    /*
     * DOT16_F's map from the product p to what is summed: p itself (0
     * chains), chain 0 of p (1), or chain 0 of p times chain 1 of p (2)
     * -- CoreMark's bit_extract(p, 2, 4) * bit_extract(p, 5, 7). A chain
     * is single-input operations with a constant (EMU_IR_ADDI, ANDI,
     * ORI, XORI, SHLI, SHRI, SARI, ROTLI, SEXT8/16, ZEXT8/16, NOT, NEG),
     * from p outward, as the IR defines each.
     */
    uint8_t fchains;
    uint8_t flen[2];
    uint8_t fop[2][EMU_IR_DOT_FN_OPS];
    uint32_t fimm[2][EMU_IR_DOT_FN_OPS];
} emu_ir_dot_t;

/*
 * A byte loop as a kernel runs it -- strcpy's copy to the NUL, memcpy's
 * copy and memset's fill: guest register numbers, as GET and PUT name
 * them.
 */
typedef struct emu_ir_copy {
    bool ok;
    uint8_t size;       /* the element's bytes, 1 */
    uint8_t sreg, dreg; /* source (0xFF for a fill) and destination registers */
    int32_t soff, doff; /* each element at its register plus this */
    int32_t step;       /* bytes an iteration, both registers: the element's */
    uint8_t vreg;       /* a fill's value: r[vreg], or vconst if 0xFF */
    uint32_t vconst;
    uint8_t ereg;       /* a counted loop's back edge, as emu_ir_dot_t's */
    int32_t estep, ec;
    uint8_t lreg;
    uint32_t lconst;
    uint32_t insns;
} emu_ir_copy_t;

typedef struct emu_ir_loop {
    emu_ir_loop_kind_t kind;
    uint32_t back;  /* the back edge's index in the block */
    uint8_t loads, stores, inductions, sums;
    uint8_t sticky; /* registers ORed into each iteration: a sticky flag */
    emu_ir_dot_t dot;   /* for EMU_IR_LOOP_DOT16 */
    emu_ir_copy_t copy; /* for COPY_NUL, COPY and FILL */
} emu_ir_loop_t;

/*
 * Whether `b` loops on itself and, if it does, what it is. Reads the
 * block as the passes left it; changes nothing.
 */
bool emu_ir_loop_shape(const emu_ir_block_t *b, emu_ir_loop_t *out);


/* An operation's name, "?" for none: for diagnostics. */
const char *emu_ir_op_name(uint8_t op);

#if defined(EMU_JIT_LOOP_CENSUS) && EMU_JIT_LOOP_CENSUS
typedef struct emu_ir_census {
    uint32_t pc;
    uint32_t insns;   /* guest instructions an iteration, from the RETIREs */
    uint16_t loads, stores, muls, calls, ops;
    uint8_t shape; /* emu_ir_loop_kind_t, EMU_IR_LOOP_KINDS for none */
    uint64_t entries, iters, caps;
} emu_ir_census_t;

/* The record for the block being lowered, or NULL: emu_ir_census_begin. */
extern emu_ir_census_t *emu_ir_census_cur;
void emu_ir_census_begin(const emu_ir_block_t *b);
#endif

/* ------------------------------------------------------------------ */
/* Optimisation                                                        */
/* ------------------------------------------------------------------ */

typedef struct emu_ir_opt_stats {
    uint32_t single_use; /* values with exactly one reader        */
    uint32_t flags_removed; /* dead EMU_IR_SETF deleted              */
    uint32_t gets_removed; /* guest register reloads elided         */
    uint32_t zero_gets; /* reads of a zero register made constants */
    uint32_t conds_forwarded; /* GETCONDs made compares of a SUB's operands */
    uint32_t branch_folded; /* EXIT_IFs on a SETCC's 0/1 made compares */
    /*
     * Blocks that loop on themselves, by what emu_ir_loop_shape found
     * (EMU_IR_LOOP_*): a kernel, or why not. Counted, nothing changed:
     * docs/jit/autovectorisation.md, P1.
     */
    uint32_t loop_shape[16];
    uint32_t kernels; /* loops given an EMU_IR_KERNEL */
    uint32_t puts_removed; /* guest register stores made redundant  */
    uint32_t setpcs_removed; /* pc writes overwritten unobserved     */
    uint32_t folded; /* constants absorbed into an immediate  */
    uint32_t addr_folded; /* displacements folded into a LOAD/STORE */
    uint32_t identities; /* no-op arithmetic turned into a move    */
    uint32_t macs; /* multiply+add pairs fused into an MLA   */
    uint32_t dead_removed; /* values nothing consumed               */
    uint32_t retires_merged; /* RETIREs folded into a later one       */
    uint32_t tail_exits; /* blocks whose last pc write now exits  */
    uint32_t blocks; /* times emu_ir_optimise ran             */
} emu_ir_opt_stats_t;

/*
 * Everything emu_ir_optimise has done since the process started.
 *
 * The per-block stats were written into a caller's struct and the only
 * caller passed NULL, so nothing could observe them -- which for an
 * optimiser means no way to tell a pass that is not paying from a pass
 * that never runs. This tree's own rule: identical counters mean the
 * code never ran, and a counter with no reader cannot even be identical.
 */
void emu_ir_opt_totals(emu_ir_opt_stats_t *out);

/*
 * A lowering refused an operation: the backends call this where their
 * lower_one returns false, and it counts the refusal only if
 * emu_ir_can_lower had promised that operation -- a frontend that
 * believed the promise lost the whole block where it would have kept it
 * with a helper. Both backends' defaults have broken it, each time
 * silently: Thumb-2 promised CLMUL, and RV32 CoreMark under the JIT
 * was 32% slower on the F746 with every host suite green. The run's
 * report prints a non-zero count as `unkept`, with the first operation
 * refused, and board-ab.sh flags a row whose firmware printed one.
 */
void emu_ir_note_refused(uint8_t op, uint8_t aux);

/* The promises broken so far; *op and *aux the first one's. */
uint32_t emu_ir_unkept(uint8_t *op, uint8_t *aux);

/* Defined under Lowering below; the passes read one field of it. */
typedef struct emu_ir_target emu_ir_target_t;

/*
 * Run every pass over the block. Host-independent by construction: a
 * pass that needed to know the host would belong in the backend.
 *
 * `live_out` names the flags that must survive to the end of the block.
 * Pass EMU_IR_F_ALL unless the frontend can prove otherwise -- a flag
 * examined by the next block or by an interrupt handler is live, and
 * getting this wrong deletes a flag definition a later block depends on,
 * which no test that runs one block at a time would catch.
 *
 * `t` is the *guest* description, not the host's: the passes need it for
 * one thing only, which is which guest registers are hardwired to zero.
 * That is a property of the architecture rather than of the machine the
 * block is being compiled for, so taking it here does not make the
 * passes host-dependent. It may be NULL, meaning no register is
 * hardwired -- but see the note on pass_reg_traffic before passing NULL
 * for an architecture that has one.
 */
void emu_ir_optimise(emu_ir_block_t *b, const emu_ir_target_t *t,
                     uint8_t live_out, emu_ir_opt_stats_t *stats);

/* ------------------------------------------------------------------ */
/* Lowering                                                            */
/* ------------------------------------------------------------------ */

/*
 * What a backend needs to know about the frontend it is lowering for:
 * where the guest register file and flags live, and what to call for
 * EMU_IR_HELPER. Supplied once, not per instruction.
 */

typedef struct emu_ir_target {
    /* Byte offset of guest register `n` within emu_cpu_t. */
    uint32_t (*reg_offset)(uint32_t n);
    /*
     * Where guest vector register n starts in the cpu state, for
     * EMU_IR_VOP; NULL for a guest that has none, which then emits none.
     */
    uint32_t (*vreg_offset)(uint32_t n);
    /*
     * The saturation word the narrowing clips set to 1 (RISC-V's vxsat),
     * a uint32_t in the cpu state; meaningful wherever vreg_offset is.
     */
    uint32_t vsat_offset;

    /* Byte offset of the word holding the guest's condition flags. */
    uint32_t flags_offset;

    /*
     * Where each IR flag sits in that word. Indexed by bit position of
     * EMU_IR_F_*; a guest that lacks a flag gives it 0.
     */
    uint32_t flag_bit[4];

    /*
     * The word's C is ARM's: set by an add that carries (FS_ADD) and by
     * a subtract that does *not* borrow (FS_SUB) -- where every other
     * guest here, and the IR, mean a borrow -- and the conditions that
     * read C (LTU, GEU, LEU, GTU) read it inverted. So after a subtract
     * each compare condition is still the relation of its operands, and
     * pass_cond_forward holds; and the word is the APSR's own, which a
     * host with ARM's flags writes with MRS and nothing to invert.
     * ARMv7-M's. FS_SHIFT's C is bit 0 of `b` either way.
     */
    bool flags_arm_carry;

    /* Guest register `n` reads as zero and ignores writes. */
    bool (*reg_is_zero)(uint32_t n);

    /* Byte offset of the guest pc. */
    uint32_t pc_offset;

    /* Helpers, indexed by the `imm` of EMU_IR_HELPER. */
    const void *const *helpers;
    uint32_t helper_count;

    /*
     * Guest memory. A guest address is not a host address -- it goes
     * through a bus with regions, permissions and passthrough windows --
     * so a backend cannot emit a plain load. These are what let
     * EMU_IR_LOAD, EMU_IR_STORE and the EMU_IR_BITOP_* class be lowered
     * generically rather than each frontend routing them to a helper of
     * its own shape.
     *
     * `spec` is EMU_IR_MEM_AUX: size in bytes plus a sign-extend flag.
     * Both return non-zero if the access trapped, in which case the trap
     * has already been entered and the block must stop. Both go to the
     * same load and store the interpreter uses, so everything those do
     * beyond the access itself -- reservations, PMP, translation -- is
     * had for free.
     *
     * The guest pc must already point at the faulting instruction when
     * either is called, because that is what the trap records. Frontends
     * emit EMU_IR_SETPC before a memory operation for exactly that
     * reason, rather than only after each instruction.
     */
    uint32_t (*load)(emu_cpu_t *cpu, uint32_t addr, uint32_t spec,
                     uint32_t *out);
    uint32_t (*store)(emu_cpu_t *cpu, uint32_t addr, uint32_t spec,
                      uint32_t val);

    /*
     * Floating point. NULL where the guest has none, which is what a
     * backend tests before lowering any of the FP class -- the same way
     * a NULL `load` makes it decline memory.
     *
     * `freg_offset` locates the FP register file inside emu_cpu_t.
     * `fp_flags` accumulates one block's worth of host sticky flags,
     * given in EMU_IR_FE_* terms, into wherever the guest keeps them;
     * it is called once per block that holds an operation from FADD to
     * FCLASS, on the single path every exit takes -- so such a block
     * does not chain.
     *
     * FGET and FPUT are register moves and call nothing: a block of
     * only those chains. A guest that tracks extension state (RISC-V's
     * mstatus.FS) marks it in the block it translates, where the
     * register is written -- RV32's `fput` -- not through this hook.
     */
    uint32_t (*freg_offset)(uint32_t n);
    void (*fp_flags)(emu_cpu_t *cpu, uint32_t flags);

    /*
     * EMU_IR_VFOP's element: op (EMU_IR_VF_*) of a, b and c rounded by
     * rm (EMU_IR_FRM_*), its EMU_IR_FE_* flags ORed into *flags -- the
     * guest's own arithmetic, which the IR interpreter uses as the
     * reference, where host floats would answer for the host's FPU.
     * NULL where the guest has no vector floating point.
     */
    uint32_t (*vfop)(uint32_t op, uint32_t a, uint32_t b, uint32_t c,
                     uint32_t rm, uint32_t *flags);
} emu_ir_target_t;

/*
 * The kernels' table: a descriptor's index (EMU_IR_NO_KERNEL when the
 * table is full), and the run an EMU_IR_KERNEL makes -- the guest
 * instructions it retired, 0 where it ran none.
 */
#define EMU_IR_NO_KERNEL 0xFFFFFFFFu
/*
 * Its entries: shared by equal loops, so a guest makes a few (RV32
 * CoreMark 3 with every kind on, crypto 4). Each is 120 bytes of a
 * board's RAM -- at 64 the table and the recogniser's arrays took a
 * third of the F446's guest RAM. A loop that finds it full stays
 * scalar, and is counted (`full` below).
 */
#define EMU_IR_KERNELS 16u
uint32_t emu_ir_kernel_add(const emu_ir_dot_t *d, const emu_ir_target_t *t,
                           const emu_ir_fastmem_t *w);
uint32_t emu_ir_kernel_add_copy(const emu_ir_copy_t *c, emu_ir_loop_kind_t kind,
                                const emu_ir_target_t *t, const emu_ir_fastmem_t *w,
                                const emu_ir_fastmem_t *rom);
uint32_t emu_ir_kernel_run(emu_cpu_t *cpu, uint32_t index, uint32_t retired);
/*
 * A dot product's descriptor, registers resolved to their offsets in the
 * cpu, for a backend that emits the loop in the block rather than calling
 * emu_ir_kernel_run (Thumb-2): the call alone cost what CoreMark's six
 * iterations an entry save. False for an index that is not a DOT16. What
 * the emitted loop does must be what emu_ir_kernel_run does, or decline
 * -- running the scalar body instead, which ends in the same state.
 */
#define EMU_IR_KERNEL_NO_REG 0xFFFFFFFFu
/*
 * EMU_IR_KERNEL's aux: the kind (EMU_IR_LOOP_*), and this bit for a dot
 * product of the shape a backend may emit in the block -- x's stride 2,
 * either element byte-swapped or not. The pass asks emu_ir_can_lower with it
 * before inserting, so a kernel the backend would neither call nor emit
 * takes no table entry and puts no op in the block.
 */
#define EMU_IR_KERNEL_PLAIN 0x80u
typedef struct emu_ir_kernel_dot {
    uint32_t xo, yo, eo, ao; /* x and y's induction registers, the back edge's, the sum */
    uint32_t lo, so;         /* L and y's stride registers, or EMU_IR_KERNEL_NO_REG */
    int32_t xoff, yoff, xstride, ysconst, estep, ec;
    uint32_t lconst, ysshift, insns;
    uint32_t base, size; /* the RAM window */
    const uint8_t *host;
    bool xswap, yswap;
    uint8_t fchains; /* the map, as emu_ir_dot_t's */
    uint8_t flen[2];
    uint8_t fop[2][EMU_IR_DOT_FN_OPS];
    uint32_t fimm[2][EMU_IR_DOT_FN_OPS];
} emu_ir_kernel_dot_t;
bool emu_ir_kernel_dot(uint32_t index, emu_ir_kernel_dot_t *out);
/* Iterations the loops a backend emitted itself ran, for the report. */
extern uint32_t emu_ir_kernel_inline_iters;
/*
 * The kinds the JIT gives a kernel, a bit each (1u << EMU_IR_LOOP_*).
 * Copy to the NUL and the 16-bit dot product by default -- the second
 * only where the backend emits it or calls it (EMU_IR_KERNEL_PLAIN) --
 * and not the copy and the fill, which were measured and did not pay on
 * the F746 (docs/jit/autovectorisation.md). fuzz-ir sets every kind, so
 * none goes untested.
 */
extern uint32_t emu_ir_kernel_kinds;
void emu_ir_kernel_reset(void); /* no block may name one: a harness's */
/*
 * Descriptors made, calls, calls that ran iterations, iterations run, and
 * loops refused one because the table was full.
 */
void emu_ir_kernel_stats(uint32_t *kernels, uint32_t *calls, uint32_t *runs,
                         uint32_t *iters, uint32_t *full);

/*
 * Lower an optimised block to host code through emu_jit_emit*().
 * Returns false if the buffer overflowed, in which case the caller
 * discards the block exactly as it would a declined translation.
 */
bool emu_ir_lower(const emu_ir_block_t *b, const emu_ir_target_t *t);

/*
 * Can this backend lower this operation, with this `aux`?
 *
 * The frontend asks *before* emitting, so that an operation only some
 * hosts have becomes a helper call rather than a declined block. Without
 * it a frontend has two bad choices: emit the op and lose the whole
 * block wherever it is unsupported, or never emit it and lose the
 * lowering wherever it exists -- and this project has already measured
 * that ending a block costs more than translating one instruction badly.
 *
 * `aux` is part of the question, not decoration. A host may have an
 * instruction for an operation and no encoding for one of its rounding
 * modes: x86 and ARM both lack RISC-V's ties-away, and a backend that
 * answered on the opcode alone would be asked to emit it anyway.
 *
 * Answering `true` is a promise. emu_ir_lower returning false for an
 * operation this said it could do discards a block the frontend built on
 * the strength of the answer.
 */
bool emu_ir_can_lower(emu_ir_op_t op, uint8_t aux);

/* ------------------------------------------------------------------ */
/* Register allocation                                                 */
/* ------------------------------------------------------------------ */

/* This temp did not get a register and lives in its frame slot. */
#define EMU_IR_NO_REG 0xFFu

/* As many host registers as any backend here offers the allocator. */
#define EMU_IR_MAX_HOST_REGS 8u

/*
 * Assign temps to host registers, linear scan over live intervals.
 *
 * Host-independent because the only thing it needs from the host is how
 * many registers there are: which ones they are, and what it costs to
 * reach them, is the backend's business. `assign[t]` comes back as an
 * index below `nregs`, or EMU_IR_NO_REG for a temp that stays in the
 * frame. The return value is how many registers were touched, and the
 * set is always 0..n-1 -- the scan takes the lowest free index, so a
 * backend can save exactly that many and no more.
 *
 * Linear scan is not a compromise here, it is the shape of the problem.
 * A temp is written once by construction and a block has one entry and
 * no joins, so a live range is a single interval [def, last use] and the
 * intervals arrive already sorted by start point. Graph colouring would
 * build an interference graph that is by construction an interval graph
 * and colour it optimally -- for the same answer, at translation time,
 * which is already 48-54% of host cycles at the code-cache size a
 * microcontroller gets.
 *
 * Two rules deserve stating because they are where this could go wrong:
 *
 *   - a register is reused only once its previous occupant's last use is
 *     *strictly* before the new definition. Sharing on the boundary
 *     instruction would be sound only if every lowering read its
 *     operands before writing its destination, which is true today and
 *     is not a property the IR guarantees.
 *   - the registers a backend offers must be callee-saved on its host.
 *     Every value being in the frame is what made a helper call clobber
 *     nothing, and allocation gives that up; the alternative -- spilling
 *     around calls -- would cost more than it saves in blocks that
 *     average four guest instructions.
 *
 * Temps whose single reader is the very next live instruction are
 * deliberately left unallocated: the backends' reload elision already
 * hands those over in the scratch register for nothing, and spending one
 * of four registers on a value that lives for one instruction is exactly
 * the wrong trade.
 */
uint32_t emu_ir_regalloc(const emu_ir_block_t *b, uint32_t nregs,
                         uint8_t *assign);

/* ------------------------------------------------------------------ */
/* What a host's jit.c needs from a frontend                           */
/* ------------------------------------------------------------------ */

/*
 * Everything that differs between one frontend/host pair and another,
 * gathered so the pair itself does not need a file.
 *
 * Before this, each pair had its own translator: the same three-line
 * pipeline, the same block cache adapters, and a handful of genuinely
 * frontend-specific callbacks, copied per pair. Three frontends and
 * three hosts is nine copies of that. What is left below is the handful
 * -- everything a host cannot know and the IR does not carry -- and one
 * jit.c per host consumes it.
 */
typedef struct emu_ir_frontend {
    const char *name;

    /* Guest instructions to IR; see the per-frontend header. */
    uint32_t (*translate)(emu_cpu_t *cpu, uint32_t pc, emu_ir_block_t *b);

    /* Where this guest keeps its registers, flags, pc and memory. */
    const emu_ir_target_t *target;

    /*
     * The parts of emu_jit_ops_t only the frontend can answer. They are
     * held here rather than built by each pair, which is what stops the
     * dispatch loop's contract from being restated once per pair -- and
     * that contract is where this project has found most of its bugs.
     */
    void (*bind)(emu_cpu_t *cpu, emu_jit_hot_t *out);
    const emu_backend_t *interp;
    bool (*is_idle)(emu_cpu_t *cpu);
    bool (*wake)(emu_cpu_t *cpu);
    bool (*take_irq)(emu_cpu_t *cpu);
    /*
     * Whether this frontend's interpreter leaves an interrupt's entry
     * uncounted, so that the JIT's dispatcher does too. RV32's and
     * PowerPC's count one, ARMv7-M's and G4MH's do not; the dispatcher
     * counted one for all four, and ARMv7-M CoreMark retired 11,145,477
     * under the JIT against 11,144,363 interpreted -- a SysTick each.
     */
    bool irq_not_retired;
    /* As emu_jit_ops_t::fault_not_charged; copied with irq_not_retired. */
    bool fault_not_charged;
    void (*count)(emu_cpu_t *cpu, uint32_t n);
    void (*after_interp)(emu_cpu_t *cpu);

    /* Code cache to ask the framework for. */
    uint32_t code_bytes;

    /*
     * How much of this guest's state a differential check compares --
     * enough to cover the register file, pc and flags, and no more. Not
     * the whole struct: that carries bus pointers, cycle counters and
     * scratch that the two paths are not required to agree on.
     */
    uint32_t diff_state_bytes;
} emu_ir_frontend_t;

/*
 * Execute an optimised block directly, without emitting anything.
 *
 * The backend's other execution strategy, and its own reference: it
 * shares nothing with emu_ir_lower but this header, so running a block
 * both ways and comparing guest state is a differential check between
 * two independent implementations rather than against a hand-written
 * expectation.
 *
 * Returns false for a block containing something it does not model, in
 * which case the caller has made no progress and nothing was changed.
 * That is EMU_IR_FSQRT and EMU_IR_FMA, which it cannot round as a host
 * does without libm, and an access or a helper the target has no
 * function for.
 *
 * It runs a block as the lowerings do, not merely its instructions:
 * a constant exit to `b->start_pc` is a back edge taken while fewer than
 * EMU_JIT_LOOP_CAP instructions have retired since entry, and helpers
 * are called as `uint32_t (*)(emu_cpu_t *, uint32_t a, uint32_t b)`,
 * the convention both lowerings use. `retired`, if not NULL, receives
 * the count a compiled block returns.
 */
bool emu_ir_interp(const emu_ir_block_t *b, emu_cpu_t *cpu,
                   const emu_ir_target_t *t, uint32_t *retired);

#ifdef __cplusplus
}
#endif

#endif /* EMU_IR_H */
