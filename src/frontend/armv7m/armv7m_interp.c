/* SPDX-License-Identifier: Apache-2.0 */
/*
 * armv7m_interp.c - the ARMv7E-M interpreter: decode, execute, run.
 *
 * Organised the way the ARM ARM's chapter A5 is: one function per
 * encoding table, each taking exactly the fields that table describes.
 * **Every defect this decoder has had was a field read too narrow** --
 * two bits where three were needed, one where two were -- aliasing one
 * group of instructions onto another. Following the tables' own
 * boundaries is the defence, and tests/armv7m-diff is the check: the
 * same generated cases run on a Cortex-M7 and here, and every line has
 * to match.
 *
 * An instruction returns the exception it raised, or ARMV7M_X_NONE. It
 * never half-completes: loads collect their values before writing any
 * register, which is what lets a fault be *taken* -- vectored through the
 * guest's table, exactly as on the board -- rather than only reported.
 */

#include "armv7m/armv7m_cpu.h"
#include "armv7m/armv7m_pairstats.h"

#include "emu/emu_bus.h"

#include <string.h>

typedef armv7m_exc_t X;
#define OK ARMV7M_X_NONE

/* CCR bits the interpreter consults. */
#define CCR_UNALIGN_TRP (1u << 3)
#define CCR_DIV_0_TRP (1u << 4)

/* ------------------------------------------------------------------ */
/* Flags                                                               */
/* ------------------------------------------------------------------ */

static inline uint32_t carry(const armv7m_cpu_t *c)
{
    return (c->xpsr & ARMV7M_C) ? 1u : 0u;
}

static void set_nz(armv7m_cpu_t *c, uint32_t v)
{
    c->xpsr &= ~(ARMV7M_N | ARMV7M_Z);
    if (v == 0u) {
        c->xpsr |= ARMV7M_Z;
    }
    c->xpsr |= v & ARMV7M_N;
}

/* N, Z and C: the logical operations, whose carry is the shifter's. */
static void set_nzc(armv7m_cpu_t *c, uint32_t v, uint32_t cy)
{
    set_nz(c, v);
    c->xpsr = (c->xpsr & ~ARMV7M_C) | (cy ? ARMV7M_C : 0u);
}

/*
 * AddWithCarry, which is also how subtraction is defined: a - b is
 * a + ~b + 1, and the flags fall out of the same arithmetic. Writing
 * subtraction any other way is how a borrow ends up inverted on one of
 * the two paths.
 */
static uint32_t add_c(armv7m_cpu_t *c, uint32_t a, uint32_t b, uint32_t ci,
                      bool setflags)
{
    const uint64_t u = (uint64_t)a + (uint64_t)b + (uint64_t)ci;
    const uint32_t res = (uint32_t)u;

    if (setflags) {
        c->xpsr &= ~(ARMV7M_C | ARMV7M_V);
        if ((u >> 32) != 0u) {
            c->xpsr |= ARMV7M_C;
        }
        if ((((a ^ res) & (b ^ res)) & 0x80000000u) != 0u) {
            c->xpsr |= ARMV7M_V;
        }
        set_nz(c, res);
    }
    return res;
}

static inline void set_q(armv7m_cpu_t *c)
{
    c->xpsr |= ARMV7M_Q;
}

static inline uint32_t ge(const armv7m_cpu_t *c)
{
    return (c->xpsr >> ARMV7M_GE_SHIFT) & 0xFu;
}

static inline void set_ge(armv7m_cpu_t *c, uint32_t g)
{
    c->xpsr = (c->xpsr & ~ARMV7M_GE_MASK) | ((g & 0xFu) << ARMV7M_GE_SHIFT);
}

/*
 * ITAdvance, from A7.3.2: the low five bits shift left, and the block
 * ends when the low three are zero.
 */
static void it_advance(armv7m_cpu_t *c)
{
    const uint32_t it = armv7m_it_get(c->xpsr);

    if ((it & 7u) == 0u) {
        c->xpsr = armv7m_it_put(c->xpsr, 0u);
    } else {
        c->xpsr = armv7m_it_put(c->xpsr, (it & 0xE0u) | ((it << 1) & 0x1Fu));
    }
}

static bool cond_holds(const armv7m_cpu_t *c, uint32_t cond)
{
    const bool n = (c->xpsr & ARMV7M_N) != 0u;
    const bool z = (c->xpsr & ARMV7M_Z) != 0u;
    const bool v = (c->xpsr & ARMV7M_V) != 0u;
    const bool cy = (c->xpsr & ARMV7M_C) != 0u;

    switch (cond >> 1) {
    case 0u:
        return (cond & 1u) ? !z : z; /* EQ / NE */
    case 1u:
        return (cond & 1u) ? !cy : cy; /* CS / CC */
    case 2u:
        return (cond & 1u) ? !n : n; /* MI / PL */
    case 3u:
        return (cond & 1u) ? !v : v; /* VS / VC */
    case 4u:
        return (cond & 1u) ? (!cy || z) : (cy && !z); /* HI / LS */
    case 5u:
        return (cond & 1u) ? (n != v) : (n == v); /* GE / LT */
    case 6u:
        return (cond & 1u) ? (z || (n != v)) : (!z && (n == v)); /* GT / LE */
    default:
        return true; /* AL */
    }
}

/* ------------------------------------------------------------------ */
/* Shifts and immediates                                               */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* Saturation                                                          */
/* ------------------------------------------------------------------ */

static int64_t ssat(int64_t v, uint32_t bits, bool *sat)
{
    const int64_t hi = ((int64_t)1 << (bits - 1u)) - 1;
    const int64_t lo = -((int64_t)1 << (bits - 1u));

    if (v > hi) {
        *sat = true;
        return hi;
    }
    if (v < lo) {
        *sat = true;
        return lo;
    }
    return v;
}

static int64_t usat(int64_t v, uint32_t bits, bool *sat)
{
    const int64_t hi = (bits >= 63u) ? INT64_MAX : (((int64_t)1 << bits) - 1);

    if (v > hi) {
        *sat = true;
        return hi;
    }
    if (v < 0) {
        *sat = true;
        return 0;
    }
    return v;
}

/* ------------------------------------------------------------------ */
/* Writing the pc                                                      */
/* ------------------------------------------------------------------ */

/* BranchWritePC / ALUWritePC: bit 0 is ignored. */
static inline void branch_to(armv7m_cpu_t *c, uint32_t addr)
{
    c->r[ARMV7M_PC] = addr & ~1u;
}

/*
 * BXWritePC / LoadWritePC.
 *
 * In Handler mode a value of the form 0xFxxxxxxx is an exception return,
 * and is left in the pc for the run loop to recognise -- there is nothing
 * there to fetch. Otherwise bit 0 becomes EPSR.T, and **a zero is not a
 * switch to ARM state**: an ARMv7-M core has none, so the next fetch
 * takes a UsageFault (INVSTATE) instead.
 */
static void bx_write_pc(armv7m_cpu_t *c, uint32_t addr)
{
    if (armv7m_handler_mode(c) &&
        (addr & ARMV7M_EXC_RETURN_MASK) == ARMV7M_EXC_RETURN_MASK) {
        c->r[ARMV7M_PC] = addr;
        return;
    }
    c->xpsr = (c->xpsr & ~ARMV7M_T) | ((addr & 1u) ? ARMV7M_T : 0u);
    c->r[ARMV7M_PC] = addr & ~1u;
}

/* ------------------------------------------------------------------ */
/* Memory                                                              */
/* ------------------------------------------------------------------ */

/*
 * An access that may be unaligned: LDR, STR, LDRH, STRH, LDRSH and
 * their unprivileged forms. With CCR.UNALIGN_TRP clear the architecture
 * performs it, and does so as a sequence of byte accesses -- which is
 * also what keeps an MPU check honest for an access straddling two
 * regions.
 */
static X rd_u(armv7m_cpu_t *c, uint32_t addr, uint32_t size, bool unpriv,
              uint32_t *out)
{
    if (size > 1u && (addr & (size - 1u)) != 0u) {
        uint32_t v = 0u;

        if ((c->ccr & CCR_UNALIGN_TRP) != 0u) {
            return ARMV7M_X_UNALIGNED;
        }
        for (uint32_t i = 0u; i < size; i++) {
            uint32_t b = 0u;
            const X x = armv7m_mem_read(c, addr + i, 1u, unpriv, &b);

            if (x != OK) {
                return x;
            }
            v |= (b & 0xFFu) << (8u * i);
        }
        *out = v;
        return OK;
    }
    return armv7m_mem_read(c, addr, size, unpriv, out);
}

static X wr_u(armv7m_cpu_t *c, uint32_t addr, uint32_t size, bool unpriv,
              uint32_t v)
{
    if (size > 1u && (addr & (size - 1u)) != 0u) {
        if ((c->ccr & CCR_UNALIGN_TRP) != 0u) {
            return ARMV7M_X_UNALIGNED;
        }
        for (uint32_t i = 0u; i < size; i++) {
            const X x =
                armv7m_mem_write(c, addr + i, 1u, unpriv, (v >> (8u * i)) & 0xFFu);

            if (x != OK) {
                return x;
            }
        }
        return OK;
    }
    return armv7m_mem_write(c, addr, size, unpriv, v);
}

/* MemA: an access that must be aligned -- LDRD, LDM, LDREX and stacking. */
static X rd_a(armv7m_cpu_t *c, uint32_t addr, uint32_t size, uint32_t *out)
{
    if ((addr & (size - 1u)) != 0u) {
        return ARMV7M_X_UNALIGNED;
    }
    return armv7m_mem_read(c, addr, size, false, out);
}

static X wr_a(armv7m_cpu_t *c, uint32_t addr, uint32_t size, uint32_t v)
{
    if ((addr & (size - 1u)) != 0u) {
        return ARMV7M_X_UNALIGNED;
    }
    return armv7m_mem_write(c, addr, size, false, v);
}

static uint32_t extend(uint32_t v, uint32_t size, bool sext)
{
    if (!sext) {
        return v;
    }
    if (size == 1u) {
        return (uint32_t)(int32_t)(int8_t)v;
    }
    if (size == 2u) {
        return (uint32_t)(int32_t)(int16_t)v;
    }
    return v;
}

/* pc as an operand: this instruction's address plus four. */
static inline uint32_t reg_pc(const armv7m_cpu_t *c, uint32_t n, uint32_t pc)
{
    return (n == ARMV7M_PC) ? pc + 4u : c->r[n];
}

