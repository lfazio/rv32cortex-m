/* SPDX-License-Identifier: Apache-2.0 */
/*
 * rv_vec.c - the vector unit: Zve32x, VLEN 128 (Zvl128b), ELEN 32.
 *
 * **In progress, and so advertised nowhere** (RV_EXT_ZVE32X, off by
 * default): what is here and what is not is in
 * docs/frontend/rv32-vector.md, and an instruction not yet here raises
 * illegal instruction, as one outside the extension would. Both backends
 * reach this file: the interpreter for OP-V and the vector loads and
 * stores; the JIT through its FP helper for the loads and stores, which
 * share the FP opcodes, and by declining OP-V.
 *
 * The semantics are RVV 1.0's as Spike implements them, which is what
 * the probes compare against (tests/arch-test/probes/vec-*.S): vset's
 * three AVL rules, vill with vl 0 for a vtype this unit cannot hold,
 * elements from vstart to vl, and tail and masked-off elements left
 * undisturbed -- which the agnostic policies permit too. A fault at an
 * element leaves vstart at it, and the instruction restarts there.
 */

#include "rv32/rv_vec.h"

#include "rv32/rv_csr.h"

#include <string.h>

#if RV_EXT_ZVE32X

#define VILL 0x80000000u

static rv_exc_t illegal(uint32_t insn, uint32_t *tval)
{
    *tval = insn;
    return RV_EXC_ILLEGAL_INSN;
}

static void vs_dirty(rv_hart_t *h)
{
    h->mstatus |= MSTATUS_VS_MASK;
}

static void xset(rv_hart_t *h, uint32_t rd, uint32_t v)
{
    if (rd != 0u) {
        h->x[rd] = v;
    }
}

/* ------------------------------------------------------------------ */
/* vtype                                                               */
/* ------------------------------------------------------------------ */

static uint32_t sew_of(uint32_t vt)
{
    return 8u << ((vt >> 3) & 7u);
}

/* log2 of LMUL: -3..3, or 4 for the reserved encoding. */
static int32_t lmul_log2(uint32_t vt)
{
    const int32_t m = (int32_t)(vt & 7u);

    return (m >= 4) ? m - 8 : m;
}

/*
 * VLMAX, or 0 for a vtype this unit cannot hold: reserved bits, SEW
 * beyond ELEN, LMUL 1/16 (the reserved encoding), or a fractional LMUL
 * too small for SEW -- LMUL must be at least SEW / ELEN, so with ELEN 32
 * mf2 holds SEW 16, mf4 SEW 8, and mf8 nothing. Spike's rule.
 */
static uint32_t vlmax_of(uint32_t vt)
{
    const uint32_t vsew = (vt >> 3) & 7u;
    const int32_t lg = lmul_log2(vt);

    if ((vt >> 8) != 0u || vsew > 2u || lg < -3) {
        return 0u;
    }
    const uint32_t sew = 8u << vsew;

    if (lg >= 0) {
        return (RV_VLEN / sew) << lg;
    }
    if (sew > (RV_ELEN >> (uint32_t)(-lg))) {
        return 0u;
    }
    return (RV_VLEN / sew) >> (uint32_t)(-lg);
}

