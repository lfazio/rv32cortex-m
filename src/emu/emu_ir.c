/* SPDX-License-Identifier: Apache-2.0 */
/*
 * emu_ir.c - Building and optimising the IR. See emu_ir.h.
 *
 * Every pass here is a backward or forward linear walk over one block.
 * That is not a placeholder for something cleverer: a block has one
 * entry, no joins and averages about four guest instructions in this
 * project's guests, so anything with a worklist would spend more time
 * setting up than walking. Translation is already 48-54% of host cycles
 * at the code-cache size a microcontroller gets, and a pass that costs
 * more than the code it saves is a net loss however good the output.
 */

#include "emu/emu_bus.h"
#include "emu/emu_ir.h"
#include "emu/emu_jit.h" /* EMU_HAVE_JIT */
#include "emu/emu_lock.h"
#include "emu/emu_regstats.h"

#include <string.h>

/* ------------------------------------------------------------------ */
/* Building                                                            */
/* ------------------------------------------------------------------ */

#if EMU_IR_VECTOR
/*
 * v >> d rounded by rm, as RISC-V's fixed-point rounding (emu_ir.h,
 * EMU_IR_VOP_RM), for a signed v of at most 34 bits and d of at most 32:
 * the increment from the bits shifted out and the one above them.
 */
static int64_t vround(int64_t v, uint32_t d, uint32_t rm)
{
    const uint64_t u = (uint64_t)v;
    const uint64_t below = (d == 0u) ? 0u : (u >> (d - 1u)) & 1u; /* v[d-1] */
    const uint64_t rest = (d <= 1u) ? 0u : u & (((uint64_t)1 << (d - 1u)) - 1u);
    const uint64_t out = (d == 0u) ? 0u : u & (((uint64_t)1 << d) - 1u);
    const uint64_t at = (u >> d) & 1u; /* v[d] */
    uint64_t r;

    switch (rm) {
    case EMU_IR_RM_RNU: r = below; break;
    case EMU_IR_RM_RNE: r = below & ((rest != 0u) | at); break;
    case EMU_IR_RM_RDN: r = 0u; break;
    default: r = (at == 0u) & (out != 0u); break; /* rod */
    }
    return (v >> d) + (int64_t)r;
}

/*
 * EMU_IR_VOP's meaning (emu_ir.h). Element i of a group is bytes
 * i * w to i * w + w - 1 from the group's first register, little-endian.
 */
void emu_ir_vop_ref(uint8_t *vd, const uint8_t *vs2, const uint8_t *vs1,
                    uint32_t scalar, uint8_t aux, uint32_t nregs, uint32_t vl,
                    uint32_t rm)
{
    const uint32_t w = 1u << EMU_IR_VOP_SEW(aux);
    const uint32_t bits = 8u * w;
    const uint32_t mask = (bits == 32u) ? 0xFFFFFFFFu : ((1u << bits) - 1u);
    const uint32_t lanes = nregs * EMU_IR_VREG_BYTES / w;
    const uint32_t n = (vl < lanes) ? vl : lanes;

    if (EMU_IR_V_IS_WIDE(EMU_IR_VOP_OP(aux))) {
        const uint32_t op = EMU_IR_VOP_OP(aux);
        const uint32_t sh = 32u - bits;
        const uint32_t ww = 2u * w; /* d's element */

        for (uint32_t i = 0; i < n; i++) {
            uint32_t a = 0u;
            uint32_t b = 0u;
            uint32_t d = 0u;

            for (uint32_t k = w; k-- > 0u;) {
                a = (a << 8) | vs2[i * w + k];
                b = (b << 8) | vs1[i * w + k];
            }
            for (uint32_t k = ww; k-- > 0u;) {
                d = (d << 8) | vd[i * ww + k];
            }
            if (EMU_IR_VOP_VX(aux)) {
                b = scalar & mask;
            }
            const int32_t sa = (int32_t)(a << sh) >> sh;
            const int32_t sb = (int32_t)(b << sh) >> sh;
            const bool sign_a = op == EMU_IR_V_WMACC || op == EMU_IR_V_WMACCUS;
            const bool sign_b = op == EMU_IR_V_WMACC || op == EMU_IR_V_WMACCSU;
            const uint32_t r = d + (sign_b ? (uint32_t)sb : b) *
                                       (sign_a ? (uint32_t)sa : a);

            for (uint32_t k = 0; k < ww; k++) {
                vd[i * ww + k] = (uint8_t)(r >> (8u * k));
            }
        }
        return;
    }

    for (uint32_t i = 0; i < n; i++) {
        uint32_t a = 0u;
        uint32_t b = 0u;

        for (uint32_t k = w; k-- > 0u;) {
            a = (a << 8) | vs2[i * w + k];
            b = (b << 8) | vs1[i * w + k];
        }
        if (EMU_IR_VOP_VX(aux)) {
            b = scalar & mask;
        }
        /* Sign-extended copies, for the signed forms. */
        const uint32_t sh = 32u - bits;
        const int32_t sa = (int32_t)(a << sh) >> sh;
        const int32_t sb = (int32_t)(b << sh) >> sh;
        const uint32_t amt = b & (bits - 1u);
        uint32_t d = 0u; /* vd's element as it was, for the multiply-adds */
        uint32_t r;

        for (uint32_t k = w; k-- > 0u;) {
            d = (d << 8) | vd[i * w + k];
        }
        switch (EMU_IR_VOP_OP(aux)) {
        case EMU_IR_V_AADDU: /* (a + b) >> 1 in 33 bits, rounded */
            r = (uint32_t)vround((int64_t)a + b, 1u, rm);
            break;
        case EMU_IR_V_AADD:
            r = (uint32_t)vround((int64_t)sa + sb, 1u, rm);
            break;
        case EMU_IR_V_MACC: r = d + b * a; break;
        case EMU_IR_V_NMSAC: r = d - b * a; break;
        case EMU_IR_V_MADD: r = b * d + a; break;
        case EMU_IR_V_NMSUB: r = a - b * d; break;
        case EMU_IR_V_ADD: r = a + b; break;
        case EMU_IR_V_SUB: r = a - b; break;
        case EMU_IR_V_RSUB: r = b - a; break;
        case EMU_IR_V_AND: r = a & b; break;
        case EMU_IR_V_OR: r = a | b; break;
        case EMU_IR_V_XOR: r = a ^ b; break;
        case EMU_IR_V_MINU: r = (a < b) ? a : b; break;
        case EMU_IR_V_MIN: r = (sa < sb) ? a : b; break;
        case EMU_IR_V_MAXU: r = (a > b) ? a : b; break;
        case EMU_IR_V_MAX: r = (sa > sb) ? a : b; break;
        case EMU_IR_V_MUL: r = a * b; break;
        case EMU_IR_V_SLL: r = a << amt; break;
        case EMU_IR_V_SRL: r = a >> amt; break;
        case EMU_IR_V_SRA: r = (uint32_t)(sa >> amt); break;
        default: r = b; break; /* EMU_IR_V_MV */
        }
        for (uint32_t k = 0; k < w; k++) {
            vd[i * w + k] = (uint8_t)(r >> (8u * k));
        }
    }
}

#endif /* EMU_IR_VECTOR */

#if EMU_IR_VECTOR
bool emu_ir_vopm_ref(uint8_t *vd, const uint8_t *vs2, const uint8_t *vs1,
                     const uint8_t *mask, uint32_t scalar, uint8_t aux,
                     uint32_t nregs, uint32_t vl, uint32_t rm)
{
    const uint32_t op = EMU_IR_VOP_OP(aux);
    const uint32_t w = 1u << EMU_IR_VOP_SEW(aux);
    const uint32_t lanes = nregs * EMU_IR_VREG_BYTES / w;
    const uint32_t n = (vl < lanes) ? vl : lanes;
    /* vd's element and group: twice the width, and the registers, wide */
    const uint32_t dw = EMU_IR_V_IS_WIDE(op) ? 2u * w : w;
    const uint32_t dregs = EMU_IR_V_IS_WIDE(op) ? 2u * nregs : nregs;
    uint8_t res[8u * EMU_IR_VREG_BYTES];

    if (EMU_IR_V_IS_NARROW(op)) {
        return emu_ir_vop_narrow_ref(vd, vs2, vs1, mask, scalar, aux, nregs, vl, rm);
    }
    /* Every element below vl computed unmasked, into a copy of vd... */
    memcpy(res, vd, dregs * EMU_IR_VREG_BYTES);
    if (EMU_IR_VOP_OP(aux) == EMU_IR_V_MERGE) {
        const uint8_t mv = EMU_IR_VOP_AUX(EMU_IR_V_MV, EMU_IR_VOP_SEW(aux),
                                          EMU_IR_VOP_VX(aux));

        emu_ir_vop_ref(res, vs2, vs1, scalar, mv, nregs, vl, rm); /* s1 */
    } else {
        emu_ir_vop_ref(res, vs2, vs1, scalar, aux, nregs, vl, rm);
    }
    /* ...then kept where the bit says, or for a merge, s2 where not. */
    for (uint32_t i = 0; i < n; i++) {
        const bool on = ((mask[i >> 3] >> (i & 7u)) & 1u) != 0u;

        if (on) {
            memcpy(vd + i * dw, res + i * dw, dw);
        } else if (op == EMU_IR_V_MERGE) {
            memcpy(vd + i * w, vs2 + i * w, w);
        }
    }
    return false;
}

void emu_ir_vfop_ref(const emu_ir_target_t *t, uint8_t *base, uint8_t aux,
                     uint32_t imm, uint32_t scalar, uint32_t vl, uint32_t *flags)
{
    const uint32_t nregs = EMU_IR_VOP_NREGS(imm);
    const uint32_t n = (vl < nregs * 4u) ? vl : nregs * 4u;
    uint8_t *const vd = base + t->vreg_offset(EMU_IR_VOP_VD(imm));
    const uint8_t *const vs2 = base + t->vreg_offset(EMU_IR_VOP_VS2(imm));
    const uint8_t *const vs1 = base + t->vreg_offset(EMU_IR_VOP_VS1(imm));
    const uint8_t *const mk = base + t->vreg_offset(EMU_IR_VOP_MREG(imm));
    uint32_t r[32];

    /* every source read before vd is written: vd may be one of them */
    for (uint32_t i = 0; i < n; i++) {
        uint32_t a, b, c;

        r[i] = 0u;
        if (EMU_IR_VFOP_MASKED(imm) && ((mk[i >> 3] >> (i & 7u)) & 1u) == 0u) {
            continue;
        }
        memcpy(&a, vs2 + 4u * i, 4u);
        memcpy(&c, vd + 4u * i, 4u);
        if (EMU_IR_VFOP_VF(imm)) {
            b = scalar;
        } else {
            memcpy(&b, vs1 + 4u * i, 4u);
        }
        r[i] = t->vfop(EMU_IR_VFOP_OP(aux), a, b, c, EMU_IR_VFOP_RM(aux), flags);
    }
    for (uint32_t i = 0; i < n; i++) {
        if (EMU_IR_VFOP_MASKED(imm) && ((mk[i >> 3] >> (i & 7u)) & 1u) == 0u) {
            continue;
        }
        memcpy(vd + 4u * i, &r[i], 4u);
    }
}

void emu_ir_vfop_call(void *cpu, const emu_ir_target_t *t, uint32_t aux,
                      uint32_t imm, uint32_t scalar, uint32_t vl)
{
    uint32_t fl = 0u;

    emu_ir_vfop_ref(t, (uint8_t *)cpu, (uint8_t)aux, imm, scalar, vl, &fl);
    if (fl != 0u && t->fp_flags != NULL) {
        t->fp_flags((emu_cpu_t *)cpu, fl);
    }
}

/* A little-endian element of w bytes at p. */
static uint32_t vred_get(const uint8_t *p, uint32_t w)
{
    uint32_t v = 0u;

    for (uint32_t k = w; k-- > 0u;) {
        v = (v << 8) | p[k];
    }
    return v;
}

void emu_ir_vred_ref(uint8_t *vd, const uint8_t *vs2, const uint8_t *vs1,
                     const uint8_t *mask, uint8_t aux, uint32_t nregs,
                     uint32_t vl)
{
    const uint32_t rop = EMU_IR_VRED_OP(aux);
    const uint32_t w = 1u << EMU_IR_VRED_SEW(aux);
    const uint32_t rw = EMU_IR_R_IS_WIDE(rop) ? 2u * w : w; /* the result's */
    const uint32_t bits = 8u * w;
    const uint32_t rmask = (rw == 4u) ? 0xFFFFFFFFu : (1u << (8u * rw)) - 1u;
    const uint32_t lanes = nregs * EMU_IR_VREG_BYTES / w;
    const uint32_t n = (vl < lanes) ? vl : lanes;
    const uint32_t xs = 32u - bits; /* an element as a signed word */

    if (n == 0u) {
        return;
    }
    uint32_t acc = vred_get(vs1, rw);

    for (uint32_t i = 0; i < n; i++) {
        if (mask != NULL && ((mask[i >> 3] >> (i & 7u)) & 1u) == 0u) {
            continue;
        }
        const uint32_t e = vred_get(vs2 + i * w, w);
        const int32_t se = (int32_t)(e << xs) >> xs;
        const int32_t sa = (int32_t)(acc << xs) >> xs;

        switch (rop) {
        case EMU_IR_R_SUM: acc = (acc + e) & rmask; break;
        case EMU_IR_R_AND: acc &= e; break;
        case EMU_IR_R_OR: acc |= e; break;
        case EMU_IR_R_XOR: acc ^= e; break;
        case EMU_IR_R_MINU: acc = (e < acc) ? e : acc; break;
        case EMU_IR_R_MIN: acc = (se < sa) ? e : acc; break;
        case EMU_IR_R_MAXU: acc = (e > acc) ? e : acc; break;
        case EMU_IR_R_MAX: acc = (se > sa) ? e : acc; break;
        case EMU_IR_R_WSUMU: acc = (acc + e) & rmask; break;
        default: acc = (acc + (uint32_t)se) & rmask; break; /* WSUM */
        }
    }
    for (uint32_t k = 0; k < rw; k++) {
        vd[k] = (uint8_t)(acc >> (8u * k));
    }
}

