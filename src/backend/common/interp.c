/* SPDX-License-Identifier: Apache-2.0 */
/*
 * src/backend/common/interp.c - Executing IR directly, on any host.
 *
 * The other half of a backend. `jit.c` compiles a block to machine code;
 * this runs the same block without emitting anything, and the two are
 * required to agree.
 *
 * Two jobs, and the second is the one that pays here:
 *
 *   - fallback. A host with no JIT, or a block the lowering declines,
 *     still has to run. Declining costs more than translating badly --
 *     this project measured that ending a block for one untranslatable
 *     instruction fragments hot code -- and an IR interpreter means a
 *     backend never has to decline a whole block, only a compilation.
 *
 *   - a reference. It shares no code with the lowering: this evaluates
 *     the IR in C, that emits x86-64 bytes, and the only thing they have
 *     in common is emu_ir.h. So running a block both ways and comparing
 *     guest state is a differential check between two genuinely
 *     different implementations, which is the same discipline this
 *     project already uses for SoftFloat against VFP. It is how the
 *     lzcnt-decodes-as-bsr bug would have been caught without a
 *     hand-written expectation.
 *
 * Shared rather than one copy per host directory.
 *
 * It started under src/backend/x86_64/ with a note saying nothing in it
 * was x86-specific yet, and that the day a second host copied it would
 * be the moment to ask whether it should be shared. Thumb-2 is that
 * second host, and the answer was yes: this evaluates the IR in C, and
 * C is the same on both. A host that ever does want to specialise it can
 * take its own copy then, with a reason.
 *
 * src/backend/common/ is where that lands: a backend concern rather than
 * an emu one, but not owned by any single host.
 */

#include "emu/emu_ir.h"

#include <string.h>


/* Where guest register `n` lives inside the frontend's state. */
static uint32_t *reg_ptr(emu_cpu_t *cpu, const emu_ir_target_t *t, uint32_t n)
{
    return (uint32_t *)(void *)((uint8_t *)cpu + t->reg_offset(n));
}

static uint32_t *word_at(emu_cpu_t *cpu, uint32_t off)
{
    return (uint32_t *)(void *)((uint8_t *)cpu + off);
}

/*
 * Derive the guest's flags from a result, exactly as the lowering does
 * from the host's flag register.
 *
 * Only the bits in `live` are touched, and the rest of the flag word is
 * preserved: a guest's condition flags share a register with state that
 * has nothing to do with arithmetic -- G4MH's PSW carries the
 * interrupt-disable and privilege bits -- and clobbering those turns an
 * arithmetic bug into a control-flow one.
 */
static void apply_flags(emu_cpu_t *cpu, const emu_ir_target_t *t,
                        const emu_ir_insn_t *in, const uint32_t *tmp)
{
    const uint8_t live = in->live;
    if (live == 0u) {
        return;
    }

    const uint32_t res = (in->a != EMU_IR_NO_TEMP) ? tmp[in->a] : 0u;
    const uint32_t rhs = (in->b != EMU_IR_NO_TEMP) ? tmp[in->b] : 0u;

    bool z = false, s = false, v = false, c = false;

    switch ((emu_ir_flagsrc_t)in->aux) {
    case EMU_IR_FS_ADD: {
        const uint32_t sum = res + rhs;
        z = (sum == 0u);
        s = (sum & 0x80000000u) != 0u;
        c = (sum < res);
        v = ((~(res ^ rhs) & (res ^ sum) & 0x80000000u) != 0u);
        break;
    }
    case EMU_IR_FS_SUB: {
        const uint32_t d = res - rhs;
        z = (d == 0u);
        s = (d & 0x80000000u) != 0u;
        c = (res < rhs);          /* borrow */
        v = (((res ^ rhs) & (res ^ d) & 0x80000000u) != 0u);
        break;
    }
    case EMU_IR_FS_LOGIC:
    case EMU_IR_FS_ZS:
    default:
        z = (res == 0u);
        s = (res & 0x80000000u) != 0u;
        break;
    }

    const bool val[4] = { z, s, v, c };
    uint32_t *const fw = word_at(cpu, t->flags_offset);
    uint32_t w = *fw;

    for (unsigned f = 0; f < 4u; f++) {
        if ((live & (1u << f)) == 0u || t->flag_bit[f] == 0u) {
            continue;
        }
        w &= ~t->flag_bit[f];
        if (val[f]) {
            w |= t->flag_bit[f];
        }
    }
    *fw = w;
}

