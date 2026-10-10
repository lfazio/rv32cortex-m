/* SPDX-License-Identifier: Apache-2.0 */
/*
 * ppc_interp.c - the e200z7 interpreter.
 *
 * Executes what ppc_decode() says an instruction is; it does not look at
 * encodings itself. It used to, and that inline decode was a second
 * description of the encoding space with nothing holding it to the
 * first: e_cmpi compared into CR0 whatever field it named, the SCI8
 * group's record forms never recorded, and se_btsti wrote the wrong CR
 * bits. One decoder, checked against binutils by tests/ppc-check, is
 * the fix for that whole class.
 *
 * ppc_exec() is the instruction semantics, and it is also what the
 * translator calls for an instruction it does not lower -- so a
 * translated block and the interpreter cannot disagree about anything
 * the block hands back.
 *
 * Everything not implemented raises a program interrupt, which is the
 * architecture's report. **A trap only reports if something catches
 * it**: a flat guest with IVPR and the IVORs still zero vectors to
 * address 0, which in a flat image is its own entry point -- so an
 * unimplemented instruction presents as a restart, not as a
 * diagnostic. Read the pc deltas in a trace: an instruction that
 * changes the pc without retiring is a trap.
 */

#include "ppc/ppc_cpu.h"
#include "ppc/ppc_decode.h"
#include "ppc/ppc_fpu.h"

#include <stdint.h>

typedef ppc_insn_t I;

/*
 * Entries in the interpreter's decode cache; a power of two.
 *
 * It is worth 35% of the interpreter on a host (CoreMark, 40
 * iterations: 397 ms without, 260 ms with) and 48 KiB of .bss at this
 * size -- which on a microcontroller is the guest's memory, so there it
 * is a sixteenth of that.
 */
#ifndef PPC_DECODE_CACHE
#if defined(__arm__)
#define PPC_DECODE_CACHE 64u
#else
#define PPC_DECODE_CACHE 1024u
#endif
#endif

/* ------------------------------------------------------------------ */
/* Condition register and XER                                          */
/* ------------------------------------------------------------------ */

/*
 * CR fields are numbered from the left: CR0 is bits 0:3, the *top*
 * nibble. So field n sits at shift 4 * (7 - n), and writing that as
 * 4 * n -- which is what the name CR0 suggests to a reader used to
 * little-endian bit numbering -- puts CR0 where CR7 belongs and makes
 * every conditional branch read the wrong field.
 */
static EMU_ALWAYS_INLINE void cr_set(ppc_cpu_t *c, uint32_t field, uint32_t v)
{
    const unsigned sh = 4u * (7u - field);

    c->cr = (c->cr & ~(0xFu << sh)) | ((v & 0xFu) << sh);
}

static EMU_ALWAYS_INLINE uint32_t cr_get(const ppc_cpu_t *c, uint32_t field)
{
    return (c->cr >> (4u * (7u - field))) & 0xFu;
}

/* CR bit `bi`, also from the left. */
static EMU_ALWAYS_INLINE uint32_t cr_bit(const ppc_cpu_t *c, uint32_t bi)
{
    return (c->cr >> (31u - bi)) & 1u;
}

static EMU_ALWAYS_INLINE uint32_t so(const ppc_cpu_t *c)
{
    return (c->xer & PPC_XER_SO) != 0u ? PPC_CR_SO : 0u;
}

static EMU_ALWAYS_INLINE void cr_compare(ppc_cpu_t *c, uint32_t field,
                                         uint32_t a, uint32_t b, bool sgn)
{
    uint32_t v;

    if (sgn) {
        v = ((int32_t)a < (int32_t)b)   ? PPC_CR_LT
            : ((int32_t)a > (int32_t)b) ? PPC_CR_GT
                                        : PPC_CR_EQ;
    } else {
        v = (a < b) ? PPC_CR_LT : ((a > b) ? PPC_CR_GT : PPC_CR_EQ);
    }
    cr_set(c, field, v | so(c));
}

/* The Rc bit: the result against zero, into CR0. */
static EMU_ALWAYS_INLINE void cr0(ppc_cpu_t *c, uint32_t res)
{
    cr_compare(c, 0u, res, 0u, true);
}

static EMU_ALWAYS_INLINE void set_ca(ppc_cpu_t *c, bool ca)
{
    c->xer = ca ? (c->xer | PPC_XER_CA) : (c->xer & ~PPC_XER_CA);
}

static EMU_ALWAYS_INLINE uint32_t get_ca(const ppc_cpu_t *c)
{
    return (c->xer & PPC_XER_CA) != 0u ? 1u : 0u;
}

/* OE: OV is this instruction's, SO is sticky. */
static EMU_ALWAYS_INLINE void set_ov(ppc_cpu_t *c, bool ov)
{
    c->xer = ov ? (c->xer | PPC_XER_OV | PPC_XER_SO) : (c->xer & ~PPC_XER_OV);
}

/* a + b + cin, with the carry out and the signed overflow. */
static EMU_ALWAYS_INLINE uint32_t add3(uint32_t a, uint32_t b, uint32_t cin,
                                       bool *ca, bool *ov)
{
    const uint64_t w = (uint64_t)a + b + cin;
    const uint32_t r = (uint32_t)w;

    *ca = (w >> 32) != 0u;
    *ov = (((a ^ r) & (b ^ r)) >> 31) != 0u;
    return r;
}