/* ------------------------------------------------------------------ */
/* Load/store multiple                                                 */
/* ------------------------------------------------------------------ */

/*
 * LDM and STM, both widths. `before` is the DB form, `wback` the
 * writeback. Loads collect every value before writing any register, so a
 * fault partway through leaves the registers as they were -- the
 * architecture permits restarting the instruction, which only works if
 * the first attempt did not consume its own base.
 */
static X ldm_stm(armv7m_cpu_t *c, uint32_t rn, uint32_t list, bool load,
                 bool before, bool wback)
{
    uint32_t count = 0u;
    uint32_t vals[16];

    for (uint32_t i = 0u; i < 16u; i++) {
        count += (list >> i) & 1u;
    }
    const uint32_t base = c->r[rn];
    const uint32_t start = before ? base - 4u * count : base;
    const uint32_t final = before ? base - 4u * count : base + 4u * count;
    uint32_t addr = start;

    for (uint32_t i = 0u; i < 16u; i++) {
        if ((list & (1u << i)) == 0u) {
            continue;
        }
        if (load) {
            const X x = rd_a(c, addr, 4u, &vals[i]);

            if (x != OK) {
                return x;
            }
        } else {
            const X x = wr_a(c, addr, 4u, c->r[i]);

            if (x != OK) {
                return x;
            }
        }
        addr += 4u;
    }
    if (wback) {
        c->r[rn] = final;
    }
    if (load) {
        for (uint32_t i = 0u; i < 15u; i++) {
            if ((list & (1u << i)) != 0u) {
                c->r[i] = vals[i];
            }
        }
        if ((list & (1u << 15)) != 0u) {
            bx_write_pc(c, vals[15]);
        }
    }
    return OK;
}

/* ------------------------------------------------------------------ */
/* Special registers                                                   */
/* ------------------------------------------------------------------ */

static uint32_t mrs(armv7m_cpu_t *c, uint32_t sysm)
{
    const bool priv = armv7m_privileged(c);
    uint32_t v = 0u;

    switch (sysm >> 3) {
    case 0u: /* the xPSR views */
        if ((sysm & 1u) != 0u) {
            v |= c->xpsr & ARMV7M_IPSR_MASK;
        }
        /* EPSR reads as zero, so bit 1 adds nothing. */
        if ((sysm & 4u) == 0u) {
            v |= c->xpsr & ARMV7M_APSR_MASK;
        }
        return v;
    case 1u:
        if (!priv) {
            return 0u;
        }
        return armv7m_get_sp(c, sysm & 7u);
    case 2u:
        switch (sysm & 7u) {
        case 0u:
            return priv ? c->primask : 0u;
        case 1u:
        case 2u:
            return priv ? c->basepri : 0u;
        case 3u:
            return priv ? c->faultmask : 0u;
        case 4u:
            return c->control & 7u;
        default:
            return 0u;
        }
    default:
        return 0u;
    }
}

static void msr(armv7m_cpu_t *c, uint32_t sysm, uint32_t mask, uint32_t v)
{
    const bool priv = armv7m_privileged(c);

    switch (sysm >> 3) {
    case 0u:
        if ((sysm & 4u) == 0u) {
            if ((mask & 1u) != 0u) {
                c->xpsr = (c->xpsr & ~ARMV7M_GE_MASK) | (v & ARMV7M_GE_MASK);
            }
            if ((mask & 2u) != 0u) {
                c->xpsr = (c->xpsr & ~0xF8000000u) | (v & 0xF8000000u);
            }
        }
        return;
    case 1u:
        if (priv) {
            armv7m_set_sp(c, sysm & 7u, v);
        }
        return;
    case 2u:
        if (!priv) {
            return;
        }
        switch (sysm & 7u) {
        case 0u:
            c->primask = v & 1u;
            return;
        case 1u:
            c->basepri = v & 0xFFu;
            return;
        case 2u: { /* BASEPRI_MAX: only ever raises the priority */
            const uint32_t nv = v & 0xFFu;

            if (nv != 0u && (nv < c->basepri || c->basepri == 0u)) {
                c->basepri = nv;
            }
            return;
        }
        case 3u:
            if (armv7m_exec_priority(c) > -1) {
                c->faultmask = v & 1u;
            }
            return;
        case 4u:
            armv7m_write_control(c, v);
            return;
        default:
            return;
        }
    default:
        return;
    }
}

/* ------------------------------------------------------------------ */
/* 16-bit instructions                                                 */
/* ------------------------------------------------------------------ */

/* Shift (immediate), add, subtract, move and compare: 00xxxx. */
static X t16_shift_add(armv7m_cpu_t *c, uint16_t insn, bool setf)
{
    const uint32_t op = (insn >> 9) & 31u;
    const uint32_t rd = insn & 7u;
    const uint32_t rm = (insn >> 3) & 7u;

    if (op < 12u) { /* LSL, LSR, ASR (immediate) */
        uint32_t t;
        uint32_t n;
        uint32_t cy;

        armv7m_decode_imm_shift(op >> 2, (insn >> 6) & 31u, &t, &n);
        c->r[rd] = armv7m_shift_c(c->r[rm], t, n, carry(c), &cy);
        if (setf) {
            set_nzc(c, c->r[rd], cy);
        }
        return OK;
    }
    switch (op) {
    case 12u: /* ADD (register) */
    case 13u: /* SUB (register) */
    case 14u: /* ADD (3-bit immediate) */
    case 15u: { /* SUB (3-bit immediate) */
        const uint32_t rn = (insn >> 3) & 7u;
        const uint32_t m = (insn >> 6) & 7u;
        const uint32_t b = (op >= 14u) ? m : c->r[m];
        const bool sub = (op & 1u) != 0u;

        c->r[rd] = add_c(c, c->r[rn], sub ? ~b : b, sub ? 1u : 0u, setf);
        return OK;
    }
    default:
        break;
    }
    {
        const uint32_t rdn = (insn >> 8) & 7u;
        const uint32_t imm8 = insn & 0xFFu;

        switch (op >> 2) {
        case 4u: /* MOV (immediate): C is unchanged */
            c->r[rdn] = imm8;
            if (setf) {
                set_nz(c, imm8);
            }
            return OK;
        case 5u: /* CMP */
            (void)add_c(c, c->r[rdn], ~imm8, 1u, true);
            return OK;
        case 6u: /* ADD (8-bit immediate) */
            c->r[rdn] = add_c(c, c->r[rdn], imm8, 0u, setf);
            return OK;
        default: /* SUB (8-bit immediate) */
            c->r[rdn] = add_c(c, c->r[rdn], ~imm8, 1u, setf);
            return OK;
        }
    }
}

/* Data processing: 010000. */
static X t16_dp(armv7m_cpu_t *c, uint16_t insn, bool setf)
{
    const uint32_t op = (insn >> 6) & 15u;
    const uint32_t rm = (insn >> 3) & 7u;
    const uint32_t rd = insn & 7u;
    const uint32_t a = c->r[rd];
    const uint32_t b = c->r[rm];
    uint32_t cy = carry(c);
    uint32_t res;

    switch (op) {
    case 0u: /* AND */
        res = a & b;
        break;
    case 1u: /* EOR */
        res = a ^ b;
        break;
    case 2u: /* LSL (register) */
        res = armv7m_shift_c(a, ARMV7M_SH_LSL, b & 0xFFu, cy, &cy);
        break;
    case 3u: /* LSR (register) */
        res = armv7m_shift_c(a, ARMV7M_SH_LSR, b & 0xFFu, cy, &cy);
        break;
    case 4u: /* ASR (register) */
        res = armv7m_shift_c(a, ARMV7M_SH_ASR, b & 0xFFu, cy, &cy);
        break;
    case 5u: /* ADC */
        c->r[rd] = add_c(c, a, b, carry(c), setf);
        return OK;
    case 6u: /* SBC */
        c->r[rd] = add_c(c, a, ~b, carry(c), setf);
        return OK;
    case 7u: /* ROR (register) */
        res = armv7m_shift_c(a, ARMV7M_SH_ROR, b & 0xFFu, cy, &cy);
        break;
    case 8u: /* TST */
        set_nzc(c, a & b, cy);
        return OK;
    case 9u: /* RSB (immediate #0): the negation */
        c->r[rd] = add_c(c, ~b, 0u, 1u, setf);
        return OK;
    case 10u: /* CMP */
        (void)add_c(c, a, ~b, 1u, true);
        return OK;
    case 11u: /* CMN */
        (void)add_c(c, a, b, 0u, true);
        return OK;
    case 12u: /* ORR */
        res = a | b;
        break;
    case 13u: /* MUL: N and Z only, C and V untouched */
        c->r[rd] = a * b;
        if (setf) {
            set_nz(c, c->r[rd]);
        }
        return OK;
    case 14u: /* BIC */
        res = a & ~b;
        break;
    default: /* MVN */
        res = ~b;
        break;
    }
    c->r[rd] = res;
    if (setf) {
        set_nzc(c, res, cy);
    }
    return OK;
}

/* Special data instructions and branch and exchange: 010001. */
static X t16_special(armv7m_cpu_t *c, uint16_t insn, uint32_t pc)
{
    const uint32_t op = (insn >> 6) & 15u;
    const uint32_t rm = (insn >> 3) & 15u;
    const uint32_t rd = (insn & 7u) | ((insn >> 4) & 8u);

    switch (op >> 2) {
    case 0u: { /* ADD (register), any registers, no flags */
        const uint32_t v = reg_pc(c, rd, pc) + reg_pc(c, rm, pc);

        if (rd == ARMV7M_PC) {
            branch_to(c, v);
        } else {
            c->r[rd] = v;
        }
        return OK;
    }
    case 1u: /* CMP (register) */
        (void)add_c(c, reg_pc(c, rd, pc), ~reg_pc(c, rm, pc), 1u, true);
        return OK;
    case 2u: { /* MOV (register), no flags */
        const uint32_t v = reg_pc(c, rm, pc);

        if (rd == ARMV7M_PC) {
            branch_to(c, v);
        } else {
            c->r[rd] = v;
        }
        return OK;
    }
    default: {
        /*
         * BX and BLX. The link register takes the address of the *next*
         * instruction with the Thumb bit set, which is what lets the
         * matching BX return to it.
         */
        const uint32_t target = reg_pc(c, rm, pc);

        if ((insn & 7u) != 0u) {
            return ARMV7M_X_UNDEF;
        }
        if ((op & 2u) != 0u) { /* BLX */
            c->r[ARMV7M_LR] = (pc + 2u) | 1u;
        }
        bx_write_pc(c, target);
        return OK;
    }
    }
}