/* Compare two values, for the operand-comparing conditions. */
static bool eval_value_cond(uint32_t a, uint32_t b, uint8_t cond)
{
    switch ((emu_ir_cond_t)cond) {
    case EMU_IR_C_EQ:  return a == b;
    case EMU_IR_C_NE:  return a != b;
    case EMU_IR_C_LT:  return (int32_t)a <  (int32_t)b;
    case EMU_IR_C_GE:  return (int32_t)a >= (int32_t)b;
    case EMU_IR_C_LTU: return a <  b;
    case EMU_IR_C_GEU: return a >= b;
    case EMU_IR_C_LE:  return (int32_t)a <= (int32_t)b;
    case EMU_IR_C_GT:  return (int32_t)a >  (int32_t)b;
    case EMU_IR_C_LEU: return a <= b;
    case EMU_IR_C_GTU: return a >  b;
    case EMU_IR_C_ALWAYS:
    default:           return true;
    }
}

static bool eval_cond(emu_cpu_t *cpu, const emu_ir_target_t *t, uint8_t cond)
{
    const uint32_t w = *word_at(cpu, t->flags_offset);
    const bool z = (t->flag_bit[0] != 0u) && ((w & t->flag_bit[0]) != 0u);
    const bool s = (t->flag_bit[1] != 0u) && ((w & t->flag_bit[1]) != 0u);
    const bool v = (t->flag_bit[2] != 0u) && ((w & t->flag_bit[2]) != 0u);
    const bool c = (t->flag_bit[3] != 0u) && ((w & t->flag_bit[3]) != 0u);

    switch ((emu_ir_cond_t)cond) {
    case EMU_IR_C_EQ:     return z;
    case EMU_IR_C_NE:     return !z;
    case EMU_IR_C_LT:     return s != v;
    case EMU_IR_C_GE:     return s == v;
    case EMU_IR_C_LTU:    return c;
    case EMU_IR_C_GEU:    return !c;
    case EMU_IR_C_LE:     return z || (s != v);
    case EMU_IR_C_GT:     return !z && (s == v);
    case EMU_IR_C_LEU:    return c || z;
    case EMU_IR_C_GTU:    return !c && !z;
    case EMU_IR_C_ALWAYS:
    default:              return true;
    }
}

/* ------------------------------------------------------------------ */
/* Floating point                                                      */
/* ------------------------------------------------------------------ */

/*
 * Bits to float and back. Through a union rather than a cast, because a
 * pointer cast between the two is a strict-aliasing violation that gcc
 * acts on at -O2: it is entitled to keep the old value in a register and
 * does.
 */
static EMU_ALWAYS_INLINE float b2f(uint32_t v)
{
    union { uint32_t u; float f; } c;
    c.u = v;
    return c.f;
}

static EMU_ALWAYS_INLINE uint32_t f2b(float v)
{
    union { uint32_t u; float f; } c;
    c.f = v;
    return c.u;
}

/*
 * NaN without <math.h>: the core may not call libm, which is the same
 * rule that made the RISC-V frontend's fsqrt a Newton-Raphson rather
 * than a call to sqrt(). A NaN is the only value not equal to itself.
 */
static EMU_ALWAYS_INLINE bool ir_isnan(float v) { return v != v; }

/* Round to an integral float under an EMU_IR_FRM_* mode. */
static float ir_round(float v, uint8_t aux)
{
    const float t = (float)(int64_t)v;      /* toward zero */

    switch (EMU_IR_FRM(aux)) {
    case EMU_IR_FRM_RTZ:
        return t;
    case EMU_IR_FRM_RDN:
        return (v < 0.0f && t != v) ? t - 1.0f : t;
    case EMU_IR_FRM_RUP:
        return (v > 0.0f && t != v) ? t + 1.0f : t;
    case EMU_IR_FRM_RMM: {
        const float d = v - t;
        if (d >= 0.5f)  { return t + 1.0f; }
        if (d <= -0.5f) { return t - 1.0f; }
        return t;
    }
    case EMU_IR_FRM_RNE:
    default: {
        const float d = v - t;
        if (d > 0.5f)  { return t + 1.0f; }
        if (d < -0.5f) { return t - 1.0f; }
        if (d == 0.5f  || d == -0.5f) {
            /* Ties to even: keep t if it already is. */
            const float h = t * 0.5f;
            if (h != (float)(int64_t)h) {
                return (d > 0.0f) ? t + 1.0f : t - 1.0f;
            }
        }
        return t;
    }
    }
}