/* MASK(mb, me) as rlwinm builds it, wrapping when mb > me. */
static EMU_ALWAYS_INLINE uint32_t rl_mask(uint32_t mb, uint32_t me)
{
    const uint32_t a = 0xFFFFFFFFu >> mb;
    const uint32_t b = 0xFFFFFFFFu << (31u - me);

    return (mb <= me) ? (a & b) : (a | b);
}

static EMU_ALWAYS_INLINE uint32_t rotl(uint32_t v, uint32_t n)
{
    n &= 31u;
    return (n == 0u) ? v : ((v << n) | (v >> (32u - n)));
}

static EMU_ALWAYS_INLINE uint32_t bswap(uint32_t v, uint32_t size)
{
    if (size == 2u) {
        return ((v >> 8) & 0xFFu) | ((v & 0xFFu) << 8);
    }
    return (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24);
}

static EMU_ALWAYS_INLINE uint32_t clz32(uint32_t v)
{
    return (v == 0u) ? 32u : (uint32_t)__builtin_clz(v);
}

/* rA|0: the value 0 where the instruction says rA = 0 means zero. */
static EMU_ALWAYS_INLINE uint32_t base_of(const ppc_cpu_t *c, const I *d)
{
    return (d->ra0 && d->ra == 0u) ? 0u : c->r[d->ra];
}

static EMU_ALWAYS_INLINE uint32_t ea_of(const ppc_cpu_t *c, const I *d)
{
    return base_of(c, d) + (d->idx ? c->r[d->rb] : d->imm);
}

/* The register a compare takes its second operand from, or its immediate. */
static EMU_ALWAYS_INLINE bool cmp_reg(const I *d)
{
    const uint32_t f = ppc_mn_format(d->id);

    return f == PPC_F_A_B || f == PPC_F_CRF_A_B;
}

/* A branch target's low bits: one in VLE, two in Book E. */
static EMU_ALWAYS_INLINE uint32_t target_mask(const ppc_cpu_t *c)
{
    return c->vle ? ~1u : ~3u;
}

/*
 * BO, Book E's encoding, which the VLE branches are decoded into:
 * 0x10 ignores the condition, 0x08 is the value it must have, 0x04
 * leaves CTR alone, 0x02 branches on CTR reaching zero rather than not.
 * The low bit is a prediction hint and changes nothing.
 */
static EMU_ALWAYS_INLINE bool bo_taken(ppc_cpu_t *c, uint32_t bo, uint32_t bi)
{
    bool ctr_ok = true;

    if ((bo & 0x04u) == 0u) {
        c->ctr--;
        ctr_ok = ((bo & 0x02u) != 0u) ? (c->ctr == 0u) : (c->ctr != 0u);
    }
    return ctr_ok && ((bo & 0x10u) != 0u || cr_bit(c, bi) == ((bo >> 3) & 1u));
}

/* ------------------------------------------------------------------ */
/* Faults                                                              */
/* ------------------------------------------------------------------ */

static bool program(ppc_cpu_t *c, uint32_t pc, uint32_t esr)
{
    ppc_cpu_raise(c, PPC_IVOR_PROGRAM, pc, esr);
    return false;
}

static bool dsi(ppc_cpu_t *c, uint32_t pc, bool store)
{
    ppc_cpu_raise(c, PPC_IVOR_DATA_STORAGE, pc, store ? PPC_ESR_ST : 0u);
    return false;
}

static bool misaligned(ppc_cpu_t *c, uint32_t pc, uint32_t ea, bool store)
{
    c->dear = ea;
    ppc_cpu_raise(c, PPC_IVOR_ALIGNMENT, pc, store ? PPC_ESR_ST : 0u);
    return false;
}

static EMU_ALWAYS_INLINE bool user(const ppc_cpu_t *c)
{
    return (c->msr & PPC_MSR_PR) != 0u;
}

/* ------------------------------------------------------------------ */
/* The multiple and context-save transfers                             */
/* ------------------------------------------------------------------ */

/*
 * The volatile context save/restore registers (3.14), by the set
 * number the decoder put in `crs`: r0 and r3-r12, then CR/LR/CTR/XER,
 * then each save/restore pair.
 */
static uint32_t *lmv_reg(ppc_cpu_t *c, uint32_t set, uint32_t i)
{
    switch (set) {
    case 0u:
        return &c->r[(i == 0u) ? 0u : i + 2u];
    case 1u: {
        uint32_t *const k[4] = {&c->cr, &c->lr, &c->ctr, &c->xer};
        return k[i];
    }
    case 4u:
        return (i == 0u) ? &c->srr0 : &c->srr1;
    case 5u:
        return (i == 0u) ? &c->csrr0 : &c->csrr1;
    case 6u:
        return (i == 0u) ? &c->dsrr0 : &c->dsrr1;
    default:
        return (i == 0u) ? &c->mcsrr0 : &c->mcsrr1;
    }
}