/* Load/store single: 0101, 011x, 100x. */
static X t16_ldst(armv7m_cpu_t *c, uint16_t insn, uint32_t pc)
{
    const uint32_t opa = insn >> 12;
    const uint32_t rt = insn & 7u;
    const uint32_t rn = (insn >> 3) & 7u;
    uint32_t addr;
    uint32_t size;
    bool load;
    bool sext = false;

    (void)pc;
    if (opa == 5u) { /* register offset */
        static const uint8_t k_size[8] = {4u, 2u, 1u, 1u, 4u, 2u, 1u, 2u};
        const uint32_t opb = (insn >> 9) & 7u;

        addr = c->r[rn] + c->r[(insn >> 6) & 7u];
        size = k_size[opb];
        load = opb >= 3u;
        sext = (opb == 3u) || (opb == 7u);
    } else if (opa == 6u || opa == 7u || opa == 8u) {
        const uint32_t imm5 = (insn >> 6) & 31u;

        size = (opa == 6u) ? 4u : ((opa == 7u) ? 1u : 2u);
        load = ((insn >> 11) & 1u) != 0u;
        addr = c->r[rn] + imm5 * size;
    } else { /* 1001: sp-relative */
        size = 4u;
        load = ((insn >> 11) & 1u) != 0u;
        addr = c->r[ARMV7M_SP] + (insn & 0xFFu) * 4u;
        if (load) {
            uint32_t v = 0u;
            const X x = rd_u(c, addr, 4u, false, &v);

            if (x == OK) {
                c->r[(insn >> 8) & 7u] = v;
            }
            return x;
        }
        return wr_u(c, addr, 4u, false, c->r[(insn >> 8) & 7u]);
    }
    if (load) {
        uint32_t v = 0u;
        const X x = rd_u(c, addr, size, false, &v);

        if (x == OK) {
            c->r[rt] = extend(v, size, sext);
        }
        return x;
    }
    return wr_u(c, addr, size, false, c->r[rt]);
}

/* Miscellaneous 16-bit instructions: 1011. */
static X t16_misc(armv7m_cpu_t *c, uint16_t insn, uint32_t pc)
{
    const uint32_t op = (insn >> 5) & 0x7Fu;

    if (op == 0x33u) { /* CPS: 1011 0110 011 im 0 0 I F */
        if (armv7m_privileged(c)) {
            const bool disable = ((insn >> 4) & 1u) != 0u;

            if ((insn & 2u) != 0u) {
                c->primask = disable ? 1u : 0u;
            }
            if ((insn & 1u) != 0u) {
                if (!disable) {
                    c->faultmask = 0u;
                } else if (armv7m_exec_priority(c) > -1) {
                    c->faultmask = 1u;
                }
            }
        }
        return OK;
    }
    /*
     * ADD/SUB sp, #imm7*4: opcode 0000xxx, and bit 7 of the instruction
     * chooses subtract. **Three bits of prefix, not five**: 00000xx is
     * ADD and 00001xx is SUB, and testing `op >> 2` took ADD alone -- so
     * every function prologue's `sub sp, #n` was an undefined
     * instruction. The fourth field-width defect in this decoder, and the
     * first one the board harness's own start-up found.
     */
    if ((op >> 3) == 0u) {
        const uint32_t imm = (insn & 0x7Fu) * 4u;

        if ((insn & 0x80u) != 0u) {
            c->r[ARMV7M_SP] -= imm;
        } else {
            c->r[ARMV7M_SP] += imm;
        }
        return OK;
    }
    /* CBZ/CBNZ: 1011 o0i1 -- bit 8 set, bit 10 clear. */
    if ((insn & 0x0500u) == 0x0100u) {
        const uint32_t rn = insn & 7u;
        const bool nonzero = ((insn >> 11) & 1u) != 0u;
        const uint32_t imm = (((insn >> 3) & 31u) << 1) | (((insn >> 9) & 1u) << 6);

        if ((c->r[rn] != 0u) == nonzero) {
            c->r[ARMV7M_PC] = pc + 4u + imm;
        }
        return OK;
    }
    if ((insn & 0xFF00u) == 0xB200u) { /* SXTH, SXTB, UXTH, UXTB */
        const uint32_t v = c->r[(insn >> 3) & 7u];
        uint32_t res;

        switch ((insn >> 6) & 3u) {
        case 0u:
            res = (uint32_t)(int32_t)(int16_t)v;
            break;
        case 1u:
            res = (uint32_t)(int32_t)(int8_t)v;
            break;
        case 2u:
            res = v & 0xFFFFu;
            break;
        default:
            res = v & 0xFFu;
            break;
        }
        c->r[insn & 7u] = res;
        return OK;
    }
    if ((insn & 0xFF00u) == 0xBA00u) { /* REV, REV16, REVSH */
        const uint32_t v = c->r[(insn >> 3) & 7u];
        uint32_t res;

        switch ((insn >> 6) & 3u) {
        case 0u:
            res = __builtin_bswap32(v);
            break;
        case 1u:
            res = ((v & 0x00FF00FFu) << 8) | ((v >> 8) & 0x00FF00FFu);
            break;
        case 3u:
            res = (uint32_t)(int32_t)(int16_t)(uint16_t)(((v & 0xFFu) << 8) |
                                                          ((v >> 8) & 0xFFu));
            break;
        default:
            return ARMV7M_X_UNDEF;
        }
        c->r[insn & 7u] = res;
        return OK;
    }
    if ((insn & 0x0600u) == 0x0400u) { /* PUSH 1011 010x, POP 1011 110x */
        const bool pop = ((insn >> 11) & 1u) != 0u;
        uint32_t list = insn & 0xFFu;

        if ((insn & 0x0100u) != 0u) {
            list |= pop ? (1u << 15) : (1u << 14);
        }
        if (list == 0u) {
            return ARMV7M_X_UNDEF; /* UNPREDICTABLE: an empty list */
        }
        return ldm_stm(c, ARMV7M_SP, list, pop, !pop, true);
    }
    if ((insn & 0xFF00u) == 0xBE00u) { /* BKPT */
        return ARMV7M_X_BKPT;
    }
    if ((insn & 0xFF00u) == 0xBF00u) { /* IT, and the hints */
        const uint32_t mask = insn & 0x0Fu;
        const uint32_t firstcond = (insn >> 4) & 0x0Fu;

        /*
         * IT is told from the hints by a non-zero mask -- 0xBF00 with
         * both fields zero is NOP. ITSTATE is firstcond:mask, and the
         * mask bits become the condition's low bit as the field shifts,
         * so ITT and ITE need no decoding of their own.
         *
         * firstcond 0b1111 does not exist, and AL with an else branch
         * would need its inverse, which also does not: both are
         * UNPREDICTABLE and are reported rather than given an answer.
         */
        if (mask != 0u) {
            if (firstcond == 0x0Fu ||
                (firstcond == 0x0Eu && (mask & 7u) != 0u &&
                 (mask & 0x0Eu) != 0x0Au)) {
                return ARMV7M_X_UNDEF;
            }
            if (armv7m_in_it(c->xpsr)) {
                return ARMV7M_X_UNDEF; /* IT inside IT */
            }
            c->xpsr = armv7m_it_put(c->xpsr, (firstcond << 4) | mask);
            return OK;
        }
        /* NOP, YIELD, WFE, WFI, SEV, and the unallocated hints, which
         * the architecture says execute as NOPs. */
        return OK;
    }
    return ARMV7M_X_UNDEF;
}

static X exec16(armv7m_cpu_t *c, uint16_t insn, uint32_t pc)
{
    /*
     * **Inside an IT block the 16-bit data-processing forms do not set
     * flags** -- `setflags = !InITBlock()` on each of those encodings.
     * A block is `cmp; it ne; addne; subne`, and if the ADD overwrote
     * the flags the SUB would be conditioned on the ADD instead of on the
     * compare. TST, CMP and CMN set them regardless.
     */
    const bool setf = !armv7m_in_it(c->xpsr);
    const uint32_t op6 = insn >> 10;

    if ((op6 >> 4) == 0u) {
        return t16_shift_add(c, insn, setf);
    }
    if (op6 == 0x10u) {
        return t16_dp(c, insn, setf);
    }
    if (op6 == 0x11u) {
        return t16_special(c, insn, pc);
    }
    if ((op6 >> 1) == 0x09u) { /* LDR (literal) */
        uint32_t v = 0u;
        const X x = rd_u(c, ((pc + 4u) & ~3u) + (insn & 0xFFu) * 4u, 4u, false,
                         &v);

        if (x == OK) {
            c->r[(insn >> 8) & 7u] = v;
        }
        return x;
    }
    if ((op6 >> 2) == 0x05u || (op6 >> 3) == 0x03u || (op6 >> 3) == 0x04u) {
        return t16_ldst(c, insn, pc);
    }
    if ((op6 >> 1) == 0x14u) { /* ADR */
        c->r[(insn >> 8) & 7u] = ((pc + 4u) & ~3u) + (insn & 0xFFu) * 4u;
        return OK;
    }
    if ((op6 >> 1) == 0x15u) { /* ADD rd, sp, #imm8*4 */
        c->r[(insn >> 8) & 7u] = c->r[ARMV7M_SP] + (insn & 0xFFu) * 4u;
        return OK;
    }
    if ((op6 >> 2) == 0x0Bu) {
        return t16_misc(c, insn, pc);
    }
    if ((op6 >> 1) == 0x18u || (op6 >> 1) == 0x19u) { /* STM / LDM */
        const uint32_t rn = (insn >> 8) & 7u;
        const uint32_t list = insn & 0xFFu;
        const bool load = ((insn >> 11) & 1u) != 0u;

        if (list == 0u) {
            return ARMV7M_X_UNDEF;
        }
        /* LDM writes back only when the base is not in the list. */
        return ldm_stm(c, rn, list, load, false,
                       !load || (list & (1u << rn)) == 0u);
    }
    if ((op6 >> 2) == 0x0Du) { /* B<cond>, UDF, SVC */
        const uint32_t cond = (insn >> 8) & 15u;

        if (cond == 15u) {
            return ARMV7M_X_SVC;
        }
        if (cond == 14u) {
            return ARMV7M_X_UNDEF;
        }
        if (armv7m_in_it(c->xpsr)) {
            return ARMV7M_X_UNDEF; /* a conditional branch inside IT */
        }
        if (cond_holds(c, cond)) {
            c->r[ARMV7M_PC] =
                (uint32_t)((int32_t)(pc + 4u) + (int32_t)(int8_t)(insn & 0xFFu) * 2);
        }
        return OK;
    }
    if ((op6 >> 1) == 0x1Cu) { /* B */
        int32_t off = (int32_t)((uint32_t)(insn & 0x7FFu) << 1);

        if ((off & 0x800) != 0) {
            off -= 0x1000;
        }
        c->r[ARMV7M_PC] = (uint32_t)((int32_t)(pc + 4u) + off);
        return OK;
    }
    return ARMV7M_X_UNDEF;
}

