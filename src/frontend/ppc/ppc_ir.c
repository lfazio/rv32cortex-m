/* SPDX-License-Identifier: Apache-2.0 */
/*
 * ppc_ir.c - e200z7 to IR.
 *
 * The frontend half of frontend -> IR -> optimisation -> backend. It
 * knows the guest and nothing about any host, and it works from the
 * same ppc_decode() the interpreter does, so both of the core's
 * encodings arrive here already reduced to one semantic form: there is
 * no VLE in this file and no Book E either.
 *
 * Three outcomes per instruction, counted (ppc_ir_get_stats):
 *
 *   lowered    to IR.
 *   helper     the interpreter's own ppc_exec, called from inside the
 *              block. The block stays whole, which this project has
 *              measured to matter more than how well one instruction is
 *              translated.
 *   declined   the block ends before it and the interpreter runs it
 *              from the dispatcher. Deliberate, and a short list:
 *              whatever changes MSR, takes an interrupt on purpose,
 *              reads a timer, or tells the translator its code moved.
 *              That makes the interpreter the single place those happen,
 *              and it is why a block needs no generation and no
 *              privilege in its context -- see ppc_cpu.h.
 *
 * The condition register and XER
 * ------------------------------
 * CR is one guest word and XER another, and an instruction that writes
 * a CR field or XER[CA] says so in IR: three compares, shifts and an
 * insert. Nothing is lazy. The shared passes forward a field from the
 * compare that wrote it to the branch that tests it, and the ARMv7-M
 * frontend measured the lazy alternative at under 4% -- after building
 * it. If a profile says otherwise it can be done here too, afterwards.
 *
 * Memory, and byte order
 * ----------------------
 * This is the tree's big-endian guest, and the window a backend may
 * access directly (emu_ir_fastmem_t) is bytes on a little-endian host.
 * So an IR LOAD here means *the bytes as the host reads them*, and the
 * translator follows every one with a byte swap -- except lwbrx and its
 * relatives, whose whole meaning is that there is none. The two
 * functions a backend calls outside the window keep the same contract:
 * they swap what the bus gave them, so that the IR's swap undoes it.
 * One rule, on both paths, and a device register read through the bus
 * comes out as the value the device holds.
 */

#include "ppc/ppc_ir.h"

#include "ppc/ppc_cpu.h"
#include "ppc/ppc_decode.h"

#include "emu/emu_ir.h"
#include "emu/emu_jit.h"

#include <stddef.h>
#include <string.h>

#if EMU_HAVE_JIT

/*
 * Guest instructions per block. Interrupts and the decrementer are
 * delivered between blocks, so this is an interrupt-latency bound as
 * much as a size one.
 *
 * **Shorter on a microcontroller, and that was measured rather than
 * argued.** There the code cache is smaller than a guest's translated
 * working set, so blocks are evicted and built again, and a long block
 * is the expensive one to lose: it is more work to translate, and it is
 * the one that overruns the buffer's reserve and has to be translated
 * twice. Nucleo-F746ZG, host cycles per guest instruction:
 *
 *                      cap 64    cap 32    cap 16    cap 8
 *   32 KB  crypto      1567.1     721.7     716.5      --
 *          CoreMark     393.4     388.3     335.1     341.2
 *   96 KB  crypto        33.2       --       35.4      --
 *          CoreMark     167.9       --      125.8      --
 *
 * Sixteen is better wherever the cache is thrashing -- which at the
 * default 32 KB is everywhere -- and 6.6% worse in the one cell where
 * the working set fits (crypto at 96 KB), because a shorter block is
 * one more trip through the dispatcher. A host has 32 MB and never
 * thrashes, so it keeps the long ones.
 *
 * It does not make the JIT a win at 32 KB: the interpreter is 243.4 on
 * crypto and 222.3 on CoreMark. See docs/jit/tuning.md.
 */
#ifndef PPC_MAX_BLOCK_INSNS
#if defined(EMU_HOST_JIT_THUMB2)
#define PPC_MAX_BLOCK_INSNS 16u
#else
#define PPC_MAX_BLOCK_INSNS 64u
#endif
#endif

/*
 * Room an instruction may need. A block that overflows is discarded, so
 * the translator stops while there is still space for the widest
 * lowering -- a carrying add with a record bit -- rather than finding
 * out afterwards.
 */
#define PPC_IR_HEADROOM 96u
#define PPC_TEMP_HEADROOM 96u

/* Guest "registers" beyond r0-r31. */
#define R_CR 32u
#define R_XER 33u
#define R_LR 34u
#define R_CTR 35u

#define NOT EMU_IR_NO_TEMP

typedef emu_ir_block_t B;
typedef ppc_insn_t I;

static ppc_ir_stats_t g_stats;

const ppc_ir_stats_t *ppc_ir_get_stats(void)
{
    return &g_stats;
}

const char *ppc_sem_name(uint32_t sem)
{
    static const char *const k[PPC_S_COUNT] = {
#define X(n) #n,
        PPC_SEMANTICS(X)
#undef X
    };

    return (sem < PPC_S_COUNT) ? k[sem] : "?";
}

/* ------------------------------------------------------------------ */
/* What the lowered code calls                                         */
/* ------------------------------------------------------------------ */

static uint32_t swap(uint32_t v, uint32_t size)
{
    if (size == 4u) {
        return (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24);
    }
    if (size == 2u) {
        return ((v >> 8) & 0xFFu) | ((v & 0xFFu) << 8);
    }
    return v;
}

/*
 * Outside the window. pc already names the instruction: the translator
 * writes it after every one, so it is this one's until this one is
 * done. The value goes back byte-swapped -- see the note at the top.
 */
static uint32_t ppc_ir_load(emu_cpu_t *cpu, uint32_t addr, uint32_t spec,
                            uint32_t *out)
{
    ppc_cpu_t *const c = (ppc_cpu_t *)cpu;
    const uint32_t size = EMU_IR_MEM_SIZE(spec);
    uint32_t v = 0u;

    if (EMU_UNLIKELY(ppc_load(c, addr, size, false, &v) != PPC_EXC_NONE)) {
        ppc_cpu_raise(c, PPC_IVOR_DATA_STORAGE, c->pc, 0u);
        return 1u;
    }
    *out = swap(v, size);
    return 0u;
}