bool emu_ir_vop_narrow_ref(uint8_t *vd, const uint8_t *vs2, const uint8_t *vs1,
                           const uint8_t *mask, uint32_t scalar, uint8_t aux,
                           uint32_t nregs, uint32_t vl, uint32_t rm)
{
    const uint32_t op = EMU_IR_VOP_OP(aux);
    const uint32_t w = 1u << EMU_IR_VOP_SEW(aux);
    const uint32_t bits = 8u * w;
    const uint32_t ww = 2u * w;
    const uint32_t lanes = nregs * EMU_IR_VREG_BYTES / w;
    const uint32_t n = (vl < lanes) ? vl : lanes;
    const int64_t smax = (int64_t)((1u << (bits - 1u)) - 1u);
    const uint64_t umax = (1u << bits) - 1u;
    bool sat = false;

    /* Ascending: with vd at s2's start, element i's bytes are read first. */
    for (uint32_t i = 0; i < n; i++) {
        uint32_t x = 0u;
        uint32_t s = 0u;

        if (mask != NULL && ((mask[i >> 3] >> (i & 7u)) & 1u) == 0u) {
            continue;
        }
        for (uint32_t k = ww; k-- > 0u;) {
            x = (x << 8) | vs2[i * ww + k];
        }
        for (uint32_t k = w; k-- > 0u;) {
            s = (s << 8) | vs1[i * w + k];
        }
        if (EMU_IR_VOP_VX(aux)) {
            s = scalar;
        }
        const uint32_t sh = s & (2u * bits - 1u);
        const uint32_t xs = 32u - 2u * bits; /* x as a 2 * SEW signed value */
        const int64_t sx = (int32_t)(x << xs) >> xs;
        uint32_t r;

        switch (op) {
        case EMU_IR_V_NSRL: r = (uint32_t)((uint64_t)x >> sh); break;
        case EMU_IR_V_NSRA: r = (uint32_t)(sx >> sh); break;
        case EMU_IR_V_NCLIPU: {
            const uint64_t v = (uint64_t)vround((int64_t)x, sh, rm);

            sat = sat || v > umax;
            r = (uint32_t)((v > umax) ? umax : v);
            break;
        }
        default: { /* EMU_IR_V_NCLIP */
            const int64_t v = vround(sx, sh, rm);

            sat = sat || v > smax || v < -smax - 1;
            r = (uint32_t)((v > smax) ? smax : (v < -smax - 1) ? -smax - 1 : v);
            break;
        }
        }
        for (uint32_t k = 0; k < w; k++) {
            vd[i * w + k] = (uint8_t)(r >> (8u * k));
        }
    }
    return sat;
}
#endif

void emu_ir_reset(emu_ir_block_t *b)
{
    b->count = 0u;
    b->next_temp = 0u;
    b->guest_insns = 0u;
    b->overflow = false;
    /*
     * The last block's start is not this one's: left, a unit test that
     * set it made the next test's exit to that address a back edge, and
     * a block that retired nothing looped past the cap for ever. Every
     * frontend sets it after the reset.
     */
    b->start_pc = 0u;
    /* A frontend that says nothing gets the helpers, which always work. */
    b->has_fast = false;
    b->has_dtlb = false;
    b->has_pt = false;
    b->has_rom = false;
}

/* ------------------------------------------------------------------ */
/* The peripheral window                                               */
/* ------------------------------------------------------------------ */

static bool pt_member(const emu_region_t *r)
{
    return r->size != 0u && r->kind == (uint8_t)EMU_MEM_PASSTHRU &&
           r->host_base == (uintptr_t)r->base &&
           (r->perm & EMU_PERM_R) != 0u &&
           (r->widths & (EMU_W8 | EMU_W16 | EMU_W32)) ==
               (EMU_W8 | EMU_W16 | EMU_W32) &&
           (r->base & 3u) == 0u && (r->size & 3u) == 0u;
}

static bool pt_build(const emu_bus_t *bus, emu_ir_ptwin_t *out)
{
    bool found = false;

    memset(out, 0, sizeof(*out));
    for (uint32_t i = 0; i < bus->count; i++) {
        if (!pt_member(&bus->regions[i])) {
            continue;
        }

        /* Grow a run from this region upwards, through adjacent members. */
        emu_ir_ptwin_t run;
        uint32_t end = bus->regions[i].base;

        memset(&run, 0, sizeof(run));
        run.base = end;
        for (;;) {
            const emu_region_t *next = NULL;

            for (uint32_t j = 0; j < bus->count; j++) {
                if (pt_member(&bus->regions[j]) &&
                    bus->regions[j].base == end) {
                    next = &bus->regions[j];
                    break;
                }
            }
            if (next == NULL) {
                break;
            }
            if ((next->perm & EMU_PERM_W) == 0u) {
                if (run.nholes >= EMU_IR_PT_HOLES) {
                    break; /* end the run before a hole it cannot hold */
                }
                run.hole[run.nholes].off = next->base - run.base;
                run.hole[run.nholes].len = next->size;
                run.nholes++;
            }
            if (next->base + next->size < next->base) {
                break; /* would wrap the address space */
            }
            end = next->base + next->size;
            run.size = end - run.base;
        }
        if (run.size > out->size) {
            *out = run;
            found = true;
        }
    }
    return found;
}

/*
 * The cache: per core, because each core may have its own bus, and keyed
 * on the epoch, so a rebuilt table -- a reload -- is never answered from
 * the one before it.
 */
static EMU_PER_CORE const emu_bus_t *g_pt_bus;
static EMU_PER_CORE uint32_t g_pt_epoch;
static EMU_PER_CORE bool g_pt_ok;
static EMU_PER_CORE emu_ir_ptwin_t g_pt_win;

bool emu_ir_pt_window(const emu_bus_t *bus, emu_ir_ptwin_t *out)
{
    if (bus == NULL) {
        return false;
    }
    if (bus != g_pt_bus || bus->epoch != g_pt_epoch) {
        g_pt_ok = pt_build(bus, &g_pt_win);
        g_pt_bus = bus;
        g_pt_epoch = bus->epoch;
    }
    if (g_pt_ok) {
        *out = g_pt_win;
    }
    return g_pt_ok;
}

/* ------------------------------------------------------------------ */
/* The image                                                           */
/* ------------------------------------------------------------------ */

/* Per core and per epoch, for the reasons the peripheral window's is. */
static EMU_PER_CORE const emu_bus_t *g_rom_bus;
static EMU_PER_CORE uint32_t g_rom_epoch;
static EMU_PER_CORE uint32_t g_rom_base;
static EMU_PER_CORE bool g_rom_ok;
static EMU_PER_CORE emu_ir_fastmem_t g_rom_win;

static bool rom_build(const emu_bus_t *bus, uint32_t base,
                      emu_ir_fastmem_t *out)
{
    for (uint32_t i = 0; i < bus->count; i++) {
        const emu_region_t *const r = &bus->regions[i];

        if (r->kind != (uint8_t)EMU_MEM_ROM || r->host == NULL ||
            r->size == 0u || base - r->base >= r->size) {
            continue;
        }
        /*
         * Every read the helper would serve, and only those: a region
         * with a width rule or without read permission is the helper's,
         * because an inlined load tests neither.
         */
        if ((r->perm & EMU_PERM_R) == 0u || r->widths != EMU_WANY) {
            return false;
        }
        out->base = r->base;
        out->size = r->size;
        out->host = r->host;
        out->store_guard_offset = EMU_IR_NO_GUARD;
        return true;
    }
    return false;
}

bool emu_ir_rom_window(const emu_bus_t *bus, uint32_t base,
                       emu_ir_fastmem_t *out)
{
    if (bus == NULL) {
        return false;
    }
    if (bus != g_rom_bus || bus->epoch != g_rom_epoch || base != g_rom_base) {
        g_rom_ok = rom_build(bus, base, &g_rom_win);
        g_rom_bus = bus;
        g_rom_epoch = bus->epoch;
        g_rom_base = base;
    }
    if (g_rom_ok) {
        *out = g_rom_win;
    }
    return g_rom_ok;
}

/* For diagnostics: fuzz-ir's dumps, the loop census. */
const char *emu_ir_op_name(uint8_t op)
{
#define N(x) [EMU_IR_##x] = #x
    static const char *const k[EMU_IR_OP_COUNT] = {
        N(NOP),     N(GET),       N(PUT),       N(CONST),     N(MOV),
        N(ADD),     N(SUB),       N(AND),       N(OR),        N(XOR),
        N(SHL),     N(SHR),       N(SAR),       N(ROTL),      N(MUL),
        N(MULHS),   N(MULHU),     N(DIVS),      N(DIVU),      N(REMS),
        N(REMU),    N(MAC),       N(ADDI),      N(ANDI),      N(ORI),
        N(XORI),    N(SHLI),      N(SHRI),      N(SARI),      N(ROTLI),
        N(NEG),     N(NOT),       N(BSWAP32),   N(BSWAP16),   N(HSWAP),
        N(CLZ),     N(CTZ),       N(POPCNT),    N(CLMUL),     N(AESSB),     N(SM4SB),
        N(BEXT),    N(BSET),
        N(BCLR),    N(BINV),      N(BITOP_SET), N(BITOP_CLR), N(BITOP_INV),
        N(BITOP_TST), N(SEXT8),   N(SEXT16),    N(ZEXT8),     N(ZEXT16),
        N(LOAD),    N(STORE),     N(SETF),      N(GETCOND),   N(SETCC),
        N(SELECT),  N(FGET),      N(FPUT),      N(FADD),      N(FSUB),
        N(FMUL),    N(FDIV),      N(FSQRT),     N(FMA),       N(FMIN),
        N(FMAX),    N(FSGNJ),     N(FCMP),      N(FCVT_TO_I), N(FCVT_FROM_I),
        N(FCLASS),  N(HELPER),    N(HELPER_TRAP), N(RETIRE),  N(SETPC),
        N(EXIT),    N(EXIT_IF),   N(VOP),       N(VOPM),      N(VMEM),
        N(VRED),      N(VMEMS),     N(VFOP),      N(CMP3),      N(KERNEL)};
#undef N
    return (op < EMU_IR_OP_COUNT && k[op] != NULL) ? k[op] : "?";
}

uint16_t emu_ir_temp(emu_ir_block_t *b)
{
    if (b->next_temp >= EMU_IR_MAX_TEMPS) {
        b->overflow = true;
        return EMU_IR_NO_TEMP;
    }
    return (uint16_t)b->next_temp++;
}

/* Does this op produce a value? */
static bool op_writes(emu_ir_op_t op)
{
    switch (op) {
    case EMU_IR_PUT:
    case EMU_IR_FPUT:
    case EMU_IR_STORE:
    case EMU_IR_SETF:
    case EMU_IR_RETIRE:
    case EMU_IR_SETPC:
    case EMU_IR_EXIT:
    case EMU_IR_EXIT_IF:
    case EMU_IR_NOP:
    /* The memory bit ops define a flag and change memory, not a temp. */
    case EMU_IR_BITOP_SET:
    case EMU_IR_BITOP_CLR:
    case EMU_IR_BITOP_INV:
    case EMU_IR_BITOP_TST:
    /* The guest's vector registers are a place, not a temp. */
    case EMU_IR_VOP:
    case EMU_IR_VOPM:
    case EMU_IR_VMEM:
    case EMU_IR_VRED:
    case EMU_IR_VMEMS:
    case EMU_IR_VFOP:
        return false;
    default:
        return true;
    }
}

uint16_t emu_ir_emit(emu_ir_block_t *b, emu_ir_op_t op, uint8_t aux, uint16_t a,
                     uint16_t bb, uint32_t imm, uint8_t defs)
{
    if (b->count >= EMU_IR_MAX_INSNS) {
        b->overflow = true;
        return EMU_IR_NO_TEMP;
    }

    emu_ir_insn_t *const in = &b->insn[b->count++];
    in->op = (uint8_t)op;
    in->aux = aux;
    in->a = a;
    in->b = bb;
    in->c = EMU_IR_NO_TEMP;
    in->imm = imm;
    in->defs = defs;
    /*
     * Live until a pass proves otherwise. The conservative direction
     * matters: an uninitialised `live` that happened to be zero would
     * delete a flag definition something depends on, and the failure is
     * a wrong branch several instructions later.
     */
    in->live = defs;
    in->uses = 0u;
    in->dead = false;
    in->dst = op_writes(op) ? emu_ir_temp(b) : EMU_IR_NO_TEMP;
    return in->dst;
}

/*
 * The three-operand form. Everything emu_ir_emit does, plus the operand
 * that only EMU_IR_FMA has.
 */
uint16_t emu_ir_emit3(emu_ir_block_t *b, emu_ir_op_t op, uint8_t aux,
                      uint16_t a, uint16_t bb, uint16_t c)
{
    const uint16_t dst = emu_ir_emit(b, op, aux, a, bb, 0u, 0u);

    if (dst != EMU_IR_NO_TEMP && b->count > 0u) {
        b->insn[b->count - 1u].c = c;
    }
    return dst;
}

void emu_ir_vmem(emu_ir_block_t *b, uint8_t helper, uint16_t addr, uint16_t vl,
                 uint16_t arg, uint32_t imm)
{
    const uint32_t before = b->count;

    (void)emu_ir_emit(b, EMU_IR_VMEM, helper, addr, vl, imm, 0u);
    if (b->count > before) {
        b->insn[b->count - 1u].c = arg;
    }
}

void emu_ir_vmems(emu_ir_block_t *b, uint8_t helper, uint16_t addr,
                  uint16_t vl, uint16_t stride, uint32_t imm)
{
    const uint32_t before = b->count;

    (void)emu_ir_emit(b, EMU_IR_VMEMS, helper, addr, vl, imm, 0u);
    if (b->count > before) {
        b->insn[b->count - 1u].c = stride;
    }
}

uint16_t emu_ir_mem_if(emu_ir_block_t *b, emu_ir_op_t op, uint8_t aux,
                       uint16_t addr, uint16_t val, uint32_t disp,
                       uint16_t pred)
{
    const uint32_t before = b->count;
    const uint16_t dst = emu_ir_emit(b, op, aux, addr, val, disp, 0u);

    if (b->count > before) {
        b->insn[b->count - 1u].c = pred;
    }
    return dst;
}

uint16_t emu_ir_get(emu_ir_block_t *b, uint32_t guest_reg)
{
    return emu_ir_emit(b, EMU_IR_GET, 0u, EMU_IR_NO_TEMP, EMU_IR_NO_TEMP,
                       guest_reg, 0u);
}

void emu_ir_put(emu_ir_block_t *b, uint32_t guest_reg, uint16_t val)
{
    (void)emu_ir_emit(b, EMU_IR_PUT, 0u, val, EMU_IR_NO_TEMP, guest_reg, 0u);
}

uint16_t emu_ir_const(emu_ir_block_t *b, uint32_t v)
{
    return emu_ir_emit(b, EMU_IR_CONST, 0u, EMU_IR_NO_TEMP, EMU_IR_NO_TEMP, v,
                       0u);
}

uint16_t emu_ir_alu(emu_ir_block_t *b, emu_ir_op_t op, uint16_t a, uint16_t bb)
{
    return emu_ir_emit(b, op, 0u, a, bb, 0u, 0u);
}

void emu_ir_bitop(emu_ir_block_t *b, emu_ir_op_t op, uint16_t addr,
                  uint16_t bit, uint32_t disp, uint8_t width)
{
    (void)emu_ir_emit(b, op, width, addr, bit, disp, EMU_IR_F_Z);
}

/* ------------------------------------------------------------------ */
/* Pass: dead flag elimination                                         */
/* ------------------------------------------------------------------ */

/*
 * Which flags does this instruction *read*?
 *
 * Only the conditional forms do. Everything else is a pure value
 * computation -- which is exactly what the SETF split buys: without it,
 * every flag-setting arithmetic op would both define and, as far as this
 * walk could tell, potentially read.
 *
 * A helper call is the conservative case. It gets the cpu pointer and may
 * do anything, including reading the guest's flag word, so it reads
 * everything.
 */
