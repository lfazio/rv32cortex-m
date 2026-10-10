/* SPDX-License-Identifier: Apache-2.0 */
/*
 * ppc_disasm.c - e200z7 disassembly.
 *
 * A printer over ppc_decode: which instruction an encoding is, and which
 * field is which, is decided there. What is here is how the assembler
 * writes it. An encoding the decoder does not know prints as `.long` or
 * `.short`, never as a guess.
 */

#include "ppc/ppc_disasm.h"
#include "ppc/ppc_decode.h"

#if PPC_ENABLE_DISASM

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    char *buf;
    size_t cap;
    size_t len;
} out_t;

static void put(out_t *o, const char *fmt, ...)
{
    va_list ap;
    int n;

    if (o->len + 1u >= o->cap) {
        return;
    }
    va_start(ap, fmt);
    n = vsnprintf(o->buf + o->len, o->cap - o->len, fmt, ap);
    va_end(ap);
    if (n > 0) {
        o->len += (size_t)n;
        if (o->len >= o->cap) {
            o->len = o->cap - 1u;
        }
    }
}

/* The record and overflow suffixes, for the Book E names that take them. */
static void mnemonic(out_t *o, const ppc_insn_t *d)
{
    const char *const name = ppc_mn_name(d->id);
    const size_t n = strlen(name);
    const bool vle = name[0] == 'e' ? (name[1] == '_') : (name[0] == 's' && name[1] == 'e' && name[2] == '_');

    put(o, "%s", name);
    if (vle) {
        return;
    }
    if (d->sem == PPC_S_B || d->sem == PPC_S_BC || d->sem == PPC_S_BCLR ||
        d->sem == PPC_S_BCCTR) {
        put(o, "%s%s", d->lk ? "l" : "", d->aa ? "a" : "");
        return;
    }
    if (d->oe) {
        put(o, "o");
    }
    if (d->rc && name[n - 1u] != '.') {
        put(o, ".");
    }
}