static uint32_t ppc_ir_store(emu_cpu_t *cpu, uint32_t addr, uint32_t spec,
                             uint32_t val)
{
    ppc_cpu_t *const c = (ppc_cpu_t *)cpu;
    const uint32_t size = EMU_IR_MEM_SIZE(spec);

    if (EMU_UNLIKELY(ppc_store(c, addr, size, swap(val, size)) != PPC_EXC_NONE)) {
        ppc_cpu_raise(c, PPC_IVOR_DATA_STORAGE, c->pc, PPC_ESR_ST);
        return 1u;
    }
    return 0u;
}

/*
 * The fallback: one instruction, by the interpreter. `insn` is as the
 * fetch assembled it, first halfword in the high half.
 *
 * Non-zero means it did not complete -- it raised, and the interrupt has
 * been entered -- so the block stops there without counting it, exactly
 * as the interpreter does not count it. Zero means it completed and
 * fell through: nothing that branches is sent here.
 */
static uint32_t ppc_ir_step(emu_cpu_t *cpu, uint32_t insn, uint32_t pc)
{
    ppc_cpu_t *const c = (ppc_cpu_t *)cpu;
    const unsigned len = c->vle ? ppc_vle_len((uint16_t)(insn >> 16)) : 4u;
    ppc_insn_t d;

    ppc_decode(insn, len, c->vle, &d);
    return ppc_exec(c, &d, pc) ? 0u : 1u;
}

static const void *const ppc_ir_helpers[] = {
    (const void *)ppc_ir_step,
};
#define PPC_HELPER_STEP 0u

static uint32_t ppc_reg_offset(uint32_t n)
{
    if (n < 32u) {
        return (uint32_t)offsetof(ppc_cpu_t, r) + n * 4u;
    }
    switch (n) {
    case R_CR:
        return (uint32_t)offsetof(ppc_cpu_t, cr);
    case R_XER:
        return (uint32_t)offsetof(ppc_cpu_t, xer);
    case R_LR:
        return (uint32_t)offsetof(ppc_cpu_t, lr);
    default:
        return (uint32_t)offsetof(ppc_cpu_t, ctr);
    }
}

static bool ppc_reg_zero(uint32_t n)
{
    (void)n;
    return false; /* r0 is a real register; "rA|0" is the decoder's */
}

const emu_ir_target_t ppc_ir_target = {
    .reg_offset = ppc_reg_offset,
    /*
     * No EMU_IR_SETF is ever emitted, so no flag has a home: CR and XER
     * are guest registers and their bits are computed in the open.
     */
    .flags_offset = (uint32_t)offsetof(ppc_cpu_t, cr),
    .flag_bit = {0u, 0u, 0u, 0u},
    .reg_is_zero = ppc_reg_zero,
    .pc_offset = (uint32_t)offsetof(ppc_cpu_t, pc),
    .helpers = ppc_ir_helpers,
    .helper_count = (uint32_t)(sizeof(ppc_ir_helpers) / sizeof(ppc_ir_helpers[0])),
    .load = ppc_ir_load,
    .store = ppc_ir_store,
};

/*
 * The window: the RAM the stack is in, which is where a compiled
 * program's loads and stores overwhelmingly go, found by asking the bus
 * where r1 points so that no memory map is written down here.
 *
 * There is nothing to say no for. This core has no MMU or MPU in this
 * model and performs unaligned accesses, so an access to RAM is a move
 * -- once it has been byte-swapped, which needs the host to have the
 * instruction. A store needs no guard either: the reservation holds no
 * address (the e200 compares none), so a store cannot break one.
 */
static bool ppc_fast_mem(ppc_cpu_t *c, emu_ir_fastmem_t *out)
{
    emu_region_t *r;

    if (c->bus == NULL || !emu_ir_can_lower(EMU_IR_BSWAP32, 0u) ||
        !emu_ir_can_lower(EMU_IR_BSWAP16, 0u)) {
        return false;
    }
    r = emu_bus_find(c->bus, c->r[1] - 4u);
    if (r == NULL || r->kind != (uint8_t)EMU_MEM_RAM || r->host == NULL ||
        r->size == 0u ||
        (r->perm & (EMU_PERM_R | EMU_PERM_W)) != (EMU_PERM_R | EMU_PERM_W)) {
        return false;
    }
    out->base = r->base;
    out->size = r->size;
    out->host = r->host;
    out->store_guard_offset = EMU_IR_NO_GUARD;
    return true;
}

/* ------------------------------------------------------------------ */
/* Building blocks                                                     */
/* ------------------------------------------------------------------ */

typedef struct {
    B *b;
    ppc_cpu_t *c;
    uint32_t pc;   /* of the instruction being lowered */
    uint32_t next; /* of the one after it              */
    uint32_t insn;
    bool ends;    /* the instruction left the block   */
    bool counted; /* ...or emitted its own RETIRE     */
} T;

static uint16_t K(B *b, uint32_t v)
{
    return emu_ir_const(b, v);
}

static uint16_t G(B *b, uint32_t r)
{
    return emu_ir_get(b, r);
}

static void P(B *b, uint32_t r, uint16_t v)
{
    emu_ir_put(b, r, v);
}

static uint16_t OP(B *b, emu_ir_op_t op, uint16_t x, uint16_t y)
{
    return emu_ir_alu(b, op, x, y);
}

static uint16_t OPI(B *b, emu_ir_op_t op, uint16_t x, uint32_t imm)
{
    return emu_ir_emit(b, op, 0u, x, NOT, imm, 0u);
}

static uint16_t U(B *b, emu_ir_op_t op, uint16_t x)
{
    return emu_ir_emit(b, op, 0u, x, NOT, 0u, 0u);
}

static uint16_t CC(B *b, emu_ir_cond_t cond, uint16_t x, uint16_t y)
{
    return emu_ir_emit(b, EMU_IR_SETCC, (uint8_t)cond, x, y, 0u, 0u);
}

static uint16_t ANDK(B *b, uint16_t x, uint32_t k)
{
    return (k == 0xFFFFFFFFu) ? x : OP(b, EMU_IR_AND, x, K(b, k));
}

static uint16_t XORK(B *b, uint16_t x, uint32_t k)
{
    return OP(b, EMU_IR_XOR, x, K(b, k));
}

static uint16_t ADDK(B *b, uint16_t x, uint32_t k)
{
    return (k == 0u) ? x : OP(b, EMU_IR_ADD, x, K(b, k));
}

static uint16_t SHL(B *b, uint16_t x, uint32_t n)
{
    return (n == 0u) ? x : OPI(b, EMU_IR_SHLI, x, n);
}