/* The ten-bit classification, which both guests spell identically. */
static uint32_t ir_fclass(uint32_t bits)
{
    const uint32_t exp  = (bits >> 23) & 0xFFu;
    const uint32_t frac = bits & 0x7FFFFFu;
    const bool neg = (bits & 0x80000000u) != 0u;

    if (exp == 0xFFu) {
        if (frac == 0u)                 { return neg ? (1u << 0) : (1u << 7); }
        return (frac & 0x400000u) ? (1u << 9) : (1u << 8);
    }
    if (exp == 0u) {
        if (frac == 0u)                 { return neg ? (1u << 3) : (1u << 4); }
        return neg ? (1u << 2) : (1u << 5);
    }
    return neg ? (1u << 1) : (1u << 6);
}

bool emu_ir_interp(const emu_ir_block_t *b, emu_cpu_t *cpu,
                   const emu_ir_target_t *t)
{
    static uint32_t tmp[EMU_IR_MAX_TEMPS];

    if (b->overflow) {
        return false;
    }

    for (uint32_t i = 0; i < b->count; i++) {
        const emu_ir_insn_t *const in = &b->insn[i];
        if (in->dead) {
            continue;
        }

        const uint32_t a = (in->a != EMU_IR_NO_TEMP) ? tmp[in->a] : 0u;
        const uint32_t bv = (in->b != EMU_IR_NO_TEMP) ? tmp[in->b] : 0u;
        uint32_t r = 0u;

        switch ((emu_ir_op_t)in->op) {
        case EMU_IR_NOP:
        case EMU_IR_RETIRE:  continue;

        case EMU_IR_GET:
            r = (t->reg_is_zero != NULL && t->reg_is_zero(in->imm))
                    ? 0u : *reg_ptr(cpu, t, in->imm);
            break;

        case EMU_IR_PUT:
            if (t->reg_is_zero == NULL || !t->reg_is_zero(in->imm)) {
                *reg_ptr(cpu, t, in->imm) = a;
            }
            continue;

        case EMU_IR_CONST:   r = in->imm; break;
        case EMU_IR_MOV:     r = a; break;

        case EMU_IR_ADD:     r = a + bv; break;
        case EMU_IR_SUB:     r = a - bv; break;
        case EMU_IR_AND:     r = a & bv; break;
        case EMU_IR_OR:      r = a | bv; break;
        case EMU_IR_XOR:     r = a ^ bv; break;

        /* The architectures agree that only the low five bits count. */
        case EMU_IR_SHL:     r = a << (bv & 31u); break;
        case EMU_IR_SHR:     r = a >> (bv & 31u); break;
        case EMU_IR_SAR:     r = (uint32_t)((int32_t)a >> (bv & 31u)); break;
        case EMU_IR_SHLI:    r = a << (in->imm & 31u); break;
        case EMU_IR_SHRI:    r = a >> (in->imm & 31u); break;
        case EMU_IR_SARI:
            r = (uint32_t)((int32_t)a >> (in->imm & 31u));
            break;

        case EMU_IR_NEG:     r = (uint32_t)(-(int32_t)a); break;
        case EMU_IR_NOT:     r = ~a; break;

        case EMU_IR_BSWAP32: r = __builtin_bswap32(a); break;
        case EMU_IR_BSWAP16:
            r = ((a & 0x00FF00FFu) << 8) | ((a & 0xFF00FF00u) >> 8);
            break;
        case EMU_IR_HSWAP:   r = (a << 16) | (a >> 16); break;

        /*
         * Defined for a zero input, which is the whole reason the IR
         * specifies it: the natural host instruction on x86 is not, and
         * a lowering built on bsr silently returns something else.
         */
        case EMU_IR_CLZ:
            r = (a == 0u) ? 32u : (uint32_t)__builtin_clz(a);
            break;
        case EMU_IR_CTZ:
            r = (a == 0u) ? 32u : (uint32_t)__builtin_ctz(a);
            break;
        case EMU_IR_POPCNT:
            r = (uint32_t)__builtin_popcount(a);
            break;

        case EMU_IR_BEXT:    r = (a >> (bv & 31u)) & 1u; break;
        case EMU_IR_BSET:    r = a | (1u << (bv & 31u)); break;
        case EMU_IR_BCLR:    r = a & ~(1u << (bv & 31u)); break;
        case EMU_IR_BINV:    r = a ^ (1u << (bv & 31u)); break;

        /* ---- floating point ---------------------------------- */
        /*
         * Computed in `float`, deliberately. The host's own single
         * precision is the reference the compiled code will use, so
         * doing it in double here and rounding once at the end would
         * make the differential harness compare two different
         * computations and call the difference a lowering bug.
         */
        case EMU_IR_FGET:
            if (t->freg_offset == NULL) { return false; }
            r = *(const uint32_t *)((const uint8_t *)cpu +
                                    t->freg_offset(in->imm));
            break;

        case EMU_IR_FPUT:
            if (t->freg_offset == NULL) { return false; }
            *(uint32_t *)((uint8_t *)cpu + t->freg_offset(in->imm)) = a;
            continue;

        case EMU_IR_FADD:  r = f2b(b2f(a) + b2f(bv)); break;
        case EMU_IR_FSUB:  r = f2b(b2f(a) - b2f(bv)); break;
        case EMU_IR_FMUL:  r = f2b(b2f(a) * b2f(bv)); break;
        case EMU_IR_FDIV:  r = f2b(b2f(a) / b2f(bv)); break;
        /*
         * Square root is declined here, not approximated.
         *
         * This file is the reference the differential harness compares
         * compiled code against, and it may not call libm -- the same
         * rule that made the RISC-V frontend's fsqrt a Newton-Raphson.
         * An approximation would disagree with a host `sqrtss`, which is
         * correctly rounded, in the last bit for some inputs, and the
         * harness would report a lowering bug that was really an error
         * in the thing doing the checking. A backend may still lower it
         * natively; blocks containing one simply go unchecked, exactly
         * as blocks containing a store already do.
         */
        case EMU_IR_FSQRT: return false;

        /*
         * A NaN operand gives the *other* operand, which is what both
         * guests define and what no host instruction of this name does.
         * Two NaNs give the canonical one.
         */
        case EMU_IR_FMIN:
        case EMU_IR_FMAX: {
            const float x = b2f(a), y = b2f(bv);
            const bool xn = ir_isnan(x), yn = ir_isnan(y);

            if (xn && yn)      { r = 0x7FC00000u; }
            else if (xn)       { r = bv; }
            else if (yn)       { r = a; }
            else if (x == y)   { r = (in->op == (uint8_t)EMU_IR_FMIN)
                                       ? (a | bv) : (a & bv); }
            else if (in->op == (uint8_t)EMU_IR_FMIN) { r = (x < y) ? a : bv; }
            else                                     { r = (x > y) ? a : bv; }
            break;
        }

        case EMU_IR_FSGNJ: {
            const uint32_t sign = (in->aux == EMU_IR_FSGNJ_N)
                                    ? (~bv & 0x80000000u)
                                : (in->aux == EMU_IR_FSGNJ_X)
                                    ? ((a ^ bv) & 0x80000000u)
                                    : (bv & 0x80000000u);
            r = (a & 0x7FFFFFFFu) | sign;
            break;
        }

        /* Unordered is false for all three, which is why EQ is not SETCC. */
        case EMU_IR_FCMP: {
            const float x = b2f(a), y = b2f(bv);

            r = (ir_isnan(x) || ir_isnan(y)) ? 0u
              : (in->aux == (uint8_t)EMU_IR_C_EQ) ? (x == y)
              : (in->aux == (uint8_t)EMU_IR_C_LT) ? (x <  y)
                                                  : (x <= y);
            break;
        }

        case EMU_IR_FCVT_TO_I: {
            /*
             * Saturation and the NaN result are the guest's rule, not
             * the host's -- C leaves an out-of-range conversion
             * undefined, so the bounds are tested before converting
             * rather than after.
             */
            const float x = b2f(a);
            const bool uns = (in->aux & EMU_IR_F_UNSIGNED) != 0u;

            if (ir_isnan(x)) {
                r = uns ? 0xFFFFFFFFu : 0x7FFFFFFFu;
            } else if (uns) {
                r = (x <= 0.0f) ? 0u
                  : (x >= 4294967296.0f) ? 0xFFFFFFFFu
                                         : (uint32_t)ir_round(x, in->aux);
            } else {
                r = (x <= -2147483648.0f) ? 0x80000000u
                  : (x >=  2147483648.0f) ? 0x7FFFFFFFu
                                          : (uint32_t)(int32_t)
                                                ir_round(x, in->aux);
            }
            break;
        }

        case EMU_IR_FCVT_FROM_I:
            r = ((in->aux & EMU_IR_F_UNSIGNED) != 0u)
                    ? f2b((float)a)
                    : f2b((float)(int32_t)a);
            break;

        case EMU_IR_FCLASS: r = ir_fclass(a); break;

        case EMU_IR_SEXT8:   r = (uint32_t)(int32_t)(int8_t)a; break;
        case EMU_IR_SEXT16:  r = (uint32_t)(int32_t)(int16_t)a; break;
        case EMU_IR_ZEXT8:   r = a & 0xFFu; break;
        case EMU_IR_ZEXT16:  r = a & 0xFFFFu; break;

        case EMU_IR_SETF:
            apply_flags(cpu, t, in, tmp);
            continue;

        case EMU_IR_SETCC:
            r = eval_value_cond(a, bv, in->aux) ? 1u : 0u;
            break;

        case EMU_IR_GETCOND:
            r = eval_cond(cpu, t, in->aux) ? 1u : 0u;
            break;

        case EMU_IR_SELECT:
            r = eval_cond(cpu, t, in->aux) ? a : bv;
            break;

        case EMU_IR_SETPC:
            *word_at(cpu, t->pc_offset) =
                (in->a != EMU_IR_NO_TEMP) ? a : in->imm;
            continue;

        case EMU_IR_EXIT:
            *word_at(cpu, t->pc_offset) =
                (in->a != EMU_IR_NO_TEMP) ? a : in->imm;
            return true;

        case EMU_IR_EXIT_IF:
            /*
             * Compares its two operands directly; it does not consult
             * the guest's flags.
             *
             * This is the shape a flagless guest needs -- a RISC-V
             * branch has no flags to read -- and it is what every
             * lowering emits: a compare of a against b, then a
             * conditional exit to `imm`. An earlier version of this
             * evaluated the *flag* condition instead and took the pc
             * from `a`, which for RV32 read four flag bits that are all
             * defined as absent and then jumped to whatever operand
             * happened to be in `a`.
             *
             * Nothing caught it, because nothing ran the two against
             * each other until the differential harness did.
             */
            if (eval_value_cond(a, bv, in->aux)) {
                *word_at(cpu, t->pc_offset) = in->imm;
                return true;
            }
            continue;

        case EMU_IR_LOAD:
            if (t->load == NULL) {
                return false;
            }
            if (t->load(cpu, a + in->imm, in->aux, &r) != 0u) {
                return true;          /* trapped; pc is in the handler */
            }
            break;

        case EMU_IR_STORE:
            if (t->store == NULL) {
                return false;
            }
            if (t->store(cpu, a + in->imm, in->aux, bv) != 0u) {
                return true;
            }
            continue;

        /*
         * One access as far as the guest is concerned: read the byte,
         * report the bit as it was in Z, write back unless this is the
         * test-only form. Z alone moves.
         */
        case EMU_IR_BITOP_SET:
        case EMU_IR_BITOP_CLR:
        case EMU_IR_BITOP_INV:
        case EMU_IR_BITOP_TST: {
            if (t->load == NULL || t->store == NULL) {
                return false;
            }
            const uint32_t adr = a + in->imm;
            const uint32_t mask = 1u << (bv & 7u);
            const uint32_t spec = EMU_IR_MEM_AUX(1u, 0u);
            uint32_t token = 0u;

            if (t->load(cpu, adr, spec, &token) != 0u) {
                return true;
            }

            uint32_t *const fw = word_at(cpu, t->flags_offset);
            if ((token & mask) != 0u) {
                *fw &= ~t->flag_bit[0];
            } else {
                *fw |= t->flag_bit[0];
            }

            if (in->op == (uint8_t)EMU_IR_BITOP_TST) {
                continue;
            }
            uint32_t out = token;
            if (in->op == (uint8_t)EMU_IR_BITOP_SET)      { out |= mask; }
            else if (in->op == (uint8_t)EMU_IR_BITOP_CLR) { out &= ~mask; }
            else                                          { out ^= mask; }
            if (t->store(cpu, adr, spec, out) != 0u) {
                return true;
            }
            continue;
        }

        /*
         * A helper still needs the frontend's own signature, which
         * emu_ir_target_t exposes only as opaque pointers. Refusing is
         * honest; guessing a signature is not.
         */
        case EMU_IR_HELPER:
        case EMU_IR_HELPER_TRAP:
        default:
            return false;
        }

        if (in->dst != EMU_IR_NO_TEMP && in->dst < EMU_IR_MAX_TEMPS) {
            tmp[in->dst] = r;
        }
    }
    return true;
}

