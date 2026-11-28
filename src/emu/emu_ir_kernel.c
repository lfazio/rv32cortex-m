/* SPDX-License-Identifier: Apache-2.0 */
/*
 * emu_ir_kernel.c - the loop kernels of docs/jit/autovectorisation.md (P2).
 *
 * A block that loops on itself and that emu_ir_loop_shape recognised as
 * a 16-bit dot product gets an EMU_IR_KERNEL ahead of its body: one call
 * to emu_ir_kernel_run, which runs as many of the loop's iterations as it
 * can prove safe -- all but the last -- and hands back the instructions
 * they retired. The scalar body then runs the rest, which is what makes
 * every register the loop writes, the flags included, end as the loop
 * would leave them: the kernel computes none of them.
 *
 * C, not emitted code: the same function serves both backends and the
 * IR interpreter, which is the reference fuzz-ir compares against the
 * block as built -- the scalar loop. On a core with the DSP extension
 * the pairs of products are SMLAD.
 */

#include <string.h>

#include "emu/emu_ir.h"
#include "emu/emu_jit.h"
#include "emu/emu_lock.h"

#if defined(EMU_IR_LOOP_SHAPE) && EMU_IR_LOOP_SHAPE

#if defined(__ARM_FEATURE_DSP) && __ARM_FEATURE_DSP
#define KERNEL_SMLAD 1
#else
#define KERNEL_SMLAD 0
#endif

#define NO_REG EMU_IR_KERNEL_NO_REG

/* A descriptor, with the registers resolved to their offsets in the cpu. */
typedef struct {
    uint32_t kind;                   /* EMU_IR_LOOP_* */
    uint32_t xo, yo, eo, ao, lo, so; /* offsets; lo, so NO_REG for constants */
    /* the byte loops: source, destination, a fill's value, the store guard */
    uint32_t sro, dro, vo, guard;
    int32_t soff, doff;
    uint32_t vconst;
    int32_t xoff, yoff, xstride, ysconst, estep, ec;
    uint32_t lconst, ysshift, insns;
    uint32_t qo, qbit; /* the sticky overflow bit, where q */
    bool q;
    uint32_t base, size;
    const uint8_t *host;
    /* the image window, read-only: a byte loop's source may be there
     * (Dhrystone's strcpy reads string literals); size 0 for none */
    uint32_t rbase, rsize;
    const uint8_t *rhost;
    bool xswap, yswap;
    /* DOT16_F's map, as emu_ir_dot_t's; fchains 0 for a plain product */
    uint8_t fchains;
    uint8_t flen[2];
    uint8_t fop[2][EMU_IR_DOT_FN_OPS];
    uint32_t fimm[2][EMU_IR_DOT_FN_OPS];
} kernel_t;

uint32_t emu_ir_kernel_kinds =
    (1u << EMU_IR_LOOP_COPY_NUL) | (1u << EMU_IR_LOOP_DOT16) | (1u << EMU_IR_LOOP_DOT16_F);

static kernel_t g_kernels[EMU_IR_KERNELS];
static uint32_t g_nkernels;
/* What the kernels did: a kernel that never runs shows as zeros here. */
static uint32_t g_calls, g_runs, g_iters, g_full;
static emu_lock_t g_kernel_lock = EMU_LOCK_INIT;

static uint32_t kernel_insert(const kernel_t *kp);