static uint16_t SHR(B *b, uint16_t x, uint32_t n)
{
    return (n == 0u) ? x : OPI(b, EMU_IR_SHRI, x, n);
}

static uint16_t INV(B *b, uint16_t x)
{
    return U(b, EMU_IR_NOT, x);
}

static void retire(T *t)
{
    (void)emu_ir_emit(t->b, EMU_IR_RETIRE, 0u, NOT, NOT, 0u, 0u);
    t->counted = true;
}

/* rA|0. */
static uint16_t base_of(T *t, const I *d)
{
    return (d->ra0 && d->ra == 0u) ? K(t->b, 0u) : G(t->b, d->ra);
}

/* XER[SO] as the low bit of a CR field. */
static uint16_t so01(B *b)
{
    return SHR(b, G(b, R_XER), 31u);
}

static uint16_t ca01(B *b)
{
    return ANDK(b, SHR(b, G(b, R_XER), 29u), 1u);
}

static void put_ca(B *b, uint16_t c01)
{
    P(b, R_XER,
      OP(b, EMU_IR_OR, ANDK(b, G(b, R_XER), ~PPC_XER_CA), SHL(b, c01, 29u)));
}

/* CR bit `bi`, numbered from the left, as 0 or 1. */
static uint16_t crbit(B *b, uint32_t bi)
{
    return ANDK(b, SHR(b, G(b, R_CR), 31u - bi), 1u);
}

/* Write a four-bit value into CR field `f`. */
static void put_crf(B *b, uint32_t f, uint16_t v4)
{
    const uint32_t sh = 4u * (7u - f);

    P(b, R_CR, OP(b, EMU_IR_OR, ANDK(b, G(b, R_CR), ~(0xFu << sh)), SHL(b, v4, sh)));
}

/* LT, GT, EQ and SO from a compare: the value a CR field takes. */
static uint16_t cmp4(B *b, uint16_t x, uint16_t y, bool sgn)
{
    const uint16_t lt = CC(b, sgn ? EMU_IR_C_LT : EMU_IR_C_LTU, x, y);
    const uint16_t gt = CC(b, sgn ? EMU_IR_C_GT : EMU_IR_C_GTU, x, y);
    const uint16_t eq = CC(b, EMU_IR_C_EQ, x, y);

    return OP(b, EMU_IR_OR, OP(b, EMU_IR_OR, SHL(b, lt, 3u), SHL(b, gt, 2u)),
              OP(b, EMU_IR_OR, SHL(b, eq, 1u), so01(b)));
}

/* The record bit: the result against zero, into CR0. */
static void rc0(B *b, const I *d, uint16_t res)
{
    if (d->rc) {
        put_crf(b, 0u, cmp4(b, res, K(b, 0u), true));
    }
}

/* x + y + cin, and the carry out as 0 or 1. */
static uint16_t add3(B *b, uint16_t x, uint16_t y, uint16_t cin, uint16_t *ca)
{
    const uint16_t t1 = OP(b, EMU_IR_ADD, x, y);
    const uint16_t c1 = CC(b, EMU_IR_C_LTU, t1, x);
    const uint16_t res = OP(b, EMU_IR_ADD, t1, cin);
    const uint16_t c2 = CC(b, EMU_IR_C_LTU, res, t1);

    *ca = OP(b, EMU_IR_OR, c1, c2);
    return res;
}

/* rotl(x, n) for a constant n, on a host with or without the rotate. */
static uint16_t rotl_k(B *b, uint16_t x, uint32_t n)
{
    n &= 31u;
    if (n == 0u) {
        return x;
    }
    if (emu_ir_can_lower(EMU_IR_ROTLI, 0u)) {
        return OPI(b, EMU_IR_ROTLI, x, n);
    }
    return OP(b, EMU_IR_OR, SHL(b, x, n), SHR(b, x, 32u - n));
}

static uint32_t rl_mask(uint32_t mb, uint32_t me)
{
    const uint32_t a = 0xFFFFFFFFu >> mb;
    const uint32_t m = 0xFFFFFFFFu << (31u - me);

    return (mb <= me) ? (a & m) : (a | m);
}

/* ------------------------------------------------------------------ */
/* Leaving a block                                                     */
/* ------------------------------------------------------------------ */

static void exit_const(T *t, uint32_t target)
{
    retire(t);
    (void)emu_ir_emit(t->b, EMU_IR_EXIT, 0u, NOT, NOT, target, 0u);
    t->ends = true;
}

static void exit_value(T *t, uint16_t target)
{
    if (!t->counted) {
        retire(t);
    }
    (void)emu_ir_emit(t->b, EMU_IR_EXIT, 0u, target, NOT, 0u, 0u);
    t->ends = true;
}

/* Leave if a 0/1 value is set; fall through otherwise. */
static void exit_if(T *t, uint16_t c01, uint32_t target)
{
    retire(t);
    (void)emu_ir_emit(t->b, EMU_IR_EXIT_IF, EMU_IR_C_NE, c01, K(t->b, 0u), target,
                      0u);
}

/*
 * BO as a 0/1 value, or NOT for "always". Decrements CTR if BO asks,
 * which is why this is called exactly once per branch and after
 * anything that reads CTR as a target.
 */
static uint16_t bo_cond(T *t, uint32_t bo, uint32_t bi)
{
    B *const b = t->b;
    uint16_t cond = NOT;

    if ((bo & 0x04u) == 0u) {
        const uint16_t ctr = OP(b, EMU_IR_SUB, G(b, R_CTR), K(b, 1u));

        P(b, R_CTR, ctr);
        cond = CC(b, ((bo & 0x02u) != 0u) ? EMU_IR_C_EQ : EMU_IR_C_NE, ctr, K(b, 0u));
    }
    if ((bo & 0x10u) == 0u) {
        uint16_t bit = crbit(b, bi);

        if ((bo & 0x08u) == 0u) {
            bit = XORK(b, bit, 1u);
        }
        cond = (cond == NOT) ? bit : OP(b, EMU_IR_AND, cond, bit);
    }
    return cond;
}

/* ------------------------------------------------------------------ */
/* Lowering                                                            */
/* ------------------------------------------------------------------ */

