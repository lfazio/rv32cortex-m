/* SPDX-License-Identifier: Apache-2.0 */
/*
 * ppc_interp.c - the e200z7 interpreter.
 *
 * VLE, so instructions are 16 or 32 bits and the length comes from the
 * first halfword -- see ppc_vle_len(), whose rule was derived from the
 * assembler rather than from a diagram.
 *
 * Everything not decoded raises a program interrupt, which is the
 * architecture's report for an unimplemented encoding. The G4MH lesson
 * applies in full and is worth repeating here, because it is easy to
 * believe otherwise: **a trap only reports if something catches it.** A
 * flat guest with IVPR and the IVORs still zero vectors to address 0,
 * which in a flat image is the guest's own entry point -- so an
 * unimplemented instruction presents as a restart, not as a diagnostic.
 * Read the pc deltas in a trace: an instruction that changes the pc
 * without retiring is a trap.
 */

#include "ppc/ppc_cpu.h"
#include "ppc/ppc_decode.h"

/* Raise `e` and restart the dispatch loop. */
#define EXC(e)                                                          \
    do {                                                                \
        c->pc = pc;                                                     \
        ppc_cpu_exception(c, (ppc_ivor_t)(e), pc);                      \
        pc = c->pc;                                                     \
        goto next_insn;                                                 \
    } while (0)

/* ------------------------------------------------------------------ */
/* Condition register                                                  */
/* ------------------------------------------------------------------ */

/*
 * CR fields are numbered from the left: CR0 is bits 0:3, the *top*
 * nibble. So field n sits at shift 4 * (7 - n), and writing that as
 * 4 * n -- which is what the name CR0 suggests to a reader used to
 * little-endian bit numbering -- puts CR0 where CR7 belongs and makes
 * every conditional branch read the wrong field.
 */
static EMU_ALWAYS_INLINE void cr_set(ppc_cpu_t *c, uint32_t field, uint32_t v)
{
    const unsigned sh = 4u * (7u - field);
    c->cr = (c->cr & ~(0xFu << sh)) | ((v & 0xFu) << sh);
}

static EMU_ALWAYS_INLINE uint32_t cr_get(const ppc_cpu_t *c, uint32_t field)
{
    return (c->cr >> (4u * (7u - field))) & 0xFu;
}

/* Compare and set a CR field, signed or unsigned, plus XER[SO]. */
static EMU_ALWAYS_INLINE void cr_compare(ppc_cpu_t *c, uint32_t field,
                                         uint32_t a, uint32_t b, bool sgn)
{
    uint32_t v;

    if (sgn) {
        v = ((int32_t)a < (int32_t)b) ? PPC_CR_LT
          : (((int32_t)a > (int32_t)b) ? PPC_CR_GT : PPC_CR_EQ);
    } else {
        v = (a < b) ? PPC_CR_LT : ((a > b) ? PPC_CR_GT : PPC_CR_EQ);
    }
    if ((c->xer & PPC_XER_SO) != 0u) {
        v |= PPC_CR_SO;
    }
    cr_set(c, field, v);
}

/* The Rc bit's effect: compare the result against zero into CR0. */
static EMU_ALWAYS_INLINE void cr0_from(ppc_cpu_t *c, uint32_t res)
{
    cr_compare(c, 0u, res, 0u, true);
}

/* ------------------------------------------------------------------ */
/* Run loop                                                            */
/* ------------------------------------------------------------------ */