/* ------------------------------------------------------------------ */
/* 32-bit: data processing                                             */
/* ------------------------------------------------------------------ */

/*
 * The sixteen data-processing operations, shared by the modified-immediate
 * and shifted-register forms -- they differ only in where the second
 * operand and its carry come from.
 *
 * **S with rd == pc is a different instruction**, not a write to the pc:
 * TST for AND, TEQ for EOR, CMN for ADD and CMP for SUB. And rn == pc
 * turns ORR into MOV and ORN into MVN. Both are handled here so neither
 * form can forget them.
 */
static X dp_op(armv7m_cpu_t *c, uint32_t op, uint32_t rd, uint32_t rn,
               uint32_t b, uint32_t cy, bool s)
{
    const uint32_t a = c->r[rn];
    uint32_t res;

    if (s && rd == ARMV7M_PC) {
        switch (op) {
        case 0u:
            set_nzc(c, a & b, cy); /* TST */
            return OK;
        case 4u:
            set_nzc(c, a ^ b, cy); /* TEQ */
            return OK;
        case 8u:
            (void)add_c(c, a, b, 0u, true); /* CMN */
            return OK;
        case 13u:
            (void)add_c(c, a, ~b, 1u, true); /* CMP */
            return OK;
        default:
            return ARMV7M_X_UNDEF;
        }
    }
    switch (op) {
    case 0u:
        res = a & b;
        break;
    case 1u:
        res = a & ~b;
        break;
    case 2u:
        res = (rn == ARMV7M_PC) ? b : (a | b);
        break;
    case 3u:
        res = (rn == ARMV7M_PC) ? ~b : (a | ~b);
        break;
    case 4u:
        res = a ^ b;
        break;
    case 8u:
        c->r[rd] = add_c(c, a, b, 0u, s);
        return OK;
    case 10u:
        c->r[rd] = add_c(c, a, b, carry(c), s);
        return OK;
    case 11u:
        c->r[rd] = add_c(c, a, ~b, carry(c), s);
        return OK;
    case 13u:
        c->r[rd] = add_c(c, a, ~b, 1u, s);
        return OK;
    case 14u:
        c->r[rd] = add_c(c, ~a, b, 1u, s);
        return OK;
    default:
        return ARMV7M_X_UNDEF;
    }
    c->r[rd] = res;
    if (s) {
        set_nzc(c, res, cy);
    }
    return OK;
}

/* Data processing (modified immediate): 11110 x0 xxxxx | 0 */
static X t32_dp_imm(armv7m_cpu_t *c, uint16_t w0, uint16_t w1)
{
    const uint32_t imm12 = ((uint32_t)((w0 >> 10) & 1u) << 11) |
                           ((uint32_t)((w1 >> 12) & 7u) << 8) | (w1 & 0xFFu);
    uint32_t cy;
    const uint32_t b = armv7m_expand_imm_c(imm12, carry(c), &cy);

    return dp_op(c, (w0 >> 5) & 15u, (w1 >> 8) & 15u, w0 & 15u, b, cy,
                 ((w0 >> 4) & 1u) != 0u);
}

/* Data processing (plain binary immediate): 11110 x1 xxxxx | 0 */
static X t32_dp_plain(armv7m_cpu_t *c, uint16_t w0, uint16_t w1, uint32_t pc)
{
    const uint32_t op = (w0 >> 4) & 31u;
    const uint32_t rn = w0 & 15u;
    const uint32_t rd = (w1 >> 8) & 15u;
    const uint32_t imm3 = (w1 >> 12) & 7u;
    const uint32_t imm2 = (w1 >> 6) & 3u;
    const uint32_t i = (w0 >> 10) & 1u;
    const uint32_t imm12 = (i << 11) | (imm3 << 8) | (w1 & 0xFFu);
    const uint32_t imm16 = ((uint32_t)(w0 & 15u) << 12) | imm12;
    const uint32_t lsb = (imm3 << 2) | imm2;
    const uint32_t field = w1 & 31u;

    switch (op) {
    case 0x00u: /* ADDW, and ADR when rn is pc */
        c->r[rd] = (rn == ARMV7M_PC) ? ((pc + 4u) & ~3u) + imm12 : c->r[rn] + imm12;
        return OK;
    case 0x04u: /* MOVW */
        c->r[rd] = imm16;
        return OK;
    case 0x0Au: /* SUBW, and ADR (subtract) */
        c->r[rd] = (rn == ARMV7M_PC) ? ((pc + 4u) & ~3u) - imm12 : c->r[rn] - imm12;
        return OK;
    case 0x0Cu: /* MOVT */
        c->r[rd] = (c->r[rd] & 0xFFFFu) | (imm16 << 16);
        return OK;
    default:
        break;
    }
    if (i != 0u) {
        return ARMV7M_X_UNDEF;
    }
    switch (op) {
    case 0x10u:   /* SSAT, LSL */
    case 0x12u: { /* SSAT, ASR -- or SSAT16 when the shift is zero */
        const bool sat16 = (op == 0x12u) && lsb == 0u;
        bool sat = false;

        if ((w1 & 0x0020u) != 0u) {
            return ARMV7M_X_UNDEF;
        }
        if (sat16) {
            const uint32_t n = field + 1u;
            const int64_t lo = ssat((int16_t)(c->r[rn] & 0xFFFFu), n, &sat);
            const int64_t hi = ssat((int16_t)(c->r[rn] >> 16), n, &sat);

            if ((w1 & 0x0030u) != 0u) {
                return ARMV7M_X_UNDEF;
            }
            c->r[rd] = ((uint32_t)lo & 0xFFFFu) | ((uint32_t)hi << 16);
        } else {
            uint32_t cy;
            const uint32_t v = armv7m_shift_c(c->r[rn], (op == 0x12u) ? ARMV7M_SH_ASR : ARMV7M_SH_LSL,
                                       lsb, 0u, &cy);

            c->r[rd] = (uint32_t)ssat((int32_t)v, field + 1u, &sat);
        }
        if (sat) {
            set_q(c);
        }
        return OK;
    }
    case 0x18u:   /* USAT, LSL */
    case 0x1Au: { /* USAT, ASR -- or USAT16 */
        const bool sat16 = (op == 0x1Au) && lsb == 0u;
        bool sat = false;

        if ((w1 & 0x0020u) != 0u || (sat16 && (w1 & 0x0010u) != 0u)) {
            return ARMV7M_X_UNDEF;
        }
        if (sat16) {
            const int64_t lo = usat((int16_t)(c->r[rn] & 0xFFFFu), field & 15u, &sat);
            const int64_t hi = usat((int16_t)(c->r[rn] >> 16), field & 15u, &sat);

            c->r[rd] = ((uint32_t)lo & 0xFFFFu) | ((uint32_t)hi << 16);
        } else {
            uint32_t cy;
            const uint32_t v = armv7m_shift_c(c->r[rn], (op == 0x1Au) ? ARMV7M_SH_ASR : ARMV7M_SH_LSL,
                                       lsb, 0u, &cy);

            c->r[rd] = (uint32_t)usat((int32_t)v, field, &sat);
        }
        if (sat) {
            set_q(c);
        }
        return OK;
    }
    case 0x14u:   /* SBFX */
    case 0x1Cu: { /* UBFX */
        const uint32_t width = field + 1u;

        if (lsb + width > 32u || (w1 & 0x0020u) != 0u) {
            return ARMV7M_X_UNDEF;
        }
        if (op == 0x1Cu) {
            c->r[rd] = (width == 32u) ? c->r[rn]
                                      : ((c->r[rn] >> lsb) & ((1u << width) - 1u));
        } else {
            const uint32_t sh = 32u - width;

            c->r[rd] = (uint32_t)((int32_t)(c->r[rn] << (sh - lsb)) >> sh);
        }
        return OK;
    }
    case 0x16u: { /* BFI, and BFC when rn is pc; the field is the msb */
        if (field < lsb || (w1 & 0x0020u) != 0u) {
            return ARMV7M_X_UNDEF;
        }
        const uint32_t width = field - lsb + 1u;
        const uint32_t mask =
            (width == 32u) ? 0xFFFFFFFFu : (((1u << width) - 1u) << lsb);
        const uint32_t ins = (rn == ARMV7M_PC) ? 0u : ((c->r[rn] << lsb) & mask);

        c->r[rd] = (c->r[rd] & ~mask) | ins;
        return OK;
    }
    default:
        return ARMV7M_X_UNDEF;
    }
}

/* Data processing (shifted register): 1110101 */
static X t32_dp_reg(armv7m_cpu_t *c, uint16_t w0, uint16_t w1)
{
    const uint32_t op = (w0 >> 5) & 15u;
    const uint32_t rn = w0 & 15u;
    const uint32_t rd = (w1 >> 8) & 15u;
    const uint32_t rm = w1 & 15u;
    const uint32_t imm5 = (((uint32_t)(w1 >> 12) & 7u) << 2) | ((w1 >> 6) & 3u);
    const uint32_t type = (w1 >> 4) & 3u;
    uint32_t t;
    uint32_t n;
    uint32_t cy;

    /*
     * **Should-be bits are decoded strictly**, here and in every table
     * below: a (0) that is one, or a (1) that is zero, is UNDEFINSTR on a
     * Cortex-M7, where the manual says only UNPREDICTABLE. Found by
     * running encodings rather than instructions on the board; see
     * armv7m_decode.c, which carries the same checks and is held to this
     * file by a test over the whole encoding space.
     */
    if ((w1 & 0x8000u) != 0u) {
        return ARMV7M_X_UNDEF;
    }
    if (op == 6u) { /* PKHBT / PKHTB */
        const bool tb = ((w1 >> 5) & 1u) != 0u;

        if (((w0 >> 4) & 1u) != 0u || ((w1 >> 4) & 1u) != 0u) {
            return ARMV7M_X_UNDEF; /* no S form, and T is zero */
        }
        const uint32_t m = armv7m_shift_c(c->r[rm], tb ? ARMV7M_SH_ASR : ARMV7M_SH_LSL,
                                   (tb && imm5 == 0u) ? 32u : imm5, 0u, &cy);

        c->r[rd] = tb ? ((c->r[rn] & 0xFFFF0000u) | (m & 0xFFFFu))
                      : ((m & 0xFFFF0000u) | (c->r[rn] & 0xFFFFu));
        return OK;
    }
    armv7m_decode_imm_shift(type, imm5, &t, &n);
    {
        const uint32_t b = armv7m_shift_c(c->r[rm], t, n, carry(c), &cy);

        return dp_op(c, op, rd, rn, b, cy, ((w0 >> 4) & 1u) != 0u);
    }
}