static uint8_t op_reads_flags(const emu_ir_insn_t *in)
{
    switch ((emu_ir_op_t)in->op) {
    case EMU_IR_GETCOND:
    case EMU_IR_SELECT:
    case EMU_IR_EXIT_IF:
        /*
         * Conservatively all of them rather than only the ones the
         * condition names. Narrowing this is a real further win for
         * G4MH -- a BE reads only Z -- but it has to be a table keyed on
         * emu_ir_cond_t, and a wrong entry deletes a definition that is
         * read, so it wants its own change and its own test.
         */
        return EMU_IR_F_ALL;
    /*
     * A faulting access hands control to a trap handler, which can read
     * anything -- so a flag defined before one is live for the same
     * reason a guest register written before one is.
     */
    case EMU_IR_LOAD:
    case EMU_IR_STORE:
    case EMU_IR_BITOP_SET:
    case EMU_IR_BITOP_CLR:
    case EMU_IR_BITOP_INV:
    case EMU_IR_BITOP_TST:
    case EMU_IR_HELPER:
    case EMU_IR_HELPER_TRAP:
    case EMU_IR_VMEM:
    case EMU_IR_VMEMS:
        return EMU_IR_F_ALL;
    default:
        return 0u;
    }
}

/*
 * Walk backwards with a live set, exactly as a register allocator would
 * for values, and delete any SETF that defines nothing still wanted.
 *
 * This is the pass the whole IR was worth building for on a flag
 * machine. On x86-64 a full G4MH flag materialisation is `seto al` plus
 * `lahf` plus masking and a store -- and most of them are overwritten by
 * the next arithmetic instruction before any conditional looks.
 */
/*
 * The guest register that *is* the flag word, if one is -- ARMv7-M's,
 * which its translator writes with PUT where SETF has no mode for the
 * instruction (a carry in, an IT block) -- or IR_MAX_GUEST_REGS. A PUT of
 * it defines every flag and a GET reads them all; SETF, GETCOND and the
 * bit operations reach the same word through flags_offset, which the
 * passes below must see as the register. Found once per target.
 */
#define IR_MAX_GUEST_REGS 64u

static uint32_t flags_reg(const emu_ir_target_t *t)
{
    static EMU_PER_CORE const emu_ir_target_t *seen;
    static EMU_PER_CORE uint32_t reg;

    if (t == NULL || t->reg_offset == NULL ||
        (t->flag_bit[0] | t->flag_bit[1] | t->flag_bit[2] | t->flag_bit[3]) == 0u) {
        return IR_MAX_GUEST_REGS;
    }
    if (t != seen) {
        reg = IR_MAX_GUEST_REGS;
        for (uint32_t n = 0; n < IR_MAX_GUEST_REGS; n++) {
            if (t->reg_offset(n) == t->flags_offset) {
                reg = n;
                break;
            }
        }
        seen = t;
    }
    return reg;
}

/* An instruction that reads or rewrites the flag word through flags_offset. */
static bool op_uses_flag_word(uint8_t op)
{
    switch ((emu_ir_op_t)op) {
    case EMU_IR_SETF: /* a read-modify-write of the live flags */
    case EMU_IR_GETCOND:
    case EMU_IR_SELECT:
    case EMU_IR_BITOP_SET:
    case EMU_IR_BITOP_CLR:
    case EMU_IR_BITOP_INV:
    case EMU_IR_BITOP_TST:
        return true;
    default:
        return false;
    }
}

static void pass_dead_flags(emu_ir_block_t *b, const emu_ir_target_t *t, uint8_t live_out,
                            emu_ir_opt_stats_t *st)
{
    uint8_t live = live_out;
    const uint32_t fr = flags_reg(t);

    for (uint32_t i = b->count; i-- > 0;) {
        emu_ir_insn_t *const in = &b->insn[i];
        if (in->dead) {
            continue;
        }

        if (in->defs != 0u) {
            in->live = (uint8_t)(in->defs & live);
            if (in->op == (uint8_t)EMU_IR_SETF && in->live == 0u) {
                in->dead = true;
                st->flags_removed++;
                continue;
            }
            /* What it defines is no longer live above it. */
            live = (uint8_t)(live & ~in->defs);
        }
        if (fr != IR_MAX_GUEST_REGS && in->imm == fr) {
            if (in->op == (uint8_t)EMU_IR_PUT) {
                live = 0u; /* the whole word: every flag defined */
            } else if (in->op == (uint8_t)EMU_IR_GET) {
                live = EMU_IR_F_ALL;
            }
        }

        live |= op_reads_flags(in);
    }
}

/* ------------------------------------------------------------------ */
/* Pass: a condition read from the compare that set it                 */
/* ------------------------------------------------------------------ */

/*
 * The flags a condition reads, as EMU_IR_F_*: eval_cond's.
 */
static uint8_t cond_reads(uint8_t cond)
{
    switch ((emu_ir_cond_t)cond) {
    case EMU_IR_C_EQ:
    case EMU_IR_C_NE:
        return EMU_IR_F_Z;
    case EMU_IR_C_LT:
    case EMU_IR_C_GE:
        return EMU_IR_F_S | EMU_IR_F_V;
    case EMU_IR_C_LTU:
    case EMU_IR_C_GEU:
        return EMU_IR_F_C;
    case EMU_IR_C_LE:
    case EMU_IR_C_GT:
        return EMU_IR_F_Z | EMU_IR_F_S | EMU_IR_F_V;
    case EMU_IR_C_LEU:
    case EMU_IR_C_GTU:
        return EMU_IR_F_C | EMU_IR_F_Z;
    default:
        return 0u; /* MI, PL, VS, VC and ALWAYS: not a compare of a and b */
    }
}

static bool op_is_pure(uint8_t op);

/* Whether an instruction leaves the guest's flag word as it found it. */
static bool keeps_flags(const emu_ir_insn_t *in, const emu_ir_target_t *t)
{
    switch ((emu_ir_op_t)in->op) {
    case EMU_IR_LOAD:  /* a trap leaves the block: nothing after runs */
    case EMU_IR_STORE:
    case EMU_IR_EXIT_IF:
    case EMU_IR_RETIRE:
    case EMU_IR_SETPC:
        return true;
    case EMU_IR_PUT:
        return t == NULL || (t->reg_offset(in->imm) & ~3u) != (t->flags_offset & ~3u);
    default:
        return op_is_pure(in->op);
    }
}

/*
 * A GETCOND whose flags a subtract set: the compare of the subtract's
 * operands. G4MH's `cmp r6, 0` then `bz` was SETF -- the subtract
 * recomputed, the APSR moved into the PSW -- then GETCOND loading the PSW
 * back and extracting Z, then EXIT_IF on that against zero. The SETF
 * still runs (the flags are the guest's, live at the block's exits), but
 * the branch compares the registers it was always about.
 *
 * Only FS_SUB, where each compare condition is exactly the relation of
 * `a` and `b` (eval_cond: LT is S != V, LTU the borrow), and only where
 * the target maps every flag the condition reads -- an unmapped flag
 * reads 0, and LT is then S alone, not a < b -- and the SETF defines
 * them all. Anything between that may write the flag word ends it.
 */
