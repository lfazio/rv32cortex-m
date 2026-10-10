/* SPDX-License-Identifier: Apache-2.0 */
/*
 * armv7m_disasm.c - Thumb-2 and FPv5 disassembly.
 *
 * A printer over armv7m_decode: everything about *which* instruction an
 * encoding is lives there, and what is here is only how the unified
 * syntax spells it. The few places that look at the encoding again do so
 * to choose between two spellings of one operation -- `adds r0, #1`
 * against `adds r0, r0, #1` -- which the decoded form deliberately does
 * not distinguish.
 *
 * An encoding the decoder does not know prints as `.inst`, never as a
 * guess: a disassembly that invents a mnemonic sends the reader looking
 * for a bug in the wrong instruction.
 */

#include "armv7m/armv7m_disasm.h"
#include "armv7m/armv7m_decode.h"

#if ARMV7M_ENABLE_DISASM

#include <stdarg.h>
#include <stdio.h>

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

static const char *const k_reg[16] = {
    "r0", "r1", "r2",  "r3",  "r4",  "r5", "r6", "r7",
    "r8", "r9", "r10", "r11", "r12", "sp", "lr", "pc",
};

static const char *const k_cond[16] = {
    "eq", "ne", "cs", "cc", "mi", "pl", "vs", "vc",
    "hi", "ls", "ge", "lt", "gt", "le", "",   "",
};

static const char *reg(uint32_t r)
{
    return (r < 16u) ? k_reg[r] : "r?";
}

/* The mnemonic: name, S, condition, then a width or type qualifier. */
static void head(out_t *o, const char *name, bool s, const char *cc,
                 const char *sfx)
{
    for (const char *p = name; *p != '\0'; p++) {
        const char ch = (*p >= 'A' && *p <= 'Z') ? (char)(*p - 'A' + 'a') : *p;

        put(o, "%c", ch);
    }
    put(o, "%s%s%s", s ? "s" : "", cc, sfx);
}

static void shift_suffix(out_t *o, uint32_t t, uint32_t n)
{
    static const char *const k[4] = {"lsl", "lsr", "asr", "ror"};

    if (t == ARMV7M_SH_RRX) {
        put(o, ", rrx");
    } else if (!(t == ARMV7M_SH_LSL && n == 0u)) {
        put(o, ", %s #%u", k[t], (unsigned)n);
    }
}

/* A core register list, with runs of three or more as a range. */
static void reglist(out_t *o, uint32_t mask)
{
    bool first = true;

    put(o, "{");
    for (uint32_t r = 0u; r < 16u; r++) {
        uint32_t e = r;

        if ((mask & (1u << r)) == 0u) {
            continue;
        }
        while (e + 1u < 13u && r < 13u && (mask & (1u << (e + 1u))) != 0u) {
            e++;
        }
        put(o, "%s%s", first ? "" : ", ", reg(r));
        if (e >= r + 2u) {
            put(o, "-%s", reg(e));
            r = e;
        }
        first = false;
    }
    put(o, "}");
}

/* [rn, #imm] in its three forms, or the literal and register forms. */
static void address(out_t *o, const armv7m_insn_t *d, uint32_t pc)
{
    const char *const sign = d->add ? "" : "-";

    switch (d->addr) {
    case ARMV7M_ADDR_LITERAL: {
        const uint32_t base = (pc + 4u) & ~3u;

        put(o, "[pc, #%s%u]  ; 0x%08x", sign, (unsigned)d->imm,
            (unsigned)(d->add ? base + d->imm : base - d->imm));
        break;
    }
    case ARMV7M_ADDR_REG:
        put(o, "[%s, %s", reg(d->rn), reg(d->rm));
        if (d->shift_n != 0u) {
            put(o, ", lsl #%u", (unsigned)d->shift_n);
        }
        put(o, "]");
        break;
    default:
        if (!d->index) {
            put(o, "[%s], #%s%u", reg(d->rn), sign, (unsigned)d->imm);
        } else if (d->imm == 0u && d->add && !d->wback) {
            put(o, "[%s]", reg(d->rn));
        } else {
            put(o, "[%s, #%s%u]%s", reg(d->rn), sign, (unsigned)d->imm,
                d->wback ? "!" : "");
        }
        break;
    }
}