/* ------------------------------------------------------------------ */
/* 32-bit: data processing (register), parallel, misc                  */
/* ------------------------------------------------------------------ */

static uint32_t ror8(uint32_t v, uint32_t rot)
{
    return (rot == 0u) ? v : ((v >> rot) | (v << (32u - rot)));
}

/* The signed and unsigned parallel add/subtract: A5.3.13 and A5.3.14. */
static X t32_parallel(armv7m_cpu_t *c, uint32_t op1, uint32_t op2, bool u,
                      uint32_t rd, uint32_t a, uint32_t b)
{
    int32_t r[4];
    uint32_t nlanes;
    const bool bytes = (op1 == 0u) || (op1 == 4u);

    if (op2 == 3u) {
        return ARMV7M_X_UNDEF;
    }
    if (bytes) {
        nlanes = 4u;
        for (uint32_t i = 0u; i < 4u; i++) {
            const int32_t x = u ? (int32_t)((a >> (8u * i)) & 0xFFu)
                                : (int32_t)(int8_t)(a >> (8u * i));
            const int32_t y = u ? (int32_t)((b >> (8u * i)) & 0xFFu)
                                : (int32_t)(int8_t)(b >> (8u * i));

            r[i] = (op1 == 0u) ? x + y : x - y;
        }
    } else {
        const int32_t alo = u ? (int32_t)(a & 0xFFFFu) : (int32_t)(int16_t)a;
        const int32_t ahi = u ? (int32_t)(a >> 16) : (int32_t)(int16_t)(a >> 16);
        const int32_t blo = u ? (int32_t)(b & 0xFFFFu) : (int32_t)(int16_t)b;
        const int32_t bhi = u ? (int32_t)(b >> 16) : (int32_t)(int16_t)(b >> 16);

        nlanes = 2u;
        switch (op1) {
        case 1u: /* ADD16 */
            r[0] = alo + blo;
            r[1] = ahi + bhi;
            break;
        case 2u: /* ASX: low = a.lo - b.hi, high = a.hi + b.lo */
            r[0] = alo - bhi;
            r[1] = ahi + blo;
            break;
        case 6u: /* SAX: low = a.lo + b.hi, high = a.hi - b.lo */
            r[0] = alo + bhi;
            r[1] = ahi - blo;
            break;
        case 5u: /* SUB16 */
            r[0] = alo - blo;
            r[1] = ahi - bhi;
            break;
        default:
            return ARMV7M_X_UNDEF;
        }
    }

    const uint32_t lane = bytes ? 8u : 16u;
    const uint32_t lmask = (1u << lane) - 1u;
    uint32_t res = 0u;
    uint32_t g = 0u;

    for (uint32_t i = 0u; i < nlanes; i++) {
        int64_t v = r[i];

        if (op2 == 0u) {
            /*
             * The plain forms set GE per lane: for a signed lane when the
             * result is non-negative, for an unsigned addition when it
             * carried out and for an unsigned subtraction when it did not
             * borrow -- which, written out, is the same test.
             */
            bool flag;
            const bool is_add = bytes ? (op1 == 0u)
                                      : ((op1 == 1u) || (i == 1u && op1 == 2u) ||
                                         (i == 0u && op1 == 6u));

            if (u && is_add) {
                flag = v >= (int64_t)1 << lane;
            } else {
                flag = v >= 0;
            }
            if (flag) {
                g |= bytes ? (1u << i) : (3u << (2u * i));
            }
        } else if (op2 == 1u) {
            bool sat = false;

            v = u ? usat(v, lane, &sat) : ssat(v, lane, &sat);
        } else {
            v >>= 1; /* halving: arithmetic on the wide result */
        }
        res |= ((uint32_t)v & lmask) << (lane * i);
    }
    if (op2 == 0u) {
        set_ge(c, g);
    }
    c->r[rd] = res;
    return OK;
}

/* Data processing (register): 11111010 */
static X t32_dp_register(armv7m_cpu_t *c, uint16_t w0, uint16_t w1)
{
    const uint32_t op1 = (w0 >> 4) & 15u;
    const uint32_t op2 = (w1 >> 4) & 15u;
    const uint32_t rn = w0 & 15u;
    const uint32_t rd = (w1 >> 8) & 15u;
    const uint32_t rm = w1 & 15u;

    if ((w1 & 0xF000u) != 0xF000u) {
        return ARMV7M_X_UNDEF;
    }
    if ((op1 >> 3) == 0u && op2 == 0u) { /* LSL, LSR, ASR, ROR (register) */
        uint32_t cy;
        const uint32_t res =
            armv7m_shift_c(c->r[rn], (op1 >> 1) & 3u, c->r[rm] & 0xFFu, carry(c), &cy);

        c->r[rd] = res;
        if ((op1 & 1u) != 0u) {
            set_nzc(c, res, cy);
        }
        return OK;
    }
    if ((op1 >> 3) == 0u && (op2 & 8u) != 0u) { /* the extends */
        const uint32_t v = ror8(c->r[rm], ((w1 >> 4) & 3u) * 8u);

        if ((w1 & 0x0040u) != 0u) {
            return ARMV7M_X_UNDEF;
        }
        const uint32_t add = (rn == ARMV7M_PC) ? 0u : c->r[rn];
        uint32_t res;

        switch (op1) {
        case 0u:
            res = add + (uint32_t)(int32_t)(int16_t)v;
            break;
        case 1u:
            res = add + (v & 0xFFFFu);
            break;
        case 2u: /* SXTAB16 / SXTB16: two lanes */
            res = ((add & 0xFFFFu) + (uint32_t)(int32_t)(int8_t)v) & 0xFFFFu;
            res |= ((add >> 16) + (uint32_t)(int32_t)(int8_t)(v >> 16)) << 16;
            break;
        case 3u:
            res = ((add & 0xFFFFu) + (v & 0xFFu)) & 0xFFFFu;
            res |= ((add >> 16) + ((v >> 16) & 0xFFu)) << 16;
            break;
        case 4u:
            res = add + (uint32_t)(int32_t)(int8_t)v;
            break;
        case 5u:
            res = add + (v & 0xFFu);
            break;
        default:
            return ARMV7M_X_UNDEF;
        }
        c->r[rd] = res;
        return OK;
    }
    if ((op1 >> 3) == 1u && (op2 >> 3) == 0u) { /* parallel */
        return t32_parallel(c, op1 & 7u, op2 & 3u, (op2 & 4u) != 0u, rd,
                            c->r[rn], c->r[rm]);
    }
    if ((op1 >> 2) == 2u && (op2 >> 2) == 2u) { /* miscellaneous */
        const uint32_t m = c->r[rm];
        const uint32_t sel = ((op1 & 3u) << 2) | (op2 & 3u);

        switch (sel) {
        case 0x0u:   /* QADD:  rm + rn */
        case 0x1u:   /* QDADD: rm + sat(2 * rn) */
        case 0x2u:   /* QSUB:  rm - rn */
        case 0x3u: { /* QDSUB: rm - sat(2 * rn) */
            bool sat = false;
            int64_t n = (int32_t)c->r[rn];

            if ((sel & 1u) != 0u) {
                n = ssat(2 * n, 32u, &sat);
            }
            const int64_t v = (sel & 2u) ? (int64_t)(int32_t)m - n
                                         : (int64_t)(int32_t)m + n;

            c->r[rd] = (uint32_t)ssat(v, 32u, &sat);
            if (sat) {
                set_q(c);
            }
            return OK;
        }
        case 0x4u:
            c->r[rd] = __builtin_bswap32(m);
            return OK;
        case 0x5u:
            c->r[rd] = ((m & 0x00FF00FFu) << 8) | ((m >> 8) & 0x00FF00FFu);
            return OK;
        case 0x6u: { /* RBIT */
            uint32_t v = m;

            v = ((v >> 1) & 0x55555555u) | ((v & 0x55555555u) << 1);
            v = ((v >> 2) & 0x33333333u) | ((v & 0x33333333u) << 2);
            v = ((v >> 4) & 0x0F0F0F0Fu) | ((v & 0x0F0F0F0Fu) << 4);
            c->r[rd] = __builtin_bswap32(v);
            return OK;
        }
        case 0x7u:
            c->r[rd] = (uint32_t)(int32_t)(int16_t)(uint16_t)(((m & 0xFFu) << 8) |
                                                              ((m >> 8) & 0xFFu));
            return OK;
        case 0x8u: { /* SEL */
            const uint32_t g = ge(c);
            const uint32_t n = c->r[rn];
            uint32_t res = 0u;

            for (uint32_t i = 0u; i < 4u; i++) {
                const uint32_t b = 0xFFu << (8u * i);

                res |= ((g >> i) & 1u) ? (n & b) : (m & b);
            }
            c->r[rd] = res;
            return OK;
        }
        case 0xCu:
            c->r[rd] = (m == 0u) ? 32u : (uint32_t)__builtin_clz(m);
            return OK;
        default:
            return ARMV7M_X_UNDEF;
        }
    }
    return ARMV7M_X_UNDEF;
}

/* ------------------------------------------------------------------ */
/* 32-bit: multiplies                                                  */
/* ------------------------------------------------------------------ */

static inline int32_t half(uint32_t v, bool top)
{
    return top ? (int32_t)(int16_t)(v >> 16) : (int32_t)(int16_t)v;
}

