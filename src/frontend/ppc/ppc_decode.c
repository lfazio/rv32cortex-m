/* SPDX-License-Identifier: Apache-2.0 */
/*
 * ppc_decode.c - e200z7 encodings, decoded once.
 *
 * One decoder for everything that needs to know what an instruction is:
 * the interpreter executes the result, the disassembler prints it, the
 * IR translator lowers it. The interpreter used to decode inline, which
 * is how the SCI8 group came to read e_xori. as e_cmpli and every
 * e_cmpi as a compare into CR0 -- a second description of the encoding
 * space is a second thing to be wrong, and nothing held the two to each
 * other.
 *
 * **Where the encodings came from.** Not from a manual: the VLE
 * Programming Environments Manual is not something this tree has. Every
 * slot here was enumerated by assembling and disassembling with binutils
 * -- the 16-bit space exhaustively, the 32-bit space by primary opcode
 * -- and the table is held to that by tests/ppc-check, which feeds what
 * the disassembler prints back through the assembler and requires the
 * same instruction. Which *semantics* each slot has is the e200z759n3
 * core reference manual's where it says, and is marked where it does not.
 *
 * **Reserved fields are checked.** The manual (section 3.16.4) says the
 * core raises an illegal instruction exception for a non-zero reserved
 * field everywhere except bit 31 of the X-form loads and stores and the
 * `z` bits of BO, and so does this.
 */

#include "ppc/ppc_decode.h"

#include <string.h>

typedef ppc_insn_t I;

static const struct {
    const char *name;
    uint8_t sem;
    uint8_t fmt;
} k_mn[PPC_M_COUNT] = {
#define X(id, name, sem, fmt) {name, PPC_S_##sem, PPC_F_##fmt},
    PPC_MNEMONICS(X)
#undef X
};

const char *ppc_mn_name(uint32_t id)
{
    return (id < PPC_M_COUNT) ? k_mn[id].name : "?";
}

uint32_t ppc_mn_format(uint32_t id)
{
    return (id < PPC_M_COUNT) ? k_mn[id].fmt : PPC_F_RAW;
}

static void set(I *d, uint32_t id)
{
    d->id = (uint16_t)id;
    d->sem = k_mn[id].sem;
}

static uint32_t sext(uint32_t v, unsigned bits)
{
    const uint32_t m = 1u << (bits - 1u);

    return ((v & ((m << 1) - 1u)) ^ m) - m;
}

/* Field extraction, from the left as the manual numbers it. */
#define F_RD(w) (((w) >> 21) & 31u)
#define F_RA(w) (((w) >> 16) & 31u)
#define F_RB(w) (((w) >> 11) & 31u)
#define F_MB(w) (((w) >> 6) & 31u)
#define F_ME(w) (((w) >> 1) & 31u)
#define F_XO10(w) (((w) >> 1) & 0x3FFu)

/* ------------------------------------------------------------------ */
/* VLE, 16-bit                                                         */
/* ------------------------------------------------------------------ */

/* rX and rY: four bits, r0-r7 and r24-r31. */
static uint8_t rxy(uint32_t f)
{
    return (uint8_t)ppc_se_reg(f & 15u);
}