size_t ppc_disasm_mode(char *buf, size_t buflen, uint32_t pc, uint32_t insn,
                       unsigned len, bool vle)
{
    ppc_insn_t d;
    out_t out = {buf, buflen, 0u};
    out_t *const o = &out;

    if (buflen == 0u) {
        return 0u;
    }
    buf[0] = '\0';
    ppc_decode(insn, len, vle, &d);

    if (d.id == PPC_M_UNKNOWN || d.id == PPC_M_SPE) {
        if (len == 2u) {
            put(o, ".short 0x%04x", (unsigned)(insn >> 16));
        } else {
            put(o, ".long 0x%08x", (unsigned)insn);
        }
        return out.len;
    }

    /* The compares print as the 32-bit extended mnemonics. */
    switch (d.id) {
    case PPC_M_CMP:
        put(o, "cmpw cr%u,r%u,r%u", d.crf, d.ra, d.rb);
        return out.len;
    case PPC_M_CMPL:
        put(o, "cmplw cr%u,r%u,r%u", d.crf, d.ra, d.rb);
        return out.len;
    case PPC_M_CMPI:
        put(o, "cmpwi cr%u,r%u,%d", d.crf, d.ra, (int)d.ui);
        return out.len;
    case PPC_M_CMPLI:
        put(o, "cmplwi cr%u,r%u,%u", d.crf, d.ra, (unsigned)d.ui);
        return out.len;
    default:
        break;
    }

    mnemonic(o, &d);
    switch (ppc_mn_format(d.id)) {
    case PPC_F_NONE:
        break;
    case PPC_F_D:
        put(o, " r%u", d.rd);
        break;
    case PPC_F_S:
        put(o, " r%u", d.rd);
        break;
    case PPC_F_D_A:
        put(o, " r%u,r%u", d.rd, d.ra);
        break;
    case PPC_F_A_S:
        put(o, " r%u,r%u", d.ra, d.rd);
        break;
    case PPC_F_D_B:
        put(o, " r%u,r%u", d.rd, d.rb);
        break;
    case PPC_F_A_B:
        put(o, " r%u,r%u", d.ra, d.rb);
        break;
    case PPC_F_D_A_B:
        put(o, " r%u,r%u,r%u", d.rd, d.ra, d.rb);
        break;
    case PPC_F_A_S_B:
        put(o, " r%u,r%u,r%u", d.ra, d.rd, d.rb);
        break;
    case PPC_F_D_A_SI:
        put(o, " r%u,r%u,%d", d.rd, d.ra, (int)d.ui);
        break;
    case PPC_F_D_A_UI:
        put(o, " r%u,r%u,%u", d.rd, d.ra, (unsigned)d.ui);
        break;
    case PPC_F_A_S_UI:
        put(o, " r%u,r%u,%u", d.ra, d.rd, (unsigned)d.ui);
        break;
    case PPC_F_A_S_SH:
        put(o, " r%u,r%u,%u", d.ra, d.rd, (unsigned)d.ui);
        break;
    case PPC_F_D_SI:
        put(o, " r%u,%d", d.rd, (int)d.ui);
        break;
    case PPC_F_D_UI:
        put(o, " r%u,%u", d.rd, (unsigned)d.ui);
        break;
    case PPC_F_A_SI:
        put(o, " r%u,%d", d.ra, (int)d.ui);
        break;
    case PPC_F_A_UI:
        put(o, " r%u,%u", d.ra, (unsigned)d.ui);
        break;
    case PPC_F_X_UI:
        put(o, " r%u,%u", d.rd ? d.rd : d.ra, (unsigned)d.ui);
        break;
    case PPC_F_D_MEM:
        put(o, " r%u,%d(r%u)", d.rd, (int)d.imm, d.ra);
        break;
    case PPC_F_MEM:
        put(o, " %d(r%u)", (int)d.imm, d.ra);
        break;
    case PPC_F_CRF_A_B:
        put(o, " cr%u,r%u,r%u", d.crf, d.ra, d.rb);
        break;
    case PPC_F_CRF_A_SI:
        put(o, " cr%u,r%u,%d", d.crf, d.ra, (int)d.ui);
        break;
    case PPC_F_CRF_A_UI:
        put(o, " cr%u,r%u,%u", d.crf, d.ra, (unsigned)d.ui);
        break;
    case PPC_F_CRB3:
        put(o, " %u,%u,%u", d.rd, d.ra, d.rb);
        break;
    case PPC_F_CRF_CRF:
        put(o, " cr%u,cr%u", d.crf, d.crs);
        break;
    case PPC_F_CRF:
        put(o, " cr%u", d.crf);
        break;
    case PPC_F_FXM_S:
        put(o, " %u,r%u", (unsigned)d.imm, d.rd);
        break;
    case PPC_F_D_SPR:
        put(o, " r%u,%u", d.rd, (unsigned)d.imm);
        break;
    case PPC_F_SPR_S:
        put(o, " %u,r%u", (unsigned)d.imm, d.rd);
        break;
    case PPC_F_ISEL:
        put(o, " r%u,r%u,r%u,%u", d.rd, d.ra, d.rb, d.bi);
        break;
    case PPC_F_RLWINM:
        put(o, " r%u,r%u,%u,%u,%u", d.ra, d.rd, d.sh, d.mb, d.me);
        break;
    case PPC_F_RLWNM:
        put(o, " r%u,r%u,r%u,%u,%u", d.ra, d.rd, d.rb, d.mb, d.me);
        break;
    case PPC_F_TARGET:
        put(o, " 0x%08x", (unsigned)((d.aa ? 0u : pc) + d.imm));
        break;
    case PPC_F_BC:
        put(o, " %u,%u,0x%08x", d.bo, d.bi, (unsigned)((d.aa ? 0u : pc) + d.imm));
        break;
    case PPC_F_BO_BI:
        put(o, " %u,%u", d.bo, d.bi);
        break;
    case PPC_F_E_BC:
        put(o, " %u,%u,0x%08x", (unsigned)d.ui, d.bi, (unsigned)(pc + d.imm));
        break;
    case PPC_F_SE_BC:
        put(o, " %u,%u,0x%08x", (d.bo == 12u) ? 1u : 0u, d.bi, (unsigned)(pc + d.imm));
        break;
    case PPC_F_TO_A_B:
        put(o, " %u,r%u,r%u", d.rd, d.ra, d.rb);
        break;
    case PPC_F_TO_A_SI:
        put(o, " %u,r%u,%d", d.rd, d.ra, (int)d.ui);
        break;
    case PPC_F_LEV:
        if (d.imm != 0u) {
            put(o, " %u", (unsigned)d.imm);
        }
        break;
    case PPC_F_E:
    case PPC_F_MO:
        put(o, " %u", (unsigned)d.imm);
        break;
    case PPC_F_CT_A_B:
        put(o, " %u,r%u,r%u", d.rd, d.ra, d.rb);
        break;
    default:
        break;
    }
    return out.len;
}

size_t ppc_disasm(char *buf, size_t buflen, uint32_t pc, uint64_t insn,
                  unsigned len)
{
    /*
     * The trace hook passes the instruction as fetched: first halfword
     * high, as one word. A 16-bit one arrives in the high half too.
     */
    return ppc_disasm_mode(buf, buflen, pc, (uint32_t)insn, len, true);
}

#endif /* PPC_ENABLE_DISASM */