static bool lower_arith(T *t, const I *d)
{
    B *const b = t->b;
    uint16_t res;
    uint16_t ca = NOT;

    /* XER[OV] and SO: the compiler never asks, so the interpreter may. */
    if (d->oe) {
        return false;
    }
    switch (d->sem) {
    case PPC_S_ADD:
        res = OP(b, EMU_IR_ADD, G(b, d->ra), G(b, d->rb));
        break;
    case PPC_S_ADDC: {
        const uint16_t x = G(b, d->ra);

        res = OP(b, EMU_IR_ADD, x, G(b, d->rb));
        ca = CC(b, EMU_IR_C_LTU, res, x);
        break;
    }
    case PPC_S_ADDE:
        res = add3(b, G(b, d->ra), G(b, d->rb), ca01(b), &ca);
        break;
    case PPC_S_ADDME:
        res = add3(b, G(b, d->ra), K(b, 0xFFFFFFFFu), ca01(b), &ca);
        break;
    case PPC_S_ADDZE: {
        const uint16_t x = G(b, d->ra);

        res = OP(b, EMU_IR_ADD, x, ca01(b));
        ca = CC(b, EMU_IR_C_LTU, res, x);
        break;
    }
    case PPC_S_SUBF:
        res = OP(b, EMU_IR_SUB, G(b, d->rb), G(b, d->ra));
        break;
    case PPC_S_SUBFC: {
        /* ~a + b + 1 carries exactly when b >= a. */
        const uint16_t x = G(b, d->ra);
        const uint16_t y = G(b, d->rb);

        res = OP(b, EMU_IR_SUB, y, x);
        ca = CC(b, EMU_IR_C_GEU, y, x);
        break;
    }
    case PPC_S_SUBFE:
        res = add3(b, INV(b, G(b, d->ra)), G(b, d->rb), ca01(b), &ca);
        break;
    case PPC_S_SUBFME:
        res = add3(b, INV(b, G(b, d->ra)), K(b, 0xFFFFFFFFu), ca01(b), &ca);
        break;
    case PPC_S_SUBFZE: {
        const uint16_t x = INV(b, G(b, d->ra));

        res = OP(b, EMU_IR_ADD, x, ca01(b));
        ca = CC(b, EMU_IR_C_LTU, res, x);
        break;
    }
    case PPC_S_NEG:
        res = U(b, EMU_IR_NEG, G(b, d->ra));
        break;
    case PPC_S_MULLW:
        res = OP(b, EMU_IR_MUL, G(b, d->ra), G(b, d->rb));
        break;
    case PPC_S_MULHW:
    case PPC_S_MULHWU: {
        const emu_ir_op_t op = (d->sem == PPC_S_MULHW) ? EMU_IR_MULHS : EMU_IR_MULHU;

        if (!emu_ir_can_lower(op, 0u)) {
            return false;
        }
        res = OP(b, op, G(b, d->ra), G(b, d->rb));
        break;
    }
    case PPC_S_DIVW:
    case PPC_S_DIVWU: {
        /*
         * The IR's divide has the host's corners: it must not be given
         * a zero divisor, nor INT_MIN / -1. This core leaves rD
         * undefined for both and the interpreter writes zero, so the
         * divisor is made 1 wherever the host would trap and the
         * quotient cleared in the same places.
         */
        const bool sgn = d->sem == PPC_S_DIVW;
        const emu_ir_op_t op = sgn ? EMU_IR_DIVS : EMU_IR_DIVU;
        uint16_t n;
        uint16_t m;
        uint16_t bad;
        uint16_t keep;

        if (!emu_ir_can_lower(op, 0u)) {
            return false;
        }
        n = G(b, d->ra);
        m = G(b, d->rb);
        bad = CC(b, EMU_IR_C_EQ, m, K(b, 0u));
        if (sgn) {
            bad = OP(b, EMU_IR_OR, bad,
                     OP(b, EMU_IR_AND, CC(b, EMU_IR_C_EQ, n, K(b, 0x80000000u)),
                        CC(b, EMU_IR_C_EQ, m, K(b, 0xFFFFFFFFu))));
        }
        keep = OP(b, EMU_IR_SUB, bad, K(b, 1u)); /* ~0 where it is fine */
        m = OP(b, EMU_IR_OR, OP(b, EMU_IR_AND, m, keep), bad);
        res = OP(b, EMU_IR_AND, OP(b, op, n, m), keep);
        break;
    }
    case PPC_S_ADDI:
        res = ADDK(b, base_of(t, d), d->imm);
        if (res == NOT) {
            return false;
        }
        break;
    case PPC_S_ADDIC: {
        const uint16_t x = G(b, d->ra);

        res = OP(b, EMU_IR_ADD, x, K(b, d->imm));
        ca = CC(b, EMU_IR_C_LTU, res, x);
        break;
    }
    case PPC_S_SUBFIC: {
        const uint16_t x = G(b, d->ra);
        const uint16_t k = K(b, d->imm);

        res = OP(b, EMU_IR_SUB, k, x);
        ca = CC(b, EMU_IR_C_GEU, k, x);
        break;
    }
    case PPC_S_MULLI:
        res = OP(b, EMU_IR_MUL, G(b, d->ra), K(b, d->imm));
        break;
    default:
        return false;
    }
    if (ca != NOT) {
        put_ca(b, ca);
    }
    P(b, d->rd, res);
    rc0(b, d, res);
    return true;
}