static void d16(uint16_t w, I *d)
{
    const uint32_t x = w & 15u;
    const uint32_t y = (w >> 4) & 15u;
    const uint32_t hi = w >> 8;
    const uint32_t ui5 = (w >> 4) & 31u;

    if (hi == 0x00u) {
        if (y == 0u) {
            static const uint16_t k[12] = {
                PPC_M_SE_ILLEGAL, PPC_M_SE_ISYNC, PPC_M_SE_SC,   PPC_M_UNKNOWN,
                PPC_M_SE_BLR,     PPC_M_SE_BLRL,  PPC_M_SE_BCTR, PPC_M_SE_BCTRL,
                PPC_M_SE_RFI,     PPC_M_SE_RFCI,  PPC_M_SE_RFDI, PPC_M_SE_RFMCI,
            };

            if (x < 12u) {
                set(d, k[x]);
                d->bo = 20u; /* branch always */
                d->lk = (x == 5u || x == 7u) ? 1u : 0u;
            }
            return;
        }
        {
            static const uint16_t k[16] = {
                PPC_M_UNKNOWN,  PPC_M_UNKNOWN,  PPC_M_SE_NOT,   PPC_M_SE_NEG,
                PPC_M_UNKNOWN,  PPC_M_UNKNOWN,  PPC_M_UNKNOWN,  PPC_M_UNKNOWN,
                PPC_M_SE_MFLR,  PPC_M_SE_MTLR,  PPC_M_SE_MFCTR, PPC_M_SE_MTCTR,
                PPC_M_SE_EXTZB, PPC_M_SE_EXTSB, PPC_M_SE_EXTZH, PPC_M_SE_EXTSH,
            };
            const uint8_t r = rxy(x);

            set(d, k[y]);
            d->rd = r;
            d->ra = r;
            d->rb = r;
            d->imm = (y == 8u || y == 9u) ? 8u : 9u; /* LR or CTR */
        }
        return;
    }

    switch (hi) {
    case 0x01u: /* se_mr rX, rY */
        set(d, PPC_M_SE_MR);
        d->rd = rxy(x);
        d->ra = rxy(y);
        return;
    case 0x02u: /* se_mtar arX, rY */
        set(d, PPC_M_SE_MTAR);
        d->rd = (uint8_t)(x + 8u);
        d->ra = rxy(y);
        return;
    case 0x03u: /* se_mfar rX, arY */
        set(d, PPC_M_SE_MFAR);
        d->rd = rxy(x);
        d->ra = (uint8_t)(y + 8u);
        return;
    case 0x04u:
    case 0x05u: /* se_add, se_mullw: rX = rX op rY */
        set(d, (hi == 4u) ? PPC_M_SE_ADD : PPC_M_SE_MULLW);
        d->rd = rxy(x);
        d->ra = rxy(x);
        d->rb = rxy(y);
        return;
    case 0x06u: /* se_sub rX, rY: rX - rY, as subf rX, rY, rX */
        set(d, PPC_M_SE_SUB);
        d->rd = rxy(x);
        d->ra = rxy(y);
        d->rb = rxy(x);
        return;
    case 0x07u: /* se_subf rX, rY: rY - rX */
        set(d, PPC_M_SE_SUBF);
        d->rd = rxy(x);
        d->ra = rxy(x);
        d->rb = rxy(y);
        return;
    case 0x0Cu:
    case 0x0Du:
    case 0x0Eu:
    case 0x0Fu: { /* the compares, always into CR0 */
        static const uint16_t k[4] = {PPC_M_SE_CMP, PPC_M_SE_CMPL, PPC_M_SE_CMPH,
                                      PPC_M_SE_CMPHL};

        set(d, k[hi - 0x0Cu]);
        d->ra = rxy(x);
        d->rb = rxy(y);
        return;
    }
    case 0x40u:
    case 0x41u:
    case 0x42u:
    case 0x44u:
    case 0x45u:
    case 0x46u:
    case 0x47u: { /* rX = rX op rY; the shifts and the logicals */
        static const uint16_t k[8] = {
            PPC_M_SE_SRW, PPC_M_SE_SRAW,  PPC_M_SE_SLW, PPC_M_UNKNOWN,
            PPC_M_SE_OR,  PPC_M_SE_ANDC,  PPC_M_SE_AND, PPC_M_SE_AND_,
        };

        set(d, k[hi - 0x40u]);
        d->ra = rxy(x);
        d->rd = rxy(x);
        d->rb = rxy(y);
        d->rc = (hi == 0x47u) ? 1u : 0u;
        return;
    }
    default:
        break;
    }

    switch (w >> 9) {
    case 0x10u: /* se_addi  rX, OIM5: 1..32 */
    case 0x11u: /* se_cmpli rX, OIM5 */
    case 0x12u: /* se_subi  rX, OIM5 */
    case 0x13u: /* se_subi. rX, OIM5 */
    {
        const uint32_t oim = ui5 + 1u;
        static const uint16_t k[4] = {PPC_M_SE_ADDI, PPC_M_SE_CMPLI, PPC_M_SE_SUBI,
                                      PPC_M_SE_SUBI_};
        const uint32_t sel = (w >> 9) - 0x10u;

        set(d, k[sel]);
        d->rd = rxy(x);
        d->ra = rxy(x);
        d->ui = oim;
        d->imm = (sel >= 2u) ? (0u - oim) : oim;
        d->rc = (sel == 3u) ? 1u : 0u;
        return;
    }
    case 0x15u: /* se_cmpi rX, UI5 */
        set(d, PPC_M_SE_CMPI);
        d->ra = rxy(x);
        d->imm = d->ui = ui5;
        return;
    case 0x16u: /* se_bmaski rX, UI5: the low UI5 bits, or all of them */
        set(d, PPC_M_SE_BMASKI);
        d->rd = rxy(x);
        d->ui = ui5;
        d->imm = (ui5 == 0u) ? 0xFFFFFFFFu : ((1u << ui5) - 1u);
        return;
    case 0x17u: /* se_andi rX, UI5 */
        set(d, PPC_M_SE_ANDI);
        d->rd = rxy(x);
        d->ra = rxy(x);
        d->imm = d->ui = ui5;
        return;
    case 0x30u:
    case 0x31u:
    case 0x32u:
    case 0x33u: { /* se_bclri, se_bgeni, se_bseti, se_btsti: bit UI5 from the left */
        static const uint16_t k[4] = {PPC_M_SE_BCLRI, PPC_M_SE_BGENI, PPC_M_SE_BSETI,
                                      PPC_M_SE_BTSTI};
        const uint32_t sel = (w >> 9) - 0x30u;
        const uint32_t bit = 0x80000000u >> ui5;

        set(d, k[sel]);
        d->rd = rxy(x);
        d->ra = rxy(x);
        d->ui = ui5;
        d->imm = (sel == 0u) ? ~bit : bit;
        return;
    }
    case 0x34u: /* se_srwi rX, UI5: rlwinm rX, rX, 32-n, n, 31 */
        set(d, PPC_M_SE_SRWI);
        d->rd = d->ra = rxy(x);
        d->ui = ui5;
        d->sh = (uint8_t)((32u - ui5) & 31u);
        d->mb = (uint8_t)ui5;
        d->me = 31u;
        return;
    case 0x35u: /* se_srawi rX, UI5 */
        set(d, PPC_M_SE_SRAWI);
        d->rd = d->ra = rxy(x);
        d->sh = (uint8_t)ui5;
        d->ui = ui5;
        return;
    case 0x36u: /* se_slwi rX, UI5: rlwinm rX, rX, n, 0, 31-n */
        set(d, PPC_M_SE_SLWI);
        d->rd = d->ra = rxy(x);
        d->ui = ui5;
        d->sh = (uint8_t)ui5;
        d->mb = 0u;
        d->me = (uint8_t)(31u - ui5);
        return;
    default:
        break;
    }

    if ((w >> 11) == 0x09u) { /* se_li rX, UI7 */
        set(d, PPC_M_SE_LI);
        d->rd = rxy(x);
        d->imm = d->ui = (w >> 4) & 0x7Fu;
        return;
    }

    if (hi >= 0x80u && hi <= 0xDFu) {
        /* SD4: data register in bits 4:7, base in 12:15, offset scaled. */
        static const uint16_t k[6] = {PPC_M_SE_LBZ, PPC_M_SE_STB, PPC_M_SE_LHZ,
                                      PPC_M_SE_STH, PPC_M_SE_LWZ, PPC_M_SE_STW};
        const uint32_t kind = (w >> 12) - 8u;
        const uint32_t size = (kind < 2u) ? 1u : ((kind < 4u) ? 2u : 4u);

        set(d, k[kind]);
        d->rd = rxy(w >> 4);
        d->ra = rxy(x);
        d->size = (uint8_t)size;
        d->imm = ((w >> 8) & 15u) * size;
        return;
    }

    if (hi >= 0xE0u && hi <= 0xE7u) {
        /* se_bc BO16, BI16: CR0 only, one bit of BO. */
        set(d, PPC_M_SE_BC);
        d->bo = ((hi & 4u) != 0u) ? 12u : 4u;
        d->bi = (uint8_t)(hi & 3u);
        d->imm = sext((uint32_t)(w & 0xFFu) << 1, 9);
        return;
    }
    if (hi == 0xE8u || hi == 0xE9u) {
        set(d, (hi == 0xE9u) ? PPC_M_SE_BL : PPC_M_SE_B);
        d->lk = (uint8_t)(hi & 1u);
        d->imm = sext((uint32_t)(w & 0xFFu) << 1, 9);
        return;
    }
}

/* ------------------------------------------------------------------ */
/* VLE, 32-bit                                                         */
/* ------------------------------------------------------------------ */

/* SCI8: F, SCL and UI8, the byte placed by SCL and the rest filled from F. */
static uint32_t sci8(uint32_t w)
{
    const uint32_t f = (w >> 10) & 1u;
    const uint32_t scl = (w >> 8) & 3u;
    const uint32_t ui8 = w & 0xFFu;
    const uint32_t sh = 8u * scl;

    return ((f != 0u) ? ~(0xFFu << sh) : 0u) | (ui8 << sh);
}

