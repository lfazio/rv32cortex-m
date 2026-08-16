/* SPDX-License-Identifier: Apache-2.0 */
/*
 * g4mh_decode.c - Instruction length, condition codes and register names.
 */

#include "g4mh/g4mh_decode.h"

unsigned g4mh_insn_len(uint16_t w)
{
    /*
     * op6 below 0x30 is 16 bits -- **except one slot**, and the
     * exception is not decoration.
     *
     * `JARL disp32, reg1` is 00000 010111 RRRRR: reg2 zero, op6 0x17,
     * reg1 the link register, and reg1 zero makes it `JR disp32`. It
     * shares MULH imm5's opcode and is 48 bits wide. Answering 2 here
     * means the second stage never runs, so g4mh_insn_is_48 -- which
     * has handled 0x17 since it was written -- was never asked, w1 and
     * w2 read as zero, and the interpreter's perfectly good
     * implementation jumped to pc + 0. An infinite loop, not a wrong
     * answer.
     *
     * This function sees only the first halfword, which is enough:
     * reg2 is in it.
     */
    const uint32_t op = g4mh_op6(w);

    if (op == 0x17u && g4mh_reg2(w) == 0u) {
        return 4u;                       /* JR / JARL disp32: really 6 */
    }
    return (op < 0x30u) ? 2u : 4u;
}

bool g4mh_insn_is_48(uint16_t w0, uint16_t w1)
{
    /*
     * Every 48-bit encoding has reg2 == 0. Checked first because it
     * rejects the whole question in one compare for ordinary code.
     */
    if (g4mh_reg2(w0) != 0u) {
        return false;
    }

    switch (g4mh_op6(w0)) {
    case 0x17u:   /* JR / JARL disp32   -- shares the MULH imm5 slot   */
    case 0x31u:   /* MOV imm32, reg1    -- shares the MOVEA slot       */
        return true;

    case 0x37u:
        /*
         * JMP disp32[reg1] shares the MULHI slot, and so does LOOP --
         * which is 32-bit where JMP is 48. Bit 0 of the second halfword
         * separates them, exactly as it does at 0x3C: LOOP's
         * displacement is even, so the bit is free to mark it.
         */
        return (w1 & 1u) == 0u;

    case 0x3Cu:
    case 0x3Du:
        /*
         * This slot holds three different things with reg2 == 0: the
         * 32-bit JR/JARL disp22, the 48-bit disp23 loads and stores, and
         * PREPARE -- which is 32, 48 *or 64* bits depending on what it
         * loads into ep.
         *
         * Bit 0 of the second halfword separates JR from the rest, and
         * is the only discriminator available for that. It is not enough
         * on its own: PREPARE's short form sets it too and is four bytes
         * long, so answering "48-bit" for it would advance the pc by two
         * bytes too many and desynchronise everything after.
         */
        if ((w1 & 1u) == 0u) {
            return false;                    /* JR / JARL disp22    */
        }
        if ((w1 & 0x1Fu) == 0x01u) {
            return false;                    /* PREPARE list12,imm5 */
        }
        if ((w1 & 0x07u) == 0x03u) {
            /*
             * PREPARE list12, imm5, sp/imm. ff names what reaches ep:
             * sp costs no extra halfword, imm16 costs one, and imm32
             * costs two -- the ISA's only 64-bit encoding, and the one
             * case where a *third* halfword is not the last. This
             * function answers "is there a third", which is true for
             * both; g4mh_insn_is_64 answers whether there is a fourth.
             */
            return ((w1 >> 3) & 3u) != 0u;
        }
        return true;                         /* the disp23 group    */

    default:
        return false;
    }
}

bool g4mh_insn_is_64(uint16_t w0, uint16_t w1)
{
    /*
     * One encoding in the whole ISA: PREPARE list12, imm5, imm32, where
     * ff = 11 puts a 32-bit immediate in the two halfwords after w1.
     * Everything else that reaches six bytes stops there.
     *
     * Confirmed against CC-RH, which had never been asked because the
     * checking script capped its own listing regex at twelve hex digits
     * and so dropped every 64-bit line:
     *
     *   prepare 0x3, 4, 0x12345678  ->  82 07 7B 00 78 56 34 12
     */
    return g4mh_reg2(w0) == 0u &&
           (g4mh_op6(w0) == 0x3Cu || g4mh_op6(w0) == 0x3Du) &&
           (w1 & 0x07u) == 0x03u &&
           ((w1 >> 3) & 3u) == 3u;
}