/* Multiply, multiply accumulate, absolute difference: 111110110 */
static X t32_mul(armv7m_cpu_t *c, uint16_t w0, uint16_t w1)
{
    const uint32_t op1 = (w0 >> 4) & 7u;
    const uint32_t op2 = (w1 >> 4) & 3u;
    const uint32_t rn = w0 & 15u;
    const uint32_t ra = (w1 >> 12) & 15u;
    const uint32_t rd = (w1 >> 8) & 15u;
    const uint32_t rm = w1 & 15u;
    const uint32_t n = c->r[rn];
    const uint32_t m = c->r[rm];
    const bool acc = ra != ARMV7M_PC;
    const int64_t a = acc ? (int64_t)(int32_t)c->r[ra] : 0;

    if ((w1 & 0x00C0u) != 0u) {
        return ARMV7M_X_UNDEF;
    }
    switch (op1) {
    case 0u:
        if (op2 == 0u) { /* MLA / MUL */
            c->r[rd] = n * m + (acc ? c->r[ra] : 0u);
            return OK;
        }
        if (op2 == 1u && acc) { /* MLS */
            c->r[rd] = c->r[ra] - n * m;
            return OK;
        }
        return ARMV7M_X_UNDEF;
    case 1u: { /* SMLA<x><y> / SMUL<x><y>: N is bit 5, M bit 4 */
        const int64_t p = (int64_t)half(n, (op2 & 2u) != 0u) *
                          half(m, (op2 & 1u) != 0u);
        const int64_t v = p + a;

        c->r[rd] = (uint32_t)v;
        if (acc && v != (int64_t)(int32_t)v) {
            set_q(c);
        }
        return OK;
    }
    case 2u:   /* SMLAD / SMUAD */
    case 4u: { /* SMLSD / SMUSD */
        const uint32_t mm = (op2 & 1u) ? ror8(m, 16u) : m;
        const int64_t p1 = (int64_t)half(n, false) * half(mm, false);
        const int64_t p2 = (int64_t)half(n, true) * half(mm, true);
        const int64_t v = ((op1 == 2u) ? p1 + p2 : p1 - p2) + a;

        if (op2 > 1u) {
            return ARMV7M_X_UNDEF;
        }
        c->r[rd] = (uint32_t)v;
        if (v != (int64_t)(int32_t)v) {
            set_q(c);
        }
        return OK;
    }
    case 3u: { /* SMLAW<y> / SMULW<y> */
        const int64_t p = (int64_t)(int32_t)n * half(m, (op2 & 1u) != 0u);
        const int64_t v = (p >> 16) + a;

        if (op2 > 1u) {
            return ARMV7M_X_UNDEF;
        }
        c->r[rd] = (uint32_t)v;
        if (acc && v != (int64_t)(int32_t)v) {
            set_q(c);
        }
        return OK;
    }
    case 5u:   /* SMMLA / SMMUL */
    case 6u: { /* SMMLS */
        const int64_t p = (int64_t)(int32_t)n * (int64_t)(int32_t)m;
        int64_t v = (int64_t)((uint64_t)a << 32);

        if (op2 > 1u || (op1 == 6u && !acc)) {
            return ARMV7M_X_UNDEF;
        }
        v = (op1 == 5u) ? v + p : v - p;
        if ((op2 & 1u) != 0u) {
            v += 0x80000000;
        }
        c->r[rd] = (uint32_t)((uint64_t)v >> 32);
        return OK;
    }
    default: { /* USAD8 / USADA8 */
        uint32_t s = 0u;

        if (op2 != 0u) {
            return ARMV7M_X_UNDEF;
        }
        for (uint32_t i = 0u; i < 4u; i++) {
            const int32_t d = (int32_t)((n >> (8u * i)) & 0xFFu) -
                              (int32_t)((m >> (8u * i)) & 0xFFu);

            s += (uint32_t)((d < 0) ? -d : d);
        }
        c->r[rd] = s + (acc ? c->r[ra] : 0u);
        return OK;
    }
    }
}

/* Long multiply, long multiply accumulate, and divide: 111110111 */
static X t32_mull(armv7m_cpu_t *c, uint16_t w0, uint16_t w1)
{
    const uint32_t op1 = (w0 >> 4) & 7u;
    const uint32_t op2 = (w1 >> 4) & 15u;
    const uint32_t rn = w0 & 15u;
    const uint32_t lo = (w1 >> 12) & 15u;
    const uint32_t hi = (w1 >> 8) & 15u;
    const uint32_t rm = w1 & 15u;
    const uint32_t n = c->r[rn];
    const uint32_t m = c->r[rm];
    const uint64_t acc = ((uint64_t)c->r[hi] << 32) | c->r[lo];
    uint64_t v;

    /* One register cannot hold both halves; the M7 refuses the encoding. */
    if (op1 != 1u && op1 != 3u && lo == hi) {
        return ARMV7M_X_UNDEF;
    }
    switch (op1) {
    case 0u:
        if (op2 != 0u) {
            return ARMV7M_X_UNDEF;
        }
        v = (uint64_t)((int64_t)(int32_t)n * (int32_t)m);
        break;
    case 2u:
        if (op2 != 0u) {
            return ARMV7M_X_UNDEF;
        }
        v = (uint64_t)n * m;
        break;
    case 1u:   /* SDIV */
    case 3u: { /* UDIV */
        if (op2 != 15u || lo != 15u) {
            return ARMV7M_X_UNDEF;
        }
        /*
         * A division by zero is 0 unless CCR.DIV_0_TRP is set, in which
         * case it is a UsageFault -- the architecture's answer, not an
         * approximation, and the reason the IR's EMU_IR_DIV* carries a
         * precondition.
         */
        if (m == 0u) {
            if ((c->ccr & CCR_DIV_0_TRP) != 0u) {
                return ARMV7M_X_DIVBYZERO;
            }
            c->r[hi] = 0u;
        } else if (op1 == 3u) {
            c->r[hi] = n / m;
        } else if (n == 0x80000000u && m == 0xFFFFFFFFu) {
            c->r[hi] = 0x80000000u;
        } else {
            c->r[hi] = (uint32_t)((int32_t)n / (int32_t)m);
        }
        return OK;
    }
    case 4u:
        if (op2 == 0u) { /* SMLAL */
            v = acc + (uint64_t)((int64_t)(int32_t)n * (int32_t)m);
        } else if ((op2 & 0xCu) == 0x8u) { /* SMLAL<x><y> */
            v = acc + (uint64_t)((int64_t)half(n, (op2 & 2u) != 0u) *
                                 half(m, (op2 & 1u) != 0u));
        } else if ((op2 & 0xEu) == 0xCu) { /* SMLALD */
            const uint32_t mm = (op2 & 1u) ? ror8(m, 16u) : m;

            v = acc + (uint64_t)((int64_t)half(n, false) * half(mm, false) +
                                 (int64_t)half(n, true) * half(mm, true));
        } else {
            return ARMV7M_X_UNDEF;
        }
        break;
    case 5u: { /* SMLSLD */
        const uint32_t mm = (op2 & 1u) ? ror8(m, 16u) : m;

        if ((op2 & 0xEu) != 0xCu) {
            return ARMV7M_X_UNDEF;
        }
        v = acc + (uint64_t)((int64_t)half(n, false) * half(mm, false) -
                             (int64_t)half(n, true) * half(mm, true));
        break;
    }
    case 6u:
        if (op2 == 0u) { /* UMLAL */
            v = acc + (uint64_t)n * m;
        } else if (op2 == 6u) { /* UMAAL */
            v = (uint64_t)n * m + c->r[lo] + c->r[hi];
        } else {
            return ARMV7M_X_UNDEF;
        }
        break;
    default:
        return ARMV7M_X_UNDEF;
    }
    c->r[lo] = (uint32_t)v;
    c->r[hi] = (uint32_t)(v >> 32);
    return OK;
}

/* ------------------------------------------------------------------ */
/* 32-bit: loads and stores                                            */
/* ------------------------------------------------------------------ */

/*
 * Every single load and store, 1111 100x: size, sign and direction from
 * w0, addressing mode from w1 -- the T2/T3 imm12, the T4 imm8 with its
 * P/U/W bits (and the unprivileged forms, which are P=1 U=1 W=0), the
 * register offset, and the pc-relative literal.
 *
 * **Rt == pc on a narrow load is a preload hint**, not a load into the
 * pc: PLD and PLI live in exactly those slots and must do nothing --
 * including not faulting on an address nobody can read.
 */
static X t32_ldst(armv7m_cpu_t *c, uint16_t w0, uint16_t w1, uint32_t pc)
{
    const uint32_t sz = (w0 >> 5) & 3u;
    const bool load = ((w0 >> 4) & 1u) != 0u;
    const bool sext = ((w0 >> 8) & 1u) != 0u;
    const uint32_t rn = w0 & 15u;
    const uint32_t rt = (w1 >> 12) & 15u;
    const uint32_t size = 1u << sz;
    bool index = true;
    bool add = true;
    bool wback = false;
    bool unpriv = false;
    uint32_t off;

    /*
     * A word has no sign to extend, so bit 24 on a word load selects no
     * row of the table. **This executed for as long as only assembled
     * instructions were tested**: `f9d0 0004` loaded exactly as
     * `f8d0 0004` does, and a Cortex-M7 raises UNDEFINSTR.
     */
    if (sz == 3u || (!load && sext) || (load && sz == 2u && sext)) {
        return ARMV7M_X_UNDEF;
    }
    if (rn == ARMV7M_PC) {
        if (!load) {
            return ARMV7M_X_UNDEF;
        }
        add = ((w0 >> 7) & 1u) != 0u;
        off = w1 & 0xFFFu;
    } else if (((w0 >> 7) & 1u) != 0u) { /* imm12, add */
        if (sext && !load) {
            return ARMV7M_X_UNDEF;
        }
        off = w1 & 0xFFFu;
    } else if ((w1 & 0x0800u) != 0u) { /* imm8 with P/U/W */
        const uint32_t puw = (w1 >> 8) & 7u;

        off = w1 & 0xFFu;
        index = (puw & 4u) != 0u;
        add = (puw & 2u) != 0u;
        wback = (puw & 1u) != 0u;
        if (!index && !wback) {
            return ARMV7M_X_UNDEF;
        }
        if (puw == 6u) { /* LDRT and friends */
            unpriv = true;
        }
    } else if ((w1 & 0x0FC0u) == 0u) { /* register, LSL #0-3 */
        off = c->r[w1 & 15u] << ((w1 >> 4) & 3u);
    } else {
        return ARMV7M_X_UNDEF;
    }

    const uint32_t base = (rn == ARMV7M_PC) ? ((pc + 4u) & ~3u) : c->r[rn];
    const uint32_t offaddr = add ? base + off : base - off;
    const uint32_t addr = index ? offaddr : base;

    if (load && rt == ARMV7M_PC && size < 4u) {
        return OK; /* PLD, PLI, and the unallocated hints */
    }
    if (load) {
        uint32_t v = 0u;
        const X x = rd_u(c, addr, size, unpriv, &v);

        if (x != OK) {
            return x;
        }
        if (wback) {
            c->r[rn] = offaddr;
        }
        if (rt == ARMV7M_PC) {
            bx_write_pc(c, v);
        } else {
            c->r[rt] = extend(v, size, sext);
        }
        return OK;
    }
    {
        const X x = wr_u(c, addr, size, unpriv, c->r[rt]);

        if (x == OK && wback) {
            c->r[rn] = offaddr;
        }
        return x;
    }
}