uint32_t emu_ir_kernel_add(const emu_ir_dot_t *d, const emu_ir_target_t *t,
                           const emu_ir_fastmem_t *w)
{
    kernel_t k;

    memset(&k, 0, sizeof(k)); /* compared whole, padding included */
    k.kind = EMU_IR_LOOP_DOT16;
    k.xo = t->reg_offset(d->xreg);
    k.yo = t->reg_offset(d->yreg);
    k.eo = t->reg_offset(d->ereg);
    k.ao = t->reg_offset(d->acc);
    k.lo = (d->lreg == 0xFFu) ? NO_REG : t->reg_offset(d->lreg);
    k.so = (d->ysreg == 0xFFu) ? NO_REG : t->reg_offset(d->ysreg);
    k.xoff = d->xoff;
    k.yoff = d->yoff;
    k.xstride = d->xstride;
    k.ysconst = d->ysconst;
    k.ysshift = d->ysshift;
    k.estep = d->estep;
    k.ec = d->ec;
    k.lconst = d->lconst;
    k.insns = d->insns;
    k.base = w->base;
    k.size = w->size;
    k.host = w->host;
    k.xswap = d->xswap;
    k.yswap = d->yswap;
    k.fchains = d->fchains;
    memcpy(k.flen, d->flen, sizeof(k.flen));
    memcpy(k.fop, d->fop, sizeof(k.fop));
    memcpy(k.fimm, d->fimm, sizeof(k.fimm));
    k.q = d->q;
    if (d->q) {
        k.qo = t->reg_offset(d->qreg);
        k.qbit = d->qbit;
    }
    return kernel_insert(&k);
}

uint32_t emu_ir_kernel_add_copy(const emu_ir_copy_t *c, emu_ir_loop_kind_t kind,
                                const emu_ir_target_t *t, const emu_ir_fastmem_t *w,
                                const emu_ir_fastmem_t *rom)
{
    kernel_t k;

    memset(&k, 0, sizeof(k));
    k.kind = (uint32_t)kind;
    k.sro = (c->sreg == 0xFFu) ? NO_REG : t->reg_offset(c->sreg);
    k.dro = t->reg_offset(c->dreg);
    k.soff = c->soff;
    k.doff = c->doff;
    k.vo = (c->vreg == 0xFFu) ? NO_REG : t->reg_offset(c->vreg);
    k.vconst = c->vconst;
    k.eo = (kind == EMU_IR_LOOP_COPY_NUL) ? NO_REG : t->reg_offset(c->ereg);
    k.lo = (kind == EMU_IR_LOOP_COPY_NUL || c->lreg == 0xFFu) ? NO_REG : t->reg_offset(c->lreg);
    k.lconst = c->lconst;
    k.estep = c->estep;
    k.ec = c->ec;
    k.insns = c->insns;
    k.guard = w->store_guard_offset;
    k.base = w->base;
    k.size = w->size;
    k.host = w->host;
    if (rom != NULL) {
        k.rbase = rom->base;
        k.rsize = rom->size;
        k.rhost = rom->host;
    }
    return kernel_insert(&k);
}

static uint32_t kernel_insert(const kernel_t *kp)
{
    const kernel_t k = *kp;
    uint32_t i;

    emu_lock(&g_kernel_lock);
    for (i = 0; i < g_nkernels; i++) {
        if (memcmp(&g_kernels[i], &k, sizeof(k)) == 0) {
            break;
        }
    }
    if (i == g_nkernels) {
        if (g_nkernels == EMU_IR_KERNELS) {
            i = EMU_IR_NO_KERNEL; /* full: the loop stays scalar */
            g_full++;
        } else {
            g_kernels[g_nkernels++] = k;
        }
    }
    emu_unlock(&g_kernel_lock);
    return i;
}

static EMU_ALWAYS_INLINE uint32_t reg(const emu_cpu_t *cpu, uint32_t off)
{
    uint32_t v;

    memcpy(&v, (const uint8_t *)cpu + off, 4);
    return v;
}

static EMU_ALWAYS_INLINE void set_reg(emu_cpu_t *cpu, uint32_t off, uint32_t v)
{
    memcpy((uint8_t *)cpu + off, &v, 4);
}

/*
 * Whether n halfwords from `first`, `stride` bytes apart, all lie in the
 * window and are halfword-aligned: the conditions the scalar body's
 * inlined loads take their fast path under. Affine, so the two ends
 * bound the rest -- in 64 bits, where a run past the top cannot wrap.
 */