static rv_exc_t vsetvl(rv_hart_t *h, uint32_t insn, uint32_t *tval)
{
    const uint32_t rd = (insn >> 7) & 31u;
    const uint32_t rs1 = (insn >> 15) & 31u;
    const uint32_t rs2 = (insn >> 20) & 31u;
    uint32_t vt;
    bool imm = false;

    if ((insn >> 31) == 0u) { /* vsetvli */
        vt = (insn >> 20) & 0x7FFu;
    } else if ((insn >> 30) == 3u) { /* vsetivli: the AVL in rs1's field */
        vt = (insn >> 20) & 0x3FFu;
        imm = true;
    } else if ((insn >> 25) == 0x40u) { /* vsetvl */
        vt = h->x[rs2];
    } else {
        return illegal(insn, tval);
    }

    uint32_t vlmax = vlmax_of(vt);

    /*
     * rs1 and rd both x0 keep vl, and a new SEW/LMUL ratio would change
     * VLMAX under it: the specification reserves that and lets vill be
     * set, which Spike does -- and so this, as the probes compare.
     */
    if (!imm && rs1 == 0u && rd == 0u && vt != h->vtype &&
        vlmax != vlmax_of(h->vtype)) {
        vlmax = 0u;
    }
    if (vlmax == 0u) {
        h->vtype = VILL;
        h->vl = 0u;
    } else {
        uint32_t avl;

        h->vtype = vt;
        if (imm) {
            avl = rs1;
        } else if (rs1 != 0u) {
            avl = h->x[rs1];
        } else if (rd != 0u) {
            avl = UINT32_MAX;
        } else {
            avl = h->vl; /* keep vl: VLMAX is the same, or vill above */
        }
        h->vl = (avl < vlmax) ? avl : vlmax;
    }
    h->vstart = 0u;
    xset(h, rd, h->vl);
    vs_dirty(h);
    return RV_EXC_NONE;
}

/* ------------------------------------------------------------------ */
/* Elements                                                            */
/* ------------------------------------------------------------------ */

/* Element i of the group at register vr, eb bytes wide, little-endian. */
static uint32_t el_get(const rv_hart_t *h, uint32_t vr, uint32_t i,
                       uint32_t eb)
{
    const uint8_t *p = &h->v[0][0] + vr * RV_VLENB + i * eb;
    uint32_t v = 0u;

    for (uint32_t k = eb; k-- > 0u;) {
        v = (v << 8) | p[k];
    }
    return v;
}

static void el_set(rv_hart_t *h, uint32_t vr, uint32_t i, uint32_t eb,
                   uint32_t v)
{
    uint8_t *p = &h->v[0][0] + vr * RV_VLENB + i * eb;

    for (uint32_t k = 0u; k < eb; k++) {
        p[k] = (uint8_t)(v >> (8u * k));
    }
}

/* Mask bit i of register vr. */
static bool m_get(const rv_hart_t *h, uint32_t vr, uint32_t i)
{
    return ((h->v[vr][i >> 3] >> (i & 7u)) & 1u) != 0u;
}

static void m_set(rv_hart_t *h, uint32_t vr, uint32_t i, bool b)
{
    const uint8_t bit = (uint8_t)(1u << (i & 7u));

    if (b) {
        h->v[vr][i >> 3] |= bit;
    } else {
        h->v[vr][i >> 3] &= (uint8_t)~bit;
    }
}

static int32_t sx(uint32_t v, uint32_t bits)
{
    return (bits >= 32u) ? (int32_t)v
                         : (int32_t)(v << (32u - bits)) >> (32u - bits);
}

static uint32_t smask_of(uint32_t sew)
{
    return (sew >= 32u) ? 0xFFFFFFFFu : ((1u << sew) - 1u);
}

/* Registers in a group of LMUL 2^lg: a fractional one is one register. */
static uint32_t group_of(int32_t lg)
{
    return (lg > 0) ? (1u << (uint32_t)lg) : 1u;
}

/* ------------------------------------------------------------------ */
/* Integer arithmetic: OPIVV, OPIVX, OPIVI                             */
/* ------------------------------------------------------------------ */

/* Which of .vv (1), .vx (2) and .vi (4) each funct6 has; 0: not here. */
static uint8_t opi_forms(uint32_t f6)
{
    switch (f6) {
    case 0x00u: /* vadd */
    case 0x09u: /* vand */
    case 0x0Au: /* vor */
    case 0x0Bu: /* vxor */
    case 0x17u: /* vmerge, vmv.v */
    case 0x18u: /* vmseq */
    case 0x19u: /* vmsne */
    case 0x1Cu: /* vmsleu */
    case 0x1Du: /* vmsle */
    case 0x25u: /* vsll */
    case 0x28u: /* vsrl */
    case 0x29u: /* vsra */
        return 7u;
    case 0x02u: /* vsub */
    case 0x04u: /* vminu */
    case 0x05u: /* vmin */
    case 0x06u: /* vmaxu */
    case 0x07u: /* vmax */
    case 0x1Au: /* vmsltu */
    case 0x1Bu: /* vmslt */
        return 3u;
    case 0x03u: /* vrsub */
    case 0x1Eu: /* vmsgtu */
    case 0x1Fu: /* vmsgt */
        return 6u;
    default:
        return 0u;
    }
}

