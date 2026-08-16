/* SPDX-License-Identifier: Apache-2.0 */
/*
 * rv_disasm.c - RV32 disassembler.
 *
 * Formatting is done with a small hand-rolled emitter rather than snprintf
 * so the firmware does not have to pull in the printf machinery; on a part
 * with 512 KiB of flash that would otherwise be the single largest thing
 * the debug path drags in.
 */

#include "rv32/rv_disasm.h"
#include "rv32/rv_decode.h"
#include "rv32/rv_csr.h"

#include <string.h>

#if RV_ENABLE_DISASM

/* ------------------------------------------------------------------ */
/* Output buffer                                                       */
/* ------------------------------------------------------------------ */

typedef struct {
    char  *buf;
    size_t cap;    /* usable characters, excluding the NUL */
    size_t len;
} out_t;

static void emit_ch(out_t *o, char c)
{
    if (o->len < o->cap) {
        o->buf[o->len++] = c;
    }
}

static void emit_str(out_t *o, const char *s)
{
    while (*s != '\0') {
        emit_ch(o, *s++);
    }
}

static void emit_uint(out_t *o, uint32_t v)
{
    char tmp[10];
    unsigned n = 0;
    do {
        tmp[n++] = (char)('0' + (v % 10u));
        v /= 10u;
    } while (v != 0u);
    while (n != 0u) {
        emit_ch(o, tmp[--n]);
    }
}

static void emit_int(out_t *o, int32_t v)
{
    if (v < 0) {
        emit_ch(o, '-');
        /* Negating INT32_MIN overflows; go through unsigned. */
        emit_uint(o, (uint32_t)0 - (uint32_t)v);
    } else {
        emit_uint(o, (uint32_t)v);
    }
}

static void emit_hex(out_t *o, uint32_t v)
{
    static const char hex[] = "0123456789abcdef";
    emit_str(o, "0x");
    bool started = false;
    for (int i = 28; i >= 0; i -= 4) {
        const unsigned d = (v >> i) & 0xFu;
        if (d != 0u || started || i == 0) {
            emit_ch(o, hex[d]);
            started = true;
        }
    }
}

/* ------------------------------------------------------------------ */

static const char *const reg_abi[32] = {
    "zero", "ra", "sp",  "gp",  "tp", "t0", "t1", "t2",
    "s0",   "s1", "a0",  "a1",  "a2", "a3", "a4", "a5",
    "a6",   "a7", "s2",  "s3",  "s4", "s5", "s6", "s7",
    "s8",   "s9", "s10", "s11", "t3", "t4", "t5", "t6",
};

const char *rv_reg_name(unsigned r)
{
    return (r < 32u) ? reg_abi[r] : "?";
}

static void emit_reg(out_t *o, unsigned r)
{
    emit_str(o, rv_reg_name(r));
}

/* mnemonic, padded so the operands line up in a trace. */
static void emit_mn(out_t *o, const char *m)
{
    emit_str(o, m);
    for (size_t i = strlen(m); i < 8u; i++) {
        emit_ch(o, ' ');
    }
}

/* "rd, rs1, rs2" */
static void emit_rrr(out_t *o, const char *m, uint32_t i)
{
    emit_mn(o, m);
    emit_reg(o, rv_rd(i));   emit_str(o, ", ");
    emit_reg(o, rv_rs1(i));  emit_str(o, ", ");
    emit_reg(o, rv_rs2(i));
}

/* "rd, rs1, imm" */
static void emit_rri(out_t *o, const char *m, uint32_t i, int32_t imm)
{
    emit_mn(o, m);
    emit_reg(o, rv_rd(i));   emit_str(o, ", ");
    emit_reg(o, rv_rs1(i));  emit_str(o, ", ");
    emit_int(o, imm);
}

