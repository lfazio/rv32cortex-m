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

/* Whether the groups of an registers at a and bn at b share one. */
static bool overlaps(uint32_t a, uint32_t an, uint32_t b, uint32_t bn)
{
    return a < b + bn && b < a + an;
}

/*
 * Whether a destination of a wider EEW overlaps a narrower source in a
 * way the specification reserves: anything but the source being the
 * highest-numbered part of the destination's group -- and, for a source
 * of fractional EMUL, anything at all. Spike's is_overlapped_widen.
 */
static bool overlaps_widen(uint32_t d, uint32_t dn, uint32_t s, uint32_t sn,
                           bool frac)
{
    if (!frac && d < s && overlaps(d, dn, s, sn) &&
        !overlaps(d, dn, s + sn, sn)) {
        return false;
    }
    return overlaps(d, dn, s, sn);
}

/*
 * The rounding increment for v shifted right by d (< 64) bits under
 * vxrm: the specification's r in roundoff_unsigned and roundoff_signed,
 * read from v's two's-complement bits either way.
 */
static uint64_t rnd(uint64_t v, uint32_t d, uint32_t vxrm)
{
    if (d == 0u) {
        return 0u;
    }
    const uint64_t half = (v >> (d - 1u)) & 1u;
    const bool low = (v & ((UINT64_C(1) << (d - 1u)) - 1u)) != 0u;
    const uint64_t lsb = (v >> d) & 1u;

    switch (vxrm & 3u) {
    case 0u: /* rnu: to nearest, ties up */
        return half;
    case 1u: /* rne: to nearest, ties to even */
        return half & ((low ? 1u : 0u) | lsb);
    case 2u: /* rdn: truncate */
        return 0u;
    default: /* rod: jam what was lost into the lsb */
        return (lsb ^ 1u) & ((half != 0u || low) ? 1u : 0u);
    }
}

/* v clipped to SEW's signed range; *sat set if it had to be. */
static uint32_t clip_s(int64_t v, uint32_t sew, bool *sat)
{
    const int64_t max = (int64_t)(smask_of(sew) >> 1);

    if (v > max) {
        *sat = true;
        return (uint32_t)max;
    }
    if (v < -max - 1) {
        *sat = true;
        return (uint32_t)(-max - 1);
    }
    return (uint32_t)v;
}

/* An OP-V instruction's fields, and the vtype it runs under. */
typedef struct {
    uint32_t f6;
    uint32_t vs2;
    uint32_t r1;   /* vs1, rs1 or the immediate */
    uint32_t vd;
    uint32_t form; /* 1 .vv, 2 .vx, 4 .vi */
    bool vm;       /* unmasked */
    uint32_t sew;
    uint32_t eb;   /* bytes an element */
    uint32_t sm;   /* SEW's mask */
    int32_t lg;    /* log2 LMUL */
    uint32_t grp;  /* registers a group */
    uint32_t vl;
    uint32_t vlmax;
} vop_t;

static vop_t vop_of(const rv_hart_t *h, uint32_t insn, uint32_t f3)
{
    vop_t o;

    o.f6 = insn >> 26;
    o.vm = ((insn >> 25) & 1u) != 0u;
    o.vs2 = (insn >> 20) & 31u;
    o.r1 = (insn >> 15) & 31u;
    o.vd = (insn >> 7) & 31u;
    o.form = (f3 == 0u || f3 == 2u) ? 1u : (f3 == 4u || f3 == 6u) ? 2u : 4u;
    o.sew = sew_of(h->vtype);
    o.eb = o.sew / 8u;
    o.sm = smask_of(o.sew);
    o.lg = lmul_log2(h->vtype);
    o.grp = group_of(o.lg);
    o.vl = h->vl;
    o.vlmax = vlmax_of(h->vtype);
    return o;
}