static rv_exc_t opi(rv_hart_t *h, uint32_t insn, uint32_t f3, uint32_t *tval)
{
    const uint32_t f6 = insn >> 26;
    const bool vm = ((insn >> 25) & 1u) != 0u;
    const uint32_t vs2 = (insn >> 20) & 31u;
    const uint32_t r1 = (insn >> 15) & 31u;
    const uint32_t vd = (insn >> 7) & 31u;
    const uint32_t form = (f3 == 0u) ? 1u : (f3 == 4u) ? 2u : 4u;
    const uint32_t sew = sew_of(h->vtype);
    const uint32_t eb = sew / 8u;
    const uint32_t sm = smask_of(sew);
    const uint32_t grp = group_of(lmul_log2(h->vtype));
    const bool cmp = f6 >= 0x18u && f6 <= 0x1Fu;
    const bool merge = f6 == 0x17u;
    const bool shift = f6 == 0x25u || f6 == 0x28u || f6 == 0x29u;

    if ((opi_forms(f6) & form) == 0u) {
        return illegal(insn, tval);
    }
    /*
     * Register groups aligned to LMUL; a masked operation may not write
     * v0, which holds its mask -- vmerge's selector included -- unless
     * it produces a mask; vmv.v has vs2 0.
     */
    if ((!cmp && vd % grp != 0u) || vs2 % grp != 0u ||
        (form == 1u && r1 % grp != 0u) || (!vm && vd == 0u && !cmp) ||
        (merge && vm && vs2 != 0u)) {
        return illegal(insn, tval);
    }

    const uint32_t scalar = (form == 2u)   ? (h->x[r1] & sm)
                            : (form == 4u) ? ((uint32_t)sx(r1, 5u) & sm)
                                           : 0u;

    for (uint32_t i = h->vstart; i < h->vl; i++) {
        const uint32_t b = (form == 1u) ? el_get(h, r1, i, eb)
                           : (shift && form == 4u) ? r1
                                                    : scalar;

        if (merge) {
            const uint32_t v =
                (vm || m_get(h, 0u, i)) ? b : el_get(h, vs2, i, eb);

            el_set(h, vd, i, eb, v);
            continue;
        }
        if (!vm && !m_get(h, 0u, i)) {
            continue;
        }

        const uint32_t a = el_get(h, vs2, i, eb);
        const int32_t sa = sx(a, sew);
        const int32_t sb = sx(b, sew);
        const uint32_t sh = b & (sew - 1u);
        uint32_t r = 0u;
        bool m = false;

        switch (f6) {
        case 0x00u: r = a + b; break;
        case 0x02u: r = a - b; break;
        case 0x03u: r = b - a; break;
        case 0x04u: r = (a < b) ? a : b; break;
        case 0x05u: r = (sa < sb) ? a : b; break;
        case 0x06u: r = (a > b) ? a : b; break;
        case 0x07u: r = (sa > sb) ? a : b; break;
        case 0x09u: r = a & b; break;
        case 0x0Au: r = a | b; break;
        case 0x0Bu: r = a ^ b; break;
        case 0x18u: m = a == b; break;
        case 0x19u: m = a != b; break;
        case 0x1Au: m = a < b; break;
        case 0x1Bu: m = sa < sb; break;
        case 0x1Cu: m = a <= b; break;
        case 0x1Du: m = sa <= sb; break;
        case 0x1Eu: m = a > b; break;
        case 0x1Fu: m = sa > sb; break;
        case 0x25u: r = a << sh; break;
        case 0x28u: r = a >> sh; break;
        default: /* 0x29u */ r = (uint32_t)(sa >> sh); break;
        }
        if (cmp) {
            m_set(h, vd, i, m);
        } else {
            el_set(h, vd, i, eb, r & sm);
        }
    }
    h->vstart = 0u;
    vs_dirty(h);
    return RV_EXC_NONE;
}

