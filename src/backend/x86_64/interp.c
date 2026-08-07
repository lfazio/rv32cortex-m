/* SPDX-License-Identifier: Apache-2.0 */
/*
 * src/backend/x86_64/interp.c - Executing IR directly on this host.
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
 * It lives under the host directory because that is where a backend's
 * two execution strategies belong, and because a host may eventually
 * want to specialise it. Nothing in it is x86-specific *yet* -- and that
 * is worth saying plainly rather than leaving to be discovered, because
 * the day a second host copies it, the copy is the moment to ask whether
 * it should have been shared instead.
 */

#include "emu/emu_ir.h"

#include <string.h>

#if defined(EMU_JIT_X86_64)

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

        case EMU_IR_SEXT8:   r = (uint32_t)(int32_t)(int8_t)a; break;
        case EMU_IR_SEXT16:  r = (uint32_t)(int32_t)(int16_t)a; break;
        case EMU_IR_ZEXT8:   r = a & 0xFFu; break;
        case EMU_IR_ZEXT16:  r = a & 0xFFFFu; break;

        case EMU_IR_SETF:
            apply_flags(cpu, t, in, tmp);
            continue;

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
            if (eval_cond(cpu, t, in->aux)) {
                *word_at(cpu, t->pc_offset) =
                    (in->a != EMU_IR_NO_TEMP) ? a : in->imm;
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

#endif /* EMU_JIT_X86_64 */