/* Whether element i takes part: unmasked, or its v0 bit set. */
static bool active(const rv_hart_t *h, const vop_t *o, uint32_t i)
{
    return o->vm || m_get(h, 0u, i);
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

static rv_exc_t opi_more(rv_hart_t *h, uint32_t insn, uint32_t f3,
                         uint32_t *tval);

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
        return opi_more(h, insn, f3, tval);
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

static rv_exc_t opm_more(rv_hart_t *h, uint32_t insn, uint32_t f3,
                         uint32_t *tval);

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
        return opm_more(h, insn, f3, tval);
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
/* Carry, fixed point, narrowing: the rest of OPIVV, OPIVX, OPIVI      */
/* ------------------------------------------------------------------ */

/*
 * vadc, vmadc, vsbc, vmsbc. vadc and vsbc take v0 as the carry and must
 * be written so (vm 0); vmadc and vmsbc take it as one when vm is 0 and
 * have none when it is 1. Every element below vl takes part, and the
 * mask forms write a bit an element.
 */
static rv_exc_t carry(rv_hart_t *h, const vop_t *o, uint32_t insn,
                      uint32_t *tval)
{
    const bool sub = o->f6 >= 0x12u;
    const bool mask_out = (o->f6 & 1u) != 0u;
    const bool vv = o->form == 1u;

    if ((sub && o->form == 4u) || (!mask_out && o->vm) ||
        o->vs2 % o->grp != 0u || (vv && o->r1 % o->grp != 0u)) {
        return illegal(insn, tval);
    }
    if (mask_out) {
        /* One mask register, which may be a source's first only. */
        if ((o->vd != o->vs2 && overlaps(o->vd, 1u, o->vs2, o->grp)) ||
            (vv && o->vd != o->r1 && overlaps(o->vd, 1u, o->r1, o->grp))) {
            return illegal(insn, tval);
        }
    } else if (o->vd == 0u || o->vd % o->grp != 0u) {
        return illegal(insn, tval);
    }

    const uint32_t b0 = (o->form == 2u) ? h->x[o->r1] : (uint32_t)sx(o->r1, 5u);

    for (uint32_t i = 0u; i < o->vl; i++) {
        const uint64_t c = (!o->vm && m_get(h, 0u, i)) ? 1u : 0u;
        const uint64_t a = el_get(h, o->vs2, i, o->eb);
        const uint64_t b = vv ? el_get(h, o->r1, i, o->eb) : (b0 & o->sm);
        const uint64_t r = sub ? a - b - c : a + b + c;

        if (mask_out) {
            m_set(h, o->vd, i, ((r >> o->sew) & 1u) != 0u);
        } else {
            el_set(h, o->vd, i, o->eb, (uint32_t)r & o->sm);
        }
    }
    return RV_EXC_NONE;
}

/*
 * The single-width fixed-point operations: vsaddu, vsadd, vssubu,
 * vssub, vsmul, vssrl, vssra here, and OPM's averaging vaaddu, vaadd,
 * vasubu, vasub. Worked in 64 bits, which hold every intermediate at
 * SEW 32; vxsat is set by any element that saturated, and never cleared.
 */
static rv_exc_t fixed(rv_hart_t *h, const vop_t *o, bool opm, uint32_t insn,
                      uint32_t *tval)
{
    const uint32_t f6 = o->f6;
    const bool vv = o->form == 1u;
    uint8_t forms;

    if (opm) {
        forms = 3u; /* vaaddu, vaadd, vasubu, vasub */
    } else if (f6 == 0x20u || f6 == 0x21u || f6 == 0x2Au || f6 == 0x2Bu) {
        forms = 7u;
    } else {
        forms = 3u; /* vssubu, vssub, vsmul */
    }
    if ((forms & o->form) == 0u || (!o->vm && o->vd == 0u) ||
        o->vd % o->grp != 0u || o->vs2 % o->grp != 0u ||
        (vv && o->r1 % o->grp != 0u)) {
        return illegal(insn, tval);
    }

    const uint32_t xrm = h->vxrm;
    const bool shift = !opm && (f6 == 0x2Au || f6 == 0x2Bu);
    const uint32_t b0 = (o->form == 2u)              ? (h->x[o->r1] & o->sm)
                        : (o->form == 4u && shift) ? o->r1
                                                   : ((uint32_t)sx(o->r1, 5u) & o->sm);
    bool sat = false;

    for (uint32_t i = 0u; i < o->vl; i++) {
        if (!active(h, o, i)) {
            continue;
        }
        const uint32_t a = el_get(h, o->vs2, i, o->eb);
        const uint32_t b = vv ? el_get(h, o->r1, i, o->eb) : b0;
        const int64_t sa = sx(a, o->sew);
        const int64_t sb = sx(b, o->sew);
        const uint32_t sh = b & (o->sew - 1u);
        uint32_t r;

        if (opm) {
            int64_t v;

            switch (f6) {
            case 0x08u: v = (int64_t)a + (int64_t)b; break; /* vaaddu */
            case 0x09u: v = sa + sb; break; /* vaadd */
            case 0x0Au: v = (int64_t)a - (int64_t)b; break; /* vasubu */
            default: v = sa - sb; break; /* 0x0B, vasub */
            }
            r = (uint32_t)((uint64_t)(v >> 1) + rnd((uint64_t)v, 1u, xrm));
        } else {
            switch (f6) {
            case 0x20u: { /* vsaddu */
                const uint64_t v = (uint64_t)a + b;

                sat = sat || v > o->sm;
                r = (v > o->sm) ? o->sm : (uint32_t)v;
                break;
            }
            case 0x21u: /* vsadd */
                r = clip_s(sa + sb, o->sew, &sat);
                break;
            case 0x22u: /* vssubu */
                sat = sat || a < b;
                r = (a < b) ? 0u : a - b;
                break;
            case 0x23u: /* vssub */
                r = clip_s(sa - sb, o->sew, &sat);
                break;
            case 0x27u: { /* vsmul: (a * b) >> (SEW - 1), rounded */
                const int64_t min = -(int64_t)(o->sm >> 1) - 1;
                const int64_t p = sa * sb;

                if (sa == min && sb == min) {
                    sat = true;
                    r = o->sm >> 1;
                } else {
                    r = (uint32_t)((uint64_t)(p >> (o->sew - 1u)) +
                                   rnd((uint64_t)p, o->sew - 1u, xrm));
                }
                break;
            }
            case 0x2Au: /* vssrl */
                r = (uint32_t)((a >> sh) + rnd(a, sh, xrm));
                break;
            default: /* 0x2B, vssra */
                r = (uint32_t)((uint64_t)(sa >> sh) + rnd((uint64_t)sa, sh, xrm));
                break;
            }
        }
        el_set(h, o->vd, i, o->eb, r & o->sm);
    }
    if (sat) {
        h->vxsat = 1u;
    }
    return RV_EXC_NONE;
}

/*
 * vnsrl, vnsra, vnclipu, vnclip: vs2 is 2 * SEW wide in a group twice
 * LMUL, vd SEW wide, and the shift is taken modulo 2 * SEW. vd may be
 * the lowest-numbered register of vs2's group, not any other.
 */
static rv_exc_t narrow(rv_hart_t *h, const vop_t *o, uint32_t insn,
                       uint32_t *tval)
{
    const uint32_t wsew = o->sew * 2u;
    const uint32_t wg = group_of(o->lg + 1);
    const bool vv = o->form == 1u;

    if (o->lg > 2 || wsew > RV_ELEN || o->vs2 % wg != 0u ||
        o->vd % o->grp != 0u || (!o->vm && o->vd == 0u) ||
        (vv && o->r1 % o->grp != 0u) ||
        (o->vd != o->vs2 && overlaps(o->vd, o->grp, o->vs2, wg))) {
        return illegal(insn, tval);
    }

    const uint32_t xrm = h->vxrm;
    const uint32_t b0 = (o->form == 2u) ? h->x[o->r1] : o->r1;
    bool sat = false;

    for (uint32_t i = 0u; i < o->vl; i++) {
        if (!active(h, o, i)) {
            continue;
        }
        const uint32_t w = el_get(h, o->vs2, i, 2u * o->eb);
        const int64_t sw = sx(w, wsew);
        const uint32_t sh =
            (vv ? el_get(h, o->r1, i, o->eb) : b0) & (wsew - 1u);
        uint32_t r;

        switch (o->f6) {
        case 0x2Cu: /* vnsrl */
            r = w >> sh;
            break;
        case 0x2Du: /* vnsra */
            r = (uint32_t)(sw >> sh);
            break;
        case 0x2Eu: { /* vnclipu */
            const uint64_t v = (uint64_t)(w >> sh) + rnd(w, sh, xrm);

            sat = sat || v > o->sm;
            r = (v > o->sm) ? o->sm : (uint32_t)v;
            break;
        }
        default: /* 0x2F, vnclip */
            r = clip_s((sw >> sh) + (int64_t)rnd((uint64_t)sw, sh, xrm),
                       o->sew, &sat);
            break;
        }
        el_set(h, o->vd, i, o->eb, r & o->sm);
    }
    if (sat) {
        h->vxsat = 1u;
    }
    return RV_EXC_NONE;
}

/*
 * vrgather (.vv, .vx, .vi), vrgatherei16.vv, vslideup and vslidedown
 * (.vx, .vi), and OPM's vslide1up, vslide1down and vcompress. An index
 * or offset is unsigned and as wide as it comes: an index at or past
 * VLMAX reads 0, as does vslidedown past the group's end.
 */
static rv_exc_t permute(rv_hart_t *h, const vop_t *o, bool opm, uint32_t insn,
                        uint32_t *tval)
{
    const uint32_t f6 = o->f6;
    const bool vv = o->form == 1u;

    if ((!o->vm && o->vd == 0u) || o->vd % o->grp != 0u ||
        o->vs2 % o->grp != 0u) {
        return illegal(insn, tval);
    }

    if (opm && f6 == 0x17u) { /* vcompress.vm: vs1 is the selecting mask */
        uint32_t pos = 0u;

        if (!vv || !o->vm || o->vd == o->vs2 ||
            overlaps(o->vd, o->grp, o->r1, 1u)) {
            return illegal(insn, tval);
        }
        for (uint32_t i = 0u; i < o->vl; i++) {
            if (m_get(h, o->r1, i)) {
                el_set(h, o->vd, pos++, o->eb, el_get(h, o->vs2, i, o->eb));
            }
        }
        return RV_EXC_NONE;
    }
    if (opm) { /* vslide1up.vx (0x0E), vslide1down.vx (0x0F) */
        const uint32_t x = h->x[o->r1] & o->sm;

        if (o->form != 2u || (f6 == 0x0Eu && o->vd == o->vs2)) {
            return illegal(insn, tval);
        }
        for (uint32_t i = 0u; i < o->vl; i++) {
            if (!active(h, o, i)) {
                continue;
            }
            uint32_t v;

            if (f6 == 0x0Eu) {
                v = (i == 0u) ? x : el_get(h, o->vs2, i - 1u, o->eb);
            } else {
                v = (i == o->vl - 1u) ? x : el_get(h, o->vs2, i + 1u, o->eb);
            }
            el_set(h, o->vd, i, o->eb, v);
        }
        return RV_EXC_NONE;
    }

    const uint32_t off = (o->form == 2u) ? h->x[o->r1] : o->r1;

    if (f6 == 0x0Cu || (f6 == 0x0Eu && vv)) { /* vrgather, vrgatherei16 */
        const bool ei16 = f6 == 0x0Eu;
        uint32_t ib = o->eb; /* bytes an index */
        uint32_t ig = o->grp; /* registers the indices take */

        if (ei16) {
            /* Indices at EEW 16: EMUL = 16 / SEW * LMUL. */
            const int32_t emul = o->lg + ((o->sew == 8u) ? 1 : (o->sew == 32u) ? -1 : 0);

            if (emul < -3 || emul > 3) {
                return illegal(insn, tval);
            }
            ib = 2u;
            ig = group_of(emul);
        }
        if (o->vd == o->vs2 ||
            (vv && (o->r1 % ig != 0u || overlaps(o->vd, o->grp, o->r1, ig)))) {
            return illegal(insn, tval);
        }
        for (uint32_t i = 0u; i < o->vl; i++) {
            if (!active(h, o, i)) {
                continue;
            }
            const uint32_t idx = vv ? el_get(h, o->r1, i, ib) : off;

            el_set(h, o->vd, i, o->eb,
                   (idx >= o->vlmax) ? 0u : el_get(h, o->vs2, idx, o->eb));
        }
        return RV_EXC_NONE;
    }
    if (vv || (f6 == 0x0Eu && o->vd == o->vs2)) {
        return illegal(insn, tval);
    }
    if (f6 == 0x0Eu) { /* vslideup: below the offset is left alone */
        for (uint32_t i = off; i < o->vl; i++) {
            if (active(h, o, i)) {
                el_set(h, o->vd, i, o->eb, el_get(h, o->vs2, i - off, o->eb));
            }
        }
    } else { /* vslidedown, which may run in place: ascending reads ahead */
        for (uint32_t i = 0u; i < o->vl; i++) {
            if (active(h, o, i)) {
                const uint64_t src = (uint64_t)i + off;

                el_set(h, o->vd, i, o->eb,
                       (src < o->vlmax) ? el_get(h, o->vs2, (uint32_t)src, o->eb)
                                        : 0u);
            }
        }
    }
    return RV_EXC_NONE;
}

/*
 * vmv1r.v, vmv2r.v, vmv4r.v, vmv8r.v: whole registers, whatever vl --
 * the count is the immediate plus one, a power of two, and both groups
 * aligned to it.
 */
static rv_exc_t vmvnr(rv_hart_t *h, const vop_t *o, uint32_t insn,
                      uint32_t *tval)
{
    const uint32_t n = o->r1 + 1u;

    if (!o->vm || (n & (n - 1u)) != 0u || n > 8u || o->vd % n != 0u ||
        o->vs2 % n != 0u) {
        return illegal(insn, tval);
    }
    if (o->vd != o->vs2) {
        memcpy(h->v[o->vd], h->v[o->vs2], n * RV_VLENB);
    }
    return RV_EXC_NONE;
}

/*
 * vwredsumu, vwredsum: vd[0] at 2 * SEW is vs1[0] at 2 * SEW plus every
 * active element of vs2, zero- or sign-extended.
 */
static rv_exc_t wreduce(rv_hart_t *h, const vop_t *o, uint32_t insn,
                        uint32_t *tval)
{
    const uint32_t wsew = o->sew * 2u;

    if (o->form != 1u || wsew > RV_ELEN || o->vs2 % o->grp != 0u) {
        return illegal(insn, tval);
    }
    if (o->vl == 0u) {
        return RV_EXC_NONE;
    }
    uint32_t acc = el_get(h, o->r1, 0u, 2u * o->eb);

    for (uint32_t i = 0u; i < o->vl; i++) {
        if (active(h, o, i)) {
            const uint32_t e = el_get(h, o->vs2, i, o->eb);

            acc += (o->f6 == 0x31u) ? (uint32_t)sx(e, o->sew) : e;
        }
    }
    el_set(h, o->vd, 0u, 2u * o->eb, acc & smask_of(wsew));
    return RV_EXC_NONE;
}

static rv_exc_t opi_more(rv_hart_t *h, uint32_t insn, uint32_t f3,
                         uint32_t *tval)
{
    const vop_t o = vop_of(h, insn, f3);

    switch (o.f6) {
    case 0x0Cu: /* vrgather */
    case 0x0Eu: /* vrgatherei16, vslideup */
    case 0x0Fu: /* vslidedown */
        return permute(h, &o, false, insn, tval);
    case 0x10u: /* vadc */
    case 0x11u: /* vmadc */
    case 0x12u: /* vsbc */
    case 0x13u: /* vmsbc */
        return carry(h, &o, insn, tval);
    case 0x20u: /* vsaddu */
    case 0x21u: /* vsadd */
    case 0x22u: /* vssubu */
    case 0x23u: /* vssub */
    case 0x2Au: /* vssrl */
    case 0x2Bu: /* vssra */
        return fixed(h, &o, false, insn, tval);
    case 0x27u: /* vsmul; .vi is vmv<n>r.v */
        return (o.form == 4u) ? vmvnr(h, &o, insn, tval)
                              : fixed(h, &o, false, insn, tval);
    case 0x2Cu: /* vnsrl */
    case 0x2Du: /* vnsra */
    case 0x2Eu: /* vnclipu */
    case 0x2Fu: /* vnclip */
        return narrow(h, &o, insn, tval);
    case 0x30u: /* vwredsumu */
    case 0x31u: /* vwredsum */
        return wreduce(h, &o, insn, tval);
    default:
        return illegal(insn, tval);
    }
}

/* ------------------------------------------------------------------ */
/* Widening, extension and the rest of OPMVV and OPMVX                 */
/* ------------------------------------------------------------------ */

/*
 * vwaddu ... vwsub.w, vwmulu, vwmulsu, vwmul, vwmaccu, vwmacc,
 * vwmaccus, vwmaccsu: vd at 2 * SEW in a group twice LMUL; vs2 too for
 * the .w forms. A narrow source may overlap vd only as its
 * highest-numbered part.
 */
static rv_exc_t widen(rv_hart_t *h, const vop_t *o, uint32_t insn,
                      uint32_t *tval)
{
    const uint32_t f6 = o->f6;
    const uint32_t wsew = o->sew * 2u;
    const uint32_t wg = group_of(o->lg + 1);
    const bool vv = o->form == 1u;
    const bool wide2 = f6 >= 0x34u && f6 <= 0x37u; /* the .w forms */
    const bool frac = o->lg < 0;

    if (f6 == 0x39u || (f6 == 0x3Eu && vv) || o->lg > 2 ||
        wsew > RV_ELEN || o->vd % wg != 0u || (!o->vm && o->vd == 0u) ||
        o->vs2 % (wide2 ? wg : o->grp) != 0u ||
        (!wide2 && overlaps_widen(o->vd, wg, o->vs2, o->grp, frac)) ||
        (vv && (o->r1 % o->grp != 0u ||
                overlaps_widen(o->vd, wg, o->r1, o->grp, frac)))) {
        return illegal(insn, tval);
    }

    const uint32_t web = 2u * o->eb;
    const uint32_t b0 = h->x[o->r1] & o->sm;

    for (uint32_t i = 0u; i < o->vl; i++) {
        if (!active(h, o, i)) {
            continue;
        }
        const uint32_t a = el_get(h, o->vs2, i, wide2 ? web : o->eb);
        const uint32_t b = vv ? el_get(h, o->r1, i, o->eb) : b0;
        const int64_t ua = a;
        const int64_t sa = sx(a, wide2 ? wsew : o->sew);
        const int64_t ub = b;
        const int64_t sb = sx(b, o->sew);
        int64_t r;

        switch (f6) {
        case 0x30u: case 0x34u: r = ua + ub; break; /* vwaddu */
        case 0x31u: case 0x35u: r = sa + sb; break; /* vwadd */
        case 0x32u: case 0x36u: r = ua - ub; break; /* vwsubu */
        case 0x33u: case 0x37u: r = sa - sb; break; /* vwsub */
        case 0x38u: r = ua * ub; break; /* vwmulu */
        case 0x3Au: r = sa * ub; break; /* vwmulsu: vs2 signed */
        case 0x3Bu: r = sa * sb; break; /* vwmul */
        default: {
            const int64_t d = el_get(h, o->vd, i, web);

            switch (f6) {
            case 0x3Cu: r = d + ua * ub; break; /* vwmaccu */
            case 0x3Du: r = d + sa * sb; break; /* vwmacc */
            case 0x3Eu: r = d + ub * sa; break; /* vwmaccus: rs1 unsigned */
            default: r = d + sb * ua; break; /* 0x3F, vwmaccsu: vs1 signed */
            }
            break;
        }
        }
        el_set(h, o->vd, i, web, (uint32_t)r & smask_of(wsew));
    }
    return RV_EXC_NONE;
}

/*
 * vzext.vf2/vf4/vf8, vsext.vf2/vf4/vf8 (VXUNARY0, vs1 2-7): vs2 at
 * SEW / f in a group LMUL / f, extended to SEW.
 */
static rv_exc_t extend(rv_hart_t *h, const vop_t *o, uint32_t insn,
                       uint32_t *tval)
{
    if (o->form != 1u || o->r1 < 2u || o->r1 > 7u) {
        return illegal(insn, tval);
    }
    const uint32_t lf = 4u - (o->r1 >> 1); /* log2 f: vf8 3, vf4 2, vf2 1 */
    const bool sign = (o->r1 & 1u) != 0u;
    const uint32_t from = o->sew >> lf;
    const int32_t slg = o->lg - (int32_t)lf;

    if (from < 8u || slg < -3 || o->vd == o->vs2 || (!o->vm && o->vd == 0u) ||
        o->vd % o->grp != 0u || o->vs2 % group_of(slg) != 0u ||
        overlaps_widen(o->vd, o->grp, o->vs2, group_of(slg), slg < 0)) {
        return illegal(insn, tval);
    }
    for (uint32_t i = 0u; i < o->vl; i++) {
        if (active(h, o, i)) {
            const uint32_t e = el_get(h, o->vs2, i, from / 8u);

            el_set(h, o->vd, i, o->eb, sign ? (uint32_t)sx(e, from) & o->sm : e);
        }
    }
    return RV_EXC_NONE;
}

static rv_exc_t opm_more(rv_hart_t *h, uint32_t insn, uint32_t f3,
                         uint32_t *tval)
{
    const vop_t o = vop_of(h, insn, f3);

    if (o.f6 >= 0x08u && o.f6 <= 0x0Bu) { /* vaaddu, vaadd, vasubu, vasub */
        return fixed(h, &o, true, insn, tval);
    }
    if (o.f6 == 0x0Eu || o.f6 == 0x0Fu || o.f6 == 0x17u) {
        return permute(h, &o, true, insn, tval);
    }
    if (o.f6 == 0x12u) {
        return extend(h, &o, insn, tval);
    }
    if (o.f6 >= 0x30u) {
        return widen(h, &o, insn, tval);
    }
    return illegal(insn, tval);
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
    /* nf is a segment's fields less one, and for whole registers their
     * count less one; a mask access has neither. */
    if (nf != 0u && mask) {
        return illegal(insn, tval);
    }

    uint32_t evl; /* elements, or segments */
    uint32_t eb; /* bytes an element */
    uint32_t ib = 0u; /* bytes an index, indexed only */
    uint32_t fields = 1u; /* a segment's */
    uint32_t fstep = 0u; /* registers from one field's group to the next */

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
            const uint32_t dg = group_of(lg);
            const uint32_t ig = group_of(emul);

            fields = nf + 1u;
            fstep = dg;
            if (emul < -3 || emul > 3 || vd % dg != 0u || umop % ig != 0u ||
                fields * dg > 8u || vd + fields * dg > 32u ||
                (load && !vm && vd == 0u)) {
                return illegal(insn, tval);
            }
            /*
             * A load's destination may overlap the indices only as the
             * EEW rules allow, and not at all with more than one field:
             * Spike's VI_CHECK_LD_INDEX.
             */
            for (uint32_t f = 0u; load && f < fields; f++) {
                const uint32_t seg = vd + f * dg;

                if ((eew > sew && seg != umop && overlaps(seg, dg, umop, ig)) ||
                    (eew < sew &&
                     overlaps_widen(seg, dg, umop, ig, emul < 0)) ||
                    (fields >= 2u && overlaps(seg, dg, umop, ig))) {
                    return illegal(insn, tval);
                }
            }
            eb = sew / 8u;
            ib = eew / 8u;
            evl = h->vl;
        } else {
            const uint32_t dg = group_of(emul);

            fields = nf + 1u;
            fstep = dg;
            if (emul < -3 || emul > 3 || vd % dg != 0u ||
                fields * dg > 8u || vd + fields * dg > 32u ||
                (load && !vm && vd == 0u)) {
                return illegal(insn, tval);
            }
            eb = eew / 8u;
            evl = h->vl;
        }
    }

    /*
     * Segment i's field f goes to element i of the group f groups past
     * vd. A fault leaves vstart at its segment, with the fields before
     * it in that segment written, as Spike leaves them.
     */
    for (uint32_t i = h->vstart; i < evl; i++) {
        if (!vm && !m_get(h, 0u, i)) {
            continue;
        }
        for (uint32_t f = 0u; f < fields; f++) {
            const uint32_t reg = vd + f * fstep;
            uint32_t addr;

            if (indexed) {
                addr = base + el_get(h, umop, i, ib) + f * eb;
            } else if (mop == 2u) { /* strided: rs2 holds the stride */
                addr = base + i * h->x[umop] + f * eb;
            } else {
                addr = base + (i * fields + f) * eb;
            }

            rv_exc_t exc;

            if (load) {
                uint32_t v = 0u;

                exc = rv_hart_load(h, addr, eb, false, &v);
                if (exc == RV_EXC_NONE) {
                    el_set(h, reg, i, eb, v);
                    continue;
                }
                /* Fault-only-first: a fault past segment 0 shortens vl. */
                if (ff && i > 0u) {
                    h->vl = i;
                    h->vstart = 0u;
                    return RV_EXC_NONE;
                }
            } else {
                exc = rv_hart_store(h, addr, eb, el_get(h, reg, i, eb));
                if (exc == RV_EXC_NONE) {
                    continue;
                }
            }
            h->vstart = i;
            *tval = addr;
            return exc;
        }
    }
    h->vstart = 0u;
    return RV_EXC_NONE;
}