/*
 * VFPExpandImm's value in decimal, exactly: it is (16 + n) / 16 * 2^e
 * with e from -3 to 4, so 128 times it is an integer and seven decimal
 * places hold the rest. No floating point -- the core does not link
 * libm, and a disassembler has no business rounding.
 */
static void fp_imm(out_t *o, uint32_t bits)
{
    const uint32_t exp = (bits >> 23) & 0xFFu;
    const uint32_t m = 16u + ((bits >> 19) & 15u);
    const uint32_t v128 = m << (exp - 124u); /* exp is 124..131 */
    uint32_t frac = (v128 & 127u) * 78125u;  /* 1e7 / 128 */
    char digits[8];
    int n = 7;

    for (int i = 6; i >= 0; i--) {
        digits[i] = (char)('0' + (frac % 10u));
        frac /= 10u;
    }
    digits[7] = '\0';
    while (n > 1 && digits[n - 1] == '0') {
        digits[--n] = '\0';
    }
    put(o, "#%s%u.%s", (bits >> 31) ? "-" : "", (unsigned)(v128 >> 7), digits);
}

static const char *sysm_name(uint32_t sysm)
{
    switch (sysm) {
    case 0u:
        return "APSR";
    case 1u:
        return "IAPSR";
    case 2u:
        return "EAPSR";
    case 3u:
        return "XPSR";
    case 5u:
        return "IPSR";
    case 6u:
        return "EPSR";
    case 7u:
        return "IEPSR";
    case 8u:
        return "MSP";
    case 9u:
        return "PSP";
    case 16u:
        return "PRIMASK";
    case 17u:
        return "BASEPRI";
    case 18u:
        return "BASEPRI_MAX";
    case 19u:
        return "FAULTMASK";
    case 20u:
        return "CONTROL";
    default:
        return NULL;
    }
}

/* An FP register list: `words` S registers from `first`, or half as many D. */
static void fp_list(out_t *o, const armv7m_insn_t *d)
{
    const char k = d->fp_dbl ? 'd' : 's';
    const uint32_t first = d->fp_dbl ? d->rd / 2u : d->rd;
    const uint32_t n = d->fp_dbl ? d->imm / 2u : d->imm;

    if (n <= 1u) {
        put(o, "{%c%u}", k, (unsigned)first);
    } else {
        put(o, "{%c%u-%c%u}", k, (unsigned)first, k, (unsigned)(first + n - 1u));
    }
}

static void undefined(out_t *o, uint32_t w0, uint32_t w1, unsigned len)
{
    if (len == 4u) {
        put(o, ".inst.w 0x%04x%04x", (unsigned)w0, (unsigned)w1);
    } else {
        put(o, ".inst.n 0x%04x", (unsigned)w0);
    }
}