static bool exec_multi(ppc_cpu_t *c, const I *d, uint32_t pc)
{
    const bool store = d->sem == PPC_S_STMW || d->sem == PPC_S_STMV;
    const bool vol = d->sem == PPC_S_LMV || d->sem == PPC_S_STMV;
    const uint32_t n = !vol ? 32u - d->rd : (d->crs == 0u) ? 11u : (d->crs == 1u) ? 4u : 2u;
    uint32_t ea = ea_of(c, d);

    /* The save/restore pairs are supervisor registers. */
    if (vol && d->crs >= 4u && user(c)) {
        return program(c, pc, PPC_ESR_PPR);
    }
    if ((ea & 3u) != 0u) {
        return misaligned(c, pc, ea, store);
    }
    /*
     * 3.16.2: the base is the value rA had when the instruction began,
     * even when the instruction loads over it -- computed once, above.
     */
    for (uint32_t i = 0u; i < n; i++, ea += 4u) {
        uint32_t *const r = vol ? lmv_reg(c, d->crs, i) : &c->r[d->rd + i];

        if (store) {
            if (ppc_store(c, ea, 4u, *r) != PPC_EXC_NONE) {
                return dsi(c, pc, true);
            }
        } else {
            uint32_t v;

            if (ppc_load(c, ea, 4u, false, &v) != PPC_EXC_NONE) {
                return dsi(c, pc, false);
            }
            *r = (vol && d->crs == 1u && i == 3u) ? (v & PPC_XER_IMPL) : v;
        }
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* One instruction                                                     */
/* ------------------------------------------------------------------ */

bool ppc_exec(ppc_cpu_t *c, const I *d, uint32_t pc)
{
    const uint32_t next = pc + d->len;
    uint32_t *const r = c->r;
    uint32_t res = 0u;
    bool ca = false;
    bool ov = false;

    switch ((ppc_sem_t)d->sem) {
    /* --- integer arithmetic, register --------------------------- */
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
    case PPC_S_NEG: {
        /*
         * Everything is a + b + carry-in. The subtracts add the
         * complement of rA; the "extended" forms take XER[CA] in;
         * ME and ZE replace rB with all ones and zero.
         */
        const uint32_t sem = d->sem;
        const bool sub = sem == PPC_S_SUBF || sem == PPC_S_SUBFC || sem == PPC_S_SUBFE ||
                         sem == PPC_S_SUBFME || sem == PPC_S_SUBFZE || sem == PPC_S_NEG;
        const uint32_t a = sub ? ~r[d->ra] : r[d->ra];
        const uint32_t b = (sem == PPC_S_ADDME || sem == PPC_S_SUBFME) ? 0xFFFFFFFFu
                           : (sem == PPC_S_ADDZE || sem == PPC_S_SUBFZE ||
                              sem == PPC_S_NEG)
                               ? 0u
                               : r[d->rb];
        const uint32_t cin = (sem == PPC_S_SUBF || sem == PPC_S_SUBFC || sem == PPC_S_NEG)
                                 ? 1u
                             : (sem == PPC_S_ADD || sem == PPC_S_ADDC) ? 0u
                                                                       : get_ca(c);

        res = add3(a, b, cin, &ca, &ov);
        if (sem != PPC_S_ADD && sem != PPC_S_SUBF && sem != PPC_S_NEG) {
            set_ca(c, ca);
        }
        if (d->oe) {
            set_ov(c, ov);
        }
        r[d->rd] = res;
        if (d->rc) {
            cr0(c, res);
        }
        break;
    }
    case PPC_S_MULLW: {
        const int64_t p = (int64_t)(int32_t)r[d->ra] * (int32_t)r[d->rb];

        res = (uint32_t)p;
        if (d->oe) {
            set_ov(c, p != (int64_t)(int32_t)res);
        }
        r[d->rd] = res;
        if (d->rc) {
            cr0(c, res);
        }
        break;
    }
    case PPC_S_MULHW:
        res = (uint32_t)(((int64_t)(int32_t)r[d->ra] * (int32_t)r[d->rb]) >> 32);
        r[d->rd] = res;
        if (d->rc) {
            cr0(c, res);
        }
        break;
    case PPC_S_MULHWU:
        res = (uint32_t)(((uint64_t)r[d->ra] * r[d->rb]) >> 32);
        r[d->rd] = res;
        if (d->rc) {
            cr0(c, res);
        }
        break;
    case PPC_S_DIVW:
    case PPC_S_DIVWU: {
        /*
         * Division by zero, and INT_MIN / -1 for divw, leave rD
         * undefined (Book E) and the e200 manual does not say what it
         * leaves. Zero, and OV when OE asks. They must not be executed
         * in C, where both are undefined behaviour and the second
         * raises SIGFPE on x86.
         */
        const uint32_t a = r[d->ra];
        const uint32_t b = r[d->rb];
        const bool bad = (b == 0u) || (d->sem == PPC_S_DIVW && a == 0x80000000u &&
                                       b == 0xFFFFFFFFu);

        res = bad ? 0u
              : (d->sem == PPC_S_DIVW) ? (uint32_t)((int32_t)a / (int32_t)b)
                                       : a / b;
        if (d->oe) {
            set_ov(c, bad);
        }
        r[d->rd] = res;
        if (d->rc) {
            cr0(c, res);
        }
        break;
    }

    /* --- integer arithmetic, immediate -------------------------- */
    case PPC_S_LI:
        r[d->rd] = d->imm;
        break;
    case PPC_S_ADDI:
        res = base_of(c, d) + d->imm;
        r[d->rd] = res;
        if (d->rc) {
            cr0(c, res);
        }
        break;
    case PPC_S_ADDIC:
        res = add3(r[d->ra], d->imm, 0u, &ca, &ov);
        set_ca(c, ca);
        r[d->rd] = res;
        if (d->rc) {
            cr0(c, res);
        }
        break;
    case PPC_S_SUBFIC:
        res = add3(~r[d->ra], d->imm, 1u, &ca, &ov);
        set_ca(c, ca);
        r[d->rd] = res;
        if (d->rc) {
            cr0(c, res);
        }
        break;
    case PPC_S_MULLI:
        res = r[d->ra] * d->imm;
        r[d->rd] = res;
        if (d->rc) {
            cr0(c, res);
        }
        break;

    /* --- logical: rA = rS op rB, or rS op imm ------------------- */
    case PPC_S_AND:
    case PPC_S_ANDC:
    case PPC_S_OR:
    case PPC_S_ORC:
    case PPC_S_XOR:
    case PPC_S_NAND:
    case PPC_S_NOR:
    case PPC_S_EQV: {
        const uint32_t s = r[d->rd];
        const uint32_t b = r[d->rb];

        switch (d->sem) {
        case PPC_S_AND:
            res = s & b;
            break;
        case PPC_S_ANDC:
            res = s & ~b;
            break;
        case PPC_S_OR:
            res = s | b;
            break;
        case PPC_S_ORC:
            res = s | ~b;
            break;
        case PPC_S_XOR:
            res = s ^ b;
            break;
        case PPC_S_NAND:
            res = ~(s & b);
            break;
        case PPC_S_NOR:
            res = ~(s | b);
            break;
        default:
            res = ~(s ^ b);
            break;
        }
        r[d->ra] = res;
        if (d->rc) {
            cr0(c, res);
        }
        break;
    }
    case PPC_S_ANDI:
    case PPC_S_ORI:
    case PPC_S_XORI:
        res = (d->sem == PPC_S_ANDI)  ? (r[d->rd] & d->imm)
              : (d->sem == PPC_S_ORI) ? (r[d->rd] | d->imm)
                                      : (r[d->rd] ^ d->imm);
        r[d->ra] = res;
        if (d->rc) {
            cr0(c, res);
        }
        break;
    case PPC_S_MR:
        r[d->rd] = r[d->ra];
        break;
    case PPC_S_EXTSB:
    case PPC_S_EXTSH:
    case PPC_S_EXTZB:
    case PPC_S_EXTZH:
    case PPC_S_CNTLZW: {
        const uint32_t s = r[d->rd];

        res = (d->sem == PPC_S_EXTSB)   ? (uint32_t)(int32_t)(int8_t)s
              : (d->sem == PPC_S_EXTSH) ? (uint32_t)(int32_t)(int16_t)s
              : (d->sem == PPC_S_EXTZB) ? (s & 0xFFu)
              : (d->sem == PPC_S_EXTZH) ? (s & 0xFFFFu)
                                        : clz32(s);
        r[d->ra] = res;
        if (d->rc) {
            cr0(c, res);
        }
        break;
    }

    /* --- shifts and rotates -------------------------------------- */
    case PPC_S_SLW:
    case PPC_S_SRW: {
        /* Six bits of rB: 32 to 63 shift everything out. */
        const uint32_t n = r[d->rb] & 0x3Fu;

        res = (n >= 32u) ? 0u : (d->sem == PPC_S_SLW) ? (r[d->rd] << n) : (r[d->rd] >> n);
        r[d->ra] = res;
        if (d->rc) {
            cr0(c, res);
        }
        break;
    }
    case PPC_S_SRAW:
    case PPC_S_SRAWI: {
        /*
         * CA is set when the value is negative and any 1 bit is shifted
         * out -- the information a rounding-toward-zero divide needs. A
         * shift of 32 or more shifts every bit out.
         */
        const uint32_t n = (d->sem == PPC_S_SRAW) ? (r[d->rb] & 0x3Fu) : d->sh;
        const uint32_t s = r[d->rd];
        const bool neg = (s & 0x80000000u) != 0u;

        if (n >= 32u) {
            res = neg ? 0xFFFFFFFFu : 0u;
            set_ca(c, neg);
        } else {
            res = (uint32_t)((int32_t)s >> n);
            set_ca(c, neg && n != 0u && (s & ((1u << n) - 1u)) != 0u);
        }
        r[d->ra] = res;
        if (d->rc) {
            cr0(c, res);
        }
        break;
    }
    case PPC_S_RLWINM:
    case PPC_S_RLWNM:
    case PPC_S_RLWIMI: {
        const uint32_t n = (d->sem == PPC_S_RLWNM) ? (r[d->rb] & 31u) : d->sh;
        const uint32_t m = rl_mask(d->mb, d->me);
        const uint32_t v = rotl(r[d->rd], n);

        res = (d->sem == PPC_S_RLWIMI) ? ((v & m) | (r[d->ra] & ~m)) : (v & m);
        r[d->ra] = res;
        if (d->rc) {
            cr0(c, res);
        }
        break;
    }
    case PPC_S_BTST:
        /* se_btsti: GT if the bit is set, EQ if not, and SO. */
        cr_set(c, 0u, (((r[d->ra] & d->imm) != 0u) ? PPC_CR_GT : PPC_CR_EQ) | so(c));
        break;

    /* --- compares ------------------------------------------------- */
    case PPC_S_CMP:
    case PPC_S_CMPL:
        cr_compare(c, d->crf, r[d->ra], cmp_reg(d) ? r[d->rb] : d->imm,
                   d->sem == PPC_S_CMP);
        break;
    case PPC_S_CMPH:
        cr_compare(c, d->crf, (uint32_t)(int32_t)(int16_t)r[d->ra],
                   cmp_reg(d) ? (uint32_t)(int32_t)(int16_t)r[d->rb] : d->imm, true);
        break;
    case PPC_S_CMPHL:
        cr_compare(c, d->crf, r[d->ra] & 0xFFFFu,
                   cmp_reg(d) ? (r[d->rb] & 0xFFFFu) : d->imm, false);
        break;

    /* --- the condition register ----------------------------------- */
    case PPC_S_CRAND:
    case PPC_S_CRANDC:
    case PPC_S_CREQV:
    case PPC_S_CRNAND:
    case PPC_S_CRNOR:
    case PPC_S_CROR:
    case PPC_S_CRORC:
    case PPC_S_CRXOR: {
        const uint32_t a = cr_bit(c, d->ra);
        const uint32_t b = cr_bit(c, d->rb);
        uint32_t t;

        switch (d->sem) {
        case PPC_S_CRAND:
            t = a & b;
            break;
        case PPC_S_CRANDC:
            t = a & (b ^ 1u);
            break;
        case PPC_S_CREQV:
            t = (a ^ b) ^ 1u;
            break;
        case PPC_S_CRNAND:
            t = (a & b) ^ 1u;
            break;
        case PPC_S_CRNOR:
            t = (a | b) ^ 1u;
            break;
        case PPC_S_CROR:
            t = a | b;
            break;
        case PPC_S_CRORC:
            t = a | (b ^ 1u);
            break;
        default:
            t = a ^ b;
            break;
        }
        c->cr = (c->cr & ~(1u << (31u - d->rd))) | (t << (31u - d->rd));
        break;
    }
    case PPC_S_MCRF:
        cr_set(c, d->crf, cr_get(c, d->crs));
        break;
    case PPC_S_MCRXR:
        cr_set(c, d->crf, c->xer >> 28);
        c->xer &= 0x0FFFFFFFu;
        break;
    case PPC_S_MFCR:
        r[d->rd] = c->cr;
        break;
    case PPC_S_MTCRF: {
        uint32_t m = 0u;

        for (uint32_t i = 0u; i < 8u; i++) {
            if (((d->imm >> (7u - i)) & 1u) != 0u) {
                m |= 0xFu << (4u * (7u - i));
            }
        }
        c->cr = (c->cr & ~m) | (r[d->rd] & m);
        break;
    }
    case PPC_S_ISEL:
        r[d->rd] = (cr_bit(c, d->bi) != 0u) ? base_of(c, d) : r[d->rb];
        break;

    /* --- memory --------------------------------------------------- */
    case PPC_S_LOAD: {
        const uint32_t ea = ea_of(c, d);
        uint32_t v;

        if (EMU_UNLIKELY(ppc_load(c, ea, d->size, d->sext != 0u, &v) != PPC_EXC_NONE)) {
            return dsi(c, pc, false);
        }
        if (d->rev) {
            v = bswap(v, d->size);
        }
        /*
         * 3.16.1: an update form with rD = rA, or rA = 0, is executed;
         * rA takes the address and then rD the data, so where they are
         * the same register the data is what is left.
         */
        if (d->upd) {
            r[d->ra] = ea;
        }
        r[d->rd] = v;
        break;
    }
    case PPC_S_STORE: {
        const uint32_t ea = ea_of(c, d);
        uint32_t v = r[d->rd];

        if (d->rev) {
            v = bswap(v, d->size);
        }
        if (EMU_UNLIKELY(ppc_store(c, ea, d->size, v) != PPC_EXC_NONE)) {
            return dsi(c, pc, true);
        }
        if (d->upd) {
            r[d->ra] = ea;
        }
        break;
    }
    case PPC_S_LMW:
    case PPC_S_STMW:
    case PPC_S_LMV:
    case PPC_S_STMV:
        if (!exec_multi(c, d, pc)) {
            return false;
        }
        break;
    case PPC_S_LARX: {
        const uint32_t ea = ea_of(c, d);
        uint32_t v;

        if ((ea & (d->size - 1u)) != 0u) {
            return misaligned(c, pc, ea, false);
        }
        if (ppc_load(c, ea, d->size, false, &v) != PPC_EXC_NONE) {
            return dsi(c, pc, false);
        }
        c->reserve = true;
        r[d->rd] = v;
        break;
    }
    case PPC_S_STCX: {
        const uint32_t ea = ea_of(c, d);

        /*
         * 3.5: with no reservation the instruction is a no-op that
         * reports failure, and "no exceptions will be taken" -- so the
         * flag is looked at before the alignment.
         */
        if (!c->reserve) {
            cr_set(c, 0u, so(c));
            break;
        }
        if ((ea & (d->size - 1u)) != 0u) {
            return misaligned(c, pc, ea, true);
        }
        if (ppc_store(c, ea, d->size, r[d->rd]) != PPC_EXC_NONE) {
            return dsi(c, pc, true);
        }
        c->reserve = false;
        cr_set(c, 0u, PPC_CR_EQ | so(c));
        break;
    }
    case PPC_S_DCBZ: {
        /*
         * 7.7.6: dcbz with the data cache disabled is an alignment
         * interrupt, and out of reset it is disabled. With L1CSR0[DCE]
         * set it zeroes the 32-byte line, which is all a cache that
         * this model does not otherwise have can be seen to do.
         */
        const uint32_t ea = ea_of(c, d);

        if ((c->l1csr0 & PPC_L1CSR0_DCE) == 0u) {
            return misaligned(c, pc, ea, true);
        }
        for (uint32_t i = 0u; i < 32u; i += 4u) {
            if (ppc_store(c, (ea & ~31u) + i, 4u, 0u) != PPC_EXC_NONE) {
                return dsi(c, pc, true);
            }
        }
        break;
    }

    /* --- branches ------------------------------------------------- */
    case PPC_S_B:
        if (d->lk) {
            c->lr = next;
        }
        c->pc = (d->aa ? 0u : pc) + d->imm;
        return true;
    case PPC_S_BC: {
        const bool t = bo_taken(c, d->bo, d->bi);

        if (d->lk) {
            c->lr = next;
        }
        c->pc = t ? ((d->aa ? 0u : pc) + d->imm) : next;
        return true;
    }
    case PPC_S_BCLR: {
        const uint32_t tgt = c->lr & target_mask(c);
        const bool t = bo_taken(c, d->bo, d->bi);

        if (d->lk) {
            c->lr = next;
        }
        c->pc = t ? tgt : next;
        return true;
    }
    case PPC_S_BCCTR: {
        /* 3.16.3: BO2 = 0 decrements, and branches to the value CTR had
         * before -- read it first. */
        const uint32_t tgt = c->ctr & target_mask(c);
        const bool t = bo_taken(c, d->bo, d->bi);

        if (d->lk) {
            c->lr = next;
        }
        c->pc = t ? tgt : next;
        return true;
    }

    /* --- system --------------------------------------------------- */
    case PPC_S_SC:
        /*
         * The platform's syscall hook gets first refusal, so a host
         * harness can offer write/exit the way it does for the other
         * frontends -- but only from supervisor state. A system call
         * from user mode belongs to whatever kernel the guest brought,
         * and answering it here would hide that kernel's handler: the
         * RV32 frontend learned that from a Linux userspace that could
         * not print.
         */
        if (c->syscall != NULL && !user(c)) {
            emu_syscall_t sc = {
                .nr = r[0],
                .arg = {r[3], r[4], r[5], r[6]},
                .ret = 0u,
            };

            c->pc = next;
            if (c->syscall((emu_cpu_t *)c, &sc, c->syscall_user)) {
                r[3] = sc.ret;
                return true;
            }
        }
        ppc_cpu_raise(c, PPC_IVOR_SYSTEM_CALL, next, 0u);
        return false;
    case PPC_S_RFI:
    case PPC_S_RFCI:
    case PPC_S_RFDI:
    case PPC_S_RFMCI: {
        uint32_t ret;
        uint32_t msr;

        if (user(c)) {
            return program(c, pc, PPC_ESR_PPR);
        }
        switch (d->sem) {
        case PPC_S_RFI:
            ret = c->srr0;
            msr = c->srr1;
            break;
        case PPC_S_RFCI:
            ret = c->csrr0;
            msr = c->csrr1;
            break;
        case PPC_S_RFDI:
            ret = c->dsrr0;
            msr = c->dsrr1;
            break;
        default:
            ret = c->mcsrr0;
            msr = c->mcsrr1;
            break;
        }
        ppc_cpu_set_msr(c, msr);
        c->pc = ret & target_mask(c);
        return true;
    }
    case PPC_S_TW: {
        const uint32_t to = d->rd;
        const uint32_t a = r[d->ra];
        const uint32_t b = (d->id == PPC_M_TWI) ? d->imm : r[d->rb];

        if (((to & 0x10u) != 0u && (int32_t)a < (int32_t)b) ||
            ((to & 0x08u) != 0u && (int32_t)a > (int32_t)b) ||
            ((to & 0x04u) != 0u && a == b) || ((to & 0x02u) != 0u && a < b) ||
            ((to & 0x01u) != 0u && a > b)) {
            return program(c, pc, PPC_ESR_PTR);
        }
        break;
    }
    case PPC_S_WAIT:
        /* 3.12: completes, and the next fetch waits for an interrupt,
         * which then names the following instruction. */
        c->state = EMU_STATE_WFI;
        break;
    case PPC_S_MFMSR:
        if (user(c)) {
            return program(c, pc, PPC_ESR_PPR);
        }
        r[d->rd] = c->msr;
        break;
    case PPC_S_MTMSR:
        if (user(c)) {
            return program(c, pc, PPC_ESR_PPR);
        }
        ppc_cpu_set_msr(c, r[d->rd]);
        break;
    case PPC_S_WRTEE: {
        const bool ee = (d->id == PPC_M_WRTEEI) ? (d->imm != 0u)
                                                : ((r[d->rd] & PPC_MSR_EE) != 0u);

        if (user(c)) {
            return program(c, pc, PPC_ESR_PPR);
        }
        ppc_cpu_set_msr(c, ee ? (c->msr | PPC_MSR_EE) : (c->msr & ~PPC_MSR_EE));
        break;
    }
    case PPC_S_MFSPR: {
        uint32_t v = 0u;
        const uint32_t e = ppc_spr_read(c, d->imm, &v);

        if (e != PPC_EXC_NONE) {
            return program(c, pc, e);
        }
        r[d->rd] = v;
        break;
    }
    case PPC_S_MTSPR: {
        const uint32_t e = ppc_spr_write(c, d->imm, r[d->rd]);

        if (e != PPC_EXC_NONE) {
            return program(c, pc, e);
        }
        break;
    }
    case PPC_S_NOP:
        /*
         * The barriers and the cache hints. icbi is the one with a
         * consequence here: it is how a guest says it rewrote code, and
         * a translator's blocks of that code are what is stale. The host
         * equivalent is discarding translations, not a host barrier.
         */
        if (d->id == PPC_M_ICBI) {
            c->jit_flush = true;
        }
        break;
    case PPC_S_PRIV_NOP:
        if (user(c)) {
            return program(c, pc, PPC_ESR_PPR);
        }
        break;

    /* --- the embedded floating-point unit ------------------------- */
    case PPC_S_EFSADD:
    case PPC_S_EFSSUB:
    case PPC_S_EFSMUL:
    case PPC_S_EFSDIV:
    case PPC_S_EFSMADD:
    case PPC_S_EFSMSUB:
    case PPC_S_EFSNMADD:
    case PPC_S_EFSNMSUB:
    case PPC_S_EFSABS:
    case PPC_S_EFSNABS:
    case PPC_S_EFSNEG:
    case PPC_S_EFSSQRT:
    case PPC_S_EFSMAX:
    case PPC_S_EFSMIN:
    case PPC_S_EFSCMPGT:
    case PPC_S_EFSCMPLT:
    case PPC_S_EFSCMPEQ:
    case PPC_S_EFSTSTGT:
    case PPC_S_EFSTSTLT:
    case PPC_S_EFSTSTEQ:
    case PPC_S_EFSCFUI:
    case PPC_S_EFSCFSI:
    case PPC_S_EFSCFUF:
    case PPC_S_EFSCFSF:
    case PPC_S_EFSCFH:
    case PPC_S_EFSCTUI:
    case PPC_S_EFSCTSI:
    case PPC_S_EFSCTUF:
    case PPC_S_EFSCTSF:
    case PPC_S_EFSCTUIZ:
    case PPC_S_EFSCTSIZ:
    case PPC_S_EFSCTH: {
        /* MSR[SPE] gates the vector half only (7.7.18): scalar EFPU
         * instructions run with it clear. */
        const uint32_t e = ppc_fpu_exec(c, d->sem, d->rd, d->ra, d->rb, d->crf);

        if (e == (uint32_t)PPC_IVOR_FP_DATA) {
            ppc_cpu_raise(c, PPC_IVOR_FP_DATA, pc, PPC_ESR_SPE);
            return false;
        }
        if (e == (uint32_t)PPC_IVOR_FP_ROUND) {
            ppc_cpu_raise(c, PPC_IVOR_FP_ROUND, next, PPC_ESR_SPE);
            return false;
        }
        break;
    }
    case PPC_S_SPE:
        /*
         * The vector instructions, SPE's and the EFPU's. Unavailable
         * while MSR[SPE] is clear, as on the core; with it set this
         * model has no vector unit to run them on, and says so with an
         * illegal instruction rather than computing nothing.
         */
        if ((c->msr & PPC_MSR_SPE) == 0u) {
            ppc_cpu_raise(c, PPC_IVOR_SPE_UNAVAIL, pc, PPC_ESR_SPE);
            return false;
        }
        return program(c, pc, PPC_ESR_PIL);
    case PPC_S_PRIV_ILLEGAL:
        return program(c, pc, user(c) ? PPC_ESR_PPR : PPC_ESR_PIL);
    case PPC_S_ILLEGAL:
    default:
        return program(c, pc, PPC_ESR_PIL);
    }
    c->pc = next;
    return true;
}

/* ------------------------------------------------------------------ */
/* Run loop                                                            */
/* ------------------------------------------------------------------ */

/*
 * Decoded instructions, by address.
 *
 * The bytes are fetched every time -- that is what makes this safe
 * without anyone announcing that code changed: an entry is used only if
 * the instruction just fetched is the one it was decoded from, in the
 * encoding it was decoded as. What it saves is the decode.
 */
typedef struct {
    uint32_t pc;
    uint32_t insn;
    uint8_t vle;
    ppc_insn_t d;
} dc_t;

static dc_t g_dc[PPC_DECODE_CACHE];

static emu_run_reason_t ppc_run(emu_cpu_t *cpu, uint32_t budget,
                                uint32_t *retired)
{
    ppc_cpu_t *const c = (ppc_cpu_t *)(void *)cpu;
    uint32_t done = 0u;
    emu_run_reason_t reason = EMU_RUN_BUDGET;

    /*
     * A waiting core leaves its wait for an enabled interrupt (3.12)
     * and for nothing else; with none pending, this slice is empty.
     */
    if (EMU_UNLIKELY(c->state == EMU_STATE_WFI)) {
        if (!ppc_cpu_wake(c)) {
            if (retired != NULL) {
                *retired = 0u;
            }
            return EMU_RUN_WFI;
        }
    }

    while (done < budget) {
        /*
         * The stretch up to the next thing the processor clock will do.
         * The timers are brought up to date, and the run is cut at the
         * instruction the decrementer expires on, so the interrupt is
         * taken exactly there without a test per instruction: the inner
         * loop's bound *is* the test.
         */
        uint32_t limit;

        ppc_cpu_sync_clock(c);
        {
            const uint32_t left = budget - done;
            const uint32_t until = ppc_cpu_clock_until(c);

            limit = done + ((until < left) ? until : left);
        }

    while (done < limit) {
        uint32_t pc;
        uint16_t w0;
        uint32_t insn;
        unsigned len;

        /*
         * Tested at the top, not only where wait or a halt is decoded:
         * the syscall hook can halt the core to implement exit(), and a
         * loop that only checks at the decode site runs one more
         * instruction and reports the wrong reason.
         */
        if (EMU_UNLIKELY(c->state != EMU_STATE_RUNNING)) {
            reason = (c->state == EMU_STATE_HALTED) ? EMU_RUN_HALTED : EMU_RUN_WFI;
            goto out;
        }
        if (EMU_UNLIKELY(c->irq_dirty)) {
            /*
             * Cleared before the evaluation, not after: on a target the
             * platform raises the external input from an ARM handler,
             * and a set performed while we are deciding would otherwise
             * be overwritten and lost.
             */
            c->irq_dirty = false;
            if (ppc_cpu_take_irq(c)) {
                done++;
                c->cycles++;
            }
            /* Whatever dirtied it may have moved the expiry: recompute. */
            break;
        }

        pc = c->pc;
        if (EMU_UNLIKELY(emu_bus_fetch16(c->bus, pc, &w0) != EMU_FAULT_NONE)) {
            ppc_cpu_raise(c, PPC_IVOR_INST_STORAGE, pc, 0u);
            done++;
            c->cycles++;
            continue;
        }
        /*
         * Classic Book E is fixed 32-bit; only VLE has a length rule.
         * Applying the VLE rule to Book E desynchronises the stream on
         * the first branch, because `bl` has top4 = 0x4 and would read
         * as a 16-bit parcel.
         */
        len = c->vle ? ppc_vle_len(w0) : 4u;
        insn = (uint32_t)w0 << 16;
        if (len == 4u) {
            uint16_t w1;

            if (EMU_UNLIKELY(emu_bus_fetch16(c->bus, pc + 2u, &w1) != EMU_FAULT_NONE)) {
                ppc_cpu_raise(c, PPC_IVOR_INST_STORAGE, pc, 0u);
                done++;
                c->cycles++;
                continue;
            }
            insn |= w1;
        }

#if EMU_ENABLE_TRACE
        if (c->trace != NULL) {
            c->trace((emu_cpu_t *)c, pc, insn, len, c->trace_user);
        }
#endif

        {
            dc_t *const e = &g_dc[(pc >> 1) & (PPC_DECODE_CACHE - 1u)];

            if (EMU_UNLIKELY(e->pc != pc || e->insn != insn ||
                             e->vle != (uint8_t)c->vle || e->d.len == 0u)) {
                ppc_decode(insn, len, c->vle, &e->d);
                e->pc = pc;
                e->insn = insn;
                e->vle = (uint8_t)c->vle;
            }
            if (ppc_exec(c, &e->d, pc)) {
                c->retired++;
            }
        }
        /*
         * `done` counts an interrupt as well as a retirement, and that
         * is termination rather than bookkeeping: with the IVORs zero a
         * fetch fault vectors to an unmapped 0 and faults again, and a
         * budget that counted only retirements would never run out.
         */
        done++;
        c->cycles++;
    }
    }

out:
    ppc_cpu_sync_clock(c);
    if (retired != NULL) {
        *retired = done;
    }
    return reason;
}

emu_run_reason_t ppc_step(ppc_cpu_t *c)
{
    return ppc_backend->run((emu_cpu_t *)c, 1u, NULL);
}

const emu_backend_t ppc_backend_interp = {
    .name = "interp",
    .run = ppc_run,
};

const emu_backend_t *ppc_backend = &ppc_backend_interp;