static EMU_ALWAYS_INLINE bool in_window(const kernel_t *k, uint32_t first, int64_t stride, uint32_t n)
{
    const int64_t lo = (int64_t)first - (int64_t)k->base;
    const int64_t hi = lo + stride * (int64_t)(n - 1u);
    const int64_t a = (lo < hi) ? lo : hi;
    const int64_t b = (lo < hi) ? hi : lo;

    return ((first | (uint32_t)stride) & 1u) == 0u && a >= 0 && b + 2 <= (int64_t)k->size;
}

static EMU_ALWAYS_INLINE int32_t elem(const kernel_t *k, uint32_t addr, bool swap)
{
    const uint8_t *const p = k->host + (addr - k->base);
    const uint16_t v = swap ? (uint16_t)((p[0] << 8) | p[1]) : (uint16_t)(p[0] | (p[1] << 8));

    return (int16_t)v;
}

/*
 * Forget every descriptor: for a harness that builds and drops blocks by
 * the thousand (fuzz-ir), with no block left that names one. The JIT
 * never calls it: a program has far fewer kernel loops than the table.
 */
void emu_ir_kernel_reset(void)
{
    emu_lock(&g_kernel_lock);
    g_nkernels = 0u;
    emu_unlock(&g_kernel_lock);
}

uint32_t emu_ir_kernel_inline_iters;
uint32_t emu_ir_kernel_inline_min = 65536u;

bool emu_ir_kernel_dot(uint32_t index, emu_ir_kernel_dot_t *out)
{
    if (index >= g_nkernels || g_kernels[index].kind != EMU_IR_LOOP_DOT16) {
        return false;
    }
    const kernel_t *const k = &g_kernels[index];

    out->xo = k->xo;
    out->yo = k->yo;
    out->eo = k->eo;
    out->ao = k->ao;
    out->lo = k->lo;
    out->so = k->so;
    out->xoff = k->xoff;
    out->yoff = k->yoff;
    out->xstride = k->xstride;
    out->ysconst = k->ysconst;
    out->estep = k->estep;
    out->ec = k->ec;
    out->lconst = k->lconst;
    out->ysshift = k->ysshift;
    out->insns = k->insns;
    out->base = k->base;
    out->size = k->size;
    out->host = k->host;
    out->xswap = k->xswap;
    out->yswap = k->yswap;
    out->fchains = k->fchains;
    memcpy(out->flen, k->flen, sizeof(out->flen));
    memcpy(out->fop, k->fop, sizeof(out->fop));
    memcpy(out->fimm, k->fimm, sizeof(out->fimm));
    out->q = k->q;
    out->qo = k->qo;
    out->qbit = k->qbit;
    return true;
}

void emu_ir_kernel_stats(uint32_t *kernels, uint32_t *calls, uint32_t *runs,
                         uint32_t *iters, uint32_t *full)
{
    *kernels = g_nkernels;
    *calls = g_calls;
    *runs = g_runs;
    *iters = g_iters;
    *full = g_full;
}

/*
 * The iterations a counted loop has after this one (the m with
 * e + ec + m * estep == L, modulo 2^32), no more than the scalar loop
 * would run before the cap ends this entry, less the last: 0 for none.
 */
/* Where the kernels go: the hot section, or flash (EMU_IR_KERNEL_HOT). */
#if defined(EMU_IR_KERNEL_HOT) && !EMU_IR_KERNEL_HOT
#define KERNEL_TEXT EMU_NOINLINE
#else
#define KERNEL_TEXT EMU_HOT_TEXT
#endif

static uint32_t kernel_trip(const emu_cpu_t *cpu, const kernel_t *k, uint32_t retired);