static emu_run_reason_t ppc_run(emu_cpu_t *cpu, uint32_t budget,
                                uint32_t *retired)
{
    ppc_cpu_t *c = (ppc_cpu_t *)(void *)cpu;
    uint32_t pc = c->pc;
    uint32_t done = 0u;
    emu_run_reason_t reason = EMU_RUN_BUDGET;

    while (done < budget) {
        /*
         * Tested at the top, not only where a halt instruction is
         * decoded: the syscall hook can halt the core to implement
         * exit(), and a loop that only checks at the decode site runs
         * one more instruction and reports the wrong reason.
         */
        if (EMU_UNLIKELY(c->state == EMU_STATE_HALTED)) {
            reason = EMU_RUN_HALTED;
            break;
        }

        /* --- fetch ------------------------------------------------- */
        uint16_t w0;
        emu_fault_t f = emu_bus_fetch16(c->bus, pc, &w0);
        if (EMU_UNLIKELY(f != EMU_FAULT_NONE)) {
            EXC(PPC_IVOR_INST_STORAGE);
        }

        /*
         * Classic Book E is fixed 32-bit; only VLE has a length rule.
         * Applying the VLE rule to Book E desynchronises the stream on
         * the first branch, because `bl` has top4 = 0x4 and would read
         * as a 16-bit parcel.
         */
        const unsigned len = c->vle ? ppc_vle_len(w0) : 4u;
        uint32_t insn = (uint32_t)w0 << 16;
        if (len == 4u) {
            uint16_t w1;
            f = emu_bus_fetch16(c->bus, pc + 2u, &w1);
            if (EMU_UNLIKELY(f != EMU_FAULT_NONE)) {
                EXC(PPC_IVOR_INST_STORAGE);
            }
            insn |= w1;
        }

        const uint32_t next = pc + len;

#if EMU_ENABLE_TRACE
        if (c->trace != NULL) {
            c->trace((emu_cpu_t *)c, pc, insn, c->trace_user);
        }
#endif

        /* --- execute ----------------------------------------------- */
        if (len == 2u) {
            /*
             * VLE's 16-bit se_ forms.
             *
             * Every encoding below came from the assembler, and the
             * grouping follows what the encodings actually do rather
             * than the manual's form names: the operand layout is not
             * uniform across them. In particular the two-register forms
             * put the *source* in bits[7:4] and the destination in
             * bits[3:0], while the load/store forms put the data
             * register in bits[7:4] and the *base* in bits[3:0] -- the
             * same two nibbles, opposite senses.
             */
            const uint16_t w = (uint16_t)(insn >> 16);
            const uint32_t rx = ppc_se_reg((uint32_t)w & 0xFu);
            const uint32_t ry = ppc_se_reg(((uint32_t)w >> 4) & 0xFu);
            const uint32_t hi = (uint32_t)w >> 8;

            if (hi == 0x00u) {
                /* No operands, or one register in the low nibble. */
                switch (((uint32_t)w >> 4) & 0xFu) {
                case 0x0u:
                    switch ((uint32_t)w & 0xFu) {
                    case 0x1u: break;               /* se_isync: a no-op  */
                    case 0x2u:                      /* se_sc              */
                        goto do_syscall;
                    case 0x4u:                      /* se_blr             */
                    case 0x5u:                      /* se_blrl            */
                        {
                            const uint32_t tgt = c->lr & ~1u;
                            if (((uint32_t)w & 1u) != 0u) { c->lr = next; }
                            pc = tgt;
                            goto retired_insn;
                        }
                    case 0x6u:                      /* se_bctr            */
                    case 0x7u:                      /* se_bctrl           */
                        {
                            const uint32_t tgt = c->ctr & ~1u;
                            if (((uint32_t)w & 1u) != 0u) { c->lr = next; }
                            pc = tgt;
                            goto retired_insn;
                        }
                    case 0x8u:                      /* se_rfi             */
                        c->msr = c->srr1;
                        pc = c->srr0;
                        goto retired_insn;
                    case 0x9u:                      /* se_rfci            */
                    case 0xAu:                      /* se_rfdi            */
                        c->msr = c->csrr1;
                        pc = c->csrr0;
                        goto retired_insn;
                    default:                        /* se_illegal, 0x0000 */
                        EXC(PPC_IVOR_PROGRAM);
                    }
                    break;
                case 0x2u: c->r[rx] = ~c->r[rx]; break;              /* not   */
                case 0x3u: c->r[rx] = (uint32_t)(-(int32_t)c->r[rx]); break;
                case 0x8u: c->r[rx] = c->lr;  break;                 /* mflr  */
                case 0x9u: c->lr    = c->r[rx]; break;               /* mtlr  */
                case 0xAu: c->r[rx] = c->ctr; break;                 /* mfctr */
                case 0xBu: c->ctr   = c->r[rx]; break;               /* mtctr */
                case 0xCu: c->r[rx] = c->r[rx] & 0xFFu; break;       /* extzb */
                case 0xDu: c->r[rx] = (uint32_t)(int32_t)(int8_t)c->r[rx]; break;
                case 0xEu: c->r[rx] = c->r[rx] & 0xFFFFu; break;     /* extzh */
                case 0xFu: c->r[rx] = (uint32_t)(int32_t)(int16_t)c->r[rx]; break;
                default:   EXC(PPC_IVOR_PROGRAM);
                }
            } else if (hi == 0x01u) {               /* se_mr  rX <- rY    */
                c->r[rx] = c->r[ry];
            } else if (hi >= 0x04u && hi <= 0x07u) {
                switch (hi) {
                case 0x04u: c->r[rx] += c->r[ry]; break;      /* se_add   */
                case 0x05u: c->r[rx] *= c->r[ry]; break;      /* se_mullw */
                /*
                 * se_sub and se_subf are not the same instruction with
                 * the operands swapped in the encoding -- they are two
                 * instructions with opposite senses, and both write rX.
                 */
                case 0x06u: c->r[rx] -= c->r[ry]; break;      /* rX - rY  */
                default:    c->r[rx] = c->r[ry] - c->r[rx]; break;
                }
            } else if (hi >= 0x0Cu && hi <= 0x0Eu) {
                /*
                 * The 16-bit compares always target CR0, and se_cmph
                 * compares only the low halfwords -- sign-extended, so
                 * it is not a masked se_cmp.
                 */
                if (hi == 0x0Cu) {
                    cr_compare(c, 0u, c->r[rx], c->r[ry], true);
                } else if (hi == 0x0Du) {
                    cr_compare(c, 0u, c->r[rx], c->r[ry], false);
                } else {
                    cr_compare(c, 0u,
                               (uint32_t)(int32_t)(int16_t)c->r[rx],
                               (uint32_t)(int32_t)(int16_t)c->r[ry], true);
                }
            } else if (hi >= 0x40u && hi <= 0x47u) {
                /* Shift counts are masked to 5 bits; PowerPC's 6-bit
                 * behaviour is a 64-bit rule and does not apply here. */
                const uint32_t sh = c->r[ry] & 0x1Fu;
                switch (hi) {
                case 0x40u: c->r[rx] >>= sh; break;                  /* srw  */
                case 0x41u: c->r[rx] =
                    (uint32_t)((int32_t)c->r[rx] >> sh); break;      /* sraw */
                case 0x42u: c->r[rx] <<= sh; break;                  /* slw  */
                case 0x44u: c->r[rx] |= c->r[ry]; break;             /* or   */
                case 0x45u: c->r[rx] &= ~c->r[ry]; break;            /* andc */
                case 0x46u: c->r[rx] &= c->r[ry]; break;             /* and  */
                case 0x47u: c->r[rx] &= c->r[ry];                    /* and. */
                            cr0_from(c, c->r[rx]); break;
                default:    EXC(PPC_IVOR_PROGRAM);
                }
            } else if ((w >> 11) == 0x09u) {        /* se_li  imm7        */
                c->r[rx] = ((uint32_t)w >> 4) & 0x7Fu;
            } else if ((w >> 9) == 0x10u || (w >> 9) == 0x12u) {
                /*
                 * OIM5 encodes 1..32 as 0..31, because adding zero is
                 * not worth an encoding. Reading it straight makes
                 * `se_addi rX,1` a no-op and `se_addi rX,32` add 31.
                 */
                const uint32_t oim = (((uint32_t)w >> 4) & 0x1Fu) + 1u;
                if ((w >> 9) == 0x10u) { c->r[rx] += oim; }
                else                   { c->r[rx] -= oim; }
            } else if ((w >> 9) == 0x15u) {         /* se_cmpi  ui5       */
                cr_compare(c, 0u, c->r[rx], ((uint32_t)w >> 4) & 0x1Fu, true);
            } else if ((w >> 9) == 0x16u) {         /* se_bmaski ui5      */
                const uint32_t n = ((uint32_t)w >> 4) & 0x1Fu;
                c->r[rx] = (n == 0u) ? 0xFFFFFFFFu : ((1u << n) - 1u);
            } else if ((w >> 9) == 0x30u) {         /* se_bclri           */
                c->r[rx] &= ~(1u << (31u - (((uint32_t)w >> 4) & 0x1Fu)));
            } else if ((w >> 9) == 0x32u) {         /* se_bseti           */
                c->r[rx] |= 1u << (31u - (((uint32_t)w >> 4) & 0x1Fu));
            } else if ((w >> 9) == 0x33u) {         /* se_btsti           */
                const uint32_t b =
                    c->r[rx] & (1u << (31u - (((uint32_t)w >> 4) & 0x1Fu)));
                cr_compare(c, 0u, b, 0u, true);
            } else if ((w >> 9) == 0x34u) {         /* se_srwi            */
                c->r[rx] >>= ((uint32_t)w >> 4) & 0x1Fu;
            } else if ((w >> 9) == 0x36u) {         /* se_slwi            */
                c->r[rx] <<= ((uint32_t)w >> 4) & 0x1Fu;
            } else if (hi >= 0x80u && hi <= 0xDFu) {
                /*
                 * SD4-form. The data register is bits[7:4] and the base
                 * is bits[3:0] -- the opposite sense to every
                 * two-register form above, which is the single easiest
                 * thing to get wrong here.
                 *
                 * The displacement is scaled by the access size, so the
                 * same nibble is 15 bytes or 60 bytes depending on the
                 * width.
                 */
                const uint32_t kind = (uint32_t)w >> 12;
                const uint32_t sz = (kind == 0x8u || kind == 0x9u) ? 1u
                                  : ((kind == 0xAu || kind == 0xBu) ? 2u : 4u);
                const uint32_t ea = c->r[rx] + ppc_se_sd4(w, sz);
                const bool store = (kind == 0x9u || kind == 0xBu ||
                                    kind == 0xDu);
                if (store) {
                    const ppc_exc_t e = ppc_store(c, ea, sz, c->r[ry]);
                    if (EMU_UNLIKELY(e != PPC_EXC_NONE)) { EXC(e); }
                } else {
                    uint32_t v;
                    const ppc_exc_t e = ppc_load(c, ea, sz, false, &v);
                    if (EMU_UNLIKELY(e != PPC_EXC_NONE)) { EXC(e); }
                    c->r[ry] = v;
                }
            } else if (hi >= 0xE0u && hi <= 0xE7u) {
                /*
                 * se_bc. The condition is CR0 only: bits[1:0] of the
                 * opcode pick LT/GT/EQ/SO and bit 2 picks true or false,
                 * so se_bge is "branch if not LT" rather than an
                 * encoding of its own.
                 */
                static const uint32_t k_bit[4] = {
                    PPC_CR_LT, PPC_CR_GT, PPC_CR_EQ, PPC_CR_SO
                };
                const bool want = (hi & 0x4u) != 0u;
                const bool got = (cr_get(c, 0u) & k_bit[hi & 0x3u]) != 0u;
                if (got == want) {
                    /* BD8 is signed and scaled by two: it counts
                     * halfwords, because no instruction is odd. */
                    const int32_t bd = (int32_t)(int8_t)(uint8_t)(w & 0xFFu);
                    pc = pc + (uint32_t)(bd * 2);
                    goto retired_insn;
                }
            } else if (hi == 0xE8u || hi == 0xE9u) {
                const int32_t bd = (int32_t)(int8_t)(uint8_t)(w & 0xFFu);
                if (hi == 0xE9u) { c->lr = next; }   /* se_bl */
                pc = pc + (uint32_t)(bd * 2);
                goto retired_insn;
            } else {
                EXC(PPC_IVOR_PROGRAM);
            }

            pc = next;
            goto retired_insn;
        }

        switch (ppc_op6(insn)) {
        case 0x0E: {                        /* addi / li  (D-form)      */
            /*
             * rA == 0 means the *literal* zero, not r0. That is
             * PowerPC's one pervasive irregularity and the reason `li`
             * is an extended mnemonic for `addi rD, 0, imm` rather than
             * a separate instruction. Reading r[0] here would make every
             * `li` return whatever r0 happened to hold.
             */
            const uint32_t a = (ppc_ra(insn) == 0u) ? 0u : c->r[ppc_ra(insn)];
            c->r[ppc_rd(insn)] = a + (uint32_t)ppc_d16(insn);
            break;
        }

        case 0x0F: {                        /* addis / lis              */
            const uint32_t a = (ppc_ra(insn) == 0u) ? 0u : c->r[ppc_ra(insn)];
            c->r[ppc_rd(insn)] = a + ((insn & 0xFFFFu) << 16);
            break;
        }

        case 0x20:                          /* lwz                      */
        case 0x22:                          /* lbz                      */
        case 0x28: {                        /* lhz                      */
            const uint32_t a = (ppc_ra(insn) == 0u) ? 0u : c->r[ppc_ra(insn)];
            const uint32_t ea = a + (uint32_t)ppc_d16(insn);
            const uint32_t sz = (ppc_op6(insn) == 0x20u) ? 4u
                              : ((ppc_op6(insn) == 0x22u) ? 1u : 2u);
            uint32_t v;
            const ppc_exc_t e = ppc_load(c, ea, sz, false, &v);
            if (EMU_UNLIKELY(e != PPC_EXC_NONE)) { EXC(e); }
            c->r[ppc_rd(insn)] = v;
            break;
        }

        case 0x24:                          /* stw                      */
        case 0x26:                          /* stb                      */
        case 0x2C: {                        /* sth                      */
            const uint32_t a = (ppc_ra(insn) == 0u) ? 0u : c->r[ppc_ra(insn)];
            const uint32_t ea = a + (uint32_t)ppc_d16(insn);
            const uint32_t sz = (ppc_op6(insn) == 0x24u) ? 4u
                              : ((ppc_op6(insn) == 0x26u) ? 1u : 2u);
            const ppc_exc_t e = ppc_store(c, ea, sz, c->r[ppc_rd(insn)]);
            if (EMU_UNLIKELY(e != PPC_EXC_NONE)) { EXC(e); }
            break;
        }

        case 0x18: {                        /* ori  (and thus nop)      */
            c->r[ppc_ra(insn)] = c->r[ppc_rd(insn)] | (insn & 0xFFFFu);
            break;
        }
        case 0x19: {                        /* oris                     */
            c->r[ppc_ra(insn)] = c->r[ppc_rd(insn)] | ((insn & 0xFFFFu) << 16);
            break;
        }
        case 0x1A: {                        /* xori                     */
            c->r[ppc_ra(insn)] = c->r[ppc_rd(insn)] ^ (insn & 0xFFFFu);
            break;
        }
        case 0x1C: {                        /* andi.  -- always sets CR0 */
            const uint32_t v = c->r[ppc_rd(insn)] & (insn & 0xFFFFu);
            c->r[ppc_ra(insn)] = v;
            cr0_from(c, v);
            break;
        }

        case 0x0B:                          /* cmpi                     */
            cr_compare(c, (insn >> 23) & 0x7u,
                       c->r[ppc_ra(insn)], (uint32_t)ppc_d16(insn), true);
            break;

        case 0x0A:                          /* cmpli                    */
            cr_compare(c, (insn >> 23) & 0x7u,
                       c->r[ppc_ra(insn)], insn & 0xFFFFu, false);
            break;

        case 0x12: {                        /* b / bl / ba / bla        */
            /*
             * LI is a signed 26-bit *byte* displacement whose low two
             * bits are architecturally zero, so the field is bits 6:29
             * shifted left by two. AA picks absolute or relative and LK
             * asks for the return address.
             */
            int32_t li = (int32_t)(insn & 0x03FFFFFCu);
            if ((li & 0x02000000) != 0) {
                li |= (int32_t)0xFC000000;  /* sign-extend from bit 25 */
            }
            const bool aa = (insn & 2u) != 0u;
            if ((insn & 1u) != 0u) {        /* LK */
                c->lr = next;
            }
            pc = aa ? (uint32_t)li : (pc + (uint32_t)li);
            goto retired_insn;
        }

        case 0x1F:                          /* the X-form pool          */
            switch (ppc_xo10(insn)) {
            case 0x10A: {                   /* add                      */
                const uint32_t v = c->r[ppc_ra(insn)] + c->r[ppc_rb(insn)];
                c->r[ppc_rd(insn)] = v;
                if (ppc_rc(insn)) { cr0_from(c, v); }
                break;
            }
            case 0x028: {                   /* subf                     */
                const uint32_t v = c->r[ppc_rb(insn)] - c->r[ppc_ra(insn)];
                c->r[ppc_rd(insn)] = v;
                if (ppc_rc(insn)) { cr0_from(c, v); }
                break;
            }
            /*
             * The logical group writes rA from rS, which is the reverse
             * of the arithmetic group's rD from rA. rD and rS are the
             * same field; only its meaning changes, and reading it as a
             * destination here would write the wrong register with the
             * right value.
             */
            case 0x1BC: {                   /* or  (and thus mr)        */
                const uint32_t v = c->r[ppc_rd(insn)] | c->r[ppc_rb(insn)];
                c->r[ppc_ra(insn)] = v;
                if (ppc_rc(insn)) { cr0_from(c, v); }
                break;
            }
            case 0x01C: {                   /* and                      */
                const uint32_t v = c->r[ppc_rd(insn)] & c->r[ppc_rb(insn)];
                c->r[ppc_ra(insn)] = v;
                if (ppc_rc(insn)) { cr0_from(c, v); }
                break;
            }
            case 0x13C: {                   /* xor                      */
                const uint32_t v = c->r[ppc_rd(insn)] ^ c->r[ppc_rb(insn)];
                c->r[ppc_ra(insn)] = v;
                if (ppc_rc(insn)) { cr0_from(c, v); }
                break;
            }

            case 0x000:                     /* cmp                      */
                cr_compare(c, (insn >> 23) & 0x7u,
                           c->r[ppc_ra(insn)], c->r[ppc_rb(insn)], true);
                break;
            case 0x020:                     /* cmpl                     */
                cr_compare(c, (insn >> 23) & 0x7u,
                           c->r[ppc_ra(insn)], c->r[ppc_rb(insn)], false);
                break;

            case 0x153:                     /* mfspr                    */
            case 0x1D3: {                   /* mtspr                    */
                /*
                 * The SPR number is *split and swapped*: bits 11:15 are
                 * the low five bits and 16:20 the high five, so the
                 * field reads back-to-front. Reading it straight gives
                 * SPR 256 where 1 was meant -- and LR is 8, which
                 * unswapped is 256, so `mflr` would silently address a
                 * different register.
                 */
                const uint32_t sprf = (insn >> 11) & 0x3FFu;
                const uint32_t spr = ((sprf & 0x1Fu) << 5) | ((sprf >> 5) & 0x1Fu);
                const bool store = ppc_xo10(insn) == 0x1D3u;
                uint32_t *slot;

                switch (spr) {
                case PPC_SPR_XER:   slot = &c->xer;   break;
                case PPC_SPR_LR:    slot = &c->lr;    break;
                case PPC_SPR_CTR:   slot = &c->ctr;   break;
                case PPC_SPR_SRR0:  slot = &c->srr0;  break;
                case PPC_SPR_SRR1:  slot = &c->srr1;  break;
                case PPC_SPR_CSRR0: slot = &c->csrr0; break;
                case PPC_SPR_CSRR1: slot = &c->csrr1; break;
                case PPC_SPR_DEAR:  slot = &c->dear;  break;
                case PPC_SPR_ESR:   slot = &c->esr;   break;
                case PPC_SPR_IVPR:  slot = &c->ivpr;  break;
                case PPC_SPR_PIR:   slot = &c->pir;   break;
                case PPC_SPR_PVR:   slot = &c->pvr;   break;
                default:
                    if (spr >= PPC_SPR_SPRG0 && spr < PPC_SPR_SPRG0 + 8u) {
                        slot = &c->sprg[spr - PPC_SPR_SPRG0];
                    } else if (spr >= PPC_SPR_IVOR0 &&
                               spr < PPC_SPR_IVOR0 + PPC_IVOR_COUNT) {
                        slot = &c->ivor[spr - PPC_SPR_IVOR0];
                    } else {
                        /* Unimplemented SPRs raise rather than reading
                         * zero: "reads zero" is how a guest silently
                         * mis-detects its own core. */
                        EXC(PPC_IVOR_PROGRAM);
                    }
                    break;
                }

                if (store) {
                    /* PVR and PIR identify the part and the core; a
                     * write is architecturally ignored rather than
                     * faulting, so a guest cannot claim to be another
                     * core by writing one. */
                    if (spr != PPC_SPR_PVR && spr != PPC_SPR_PIR) {
                        *slot = c->r[ppc_rd(insn)];
                    }
                } else {
                    c->r[ppc_rd(insn)] = *slot;
                }
                break;
            }

            case 0x053:                     /* mfmsr                    */
                c->r[ppc_rd(insn)] = c->msr;
                break;
            case 0x092:                     /* mtmsr                    */
                c->msr = c->r[ppc_rd(insn)];
                c->irq_dirty = true;
                break;

            case 0x013:                     /* mfcr                     */
                c->r[ppc_rd(insn)] = c->cr;
                break;

            default:
                EXC(PPC_IVOR_PROGRAM);
            }
            break;

        case 0x11:                          /* sc -- the system call    */
        do_syscall:
            /*
             * The platform's syscall hook gets first refusal, so a host
             * harness can offer write/exit the way it does for the other
             * frontends. Only if it declines does this become an
             * architectural interrupt.
             */
            if (c->syscall != NULL) {
                /*
                 * The PowerPC EABI passes arguments in r3..r10 and
                 * returns in r3, so the syscall number is r0 by the
                 * Linux convention and the arguments start at r3. That
                 * is an ABI choice, not an architectural one -- `sc`
                 * itself says nothing about where anything lives.
                 */
                emu_syscall_t sc = {
                    .nr  = c->r[0],
                    .arg = { c->r[3], c->r[4], c->r[5], c->r[6] },
                    .ret = 0u,
                };

                c->pc = next;
                if (c->syscall((emu_cpu_t *)c, &sc, c->syscall_user)) {
                    c->r[3] = sc.ret;
                    pc = c->pc;
                    goto retired_insn;
                }
            }
            EXC(PPC_IVOR_SYSTEM_CALL);

        default:
            EXC(PPC_IVOR_PROGRAM);
        }

        pc = next;

    retired_insn:
        done++;
        c->retired++;
        c->cycles++;
        continue;

    next_insn:
        /*
         * Reached after an exception: the pc is the handler's and the
         * instruction did *not* retire -- so `retired` is not bumped.
         *
         * `done` is, and that is not bookkeeping, it is termination. The
         * budget bounds *work*, and an exception is work; counting only
         * retirements means a core that never retires is never bounded.
         * That is reachable in one line of guest code: with IVPR and the
         * IVORs still zero the vector is address 0, which is unmapped,
         * so the fetch faults, vectors to 0 again, and spins forever
         * without the caller's cap ever being consulted. Found exactly
         * that way by test_unimplemented_reports hanging.
         */
        done++;
        c->cycles++;
        continue;
    }

    c->pc = pc;
    if (retired != NULL) {
        *retired = done;
    }
    return reason;
}

emu_run_reason_t ppc_step(ppc_cpu_t *c)
{
    return ppc_backend->run((emu_cpu_t *)c, 1u, NULL);
}

const emu_backend_t ppc_backend_interp = {
    .name = "interp",
    .run  = ppc_run,
};

const emu_backend_t *ppc_backend = &ppc_backend_interp;