static void d_vle_d8(uint32_t w, I *d)
{
    const uint32_t xo = (w >> 8) & 0xFFu;
    const uint32_t rt = F_RD(w);

    d->rd = (uint8_t)rt;
    d->ra = (uint8_t)F_RA(w);
    d->imm = sext(w & 0xFFu, 8);
    switch (xo) {
    case 0x00u:
    case 0x01u:
    case 0x02u:
    case 0x03u:
    case 0x04u:
    case 0x05u:
    case 0x06u: {
        static const uint16_t k[7] = {PPC_M_E_LBZU, PPC_M_E_LHZU, PPC_M_E_LWZU,
                                      PPC_M_E_LHAU, PPC_M_E_STBU, PPC_M_E_STHU,
                                      PPC_M_E_STWU};
        static const uint8_t k_size[7] = {1u, 2u, 4u, 2u, 1u, 2u, 4u};

        set(d, k[xo]);
        d->size = k_size[xo];
        d->sext = (xo == 3u) ? 1u : 0u;
        d->upd = 1u;
        return;
    }
    case 0x08u:
        set(d, PPC_M_E_LMW);
        d->ra0 = 1u;
        return;
    case 0x09u:
        set(d, PPC_M_E_STMW);
        d->ra0 = 1u;
        return;
    case 0x10u:
    case 0x11u: {
        /* The volatile context save and restore; RT says which set. */
        static const uint16_t k_l[8] = {PPC_M_E_LMVGPRW, PPC_M_E_LMVSPRW, 0u, 0u,
                                        PPC_M_E_LMVSRRW, PPC_M_E_LMVCSRRW,
                                        PPC_M_E_LMVDSRRW, PPC_M_E_LMVMCSRRW};
        static const uint16_t k_s[8] = {PPC_M_E_STMVGPRW, PPC_M_E_STMVSPRW, 0u, 0u,
                                        PPC_M_E_STMVSRRW, PPC_M_E_STMVCSRRW,
                                        PPC_M_E_STMVDSRRW, PPC_M_E_STMVMCSRRW};
        const uint16_t id = (rt < 8u) ? ((xo == 0x10u) ? k_l : k_s)[rt] : 0u;

        if (id != 0u) {
            set(d, id);
            d->crs = (uint8_t)rt; /* which register set */
            d->ra0 = 1u;
        }
        return;
    }
    default:
        return;
    }
}

static void d_vle_sci8(uint32_t w, I *d)
{
    const uint32_t xo5 = (w >> 11) & 31u; /* XO and Rc */
    const uint32_t imm = sci8(w);
    const uint32_t rd = F_RD(w);

    d->rd = (uint8_t)rd;
    d->ra = (uint8_t)F_RA(w);
    d->imm = imm;
    d->ui = imm;
    d->rc = (uint8_t)(xo5 & 1u);
    switch (xo5) {
    case 0x10u:
    case 0x11u: /* e_addi[.] rD, rA, SCI8 */
        set(d, (xo5 & 1u) ? PPC_M_E_ADDI_ : PPC_M_E_ADDI);
        d->ra0 = 1u;
        return;
    case 0x12u:
    case 0x13u:
        set(d, (xo5 & 1u) ? PPC_M_E_ADDIC_ : PPC_M_E_ADDIC);
        return;
    case 0x14u:
        set(d, PPC_M_E_MULLI);
        return;
    case 0x15u:
        /*
         * e_cmpi and e_cmpli share XO and Rc, told apart by bits 6:8, and
         * the CR field is bits 9:10 -- CR0 to CR3. Reading the field as
         * bits 6:8 made every e_cmpi compare into CR0 and every e_cmpli
         * an e_cmpi.
         */
        if ((rd >> 2) == 0u) {
            set(d, PPC_M_E_CMPI);
        } else if ((rd >> 2) == 1u) {
            set(d, PPC_M_E_CMPLI);
        } else {
            return;
        }
        d->crf = (uint8_t)(rd & 3u);
        d->rc = 0u;
        return;
    case 0x16u:
    case 0x17u:
        set(d, (xo5 & 1u) ? PPC_M_E_SUBFIC_ : PPC_M_E_SUBFIC);
        return;
    case 0x18u:
    case 0x19u:
        set(d, (xo5 & 1u) ? PPC_M_E_ANDI_ : PPC_M_E_ANDI);
        return;
    case 0x1Au:
    case 0x1Bu:
        set(d, (xo5 & 1u) ? PPC_M_E_ORI_ : PPC_M_E_ORI);
        return;
    case 0x1Cu:
    case 0x1Du:
        set(d, (xo5 & 1u) ? PPC_M_E_XORI_ : PPC_M_E_XORI);
        return;
    default:
        return;
    }
}

/* Opcode 28: e_li (LI20), and the I16A and I16L forms. */
static void d_vle_28(uint32_t w, I *d)
{
    if ((w & 0x8000u) == 0u) {
        const uint32_t li20 = (((w >> 11) & 0x0Fu) << 16) | (((w >> 16) & 0x1Fu) << 11) |
                              (w & 0x7FFu);

        set(d, PPC_M_E_LI);
        d->rd = (uint8_t)F_RD(w);
        d->imm = d->ui = sext(li20, 20);
        return;
    }
    {
        const uint32_t xo = (w >> 11) & 31u;
        const uint32_t lo = w & 0x7FFu;
        const uint32_t ui16_l = (F_RA(w) << 11) | lo; /* I16L: rD, ui16 */
        const uint32_t ui16_a = (F_RD(w) << 11) | lo; /* I16A: rA, si16 */

        switch (xo) {
        case 0x11u: /* e_add2i. rA, si16 */
        case 0x12u: /* e_add2is rA, si16 */
        case 0x13u: /* e_cmp16i rA, si16 */
        case 0x14u: /* e_mull2i rA, si16 */
        case 0x15u: /* e_cmpl16i rA, ui16 */
        case 0x16u: /* e_cmph16i rA, si16 */
        case 0x17u: /* e_cmphl16i rA, ui16 */
        {
            static const uint16_t k[7] = {PPC_M_E_ADD2I_,  PPC_M_E_ADD2IS,
                                          PPC_M_E_CMP16I,  PPC_M_E_MULL2I,
                                          PPC_M_E_CMPL16I, PPC_M_E_CMPH16I,
                                          PPC_M_E_CMPHL16I};
            const bool uns = (xo == 0x15u || xo == 0x17u);

            set(d, k[xo - 0x11u]);
            d->ra = (uint8_t)F_RA(w);
            d->rd = d->ra;
            d->ui = uns ? ui16_a : sext(ui16_a, 16);
            d->imm = d->ui;
            if (xo == 0x11u) {
                d->rc = 1u;
            } else if (xo == 0x12u) {
                d->imm = d->ui << 16;
            }
            return;
        }
        case 0x18u: /* e_or2i rD, ui16 */
        case 0x19u: /* e_and2i. rD, ui16 */
        case 0x1Au: /* e_or2is rD, ui16 */
        case 0x1Cu: /* e_lis rD, ui16 */
        case 0x1Du: /* e_and2is. rD, ui16 */
        {
            static const uint16_t k[6] = {PPC_M_E_OR2I, PPC_M_E_AND2I_, PPC_M_E_OR2IS,
                                          0u, PPC_M_E_LIS, PPC_M_E_AND2IS_};

            set(d, k[xo - 0x18u]);
            d->rd = (uint8_t)F_RD(w);
            d->ra = d->rd; /* rD is both source and destination */
            d->ui = ui16_l;
            d->imm = (xo == 0x1Au || xo == 0x1Cu || xo == 0x1Du) ? ui16_l << 16 : ui16_l;
            d->rc = (xo == 0x19u || xo == 0x1Du) ? 1u : 0u;
            return;
        }
        default:
            return;
        }
    }
}