/* rA = f(rS, rB) or f(rS, imm): the destination is the `ra` field. */
static bool lower_logic(T *t, const I *d)
{
    B *const b = t->b;
    const uint16_t s = G(b, d->rd);
    uint16_t res;

    switch (d->sem) {
    case PPC_S_AND:
        res = OP(b, EMU_IR_AND, s, G(b, d->rb));
        break;
    case PPC_S_ANDC:
        res = OP(b, EMU_IR_AND, s, INV(b, G(b, d->rb)));
        break;
    case PPC_S_OR:
        /* `or rA, rS, rS` is how Book E spells a register move. */
        res = (d->rd == d->rb) ? s : OP(b, EMU_IR_OR, s, G(b, d->rb));
        break;
    case PPC_S_ORC:
        res = OP(b, EMU_IR_OR, s, INV(b, G(b, d->rb)));
        break;
    case PPC_S_XOR:
        res = OP(b, EMU_IR_XOR, s, G(b, d->rb));
        break;
    case PPC_S_NAND:
        res = INV(b, OP(b, EMU_IR_AND, s, G(b, d->rb)));
        break;
    case PPC_S_NOR:
        res = INV(b, (d->rd == d->rb) ? s : OP(b, EMU_IR_OR, s, G(b, d->rb)));
        break;
    case PPC_S_EQV:
        res = INV(b, OP(b, EMU_IR_XOR, s, G(b, d->rb)));
        break;
    case PPC_S_ANDI:
        res = ANDK(b, s, d->imm);
        break;
    case PPC_S_ORI:
        res = (d->imm == 0u) ? s : OP(b, EMU_IR_OR, s, K(b, d->imm));
        break;
    case PPC_S_XORI:
        res = (d->imm == 0u) ? s : XORK(b, s, d->imm);
        break;
    case PPC_S_EXTSB:
        res = U(b, EMU_IR_SEXT8, s);
        break;
    case PPC_S_EXTSH:
        res = U(b, EMU_IR_SEXT16, s);
        break;
    case PPC_S_EXTZB:
        res = ANDK(b, s, 0xFFu);
        break;
    case PPC_S_EXTZH:
        res = ANDK(b, s, 0xFFFFu);
        break;
    case PPC_S_CNTLZW:
        if (!emu_ir_can_lower(EMU_IR_CLZ, 0u)) {
            return false;
        }
        res = U(b, EMU_IR_CLZ, s);
        break;
    default:
        return false;
    }
    P(b, d->ra, res);
    rc0(b, d, res);
    return true;
}

static bool lower_shift(T *t, const I *d)
{
    B *const b = t->b;
    const uint16_t s = G(b, d->rd);
    uint16_t res;

    switch (d->sem) {
    case PPC_S_SLW:
    case PPC_S_SRW: {
        /* Six bits of rB: 32 and up shift everything out. */
        const uint16_t n = ANDK(b, G(b, d->rb), 0x3Fu);
        const uint16_t m32 =
            OP(b, EMU_IR_SUB, K(b, 0u), CC(b, EMU_IR_C_LTU, n, K(b, 32u)));

        res = OP(b, EMU_IR_AND,
                 OP(b, (d->sem == PPC_S_SLW) ? EMU_IR_SHL : EMU_IR_SHR, s,
                    ANDK(b, n, 31u)),
                 m32);
        break;
    }
    case PPC_S_SRAWI: {
        const uint32_t n = d->sh;

        if (n == 0u) {
            res = s;
            put_ca(b, K(b, 0u));
        } else {
            /* CA: negative, and a 1 bit shifted out. */
            res = OPI(b, EMU_IR_SARI, s, n);
            put_ca(b, OP(b, EMU_IR_AND, SHR(b, s, 31u),
                         CC(b, EMU_IR_C_NE, ANDK(b, s, (1u << n) - 1u), K(b, 0u))));
        }
        break;
    }
    case PPC_S_SRAW: {
        /*
         * The count held at 31 from 32 up, which gives the all-sign
         * result; and the bits shifted out are then all of them.
         */
        const uint16_t n = ANDK(b, G(b, d->rb), 0x3Fu);
        const uint16_t m32 =
            OP(b, EMU_IR_SUB, K(b, 0u), CC(b, EMU_IR_C_LTU, n, K(b, 32u)));
        const uint16_t big = INV(b, m32);
        const uint16_t amt =
            OP(b, EMU_IR_OR, OP(b, EMU_IR_AND, n, m32), ANDK(b, big, 31u));
        const uint16_t lost = OP(
            b, EMU_IR_OR,
            INV(b, OP(b, EMU_IR_SHL, K(b, 0xFFFFFFFFu), ANDK(b, n, 31u))), big);

        res = OP(b, EMU_IR_SAR, s, amt);
        put_ca(b, OP(b, EMU_IR_AND, SHR(b, s, 31u),
                     CC(b, EMU_IR_C_NE, OP(b, EMU_IR_AND, s, lost), K(b, 0u))));
        break;
    }
    case PPC_S_RLWINM: {
        const uint32_t m = rl_mask(d->mb, d->me);
        const uint32_t sh = d->sh & 31u;

        /* The two shifts hide in here; say so, for hosts with no rotate. */
        if (sh != 0u && d->mb == 0u && d->me == 31u - sh) {
            res = SHL(b, s, sh);
        } else if (sh != 0u && d->me == 31u && d->mb == 32u - sh) {
            res = SHR(b, s, 32u - sh);
        } else {
            res = ANDK(b, rotl_k(b, s, sh), m);
        }
        break;
    }
    case PPC_S_RLWIMI: {
        const uint32_t m = rl_mask(d->mb, d->me);

        res = OP(b, EMU_IR_OR, ANDK(b, rotl_k(b, s, d->sh), m),
                 ANDK(b, G(b, d->ra), ~m));
        break;
    }
    case PPC_S_RLWNM: {
        const uint16_t n = ANDK(b, G(b, d->rb), 31u);
        uint16_t r;

        if (emu_ir_can_lower(EMU_IR_ROTL, 0u)) {
            r = OP(b, EMU_IR_ROTL, s, n);
        } else {
            /* (32 - n) & 31 keeps the n = 0 case a shift by zero. */
            r = OP(b, EMU_IR_OR, OP(b, EMU_IR_SHL, s, n),
                   OP(b, EMU_IR_SHR, s,
                      ANDK(b, OP(b, EMU_IR_SUB, K(b, 32u), n), 31u)));
        }
        res = ANDK(b, r, rl_mask(d->mb, d->me));
        break;
    }
    default:
        return false;
    }
    P(b, d->ra, res);
    rc0(b, d, res);
    return true;
}

static bool cmp_reg(const I *d)
{
    const uint32_t f = ppc_mn_format(d->id);

    return f == PPC_F_A_B || f == PPC_F_CRF_A_B;
}