/* LDRD and STRD, immediate and literal: word aligned, or a UsageFault. */
static X t32_dual(armv7m_cpu_t *c, uint16_t w0, uint16_t w1, uint32_t pc)
{
    const uint32_t rn = w0 & 15u;
    const uint32_t rt = (w1 >> 12) & 15u;
    const uint32_t rt2 = (w1 >> 8) & 15u;
    const uint32_t imm8 = w1 & 0xFFu;
    const bool index = ((w0 >> 8) & 1u) != 0u;
    const bool add = ((w0 >> 7) & 1u) != 0u;
    const bool wback = ((w0 >> 5) & 1u) != 0u;
    const bool load = ((w0 >> 4) & 1u) != 0u;
    const uint32_t base = (rn == ARMV7M_PC) ? ((pc + 4u) & ~3u) : c->r[rn];
    const uint32_t offaddr = add ? base + imm8 * 4u : base - imm8 * 4u;
    const uint32_t addr = index ? offaddr : base;

    if (load) {
        uint32_t v1 = 0u;
        uint32_t v2 = 0u;
        X x;

        if (rt == rt2) {
            return ARMV7M_X_UNDEF; /* two words into one register */
        }
        x = rd_a(c, addr, 4u, &v1);
        if (x == OK) {
            x = rd_a(c, addr + 4u, 4u, &v2);
        }
        if (x != OK) {
            return x;
        }
        if (wback) {
            c->r[rn] = offaddr;
        }
        c->r[rt] = v1;
        c->r[rt2] = v2;
        return OK;
    }
    {
        X x = wr_a(c, addr, 4u, c->r[rt]);

        if (x == OK) {
            x = wr_a(c, addr + 4u, 4u, c->r[rt2]);
        }
        if (x == OK && wback) {
            c->r[rn] = offaddr;
        }
        return x;
    }
}

/* Load/store dual or exclusive, table branch: 1110100xx1 */
static X t32_dual_excl(armv7m_cpu_t *c, uint16_t w0, uint16_t w1, uint32_t pc)
{
    const uint32_t op1 = (w0 >> 7) & 3u;
    const uint32_t op2 = (w0 >> 4) & 3u;
    const uint32_t op3 = (w1 >> 4) & 15u;
    const uint32_t rn = w0 & 15u;
    const uint32_t rt = (w1 >> 12) & 15u;
    const uint32_t rt2 = (w1 >> 8) & 15u;
    const uint32_t imm8 = w1 & 0xFFu;

    /*
     * **LDRD/STRD first.** Table A5-17 gives them every encoding with
     * op1<1> or op2<1> set, and the exclusives and table branches only
     * what is left. Checked the other way round, a post-indexed STRD
     * whose second halfword happened to have bits 7:4 = 0100 decoded as
     * STREXB -- the first case of the board harness to stop.
     */
    if ((op1 & 2u) != 0u || (op2 & 2u) != 0u) {
        return t32_dual(c, w0, w1, pc);
    }
    if (op1 == 0u) { /* STREX / LDREX */
        const uint32_t addr = c->r[rn] + imm8 * 4u;

        if (op2 == 1u) {
            uint32_t v = 0u;
            X x;

            if (rt2 != 15u) {
                return ARMV7M_X_UNDEF; /* (1)(1)(1)(1) where STREX has Rd */
            }
            x = rd_a(c, addr, 4u, &v);

            if (x != OK) {
                return x;
            }
            c->excl_open = true;
            c->excl_addr = addr;
            c->r[rt] = v;
            return OK;
        }
        if ((addr & 3u) != 0u) {
            return ARMV7M_X_UNALIGNED;
        }
        if (c->excl_open) {
            const X x = wr_a(c, addr, 4u, c->r[rt]);

            if (x != OK) {
                return x;
            }
            c->r[rt2] = 0u;
        } else {
            c->r[rt2] = 1u;
        }
        c->excl_open = false;
        return OK;
    }
    if (op1 == 1u && op2 == 1u && (op3 == 0u || op3 == 1u)) { /* TBB, TBH */
        const uint32_t base = (rn == ARMV7M_PC) ? pc + 4u : c->r[rn];
        const uint32_t m = c->r[w1 & 15u];
        uint32_t e = 0u;
        X x;

        if ((w1 & 0xFF00u) != 0xF000u) {
            return ARMV7M_X_UNDEF;
        }
        x = (op3 == 0u) ? armv7m_mem_read(c, base + m, 1u, false, &e)
                        : rd_u(c, base + (m << 1), 2u, false, &e);
        if (x != OK) {
            return x;
        }
        c->r[ARMV7M_PC] = pc + 4u + 2u * e;
        return OK;
    }
    if (op1 == 1u && (op3 == 4u || op3 == 5u)) { /* the byte/half exclusives */
        const uint32_t size = (op3 == 4u) ? 1u : 2u;
        const uint32_t addr = c->r[rn];

        if (rt2 != 15u || (op2 == 1u && (w1 & 15u) != 15u)) {
            return ARMV7M_X_UNDEF;
        }
        if (op2 == 1u) {
            uint32_t v = 0u;
            const X x = rd_a(c, addr, size, &v);

            if (x != OK) {
                return x;
            }
            c->excl_open = true;
            c->excl_addr = addr;
            c->r[rt] = v;
            return OK;
        }
        if (op2 != 0u) {
            return ARMV7M_X_UNDEF;
        }
        if ((addr & (size - 1u)) != 0u) {
            return ARMV7M_X_UNALIGNED;
        }
        if (c->excl_open) {
            const X x = wr_a(c, addr, size, c->r[rt]);

            if (x != OK) {
                return x;
            }
            c->r[w1 & 15u] = 0u;
        } else {
            c->r[w1 & 15u] = 1u;
        }
        c->excl_open = false;
        return OK;
    }
    return ARMV7M_X_UNDEF;
}

/* ------------------------------------------------------------------ */
/* 32-bit: branches and miscellaneous control                          */
/* ------------------------------------------------------------------ */

static X t32_branch_misc(armv7m_cpu_t *c, uint16_t w0, uint16_t w1, uint32_t pc)
{
    const uint32_t op = (w0 >> 4) & 0x7Fu;
    const uint32_t op1 = (w1 >> 12) & 7u;
    const uint32_t s = (w0 >> 10) & 1u;
    const uint32_t j1 = (w1 >> 13) & 1u;
    const uint32_t j2 = (w1 >> 11) & 1u;

    if ((op1 & 5u) == 0u) { /* 0x0 */
        if ((op & 0x38u) != 0x38u) { /* B<cond>.W */
            const uint32_t cond = (w0 >> 6) & 15u;
            int32_t off = (int32_t)((s << 20) | (j2 << 19) | (j1 << 18) |
                                    ((uint32_t)(w0 & 0x3Fu) << 12) |
                                    ((uint32_t)(w1 & 0x7FFu) << 1));

            if (armv7m_in_it(c->xpsr)) {
                return ARMV7M_X_UNDEF;
            }
            if (s != 0u) {
                off -= 0x200000;
            }
            if (cond_holds(c, cond)) {
                c->r[ARMV7M_PC] = (uint32_t)((int32_t)(pc + 4u) + off);
            }
            return OK;
        }
        if ((op & 0x7Eu) == 0x38u) { /* MSR */
            const uint32_t mask = (w1 >> 10) & 3u;

            if (op != 0x38u || (w1 & 0x2300u) != 0u || mask == 0u ||
                !armv7m_sysm_valid(w1 & 0xFFu)) {
                return ARMV7M_X_UNDEF;
            }
            msr(c, w1 & 0xFFu, mask, c->r[w0 & 15u]);
            return OK;
        }
        if (op == 0x3Au) { /* hints */
            /*
             * Not strict, unlike everything around it: the board runs a
             * hint whose should-be bits are wrong.
             */
            if (((w1 >> 8) & 7u) != 0u) {
                return ARMV7M_X_UNDEF; /* the A/R-profile CPS */
            }
            return OK; /* NOP, YIELD, WFE, WFI, SEV, CSDB, DBG */
        }
        if (op == 0x3Bu) { /* misc control */
            if ((w0 & 15u) != 15u || (w1 & 0x2F00u) != 0x0F00u) {
                return ARMV7M_X_UNDEF;
            }
            switch ((w1 >> 4) & 15u) {
            case 2u: /* CLREX */
                if ((w1 & 15u) != 15u) {
                    return ARMV7M_X_UNDEF;
                }
                c->excl_open = false;
                return OK;
            case 4u: /* DSB (and SSBB, PSSBB) */
            case 5u: /* DMB */
            case 6u: /* ISB */
                return OK;
            default:
                return ARMV7M_X_UNDEF;
            }
        }
        if ((op & 0x7Eu) == 0x3Eu) { /* MRS */
            const uint32_t rd = (w1 >> 8) & 15u;

            if (rd == ARMV7M_SP || rd == ARMV7M_PC || op != 0x3Eu ||
                (w0 & 15u) != 15u || (w1 & 0x2000u) != 0u ||
                !armv7m_sysm_valid(w1 & 0xFFu)) {
                return ARMV7M_X_UNDEF;
            }
            c->r[rd] = mrs(c, w1 & 0xFFu);
            return OK;
        }
        return ARMV7M_X_UNDEF;
    }
    if ((op1 & 5u) == 1u || (op1 & 5u) == 5u) { /* B.W, BL */
        const uint32_t i1 = (~(j1 ^ s)) & 1u;
        const uint32_t i2 = (~(j2 ^ s)) & 1u;
        int32_t off = (int32_t)((s << 24) | (i1 << 23) | (i2 << 22) |
                                ((uint32_t)(w0 & 0x3FFu) << 12) |
                                ((uint32_t)(w1 & 0x7FFu) << 1));

        if (s != 0u) {
            off -= 0x2000000;
        }
        if ((op1 & 4u) != 0u) {
            c->r[ARMV7M_LR] = (pc + 4u) | 1u;
        } else if (armv7m_in_it(c->xpsr) &&
                   (armv7m_it_get(c->xpsr) & 7u) != 0u) {
            /* B inside an IT block must be its last instruction. */
            return ARMV7M_X_UNDEF;
        }
        c->r[ARMV7M_PC] = (uint32_t)((int32_t)(pc + 4u) + off);
        return OK;
    }
    return ARMV7M_X_UNDEF; /* UDF, and BLX (immediate), which v7-M lacks */
}

