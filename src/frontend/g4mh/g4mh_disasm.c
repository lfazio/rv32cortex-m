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

size_t g4mh_disasm(char *buf, size_t buflen, uint32_t pc, uint64_t insn,
                   unsigned len)
{
    const uint32_t w0 = (uint32_t)(insn & 0xFFFFu);
    const uint32_t w1 = (uint32_t)((insn >> 16) & 0xFFFFu);
    /*
     * The halfwords past the second, and the reason this function grew a
     * `len`. Zero when the instruction is narrower, which is safe to
     * read but is not the same as *knowing* it is narrower -- so the
     * cases that print them test `len` and not the value.
     */
    const uint32_t w2 = (uint32_t)((insn >> 32) & 0xFFFFu);
    const uint32_t w3 = (uint32_t)((insn >> 48) & 0xFFFFu);
    const uint32_t d23 = (w2 << 7) | ((w1 >> 4) & 0x7Fu);
    /*
     * PREPARE's stack adjustment sits at bits 5:1 and is unsigned --
     * *not* Format II's g4mh_imm5, which is bits 4:0 sign-extended.
     * Using that one here printed 2 where the field held 1.
     */
    const unsigned prep_imm5 = (unsigned)((w0 >> 1) & 0x1Fu);

    /*
     * What the *encoding* says the length is, against what the caller
     * says. For RH850 the two must agree: both come from
     * g4mh_insn_len/is_48/is_64, and the caller ran them to fetch the
     * instruction in the first place.
     *
     * So `len` is not information this function lacks -- it is a second
     * opinion, and a disagreement means the caller and the decoder have
     * diverged. That is the defect this frontend keeps recording ("the
     * length decoder and the execute switch have to make the *same*
     * test"), and a wrong length is not a wrong answer but a
     * desynchronised stream. Reporting it as `.short` costs one compare
     * on a path that is already doing formatted output, and turns an
     * invisible divergence into a visible one.
     */
    const unsigned want = (g4mh_insn_len((uint16_t)w0) == 2u) ? 2u
                        : !g4mh_insn_is_48((uint16_t)w0, (uint16_t)w1) ? 4u
                        : g4mh_insn_is_64((uint16_t)w0, (uint16_t)w1) ? 8u
                                                                      : 6u;
    const uint32_t r1 = g4mh_reg1(w0);
    const uint32_t r2 = g4mh_reg2(w0);
    const uint32_t op = g4mh_op6(w0);
    const char *a = g4mh_reg_name(r1);
    const char *b = g4mh_reg_name(r2);
    int n = 0;

    if (buflen == 0u) {
        return 0u;
    }

    if (len != want) {
        n = snprintf(buf, buflen, ".short 0x%04x  ; len %u, want %u",
                     (unsigned)w0, len, want);
        goto done;
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
    case 0x17:
        /* reg2 == 0 is JR/JARL disp32, not a multiply. */
        n = (r2 == 0u)
          ? snprintf(buf, buflen, "jr 0x%08x",
                     pc + (((w2 << 16) | (w1 & 0xFFFEu))))
          : snprintf(buf, buflen, "mulh %d, %s", g4mh_imm5(w0), b);
        break;

    case 0x30: n = snprintf(buf, buflen, "addi %d, %s, %s",
                            (int)emu_sext(w1, 16), a, b); break;
    case 0x31:
        n = (r2 == 0u)
          ? snprintf(buf, buflen, "mov 0x%04x%04x, %s",
                     (unsigned)w2, (unsigned)w1, a)
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
    case 0x37:
        /*
         * reg2 == 0 holds two instructions of *different lengths*: JMP
         * disp32[reg1] at 48 bits and LOOP at 32, told apart by bit 0
         * of the second halfword.
         */
        n = (r2 != 0u)
          ? snprintf(buf, buflen, "mulhi %d, %s, %s",
                     (int)emu_sext(w1, 16), a, b)
          : ((w1 & 1u) != 0u)
          ? snprintf(buf, buflen, "loop %s, 0x%08x", a,
                     pc - (uint32_t)(w1 & 0xFFFEu))
          : snprintf(buf, buflen, "jmp 0x%08x[%s]",
                     (unsigned)((w2 << 16) | (w1 & 0xFFFEu)), a);
        break;

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
    case 0x3D:
        /*
         * The most crowded slot in the ISA, and this printed `jr` for
         * all of it. LD.BU, PREPARE at three widths and the whole
         * disp23 group came out as jumps to an address computed from
         * their operands -- confident nonsense of exactly the kind this
         * file's header promises not to produce, and worse than
         * `.short` because it sends the reader after a control-flow bug
         * in a load.
         *
         * The discrimination is the interpreter's, in the same order.
         */
        if ((w1 & 1u) != 0u) {
            if (r2 != 0u) {
                /* LD.BU disp16, whose disp[0] rides in the opcode. */
                const uint32_t disp = (w1 & 0xFFFEu) | (op & 1u);
                n = snprintf(buf, buflen, "ld.bu %d[%s], %s",
                             (int)emu_sext(disp, 16), a, b);
            } else if ((w1 & 0x1Fu) == 0x01u) {
                n = snprintf(buf, buflen, "prepare 0x%03x, %u",
                             (unsigned)(w1 >> 5), prep_imm5);
            } else if ((w1 & 0x07u) == 0x03u) {
                /*
                 * ff names what reaches ep, and it also names the
                 * *width*: sp costs no halfword, imm16 one, imm32 two.
                 * The 64-bit case is the only one in the ISA and is why
                 * w3 exists.
                 */
                const uint32_t ff = (w1 >> 3) & 3u;
                char ep[16];

                switch (ff) {
                case 0u: snprintf(ep, sizeof(ep), "sp"); break;
                case 1u: snprintf(ep, sizeof(ep), "0x%08x",
                                  (unsigned)(uint32_t)emu_sext(w2, 16));
                         break;
                case 2u: snprintf(ep, sizeof(ep), "0x%04x0000",
                                  (unsigned)w2); break;
                default: snprintf(ep, sizeof(ep), "0x%04x%04x",
                                  (unsigned)w3, (unsigned)w2); break;
                }
                n = snprintf(buf, buflen, "prepare 0x%03x, %u, %s",
                             (unsigned)(w1 >> 5), prep_imm5, ep);
            } else {
                /*
                 * The disp23 group. disp[6:0] is in w1 and disp[22:7]
                 * in w2, so the whole displacement is printable now
                 * that the third halfword arrives -- this used to show
                 * seven bits behind an ellipsis.
                 */
                static const char *const nm[16] = {
                    NULL, NULL, NULL, NULL, NULL, "ld.b",  NULL, "ld.h",
                    NULL, "ld.w", NULL, NULL,  NULL, "st.b", NULL, "st.w"
                };
                static const char *const nmu[16] = {
                    NULL, NULL, NULL, NULL, NULL, "ld.bu", NULL, "ld.hu",
                    NULL, "ld.dw", NULL, NULL, NULL, "st.h", NULL, "st.dw"
                };
                const char *const *tab = (op & 1u) ? nmu : nm;
                const char *mn = tab[w1 & 0xFu];

                const int32_t disp = (int32_t)emu_sext(d23, 23);

                if (mn == NULL) {
                    n = snprintf(buf, buflen, ".short 0x%04x, 0x%04x",
                                 (unsigned)w0, (unsigned)w1);
                } else if ((w1 & 0xFu) == 0xDu || (w1 & 0xFu) == 0xFu) {
                    n = snprintf(buf, buflen, "%s %s, %d[%s]", mn,
                                 g4mh_reg_name((w1 >> 11) & 0x1Fu),
                                 disp, a);
                } else {
                    n = snprintf(buf, buflen, "%s %d[%s], %s", mn,
                                 disp, a,
                                 g4mh_reg_name((w1 >> 11) & 0x1Fu));
                }
            }
        } else {
            /*
             * JR/JARL disp22, and the split is the *high* bits first:
             * disp[21:16] in w0[5:0] and disp[15:1] in w1[15:1]. This
             * had the other order -- low bits first, as RISC-V does it
             * -- which is the same mistake the interpreter records
             * having made and fixed, left uncorrected here. It gives a
             * plausible target for small forward jumps and garbage for
             * everything else.
             */
            const uint32_t d = ((uint32_t)(w0 & 0x3Fu) << 16) |
                               ((uint32_t)w1 & 0xFFFEu);
            const uint32_t tgt = pc + (uint32_t)emu_sext(d, 22);

            n = (r2 == 0u) ? snprintf(buf, buflen, "jr 0x%08x", tgt)
                           : snprintf(buf, buflen, "jarl 0x%08x, %s",
                                      tgt, b);
        }
        break;

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
        /*
         * The link/pointer-update slot. reg2 is an opcode extension here
         * (0-1 link, 2-3 post-increment, 4-5 post-decrement, low bit
         * zero-extends), so these six sub-opcodes cover eighteen
         * mnemonics. Printing them as `.short` is how a loop that never
         * advanced its pointer read as a disassembler gap for a whole
         * debugging session -- the trace showed `.short 0x17e6, 0x037a`
         * where `st.w r0, [r6]+` would have named the bug.
         */
        case 0x370: case 0x372: case 0x374:
        case 0x376: case 0x378: case 0x37A: {
            const uint32_t mode = r2 >> 1;
            const bool store = (sub & 2u) != 0u;
            const char *const sz = (sub < 0x374u) ? "b"
                                 : ((sub < 0x378u) ? "h" : "w");
            const char *const u =
                (sub >= 0x378u || store || (r2 & 1u) == 0u) ? "" : "u";
            const char *const rn = g4mh_reg_name(sel);

            if (mode == 0u) {
                n = store ? snprintf(buf, buflen, "stc.%s %s, [%s]",
                                     sz, rn, a)
                          : snprintf(buf, buflen, "ldl.%s%s [%s], %s",
                                     sz, (sub >= 0x378u) ? "" : "u", a, rn);
            } else if (mode <= 2u) {
                const char step = (mode == 1u) ? '+' : '-';
                n = store ? snprintf(buf, buflen, "st.%s %s, [%s]%c",
                                     sz, rn, a, step)
                          : snprintf(buf, buflen, "ld.%s%s [%s]%c, %s",
                                     sz, u, a, step, rn);
            } else {
                n = snprintf(buf, buflen, ".short 0x%04x, 0x%04x",
                             (unsigned)w0, (unsigned)w1);
            }
            break;
        }

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