/* Opcode 31 in a VLE page: the forms only VLE has. */
static bool d_vle_31(uint32_t w, I *d)
{
    const uint32_t xo = F_XO10(w);
    const uint32_t rc = w & 1u;

    switch (xo) {
    case 0x00Eu: /* e_cmph  crD, rA, rB */
    case 0x02Eu: /* e_cmphl crD, rA, rB */
        if ((w & 0x00600001u) != 0u) {
            return true;
        }
        set(d, (xo == 0x00Eu) ? PPC_M_E_CMPH : PPC_M_E_CMPHL);
        d->crf = (uint8_t)((w >> 23) & 7u);
        d->ra = (uint8_t)F_RA(w);
        d->rb = (uint8_t)F_RB(w);
        return true;
    case 0x010u: /* e_mcrf crD, crS */
        if ((w & 0x0063F801u) != 0u) {
            return true;
        }
        set(d, PPC_M_E_MCRF);
        d->crf = (uint8_t)((w >> 23) & 7u);
        d->crs = (uint8_t)((w >> 18) & 7u);
        return true;
    case 0x021u:
    case 0x081u:
    case 0x0C1u:
    case 0x0E1u:
    case 0x101u:
    case 0x121u:
    case 0x1A1u:
    case 0x1C1u: {
        static const struct {
            uint16_t xo, id;
        } k[8] = {{0x021u, PPC_M_E_CRNOR},  {0x081u, PPC_M_E_CRANDC},
                  {0x0C1u, PPC_M_E_CRXOR},  {0x0E1u, PPC_M_E_CRNAND},
                  {0x101u, PPC_M_E_CRAND},  {0x121u, PPC_M_E_CREQV},
                  {0x1A1u, PPC_M_E_CRORC},  {0x1C1u, PPC_M_E_CROR}};

        if (rc != 0u) {
            return true;
        }
        for (uint32_t i = 0u; i < 8u; i++) {
            if (k[i].xo == xo) {
                set(d, k[i].id);
            }
        }
        d->rd = (uint8_t)F_RD(w);
        d->ra = (uint8_t)F_RA(w);
        d->rb = (uint8_t)F_RB(w);
        return true;
    }
    case 0x118u: /* e_rlw[.] rA, rS, rB */
        set(d, rc ? PPC_M_E_RLW_ : PPC_M_E_RLW);
        d->rd = (uint8_t)F_RD(w);
        d->ra = (uint8_t)F_RA(w);
        d->rb = (uint8_t)F_RB(w);
        d->mb = 0u;
        d->me = 31u;
        d->rc = (uint8_t)rc;
        return true;
    case 0x138u: /* e_rlwi[.] rA, rS, SH */
    case 0x038u: /* e_slwi[.] */
    case 0x238u: /* e_srwi[.] */
    {
        const uint32_t n = F_RB(w);

        d->rd = (uint8_t)F_RD(w);
        d->ra = (uint8_t)F_RA(w);
        d->rc = (uint8_t)rc;
        d->ui = n;
        if (xo == 0x138u) {
            set(d, rc ? PPC_M_E_RLWI_ : PPC_M_E_RLWI);
            d->sh = (uint8_t)n;
            d->mb = 0u;
            d->me = 31u;
        } else if (xo == 0x038u) {
            set(d, rc ? PPC_M_E_SLWI_ : PPC_M_E_SLWI);
            d->sh = (uint8_t)n;
            d->mb = 0u;
            d->me = (uint8_t)(31u - n);
        } else {
            set(d, rc ? PPC_M_E_SRWI_ : PPC_M_E_SRWI);
            d->sh = (uint8_t)((32u - n) & 31u);
            d->mb = (uint8_t)n;
            d->me = 31u;
        }
        return true;
    }
    case 0x024u: /* e_sc LEV: LEV in 16:20, 6:15 and 31 reserved */
        if ((w & 0x03FF0001u) != 0u) {
            return true;
        }
        set(d, PPC_M_E_SC);
        d->imm = d->ui = (w >> 11) & 31u;
        return true;
    default:
        return false;
    }
}

static void d_vle32(uint32_t w, I *d)
{
    const uint32_t op = w >> 26;

    switch (op) {
    case 0x04u:
        break; /* the EFPU and SPE, below */
    case 0x06u:
        /*
         * D8 and SCI8 share the opcode: D8's eight-bit XO starts 000x,
         * SCI8's four-bit one starts 1. The volatile save and restore are
         * D8 at XO 0x10 and 0x11, which a test for 0000 alone misses.
         */
        if (((w >> 12) & 0xFu) <= 1u) {
            d_vle_d8(w, d);
        } else if (((w >> 15) & 1u) != 0u) {
            d_vle_sci8(w, d);
        }
        return;
    case 0x07u: /* e_add16i rD, rA, SI */
        set(d, PPC_M_E_ADD16I);
        d->rd = (uint8_t)F_RD(w);
        d->ra = (uint8_t)F_RA(w);
        d->ra0 = 1u;
        d->imm = d->ui = sext(w & 0xFFFFu, 16);
        return;
    case 0x0Cu:
    case 0x0Du:
    case 0x0Eu:
    case 0x14u:
    case 0x15u:
    case 0x16u:
    case 0x17u: { /* the D-form loads and stores */
        static const struct {
            uint16_t id;
            uint8_t size, sext;
        } k[24] = {
            [0x0C] = {PPC_M_E_LBZ, 1u, 0u}, [0x0D] = {PPC_M_E_STB, 1u, 0u},
            [0x0E] = {PPC_M_E_LHA, 2u, 1u}, [0x14] = {PPC_M_E_LWZ, 4u, 0u},
            [0x15] = {PPC_M_E_STW, 4u, 0u}, [0x16] = {PPC_M_E_LHZ, 2u, 0u},
            [0x17] = {PPC_M_E_STH, 2u, 0u},
        };

        set(d, k[op].id);
        d->rd = (uint8_t)F_RD(w);
        d->ra = (uint8_t)F_RA(w);
        d->ra0 = 1u;
        d->size = k[op].size;
        d->sext = k[op].sext;
        d->imm = sext(w & 0xFFFFu, 16);
        return;
    }
    case 0x1Cu:
        d_vle_28(w, d);
        return;
    case 0x1Du: { /* e_rlwimi / e_rlwinm: bit 31 selects, it is not Rc */
        set(d, (w & 1u) ? PPC_M_E_RLWINM : PPC_M_E_RLWIMI);
        d->rd = (uint8_t)F_RD(w);
        d->ra = (uint8_t)F_RA(w);
        d->sh = (uint8_t)F_RB(w);
        d->mb = (uint8_t)F_MB(w);
        d->me = (uint8_t)F_ME(w);
        return;
    }
    case 0x1Eu:
        if ((w & 0x02000000u) == 0u) { /* e_b / e_bl: BD24 */
            set(d, (w & 1u) ? PPC_M_E_BL : PPC_M_E_B);
            d->lk = (uint8_t)(w & 1u);
            d->imm = sext(w & 0x01FFFFFEu, 25);
            return;
        }
        if ((w & 0x01C00000u) != 0u) { /* e_bc: bits 7:9 are zero */
            return;
        }
        {
            /* BO32: false, true, decrement-not-zero, decrement-zero. */
            static const uint8_t k_bo[4] = {4u, 12u, 16u, 18u};

            set(d, (w & 1u) ? PPC_M_E_BCL : PPC_M_E_BC);
            d->lk = (uint8_t)(w & 1u);
            d->bo = k_bo[(w >> 20) & 3u];
            d->bi = (uint8_t)((w >> 16) & 15u);
            d->ui = (w >> 20) & 3u; /* BO32 as written */
            d->imm = sext(w & 0xFFFEu, 16);
        }
        return;
    case 0x1Fu:
        if (d_vle_31(w, d)) {
            return;
        }
        break; /* the shared X-form pool */
    default:
        return;
    }
}