static bool lower_cr(T *t, const I *d)
{
    B *const b = t->b;

    switch (d->sem) {
    case PPC_S_CMP:
    case PPC_S_CMPL:
        put_crf(b, d->crf,
                cmp4(b, G(b, d->ra), cmp_reg(d) ? G(b, d->rb) : K(b, d->imm),
                     d->sem == PPC_S_CMP));
        return true;
    case PPC_S_CMPH:
        put_crf(b, d->crf,
                cmp4(b, U(b, EMU_IR_SEXT16, G(b, d->ra)),
                     cmp_reg(d) ? U(b, EMU_IR_SEXT16, G(b, d->rb)) : K(b, d->imm),
                     true));
        return true;
    case PPC_S_CMPHL:
        put_crf(b, d->crf,
                cmp4(b, ANDK(b, G(b, d->ra), 0xFFFFu),
                     cmp_reg(d) ? ANDK(b, G(b, d->rb), 0xFFFFu) : K(b, d->imm),
                     false));
        return true;
    case PPC_S_BTST: {
        /* GT if the bit is set, EQ if not, and SO. */
        const uint16_t v =
            CC(b, EMU_IR_C_NE, ANDK(b, G(b, d->ra), d->imm), K(b, 0u));

        put_crf(b, 0u,
                OP(b, EMU_IR_OR,
                   OP(b, EMU_IR_OR, SHL(b, v, 2u), SHL(b, XORK(b, v, 1u), 1u)),
                   so01(b)));
        return true;
    }
    case PPC_S_CRAND:
    case PPC_S_CRANDC:
    case PPC_S_CREQV:
    case PPC_S_CRNAND:
    case PPC_S_CRNOR:
    case PPC_S_CROR:
    case PPC_S_CRORC:
    case PPC_S_CRXOR: {
        const uint32_t sh = 31u - d->rd;
        uint16_t v;

        if (d->ra == d->rb && (d->sem == PPC_S_CRXOR || d->sem == PPC_S_CRANDC)) {
            v = K(b, 0u); /* crclr, as a variadic call's prologue has it */
        } else if (d->ra == d->rb && (d->sem == PPC_S_CREQV || d->sem == PPC_S_CRORC)) {
            v = K(b, 1u); /* crset */
        } else {
            const uint16_t x = crbit(b, d->ra);
            const uint16_t y = crbit(b, d->rb);

            switch (d->sem) {
            case PPC_S_CRAND:
                v = OP(b, EMU_IR_AND, x, y);
                break;
            case PPC_S_CRANDC:
                v = OP(b, EMU_IR_AND, x, XORK(b, y, 1u));
                break;
            case PPC_S_CREQV:
                v = XORK(b, OP(b, EMU_IR_XOR, x, y), 1u);
                break;
            case PPC_S_CRNAND:
                v = XORK(b, OP(b, EMU_IR_AND, x, y), 1u);
                break;
            case PPC_S_CRNOR:
                v = XORK(b, OP(b, EMU_IR_OR, x, y), 1u);
                break;
            case PPC_S_CROR:
                v = OP(b, EMU_IR_OR, x, y);
                break;
            case PPC_S_CRORC:
                v = OP(b, EMU_IR_OR, x, XORK(b, y, 1u));
                break;
            default:
                v = OP(b, EMU_IR_XOR, x, y);
                break;
            }
        }
        P(b, R_CR, OP(b, EMU_IR_OR, ANDK(b, G(b, R_CR), ~(1u << sh)), SHL(b, v, sh)));
        return true;
    }
    case PPC_S_MCRF:
        put_crf(b, d->crf, ANDK(b, SHR(b, G(b, R_CR), 4u * (7u - d->crs)), 15u));
        return true;
    case PPC_S_MFCR:
        P(b, d->rd, G(b, R_CR));
        return true;
    case PPC_S_MTCRF: {
        uint32_t m = 0u;

        for (uint32_t i = 0u; i < 8u; i++) {
            if (((d->imm >> (7u - i)) & 1u) != 0u) {
                m |= 0xFu << (4u * (7u - i));
            }
        }
        if (m == 0xFFFFFFFFu) {
            P(b, R_CR, G(b, d->rd));
        } else {
            P(b, R_CR,
              OP(b, EMU_IR_OR, ANDK(b, G(b, R_CR), ~m), ANDK(b, G(b, d->rd), m)));
        }
        return true;
    }
    case PPC_S_ISEL: {
        /* Branch-free: rB, with rA's bits swapped in where the bit is set. */
        const uint16_t mask = OP(b, EMU_IR_SUB, K(b, 0u), crbit(b, d->bi));
        const uint16_t x = base_of(t, d);
        const uint16_t y = G(b, d->rb);

        P(b, d->rd, OP(b, EMU_IR_XOR, y, OP(b, EMU_IR_AND, OP(b, EMU_IR_XOR, x, y), mask)));
        return true;
    }
    default:
        return false;
    }
}

static bool lower_mem(T *t, const I *d)
{
    B *const b = t->b;
    const uint8_t spec = EMU_IR_MEM_AUX(d->size, 0u);
    const emu_ir_op_t sw = (d->size == 4u) ? EMU_IR_BSWAP32 : EMU_IR_BSWAP16;
    uint16_t addr;
    uint32_t disp = 0u;

    /* A swap the host cannot do is a load the interpreter does. */
    if (d->size != 1u && !d->rev && !emu_ir_can_lower(sw, 0u)) {
        return false;
    }
    addr = base_of(t, d);
    if (d->idx) {
        addr = OP(b, EMU_IR_ADD, addr, G(b, d->rb));
    } else {
        disp = d->imm;
    }

    if (d->sem == PPC_S_LOAD) {
        uint16_t v = emu_ir_emit(b, EMU_IR_LOAD, spec, addr, NOT, disp, 0u);

        if (d->size != 1u && !d->rev) {
            v = U(b, sw, v);
        }
        if (d->sext) {
            v = U(b, EMU_IR_SEXT16, v);
        }
        /*
         * 3.16.1, as the interpreter has it: rA takes the address and
         * then rD the data, so where they are one register the data is
         * what is left. Both after the load, which may not complete.
         */
        if (d->upd) {
            P(b, d->ra, ADDK(b, addr, disp));
        }
        P(b, d->rd, v);
        return true;
    }
    {
        uint16_t v = G(b, d->rd);

        if (d->size != 1u && !d->rev) {
            v = U(b, sw, v);
        }
        (void)emu_ir_emit(b, EMU_IR_STORE, spec, addr, v, disp, 0u);
        if (d->upd) {
            P(b, d->ra, ADDK(b, addr, disp));
        }
    }
    return true;
}