/* "rd, imm(rs1)" */
static void emit_mem(out_t *o, const char *m, uint32_t rd, int32_t imm,
                     uint32_t rs1)
{
    emit_mn(o, m);
    emit_reg(o, rd);  emit_str(o, ", ");
    emit_int(o, imm); emit_ch(o, '(');
    emit_reg(o, rs1); emit_ch(o, ')');
}

/* ------------------------------------------------------------------ */

static const char *const branch_mn[8] = {
    "beq", "bne", NULL, NULL, "blt", "bge", "bltu", "bgeu"
};
static const char *const load_mn[8] = {
    "lb", "lh", "lw", NULL, "lbu", "lhu", NULL, NULL
};
static const char *const store_mn[8] = {
    "sb", "sh", "sw", NULL, NULL, NULL, NULL, NULL
};
static const char *const opimm_mn[8] = {
    "addi", "slli", "slti", "sltiu", "xori", NULL, "ori", "andi"
};
static const char *const op_mn[8] = {
    "add", "sll", "slt", "sltu", "xor", "srl", "or", "and"
};
static const char *const mul_mn[8] = {
    "mul", "mulh", "mulhsu", "mulhu", "div", "divu", "rem", "remu"
};
static const char *const amo_mn[32] = {
    "amoadd.w", "amoswap.w", "lr.w", "sc.w",
    "amoxor.w", NULL, NULL, NULL,
    "amoor.w", NULL, NULL, NULL,
    "amoand.w", NULL, NULL, NULL,
    "amomin.w", NULL, NULL, NULL,
    "amomax.w", NULL, NULL, NULL,
    "amominu.w", NULL, NULL, NULL,
    "amomaxu.w", NULL, NULL, NULL,
};
static const char *const csr_mn[8] = {
    NULL, "csrrw", "csrrs", "csrrc", NULL, "csrrwi", "csrrsi", "csrrci"
};