/* ------------------------------------------------------------------ */
/* Book E: primary opcodes other than 31 (non-VLE pages)               */
/* ------------------------------------------------------------------ */

static void d_booke_19(uint32_t w, I *d)
{
    const uint32_t xo = F_XO10(w);
    const uint32_t lk = w & 1u;

    switch (xo) {
    case 0x000u: /* mcrf */
        if ((w & 0x0063F801u) != 0u) {
            return;
        }
        set(d, PPC_M_MCRF);
        d->crf = (uint8_t)((w >> 23) & 7u);
        d->crs = (uint8_t)((w >> 18) & 7u);
        return;
    case 0x010u: /* bclr */
    case 0x210u: /* bcctr */
        if ((w & 0x0000F800u) != 0u) { /* BH is not on this core */
            return;
        }
        set(d, (xo == 0x010u) ? PPC_M_BCLR : PPC_M_BCCTR);
        d->bo = (uint8_t)F_RD(w);
        d->bi = (uint8_t)F_RA(w);
        d->lk = (uint8_t)lk;
        return;
    case 0x021u:
    case 0x081u:
    case 0x0C1u:
    case 0x0E1u:
    case 0x101u:
    case 0x121u:
    case 0x1A1u:
    case 0x1C1u: {
        static const struct {
            uint16_t xo, id;
        } k[8] = {{0x021u, PPC_M_CRNOR},  {0x081u, PPC_M_CRANDC},
                  {0x0C1u, PPC_M_CRXOR},  {0x0E1u, PPC_M_CRNAND},
                  {0x101u, PPC_M_CRAND},  {0x121u, PPC_M_CREQV},
                  {0x1A1u, PPC_M_CRORC},  {0x1C1u, PPC_M_CROR}};

        if (lk != 0u) {
            return;
        }
        for (uint32_t i = 0u; i < 8u; i++) {
            if (k[i].xo == xo) {
                set(d, k[i].id);
            }
        }
        d->rd = (uint8_t)F_RD(w);
        d->ra = (uint8_t)F_RA(w);
        d->rb = (uint8_t)F_RB(w);
        return;
    }
    case 0x032u:
    case 0x033u:
    case 0x027u:
    case 0x026u:
    case 0x096u: {
        static const struct {
            uint16_t xo, id;
        } k[5] = {{0x032u, PPC_M_RFI},   {0x033u, PPC_M_RFCI}, {0x027u, PPC_M_RFDI},
                  {0x026u, PPC_M_RFMCI}, {0x096u, PPC_M_ISYNC}};

        if ((w & 0x03FFF801u) != 0u) {
            return;
        }
        for (uint32_t i = 0u; i < 5u; i++) {
            if (k[i].xo == xo) {
                set(d, k[i].id);
            }
        }
        return;
    }
    default:
        return;
    }
}

static void d_booke(uint32_t w, I *d)
{
    const uint32_t op = w >> 26;
    const uint32_t rd = F_RD(w);
    const uint32_t ra = F_RA(w);
    const uint32_t ui = w & 0xFFFFu;
    const uint32_t si = sext(ui, 16);

    d->rd = (uint8_t)rd;
    d->ra = (uint8_t)ra;
    switch (op) {
    case 3u: /* twi TO, rA, SI */
        set(d, PPC_M_TWI);
        d->imm = d->ui = si;
        return;
    case 7u:
        set(d, PPC_M_MULLI);
        d->imm = d->ui = si;
        return;
    case 8u:
        set(d, PPC_M_SUBFIC);
        d->imm = d->ui = si;
        return;
    case 10u:
    case 11u: /* cmpli / cmpi: L (bit 10) must be 0 on a 32-bit core */
        if ((w & 0x00600000u) != 0u) {
            return;
        }
        set(d, (op == 10u) ? PPC_M_CMPLI : PPC_M_CMPI);
        d->crf = (uint8_t)((w >> 23) & 7u);
        d->imm = d->ui = (op == 10u) ? ui : si;
        return;
    case 12u:
    case 13u:
        set(d, (op == 13u) ? PPC_M_ADDIC_ : PPC_M_ADDIC);
        d->rc = (uint8_t)(op == 13u);
        d->imm = d->ui = si;
        return;
    case 14u:
        set(d, PPC_M_ADDI);
        d->ra0 = 1u;
        d->imm = d->ui = si;
        return;
    case 15u:
        set(d, PPC_M_ADDIS);
        d->ra0 = 1u;
        d->ui = si;
        d->imm = ui << 16;
        return;
    case 16u: /* bc BO, BI, BD */
        set(d, PPC_M_BC);
        d->bo = (uint8_t)rd;
        d->bi = (uint8_t)ra;
        d->aa = (uint8_t)((w >> 1) & 1u);
        d->lk = (uint8_t)(w & 1u);
        d->imm = sext(w & 0xFFFCu, 16);
        return;
    case 17u: /* sc: no LEV on this core; all but bit 30 reserved */
        if ((w & 0x03FFFFFDu) != 0u || (w & 2u) == 0u) {
            return;
        }
        set(d, PPC_M_SC);
        return;
    case 18u: /* b LI */
        set(d, PPC_M_B);
        d->aa = (uint8_t)((w >> 1) & 1u);
        d->lk = (uint8_t)(w & 1u);
        d->imm = sext(w & 0x03FFFFFCu, 26);
        return;
    case 19u:
        d_booke_19(w, d);
        return;
    case 20u:
    case 21u:
    case 23u:
        set(d, (op == 20u) ? PPC_M_RLWIMI : (op == 21u) ? PPC_M_RLWINM : PPC_M_RLWNM);
        d->sh = (uint8_t)F_RB(w);
        d->rb = (uint8_t)F_RB(w);
        d->mb = (uint8_t)F_MB(w);
        d->me = (uint8_t)F_ME(w);
        d->rc = (uint8_t)(w & 1u);
        return;
    case 24u:
    case 25u:
    case 26u:
    case 27u:
    case 28u:
    case 29u: { /* ori, oris, xori, xoris, andi., andis.: rA = rS op ui */
        static const uint16_t k[6] = {PPC_M_ORI,  PPC_M_ORIS,  PPC_M_XORI,
                                      PPC_M_XORIS, PPC_M_ANDI_, PPC_M_ANDIS_};

        set(d, k[op - 24u]);
        d->ui = ui;
        d->imm = (op & 1u) ? (ui << 16) : ui;
        d->rc = (op >= 28u) ? 1u : 0u;
        return;
    }
    case 32u:
    case 33u:
    case 34u:
    case 35u:
    case 36u:
    case 37u:
    case 38u:
    case 39u:
    case 40u:
    case 41u:
    case 42u:
    case 43u:
    case 44u:
    case 45u: { /* the D-form loads and stores, with and without update */
        static const struct {
            uint16_t id;
            uint8_t size, sext;
        } k[14] = {
            {PPC_M_LWZ, 4u, 0u}, {PPC_M_LWZU, 4u, 0u}, {PPC_M_LBZ, 1u, 0u},
            {PPC_M_LBZU, 1u, 0u}, {PPC_M_STW, 4u, 0u}, {PPC_M_STWU, 4u, 0u},
            {PPC_M_STB, 1u, 0u}, {PPC_M_STBU, 1u, 0u}, {PPC_M_LHZ, 2u, 0u},
            {PPC_M_LHZU, 2u, 0u}, {PPC_M_LHA, 2u, 1u}, {PPC_M_LHAU, 2u, 1u},
            {PPC_M_STH, 2u, 0u}, {PPC_M_STHU, 2u, 0u},
        };
        const uint32_t i = op - 32u;

        set(d, k[i].id);
        d->size = k[i].size;
        d->sext = k[i].sext;
        d->upd = (uint8_t)(op & 1u);
        d->ra0 = (uint8_t)!(op & 1u);
        d->imm = si;
        return;
    }
    case 46u:
    case 47u:
        set(d, (op == 46u) ? PPC_M_LMW : PPC_M_STMW);
        d->ra0 = 1u;
        d->imm = si;
        return;
    default:
        return;
    }
}

