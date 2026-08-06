/* SPDX-License-Identifier: Apache-2.0 */
/*
 * g4mh_disasm.c - RH850 disassembly.
 *
 * Covers exactly what g4mh_interp.c executes. An encoding the interpreter
 * would refuse prints as ".short 0x...." rather than as a guess: a
 * disassembly that invents a mnemonic for something the core will not run
 * is worse than one that admits it does not know, because it sends the
 * reader looking for a bug in the wrong instruction.
 */

#include "g4mh/g4mh_disasm.h"
#include "g4mh/g4mh_decode.h"

#if G4MH_ENABLE_DISASM

#include <stdio.h>

size_t g4mh_disasm(char *buf, size_t buflen, uint32_t pc, uint32_t insn)
{
    const uint32_t w0 = insn & 0xFFFFu;
    const uint32_t w1 = insn >> 16;
    const uint32_t r1 = g4mh_reg1(w0);
    const uint32_t r2 = g4mh_reg2(w0);
    const uint32_t op = g4mh_op6(w0);
    const char *a = g4mh_reg_name(r1);
    const char *b = g4mh_reg_name(r2);
    int n = 0;

    if (buflen == 0u) {
        return 0u;
    }

    switch (op) {
    case 0x00:
        n = (r1 == 0u && r2 == 0u) ? snprintf(buf, buflen, "nop")
                                   : snprintf(buf, buflen, "mov %s, %s", a, b);
        break;
    case 0x01: n = snprintf(buf, buflen, "not %s, %s", a, b); break;
    case 0x02:
        n = (r2 == 0u) ? snprintf(buf, buflen, "switch %s", a)
                       : snprintf(buf, buflen, "divh %s, %s", a, b);
        break;
    case 0x03: n = snprintf(buf, buflen, "jmp [%s]", a); break;
    case 0x04:
        n = (r2 == 0u) ? snprintf(buf, buflen, "zxb %s", a)
                       : snprintf(buf, buflen, "satsubr %s, %s", a, b);
        break;
    case 0x05:
        n = (r2 == 0u) ? snprintf(buf, buflen, "sxb %s", a)
                       : snprintf(buf, buflen, "satsub %s, %s", a, b);
        break;
    case 0x06:
        n = (r2 == 0u) ? snprintf(buf, buflen, "zxh %s", a)
                       : snprintf(buf, buflen, "satadd %s, %s", a, b);
        break;
    case 0x07:
        n = (r2 == 0u) ? snprintf(buf, buflen, "sxh %s", a)
                       : snprintf(buf, buflen, "mulh %s, %s", a, b);
        break;
    case 0x08: n = snprintf(buf, buflen, "or %s, %s", a, b);   break;
    case 0x09: n = snprintf(buf, buflen, "xor %s, %s", a, b);  break;
    case 0x0A: n = snprintf(buf, buflen, "and %s, %s", a, b);  break;
    case 0x0B: n = snprintf(buf, buflen, "tst %s, %s", a, b);  break;
    case 0x0C: n = snprintf(buf, buflen, "subr %s, %s", a, b); break;
    case 0x0D: n = snprintf(buf, buflen, "sub %s, %s", a, b);  break;
    case 0x0E: n = snprintf(buf, buflen, "add %s, %s", a, b);  break;
    case 0x0F: n = snprintf(buf, buflen, "cmp %s, %s", a, b);  break;

    case 0x10: n = snprintf(buf, buflen, "mov %d, %s", g4mh_imm5(w0), b); break;
    case 0x11: n = snprintf(buf, buflen, "satadd %d, %s", g4mh_imm5(w0), b); break;
    case 0x12: n = snprintf(buf, buflen, "add %d, %s", g4mh_imm5(w0), b); break;
    case 0x13: n = snprintf(buf, buflen, "cmp %d, %s", g4mh_imm5(w0), b); break;
    case 0x14: n = snprintf(buf, buflen, "shr %u, %s", (unsigned)(w0 & 0x1Fu), b); break;
    case 0x15: n = snprintf(buf, buflen, "sar %u, %s", (unsigned)(w0 & 0x1Fu), b); break;
    case 0x16: n = snprintf(buf, buflen, "shl %u, %s", (unsigned)(w0 & 0x1Fu), b); break;
    case 0x17: n = snprintf(buf, buflen, "mulh %d, %s", g4mh_imm5(w0), b); break;

    case 0x30: n = snprintf(buf, buflen, "addi %d, %s, %s",
                            (int)emu_sext(w1, 16), a, b); break;
    case 0x31:
        n = (r2 == 0u)
          ? snprintf(buf, buflen, "mov 0x....%04x, %s", (unsigned)w1, a)
          : snprintf(buf, buflen, "movea %d, %s, %s",
                     (int)emu_sext(w1, 16), a, b);
        break;
    case 0x32: n = snprintf(buf, buflen, "movhi 0x%04x, %s, %s",
                            (unsigned)w1, a, b); break;
    case 0x33: n = snprintf(buf, buflen, "satsubi %d, %s, %s",
                            (int)emu_sext(w1, 16), a, b); break;
    case 0x34: n = snprintf(buf, buflen, "ori 0x%04x, %s, %s",
                            (unsigned)w1, a, b); break;
    case 0x35: n = snprintf(buf, buflen, "xori 0x%04x, %s, %s",
                            (unsigned)w1, a, b); break;
    case 0x36: n = snprintf(buf, buflen, "andi 0x%04x, %s, %s",
                            (unsigned)w1, a, b); break;
    case 0x37: n = snprintf(buf, buflen, "mulhi %d, %s, %s",
                            (int)emu_sext(w1, 16), a, b); break;

    case 0x38: n = snprintf(buf, buflen, "ld.b %d[%s], %s",
                            (int)emu_sext(w1, 16), a, b); break;
    case 0x39: n = snprintf(buf, buflen, "ld.%c %d[%s], %s",
                            (w1 & 1u) ? 'w' : 'h',
                            (int)emu_sext(w1 & 0xFFFEu, 16), a, b); break;
    case 0x3A: n = snprintf(buf, buflen, "st.b %s, %d[%s]", b,
                            (int)emu_sext(w1, 16), a); break;
    case 0x3B: n = snprintf(buf, buflen, "st.%c %s, %d[%s]",
                            (w1 & 1u) ? 'w' : 'h', b,
                            (int)emu_sext(w1 & 0xFFFEu, 16), a); break;

    case 0x3C:
    case 0x3D: {
        const uint32_t d = (w0 & 0x3Eu) | (w1 << 6);
        const uint32_t tgt = pc + (uint32_t)emu_sext(d, 22);
        n = (r2 == 0u) ? snprintf(buf, buflen, "jr 0x%08x", tgt)
                       : snprintf(buf, buflen, "jarl 0x%08x, %s", tgt, b);
        break;
    }

    case 0x3F: {
        const uint32_t sub = w1 & 0x7FFu;
        const uint32_t sel = (w1 >> 11) & 0x1Fu;
        const char *srn = g4mh_sr_name(sel, r1);

        switch (sub) {
        case 0x000: n = snprintf(buf, buflen, "setf %s, %s",
                                 g4mh_cond_name(r1), b); break;
        case 0x020:
            n = (srn != NULL)
              ? snprintf(buf, buflen, "ldsr %s, %s", b, srn)
              : snprintf(buf, buflen, "ldsr %s, %u, %u", b,
                         (unsigned)r1, (unsigned)sel);
            break;
        case 0x040:
            n = (srn != NULL)
              ? snprintf(buf, buflen, "stsr %s, %s", srn, b)
              : snprintf(buf, buflen, "stsr %u, %s, %u",
                         (unsigned)r1, b, (unsigned)sel);
            break;
        case 0x080: n = snprintf(buf, buflen, "shr %s, %s", a, b); break;
        case 0x0A0: n = snprintf(buf, buflen, "sar %s, %s", a, b); break;
        case 0x0C0: n = snprintf(buf, buflen, "shl %s, %s", a, b); break;
        case 0x100: n = snprintf(buf, buflen, "trap %u", (unsigned)r1); break;
        case 0x120: n = snprintf(buf, buflen, "halt"); break;
        case 0x140: n = snprintf(buf, buflen, "reti"); break;
        case 0x160: n = snprintf(buf, buflen,
                                 (w0 & 0x8000u) ? "ei" : "di"); break;
        default:
            switch (sub & 0x7FDu) {
            case 0x220:
                n = snprintf(buf, buflen, "mul%s %s, %s, %s",
                             (sub & 2u) ? "u" : "", a, b,
                             g4mh_reg_name(sel));
                break;
            case 0x2C0:
                n = snprintf(buf, buflen, "div%s %s, %s, %s",
                             (sub & 2u) ? "u" : "", a, b,
                             g4mh_reg_name(sel));
                break;
            default:
                n = snprintf(buf, buflen, ".short 0x%04x, 0x%04x",
                             (unsigned)w0, (unsigned)w1);
                break;
            }
            break;
        }
        break;
    }

    default: {
        const uint32_t op4 = g4mh_op4(w0);
        if (op4 == 0x0Bu) {
            const uint32_t d = (((w0 >> 11) & 0x1Fu) << 4) |
                               (((w0 >> 4) & 0x7u) << 1);
            n = snprintf(buf, buflen, "b%s 0x%08x",
                         g4mh_cond_name(w0 & 0xFu),
                         pc + (uint32_t)emu_sext(d, 9));
        } else {
            static const char *const sl[6] = {
                "sld.b", "sst.b", "sld.h", "sst.h", "sld.w", "sst.w",
            };
            unsigned idx;
            unsigned disp;
            switch (op4) {
            case 0x06: idx = 0u; disp = w0 & 0x7Fu; break;
            case 0x07: idx = 1u; disp = w0 & 0x7Fu; break;
            case 0x08: idx = 2u; disp = (w0 & 0x7Fu) << 1; break;
            case 0x09: idx = 3u; disp = (w0 & 0x7Fu) << 1; break;
            case 0x0A: idx = (w0 & 1u) ? 5u : 4u;
                       disp = (w0 & 0x7Eu) << 1; break;
            default:
                n = snprintf(buf, buflen, ".short 0x%04x", (unsigned)w0);
                goto done;
            }
            n = (idx & 1u)
              ? snprintf(buf, buflen, "%s %s, %u[ep]", sl[idx], b, disp)
              : snprintf(buf, buflen, "%s %u[ep], %s", sl[idx], disp, b);
        }
        break;
    }
    }

done:
    if (n < 0) {
        buf[0] = '\0';
        return 0u;
    }
    /* snprintf reports what it *would* have written; clamp to what fits. */
    return ((size_t)n < buflen) ? (size_t)n : (buflen - 1u);
}

#endif /* G4MH_ENABLE_DISASM */