static void pass_cond_forward(emu_ir_block_t *b, const emu_ir_target_t *t,
                              emu_ir_opt_stats_t *st)
{
    const emu_ir_insn_t *setf = NULL;

    if (t == NULL) {
        return;
    }
    for (uint32_t i = 0; i < b->count; i++) {
        emu_ir_insn_t *const in = &b->insn[i];

        if (in->dead) {
            continue;
        }
        if (in->op == (uint8_t)EMU_IR_SETF) {
            setf = (in->aux == (uint8_t)EMU_IR_FS_SUB || in->aux == (uint8_t)EMU_IR_FS_ZS ||
                    in->aux == (uint8_t)EMU_IR_FS_LOGIC || in->aux == (uint8_t)EMU_IR_FS_SHIFT)
                       ? in
                       : NULL;
            continue;
        }
        if (in->op == (uint8_t)EMU_IR_GETCOND && setf != NULL &&
            setf->aux != (uint8_t)EMU_IR_FS_SUB) {
            /*
             * A result's own flags (ARMv7-M's `tst; bne`, `lsls; bmi`): Z
             * is the result against zero and S its sign, whatever the
             * mode did to C and V. No second operand: zero.
             */
            const uint8_t c = in->aux;
            const bool zc = c == (uint8_t)EMU_IR_C_EQ || c == (uint8_t)EMU_IR_C_NE;
            const bool sc = c == (uint8_t)EMU_IR_C_MI || c == (uint8_t)EMU_IR_C_PL;
            const unsigned f = zc ? 0u : 1u; /* EMU_IR_F_Z or EMU_IR_F_S */

            if ((zc || sc) && (setf->defs & (1u << f)) != 0u && t->flag_bit[f] != 0u) {
                in->op = (uint8_t)EMU_IR_SETCC;
                in->aux = zc ? c
                          : (c == (uint8_t)EMU_IR_C_MI) ? (uint8_t)EMU_IR_C_LT
                                                        : (uint8_t)EMU_IR_C_GE;
                in->a = setf->a;
                in->b = EMU_IR_NO_TEMP;
                st->conds_forwarded++;
            }
            continue;
        }
        if (in->op == (uint8_t)EMU_IR_GETCOND && setf != NULL) {
            const uint8_t need = cond_reads(in->aux);
            bool mapped = need != 0u && (setf->defs & need) == need;

            for (unsigned f = 0; f < 4u && mapped; f++) {
                if ((need & (1u << f)) != 0u && t->flag_bit[f] == 0u) {
                    mapped = false;
                }
            }
            if (mapped) {
                in->op = (uint8_t)EMU_IR_SETCC;
                in->a = setf->a;
                in->b = setf->b;
                st->conds_forwarded++;
            }
            continue;
        }
        if (!keeps_flags(in, t)) {
            setf = NULL;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Pass: guest register traffic                                        */
/* ------------------------------------------------------------------ */

/*
 * Forward walk keeping, per guest register, the temp that currently holds
 * its value. A GET of a register whose value is already in a temp becomes
 * a MOV from that temp, which the value pass below then deletes outright
 * if nothing else changed.
 *
 * This is the measured round trip: with the register file in memory a
 * dependent pair emits `store slot` then `load slot`, and a quarter to a
 * third of adjacent pairs in this project's guests are dependent.
 *
 * The table is invalidated wholesale at anything that could write the
 * register file behind our back -- a helper call, or the end of the
 * block. Being wrong here is a stale value read as a guest register,
 * which is a wrong answer rather than a crash, so the invalidation is
 * deliberately blunt.
 *
 * A hardwired-zero register is not tracked at all, and that is the whole
 * reason this pass needs the target.
 *
 * Every lowering already discards a PUT to x0 and answers a GET of it
 * with a constant, so the *emitted* code is right whatever this table
 * says. The trap is that this pass runs first and rewrites the IR: a PUT
 * to x0 recorded `cur[0] = temp`, and the next GET of x0 then became a
 * MOV of that temp -- a real value, in place of the zero every lowering
 * would have produced. The reference interpreter runs the same optimised
 * IR, so it agreed with the compiled code exactly, and the differential
 * checker reported nothing while 36 of 39 architecture tests failed.
 * x0 is written constantly: every discarded result and every canonical
 * NOP is a PUT to it.
 *
 * A pass that rewrites the IR must therefore honour the same guest
 * invariants the lowerings do. Being right in all three backends does
 * not help when the thing that is wrong runs before them.
 */
/*
 * Whether a guest register reads as zero and discards writes. NULL target
 * means none does -- the passes stay usable by a unit test that builds a
 * block without one.
 */
static bool reg_is_zero(const emu_ir_target_t *t, uint32_t n)
{
    return t != NULL && t->reg_is_zero != NULL && t->reg_is_zero(n);
}

/*
 * The guest register that *is* the pc, if one is -- ARMv7-M's r15 --
 * or IR_MAX_GUEST_REGS. Found once per target, since the passes run per
 * translation and the answer is fixed.
 */
static uint32_t pc_reg(const emu_ir_target_t *t)
{
    static EMU_PER_CORE const emu_ir_target_t *seen;
    static EMU_PER_CORE uint32_t reg;

    if (t == NULL || t->reg_offset == NULL) {
        return IR_MAX_GUEST_REGS;
    }
    if (t != seen) {
        reg = IR_MAX_GUEST_REGS;
        for (uint32_t n = 0; n < IR_MAX_GUEST_REGS; n++) {
            if (t->reg_offset(n) == t->pc_offset) {
                reg = n;
                break;
            }
        }
        seen = t;
    }
    return reg;
}

static void pass_reg_traffic(emu_ir_block_t *b, const emu_ir_target_t *t,
                             emu_ir_opt_stats_t *st)
{
    uint16_t cur[IR_MAX_GUEST_REGS];
    const uint32_t pcr = pc_reg(t);
    const uint32_t fr = flags_reg(t);
    static EMU_PER_CORE uint16_t copy[EMU_IR_MAX_TEMPS];

    for (uint32_t i = 0; i < IR_MAX_GUEST_REGS; i++) {
        cur[i] = EMU_IR_NO_TEMP;
    }
    for (uint32_t i = 0; i < b->next_temp && i < EMU_IR_MAX_TEMPS; i++) {
        copy[i] = (uint16_t)i;
    }

    /*
     * Copy propagation is folded into this walk rather than run as its
     * own pass, because it is what makes the rewrite above pay. Turning
     * a redundant GET into a MOV saves the memory access but still emits
     * an instruction; resolving its readers straight to the original
     * temp leaves the MOV with no users at all, and the dead-value pass
     * then deletes it outright.
     *
     * Sound without any dominance reasoning because a temp is written
     * exactly once by construction and a block has one entry -- which is
     * the property that let this IR skip SSA.
     */
    for (uint32_t i = 0; i < b->count; i++) {
        emu_ir_insn_t *const in = &b->insn[i];
        if (in->dead) {
            continue;
        }

        if (in->a != EMU_IR_NO_TEMP && in->a < EMU_IR_MAX_TEMPS) {
            in->a = copy[in->a];
        }
        if (in->b != EMU_IR_NO_TEMP && in->b < EMU_IR_MAX_TEMPS) {
            in->b = copy[in->b];
        }
        if (in->c != EMU_IR_NO_TEMP && in->c < EMU_IR_MAX_TEMPS) {
            in->c = copy[in->c];
        }

        switch ((emu_ir_op_t)in->op) {
        case EMU_IR_GET:
            if (reg_is_zero(t, in->imm)) {
                /*
                 * A constant, so pass_fuse can see it: left a GET, the
                 * zero was materialised in a register and RV32's c.mv
                 * (`add rd, x0, rs2`) and every beqz/bnez computed with
                 * it -- `movw r8, #0` then `cmp r7, r8` in the F746's
                 * hottest CoreMark block.
                 */
                in->op = (uint8_t)EMU_IR_CONST;
                in->imm = 0u;
                st->zero_gets++;
                break;
            }
            if (in->imm < IR_MAX_GUEST_REGS) {
                if (cur[in->imm] != EMU_IR_NO_TEMP) {
                    /* Already in a temp: reuse it instead of reloading. */
                    in->op = (uint8_t)EMU_IR_MOV;
                    in->a = cur[in->imm];
                    st->gets_removed++;
                } else {
                    cur[in->imm] = in->dst;
                }
            }
            break;

        case EMU_IR_PUT:
            if (in->imm < IR_MAX_GUEST_REGS && !reg_is_zero(t, in->imm)) {
                cur[in->imm] = in->a;
            }
            break;

        case EMU_IR_HELPER:
        case EMU_IR_HELPER_TRAP:
        case EMU_IR_VMEM: /* its helper may */
        case EMU_IR_VMEMS:
        case EMU_IR_KERNEL:
            /* May touch anything. */
            for (uint32_t r = 0; r < IR_MAX_GUEST_REGS; r++) {
                cur[r] = EMU_IR_NO_TEMP;
            }
            break;

        /*
         * A write of the pc is a write of the register that is the pc.
         * Forwarding a read of r15 from before a SETPC to one after it
         * gave the old pc -- and then pass_dead_puts, seeing no read of
         * the pc left, deleted the SETPC as overwritten. No frontend
         * reads that register twice across a SETPC today (ARMv7-M reads
         * it only after a helper); fuzz-ir built the block that does.
         */
        case EMU_IR_SETPC:
            if (pcr < IR_MAX_GUEST_REGS) {
                cur[pcr] = EMU_IR_NO_TEMP;
            }
            break;

        default:
            /*
             * The same for the flag word: SETF and the bit operations
             * rewrite it through flags_offset, and a GET forwarded from
             * a PUT before one of them would read the flags before it.
             */
            if (fr < IR_MAX_GUEST_REGS && op_uses_flag_word(in->op)) {
                cur[fr] = EMU_IR_NO_TEMP;
            }
            break;
        }

        if (in->op == (uint8_t)EMU_IR_MOV && in->dst != EMU_IR_NO_TEMP &&
            in->dst < EMU_IR_MAX_TEMPS && in->a != EMU_IR_NO_TEMP) {
            copy[in->dst] = in->a;
        }
    }
}

/*
 * Delete a PUT that a later PUT to the same register overwrites with
 * nothing in between that could observe it.
 *
 * "Nothing that could observe it" is the whole difficulty. A helper call
 * reads the register file, and so does the end of the block -- the next
 * block, an interrupt handler and the debugger all see guest registers --
 * so only a PUT strictly dominated by another PUT with no helper and no
 * block exit between them can go.
 */
/*
 * The pc is one more register, and SETPC is the PUT that writes it --
 * so the same walk deletes a SETPC that a later one overwrites with
 * nothing in between that could observe it.
 *
 * It is worth having because frontends emit one per guest instruction:
 * the pc has to be right wherever a fault can be taken, and saying so
 * after every instruction is the version of that which cannot be got
 * wrong. Measured before this existed, SETPC was 19.3% of the code
 * emitted for RV32 CoreMark and 13% for PowerPC's, on a host where code
 * size is what decides whether the cache holds the working set.
 *
 * Three things differ from a register, and each is a way to get it
 * wrong:
 *
 * - **EXIT writes the pc itself**, so it is an overwrite and not only an
 *   observation: a SETPC before it with nothing between is dead.
 * - **EXIT_IF is neither.** Taken, it writes the pc; not taken, it falls
 *   through to whatever follows. Both ways an earlier SETPC is
 *   overwritten or it is not, exactly as it would be without the branch,
 *   so the state passes through unchanged. Treating it as an observer --
 *   which it is for *registers*, the line below -- would keep the SETPC
 *   ahead of every conditional branch for nothing.
 * - **A guest register can be the pc.** ARMv7-M's r15 is, and its
 *   translator ends a block with EXIT on `GET r15` after a helper has
 *   written it. So a GET of the register whose slot is pc_offset reads
 *   the pc and is an observer -- which is why this needs the target,
 *   and why it deletes nothing when handed none.
 */
static bool reg_is_pc(const emu_ir_target_t *t, uint32_t n)
{
    return t->reg_offset != NULL && t->reg_offset(n) == t->pc_offset;
}

/*
 * May the block be left here, or its state be read by something other
 * than the block? Then everything the guest has done so far has to be
 * true at this point: its registers, its pc and its retired count.
 *
 * One list, for every pass that moves or deletes a write of guest state
 * -- pass_dead_puts for the registers and the pc, pass_merge_retire for
 * the count. Two copies of it would drift, and the first to lose an
 * operation would make a trap see a guest that had not done what it had.
 *
 * The memory operations belong here and it is not obvious: they can
 * fault, and a fault leaves the block with everything before it
 * visible. See the riscv-tests ma_addr note in pass_dead_puts.
 */
static bool op_observes_state(uint8_t op)
{
    switch ((emu_ir_op_t)op) {
    case EMU_IR_LOAD:
    case EMU_IR_STORE:
    case EMU_IR_BITOP_SET:
    case EMU_IR_BITOP_CLR:
    case EMU_IR_BITOP_INV:
    case EMU_IR_BITOP_TST:
    case EMU_IR_HELPER:
    case EMU_IR_HELPER_TRAP:
    case EMU_IR_VMEM:
    case EMU_IR_VMEMS:
    case EMU_IR_KERNEL:
    case EMU_IR_EXIT:
    case EMU_IR_EXIT_IF:
        return true;
    default:
        return false;
    }
}

static void pass_dead_puts(emu_ir_block_t *b, const emu_ir_target_t *t,
                           emu_ir_opt_stats_t *st)
{
    bool seen[IR_MAX_GUEST_REGS];
    /*
     * True while every path from here writes the pc again before
     * anything reads it. False at the end of the block: the dispatcher
     * reads it there.
     */
    bool pc_rewritten = false;
    const uint32_t fr = flags_reg(t);

    memset(seen, 0, sizeof(seen));

    for (uint32_t i = b->count; i-- > 0;) {
        emu_ir_insn_t *const in = &b->insn[i];
        if (in->dead) {
            continue;
        }
        /*
         * A read of the flag word that is not a GET -- no forwarding
         * answers it from the PUT before -- reads that PUT from memory.
         */
        if (fr < IR_MAX_GUEST_REGS && op_uses_flag_word(in->op)) {
            seen[fr] = false;
        }

        switch ((emu_ir_op_t)in->op) {
        case EMU_IR_PUT:
            if (in->imm < IR_MAX_GUEST_REGS) {
                if (seen[in->imm]) {
                    in->dead = true;
                    st->puts_removed++;
                } else {
                    seen[in->imm] = true;
                }
            }
            break;

        case EMU_IR_SETPC:
            if (t == NULL) {
                break; /* cannot tell which GET reads the pc */
            }
            if (pc_rewritten) {
                in->dead = true;
                st->setpcs_removed++;
            } else {
                pc_rewritten = true;
            }
            break;

        case EMU_IR_GET:
            if (t != NULL && reg_is_pc(t, in->imm)) {
                pc_rewritten = false;
            }
            break;

        /*
         * Anything that can leave the block early is an observation
         * point: everything written before it is visible to whatever
         * runs next.
         *
         * The memory operations belong here and it is not obvious. A
         * store to a register followed by a *second* store to the same
         * register looks like a dead first store -- unless the
         * instruction between them can fault, because then the second
         * never runs and the first is the value the trap handler sees.
         *
         *     addi t1, s0, 1     ; t1 = the address about to fault
         *     lh   t1, 1(s0)     ; traps; never writes t1
         *
         * riscv-tests' ma_addr does exactly this and then has its
         * handler compare mtval against t1. Deleting the first store
         * left t1 stale, the comparison failed, and every other test in
         * the suite still passed.
         */
        default:
            if (!op_observes_state(in->op)) {
                break;
            }
            memset(seen, 0, sizeof(seen));
            if (in->op == (uint8_t)EMU_IR_EXIT) {
                pc_rewritten = true; /* it writes the pc; see above */
            } else if (in->op != (uint8_t)EMU_IR_EXIT_IF) {
                /* A fault records the pc, and a helper may read it. */
                pc_rewritten = false;
            } /* EXIT_IF: the pc passes through */
            break;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Pass: dead values                                                   */
/* ------------------------------------------------------------------ */

/*
 * Backward walk deleting any instruction whose result nothing reads.
 *
 * Runs last, because it is what turns the rewrites above into an actual
 * saving: a GET rewritten to a MOV is still an instruction until nothing
 * reads it, and the operands of a deleted SETF are frequently used by
 * nothing else.
 */
/*
 * Is this operation nothing but its result?
 *
 * **The list is of the pure ones, so an operation nobody has thought
 * about is kept.** It was the other way round -- a list of the
 * operations with an effect, everything else removable when unread --
 * and what fell off it was LOAD.
 *
 * A load can fault, and it reads: on a device, reading is an action --
 * a claim register acknowledges, a data register pops, a status
 * register clears -- and `(void)REG;` is how a driver asks for exactly
 * that. Judged by its result alone, a load whose destination was
 * overwritten later in the block was deleted, and the access never
 * happened. Under a JIT whose reason for existing is a guest driving
 * real peripherals.
 *
 * It took a narrow shape to reach: the overwrite has to come with no
 * load, store, helper or exit in between, because any of those keeps
 * the first write alive in pass_dead_puts. A compiler seldom leaves
 * that, which is why 378 architecture tests and every guest in the tree
 * ran translated without it showing. `pmpsm_cfg_A_tor_zero`, from a
 * suite nobody had named, does it on purpose: `lw a4, 0(zero)` and then
 * loads a4 with an address.
 *
 * Not pure, and therefore kept, without being named here:
 *
 *   LOAD, STORE                                memory, and faults
 *   the memory bit operations                  likewise -- three of the
 *                                              four define only a flag,
 *                                              and judged by that alone
 *                                              a SET1 whose Z nobody
 *                                              read would be deleted
 *   PUT, FPUT, SETF, SETPC, RETIRE             guest state
 *   HELPER, HELPER_TRAP, EXIT, EXIT_IF         anything at all
 *   FADD .. FMA, FMIN, FMAX, FCMP, the FCVTs   the FP flags, which are
 *                                              sometimes the only reason
 *                                              one is executed
 */
static bool op_is_pure(uint8_t op)
{
    switch ((emu_ir_op_t)op) {
    case EMU_IR_GET:
    case EMU_IR_CONST:
    case EMU_IR_MOV:
    case EMU_IR_ADD:
    case EMU_IR_SUB:
    case EMU_IR_AND:
    case EMU_IR_OR:
    case EMU_IR_XOR:
    case EMU_IR_SHL:
    case EMU_IR_SHR:
    case EMU_IR_SAR:
    case EMU_IR_ROTL:
    case EMU_IR_MUL:
    case EMU_IR_MULHS:
    case EMU_IR_MULHU:
    case EMU_IR_DIVS:
    case EMU_IR_DIVU:
    case EMU_IR_REMS:
    case EMU_IR_REMU:
    case EMU_IR_MAC:
    case EMU_IR_ADDI:
    case EMU_IR_ANDI:
    case EMU_IR_ORI:
    case EMU_IR_XORI:
    case EMU_IR_SHLI:
    case EMU_IR_SHRI:
    case EMU_IR_SARI:
    case EMU_IR_ROTLI:
    case EMU_IR_NEG:
    case EMU_IR_NOT:
    case EMU_IR_BSWAP32:
    case EMU_IR_BSWAP16:
    case EMU_IR_HSWAP:
    case EMU_IR_CLZ:
    case EMU_IR_CTZ:
    case EMU_IR_POPCNT:
    case EMU_IR_CLMUL:
    case EMU_IR_AESSB:
    case EMU_IR_SM4SB:
    case EMU_IR_BEXT:
    case EMU_IR_BSET:
    case EMU_IR_BCLR:
    case EMU_IR_BINV:
    case EMU_IR_SEXT8:
    case EMU_IR_SEXT16:
    case EMU_IR_ZEXT8:
    case EMU_IR_ZEXT16:
    case EMU_IR_GETCOND:
    case EMU_IR_SETCC:
    case EMU_IR_CMP3:
    case EMU_IR_SELECT:
    case EMU_IR_FGET:
    case EMU_IR_FSGNJ: /* moves bits; raises nothing */
    case EMU_IR_FCLASS: /* likewise */
        return true;
    default:
        return false;
    }
}

static void pass_dead_values(emu_ir_block_t *b, emu_ir_opt_stats_t *st)
{
    bool used[EMU_IR_MAX_TEMPS];

    memset(used, 0, sizeof(used));

    for (uint32_t i = b->count; i-- > 0;) {
        emu_ir_insn_t *const in = &b->insn[i];
        if (in->dead) {
            continue;
        }

        const bool has_effect = !op_is_pure(in->op);

        if (!has_effect && in->dst != EMU_IR_NO_TEMP &&
            in->dst < EMU_IR_MAX_TEMPS && !used[in->dst]) {
            in->dead = true;
            st->dead_removed++;
            continue;
        }

        if (in->a != EMU_IR_NO_TEMP && in->a < EMU_IR_MAX_TEMPS) {
            used[in->a] = true;
        }
        if (in->b != EMU_IR_NO_TEMP && in->b < EMU_IR_MAX_TEMPS) {
            used[in->b] = true;
        }
        if (in->c != EMU_IR_NO_TEMP && in->c < EMU_IR_MAX_TEMPS) {
            used[in->c] = true;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Pass: how many readers each value has                               */
/* ------------------------------------------------------------------ */

/*
 * Counted last, after every pass that deletes code, so a value is not
 * credited with readers that are themselves about to go.
 *
 * Sound without any dominance reasoning for the same reason the rest of
 * this file is: a temp is written exactly once by construction and a
 * block has one entry.
 */
/* ------------------------------------------------------------------ */
/* Fusion                                                             */
/* ------------------------------------------------------------------ */

/*
 * Three rewrites, all of which turn two IR instructions into one.
 *
 * **Here rather than in a backend**, and that is the point rather than a
 * convenience. A frontend decodes and emits IR; passes over that IR fuse
 * and eliminate; backends lower what survives. Written here each of these
 * serves x86-64 and Thumb-2 at once and matches on operations rather than
 * on encodings the IR has already abstracted away. The one fusion this
 * project built the other way -- ARM's shifted second operand, as a
 * backend peephole -- was correct on hardware, fired on *nothing*, and
 * cost 10%.
 *
 * What the histogram says to attack, in order:
 *
 *   const -> immediate   a materialised constant feeding an ALU
 *                        operation, which every frontend emits because
 *                        the IR's register forms are the general case
 *   addr fold            an ADDI feeding a LOAD or STORE, which already
 *                        carries a displacement of its own
 *   identity             arithmetic that computes its own input
 *
 * Every one is guarded on `uses == 1`. Folding a value that something
 * else still reads computes it twice, which is larger and slower than not
 * folding at all -- the rule emu_ir.h states for the backends' own
 * fusion, and it applies identically here.
 */

/* The immediate form of a register-register operation, or NOP if it has
 * none. NOP rather than a bool so the table is the answer. */
static uint8_t imm_form_of(uint8_t op)
{
    switch ((emu_ir_op_t)op) {
    case EMU_IR_ADD:
        return (uint8_t)EMU_IR_ADDI;
    case EMU_IR_AND:
        return (uint8_t)EMU_IR_ANDI;
    case EMU_IR_OR:
        return (uint8_t)EMU_IR_ORI;
    case EMU_IR_XOR:
        return (uint8_t)EMU_IR_XORI;
    case EMU_IR_SHL:
        return (uint8_t)EMU_IR_SHLI;
    case EMU_IR_SHR:
        return (uint8_t)EMU_IR_SHRI;
    case EMU_IR_SAR:
        return (uint8_t)EMU_IR_SARI;
    case EMU_IR_ROTL:
        return (uint8_t)EMU_IR_ROTLI;
    default:
        return (uint8_t)EMU_IR_NOP;
    }
}

/* Whether the constant may come from either operand. A shift's amount is
 * its right operand and nothing else; getting this wrong would compute
 * `k >> x` for `x >> k`. */
static bool op_commutes(uint8_t op)
{
    switch ((emu_ir_op_t)op) {
    case EMU_IR_ADD:
    case EMU_IR_AND:
    case EMU_IR_OR:
    case EMU_IR_XOR:
        return true;
    default:
        return false;
    }
}

/*
 * Whether the backend can lower this, when there is a backend.
 *
 * emu_ir_can_lower is defined by whichever host emitter is compiled in,
 * so an interpreter-only build has none and referencing it fails to
 * link. Without a backend nothing lowers this IR at all -- emu_ir_interp
 * implements every immediate form -- so the fold is unconditionally
 * safe there.
 */
static bool backend_can_lower(uint8_t op, uint8_t aux)
{
#if EMU_HAVE_JIT
    return emu_ir_can_lower((emu_ir_op_t)op, aux);
#else
    (void)op;
    (void)aux;
    return true;
#endif
}

/* A shift amount only folds when it is one the immediate form can mean.
 * Guests mask this -- RISC-V takes rs2 modulo 32 -- and an immediate of
 * 33 would be masked by one backend and not another. */
/* A compare condition's inverse, or 0xFF for one that is not a compare. */
static uint8_t cond_inverse(uint8_t c)
{
    switch ((emu_ir_cond_t)c) {
    case EMU_IR_C_EQ: return (uint8_t)EMU_IR_C_NE;
    case EMU_IR_C_NE: return (uint8_t)EMU_IR_C_EQ;
    case EMU_IR_C_LT: return (uint8_t)EMU_IR_C_GE;
    case EMU_IR_C_GE: return (uint8_t)EMU_IR_C_LT;
    case EMU_IR_C_LTU: return (uint8_t)EMU_IR_C_GEU;
    case EMU_IR_C_GEU: return (uint8_t)EMU_IR_C_LTU;
    case EMU_IR_C_LE: return (uint8_t)EMU_IR_C_GT;
    case EMU_IR_C_GT: return (uint8_t)EMU_IR_C_LE;
    case EMU_IR_C_LEU: return (uint8_t)EMU_IR_C_GTU;
    case EMU_IR_C_GTU: return (uint8_t)EMU_IR_C_LEU;
    default: return 0xFFu;
    }
}

static bool shift_imm_ok(uint8_t op, uint32_t imm)
{
    switch ((emu_ir_op_t)op) {
    case EMU_IR_SHLI:
    case EMU_IR_SHRI:
    case EMU_IR_SARI:
    case EMU_IR_ROTLI:
        return imm < 32u;
    default:
        return true;
    }
}


/*
 * Multiply-accumulate: dst = c +/- (a * b), ARM's MLA and MLS.
 *
 * **A separate pass, and it has to run after the dead-code sweeps.** The
 * frontend emits `PUT rd, MUL(...)` because RISC-V's mul really does
 * write rd, so at pass_fuse time every multiply has two readers -- the
 * PUT and the add -- and `uses == 1` is never true. Measured on the
 * board before this was moved: 277 add/subs whose second operand came
 * from a MUL, and **zero** with a single use. Only once pass_dead_puts
 * has removed the PUT of a register nothing reads again does the count
 * become the one fusion needs.
 *
 * `uses == 1` remains the soundness condition. Fusing a multiply that
 * something else still reads computes it twice, which is bigger and
 * slower than not fusing.
 */
static void pass_mac(emu_ir_block_t *b, emu_ir_opt_stats_t *st)
{
    static EMU_PER_CORE uint8_t uses[EMU_IR_MAX_TEMPS];
    static EMU_PER_CORE uint16_t def[EMU_IR_MAX_TEMPS];

    memset(uses, 0, sizeof(uses));
    for (uint32_t i = 0; i < EMU_IR_MAX_TEMPS; i++) {
        def[i] = (uint16_t)EMU_IR_NO_TEMP;
    }

    for (uint32_t i = 0; i < b->count; i++) {
        const emu_ir_insn_t *const in = &b->insn[i];

        if (in->dead) {
            continue;
        }
        if (in->a != EMU_IR_NO_TEMP && in->a < EMU_IR_MAX_TEMPS &&
            uses[in->a] != 255u) {
            uses[in->a]++;
        }
        if (in->b != EMU_IR_NO_TEMP && in->b < EMU_IR_MAX_TEMPS &&
            uses[in->b] != 255u) {
            uses[in->b]++;
        }
        if (in->c != EMU_IR_NO_TEMP && in->c < EMU_IR_MAX_TEMPS &&
            uses[in->c] != 255u) {
            uses[in->c]++;
        }
        if (in->dst != EMU_IR_NO_TEMP && in->dst < EMU_IR_MAX_TEMPS) {
            def[in->dst] = (uint16_t)i;
        }
    }

    for (uint32_t i = 0; i < b->count; i++) {
        emu_ir_insn_t *const in = &b->insn[i];

        if (in->dead) {
            continue;
        }
        /* --- multiply-accumulate ------------------------------------ */
        /*
         * dst = c +/- (a * b), where the multiply's *only* consumer is
         * this add or subtract. ARM's MLA and MLS; RISC-V has no such
         * instruction, so the pattern exists only as two.
         *
         * **`uses == 1` is the whole soundness condition.** Fusing a
         * multiply that something else also reads computes it twice,
         * which is bigger and slower than not fusing -- the rule the
         * `uses` field exists for, and which the reverted
         * shifted-operand fusion is recorded as needing.
         *
         * **Measured before it was written.** CoreMark retires 6,156
         * `mul` immediately followed by a dependent `add` in a
         * 400,000-instruction sample, out of 9,470 multiplies. The pair
         * histogram's top-N list did not show it -- a direct trace did,
         * which is worth remembering the next time a pattern looks
         * absent.
         *
         * The subtracting form is asymmetric and easy to get backwards:
         * MLS is `c - a*b`, so only a SUB whose *second* operand is the
         * product qualifies. `a*b - c` is not an MLS and is left alone,
         * which is also why the commuted case below is ADD-only.
         */
        if (in->op == (uint8_t)EMU_IR_ADD ||
            in->op == (uint8_t)EMU_IR_SUB) {
            const bool is_sub = in->op == (uint8_t)EMU_IR_SUB;
            uint16_t prod = (uint16_t)EMU_IR_NO_TEMP;
            uint16_t addend = (uint16_t)EMU_IR_NO_TEMP;

            if (in->b != EMU_IR_NO_TEMP && in->b < EMU_IR_MAX_TEMPS &&
                uses[in->b] == 1u &&
                def[in->b] != (uint16_t)EMU_IR_NO_TEMP &&
                b->insn[def[in->b]].op == (uint8_t)EMU_IR_MUL &&
                !b->insn[def[in->b]].dead) {
                prod = in->b;
                addend = in->a;
            } else if (!is_sub && in->a != EMU_IR_NO_TEMP &&
                       in->a < EMU_IR_MAX_TEMPS && uses[in->a] == 1u &&
                       def[in->a] != (uint16_t)EMU_IR_NO_TEMP &&
                       b->insn[def[in->a]].op == (uint8_t)EMU_IR_MUL &&
                       !b->insn[def[in->a]].dead) {
                prod = in->a;
                addend = in->b;
            }

            if (prod != (uint16_t)EMU_IR_NO_TEMP &&
                addend != (uint16_t)EMU_IR_NO_TEMP &&
                backend_can_lower((uint8_t)EMU_IR_MAC,
                                  is_sub ? EMU_IR_MAC_SUB : 0u)) {
                emu_ir_insn_t *const mul = &b->insn[def[prod]];

                in->op = (uint8_t)EMU_IR_MAC;
                in->aux = is_sub ? (uint8_t)EMU_IR_MAC_SUB : 0u;
                in->a = mul->a;
                in->b = mul->b;
                in->c = addend;
                mul->dead = true;
                st->macs++;
            }
        }
    }
}

static void pass_fuse(emu_ir_block_t *b, emu_ir_opt_stats_t *st)
{
    static EMU_PER_CORE uint8_t uses[EMU_IR_MAX_TEMPS];
    static EMU_PER_CORE uint16_t def[EMU_IR_MAX_TEMPS];

    memset(uses, 0, sizeof(uses));
    for (uint32_t i = 0; i < EMU_IR_MAX_TEMPS; i++) {
        def[i] = (uint16_t)EMU_IR_NO_TEMP;
    }

    /*
     * Use counts and definition sites, over the *live* instructions. A
     * temp is written once by construction, so `def` is unambiguous --
     * the same property the register allocator relies on.
     */
    for (uint32_t i = 0; i < b->count; i++) {
        const emu_ir_insn_t *const in = &b->insn[i];

        if (in->dead) {
            continue;
        }
        if (in->a != EMU_IR_NO_TEMP && in->a < EMU_IR_MAX_TEMPS &&
            uses[in->a] != 255u) {
            uses[in->a]++;
        }
        if (in->b != EMU_IR_NO_TEMP && in->b < EMU_IR_MAX_TEMPS &&
            uses[in->b] != 255u) {
            uses[in->b]++;
        }
        if (in->c != EMU_IR_NO_TEMP && in->c < EMU_IR_MAX_TEMPS &&
            uses[in->c] != 255u) {
            uses[in->c]++;
        }
        if (in->dst != EMU_IR_NO_TEMP && in->dst < EMU_IR_MAX_TEMPS) {
            def[in->dst] = (uint16_t)i;
        }
    }

    for (uint32_t i = 0; i < b->count; i++) {
        emu_ir_insn_t *const in = &b->insn[i];

        if (in->dead) {
            continue;
        }

        /* --- a constant operand becomes an immediate ---------------- */
        const uint8_t iform = imm_form_of(in->op);

        if (iform != (uint8_t)EMU_IR_NOP) {
            uint16_t src = (uint16_t)EMU_IR_NO_TEMP;
            bool from_a = false;

            if (in->b != EMU_IR_NO_TEMP && in->b < EMU_IR_MAX_TEMPS &&
                uses[in->b] == 1u && def[in->b] != (uint16_t)EMU_IR_NO_TEMP &&
                b->insn[def[in->b]].op == (uint8_t)EMU_IR_CONST) {
                src = in->b;
            } else if (op_commutes(in->op) && in->a != EMU_IR_NO_TEMP &&
                       in->a < EMU_IR_MAX_TEMPS && uses[in->a] == 1u &&
                       def[in->a] != (uint16_t)EMU_IR_NO_TEMP &&
                       b->insn[def[in->a]].op == (uint8_t)EMU_IR_CONST) {
                src = in->a;
                from_a = true;
            }

            if (src != (uint16_t)EMU_IR_NO_TEMP) {
                const uint32_t k = b->insn[def[src]].imm;

                /*
                 * **Ask before rewriting.** An immediate form only helps
                 * if the backend has one, and turning a lowerable
                 * register operation into an unlowerable immediate one
                 * loses the whole block to the interpreter.
                 *
                 * That is not hypothetical: the first version of this
                 * pass had no guard, and on CoreMark it took the x86-64
                 * backend from 350 blocks and 23,133 interpreted
                 * instructions to 208 blocks and **423,764** -- 82% of
                 * the run, because neither backend lowers ADDI or ANDI.
                 * Every test still passed, because declining is
                 * correct: it is a correctness-preserving way to have no
                 * JIT, and no test of correctness can see it.
                 *
                 * emu_ir_can_lower is exactly this question and already
                 * existed for the frontends to ask before emitting.
                 */
                if (shift_imm_ok(iform, k) &&
                    backend_can_lower(iform, in->aux)) {
                    if (from_a) {
                        in->a = in->b; /* the surviving operand */
                    }
                    in->op = iform;
                    in->b = (uint16_t)EMU_IR_NO_TEMP;
                    in->imm = k;
                    b->insn[def[src]].dead = true;
                    uses[src] = 0u;
                    st->folded++;
                }
            }
        }

        /* --- an ADDI feeding an access becomes its displacement ----- */
        if ((in->op == (uint8_t)EMU_IR_LOAD ||
             in->op == (uint8_t)EMU_IR_STORE) &&
            in->a != EMU_IR_NO_TEMP && in->a < EMU_IR_MAX_TEMPS &&
            uses[in->a] == 1u && def[in->a] != (uint16_t)EMU_IR_NO_TEMP) {
            emu_ir_insn_t *const src = &b->insn[def[in->a]];

            if (!src->dead && src->op == (uint8_t)EMU_IR_ADDI &&
                src->a != EMU_IR_NO_TEMP) {
                /*
                 * Wrapping is the guest's own address arithmetic, so the
                 * sum needs no range check: a displacement that overflows
                 * 32 bits wraps in exactly the same place either way.
                 */
                in->imm += src->imm;
                in->a = src->a;
                src->dead = true;
                uses[in->a] = (uses[in->a] == 255u) ? 255u : uses[in->a];
                st->addr_folded++;
            }
        }

        /* --- a branch on a compare's 0/1 is a branch on the compare -- */
        if (in->op == (uint8_t)EMU_IR_EXIT_IF &&
            (in->aux == (uint8_t)EMU_IR_C_NE || in->aux == (uint8_t)EMU_IR_C_EQ) &&
            in->a != EMU_IR_NO_TEMP && in->a < EMU_IR_MAX_TEMPS && uses[in->a] == 1u &&
            def[in->a] != (uint16_t)EMU_IR_NO_TEMP && in->b != EMU_IR_NO_TEMP &&
            in->b < EMU_IR_MAX_TEMPS && def[in->b] != (uint16_t)EMU_IR_NO_TEMP &&
            b->insn[def[in->b]].op == (uint8_t)EMU_IR_CONST &&
            b->insn[def[in->b]].imm == 0u) {
            emu_ir_insn_t *const cc = &b->insn[def[in->a]];
            const uint8_t inv = cond_inverse(cc->aux);

            if (!cc->dead && cc->op == (uint8_t)EMU_IR_SETCC && inv != 0xFFu) {
                /*
                 * `x != 0` where x = (a c b) is (a c b); `x == 0` its
                 * inverse. The SETCC goes, and the zero with it once
                 * nothing else reads it: RV32's `slt; bnez`, and every
                 * condition a frontend turns into a 0/1 first.
                 */
                if (uses[in->b] != 255u && uses[in->b] != 0u) {
                    uses[in->b]--;
                }
                in->aux = (in->aux == (uint8_t)EMU_IR_C_NE) ? cc->aux : inv;
                in->a = cc->a;
                in->b = cc->b;
                cc->dead = true;
                st->branch_folded++;
            }
        }

        /* --- arithmetic that computes its own input ----------------- */
        switch ((emu_ir_op_t)in->op) {
        case EMU_IR_ADDI:
        case EMU_IR_ORI:
        case EMU_IR_XORI:
        case EMU_IR_SHLI:
        case EMU_IR_SHRI:
        case EMU_IR_SARI:
        case EMU_IR_ROTLI:
            if (in->imm == 0u && in->a != EMU_IR_NO_TEMP) {
                in->op = (uint8_t)EMU_IR_MOV;
                st->identities++;
            }
            break;
        case EMU_IR_ANDI:
            if (in->imm == 0xFFFFFFFFu && in->a != EMU_IR_NO_TEMP) {
                in->op = (uint8_t)EMU_IR_MOV;
                st->identities++;
            } else if (in->imm == 0u) {
                in->op = (uint8_t)EMU_IR_CONST;
                in->a = (uint16_t)EMU_IR_NO_TEMP;
                st->identities++;
            }
            break;
        default:
            break;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Pass: merge retirements                                             */
/* ------------------------------------------------------------------ */

/*
 * One RETIRE per stretch between observation points, carrying the count
 * of the ones it absorbed.
 *
 * Every guest instruction ends in a RETIRE, and each is an add on the
 * host -- 11% of everything the Thumb-2 backend emitted for CoreMark once
 * the memory accesses were shrunk. The count is read only where the
 * block can be left -- a trap out of a load or a helper, an exit, the
 * loop cap a back edge tests -- so between two of those the adds can be
 * one, provided it sits *before* the second. Keeping the last RETIRE of
 * each stretch does that: it is after every instruction it counts and
 * before anything that can read the total, which is therefore exactly
 * what it was at every point it could be read.
 *
 * `imm` is the count, 0 meaning 1, so a frontend still emits one per
 * instruction and knows nothing of this.
 */
static void pass_merge_retire(emu_ir_block_t *b, emu_ir_opt_stats_t *st)
{
    emu_ir_insn_t *carry = NULL;

    for (uint32_t i = 0; i < b->count; i++) {
        emu_ir_insn_t *const in = &b->insn[i];

        if (in->dead) {
            continue;
        }
        if (in->op == (uint8_t)EMU_IR_RETIRE) {
            const uint32_t n = (in->imm != 0u) ? in->imm : 1u;

            if (carry != NULL) {
                in->imm = n + carry->imm;
                carry->dead = true;
                st->retires_merged++;
            } else {
                in->imm = n;
            }
            carry = in;
        } else if (op_observes_state(in->op)) {
            carry = NULL;
        }
    }
}

/*
 * A block that stops without a jump -- at its length cap, before an
 * instruction its translator declines, out of room -- ends in the pc
 * write after its last instruction, and falls off its end to the
 * dispatcher. That pc is a constant, so the write becomes an exit to it,
 * which a backend can chain: PowerPC Whetstone's five 64-instruction
 * blocks in sqrtf and logf went through the dispatcher 46,500 times, a
 * quarter of its block entries, with every one of their successors
 * resident.
 *
 * Not after an exit that always leaves: the write there is never
 * reached, and as an exit it would take a chain slot for nothing.
 */
static void pass_tail_exit(emu_ir_block_t *b, emu_ir_opt_stats_t *st)
{
    emu_ir_insn_t *last = NULL;

    for (uint32_t i = 0; i < b->count; i++) {
        emu_ir_insn_t *const in = &b->insn[i];

        if (in->dead) {
            continue;
        }
        if (in->op == (uint8_t)EMU_IR_EXIT) {
            return;
        }
        last = in;
    }
    if (last != NULL && last->op == (uint8_t)EMU_IR_SETPC &&
        last->a == EMU_IR_NO_TEMP) {
        last->op = (uint8_t)EMU_IR_EXIT;
        st->tail_exits++;
    }
}

static void pass_count_uses(emu_ir_block_t *b, emu_ir_opt_stats_t *st)
{
    static EMU_PER_CORE uint8_t uses[EMU_IR_MAX_TEMPS];

    memset(uses, 0, sizeof(uses));

    for (uint32_t i = 0; i < b->count; i++) {
        const emu_ir_insn_t *const in = &b->insn[i];

        if (in->dead) {
            continue;
        }
        if (in->a != EMU_IR_NO_TEMP && in->a < EMU_IR_MAX_TEMPS &&
            uses[in->a] != 255u) {
            uses[in->a]++;
        }
        if (in->b != EMU_IR_NO_TEMP && in->b < EMU_IR_MAX_TEMPS &&
            uses[in->b] != 255u) {
            uses[in->b]++;
        }
        if (in->c != EMU_IR_NO_TEMP && in->c < EMU_IR_MAX_TEMPS &&
            uses[in->c] != 255u) {
            uses[in->c]++;
        }
    }

    for (uint32_t i = 0; i < b->count; i++) {
        emu_ir_insn_t *const in = &b->insn[i];

        in->uses = (!in->dead && in->dst != EMU_IR_NO_TEMP &&
                    in->dst < EMU_IR_MAX_TEMPS)
                       ? uses[in->dst]
                       : 0u;
        if (in->uses == 1u) {
            st->single_use++;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Register allocation                                                 */
/* ------------------------------------------------------------------ */

/*
 * Live intervals, indexed by temp. Static rather than automatic because
 * on the microcontroller these are 1 KB and the translator runs on the
 * guest's stack; the pass is not reentrant, which is true of every pass
 * here and of the block buffer they all walk.
 */
static EMU_PER_CORE uint16_t g_def[EMU_IR_MAX_TEMPS];
static EMU_PER_CORE uint16_t g_last[EMU_IR_MAX_TEMPS];

#define IR_NO_POS 0xFFFFu

uint32_t emu_ir_regalloc(const emu_ir_block_t *b, uint32_t nregs,
                         uint8_t *assign)
{
    uint16_t inreg[EMU_IR_MAX_HOST_REGS];
    const uint32_t nt =
        (b->next_temp < EMU_IR_MAX_TEMPS) ? b->next_temp : EMU_IR_MAX_TEMPS;
    uint32_t used = 0u;

    for (uint32_t i = 0; i < nt; i++) {
        assign[i] = EMU_IR_NO_REG;
        g_def[i] = IR_NO_POS;
        g_last[i] = 0u;
    }
    if (nregs == 0u || nregs > EMU_IR_MAX_HOST_REGS || b->overflow) {
        return 0u;
    }
    for (uint32_t r = 0; r < nregs; r++) {
        inreg[r] = EMU_IR_NO_TEMP;
    }

    /*
     * One walk for both ends of every interval. `a`, `b` and `c` are
     * EMU_IR_NO_TEMP when absent, which is 0xFFFF and so above `nt` --
     * the bound doubles as the absent test.
     *
     * **`c` has to be here or the allocator reuses a live register.**
     * It is read by the fused multiply-adds alone, so a version that
     * forgot it would be correct on every block without an FMA and wrong
     * on the ones that have one -- the shape of bug this file's history
     * is full of.
     */
    for (uint32_t i = 0; i < b->count; i++) {
        const emu_ir_insn_t *const in = &b->insn[i];

        if (in->dead) {
            continue;
        }
        if ((uint32_t)in->a < nt) {
            g_last[in->a] = (uint16_t)i;
        }
        if ((uint32_t)in->b < nt) {
            g_last[in->b] = (uint16_t)i;
        }
        if ((uint32_t)in->c < nt) {
            g_last[in->c] = (uint16_t)i;
        }
        if ((uint32_t)in->dst < nt) {
            g_def[in->dst] = (uint16_t)i;
        }
    }

    /*
     * Temps are numbered in emission order, so walking them in order is
     * walking the intervals by start point -- which is what linear scan
     * needs and what would otherwise be a sort.
     */
    for (uint32_t tmp = 0; tmp < nt; tmp++) {
        const uint32_t d = g_def[tmp];

        if (d == IR_NO_POS || (uint32_t)g_last[tmp] <= d) {
            continue; /* never defined, or never read */
        }

        /*
         * Leave the one-instruction values to the reload elision. It
         * carries them in the scratch register for no instruction at
         * all, where a register here would cost one of very few.
         */
        if (b->insn[d].uses == 1u) {
            uint32_t j = d + 1u;

            while (j < b->count && b->insn[j].dead) {
                j++;
            }
            if (j == (uint32_t)g_last[tmp]) {
                continue;
            }
        }

        for (uint32_t r = 0; r < nregs; r++) {
            if (inreg[r] != EMU_IR_NO_TEMP && (uint32_t)g_last[inreg[r]] < d) {
                inreg[r] = EMU_IR_NO_TEMP;
            }
        }

        uint32_t pick = nregs;
        for (uint32_t r = 0; r < nregs; r++) {
            if (inreg[r] == EMU_IR_NO_TEMP) {
                pick = r;
                break;
            }
        }

        if (pick == nregs) {
            /*
             * All busy. Evict whichever interval runs furthest past this
             * one -- and if that is this one, it simply does not get a
             * register. Eviction is total: the temp goes back to its
             * frame slot for its whole life, which is sound only because
             * nothing has been emitted yet.
             */
            uint32_t worst = 0u;

            for (uint32_t r = 1u; r < nregs; r++) {
                if (g_last[inreg[r]] > g_last[inreg[worst]]) {
                    worst = r;
                }
            }
            if (g_last[inreg[worst]] <= g_last[tmp]) {
                continue;
            }
            assign[inreg[worst]] = EMU_IR_NO_REG;
            pick = worst;
        }

        inreg[pick] = (uint16_t)tmp;
        assign[tmp] = (uint8_t)pick;
        if (pick + 1u > used) {
            used = pick + 1u;
        }
    }

    return used;
}

/* ------------------------------------------------------------------ */

/* One total for every core: it changes once per translation, never per
 * dispatch, so a lock costs nothing that matters (emu_lock.h). */
static emu_ir_opt_stats_t g_opt_totals;
static emu_lock_t g_opt_lock = EMU_LOCK_INIT;

void emu_ir_opt_totals(emu_ir_opt_stats_t *out)
{
    emu_lock(&g_opt_lock);
    *out = g_opt_totals;
    emu_unlock(&g_opt_lock);
}

static uint32_t g_unkept;
static uint8_t g_unkept_op;
static uint8_t g_unkept_aux;

void emu_ir_note_refused(uint8_t op, uint8_t aux)
{
    if (op >= (uint8_t)EMU_IR_OP_COUNT ||
        !emu_ir_can_lower((emu_ir_op_t)op, aux)) {
        return; /* refused as promised */
    }
    emu_lock(&g_opt_lock);
    if (g_unkept == 0u) {
        g_unkept_op = op;
        g_unkept_aux = aux;
    }
    g_unkept++;
    emu_unlock(&g_opt_lock);
}

uint32_t emu_ir_unkept(uint8_t *op, uint8_t *aux)
{
    emu_lock(&g_opt_lock);
    const uint32_t n = g_unkept;

    *op = g_unkept_op;
    *aux = g_unkept_aux;
    emu_unlock(&g_opt_lock);
    return n;
}

/* ------------------------------------------------------------------ */
/* Analysis: what a loop is (docs/jit/autovectorisation.md, P1)        */
/* ------------------------------------------------------------------ */

/*
 * A temp's value as the loop sees it. Each block iteration starts with
 * the guest registers as the last one left them: a register the body
 * writes after reading is carried, and its read is "r at the start of
 * the iteration". Every other temp is built from those, constants and
 * registers the body never writes (invariant), and loads (a value of
 * this iteration).
 */
enum {
    LK_INV = 1, /* invariant: constants and registers the body does not write */
    LK_AFF,     /* carried register r plus the constant `off` */
    LK_AFFX,    /* carried register r plus an invariant, not a constant */
    LK_VAR,     /* this iteration's, from loads; no carried register in it */
    LK_DEP,     /* depends on a carried register other than affinely */
};

typedef struct {
    uint8_t kind;
    uint8_t reg;
    uint16_t def; /* the instruction defining the temp; in what was padding */
    uint32_t off; /* wrapping, as the guest's addresses do (fuzz-ir: INT_MIN) */
} lk_t;

/* 2 KB of a board's RAM at 256 temps: ARMv7-M's kernel loops have 140. */
static EMU_PER_CORE lk_t g_lk[EMU_IR_MAX_TEMPS];

static lk_t lk_of(uint16_t t)
{
    const lk_t inv = {.kind = LK_INV};

    return (t < EMU_IR_MAX_TEMPS && g_lk[t].kind != 0u) ? g_lk[t] : inv;
}

static bool lk_const(const emu_ir_block_t *b, uint16_t t, uint32_t *v)
{
    if (t >= EMU_IR_MAX_TEMPS || g_lk[t].def == EMU_IR_NO_TEMP ||
        b->insn[g_lk[t].def].op != (uint8_t)EMU_IR_CONST) {
        return false;
    }
    *v = b->insn[g_lk[t].def].imm;
    return true;
}

/* The kind of a value built from two by anything but an affine step. */
static lk_t lk_mix(lk_t x, lk_t y)
{
    lk_t r = {.kind = LK_INV};

    if (x.kind >= LK_AFF && x.kind != LK_VAR) {
        r.kind = LK_DEP;
        r.reg = x.reg;
    } else if (y.kind >= LK_AFF && y.kind != LK_VAR) {
        r.kind = LK_DEP;
        r.reg = y.reg;
    } else if (x.kind == LK_VAR || y.kind == LK_VAR) {
        r.kind = LK_VAR;
    }
    return r;
}

static const emu_ir_insn_t *lk_def_insn(const emu_ir_block_t *b, uint16_t t)
{
    return (t < EMU_IR_MAX_TEMPS && g_lk[t].def != EMU_IR_NO_TEMP) ? &b->insn[g_lk[t].def]
                                                                    : NULL;
}

/*
 * Whether t is a 16-bit element read as signed: a signed halfword load
 * (RV32's lh), a halfword sign-extended (ARMv7-M's SMLABB takes the
 * bottom half of what `ldrh` loaded), or one byte-swapped first
 * (PowerPC's lha, big-endian).
 */
static bool lk_elem16(const emu_ir_block_t *b, uint16_t t)
{
    const emu_ir_insn_t *in = lk_def_insn(b, t);

    if (in != NULL && in->op == (uint8_t)EMU_IR_SEXT16) {
        in = lk_def_insn(b, in->a);
        if (in != NULL && in->op == (uint8_t)EMU_IR_BSWAP16) {
            in = lk_def_insn(b, in->a);
        }
        return in != NULL && in->op == (uint8_t)EMU_IR_LOAD && EMU_IR_MEM_SIZE(in->aux) == 2u;
    }
    return in != NULL && in->op == (uint8_t)EMU_IR_LOAD && EMU_IR_MEM_SIZE(in->aux) == 2u &&
           (in->aux & EMU_IR_MEM_SIGNED) != 0u;
}

/* The LOAD behind a 16-bit element (lk_elem16), and whether it is swapped. */
static const emu_ir_insn_t *lk_elem_load(const emu_ir_block_t *b, uint16_t t, bool *swap)
{
    const emu_ir_insn_t *in = lk_def_insn(b, t);

    *swap = false;
    if (in != NULL && in->op == (uint8_t)EMU_IR_SEXT16) {
        in = lk_def_insn(b, in->a);
        if (in != NULL && in->op == (uint8_t)EMU_IR_BSWAP16) {
            *swap = true;
            in = lk_def_insn(b, in->a);
        }
    }
    return in;
}

/* Whether temp t's value is built only from one MUL of two 16-bit signed
 * elements, through pure single-input operations and constants. */
static bool lk_from_product(const emu_ir_block_t *b, uint16_t t, uint32_t depth,
                            bool *mapped)
{
    if (t >= EMU_IR_MAX_TEMPS || g_lk[t].def == EMU_IR_NO_TEMP || depth > 12u) {
        return false;
    }
    const emu_ir_insn_t *const in = &b->insn[g_lk[t].def];

    if (in->op == (uint8_t)EMU_IR_MUL) {
        if (lk_elem16(b, in->a) && lk_elem16(b, in->b)) {
            return true;
        }
        /* a product of two mapped products: bitextract's (p >> 5 & 127) * (p >> 2 & 15);
         * the two loads the caller counts make them the same product */
        *mapped = true;
        return lk_from_product(b, in->a, depth + 1u, mapped) &&
               lk_from_product(b, in->b, depth + 1u, mapped);
    }
    switch ((emu_ir_op_t)in->op) {
    /* one input and constants: PowerPC's rlwinm is ROTLI then ANDI */
    case EMU_IR_ADDI:
    case EMU_IR_ANDI:
    case EMU_IR_ORI:
    case EMU_IR_XORI:
    case EMU_IR_SHLI:
    case EMU_IR_SHRI:
    case EMU_IR_SARI:
    case EMU_IR_ROTLI:
    case EMU_IR_SEXT8:
    case EMU_IR_SEXT16:
    case EMU_IR_ZEXT8:
    case EMU_IR_ZEXT16:
    case EMU_IR_NOT:
    case EMU_IR_NEG:
    case EMU_IR_MOV:
        *mapped = *mapped || in->op != (uint8_t)EMU_IR_MOV;
        return lk_from_product(b, in->a, depth + 1u, mapped);
    default:
        break;
    }
    return false;
}

/* Whether t is the constant 0: strcpy's back edge compares its byte with it. */
static bool lk_is_zero(const emu_ir_block_t *b, uint16_t t)
{
    uint32_t v;

    return lk_const(b, t, &v) && v == 0u;
}

/*
 * An induction's step: a constant, or another register shifted -- RV32's
 * `sh1add a2, a0, a2`, PowerPC's `add r10, r10, r5`. False for any other.
 */
static bool lk_step(const emu_ir_block_t *b, uint16_t get_r, uint16_t put_r, int32_t *k,
                    uint8_t *sreg, uint8_t *sshift)
{
    const lk_t kv = lk_of(put_r);
    const emu_ir_insn_t *const d = lk_def_insn(b, put_r);

    *sreg = 0xFFu;
    *sshift = 0u;
    if (kv.kind == LK_AFF) {
        *k = (int32_t)kv.off;
        return true;
    }
    if (d == NULL || d->op != (uint8_t)EMU_IR_ADD || (d->a != get_r && d->b != get_r)) {
        return false;
    }
    const emu_ir_insn_t *w = lk_def_insn(b, (d->a == get_r) ? d->b : d->a);

    if (w != NULL && w->op == (uint8_t)EMU_IR_SHLI && w->imm < 4u) {
        *sshift = (uint8_t)w->imm;
        w = lk_def_insn(b, w->a);
    }
    if (w == NULL || w->op != (uint8_t)EMU_IR_GET || w->imm >= EMU_IR_LOOP_MAX_REGS) {
        return false;
    }
    *sreg = (uint8_t)w->imm;
    return true;
}

/*
 * A DOT16 loop's descriptor (emu_ir_dot_t), or ok false where a kernel
 * could not run it as it stands: an element not at a constant offset
 * from its register, a stride neither constant nor a register shifted,
 * an induction beyond the two elements' and the exit's, a back edge not
 * on NE against an invariant register or constant.
 */
static void lk_dot(const emu_ir_block_t *b, emu_ir_loop_t *out, uint8_t acc, uint16_t prod,
                   uint32_t back, const uint8_t *cls, const uint16_t *get, const uint16_t *put)
{
    emu_ir_dot_t *const d = &out->dot;
    const emu_ir_insn_t *const m = lk_def_insn(b, prod);
    const emu_ir_insn_t *const be = &b->insn[back];

    memset(d, 0, sizeof(*d));
    if (m == NULL || m->op != (uint8_t)EMU_IR_MUL) {
        return;
    }
    const emu_ir_insn_t *const lx = lk_elem_load(b, m->a, &d->xswap);
    const emu_ir_insn_t *const ly = lk_elem_load(b, m->b, &d->yswap);

    if (lx == NULL || ly == NULL) {
        return;
    }
    const lk_t ax = lk_of(lx->a), ay = lk_of(ly->a);

    if (ax.kind != LK_AFF || ay.kind != LK_AFF || ax.reg == ay.reg) {
        return;
    }
    d->xreg = ax.reg;
    d->yreg = ay.reg;
    d->xoff = (int32_t)(ax.off + lx->imm);
    d->yoff = (int32_t)(ay.off + ly->imm);

    uint8_t sreg, sshift;
    int32_t k;

    if (!lk_step(b, get[d->xreg], put[d->xreg], &d->xstride, &sreg, &sshift) ||
        sreg != 0xFFu) {
        return; /* x walks by a constant */
    }
    if (!lk_step(b, get[d->yreg], put[d->yreg], &k, &d->ysreg, &d->ysshift)) {
        return;
    }
    d->ysconst = k;
    if (d->ysreg != 0xFFu && cls[d->ysreg] > 1u) {
        return; /* a stride register the loop writes */
    }

    /* The back edge, normalised as the recogniser did. */
    uint16_t x = be->a, y = be->b;
    uint32_t z;

    if (be->op != (uint8_t)EMU_IR_EXIT_IF ||
        (be->aux != (uint8_t)EMU_IR_C_NE && be->aux != (uint8_t)EMU_IR_C_EQ)) {
        return;
    }
    if (lk_const(b, y, &z) && z == 0u) {
        const emu_ir_insn_t *const s = lk_def_insn(b, x);

        if (s != NULL && s->op == (uint8_t)EMU_IR_SUB) {
            x = s->a;
            y = s->b;
        }
    }
    if (be->aux != (uint8_t)EMU_IR_C_NE) {
        return;
    }
    lk_t ke = lk_of(x), kl = lk_of(y);
    uint16_t lt = y;

    if (ke.kind != LK_AFF) {
        ke = lk_of(y);
        kl = lk_of(x);
        lt = x;
    }
    if (ke.kind != LK_AFF || cls[ke.reg] != 3u || kl.kind != LK_INV) {
        return;
    }
    d->ereg = ke.reg;
    d->ec = (int32_t)ke.off;
    if (!lk_step(b, get[d->ereg], put[d->ereg], &d->estep, &sreg, &sshift) || sreg != 0xFFu ||
        (d->estep != 1 && d->estep != -1 && d->estep != 2 && d->estep != -2 &&
         d->estep != 4 && d->estep != -4)) {
        return;
    }
    if (lk_const(b, lt, &z)) {
        d->lreg = 0xFFu;
        d->lconst = z;
    } else {
        const emu_ir_insn_t *const g = lk_def_insn(b, lt);

        if (g == NULL || g->op != (uint8_t)EMU_IR_GET || g->imm >= EMU_IR_LOOP_MAX_REGS) {
            return;
        }
        d->lreg = (uint8_t)g->imm;
    }

    /* No induction the kernel would not move. */
    for (uint32_t r = 0; r < EMU_IR_LOOP_MAX_REGS; r++) {
        if (cls[r] == 3u && r != d->xreg && r != d->yreg && r != d->ereg) {
            return;
        }
    }
    d->acc = acc;
    for (uint32_t i = 0; i <= back; i++) {
        const emu_ir_insn_t *const in = &b->insn[i];

        if (!in->dead && in->op == (uint8_t)EMU_IR_RETIRE) {
            d->insns += (in->imm == 0u) ? 1u : in->imm;
        }
    }
    d->ok = d->insns != 0u;
}

/*
 * A counted back edge -- NE of an induction register plus a constant
 * against an invariant register or constant -- as emu_ir_dot_t and
 * emu_ir_copy_t hold it. False for any other.
 */
static bool lk_counted_exit(const emu_ir_block_t *b, uint32_t back, const uint8_t *cls,
                            const uint16_t *get, const uint16_t *put, uint8_t *ereg,
                            int32_t *estep, int32_t *ec, uint8_t *lreg, uint32_t *lconst)
{
    const emu_ir_insn_t *const be = &b->insn[back];
    uint16_t x = be->a, y = be->b;
    uint8_t sreg, sshift;
    uint32_t z;

    if (be->op != (uint8_t)EMU_IR_EXIT_IF ||
        (be->aux != (uint8_t)EMU_IR_C_NE && be->aux != (uint8_t)EMU_IR_C_EQ)) {
        return false;
    }
    if (lk_const(b, y, &z) && z == 0u) {
        const emu_ir_insn_t *const s = lk_def_insn(b, x);

        if (s != NULL && s->op == (uint8_t)EMU_IR_SUB) {
            x = s->a;
            y = s->b;
        }
    }
    if (be->aux != (uint8_t)EMU_IR_C_NE) {
        return false;
    }
    lk_t ke = lk_of(x), kl = lk_of(y);
    uint16_t lt = y;

    if (ke.kind != LK_AFF) {
        ke = lk_of(y);
        kl = lk_of(x);
        lt = x;
    }
    if (ke.kind != LK_AFF || cls[ke.reg] != 3u || kl.kind != LK_INV) {
        return false;
    }
    *ereg = ke.reg;
    *ec = (int32_t)ke.off;
    if (!lk_step(b, get[*ereg], put[*ereg], estep, &sreg, &sshift) || sreg != 0xFFu ||
        (*estep != 1 && *estep != -1 && *estep != 2 && *estep != -2 && *estep != 4 &&
         *estep != -4)) {
        return false;
    }
    if (lk_const(b, lt, &z)) {
        *lreg = 0xFFu;
        *lconst = z;
        return true;
    }
    const emu_ir_insn_t *const g = lk_def_insn(b, lt);

    if (g == NULL || g->op != (uint8_t)EMU_IR_GET || g->imm >= EMU_IR_LOOP_MAX_REGS) {
        return false;
    }
    *lreg = (uint8_t)g->imm;
    return true;
}

/*
 * A byte loop's descriptor (emu_ir_copy_t), or ok false where a kernel
 * could not run it: an element not one byte, an access not at a constant
 * offset from its register, a register not stepping by the element, an
 * induction beyond the source's, the destination's and the exit's.
 */
static void lk_copy(const emu_ir_block_t *b, emu_ir_loop_t *out, const emu_ir_insn_t *load,
                    const emu_ir_insn_t *store, uint32_t back, const uint8_t *cls,
                    const uint16_t *get, const uint16_t *put)
{
    emu_ir_copy_t *const c = &out->copy;
    uint8_t sreg, sshift;
    int32_t k;

    memset(c, 0, sizeof(*c));
    c->sreg = 0xFFu;
    c->vreg = 0xFFu;
    c->size = (uint8_t)EMU_IR_MEM_SIZE(store->aux);
    if (c->size != 1u || (load != NULL && EMU_IR_MEM_SIZE(load->aux) != 1u)) {
        return;
    }
    const lk_t ad = lk_of(store->a);

    if (ad.kind != LK_AFF) {
        return;
    }
    c->dreg = ad.reg;
    c->doff = (int32_t)(ad.off + store->imm);
    if (!lk_step(b, get[c->dreg], put[c->dreg], &c->step, &sreg, &sshift) || sreg != 0xFFu ||
        c->step != 1) {
        return;
    }
    if (load != NULL) {
        const lk_t as = lk_of(load->a);

        if (as.kind != LK_AFF || as.reg == c->dreg) {
            return;
        }
        c->sreg = as.reg;
        c->soff = (int32_t)(as.off + load->imm);
        if (!lk_step(b, get[c->sreg], put[c->sreg], &k, &sreg, &sshift) || sreg != 0xFFu ||
            k != c->step) {
            return;
        }
    } else {
        uint32_t v;
        const emu_ir_insn_t *const g = lk_def_insn(b, store->b);

        if (lk_const(b, store->b, &v)) {
            c->vconst = v;
        } else if (g != NULL && g->op == (uint8_t)EMU_IR_GET &&
                   g->imm < EMU_IR_LOOP_MAX_REGS && cls[g->imm] <= 1u) {
            c->vreg = (uint8_t)g->imm;
        } else {
            return;
        }
    }
    if (out->kind != EMU_IR_LOOP_COPY_NUL &&
        !lk_counted_exit(b, back, cls, get, put, &c->ereg, &c->estep, &c->ec, &c->lreg,
                         &c->lconst)) {
        return;
    }
    for (uint32_t r = 0; r < EMU_IR_LOOP_MAX_REGS; r++) {
        if (cls[r] == 3u && r != c->sreg && r != c->dreg &&
            (out->kind == EMU_IR_LOOP_COPY_NUL || r != c->ereg)) {
            return; /* an induction the kernel would not move */
        }
    }
    for (uint32_t i = 0; i <= back; i++) {
        const emu_ir_insn_t *const in = &b->insn[i];

        if (!in->dead && in->op == (uint8_t)EMU_IR_RETIRE) {
            c->insns += (in->imm == 0u) ? 1u : in->imm;
        }
    }
    c->ok = c->insns != 0u;
}

bool emu_ir_loop_shape(const emu_ir_block_t *b, emu_ir_loop_t *out)
{
    enum { R = EMU_IR_LOOP_MAX_REGS };
    uint16_t get[R], put[R];
    bool put_first[R];
    uint8_t cls[R]; /* 0 untouched, 1 invariant, 2 per-iteration, 3 induction, 4 sum,
                     * 5 sticky (ORed into) */
    uint16_t sum_term[R];
    uint32_t back = b->count;

    memset(out, 0, sizeof(*out));
    for (uint32_t i = b->count; i-- > 0;) {
        const emu_ir_insn_t *const in = &b->insn[i];

        if (!in->dead && ((in->op == (uint8_t)EMU_IR_EXIT_IF && in->imm == b->start_pc) ||
                          (in->op == (uint8_t)EMU_IR_EXIT && in->a == EMU_IR_NO_TEMP &&
                           in->imm == b->start_pc))) {
            back = i;
            break;
        }
    }
    if (back == b->count) {
        return false;
    }
    out->back = back;

    for (uint32_t r = 0; r < R; r++) {
        get[r] = put[r] = EMU_IR_NO_TEMP;
        put_first[r] = false;
        cls[r] = 0u;
        sum_term[r] = EMU_IR_NO_TEMP;
    }
    const uint32_t nt = (b->next_temp < EMU_IR_MAX_TEMPS) ? b->next_temp : EMU_IR_MAX_TEMPS;

    for (uint32_t t = 0; t < nt; t++) {
        g_lk[t].kind = 0u;
        g_lk[t].def = EMU_IR_NO_TEMP;
    }

    /* The body's operations, its registers, and what it may not hold. */
    bool flags_set = false;

    for (uint32_t i = 0; i <= back; i++) {
        const emu_ir_insn_t *const in = &b->insn[i];

        if (in->dead) {
            continue;
        }
        if (in->dst < nt) {
            g_lk[in->dst].def = (uint16_t)i;
        }
        switch ((emu_ir_op_t)in->op) {
        case EMU_IR_HELPER:
        case EMU_IR_HELPER_TRAP:
        case EMU_IR_VMEM:
        case EMU_IR_VMEMS:
        case EMU_IR_VOP:
        case EMU_IR_VOPM:
        case EMU_IR_VRED:
        case EMU_IR_VFOP:
        case EMU_IR_BITOP_SET:
        case EMU_IR_BITOP_CLR:
        case EMU_IR_BITOP_INV:
        case EMU_IR_BITOP_TST:
            out->kind = EMU_IR_LOOP_NO_CALL;
            return true;
        case EMU_IR_EXIT:
        case EMU_IR_EXIT_IF:
            if (i != back) {
                out->kind = EMU_IR_LOOP_NO_SIDE;
                return true;
            }
            break;
        case EMU_IR_SETF:
            flags_set = true;
            break;
        case EMU_IR_GETCOND:
        case EMU_IR_SELECT:
            if (!flags_set) {
                out->kind = EMU_IR_LOOP_NO_FLAGS;
                return true;
            }
            break;
        case EMU_IR_GET:
        case EMU_IR_PUT:
            if (in->imm >= R) {
                out->kind = EMU_IR_LOOP_NO_CARRIED;
                return true;
            }
            if (in->op == (uint8_t)EMU_IR_GET) {
                if (get[in->imm] == EMU_IR_NO_TEMP && put[in->imm] == EMU_IR_NO_TEMP) {
                    get[in->imm] = in->dst;
                } else {
                    out->kind = EMU_IR_LOOP_NO_CARRIED; /* reread past a write */
                    return true;
                }
            } else {
                if (put[in->imm] == EMU_IR_NO_TEMP && get[in->imm] == EMU_IR_NO_TEMP) {
                    put_first[in->imm] = true;
                }
                put[in->imm] = in->a;
            }
            break;
        default:
            if (in->op >= (uint8_t)EMU_IR_FGET && in->op <= (uint8_t)EMU_IR_FCLASS) {
                out->kind = EMU_IR_LOOP_NO_FP;
                return true;
            }
            break;
        }
    }

    /* Kinds, forward: the temps are in definition order. */
    for (uint32_t i = 0; i <= back; i++) {
        const emu_ir_insn_t *const in = &b->insn[i];

        if (in->dead || in->dst >= nt) {
            continue;
        }
        lk_t k = {.kind = LK_INV};
        const lk_t ka = lk_of(in->a), kb = lk_of(in->b);
        uint32_t c;

        switch ((emu_ir_op_t)in->op) {
        case EMU_IR_GET:
            if (put[in->imm] != EMU_IR_NO_TEMP) {
                k.kind = LK_AFF;
                k.reg = (uint8_t)in->imm;
            }
            break;
        case EMU_IR_CONST:
            break;
        case EMU_IR_MOV:
            k = ka;
            break;
        case EMU_IR_ADDI:
            k = ka;
            if (ka.kind == LK_AFF) {
                k.off += in->imm;
            }
            break;
        case EMU_IR_ADD:
        case EMU_IR_SUB: {
            const bool sub = in->op == (uint8_t)EMU_IR_SUB;

            if ((ka.kind == LK_AFF || ka.kind == LK_AFFX) && kb.kind == LK_INV) {
                k = ka;
                if (ka.kind == LK_AFF && lk_const(b, in->b, &c)) {
                    k.off += sub ? 0u - c : c;
                } else {
                    k.kind = LK_AFFX;
                }
            } else if (!sub && kb.kind >= LK_AFF && kb.kind <= LK_AFFX && ka.kind == LK_INV) {
                k = kb;
                if (kb.kind == LK_AFF && lk_const(b, in->a, &c)) {
                    k.off += c;
                } else {
                    k.kind = LK_AFFX;
                }
            } else {
                k = lk_mix(ka, kb);
            }
            break;
        }
        case EMU_IR_LOAD:
            k.kind = LK_VAR;
            break;
        default:
            k = lk_mix(ka, lk_mix(kb, lk_of(in->c)));
            break;
        }
        g_lk[in->dst].kind = k.kind; /* not .def: where this temp was defined */
        g_lk[in->dst].reg = k.reg;
        g_lk[in->dst].off = k.off;
    }

    /* Each register the body writes: per-iteration, an induction, or a sum. */
    for (uint32_t r = 0; r < R; r++) {
        if (put[r] == EMU_IR_NO_TEMP) {
            cls[r] = (get[r] != EMU_IR_NO_TEMP) ? 1u : 0u;
            continue;
        }
        if (put_first[r]) {
            cls[r] = 2u;
            continue;
        }
        const lk_t kv = lk_of(put[r]);

        if (put[r] == get[r]) {
            cls[r] = 1u;
        } else if ((kv.kind == LK_AFF && kv.reg == r && kv.off != 0u) ||
                   (kv.kind == LK_AFFX && kv.reg == r)) {
            cls[r] = 3u;
            out->inductions++;
        } else {
            const emu_ir_insn_t *const d =
                (put[r] < nt && g_lk[put[r]].def != EMU_IR_NO_TEMP) ? &b->insn[g_lk[put[r]].def]
                                                                     : NULL;
            const bool sum_op =
                d != NULL && (d->op == (uint8_t)EMU_IR_ADD || d->op == (uint8_t)EMU_IR_OR);
            const uint16_t other = !sum_op             ? EMU_IR_NO_TEMP
                                   : (d->a == get[r]) ? d->b
                                   : (d->b == get[r]) ? d->a
                                                      : EMU_IR_NO_TEMP;

            const lk_t ko = (other == EMU_IR_NO_TEMP) ? lk_of(0u) : lk_of(other);
            /* a sticky bit may come from a sum: ARMv7-M's Q from SMLABB's add */
            const bool from_sum = d != NULL && d->op == (uint8_t)EMU_IR_OR &&
                                  ko.kind == LK_DEP && cls[ko.reg] == 4u;

            if (other == EMU_IR_NO_TEMP ||
                (ko.kind >= LK_AFF && ko.kind != LK_VAR && !from_sum)) {
                out->kind = EMU_IR_LOOP_NO_CARRIED;
                return true;
            }
            if (d->op == (uint8_t)EMU_IR_OR) {
                /* a sticky bit -- ARMv7-M's Q, which SMLABB sets -- ORed in */
                cls[r] = 5u;
                out->sticky++;
                continue;
            }
            cls[r] = 4u;
            sum_term[r] = other;
            out->sums++;
        }
    }

    /* Every access at an induction register plus a constant or the stride. */
    uint16_t load_t = EMU_IR_NO_TEMP;
    const emu_ir_insn_t *store = NULL;

    for (uint32_t i = 0; i <= back; i++) {
        const emu_ir_insn_t *const in = &b->insn[i];

        if (in->dead || (in->op != (uint8_t)EMU_IR_LOAD && in->op != (uint8_t)EMU_IR_STORE)) {
            continue;
        }
        const lk_t ka = lk_of(in->a);

        if ((ka.kind != LK_AFF && ka.kind != LK_AFFX) || cls[ka.reg] != 3u ||
            in->c != EMU_IR_NO_TEMP) {
            out->kind = EMU_IR_LOOP_NO_ADDR;
            return true;
        }
        if (in->op == (uint8_t)EMU_IR_LOAD) {
            out->loads++;
            load_t = in->dst;
        } else {
            out->stores++;
            store = in;
        }
    }

    /* The back edge: on a count, or on a value loaded this iteration. */
    const emu_ir_insn_t *const be = &b->insn[back];
    bool counted = be->op == (uint8_t)EMU_IR_EXIT;
    bool on_data = false;
    uint16_t data_t = EMU_IR_NO_TEMP;
    uint16_t data_k = EMU_IR_NO_TEMP; /* what the loaded value is compared with */

    if (be->op == (uint8_t)EMU_IR_EXIT_IF) {
        uint16_t x = be->a, y = be->b;
        uint32_t z;

        /* ARMv7-M's `cmp; bne`: (p - q) != 0 is p != q */
        if ((be->aux == (uint8_t)EMU_IR_C_NE || be->aux == (uint8_t)EMU_IR_C_EQ) &&
            lk_const(b, y, &z) && z == 0 && x < nt && g_lk[x].def != EMU_IR_NO_TEMP &&
            b->insn[g_lk[x].def].op == (uint8_t)EMU_IR_SUB) {
            y = b->insn[g_lk[x].def].b;
            x = b->insn[g_lk[x].def].a;
        }
        const lk_t kx = lk_of(x), ky = lk_of(y);
        const bool ix = (kx.kind == LK_AFF || kx.kind == LK_AFFX) && cls[kx.reg] == 3u;
        const bool iy = (ky.kind == LK_AFF || ky.kind == LK_AFFX) && cls[ky.reg] == 3u;

        counted = (ix && ky.kind == LK_INV) || (iy && kx.kind == LK_INV);
        on_data = (kx.kind == LK_VAR && ky.kind == LK_INV) ||
                  (ky.kind == LK_VAR && kx.kind == LK_INV);
        data_t = (kx.kind == LK_VAR) ? x : y;
        data_k = (kx.kind == LK_VAR) ? y : x;
    }
    if (!counted && !on_data) {
        out->kind = EMU_IR_LOOP_NO_EXIT;
        return true;
    }

    /* The kernels. */
    out->kind = EMU_IR_LOOP_AFFINE;
    if (counted && out->sums == 1u && out->stores == 0u && out->loads == 2u) {
        for (uint32_t r = 0; r < R; r++) {
            bool mapped = false;

            if (cls[r] == 4u && lk_from_product(b, sum_term[r], 0u, &mapped)) {
                out->kind = mapped ? EMU_IR_LOOP_DOT16_F : EMU_IR_LOOP_DOT16;
                if (!mapped && out->sticky == 0u) {
                    lk_dot(b, out, (uint8_t)r, sum_term[r], back, cls, get, put);
                }
            }
        }
    } else if (on_data && out->sums == 0u && out->loads == 1u && out->stores == 1u &&
               be->aux == (uint8_t)EMU_IR_C_NE && store->b == load_t && data_t == load_t &&
               EMU_IR_MEM_SIZE(store->aux) == 1u &&
               EMU_IR_MEM_SIZE(b->insn[g_lk[load_t].def].aux) == 1u &&
               lk_is_zero(b, data_k)) {
        out->kind = EMU_IR_LOOP_COPY_NUL;
        if (out->sticky == 0u) {
            lk_copy(b, out, &b->insn[g_lk[load_t].def], store, back, cls, get, put);
        }
    } else if (counted && out->sums == 0u && out->loads == 1u && out->stores == 1u &&
               store->b == load_t) {
        out->kind = EMU_IR_LOOP_COPY;
        if (out->sticky == 0u) {
            lk_copy(b, out, &b->insn[g_lk[load_t].def], store, back, cls, get, put);
        }
    } else if (counted && out->sums == 0u && out->loads == 0u && out->stores == 1u &&
               lk_of(store->b).kind == LK_INV) {
        out->kind = EMU_IR_LOOP_FILL;
        if (out->sticky == 0u) {
            lk_copy(b, out, NULL, store, back, cls, get, put);
        }
    }
    return true;
}

#if EMU_IR_LOOP_SHAPE
/*
 * A recognised loop's kernel, first in the block: everything else moves
 * up one. Only where the block has its RAM window -- the kernel's checks
 * are that window's -- the backend lowers one, and there is room.
 */
static void loop_kernel(emu_ir_block_t *b, const emu_ir_target_t *t, const emu_ir_loop_t *lp,
                        emu_ir_opt_stats_t *st)
{
    const bool dot = lp->kind == EMU_IR_LOOP_DOT16 && lp->dot.ok;
    const bool bytes = (lp->kind == EMU_IR_LOOP_COPY_NUL || lp->kind == EMU_IR_LOOP_COPY ||
                        lp->kind == EMU_IR_LOOP_FILL) &&
                       lp->copy.ok;

    const uint8_t aux =
        (uint8_t)(lp->kind | ((dot && lp->dot.xstride == 2 && !lp->dot.xswap && !lp->dot.yswap)
                                  ? EMU_IR_KERNEL_PLAIN
                                  : 0u));

    if (t == NULL || (!dot && !bytes) || (emu_ir_kernel_kinds & (1u << lp->kind)) == 0u ||
        !b->has_fast || b->count + 1u > EMU_IR_MAX_INSNS ||
        !backend_can_lower(EMU_IR_KERNEL, aux)) {
        return;
    }
    const uint32_t k = dot ? emu_ir_kernel_add(&lp->dot, t, &b->fast)
                           : emu_ir_kernel_add_copy(&lp->copy, lp->kind, t, &b->fast,
                                                    b->has_rom ? &b->rom : NULL);

    if (k == EMU_IR_NO_KERNEL) {
        return;
    }
    memmove(&b->insn[1], &b->insn[0], b->count * sizeof(b->insn[0]));
    memset(&b->insn[0], 0, sizeof(b->insn[0]));
    b->insn[0].op = (uint8_t)EMU_IR_KERNEL;
    b->insn[0].aux = aux;
    b->insn[0].dst = EMU_IR_NO_TEMP;
    b->insn[0].a = EMU_IR_NO_TEMP;
    b->insn[0].b = EMU_IR_NO_TEMP;
    b->insn[0].c = EMU_IR_NO_TEMP;
    b->insn[0].imm = k;
    b->count++;
    st->kernels++;
}
#endif

void emu_ir_optimise(emu_ir_block_t *b, const emu_ir_target_t *t,
                     uint8_t live_out, emu_ir_opt_stats_t *stats)
{
    emu_ir_opt_stats_t local;

    if (stats == NULL) {
        stats = &local;
    }
    memset(stats, 0, sizeof(*stats));

    if (b->overflow) {
        return;
    }

    /*
     * Order matters. Flags first, because deleting a SETF is what makes
     * its operands dead; register traffic next, because it rewrites GETs
     * into MOVs; dead values last, to sweep up what the other two
     * orphaned.
     */
    pass_cond_forward(b, t, stats); /* first: what it rewrites reads no flags */
    pass_dead_flags(b, t, live_out, stats);
    pass_reg_traffic(b, t, stats);
    /*
     * After reg_traffic, which rewrites GETs into MOVs and so exposes
     * operands this can reach; before the two sweeps, which are what
     * remove the CONSTs and ADDIs it orphans.
     */
    pass_fuse(b, stats);
    pass_dead_puts(b, t, stats);
    pass_dead_values(b, stats);
    pass_mac(b, stats);
    pass_merge_retire(b, stats);
    pass_tail_exit(b, stats);
    pass_count_uses(b, stats);
#if EMU_IR_LOOP_SHAPE
    {
        emu_ir_loop_t lp;

        if (emu_ir_loop_shape(b, &lp)) {
            stats->loop_shape[lp.kind]++;
            loop_kernel(b, t, &lp, stats);
        }
    }
#endif

    emu_lock(&g_opt_lock);
    g_opt_totals.blocks++;
    g_opt_totals.single_use += stats->single_use;
    g_opt_totals.flags_removed += stats->flags_removed;
    g_opt_totals.gets_removed += stats->gets_removed;
    g_opt_totals.zero_gets += stats->zero_gets;
    g_opt_totals.conds_forwarded += stats->conds_forwarded;
    for (uint32_t k = 0; k < EMU_IR_LOOP_KINDS; k++) {
        g_opt_totals.loop_shape[k] += stats->loop_shape[k];
    }
    g_opt_totals.kernels += stats->kernels;
    g_opt_totals.branch_folded += stats->branch_folded;
    g_opt_totals.puts_removed += stats->puts_removed;
    g_opt_totals.setpcs_removed += stats->setpcs_removed;
    g_opt_totals.folded += stats->folded;
    g_opt_totals.addr_folded += stats->addr_folded;
    g_opt_totals.identities += stats->identities;
    g_opt_totals.macs += stats->macs;
    g_opt_totals.dead_removed += stats->dead_removed;
    g_opt_totals.retires_merged += stats->retires_merged;
    g_opt_totals.tail_exits += stats->tail_exits;
    emu_unlock(&g_opt_lock);

#if EMU_JIT_HOT_REG_STATS
    /*
     * Here and not in a backend: the register traffic a block leaves
     * behind is a property of the IR after the passes, and measuring it
     * per host would be two copies of one number that must agree.
     */
    emu_reg_note_block(b, t);
#endif
}