/* One of a map's operations, as src/backend/common/interp.c has it. */
static EMU_ALWAYS_INLINE uint32_t map_op(uint8_t op, uint32_t imm, uint32_t v)
{
    switch ((emu_ir_op_t)op) {
    case EMU_IR_ADDI:
        return v + imm;
    case EMU_IR_ANDI:
        return v & imm;
    case EMU_IR_ORI:
        return v | imm;
    case EMU_IR_XORI:
        return v ^ imm;
    case EMU_IR_SHLI:
        return v << (imm & 31u);
    case EMU_IR_SHRI:
        return v >> (imm & 31u);
    case EMU_IR_SARI:
        return (uint32_t)((int32_t)v >> (imm & 31u));
    case EMU_IR_ROTLI:
        return ((imm & 31u) == 0u) ? v : ((v << (imm & 31u)) | (v >> (32u - (imm & 31u))));
    case EMU_IR_SEXT8:
        return (uint32_t)(int32_t)(int8_t)v;
    case EMU_IR_SEXT16:
        return (uint32_t)(int32_t)(int16_t)v;
    case EMU_IR_ZEXT8:
        return v & 0xFFu;
    case EMU_IR_ZEXT16:
        return v & 0xFFFFu;
    case EMU_IR_NOT:
        return ~v;
    default: /* EMU_IR_NEG: the recogniser takes no other */
        return 0u - v;
    }
}

static EMU_ALWAYS_INLINE uint32_t map_chain(const kernel_t *k, unsigned c, uint32_t v)
{
    for (uint32_t i = 0u; i < k->flen[c]; i++) {
        v = map_op(k->fop[c][i], k->fimm[c][i], v);
    }
    return v;
}

/* What an iteration adds for the product p: DOT16_F's map of it. */
static EMU_ALWAYS_INLINE uint32_t mapped(const kernel_t *k, uint32_t p)
{
    if (k->fchains == 0u) {
        return p;
    }
    if (k->fchains == 1u) {
        return map_chain(k, 0u, p);
    }
    return map_chain(k, 0u, p) * map_chain(k, 1u, p);
}

/* How many the cap leaves: the scalar loop's iterations this entry, less one. */
static EMU_ALWAYS_INLINE uint32_t kernel_room(const kernel_t *k, uint32_t retired)
{
    return (retired >= EMU_JIT_LOOP_CAP)
               ? 0u
               : (EMU_JIT_LOOP_CAP - retired + k->insns - 1u) / k->insns - 1u;
}

KERNEL_TEXT static uint32_t kernel_trip(const emu_cpu_t *cpu, const kernel_t *k,
                                         uint32_t retired)
{
    const uint32_t e = reg(cpu, k->eo);
    const uint32_t l = (k->lo == NO_REG) ? k->lconst : reg(cpu, k->lo);
    const uint32_t s = (uint32_t)((k->estep < 0) ? -k->estep : k->estep);
    uint32_t d = l - e - (uint32_t)k->ec;

    /*
     * The iterations after this one: the m with e + ec + m * estep == L,
     * modulo 2^32. The back edge is taken after iteration i while
     * e + ec + i * estep != L, so the loop runs m + 1 times; the kernel
     * runs at most m and leaves the rest to the scalar body.
     */
    if (k->estep < 0) {
        d = 0u - d;
    }
    if ((d & (s - 1u)) != 0u) {
        /*
         * Not a whole number of steps: the scalar loop never meets L and
         * runs to the cap, which the kernel never passes either -- so this
         * saves work and decides nothing (a mutant without it is
         * equivalent, and fuzz-ir rightly does not find it).
         */
        return 0u;
    }
    uint32_t m = (s == 1u) ? d : (s == 2u) ? (d >> 1) : (d >> 2);

    /*
     * And no more than the scalar loop would run in this entry: its back
     * edge leaves once the count reaches EMU_JIT_LOOP_CAP, which bounds
     * an interrupt's wait, so the iteration the cap ends the entry on is
     * the j-th with retired + j * insns >= cap. The kernel runs j - 1 of
     * them and the body the j-th: the block leaves where it would have,
     * in the state it would have -- which is what lets fuzz-ir compare a
     * block with its kernel to the block as built.
     */
    const uint32_t room = kernel_room(k, retired);

    return (m > room) ? room : m;
}