static X exec32(armv7m_cpu_t *c, uint16_t w0, uint16_t w1, uint32_t pc)
{
    const uint32_t op1 = (w0 >> 11) & 3u;
    const uint32_t op2 = (w0 >> 4) & 0x7Fu;

    if (op1 == 1u) {
        if ((op2 & 0x64u) == 0x00u) { /* 00xx0xx: LDM/STM */
            const uint32_t op = (w0 >> 7) & 3u;
            const bool load = ((w0 >> 4) & 1u) != 0u;
            const bool wback = ((w0 >> 5) & 1u) != 0u;
            const uint32_t rn = w0 & 15u;

            if (op != 1u && op != 2u) {
                return ARMV7M_X_UNDEF;
            }
            if ((w1 & (1u << 13)) != 0u || (!load && (w1 & (1u << 15)) != 0u) ||
                rn == ARMV7M_PC) {
                return ARMV7M_X_UNDEF;
            }
            return ldm_stm(c, rn, w1, load, op == 2u, wback);
        }
        if ((op2 & 0x64u) == 0x04u) { /* 00xx1xx: dual, exclusive, table */
            return t32_dual_excl(c, w0, w1, pc);
        }
        if ((op2 & 0x60u) == 0x20u) { /* 01xxxxx: DP shifted register */
            return t32_dp_reg(c, w0, w1);
        }
        return armv7m_fpu_exec(c, w0, w1, pc); /* 1xxxxxx: coprocessor */
    }
    if (op1 == 2u) {
        if ((w1 & 0x8000u) != 0u) {
            return t32_branch_misc(c, w0, w1, pc);
        }
        if ((op2 & 0x20u) == 0u) {
            return t32_dp_imm(c, w0, w1);
        }
        return t32_dp_plain(c, w0, w1, pc);
    }
    /* op1 == 3 */
    if ((op2 & 0x71u) == 0x00u || (op2 & 0x67u) == 0x01u ||
        (op2 & 0x67u) == 0x03u || (op2 & 0x67u) == 0x05u) {
        return t32_ldst(c, w0, w1, pc);
    }
    if ((op2 & 0x70u) == 0x20u) {
        return t32_dp_register(c, w0, w1);
    }
    if ((op2 & 0x78u) == 0x30u) {
        return t32_mul(c, w0, w1);
    }
    if ((op2 & 0x78u) == 0x38u) {
        return t32_mull(c, w0, w1);
    }
    if ((op2 & 0x40u) != 0u) {
        return armv7m_fpu_exec(c, w0, w1, pc);
    }
    return ARMV7M_X_UNDEF;
}

/* ------------------------------------------------------------------ */
/* One instruction                                                     */
/* ------------------------------------------------------------------ */

/*
 * Execute one fetched instruction: its IT condition, the instruction,
 * and -- if it completed -- ITAdvance. A fault is *returned*, with the pc
 * put back, and has not been raised.
 *
 * **One copy, for two callers.** The run loop below and the translator's
 * fallback (armv7m_step_insn) both come through here, because the other
 * frontend with a JIT kept a second copy of its run loop's logic for the
 * translated path and has been bitten by it three times. Inlined into
 * the loop, so sharing it costs the interpreter nothing.
 */
static inline X step(armv7m_cpu_t *c, uint16_t hw, uint16_t w1, uint32_t pc,
                     uint32_t len)
{
    /*
     * Whether this instruction runs at all, read before it executes:
     * ITAdvance happens after, and the condition and the setflags rule
     * are both about the state this instruction sees. A failed
     * condition still retires and still advances ITSTATE -- it is
     * executed-as-a-NOP, not skipped.
     */
    const bool was_in_it = armv7m_in_it(c->xpsr);
    const bool runs = !was_in_it || cond_holds(c, armv7m_it_get(c->xpsr) >> 4);
    X x = OK;

    c->r[ARMV7M_PC] = pc + len;
    if (runs) {
        x = (len == 4u) ? exec32(c, hw, w1, pc) : exec16(c, hw, pc);
    }
    if (x == OK || x == ARMV7M_X_SVC) {
        /* SVC completes; the exception it raises returns to the next one. */
        if (was_in_it) {
            it_advance(c);
        }
        return x;
    }
    if (x != ARMV7M_X_BKPT) {
        /*
         * A fault: the instruction did not happen, and the exception
         * returns to it. The pc and encoding are kept even though the
         * fault is *taken* -- a guest with no handler ends in lockup,
         * and the lockup report has to be able to say what started it.
         */
        c->fault_pc = pc;
        c->fault_insn = (len == 4u) ? (((uint32_t)hw << 16) | w1) : hw;
    }
    c->r[ARMV7M_PC] = pc;
    return x;
}

bool armv7m_step_insn(armv7m_cpu_t *c, uint16_t w0, uint16_t w1, uint32_t pc)
{
    const uint32_t len = armv7m_insn_len(w0);
    const X x = step(c, w0, w1, pc, len);

    if (x == OK) {
        return true;
    }
    if (x == ARMV7M_X_BKPT) {
        c->state = EMU_STATE_HALTED;
    } else {
        armv7m_raise(c, x, (x == ARMV7M_X_SVC) ? pc + len : pc);
    }
    return false;
}

armv7m_exc_t armv7m_load_u(armv7m_cpu_t *c, uint32_t addr, uint32_t size,
                           uint32_t *out)
{
    return rd_u(c, addr, size, false, out);
}

armv7m_exc_t armv7m_store_u(armv7m_cpu_t *c, uint32_t addr, uint32_t size,
                            uint32_t v)
{
    return wr_u(c, addr, size, false, v);
}

/* ------------------------------------------------------------------ */
/* The run loop                                                        */
/* ------------------------------------------------------------------ */

emu_run_reason_t armv7m_run(armv7m_cpu_t *c, uint32_t budget, uint32_t *retired)
{
    uint32_t done = 0u;

    while (done < budget) {
        if (c->state != EMU_STATE_RUNNING) {
            break;
        }

        /*
         * **EXC_RETURN is recognised here, before the fetch**, because
         * there is nothing at 0xFFFFFFF9 to fetch. A handler returns by
         * branching to a magic address, and the core recognises it as an
         * address rather than as an instruction.
         */
        if (armv7m_handler_mode(c) &&
            (c->r[ARMV7M_PC] & ARMV7M_EXC_RETURN_MASK) == ARMV7M_EXC_RETURN_MASK) {
            const X x = armv7m_exc_return(c, c->r[ARMV7M_PC]);

            if (x != OK) {
                armv7m_raise(c, x, c->r[ARMV7M_PC]);
            }
            continue;
        }

        /*
         * An asynchronous exception, if one is pending and can preempt.
         * Between instructions only: the architecture permits taking one
         * partway through a long LDM, and doing so would need the
         * instruction restartable from its ICI state, which none of these
         * are written to be.
         */
        {
            bool preempts = false;
            const uint32_t exc = armv7m_pending_exc(c, &preempts);

            if (preempts) {
                (void)armv7m_take(c, exc, c->r[ARMV7M_PC]);
                continue;
            }
        }

        const uint32_t pc = c->r[ARMV7M_PC];

        if ((c->xpsr & ARMV7M_T) == 0u) {
            armv7m_raise(c, ARMV7M_X_INVSTATE, pc);
            continue;
        }

        uint16_t hw = 0u;
        {
            const X x = armv7m_mem_fetch16(c, pc, &hw);

            if (x != OK) {
                armv7m_raise(c, x, pc);
                continue;
            }
        }

        const uint32_t len = armv7m_insn_len(hw);
        uint16_t w1 = 0u;

        if (len == 4u) {
            /*
             * The second halfword gets its own fetch, because it can sit
             * in a different MPU region from the first -- the straddle
             * case the RV32 frontend had to learn about separately.
             */
            const X x = armv7m_mem_fetch16(c, pc + 2u, &w1);

            if (x != OK) {
                armv7m_raise(c, x, pc);
                continue;
            }
        }

#if EMU_PAIR_STATS
        emu_pair_note(&armv7m_pair_ops, pc, (uint64_t)hw | ((uint64_t)w1 << 16),
                      len);
#endif

#if EMU_ENABLE_TRACE
        if (c->trace != NULL) {
            c->trace((emu_cpu_t *)c, pc, (uint64_t)hw | ((uint64_t)w1 << 16), len,
                     c->trace_user);
        }
#endif

        const X x = step(c, hw, w1, pc, len);

        if (x == ARMV7M_X_BKPT) {
            /* The emulator's convention for "this guest is finished". */
            c->state = EMU_STATE_HALTED;
            done++;
            c->retired++;
            break;
        }
        if (x == ARMV7M_X_SVC) {
            done++;
            c->retired++;
            armv7m_raise(c, x, pc + len);
            continue;
        }
        if (x != OK) {
            armv7m_raise(c, x, pc);
            continue;
        }

        done++;
        c->retired++;

        /*
         * **Per instruction, not once per slice.** Ticking in bulk sets
         * the pending latch once however many times the counter wrapped,
         * so at most one SysTick could ever be delivered per slice.
         */
        if ((c->systick_ctrl & 1u) != 0u) {
            armv7m_systick_tick(c, 1u);
        }
    }

    if (retired != NULL) {
        *retired = done;
    }
    if (c->state == EMU_STATE_HALTED) {
        return EMU_RUN_HALTED;
    }
    return EMU_RUN_BUDGET;
}