/* ------------------------------------------------------------------ */

rv_exc_t rv_hart_vec(rv_hart_t *h, uint32_t insn, uint32_t *tval)
{
    const uint32_t op = insn & 0x7Fu;

    const uint32_t f3 = (insn >> 12) & 7u;
    rv_exc_t e;

    if ((h->mstatus & MSTATUS_VS_MASK) == 0u) {
        return illegal(insn, tval);
    }
    if (op == 0x07u || op == 0x27u) {
        e = vmem(h, insn, tval);
    } else if (op != 0x57u) {
        return illegal(insn, tval);
    } else if (f3 == 7u) {
        return vsetvl(h, insn, tval);
    } else if ((h->vtype & VILL) != 0u || h->vstart != 0u) {
        /*
         * Arithmetic from a nonzero vstart is refused, which the
         * specification permits and Spike does: only an access can be
         * stopped part way, so only an access resumes.
         */
        return illegal(insn, tval);
    } else {
        switch (f3) {
        case 0u: /* OPIVV */
        case 3u: /* OPIVI */
        case 4u: /* OPIVX */
            e = opi(h, insn, f3, tval);
            break;
        case 2u: /* OPMVV */
        case 6u: /* OPMVX */
            e = opm(h, insn, f3, tval);
            break;
        default: /* OPFVV, OPFVF: Zve32f, not this */
            return illegal(insn, tval);
        }
    }
    /*
     * Whatever ran wrote vstart, to 0 or to the element that faulted, and
     * so marks VS Dirty -- a store and a scalar result included. Spike's
     * rule, which the probes compare.
     */
    if (e != RV_EXC_ILLEGAL_INSN) {
        vs_dirty(h);
    }
    return e;
}

#endif /* RV_EXT_ZVE32X */