/* ------------------------------------------------------------------ */
/* Opcode 31: both kinds of page                                       */
/* ------------------------------------------------------------------ */

/*
 * One entry per X/XO-form instruction. `zero` is the instruction bits
 * that must be clear -- the reserved fields, checked as the core does
 * -- and `oe` says the form has an OE bit at 21, so that its XO is the
 * low nine bits and OE is not part of the match.
 */
typedef struct {
    uint16_t xo;
    uint16_t id;
    uint32_t zero;
    uint8_t oe;
    uint8_t rc; /* has an Rc bit; otherwise bit 31 is reserved */
} x_form_t;

#define Z_RA 0x001F0000u
#define Z_RB 0x0000F800u
#define Z_RD 0x03E00000u
#define Z_B31 0x00000001u

static const x_form_t k_x31[] = {
    {0x000u, PPC_M_CMP, 0x00600001u, 0u, 0u},
    {0x020u, PPC_M_CMPL, 0x00600001u, 0u, 0u},
    {0x004u, PPC_M_TW, Z_B31, 0u, 0u},
    {0x008u, PPC_M_SUBFC, 0u, 1u, 1u},
    {0x00Au, PPC_M_ADDC, 0u, 1u, 1u},
    {0x00Bu, PPC_M_MULHWU, 0x00000400u, 0u, 1u},
    {0x013u, PPC_M_MFCR, Z_RA | Z_RB | 0x00100000u | Z_B31, 0u, 0u},
    /* X-form integer loads: bit 31 is ignored, not checked (3.16.4). */
    {0x014u, PPC_M_LWARX, 0u, 0u, 0u},
    {0x034u, PPC_M_LBARX, 0u, 0u, 0u},
    {0x074u, PPC_M_LHARX, 0u, 0u, 0u},
    {0x017u, PPC_M_LWZX, 0u, 0u, 0u},
    {0x018u, PPC_M_SLW, 0u, 0u, 1u},
    {0x01Au, PPC_M_CNTLZW, Z_RB, 0u, 1u},
    {0x01Cu, PPC_M_AND, 0u, 0u, 1u},
    {0x028u, PPC_M_SUBF, 0u, 1u, 1u},
    {0x037u, PPC_M_LWZUX, 0u, 0u, 0u},
    {0x03Cu, PPC_M_ANDC, 0u, 0u, 1u},
    {0x04Bu, PPC_M_MULHW, 0x00000400u, 0u, 1u},
    {0x053u, PPC_M_MFMSR, Z_RA | Z_RB | Z_B31, 0u, 0u},
    {0x057u, PPC_M_LBZX, 0u, 0u, 0u},
    {0x068u, PPC_M_NEG, Z_RB, 1u, 1u},
    {0x077u, PPC_M_LBZUX, 0u, 0u, 0u},
    {0x07Cu, PPC_M_NOR, 0u, 0u, 1u},
    {0x083u, PPC_M_WRTEE, Z_RA | Z_RB | Z_B31, 0u, 0u},
    {0x088u, PPC_M_SUBFE, 0u, 1u, 1u},
    {0x08Au, PPC_M_ADDE, 0u, 1u, 1u},
    {0x090u, PPC_M_MTCRF, 0x00100801u, 0u, 0u},
    {0x092u, PPC_M_MTMSR, Z_RA | Z_RB | Z_B31, 0u, 0u},
    {0x096u, PPC_M_STWCX_, 0u, 0u, 0u},
    {0x2B6u, PPC_M_STBCX_, 0u, 0u, 0u},
    {0x2D6u, PPC_M_STHCX_, 0u, 0u, 0u},
    {0x097u, PPC_M_STWX, 0u, 0u, 0u},
    {0x0A3u, PPC_M_WRTEEI, Z_RD | Z_RA | 0x00007801u, 0u, 0u},
    {0x0B7u, PPC_M_STWUX, 0u, 0u, 0u},
    {0x0C8u, PPC_M_SUBFZE, Z_RB, 1u, 1u},
    {0x0CAu, PPC_M_ADDZE, Z_RB, 1u, 1u},
    {0x0D7u, PPC_M_STBX, 0u, 0u, 0u},
    {0x0E8u, PPC_M_SUBFME, Z_RB, 1u, 1u},
    {0x0EAu, PPC_M_ADDME, Z_RB, 1u, 1u},
    {0x0EBu, PPC_M_MULLW, 0u, 1u, 1u},
    {0x0F7u, PPC_M_STBUX, 0u, 0u, 0u},
    {0x10Au, PPC_M_ADD, 0u, 1u, 1u},
    {0x117u, PPC_M_LHZX, 0u, 0u, 0u},
    {0x11Cu, PPC_M_EQV, 0u, 0u, 1u},
    {0x137u, PPC_M_LHZUX, 0u, 0u, 0u},
    {0x13Cu, PPC_M_XOR, 0u, 0u, 1u},
    {0x153u, PPC_M_MFSPR, Z_B31, 0u, 0u},
    {0x157u, PPC_M_LHAX, 0u, 0u, 0u},
    {0x177u, PPC_M_LHAUX, 0u, 0u, 0u},
    {0x197u, PPC_M_STHX, 0u, 0u, 0u},
    {0x19Cu, PPC_M_ORC, 0u, 0u, 1u},
    {0x1B7u, PPC_M_STHUX, 0u, 0u, 0u},
    {0x1BCu, PPC_M_OR, 0u, 0u, 1u},
    {0x1CBu, PPC_M_DIVWU, 0u, 1u, 1u},
    {0x1D3u, PPC_M_MTSPR, Z_B31, 0u, 0u},
    {0x1DCu, PPC_M_NAND, 0u, 0u, 1u},
    {0x1EBu, PPC_M_DIVW, 0u, 1u, 1u},
    {0x200u, PPC_M_MCRXR, 0x007FF801u, 0u, 0u},
    {0x216u, PPC_M_LWBRX, 0u, 0u, 0u},
    {0x218u, PPC_M_SRW, 0u, 0u, 1u},
    {0x316u, PPC_M_LHBRX, 0u, 0u, 0u},
    {0x318u, PPC_M_SRAW, 0u, 0u, 1u},
    {0x338u, PPC_M_SRAWI, 0u, 0u, 1u},
    {0x356u, PPC_M_MBAR, Z_RA | Z_RB | Z_B31, 0u, 0u},
    {0x396u, PPC_M_STHBRX, 0u, 0u, 0u},
    {0x39Au, PPC_M_EXTSH, Z_RB, 0u, 1u},
    {0x3BAu, PPC_M_EXTSB, Z_RB, 0u, 1u},
    {0x296u, PPC_M_STWBRX, 0u, 0u, 0u},
    {0x256u, PPC_M_MSYNC, Z_RD | Z_RA | Z_RB | Z_B31, 0u, 0u},
    {0x03Eu, PPC_M_WAIT, Z_RD | Z_RA | Z_RB | Z_B31, 0u, 0u},
    {0x056u, PPC_M_DCBF, Z_RD | Z_B31, 0u, 0u},
    {0x036u, PPC_M_DCBST, Z_RD | Z_B31, 0u, 0u},
    {0x116u, PPC_M_DCBT, Z_B31, 0u, 0u},
    {0x0F6u, PPC_M_DCBTST, Z_B31, 0u, 0u},
    {0x3F6u, PPC_M_DCBZ, Z_RD | Z_B31, 0u, 0u},
    {0x1D6u, PPC_M_DCBI, Z_RD | Z_B31, 0u, 0u},
    {0x3D6u, PPC_M_ICBI, Z_RD | Z_B31, 0u, 0u},
    {0x2F6u, PPC_M_DCBA, Z_RD | Z_B31, 0u, 0u},
    {0x016u, PPC_M_ICBT, Z_B31, 0u, 0u},
};