static EMU_ALWAYS_INLINE bool byte_in(const kernel_t *k, uint32_t addr)
{
    return addr - k->base < k->size;
}

static EMU_ALWAYS_INLINE bool byte_in_rom(const kernel_t *k, uint32_t addr)
{
    return addr - k->rbase < k->rsize;
}

/*
 * The byte loops, a byte at a time in the scalar loop's order -- so a
 * destination overlapping its source reads what it wrote, as the loop
 * would -- and with memcpy or memset where the two cannot meet. Each
 * stops one iteration short of where the scalar loop would end this
 * entry, and only where that iteration's bytes are in the window too: it
 * is the body's, and must complete (emu_ir_kernel_run's dot product).
 */
KERNEL_TEXT static uint32_t kernel_bytes(emu_cpu_t *cpu, const kernel_t *k, uint32_t retired)
{
    if (k->guard != EMU_IR_NO_GUARD && ((const uint8_t *)cpu)[k->guard] != 0u) {
        return 0u; /* the inlined stores' guard: they would take the slow path */
    }
    const uint32_t s = (k->sro == NO_REG) ? 0u : reg(cpu, k->sro) + (uint32_t)k->soff;
    const uint32_t d = reg(cpu, k->dro) + (uint32_t)k->doff;
    uint8_t *const host = (uint8_t *)k->host;
    uint32_t m;

    if (k->kind == EMU_IR_LOOP_COPY_NUL) {
        m = kernel_room(k, retired);
    } else {
        m = kernel_trip(cpu, k, retired);
    }
    /*
     * Bytes 0..m: the kernel's m and the body's one, the destination's in
     * the RAM window and the source's wholly in it or wholly in the image
     * -- which the body reads through the same window.
     */
    const uint8_t *ps = NULL;

    if (m != 0u && byte_in(k, d) && byte_in(k, d + m) && d + m >= d &&
        (k->sro == NO_REG || s + m >= s)) {
        if (k->sro == NO_REG) {
            ps = host; /* a fill reads nothing */
        } else if (byte_in(k, s) && byte_in(k, s + m)) {
            ps = host + (s - k->base);
        } else if (byte_in_rom(k, s) && byte_in_rom(k, s + m)) {
            ps = k->rhost + (s - k->rbase);
        }
    }
    if (ps == NULL) {
        return 0u;
    }
    uint8_t *const pd = host + (d - k->base);
    uint32_t n = m;

    if (k->kind == EMU_IR_LOOP_COPY_NUL) {
        for (n = 0u; n < m; n++) {
            const uint8_t c = ps[n];

            if (c == 0u) {
                break; /* the NUL's iteration is the body's */
            }
            pd[n] = c;
        }
    } else if (k->kind == EMU_IR_LOOP_FILL) {
        memset(pd, (int)((k->vo == NO_REG) ? k->vconst : reg(cpu, k->vo)), m);
    } else if (ps + m <= pd || pd + m <= ps) {
        memcpy(pd, ps, m);
    } else {
        for (uint32_t i = 0; i < m; i++) {
            pd[i] = ps[i];
        }
    }
    if (n == 0u) {
        return 0u;
    }
    set_reg(cpu, k->dro, reg(cpu, k->dro) + n);
    if (k->sro != NO_REG) {
        set_reg(cpu, k->sro, reg(cpu, k->sro) + n);
    }
    if (k->eo != NO_REG && k->eo != k->dro && k->eo != k->sro) {
        set_reg(cpu, k->eo, reg(cpu, k->eo) + (uint32_t)k->estep * n);
    }
    g_runs++;
    g_iters += n;
    return n * k->insns;
}

/*
 * In ITCM where the platform has it (EMU_HOT_TEXT): called once a block
 * entry, it was in the F746's flash, and RV32 CoreMark ran 3.7% slower
 * with it than without. Not in G4MH's F746 firmware (EMU_IR_KERNEL_HOT),
 * whose ITCM the loop cap's check wants more.
 */