size_t armv7m_disasm_it(char *buf, size_t buflen, uint32_t pc, uint64_t insn,
                        unsigned len, uint32_t itstate)
{
    const uint32_t w0 = (uint32_t)(insn & 0xFFFFu);
    const uint32_t w1 = (uint32_t)((insn >> 16) & 0xFFFFu);
    const unsigned want = armv7m_insn_len((uint16_t)w0);
    const bool in_it = (itstate & 0xFu) != 0u;
    const char *const cc = in_it ? k_cond[(itstate >> 4) & 15u] : "";
    armv7m_insn_t d;
    out_t out = {buf, buflen, 0u};
    out_t *const o = &out;
    const char *name;
    const char *w;
    bool s;

    if (buflen == 0u) {
        return 0u;
    }
    buf[0] = '\0';
    if (len != want) {
        put(o, ".inst.n 0x%04x  ; len %u, want %u", (unsigned)w0, len, want);
        return out.len;
    }

    armv7m_decode((uint16_t)w0, (uint16_t)w1, &d);
    name = armv7m_op_name(d.op);
    w = (d.wide && len == 4u) ? ".w" : "";
    s = (d.setflags == ARMV7M_SF_YES) ||
        (d.setflags == ARMV7M_SF_NOT_IN_IT && !in_it);

    switch (d.op) {
    case ARMV7M_OP_AND:
    case ARMV7M_OP_BIC:
    case ARMV7M_OP_ORR:
    case ARMV7M_OP_ORN:
    case ARMV7M_OP_EOR:
    case ARMV7M_OP_ADD:
    case ARMV7M_OP_ADC:
    case ARMV7M_OP_SBC:
    case ARMV7M_OP_SUB:
    case ARMV7M_OP_RSB:
        if (len == 4u && d.opnd == ARMV7M_OPND_IMM && d.imm2 != 0u) {
            /* The plain 12-bit immediate: its own mnemonic, no S. */
            put(o, "%sw%s %s, %s, #%u", (d.op == ARMV7M_OP_ADD) ? "add" : "sub",
                cc, reg(d.rd), reg(d.rn), (unsigned)d.imm);
            break;
        }
        head(o, name, s, cc, w);
        if (len == 2u && d.rd == d.rn && d.op != ARMV7M_OP_RSB &&
            (d.opnd == ARMV7M_OPND_REG || (w0 >> 13) == 1u ||
             (w0 & 0xFF00u) == 0xB000u)) {
            /* The two-operand forms: rdn is written once. */
            if (d.opnd == ARMV7M_OPND_REG) {
                put(o, " %s, %s", reg(d.rd), reg(d.rm));
            } else {
                put(o, " %s, #%u", reg(d.rd), (unsigned)d.imm);
            }
            break;
        }
        put(o, " %s, %s", reg(d.rd), reg(d.rn));
        if (d.opnd == ARMV7M_OPND_IMM) {
            put(o, ", #%u", (unsigned)d.imm);
        } else {
            put(o, ", %s", reg(d.rm));
            shift_suffix(o, d.shift_t, d.shift_n);
        }
        break;

    case ARMV7M_OP_MOV:
    case ARMV7M_OP_MVN:
        head(o, name, s, cc, w);
        if (d.opnd == ARMV7M_OPND_IMM) {
            put(o, " %s, #%u", reg(d.rd), (unsigned)d.imm);
        } else {
            put(o, " %s, %s", reg(d.rd), reg(d.rm));
            shift_suffix(o, d.shift_t, d.shift_n);
        }
        break;

    case ARMV7M_OP_TST:
    case ARMV7M_OP_TEQ:
    case ARMV7M_OP_CMN:
    case ARMV7M_OP_CMP:
        head(o, name, false, cc, w);
        if (d.opnd == ARMV7M_OPND_IMM) {
            put(o, " %s, #%u", reg(d.rn), (unsigned)d.imm);
        } else {
            put(o, " %s, %s", reg(d.rn), reg(d.rm));
            shift_suffix(o, d.shift_t, d.shift_n);
        }
        break;

    case ARMV7M_OP_LSL:
    case ARMV7M_OP_LSR:
    case ARMV7M_OP_ASR:
    case ARMV7M_OP_ROR:
    case ARMV7M_OP_RRX:
        head(o, name, s, cc, w);
        if (d.opnd == ARMV7M_OPND_REGSHIFT) {
            if (len == 2u) {
                put(o, " %s, %s", reg(d.rd), reg(d.rm));
            } else {
                put(o, " %s, %s, %s", reg(d.rd), reg(d.rn), reg(d.rm));
            }
        } else if (d.opnd == ARMV7M_OPND_IMM) {
            put(o, " %s, %s, #%u", reg(d.rd), reg(d.rn), (unsigned)d.imm);
        } else {
            put(o, " %s, %s", reg(d.rd), reg(d.rn));
        }
        break;

    case ARMV7M_OP_MOVW:
    case ARMV7M_OP_MOVT:
        head(o, name, false, cc, "");
        put(o, " %s, #%u", reg(d.rd), (unsigned)d.imm);
        break;

    case ARMV7M_OP_ADR: {
        const uint32_t base = (pc + 4u) & ~3u;

        head(o, name, false, cc, w);
        put(o, " %s, 0x%08x", reg(d.rd),
            (unsigned)(d.add ? base + d.imm : base - d.imm));
        break;
    }

    case ARMV7M_OP_MUL:
        head(o, name, s, cc, w);
        put(o, " %s, %s, %s", reg(d.rd), reg(d.rn), reg(d.rm));
        break;

    case ARMV7M_OP_SMULL:
    case ARMV7M_OP_UMULL:
    case ARMV7M_OP_SMLAL:
    case ARMV7M_OP_UMLAL:
    case ARMV7M_OP_UMAAL:
    case ARMV7M_OP_SMLALBB:
    case ARMV7M_OP_SMLALBT:
    case ARMV7M_OP_SMLALTB:
    case ARMV7M_OP_SMLALTT:
    case ARMV7M_OP_SMLALD:
    case ARMV7M_OP_SMLALDX:
    case ARMV7M_OP_SMLSLD:
    case ARMV7M_OP_SMLSLDX:
        head(o, name, false, cc, "");
        put(o, " %s, %s, %s, %s", reg(d.rd), reg(d.ra), reg(d.rn), reg(d.rm));
        break;

    case ARMV7M_OP_MLA:
    case ARMV7M_OP_MLS:
    case ARMV7M_OP_SDIV:
    case ARMV7M_OP_UDIV:
    case ARMV7M_OP_SMLABB:
    case ARMV7M_OP_SMLABT:
    case ARMV7M_OP_SMLATB:
    case ARMV7M_OP_SMLATT:
    case ARMV7M_OP_SMULBB:
    case ARMV7M_OP_SMULBT:
    case ARMV7M_OP_SMULTB:
    case ARMV7M_OP_SMULTT:
    case ARMV7M_OP_SMLAD:
    case ARMV7M_OP_SMLADX:
    case ARMV7M_OP_SMUAD:
    case ARMV7M_OP_SMUADX:
    case ARMV7M_OP_SMLAWB:
    case ARMV7M_OP_SMLAWT:
    case ARMV7M_OP_SMULWB:
    case ARMV7M_OP_SMULWT:
    case ARMV7M_OP_SMLSD:
    case ARMV7M_OP_SMLSDX:
    case ARMV7M_OP_SMUSD:
    case ARMV7M_OP_SMUSDX:
    case ARMV7M_OP_SMMLA:
    case ARMV7M_OP_SMMLAR:
    case ARMV7M_OP_SMMUL:
    case ARMV7M_OP_SMMULR:
    case ARMV7M_OP_SMMLS:
    case ARMV7M_OP_SMMLSR:
    case ARMV7M_OP_USAD8:
    case ARMV7M_OP_USADA8:
    case ARMV7M_OP_SEL:
        head(o, name, false, cc, "");
        put(o, " %s, %s, %s", reg(d.rd), reg(d.rn), reg(d.rm));
        if (d.ra != ARMV7M_NOREG) {
            put(o, ", %s", reg(d.ra));
        }
        break;

    case ARMV7M_OP_QADD:
    case ARMV7M_OP_QDADD:
    case ARMV7M_OP_QSUB:
    case ARMV7M_OP_QDSUB:
        /* Rm first: the saturating forms name their operands backwards. */
        head(o, name, false, cc, "");
        put(o, " %s, %s, %s", reg(d.rd), reg(d.rm), reg(d.rn));
        break;

    case ARMV7M_OP_SSAT:
    case ARMV7M_OP_USAT:
    case ARMV7M_OP_SSAT16:
    case ARMV7M_OP_USAT16:
        head(o, name, false, cc, "");
        put(o, " %s, #%u, %s", reg(d.rd), (unsigned)d.imm, reg(d.rn));
        if (d.op == ARMV7M_OP_SSAT || d.op == ARMV7M_OP_USAT) {
            shift_suffix(o, d.shift_t, d.shift_n);
        }
        break;

    case ARMV7M_OP_SBFX:
    case ARMV7M_OP_UBFX:
    case ARMV7M_OP_BFI:
        head(o, name, false, cc, "");
        put(o, " %s, %s, #%u, #%u", reg(d.rd), reg(d.rn), (unsigned)d.imm,
            (unsigned)d.imm2);
        break;

    case ARMV7M_OP_BFC:
        head(o, name, false, cc, "");
        put(o, " %s, #%u, #%u", reg(d.rd), (unsigned)d.imm, (unsigned)d.imm2);
        break;

    case ARMV7M_OP_SXTB:
    case ARMV7M_OP_SXTH:
    case ARMV7M_OP_UXTB:
    case ARMV7M_OP_UXTH:
    case ARMV7M_OP_SXTB16:
    case ARMV7M_OP_UXTB16:
    case ARMV7M_OP_SXTAB:
    case ARMV7M_OP_SXTAH:
    case ARMV7M_OP_UXTAB:
    case ARMV7M_OP_UXTAH:
    case ARMV7M_OP_SXTAB16:
    case ARMV7M_OP_UXTAB16:
        head(o, name, false, cc, w);
        put(o, " %s, ", reg(d.rd));
        if (d.rn != ARMV7M_NOREG) {
            put(o, "%s, ", reg(d.rn));
        }
        put(o, "%s", reg(d.rm));
        if (d.imm != 0u) {
            put(o, ", ror #%u", (unsigned)d.imm);
        }
        break;

    case ARMV7M_OP_REV:
    case ARMV7M_OP_REV16:
    case ARMV7M_OP_REVSH:
    case ARMV7M_OP_RBIT:
    case ARMV7M_OP_CLZ:
        head(o, name, false, cc, w);
        put(o, " %s, %s", reg(d.rd), reg(d.rm));
        break;

    case ARMV7M_OP_PKHBT:
    case ARMV7M_OP_PKHTB:
        head(o, name, false, cc, "");
        put(o, " %s, %s, %s", reg(d.rd), reg(d.rn), reg(d.rm));
        shift_suffix(o, d.shift_t, d.shift_n);
        break;

    case ARMV7M_OP_PARALLEL: {
        static const char *const k_pre[2][3] = {{"s", "q", "sh"},
                                                {"u", "uq", "uh"}};
        static const char *const k_op[8] = {"add8",  "add16", "asx", NULL,
                                            "sub8",  "sub16", "sax", NULL};

        put(o, "%s%s%s %s, %s, %s", k_pre[(d.imm >> 8) & 1u][(d.imm >> 4) & 3u],
            k_op[d.imm & 7u], cc, reg(d.rd), reg(d.rn), reg(d.rm));
        break;
    }

    case ARMV7M_OP_LDR:
    case ARMV7M_OP_LDRB:
    case ARMV7M_OP_LDRH:
    case ARMV7M_OP_LDRSB:
    case ARMV7M_OP_LDRSH:
    case ARMV7M_OP_STR:
    case ARMV7M_OP_STRB:
    case ARMV7M_OP_STRH:
        head(o, name, false, cc, w);
        put(o, " %s, ", reg(d.rd));
        address(o, &d, pc);
        break;

    case ARMV7M_OP_LDRT:
    case ARMV7M_OP_LDRBT:
    case ARMV7M_OP_LDRHT:
    case ARMV7M_OP_LDRSBT:
    case ARMV7M_OP_LDRSHT:
    case ARMV7M_OP_STRT:
    case ARMV7M_OP_STRBT:
    case ARMV7M_OP_STRHT:
        head(o, name, false, cc, "");
        put(o, " %s, ", reg(d.rd));
        address(o, &d, pc);
        break;

    case ARMV7M_OP_PLD:
    case ARMV7M_OP_PLI:
        head(o, name, false, cc, "");
        put(o, " ");
        address(o, &d, pc);
        break;

    case ARMV7M_OP_LDRD:
    case ARMV7M_OP_STRD:
        head(o, name, false, cc, "");
        put(o, " %s, %s, ", reg(d.rd), reg(d.ra));
        address(o, &d, pc);
        break;

    case ARMV7M_OP_LDREX:
    case ARMV7M_OP_LDREXB:
    case ARMV7M_OP_LDREXH:
        head(o, name, false, cc, "");
        put(o, " %s, [%s", reg(d.rd), reg(d.rn));
        if (d.imm != 0u) {
            put(o, ", #%u", (unsigned)d.imm);
        }
        put(o, "]");
        break;

    case ARMV7M_OP_STREX:
    case ARMV7M_OP_STREXB:
    case ARMV7M_OP_STREXH:
        head(o, name, false, cc, "");
        put(o, " %s, %s, [%s", reg(d.rd), reg(d.rm), reg(d.rn));
        if (d.imm != 0u) {
            put(o, ", #%u", (unsigned)d.imm);
        }
        put(o, "]");
        break;

    case ARMV7M_OP_CLREX:
        head(o, name, false, cc, "");
        break;

    case ARMV7M_OP_LDM:
    case ARMV7M_OP_LDMDB:
    case ARMV7M_OP_STM:
    case ARMV7M_OP_STMDB:
        head(o, name, false, cc, w);
        put(o, " %s%s, ", reg(d.rn), d.wback ? "!" : "");
        reglist(o, d.imm);
        break;

    case ARMV7M_OP_PUSH:
    case ARMV7M_OP_POP:
        head(o, name, false, cc, w);
        put(o, " ");
        reglist(o, d.imm);
        break;

    case ARMV7M_OP_TBB:
        head(o, name, false, cc, "");
        put(o, " [%s, %s]", reg(d.rn), reg(d.rm));
        break;

    case ARMV7M_OP_TBH:
        head(o, name, false, cc, "");
        put(o, " [%s, %s, lsl #1]", reg(d.rn), reg(d.rm));
        break;

    case ARMV7M_OP_B:
        head(o, "b", false, cc, (len == 4u) ? ".w" : ".n");
        put(o, " 0x%08x", (unsigned)(pc + 4u + d.imm));
        break;

    case ARMV7M_OP_BCC:
        head(o, "b", false, k_cond[d.cond], (len == 4u) ? ".w" : ".n");
        put(o, " 0x%08x", (unsigned)(pc + 4u + d.imm));
        break;

    case ARMV7M_OP_BL:
        head(o, name, false, cc, "");
        put(o, " 0x%08x", (unsigned)(pc + 4u + d.imm));
        break;

    case ARMV7M_OP_BX:
    case ARMV7M_OP_BLX:
        head(o, name, false, cc, "");
        put(o, " %s", reg(d.rm));
        break;

    case ARMV7M_OP_CBZ:
    case ARMV7M_OP_CBNZ:
        head(o, name, false, "", "");
        put(o, " %s, 0x%08x", reg(d.rn), (unsigned)(pc + 4u + d.imm));
        break;

    case ARMV7M_OP_SVC:
        head(o, name, false, cc, "");
        put(o, " %u", (unsigned)d.imm);
        break;

    case ARMV7M_OP_BKPT:
        put(o, "bkpt 0x%04x", (unsigned)d.imm);
        break;

    case ARMV7M_OP_UDF:
        put(o, "udf%s #%u", (len == 4u) ? ".w" : "", (unsigned)d.imm);
        break;

    case ARMV7M_OP_IT: {
        uint32_t low = 0u;

        while (((d.imm >> low) & 1u) == 0u) {
            low++;
        }
        put(o, "it");
        for (uint32_t i = 3u; i > low; i--) {
            put(o, "%c", (((d.imm >> i) & 1u) == (d.cond & 1u)) ? 't' : 'e');
        }
        put(o, " %s", (d.cond == 14u) ? "al" : k_cond[d.cond]);
        break;
    }

    case ARMV7M_OP_NOP:
    case ARMV7M_OP_YIELD:
    case ARMV7M_OP_WFE:
    case ARMV7M_OP_WFI:
    case ARMV7M_OP_SEV:
        if (d.imm > 4u) {
            /* A hint with no name: it executes as a NOP and is not one. */
            undefined(o, w0, w1, len);
            put(o, "  ; hint %u", (unsigned)d.imm);
            break;
        }
        head(o, name, false, cc, w);
        break;

    case ARMV7M_OP_CPSIE:
    case ARMV7M_OP_CPSID:
        if (d.imm == 0u) {
            undefined(o, w0, w1, len);
            break;
        }
        put(o, "%s %s%s", (d.op == ARMV7M_OP_CPSIE) ? "cpsie" : "cpsid",
            (d.imm & 2u) ? "i" : "", (d.imm & 1u) ? "f" : "");
        break;

    case ARMV7M_OP_MSR:
    case ARMV7M_OP_MRS: {
        static const char *const k_mask[4] = {"", "_g", "_nzcvq", "_nzcvqg"};
        const char *const sys = sysm_name(d.imm2);

        if (sys == NULL) {
            undefined(o, w0, w1, len);
            put(o, "  ; %s sysm %u", (d.op == ARMV7M_OP_MSR) ? "msr" : "mrs",
                (unsigned)d.imm2);
            break;
        }
        head(o, name, false, cc, "");
        if (d.op == ARMV7M_OP_MRS) {
            put(o, " %s, %s", reg(d.rd), sys);
        } else {
            put(o, " %s%s, %s", sys, (d.imm2 < 4u) ? k_mask[d.imm & 3u] : "",
                reg(d.rn));
        }
        break;
    }

    case ARMV7M_OP_DSB:
    case ARMV7M_OP_DMB:
    case ARMV7M_OP_ISB:
        head(o, name, false, cc, "");
        if (d.imm == 15u) {
            put(o, " sy");
        } else {
            put(o, " #%u", (unsigned)d.imm);
        }
        break;

    /* ---- floating point ---- */

    case ARMV7M_OP_VADD:
    case ARMV7M_OP_VSUB:
    case ARMV7M_OP_VMUL:
    case ARMV7M_OP_VDIV:
    case ARMV7M_OP_VNMUL:
    case ARMV7M_OP_VMLA:
    case ARMV7M_OP_VMLS:
    case ARMV7M_OP_VNMLA:
    case ARMV7M_OP_VNMLS:
    case ARMV7M_OP_VFMA:
    case ARMV7M_OP_VFMS:
    case ARMV7M_OP_VFNMA:
    case ARMV7M_OP_VFNMS:
    case ARMV7M_OP_VMAXNM:
    case ARMV7M_OP_VMINNM:
        head(o, name, false, cc, ".f32");
        put(o, " s%u, s%u, s%u", d.rd, d.rn, d.rm);
        break;

    case ARMV7M_OP_VSEL: {
        static const char *const k[4] = {"eq", "vs", "ge", "gt"};

        put(o, "vsel%s.f32 s%u, s%u, s%u", k[d.cond & 3u], d.rd, d.rn, d.rm);
        break;
    }

    case ARMV7M_OP_VSQRT:
    case ARMV7M_OP_VABS:
    case ARMV7M_OP_VNEG:
    case ARMV7M_OP_VMOV:
    case ARMV7M_OP_VCMP:
    case ARMV7M_OP_VCMPE:
    case ARMV7M_OP_VRINTA:
    case ARMV7M_OP_VRINTN:
    case ARMV7M_OP_VRINTP:
    case ARMV7M_OP_VRINTM:
    case ARMV7M_OP_VRINTR:
    case ARMV7M_OP_VRINTZ:
    case ARMV7M_OP_VRINTX:
        head(o, name, false, cc, ".f32");
        put(o, " s%u, s%u", d.rd, d.rm);
        break;

    case ARMV7M_OP_VCMPZ:
    case ARMV7M_OP_VCMPEZ:
        head(o, (d.op == ARMV7M_OP_VCMPZ) ? "vcmp" : "vcmpe", false, cc, ".f32");
        put(o, " s%u, #0.0", d.rd);
        break;

    case ARMV7M_OP_VMOVI:
        head(o, "vmov", false, cc, ".f32");
        put(o, " s%u, ", d.rd);
        fp_imm(o, d.imm);
        break;

    case ARMV7M_OP_VCVT_TOS:
    case ARMV7M_OP_VCVT_TOU:
    case ARMV7M_OP_VCVTR_TOS:
    case ARMV7M_OP_VCVTR_TOU: {
        const bool r = d.op == ARMV7M_OP_VCVTR_TOS || d.op == ARMV7M_OP_VCVTR_TOU;
        const bool u = d.op == ARMV7M_OP_VCVT_TOU || d.op == ARMV7M_OP_VCVTR_TOU;

        put(o, "vcvt%s%s.%s32.f32 s%u, s%u", r ? "r" : "", cc, u ? "u" : "s",
            d.rd, d.rm);
        break;
    }

    case ARMV7M_OP_VCVT_FROMS:
    case ARMV7M_OP_VCVT_FROMU:
        put(o, "vcvt%s.f32.%s32 s%u, s%u", cc,
            (d.op == ARMV7M_OP_VCVT_FROMU) ? "u" : "s", d.rd, d.rm);
        break;

    case ARMV7M_OP_VCVT_FIX: {
        const char *const sign = (d.imm2 & 2u) ? "u" : "s";
        const unsigned bits = (d.imm2 & 1u) ? 32u : 16u;

        if ((d.imm2 & 4u) != 0u) {
            put(o, "vcvt%s.%s%u.f32", cc, sign, bits);
        } else {
            put(o, "vcvt%s.f32.%s%u", cc, sign, bits);
        }
        put(o, " s%u, s%u, #%u", d.rd, d.rd, (unsigned)d.imm);
        break;
    }

    case ARMV7M_OP_VCVTB:
    case ARMV7M_OP_VCVTT:
        head(o, name, false, cc, d.imm ? ".f16.f32" : ".f32.f16");
        put(o, " s%u, s%u", d.rd, d.rm);
        break;

    case ARMV7M_OP_VCVTA:
    case ARMV7M_OP_VCVTN:
    case ARMV7M_OP_VCVTP:
    case ARMV7M_OP_VCVTM:
        head(o, name, false, "", d.imm ? ".s32.f32" : ".u32.f32");
        put(o, " s%u, s%u", d.rd, d.rm);
        break;

    case ARMV7M_OP_VMOV_CORE:
        head(o, "vmov", false, cc, "");
        if (d.imm != 0u) {
            put(o, " %s, s%u", reg(d.rd), d.rn);
        } else {
            put(o, " s%u, %s", d.rn, reg(d.rd));
        }
        break;

    case ARMV7M_OP_VMOV_CORE2:
        head(o, "vmov", false, cc, "");
        if (d.fp_dbl) {
            if (d.imm != 0u) {
                put(o, " %s, %s, d%u", reg(d.rd), reg(d.ra), d.rm / 2u);
            } else {
                put(o, " d%u, %s, %s", d.rm / 2u, reg(d.rd), reg(d.ra));
            }
        } else if (d.imm != 0u) {
            put(o, " %s, %s, s%u, s%u", reg(d.rd), reg(d.ra), d.rm, d.rm + 1u);
        } else {
            put(o, " s%u, s%u, %s, %s", d.rm, d.rm + 1u, reg(d.rd), reg(d.ra));
        }
        break;

    case ARMV7M_OP_VMOV_SCALAR:
        head(o, "vmov", false, cc, ".32");
        if (d.imm != 0u) {
            put(o, " %s, d%u[%u]", reg(d.rd), d.rn, (unsigned)d.imm2);
        } else {
            put(o, " d%u[%u], %s", d.rn, (unsigned)d.imm2, reg(d.rd));
        }
        break;

    case ARMV7M_OP_VMRS:
        head(o, name, false, cc, "");
        put(o, " %s, fpscr", (d.rd == 15u) ? "APSR_nzcv" : reg(d.rd));
        break;

    case ARMV7M_OP_VMSR:
        head(o, name, false, cc, "");
        put(o, " fpscr, %s", reg(d.rd));
        break;

    case ARMV7M_OP_VLDR:
    case ARMV7M_OP_VSTR:
        head(o, name, false, cc, "");
        put(o, " %c%u, ", d.fp_dbl ? 'd' : 's', d.fp_dbl ? d.rd / 2u : d.rd);
        address(o, &d, pc);
        break;

    case ARMV7M_OP_VLDM:
    case ARMV7M_OP_VSTM:
    case ARMV7M_OP_VLDMDB:
    case ARMV7M_OP_VSTMDB: {
        const bool load = d.op == ARMV7M_OP_VLDM || d.op == ARMV7M_OP_VLDMDB;
        const bool db = d.op == ARMV7M_OP_VLDMDB || d.op == ARMV7M_OP_VSTMDB;

        put(o, "v%s%s%s %s%s, ", load ? "ldm" : "stm", db ? "db" : "ia", cc,
            reg(d.rn), d.wback ? "!" : "");
        fp_list(o, &d);
        break;
    }

    case ARMV7M_OP_VPUSH:
    case ARMV7M_OP_VPOP:
        head(o, name, false, cc, "");
        put(o, " ");
        fp_list(o, &d);
        break;

    default:
        undefined(o, w0, w1, len);
        break;
    }
    return out.len;
}

/* ITAdvance, as the architecture spells it. */
static uint32_t it_advance(uint32_t itstate)
{
    if ((itstate & 7u) == 0u) {
        return 0u;
    }
    return (itstate & 0xE0u) | ((itstate << 1) & 0x1Fu);
}

size_t armv7m_disasm(char *buf, size_t buflen, uint32_t pc, uint64_t insn,
                     unsigned len)
{
    static uint32_t s_next_pc;
    static uint32_t s_itstate;
    const uint32_t w0 = (uint32_t)(insn & 0xFFFFu);
    uint32_t it = 0u;
    size_t n;

    if (pc == s_next_pc) {
        it = s_itstate;
    }
    n = armv7m_disasm_it(buf, buflen, pc, insn, len, it);

    if (len == 2u && (w0 & 0xFF00u) == 0xBF00u && (w0 & 0x000Fu) != 0u) {
        s_itstate = w0 & 0xFFu;
    } else {
        s_itstate = it_advance(it);
    }
    s_next_pc = pc + len;
    return n;
}

#endif /* ARMV7M_ENABLE_DISASM */