/* ------------------------------------------------------------------ */
/* OPMVV and OPMVX                                                     */
/* ------------------------------------------------------------------ */

static uint32_t mul_hi(uint32_t a, uint32_t b, uint32_t sew, int kind)
{
    /* kind 0 unsigned, 1 signed, 2 vs2 signed and the other unsigned */
    int64_t p;

    if (kind == 1) {
        p = (int64_t)sx(a, sew) * (int64_t)sx(b, sew);
    } else if (kind == 2) {
        p = (int64_t)sx(a, sew) * (int64_t)(uint64_t)b;
    } else {
        p = (int64_t)((uint64_t)a * (uint64_t)b);
    }
    return (uint32_t)((uint64_t)p >> sew);
}

static uint32_t divide(uint32_t f6, uint32_t a, uint32_t b, uint32_t sew)
{
    const int32_t sa = sx(a, sew);
    const int32_t sb = sx(b, sew);
    const uint32_t sm = smask_of(sew);
    const int32_t min =
        (sew >= 32u) ? INT32_MIN : -(int32_t)(1u << (sew - 1u));

    switch (f6) {
    case 0x20u: /* vdivu */
        return (b == 0u) ? sm : a / b;
    case 0x21u: /* vdiv */
        if (b == 0u) {
            return sm;
        }
        if (sa == min && sb == -1) {
            return a; /* overflow: the dividend */
        }
        return (uint32_t)(sa / sb);
    case 0x22u: /* vremu */
        return (b == 0u) ? a : a % b;
    default: /* 0x23u, vrem */
        if (b == 0u) {
            return a;
        }
        if (sa == min && sb == -1) {
            return 0u;
        }
        return (uint32_t)(sa % sb);
    }
}