static bool lower_branch(T *t, const I *d)
{
    B *const b = t->b;
    const uint32_t mask = t->c->vle ? ~1u : ~3u;

    switch (d->sem) {
    case PPC_S_B:
        if (d->lk) {
            P(b, R_LR, K(b, t->next));
        }
        exit_const(t, (d->aa ? 0u : t->pc) + d->imm);
        return true;
    case PPC_S_BC: {
        const uint16_t cond = bo_cond(t, d->bo, d->bi);
        const uint32_t target = (d->aa ? 0u : t->pc) + d->imm;

        if (d->lk) {
            P(b, R_LR, K(b, t->next));
        }
        if (cond == NOT) {
            exit_const(t, target);
        } else {
            exit_if(t, cond, target);
        }
        return true;
    }
    case PPC_S_BCLR:
    case PPC_S_BCCTR: {
        /*
         * The target is read first: bclrl writes LR, and a bcctr that
         * decrements (3.16.3) branches to the value CTR had before.
         */
        const uint16_t tgt =
            ANDK(b, G(b, (d->sem == PPC_S_BCLR) ? R_LR : R_CTR), mask);
        const uint16_t cond = bo_cond(t, d->bo, d->bi);

        if (d->lk) {
            P(b, R_LR, K(b, t->next));
        }
        if (cond != NOT) {
            /* Not taken leaves for the next instruction; a conditional
             * exit's target is a constant, and this one's is not. */
            exit_if(t, XORK(b, cond, 1u), t->next);
        }
        exit_value(t, tgt);
        return true;
    }
    default:
        return false;
    }
}

static bool lower_native(T *t, const I *d)
{
    B *const b = t->b;

    switch ((ppc_sem_t)d->sem) {
    case PPC_S_ADD:
    case PPC_S_ADDC:
    case PPC_S_ADDE:
    case PPC_S_ADDME:
    case PPC_S_ADDZE:
    case PPC_S_SUBF:
    case PPC_S_SUBFC:
    case PPC_S_SUBFE:
    case PPC_S_SUBFME:
    case PPC_S_SUBFZE:
    case PPC_S_NEG:
    case PPC_S_MULLW:
    case PPC_S_MULHW:
    case PPC_S_MULHWU:
    case PPC_S_DIVW:
    case PPC_S_DIVWU:
    case PPC_S_ADDI:
    case PPC_S_ADDIC:
    case PPC_S_SUBFIC:
    case PPC_S_MULLI:
        return lower_arith(t, d);
    case PPC_S_LI:
        P(b, d->rd, K(b, d->imm));
        return true;
    case PPC_S_MR:
        P(b, d->rd, G(b, d->ra));
        return true;
    case PPC_S_AND:
    case PPC_S_ANDC:
    case PPC_S_OR:
    case PPC_S_ORC:
    case PPC_S_XOR:
    case PPC_S_NAND:
    case PPC_S_NOR:
    case PPC_S_EQV:
    case PPC_S_ANDI:
    case PPC_S_ORI:
    case PPC_S_XORI:
    case PPC_S_EXTSB:
    case PPC_S_EXTSH:
    case PPC_S_EXTZB:
    case PPC_S_EXTZH:
    case PPC_S_CNTLZW:
        return lower_logic(t, d);
    case PPC_S_SLW:
    case PPC_S_SRW:
    case PPC_S_SRAW:
    case PPC_S_SRAWI:
    case PPC_S_RLWINM:
    case PPC_S_RLWIMI:
    case PPC_S_RLWNM:
        return lower_shift(t, d);
    case PPC_S_CMP:
    case PPC_S_CMPL:
    case PPC_S_CMPH:
    case PPC_S_CMPHL:
    case PPC_S_BTST:
    case PPC_S_CRAND:
    case PPC_S_CRANDC:
    case PPC_S_CREQV:
    case PPC_S_CRNAND:
    case PPC_S_CRNOR:
    case PPC_S_CROR:
    case PPC_S_CRORC:
    case PPC_S_CRXOR:
    case PPC_S_MCRF:
    case PPC_S_MFCR:
    case PPC_S_MTCRF:
    case PPC_S_ISEL:
        return lower_cr(t, d);
    case PPC_S_LOAD:
    case PPC_S_STORE:
        return lower_mem(t, d);
    case PPC_S_B:
    case PPC_S_BC:
    case PPC_S_BCLR:
    case PPC_S_BCCTR:
        return lower_branch(t, d);
    case PPC_S_MFSPR:
        /* The three a compiler touches; the rest end the block. */
        switch (d->imm) {
        case PPC_SPR_LR:
            P(b, d->rd, G(b, R_LR));
            return true;
        case PPC_SPR_CTR:
            P(b, d->rd, G(b, R_CTR));
            return true;
        case PPC_SPR_XER:
            P(b, d->rd, G(b, R_XER));
            return true;
        default:
            return false;
        }
    case PPC_S_MTSPR:
        switch (d->imm) {
        case PPC_SPR_LR:
            P(b, R_LR, G(b, d->rd));
            return true;
        case PPC_SPR_CTR:
            P(b, R_CTR, G(b, d->rd));
            return true;
        case PPC_SPR_XER:
            P(b, R_XER, ANDK(b, G(b, d->rd), PPC_XER_IMPL));
            return true;
        default:
            return false;
        }
    case PPC_S_NOP:
        return true; /* the barriers and the cache hints; icbi is declined */
    default:
        return false;
    }
}

/*
 * What ends a block rather than joining one: the interpreter runs it,
 * from the dispatcher, with the block's instruction count already
 * handed over. Everything that changes MSR is here, so an interrupt that
 * has just been enabled is taken before the next instruction and the
 * translator's context is re-derived in one place; so is everything
 * that reads a timer, because a block reports how many instructions it
 * ran only when it leaves, and a time base read from inside one would
 * be short by the block so far.
 */
static bool must_decline(const I *d)
{
    switch ((ppc_sem_t)d->sem) {
    case PPC_S_SC:
    case PPC_S_RFI:
    case PPC_S_RFCI:
    case PPC_S_RFDI:
    case PPC_S_RFMCI:
    case PPC_S_WAIT:
    case PPC_S_MFMSR:
    case PPC_S_MTMSR:
    case PPC_S_WRTEE:
    case PPC_S_PRIV_NOP:
    case PPC_S_ILLEGAL:
    case PPC_S_PRIV_ILLEGAL:
        return true;
    case PPC_S_MFSPR:
    case PPC_S_MTSPR:
        return d->imm != PPC_SPR_LR && d->imm != PPC_SPR_CTR && d->imm != PPC_SPR_XER;
    case PPC_S_NOP:
        /* "I rewrote code": the translations of it have to go, and the
         * dispatcher is where that can happen. */
        return d->id == PPC_M_ICBI;
    default:
        return false;
    }
}