size_t rv_disasm(char *buf, size_t buflen, uint32_t pc, uint64_t insn64,
                 unsigned len)
{
    out_t o = { buf, (buflen == 0u) ? 0u : buflen - 1u, 0u };
    /* RV32 is 2 or 4 bytes and the caller has already expanded RVC, so
     * the length says nothing the encoding does not. */
    const uint32_t insn = (uint32_t)insn64;

    (void)len;
    if (buflen == 0u) {
        return 0u;
    }

    const uint32_t f3 = rv_funct3(insn);
    const uint32_t f7 = rv_funct7(insn);
    const uint32_t op = rv_opcode(insn);

    switch (op) {
    case OP_LUI:
        emit_mn(&o, "lui");
        emit_reg(&o, rv_rd(insn));
        emit_str(&o, ", ");
        emit_hex(&o, rv_imm_u(insn) >> 12);
        break;

    case OP_AUIPC:
        emit_mn(&o, "auipc");
        emit_reg(&o, rv_rd(insn));
        emit_str(&o, ", ");
        emit_hex(&o, rv_imm_u(insn) >> 12);
        break;

    case OP_JAL: {
        const uint32_t target = pc + (uint32_t)rv_imm_j(insn);
        /* jal x0, target is the canonical unconditional jump. */
        emit_mn(&o, rv_rd(insn) == 0u ? "j" : "jal");
        if (rv_rd(insn) != 0u && rv_rd(insn) != 1u) {
            emit_reg(&o, rv_rd(insn));
            emit_str(&o, ", ");
        }
        emit_hex(&o, target);
        break;
    }

    case OP_JALR:
        if (rv_rd(insn) == 0u && rv_rs1(insn) == 1u && rv_imm_i(insn) == 0) {
            emit_str(&o, "ret");
            break;
        }
        emit_mem(&o, rv_rd(insn) == 0u ? "jr" : "jalr",
                 rv_rd(insn), rv_imm_i(insn), rv_rs1(insn));
        break;

    case OP_BRANCH: {
        if (branch_mn[f3] == NULL) {
            emit_str(&o, "illegal");
            break;
        }
        emit_mn(&o, branch_mn[f3]);
        emit_reg(&o, rv_rs1(insn)); emit_str(&o, ", ");
        emit_reg(&o, rv_rs2(insn)); emit_str(&o, ", ");
        emit_hex(&o, pc + (uint32_t)rv_imm_b(insn));
        break;
    }

    case OP_LOAD:
        if (load_mn[f3] == NULL) {
            emit_str(&o, "illegal");
            break;
        }
        emit_mem(&o, load_mn[f3], rv_rd(insn), rv_imm_i(insn), rv_rs1(insn));
        break;

    case OP_STORE:
        if (store_mn[f3] == NULL) {
            emit_str(&o, "illegal");
            break;
        }
        emit_mem(&o, store_mn[f3], rv_rs2(insn), rv_imm_s(insn), rv_rs1(insn));
        break;

    case OP_IMM:
        if (f3 == 5u) {   /* srli / srai share funct3 */
            emit_rri(&o, (f7 == 0x20u) ? "srai" : "srli", insn,
                     (int32_t)rv_rs2(insn));
            break;
        }
        if (f3 == 1u) {
            emit_rri(&o, "slli", insn, (int32_t)rv_rs2(insn));
            break;
        }
        /* addi rd, zero, imm is the canonical load-immediate. */
        if (f3 == 0u && rv_rs1(insn) == 0u) {
            emit_mn(&o, "li");
            emit_reg(&o, rv_rd(insn));
            emit_str(&o, ", ");
            emit_int(&o, rv_imm_i(insn));
            break;
        }
        if (f3 == 0u && rv_imm_i(insn) == 0) {
            emit_mn(&o, "mv");
            emit_reg(&o, rv_rd(insn));
            emit_str(&o, ", ");
            emit_reg(&o, rv_rs1(insn));
            break;
        }
        emit_rri(&o, opimm_mn[f3], insn, rv_imm_i(insn));
        break;

    case OP_OP:
        if (f7 == 0x01u) {
            emit_rrr(&o, mul_mn[f3], insn);
        } else if (f7 == 0x20u) {
            emit_rrr(&o, (f3 == 0u) ? "sub" : "sra", insn);
        } else {
            emit_rrr(&o, op_mn[f3], insn);
        }
        break;

    case OP_MISC_MEM:
        emit_str(&o, (f3 == 1u) ? "fence.i" : "fence");
        break;

#if RV_EXT_F
    /*
     * Floating point, which this file did not decode at all.
     *
     * That was not merely a gap in a listing. A trace histogrammed by
     * mnemonic to ask "how much FP does this guest execute" answered
     * *zero*, because every FP instruction printed as `illegal` -- and
     * in the hard-float Whetstone that bucket was 60% of the window.
     * A category the instrument cannot represent reads exactly like a
     * category that is empty, and the conclusion drawn from it was
     * "there is no floating point here to accelerate".
     *
     * The register names are f0..f31 rather than the ABI's ft0/fa0
     * spellings for the same reason rv_reg_name gives the integer file
     * theirs: those are a calling convention, and the disassembler
     * should not assert one the core does not have. Here that matters
     * more than there, because the FP ABI is a build choice -- this
     * tree has guests at both ilp32 and ilp32f.
     */
    case OP_LOAD_FP:
    case OP_STORE_FP: {
        static const char *const w[8] = {
            NULL, NULL, "w", "d", NULL, NULL, NULL, NULL
        };
        char mn[8];

        if (w[f3] == NULL) {
            emit_str(&o, "illegal");
            break;
        }
        mn[0] = 'f';
        mn[1] = (op == OP_LOAD_FP) ? 'l' : 's';
        mn[2] = w[f3][0];
        mn[3] = '\0';
        emit_mn(&o, mn);
        emit_ch(&o, 'f');
        emit_uint(&o, (op == OP_LOAD_FP) ? rv_rd(insn) : rv_rs2(insn));
        emit_str(&o, ", ");
        emit_int(&o, (op == OP_LOAD_FP) ? rv_imm_i(insn) : rv_imm_s(insn));
        emit_ch(&o, '(');
        emit_reg(&o, rv_rs1(insn));
        emit_ch(&o, ')');
        break;
    }

    case OP_MADD:
    case OP_MSUB:
    case OP_NMSUB:
    case OP_NMADD: {
        static const char *const fma[4] = {
            "fmadd", "fmsub", "fnmsub", "fnmadd"
        };
        const uint32_t fmt = rv_funct7(insn) & 3u;

        emit_str(&o, fma[(op >> 2) & 3u]);
        emit_ch(&o, '.');
        emit_ch(&o, (fmt == 1u) ? 'd' : 's');
        emit_ch(&o, ' ');
        emit_ch(&o, 'f'); emit_uint(&o, rv_rd(insn));  emit_str(&o, ", ");
        emit_ch(&o, 'f'); emit_uint(&o, rv_rs1(insn)); emit_str(&o, ", ");
        emit_ch(&o, 'f'); emit_uint(&o, rv_rs2(insn)); emit_str(&o, ", ");
        emit_ch(&o, 'f'); emit_uint(&o, insn >> 27);
        break;
    }

    case OP_FP: {
        /*
         * funct5 names the operation and the two low bits of funct7 the
         * format. Getting the funct5 *values* right is the whole of
         * this case: fsgnj is 4 and fmin 5, not 0x10 and 0x11 -- those
         * are the funct7 values, and using them put both families where
         * nothing decodes and printed `illegal` for every sign
         * injection.
         *
         * Which register file each operand comes from is the other
         * half. The comparisons, fcvt.w.s and fclass write an *integer*
         * rd; fcvt.s.w and fmv.w.x read an integer rs1. Printing them
         * all as f registers reads plausibly and is wrong in exactly
         * the places a reader is trying to follow a value between the
         * two files.
         */
        const uint32_t f5v = f7 >> 2;
        const uint32_t fmt = f7 & 3u;
        const char sfx = (fmt == 1u) ? 'd' : 's';
        const char *m = NULL;
        bool rd_is_fp = true, rs1_is_fp = true, two_src = true, sfxd = true;

        switch (f5v) {
        case 0x00u: m = "fadd";  break;
        case 0x01u: m = "fsub";  break;
        case 0x02u: m = "fmul";  break;
        case 0x03u: m = "fdiv";  break;
        case 0x0Bu: m = "fsqrt"; two_src = false; break;
        case 0x04u: {
            static const char *const sg[3] = { "fsgnj", "fsgnjn", "fsgnjx" };
            m = (f3 < 3u) ? sg[f3] : NULL;
            break;
        }
        case 0x05u: m = (f3 == 0u) ? "fmin" : "fmax"; break;
        case 0x14u: {
            static const char *const cm[3] = { "fle", "flt", "feq" };
            m = (f3 < 3u) ? cm[f3] : NULL;
            rd_is_fp = false;
            break;
        }
        case 0x08u:
            /* Precision conversion: the *destination* format is in fmt
             * and rs2 names the source, so the suffix is written out. */
            m = (fmt == 1u) ? "fcvt.d.s" : "fcvt.s.d";
            two_src = false; sfxd = false;
            break;
        /*
         * The integer conversions name *both* formats, and in opposite
         * orders: fcvt.w.s goes FP to integer and fcvt.s.w the other
         * way. Built whole rather than by appending a suffix, because
         * appending gave "fcvt.w.x" for what is fcvt.s.w -- the suffix
         * belongs in the middle for one of them.
         */
        case 0x18u:
            m = (rv_rs2(insn) == 0u)
              ? ((fmt == 1u) ? "fcvt.w.d"  : "fcvt.w.s")
              : ((fmt == 1u) ? "fcvt.wu.d" : "fcvt.wu.s");
            rd_is_fp = false; two_src = false; sfxd = false;
            break;
        case 0x1Au:
            m = (rv_rs2(insn) == 0u)
              ? ((fmt == 1u) ? "fcvt.d.w"  : "fcvt.s.w")
              : ((fmt == 1u) ? "fcvt.d.wu" : "fcvt.s.wu");
            rs1_is_fp = false; two_src = false; sfxd = false;
            break;
        case 0x1Cu:
            m = (f3 == 0u) ? "fmv.x.w" : "fclass";
            rd_is_fp = false; two_src = false;
            sfxd = (f3 != 0u);
            break;
        case 0x1Eu:
            m = "fmv.w.x";
            rs1_is_fp = false; two_src = false; sfxd = false;
            break;
        default: break;
        }

        if (m == NULL) {
            emit_str(&o, "illegal");
            break;
        }
        emit_str(&o, m);
        if (sfxd) {
            emit_ch(&o, '.');
            emit_ch(&o, sfx);
        }
        emit_ch(&o, ' ');
        if (rd_is_fp) { emit_ch(&o, 'f'); emit_uint(&o, rv_rd(insn)); }
        else          { emit_reg(&o, rv_rd(insn)); }
        emit_str(&o, ", ");
        if (rs1_is_fp) { emit_ch(&o, 'f'); emit_uint(&o, rv_rs1(insn)); }
        else           { emit_reg(&o, rv_rs1(insn)); }
        if (two_src) {
            emit_str(&o, ", ");
            emit_ch(&o, 'f'); emit_uint(&o, rv_rs2(insn));
        }
        break;
    }
#endif /* RV_EXT_F */

    case OP_AMO: {
        const char *m = amo_mn[rv_funct7(insn) >> 2];
        if (m == NULL) {
            emit_str(&o, "illegal");
            break;
        }
        emit_mn(&o, m);
        emit_reg(&o, rv_rd(insn));
        emit_str(&o, ", ");
        if ((rv_funct7(insn) >> 2) != 0x02u) {   /* lr.w has no source */
            emit_reg(&o, rv_rs2(insn));
            emit_str(&o, ", ");
        }
        emit_ch(&o, '(');
        emit_reg(&o, rv_rs1(insn));
        emit_ch(&o, ')');
        break;
    }

    case OP_SYSTEM: {
        if (f3 == 0u) {
            switch (insn >> 20) {
            case 0x000u: emit_str(&o, "ecall"); break;
            case 0x001u: emit_str(&o, "ebreak"); break;
            case 0x302u: emit_str(&o, "mret"); break;
            case 0x105u: emit_str(&o, "wfi"); break;
            default:     emit_str(&o, "illegal"); break;
            }
            break;
        }
        if (csr_mn[f3] == NULL) {
            emit_str(&o, "illegal");
            break;
        }

        const uint32_t csr = insn >> 20;
        emit_mn(&o, csr_mn[f3]);
        emit_reg(&o, rv_rd(insn));
        emit_str(&o, ", ");

        const char *cn = rv_csr_name(csr);
        if (cn != NULL) {
            emit_str(&o, cn);
        } else {
            emit_hex(&o, csr);
        }
        emit_str(&o, ", ");

        if (f3 & 4u) {
            emit_uint(&o, rv_rs1(insn));    /* immediate form */
        } else {
            emit_reg(&o, rv_rs1(insn));
        }
        break;
    }

    default:
        emit_str(&o, "illegal");
        break;
    }

    o.buf[o.len] = '\0';
    return o.len;
}

#else /* !RV_ENABLE_DISASM */

size_t rv_disasm(char *buf, size_t buflen, uint32_t pc, uint64_t insn,
                 unsigned len)
{
    (void)pc;
    (void)insn;
    (void)len;
    if (buflen != 0u) {
        buf[0] = '\0';
    }
    return 0u;
}

const char *rv_reg_name(unsigned r)
{
    (void)r;
    return "?";
}

#endif /* RV_ENABLE_DISASM */