bool g4mh_cond(uint32_t cond, uint32_t psw)
{
    const bool z  = (psw & G4MH_PSW_Z)  != 0u;
    const bool s  = (psw & G4MH_PSW_S)  != 0u;
    const bool ov = (psw & G4MH_PSW_OV) != 0u;
    const bool cy = (psw & G4MH_PSW_CY) != 0u;
    const bool sat = (psw & G4MH_PSW_SAT) != 0u;

    switch (cond & 0xFu) {
    case 0x0: return ov;                    /* BV  overflow              */
    case 0x1: return cy;                    /* BL  lower (unsigned <)    */
    case 0x2: return z;                     /* BE  equal                 */
    case 0x3: return cy || z;               /* BNH not higher            */
    case 0x4: return s;                     /* BN  negative              */
    case 0x5: return true;                  /* BR  always                */
    case 0x6: return s != ov;               /* BLT less than (signed)    */
    case 0x7: return (s != ov) || z;        /* BLE less or equal         */
    case 0x8: return !ov;                   /* BNV                       */
    case 0x9: return !cy;                   /* BNL / BNC                 */
    case 0xA: return !z;                    /* BNE                       */
    case 0xB: return !(cy || z);            /* BH  higher                */
    case 0xC: return !s;                    /* BP  positive              */
    case 0xD: return sat;                   /* BSA saturated             */
    case 0xE: return s == ov;               /* BGE greater or equal      */
    default:  return (s == ov) && !z;       /* BGT greater than          */
    }
}

const char *g4mh_cond_name(uint32_t cond)
{
    static const char *const n[16] = {
        "v",  "l",  "e",  "nh", "n",  "r",  "lt", "le",
        "nv", "nl", "ne", "h",  "p",  "sa", "ge", "gt",
    };
    return n[cond & 0xFu];
}

const char *g4mh_reg_name(unsigned r)
{
    /*
     * The architectural names for the registers the hardware or the ABI
     * fixes, and rN for the rest. Naming only what is really fixed keeps a
     * disassembly listing honest: r10 is a scratch register by convention
     * and calling it "a0" would be asserting an ABI the core does not have.
     */
    static const char *const n[32] = {
        "zero", "r1",  "r2",  "sp",  "gp",  "tp",  "r6",  "r7",
        "r8",   "r9",  "r10", "r11", "r12", "r13", "r14", "r15",
        "r16",  "r17", "r18", "r19", "r20", "r21", "r22", "r23",
        "r24",  "r25", "r26", "r27", "r28", "r29", "ep",  "lp",
    };
    return (r < 32u) ? n[r] : "r?";
}

const char *g4mh_sr_name(unsigned bank, unsigned reg)
{
    if (bank == 0u) {
        switch (reg) {
        case G4MH_SR_EIPC:  return "eipc";
        case G4MH_SR_EIPSW: return "eipsw";
        case G4MH_SR_FEPC:  return "fepc";
        case G4MH_SR_FEPSW: return "fepsw";
        case G4MH_SR_PSW:   return "psw";
        case G4MH_SR_EIIC:  return "eiic";
        case G4MH_SR_FEIC:  return "feic";
        case G4MH_SR_CTPC:  return "ctpc";
        case G4MH_SR_CTPSW: return "ctpsw";
        case G4MH_SR_CTBP:  return "ctbp";
        case G4MH_SR_EIWR:  return "eiwr";
        case G4MH_SR_FEWR:  return "fewr";
        case G4MH_SR_BSEL:  return "bsel";
        default: break;
        }
    } else if (bank == 1u) {
        switch (reg) {
        case G4MH_SR_MCFG0: return "mcfg0";
        case G4MH_SR_RBASE: return "rbase";
        case G4MH_SR_EBASE: return "ebase";
        case G4MH_SR_INTBP: return "intbp";
        case G4MH_SR_MCTL:  return "mctl";
        case G4MH_SR_PID:   return "pid";
        case G4MH_SR_SCCFG: return "sccfg";
        case G4MH_SR_SCBP:  return "scbp";
        default: break;
        }
    } else if (bank == 2u) {
        switch (reg) {
        case G4MH_SR_HTCFG0: return "htcfg0";
        case G4MH_SR_MEA:    return "mea";
        case G4MH_SR_ASID:   return "asid";
        case G4MH_SR_MEI:    return "mei";
        case G4MH_SR_ISPR:   return "ispr";
        case G4MH_SR_PMR:    return "pmr";
        case G4MH_SR_ICSR:   return "icsr";
        case G4MH_SR_INTCFG: return "intcfg";
        default: break;
        }
    }
    return NULL;
}