static void d_31(uint32_t w, I *d)
{
    const uint32_t xo10 = F_XO10(w);
    const uint32_t xo9 = xo10 & 0x1FFu;

    /* isel: XO is five bits, at 26:30, and BC in 21:25 is an operand. */
    if (((w >> 1) & 31u) == 0x0Fu) {
        set(d, PPC_M_ISEL);
        d->rd = (uint8_t)F_RD(w);
        d->ra = (uint8_t)F_RA(w);
        d->rb = (uint8_t)F_RB(w);
        d->bi = (uint8_t)((w >> 6) & 31u);
        d->ra0 = 1u;
        return;
    }
    for (uint32_t i = 0u; i < sizeof(k_x31) / sizeof(k_x31[0]); i++) {
        const x_form_t *const e = &k_x31[i];
        const uint32_t key = e->oe ? xo9 : xo10;

        if (e->xo != key) {
            continue;
        }
        if ((w & e->zero) != 0u) {
            return;
        }
        set(d, e->id);
        d->rd = (uint8_t)F_RD(w);
        d->ra = (uint8_t)F_RA(w);
        d->rb = (uint8_t)F_RB(w);
        d->rc = e->rc ? (uint8_t)(w & 1u) : 0u;
        d->oe = e->oe ? (uint8_t)((w >> 10) & 1u) : 0u;
        break;
    }

    switch (d->sem) {
    case PPC_S_LOAD:
    case PPC_S_STORE: {
        /* XO bits: the size, update and byte-reversal all come from it. */
        const uint32_t id = d->id;

        d->idx = 1u;
        d->ra0 = 1u;
        d->size = (id == PPC_M_LWZX || id == PPC_M_LWZUX || id == PPC_M_STWX ||
                   id == PPC_M_STWUX || id == PPC_M_LWBRX || id == PPC_M_STWBRX)
                      ? 4u
                  : (id == PPC_M_LBZX || id == PPC_M_LBZUX || id == PPC_M_STBX ||
                     id == PPC_M_STBUX)
                      ? 1u
                      : 2u;
        d->sext = (id == PPC_M_LHAX || id == PPC_M_LHAUX) ? 1u : 0u;
        d->rev = (id == PPC_M_LWBRX || id == PPC_M_LHBRX || id == PPC_M_STWBRX ||
                  id == PPC_M_STHBRX)
                     ? 1u
                     : 0u;
        if (id == PPC_M_LWZUX || id == PPC_M_LBZUX || id == PPC_M_LHZUX ||
            id == PPC_M_LHAUX || id == PPC_M_STWUX || id == PPC_M_STBUX ||
            id == PPC_M_STHUX) {
            d->upd = 1u;
            d->ra0 = 0u;
        }
        return;
    }
    case PPC_S_LARX:
    case PPC_S_STCX:
        d->idx = 1u;
        d->ra0 = 1u;
        d->size = (d->id == PPC_M_LBARX || d->id == PPC_M_STBCX_)  ? 1u
                  : (d->id == PPC_M_LHARX || d->id == PPC_M_STHCX_) ? 2u
                                                                     : 4u;
        d->rc = (d->sem == PPC_S_STCX) ? 1u : 0u;
        if (d->sem == PPC_S_STCX && (w & 1u) == 0u) {
            set(d, PPC_M_UNKNOWN); /* the record bit is the instruction */
        }
        return;
    case PPC_S_CMP:
    case PPC_S_CMPL:
        d->crf = (uint8_t)((w >> 23) & 7u);
        return;
    case PPC_S_MFSPR:
    case PPC_S_MTSPR:
        /* The ten-bit SPR field is split and its halves swapped. */
        d->imm = d->ui = F_RA(w) | (F_RB(w) << 5);
        return;
    case PPC_S_MTCRF:
        d->imm = d->ui = (w >> 12) & 0xFFu;
        return;
    case PPC_S_MCRXR:
        d->crf = (uint8_t)((w >> 23) & 7u);
        return;
    case PPC_S_WRTEE:
        if (d->id == PPC_M_WRTEEI) {
            d->imm = d->ui = (w >> 15) & 1u;
        }
        return;
    case PPC_S_SRAWI:
        d->sh = (uint8_t)F_RB(w);
        d->ui = d->sh;
        return;
    case PPC_S_TW:
        d->ui = d->rd; /* TO */
        return;
    case PPC_S_NOP:
        if (d->id == PPC_M_MBAR) {
            /* MO 0, 1 and 2 only: anything else is illegal here. */
            d->imm = d->ui = F_RD(w);
            if (d->imm > 2u) {
                set(d, PPC_M_UNKNOWN);
            }
        } else if (d->id == PPC_M_DCBT || d->id == PPC_M_DCBTST || d->id == PPC_M_ICBT) {
            d->ui = d->rd; /* CT */
        }
        return;
    case PPC_S_DCBZ:
        d->ra0 = 1u;
        return;
    default:
        return;
    }
}