static rv_exc_t opm(rv_hart_t *h, uint32_t insn, uint32_t f3, uint32_t *tval)
{
    const uint32_t f6 = insn >> 26;
    const bool vm = ((insn >> 25) & 1u) != 0u;
    const uint32_t vs2 = (insn >> 20) & 31u;
    const uint32_t r1 = (insn >> 15) & 31u;
    const uint32_t vd = (insn >> 7) & 31u;
    const bool vv = (f3 == 2u);
    const uint32_t sew = sew_of(h->vtype);
    const uint32_t eb = sew / 8u;
    const uint32_t sm = smask_of(sew);
    const uint32_t grp = group_of(lmul_log2(h->vtype));
    const uint32_t vl = h->vl;

    /* --- the scalar moves and the mask counts (VWXUNARY0, VRXUNARY0) */
    if (f6 == 0x10u) {
        if (vv && r1 == 0x00u && vm) { /* vmv.x.s: element 0, whatever vl */
            xset(h, vd, (uint32_t)sx(el_get(h, vs2, 0u, eb), sew));
            h->vstart = 0u;
            return RV_EXC_NONE;
        }
        if (vv && (r1 == 0x10u || r1 == 0x11u)) { /* vcpop.m, vfirst.m */
            uint32_t n = 0u;
            uint32_t first = UINT32_MAX;

            if (h->vstart != 0u) {
                return illegal(insn, tval);
            }
            for (uint32_t i = 0u; i < vl; i++) {
                if ((vm || m_get(h, 0u, i)) && m_get(h, vs2, i)) {
                    n++;
                    if (first == UINT32_MAX) {
                        first = i;
                    }
                }
            }
            xset(h, vd, (r1 == 0x10u) ? n : first);
            return RV_EXC_NONE;
        }
        if (!vv && vs2 == 0u && vm) { /* vmv.s.x */
            if (vl > 0u && h->vstart < vl) {
                el_set(h, vd, 0u, eb, h->x[r1] & sm);
            }
            h->vstart = 0u;
            vs_dirty(h);
            return RV_EXC_NONE;
        }
        return illegal(insn, tval);
    }

    /* --- vmsbf, vmsof, vmsif, viota, vid (VMUNARY0) ----------------- */
    if (f6 == 0x14u && vv) {
        if (r1 == 0x11u) { /* vid.v */
            if (vs2 != 0u || vd % grp != 0u || (!vm && vd == 0u)) {
                return illegal(insn, tval);
            }
            for (uint32_t i = h->vstart; i < vl; i++) {
                if (vm || m_get(h, 0u, i)) {
                    el_set(h, vd, i, eb, i & sm);
                }
            }
        } else if (r1 == 0x10u) { /* viota.m */
            uint32_t n = 0u;

            if (h->vstart != 0u || vd % grp != 0u || (!vm && vd == 0u) ||
                vd == vs2) {
                return illegal(insn, tval);
            }
            for (uint32_t i = 0u; i < vl; i++) {
                if (vm || m_get(h, 0u, i)) {
                    el_set(h, vd, i, eb, n & sm);
                    n += m_get(h, vs2, i) ? 1u : 0u;
                }
            }
        } else if (r1 >= 1u && r1 <= 3u) { /* vmsbf, vmsof, vmsif */
            bool seen = false;

            if (h->vstart != 0u || vd == vs2 || (!vm && vd == 0u)) {
                return illegal(insn, tval);
            }
            for (uint32_t i = 0u; i < vl; i++) {
                if (!vm && !m_get(h, 0u, i)) {
                    continue;
                }
                const bool s = m_get(h, vs2, i);
                bool r;

                if (r1 == 1u) {
                    r = !seen && !s; /* before the first */
                } else if (r1 == 2u) {
                    r = !seen && s; /* only the first */
                } else {
                    r = !seen; /* up to and including it */
                }
                seen = seen || s;
                m_set(h, vd, i, r);
            }
        } else {
            return illegal(insn, tval);
        }
        h->vstart = 0u;
        vs_dirty(h);
        return RV_EXC_NONE;
    }

    /* --- mask logical: vl bits, unmasked ---------------------------- */
    if (f6 >= 0x18u && f6 <= 0x1Fu && vv) {
        if (!vm) {
            return illegal(insn, tval);
        }
        for (uint32_t i = h->vstart; i < vl; i++) {
            const bool a = m_get(h, vs2, i);
            const bool b = m_get(h, r1, i);
            bool r;

            switch (f6) {
            case 0x18u: r = a && !b; break; /* vmandn */
            case 0x19u: r = a && b; break; /* vmand */
            case 0x1Au: r = a || b; break; /* vmor */
            case 0x1Bu: r = a != b; break; /* vmxor */
            case 0x1Cu: r = a || !b; break; /* vmorn */
            case 0x1Du: r = !(a && b); break; /* vmnand */
            case 0x1Eu: r = !(a || b); break; /* vmnor */
            default: r = a == b; break; /* vmxnor */
            }
            m_set(h, vd, i, r);
        }
        h->vstart = 0u;
        vs_dirty(h);
        return RV_EXC_NONE;
    }

    /* --- single-width reductions ------------------------------------ */
    if (f6 <= 0x07u && vv) {
        if (h->vstart != 0u || vs2 % grp != 0u) {
            return illegal(insn, tval);
        }
        if (vl == 0u) {
            return RV_EXC_NONE;
        }
        uint32_t acc = el_get(h, r1, 0u, eb);

        for (uint32_t i = 0u; i < vl; i++) {
            if (!vm && !m_get(h, 0u, i)) {
                continue;
            }
            const uint32_t e = el_get(h, vs2, i, eb);

            switch (f6) {
            case 0x00u: acc = (acc + e) & sm; break; /* vredsum */
            case 0x01u: acc &= e; break; /* vredand */
            case 0x02u: acc |= e; break; /* vredor */
            case 0x03u: acc ^= e; break; /* vredxor */
            case 0x04u: acc = (e < acc) ? e : acc; break; /* vredminu */
            case 0x05u: acc = (sx(e, sew) < sx(acc, sew)) ? e : acc; break;
            case 0x06u: acc = (e > acc) ? e : acc; break; /* vredmaxu */
            default: acc = (sx(e, sew) > sx(acc, sew)) ? e : acc; break;
            }
        }
        el_set(h, vd, 0u, eb, acc);
        vs_dirty(h);
        return RV_EXC_NONE;
    }

    /* --- multiply, divide, multiply-add: .vv and .vx ---------------- */
    const bool muldiv = f6 >= 0x20u && f6 <= 0x27u;
    const bool fma = f6 == 0x29u || f6 == 0x2Bu || f6 == 0x2Du || f6 == 0x2Fu;

    if (!muldiv && !fma) {
        return illegal(insn, tval);
    }
    if (vd % grp != 0u || vs2 % grp != 0u || (vv && r1 % grp != 0u) ||
        (!vm && vd == 0u)) {
        return illegal(insn, tval);
    }
    for (uint32_t i = h->vstart; i < vl; i++) {
        if (!vm && !m_get(h, 0u, i)) {
            continue;
        }
        const uint32_t a = el_get(h, vs2, i, eb);
        const uint32_t b = vv ? el_get(h, r1, i, eb) : (h->x[r1] & sm);
        uint32_t r;

        switch (f6) {
        case 0x24u: r = mul_hi(a, b, sew, 0); break; /* vmulhu */
        case 0x25u: r = a * b; break; /* vmul */
        case 0x26u: r = mul_hi(a, b, sew, 2); break; /* vmulhsu */
        case 0x27u: r = mul_hi(a, b, sew, 1); break; /* vmulh */
        case 0x29u: r = b * el_get(h, vd, i, eb) + a; break; /* vmadd */
        case 0x2Bu: r = a - b * el_get(h, vd, i, eb); break; /* vnmsub */
        case 0x2Du: r = b * a + el_get(h, vd, i, eb); break; /* vmacc */
        case 0x2Fu: r = el_get(h, vd, i, eb) - b * a; break; /* vnmsac */
        default: r = divide(f6, a, b, sew); break;
        }
        el_set(h, vd, i, eb, r & sm);
    }
    h->vstart = 0u;
    vs_dirty(h);
    return RV_EXC_NONE;
}