KERNEL_TEXT uint32_t emu_ir_kernel_run(emu_cpu_t *cpu, uint32_t index, uint32_t retired)
{
    g_calls++;
    if (index >= g_nkernels) {
        return 0u;
    }
    const kernel_t *const k = &g_kernels[index];

    if (k->kind != EMU_IR_LOOP_DOT16) {
        return kernel_bytes(cpu, k, retired);
    }
    const uint32_t e = reg(cpu, k->eo);
    const uint32_t m = kernel_trip(cpu, k, retired);

    if (m == 0u) {
        return 0u;
    }
    const uint32_t x = reg(cpu, k->xo) + (uint32_t)k->xoff;
    const uint32_t y = reg(cpu, k->yo) + (uint32_t)k->yoff;
    const int32_t ys = (k->so == NO_REG) ? k->ysconst : (int32_t)(reg(cpu, k->so) << k->ysshift);

    /*
     * The window for m + 1 iterations, not m: the iteration after the
     * kernel is the scalar body's, and the registers each iteration
     * writes -- a product, a loaded value, the flags -- end as that one
     * leaves them only if it completes. Were its access outside, it
     * would trap partway and leave them as the kernel's last iteration
     * found them, not as the scalar loop's last left them (fuzz-ir: r6,
     * an array ending one element short of the window's end).
     */
    if (!in_window(k, x, k->xstride, m + 1u) || !in_window(k, y, ys, m + 1u)) {
        return 0u;
    }

    uint32_t sum = 0u;
    uint32_t i = 0u;

    if (k->q) {
        /*
         * The sum step by step from the register's value, each add's
         * signed overflow ORed into the sticky bit -- SMLABB's Q, set by
         * an intermediate overflow even where the total comes back.
         */
        uint32_t s = reg(cpu, k->ao), ovf = 0u;

        for (; i < m; i++) {
            const uint32_t v = mapped(k, (uint32_t)(elem(k, x + (uint32_t)k->xstride * i, k->xswap) *
                                                    elem(k, y + (uint32_t)ys * i, k->yswap)));
            const uint32_t s2 = s + v;

            ovf |= (v ^ s2) & (s ^ s2);
            s = s2;
        }
        sum = s - reg(cpu, k->ao);
        if ((ovf >> 31) != 0u) {
            set_reg(cpu, k->qo, reg(cpu, k->qo) | (1u << k->qbit));
        }
    }
#if KERNEL_SMLAD
    if (!k->q && k->fchains == 0u && k->xstride == 2 && !k->xswap && !k->yswap &&
        ((x - k->base) & 3u) == 0u) {
        /* Two products a SMLAD: x's pair from one word, y's packed. */
        const uint8_t *px = k->host + (x - k->base);

        for (; i + 1u < m; i += 2u) {
            uint32_t xw;
            const uint32_t y0 = (uint16_t)elem(k, y + (uint32_t)ys * i, false);
            const uint32_t y1 = (uint16_t)elem(k, y + (uint32_t)ys * (i + 1u), false);

            memcpy(&xw, px + 2u * i, 4);
            __asm__("smlad %0, %1, %2, %0" : "+r"(sum) : "r"(xw), "r"(y0 | (y1 << 16)));
        }
    }
#endif
    for (; i < m; i++) {
        sum += mapped(k, (uint32_t)(elem(k, x + (uint32_t)k->xstride * i, k->xswap) *
                                    elem(k, y + (uint32_t)ys * i, k->yswap)));
    }

    set_reg(cpu, k->xo, reg(cpu, k->xo) + (uint32_t)k->xstride * m);
    set_reg(cpu, k->yo, reg(cpu, k->yo) + (uint32_t)ys * m);
    if (k->eo != k->xo && k->eo != k->yo) {
        set_reg(cpu, k->eo, e + (uint32_t)k->estep * m);
    }
    set_reg(cpu, k->ao, reg(cpu, k->ao) + sum);
    g_runs++;
    g_iters += m;
    return m * k->insns;
}

#endif /* EMU_IR_LOOP_SHAPE */