/* The interpreter, for this instruction, from inside the block. */
static void lower_helper(T *t)
{
    B *const b = t->b;

    (void)emu_ir_emit(b, EMU_IR_HELPER_TRAP, 0u, K(b, t->insn), K(b, t->pc),
                      PPC_HELPER_STEP, 0u);
}

static bool fetch(const ppc_cpu_t *c, uint32_t pc, uint32_t *insn, uint32_t *len)
{
    uint16_t w0;
    uint16_t w1 = 0u;

    if (emu_bus_fetch16(c->bus, pc, &w0) != EMU_FAULT_NONE) {
        return false;
    }
    *len = c->vle ? ppc_vle_len(w0) : 4u;
    if (*len == 4u && emu_bus_fetch16(c->bus, pc + 2u, &w1) != EMU_FAULT_NONE) {
        return false;
    }
    *insn = ((uint32_t)w0 << 16) | w1;
    return true;
}

uint32_t ppc_ir_translate(emu_cpu_t *cpu, uint32_t pc, emu_ir_block_t *b)
{
    ppc_cpu_t *const c = (ppc_cpu_t *)cpu;
    uint32_t cur = pc;
    uint32_t count = 0u;

    emu_ir_reset(b);
    b->start_pc = pc;
    b->has_fast = ppc_fast_mem(c, &b->fast);

    while (count < PPC_MAX_BLOCK_INSNS && !b->overflow &&
           b->count + PPC_IR_HEADROOM <= EMU_IR_MAX_INSNS &&
           b->next_temp + PPC_TEMP_HEADROOM <= EMU_IR_MAX_TEMPS) {
        uint32_t insn;
        uint32_t len;
        I d;
        T t;

        if (!fetch(c, cur, &insn, &len)) {
            break;
        }
        ppc_decode(insn, len, c->vle, &d);
        if (must_decline(&d)) {
            g_stats.declined++;
            g_stats.declined_by_sem[d.sem]++;
            break;
        }

        memset(&t, 0, sizeof(t));
        t.b = b;
        t.c = c;
        t.pc = cur;
        t.next = cur + len;
        t.insn = insn;
        {
            const uint32_t mark = b->count;
            const uint32_t mark_temp = b->next_temp;

            if (lower_native(&t, &d)) {
                g_stats.native++;
            } else {
                /* Whatever the attempt emitted, discard: a half-lowered
                 * instruction does not fail, it computes something else. */
                b->count = mark;
                b->next_temp = mark_temp;
                t.ends = false;
                t.counted = false;
                lower_helper(&t);
                g_stats.helper++;
                g_stats.helper_by_sem[d.sem]++;
            }
        }

        cur += len;
        count++;
        if (!t.counted) {
            (void)emu_ir_emit(b, EMU_IR_RETIRE, 0u, NOT, NOT, 0u, 0u);
        }
        if (t.ends) {
            break;
        }
        /*
         * pc after every instruction, so that a fault, an interrupt or
         * the interpreter picking up where the block stopped sees the
         * right address -- and so that the *next* instruction's fault
         * records its own.
         */
        (void)emu_ir_emit(b, EMU_IR_SETPC, 0u, NOT, NOT, cur, 0u);
    }

    if (b->overflow || count == 0u) {
        return 0u;
    }
    b->guest_insns = count;
    return count;
}

/* ------------------------------------------------------------------ */
/* What the host's jit.c needs from this frontend                      */
/* ------------------------------------------------------------------ */

static bool ppc_jit_is_idle(emu_cpu_t *cpu)
{
    return ((ppc_cpu_t *)cpu)->state == EMU_STATE_WFI;
}

static bool ppc_jit_wake(emu_cpu_t *cpu)
{
    return ppc_cpu_wake((ppc_cpu_t *)cpu);
}

/*
 * An interrupt, between blocks -- the same function the interpreter
 * calls between instructions.
 */
static bool ppc_jit_take_irq(emu_cpu_t *cpu)
{
    ppc_cpu_t *const c = (ppc_cpu_t *)cpu;

    /* Cleared before the look, not after: a line raised by a host
     * interrupt during it must not be lost to the clear. */
    c->irq_dirty = false;
    return ppc_cpu_take_irq(c);
}

/*
 * The processor clock, at block granularity. A block reports how many
 * instructions it retired and they become cycles here; the timers are
 * given them only when that reaches the decrementer's expiry, which is
 * one compare for every block that does not. The cost is latency: the
 * interrupt arrives at the end of the chain of blocks that passed the
 * expiry rather than on the instruction, where the interpreter puts it.
 */
static void ppc_jit_count(emu_cpu_t *cpu, uint32_t n)
{
    ppc_cpu_t *const c = (ppc_cpu_t *)cpu;

    c->retired += n;
    c->cycles += n;
    if (c->cycles - c->clk_synced >= (uint64_t)ppc_cpu_clock_until(c)) {
        ppc_cpu_sync_clock(c);
    }
}

/* The interpreter ran: an icbi may have asked for the cache to go. */
static void ppc_jit_after_interp(emu_cpu_t *cpu)
{
    ppc_cpu_t *const c = (ppc_cpu_t *)cpu;

    if (c->jit_flush) {
        c->jit_flush = false;
        emu_jit_flush();
    }
    c->jit_ctx = ppc_cpu_ctx(c);
}

static void ppc_jit_bind(emu_cpu_t *cpu, emu_jit_hot_t *out)
{
    ppc_cpu_t *const c = (ppc_cpu_t *)cpu;

    c->jit_ctx = ppc_cpu_ctx(c);
    out->pc = &c->pc;
    out->state = (const uint8_t *)&c->state;
    out->generation = &c->jit_gen;
    out->context = &c->jit_ctx;
    out->irq_pending = &c->irq_dirty;
}

const emu_ir_frontend_t ppc_ir_frontend = {
    .name = "ppc",
    .translate = ppc_ir_translate,
    .target = &ppc_ir_target,
    .bind = ppc_jit_bind,
    .interp = &ppc_backend_interp,
    .is_idle = ppc_jit_is_idle,
    .wake = ppc_jit_wake,
    .take_irq = ppc_jit_take_irq,
    .count = ppc_jit_count,
    .after_interp = ppc_jit_after_interp,
    .code_bytes = EMU_HOST_JIT_CODE_BYTES,
    /* r0-r31, pc, and CR/XER/LR/CTR beyond the bus pointer between. */
    .diff_state_bytes = (uint32_t)offsetof(ppc_cpu_t, msr),
};

#endif /* EMU_HAVE_JIT */