/* ------------------------------------------------------------------ */
/* Loads and stores                                                    */
/* ------------------------------------------------------------------ */

static rv_exc_t vmem(rv_hart_t *h, uint32_t insn, uint32_t *tval)
{
    const bool load = (insn & 0x7Fu) == 0x07u;
    const uint32_t width = (insn >> 12) & 7u;
    const uint32_t nf = insn >> 29;
    const bool mew = ((insn >> 28) & 1u) != 0u;
    const uint32_t mop = (insn >> 26) & 3u;
    const bool vm = ((insn >> 25) & 1u) != 0u;
    const uint32_t umop = (insn >> 20) & 31u; /* lumop/sumop, or rs2/vs2 */
    const uint32_t rs1 = (insn >> 15) & 31u;
    const uint32_t vd = (insn >> 7) & 31u; /* vs3 for a store */
    const uint32_t eew = (width == 0u) ? 8u : (8u << (width - 4u));
    const uint32_t base = h->x[rs1];

    if (mew || eew > RV_ELEN) {
        return illegal(insn, tval);
    }

    const bool unit = mop == 0u;
    const bool whole = unit && umop == 0x08u;
    const bool mask = unit && umop == 0x0Bu;
    const bool ff = unit && load && umop == 0x10u;
    const bool indexed = (mop & 1u) != 0u;

    if (unit && umop != 0u && !whole && !mask && !ff) {
        return illegal(insn, tval);
    }
    /* Segments (nf > 0) are not here yet; whole registers use the field. */
    if (nf != 0u && !whole) {
        return illegal(insn, tval);
    }

    uint32_t evl; /* elements */
    uint32_t eb; /* bytes an element */
    uint32_t ib = 0u; /* bytes an index, indexed only */

    if (whole) {
        const uint32_t nreg = nf + 1u;

        /* Whole registers ignore vtype -- even vill -- and vl. */
        if ((nreg & (nreg - 1u)) != 0u || vd % nreg != 0u || !vm ||
            (!load && width != 0u)) {
            return illegal(insn, tval);
        }
        eb = eew / 8u;
        evl = nreg * RV_VLENB / eb;
    } else {
        if ((h->vtype & VILL) != 0u) {
            return illegal(insn, tval);
        }
        const uint32_t sew = sew_of(h->vtype);
        const int32_t lg = lmul_log2(h->vtype);
        /* log2(EEW / SEW * LMUL), the group this access's data takes */
        int32_t emul = lg;
        uint32_t e = eew;
        uint32_t s = sew;

        while (e > s) {
            emul++;
            e >>= 1;
        }
        while (s > e) {
            emul--;
            s >>= 1;
        }
        if (mask) {
            if (eew != 8u || !vm) {
                return illegal(insn, tval);
            }
            eb = 1u;
            evl = (h->vl + 7u) / 8u;
        } else if (indexed) {
            /* The data at SEW and LMUL, the index vector at EEW. */
            if (emul < -3 || emul > 3 || vd % group_of(lg) != 0u ||
                umop % group_of(emul) != 0u || (load && !vm && vd == 0u)) {
                return illegal(insn, tval);
            }
            eb = sew / 8u;
            ib = eew / 8u;
            evl = h->vl;
        } else {
            if (emul < -3 || emul > 3 || vd % group_of(emul) != 0u ||
                (load && !vm && vd == 0u)) {
                return illegal(insn, tval);
            }
            eb = eew / 8u;
            evl = h->vl;
        }
    }

    for (uint32_t i = h->vstart; i < evl; i++) {
        if (!vm && !m_get(h, 0u, i)) {
            continue;
        }
        uint32_t addr;

        if (indexed) {
            addr = base + el_get(h, umop, i, ib);
        } else if (mop == 2u) { /* strided: rs2 holds the stride */
            addr = base + i * h->x[umop];
        } else {
            addr = base + i * eb;
        }

        rv_exc_t exc;

        if (load) {
            uint32_t v = 0u;

            exc = rv_hart_load(h, addr, eb, false, &v);
            if (exc == RV_EXC_NONE) {
                el_set(h, vd, i, eb, v);
                continue;
            }
            /* Fault-only-first: a fault past element 0 shortens vl. */
            if (ff && i > 0u) {
                h->vl = i;
                break;
            }
        } else {
            exc = rv_hart_store(h, addr, eb, el_get(h, vd, i, eb));
            if (exc == RV_EXC_NONE) {
                continue;
            }
        }
        h->vstart = i;
        *tval = addr;
        if (load) {
            vs_dirty(h);
        }
        return exc;
    }
    h->vstart = 0u;
    if (load) {
        vs_dirty(h);
    }
    return RV_EXC_NONE;
}

/* ------------------------------------------------------------------ */

rv_exc_t rv_hart_vec(rv_hart_t *h, uint32_t insn, uint32_t *tval)
{
    const uint32_t op = insn & 0x7Fu;

    if ((h->mstatus & MSTATUS_VS_MASK) == 0u) {
        return illegal(insn, tval);
    }
    if (op == 0x07u || op == 0x27u) {
        return vmem(h, insn, tval);
    }
    if (op != 0x57u) {
        return illegal(insn, tval);
    }

    const uint32_t f3 = (insn >> 12) & 7u;

    if (f3 == 7u) {
        return vsetvl(h, insn, tval);
    }
    if ((h->vtype & VILL) != 0u) {
        return illegal(insn, tval);
    }
    switch (f3) {
    case 0u: /* OPIVV */
    case 3u: /* OPIVI */
    case 4u: /* OPIVX */
        return opi(h, insn, f3, tval);
    case 2u: /* OPMVV */
    case 6u: /* OPMVX */
        return opm(h, insn, f3, tval);
    default: /* OPFVV, OPFVF: Zve32f, not this */
        return illegal(insn, tval);
    }
}

#endif /* RV_EXT_ZVE32X */