/* ------------------------------------------------------------------ */
/* Opcode 4: the EFPU, and SPE                                         */
/* ------------------------------------------------------------------ */

/*
 * Extended opcodes that are SPE or EFPU vector instructions: objdump's
 * SPE and EFS2 tables, swept, plus evfssqrt (0x287), which is in the
 * e200z759n3 manual and not in objdump.
 */
static const uint32_t k_spe_xo[64] = {
    0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    0x4BC6FF55u, 0x001FFF5Fu, 0x00000000u, 0xFF000000u,
    0x75FF7FFFu, 0x0000DFFFu, 0x00000000u, 0x00000000u,
    0x33F3F33Fu, 0x3333003Fu, 0x00000000u, 0x00000000u,
    0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    0x0000BB88u, 0x0000BB88u, 0x0B08B988u, 0x0B08B988u,
    0x00000000u, 0x00000000u, 0x00000FDFu, 0x00000000u,
    0x0000BBBBu, 0x0000BB00u, 0x0B08BBBBu, 0x000080B0u,
    0x0000BBBBu, 0x0000BB00u, 0x0B08BBBBu, 0x000080B0u,
    0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
};

static void d_4(uint32_t w, I *d)
{
    /* The scalar single-precision group, 0x2B0..0x2DE. */
    static const struct {
        uint16_t xo, id;
        uint8_t fmt; /* 0 D_A_B, 1 D_A, 2 D_B, 3 CRF_A_B, 4 D_B with rA = 4 */
    } k[] = {
        {0x2B0u, PPC_M_EFSMAX, 0u},   {0x2B1u, PPC_M_EFSMIN, 0u},
        {0x2C0u, PPC_M_EFSADD, 0u},   {0x2C1u, PPC_M_EFSSUB, 0u},
        {0x2C2u, PPC_M_EFSMADD, 0u},  {0x2C3u, PPC_M_EFSMSUB, 0u},
        {0x2C4u, PPC_M_EFSABS, 1u},   {0x2C5u, PPC_M_EFSNABS, 1u},
        {0x2C6u, PPC_M_EFSNEG, 1u},   {0x2C7u, PPC_M_EFSSQRT, 1u},
        {0x2C8u, PPC_M_EFSMUL, 0u},   {0x2C9u, PPC_M_EFSDIV, 0u},
        {0x2CAu, PPC_M_EFSNMADD, 0u}, {0x2CBu, PPC_M_EFSNMSUB, 0u},
        {0x2CCu, PPC_M_EFSCMPGT, 3u}, {0x2CDu, PPC_M_EFSCMPLT, 3u},
        {0x2CEu, PPC_M_EFSCMPEQ, 3u}, {0x2D0u, PPC_M_EFSCFUI, 2u},
        {0x2D1u, PPC_M_EFSCFSI, 2u},  {0x2D2u, PPC_M_EFSCFUF, 2u},
        {0x2D3u, PPC_M_EFSCFSF, 2u},  {0x2D4u, PPC_M_EFSCTUI, 2u},
        {0x2D5u, PPC_M_EFSCTSI, 2u},  {0x2D6u, PPC_M_EFSCTUF, 2u},
        {0x2D7u, PPC_M_EFSCTSF, 2u},  {0x2D8u, PPC_M_EFSCTUIZ, 2u},
        {0x2DAu, PPC_M_EFSCTSIZ, 2u}, {0x2DCu, PPC_M_EFSTSTGT, 3u},
        {0x2DDu, PPC_M_EFSTSTLT, 3u}, {0x2DEu, PPC_M_EFSTSTEQ, 3u},
    };
    const uint32_t xo = w & 0x7FFu;

    d->rd = (uint8_t)F_RD(w);
    d->ra = (uint8_t)F_RA(w);
    d->rb = (uint8_t)F_RB(w);
    for (uint32_t i = 0u; i < sizeof(k) / sizeof(k[0]); i++) {
        if (k[i].xo != xo) {
            continue;
        }
        switch (k[i].fmt) {
        case 1u:
            if ((w & Z_RB) != 0u) {
                return;
            }
            break;
        case 2u:
            /*
             * The conversions with half precision hide in two of these
             * slots, chosen by rA = 4: efscfh beside efscfsi and efscth
             * beside efsctsi. Any other non-zero rA is reserved.
             */
            if (F_RA(w) == 4u && (xo == 0x2D1u || xo == 0x2D5u)) {
                set(d, (xo == 0x2D1u) ? PPC_M_EFSCFH : PPC_M_EFSCTH);
                return;
            }
            if ((w & Z_RA) != 0u) {
                return;
            }
            break;
        case 3u:
            if ((w & 0x00600000u) != 0u) {
                return;
            }
            d->crf = (uint8_t)((w >> 23) & 7u);
            break;
        default:
            break;
        }
        set(d, k[i].id);
        return;
    }
    if ((k_spe_xo[xo >> 5] & (1u << (xo & 31u))) != 0u) {
        set(d, PPC_M_SPE);
    }
}

/* ------------------------------------------------------------------ */
/* Entry                                                               */
/* ------------------------------------------------------------------ */

void ppc_decode(uint32_t insn, unsigned len, bool vle, ppc_insn_t *out)
{
    memset(out, 0, sizeof(*out));
    out->len = (uint8_t)len;
    set(out, PPC_M_UNKNOWN);

    if (len == 2u) {
        d16((uint16_t)(insn >> 16), out);
        return;
    }
    if (vle) {
        d_vle32(insn, out);
        if (out->id != PPC_M_UNKNOWN) {
            return;
        }
        if ((insn >> 26) != 31u && (insn >> 26) != 4u) {
            return; /* the Book E D-forms are not in a VLE page */
        }
    } else {
        d_booke(insn, out);
        if (out->id != PPC_M_UNKNOWN) {
            return;
        }
    }
    if ((insn >> 26) == 31u) {
        d_31(insn, out);
    } else if ((insn >> 26) == 4u) {
        d_4(insn, out);
    }
}
