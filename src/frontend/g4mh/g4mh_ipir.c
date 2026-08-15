/* SPDX-License-Identifier: Apache-2.0 */
/*
 * g4mh_ipir.c - IPIR, inter-processor interrupts. U2B manual section 3.4.
 *
 * Four channels, each a full 6x6 matrix: PEm asks for an interrupt on
 * PEx by setting IPInREQm[x], and PEx sees it in IPInFLGx[m] -- so the
 * receiver learns *which* PE asked, which is the feature that makes this
 * cheaper than a software flag word.
 *
 * The enable is owned by the receiver and gates the transfer, not the
 * request. Table 3.152 spells out the four cases and the asymmetry
 * matters:
 *
 *   REQx[m] set while ENx[m] = 1   REQ set,  FLG set
 *   REQx[m] set while ENx[m] = 0   REQ set,  FLG unchanged
 *
 * So a request raised before the receiver enables it is *remembered* in
 * REQ and does not arrive. It also does not arrive later: nothing
 * re-evaluates on a write to EN, and the manual's initial-setting
 * sequence has the receiver clear the stale requests with FCLR before
 * enabling, precisely because they would otherwise sit there. This
 * implementation does the same -- it does not re-evaluate on EN, which
 * is a deliberate difference from the barrier next door, where the
 * hardware does.
 *
 * The interrupt line is level: it is asserted while any FLG bit of that
 * channel is set for that PE. INTIPIRn is EIINTn.
 */

#include "g4mh/g4mh_intercpu.h"
#include "g4mh/g4mh_intc.h"

#include <string.h>

void g4mh_ipir_init(g4mh_ipir_t *p)
{
    memset(p, 0, sizeof(*p));
}

void g4mh_ipir_bind(g4mh_ipir_t *p, unsigned pe, struct g4mh_intc *ic)
{
    if (pe < G4MH_PE_COUNT) {
        p->intc[pe] = ic;
    }
}

/*
 * Deliver channel n to PE x if anything is flagged for it.
 *
 * Raise-only, because that is all the INTC offers: a channel is cleared
 * by the guest acknowledging it, not by the device lowering a line. The
 * same shape as the RV32 frontend's APLIC, and for the same reason --
 * see rv32_set_irq, which drops a lowering as "describing an edge".
 */
static void ipir_deliver(g4mh_ipir_t *p, unsigned n, unsigned x)
{
    if (x >= G4MH_PE_COUNT || p->intc[x] == NULL) {
        return;
    }
    if (p->flg[n][x] != 0u) {
        g4mh_intc_raise(p->intc[x], G4MH_IPIR_CHANNEL(n));
    }
}

/*
 * REQm[x] <- 1. Sets the request unconditionally and the flag only if
 * the receiver has enabled this sender.
 */
static void ipir_request(g4mh_ipir_t *p, unsigned n, unsigned m,
                         unsigned x)
{
    p->req[n][m] |= (uint8_t)(1u << x);
    if (x < G4MH_INTERCPU_PES && ((p->en[n][x] >> m) & 1u) != 0u) {
        p->flg[n][x] |= (uint8_t)(1u << m);
        ipir_deliver(p, n, x);
    }
}

/* FCLRm[x] <- 1: the *receiver* m dismissing sender x. */
static void ipir_flag_clear(g4mh_ipir_t *p, unsigned n, unsigned m,
                            unsigned x)
{
    p->flg[n][m] &= (uint8_t)~(1u << x);
    if (x < G4MH_INTERCPU_PES) {
        p->req[n][x] &= (uint8_t)~(1u << m);
    }
}

/* RCLRm[x] <- 1: the *sender* m withdrawing its request to x. */
static void ipir_req_clear(g4mh_ipir_t *p, unsigned n, unsigned m,
                           unsigned x)
{
    p->req[n][m] &= (uint8_t)~(1u << x);
    if (x < G4MH_INTERCPU_PES && ((p->en[n][x] >> m) & 1u) != 0u) {
        p->flg[n][x] &= (uint8_t)~(1u << m);
    }
}

/*
 * Decode an offset into a channel, a PE and a register. Holes read as
 * zero and ignore writes.
 */
static bool ipir_decode(uint32_t off, unsigned self_pe, unsigned *chan,
                        unsigned *pe, unsigned *reg)
{
    if (off < 0x800u) {                     /* the self region          */
        const uint32_t n = off / 0x20u;
        const uint32_t r = off % 0x20u;

        if (n >= G4MH_IPIR_CHANNELS) {
            return false;
        }
        switch (r) {
        case G4MH_IPIR_ENS:   *reg = G4MH_IPIR_EN;   break;
        case G4MH_IPIR_FLGS:  *reg = G4MH_IPIR_FLG;  break;
        case G4MH_IPIR_FCLRS: *reg = G4MH_IPIR_FCLR; break;
        case G4MH_IPIR_REQS:  *reg = G4MH_IPIR_REQ;  break;
        case G4MH_IPIR_RCLRS: *reg = G4MH_IPIR_RCLR; break;
        default: return false;
        }
        *chan = (unsigned)n;
        *pe   = self_pe;
        return true;
    }

    {
        const uint32_t rel = off - 0x800u;
        const uint32_t m   = rel / 0x100u;
        const uint32_t n   = (rel % 0x100u) / 0x20u;
        const uint32_t r   = rel % 0x20u;

        if (m >= G4MH_INTERCPU_PES || n >= G4MH_IPIR_CHANNELS) {
            return false;
        }
        switch (r) {
        case G4MH_IPIR_EN & 0x1Fu:   *reg = G4MH_IPIR_EN;   break;
        case G4MH_IPIR_FLG & 0x1Fu:  *reg = G4MH_IPIR_FLG;  break;
        case G4MH_IPIR_FCLR & 0x1Fu: *reg = G4MH_IPIR_FCLR; break;
        case G4MH_IPIR_REQ & 0x1Fu:  *reg = G4MH_IPIR_REQ;  break;
        case G4MH_IPIR_RCLR & 0x1Fu: *reg = G4MH_IPIR_RCLR; break;
        default: return false;
        }
        *chan = (unsigned)n;
        *pe   = (unsigned)m;
        return true;
    }
}

static emu_fault_t ipir_read(void *ctx, uint32_t off, uint32_t size,
                             uint32_t *out)
{
    const g4mh_intercpu_port_t *p = (const g4mh_intercpu_port_t *)ctx;
    const g4mh_ipir_t *ip = (const g4mh_ipir_t *)p->state;
    unsigned n, pe, reg;

    (void)size;
    *out = 0u;

    if (!ipir_decode(off, p->pe, &n, &pe, &reg)) {
        return EMU_FAULT_NONE;
    }
    if (pe >= G4MH_INTERCPU_PES) {
        return EMU_FAULT_NONE;
    }

    switch (reg) {
    case G4MH_IPIR_EN:  *out = ip->en[n][pe];  break;
    case G4MH_IPIR_FLG: *out = ip->flg[n][pe]; break;
    case G4MH_IPIR_REQ: *out = ip->req[n][pe]; break;
    default:            break;      /* FCLR and RCLR are write-only */
    }
    return EMU_FAULT_NONE;
}

static emu_fault_t ipir_write(void *ctx, uint32_t off, uint32_t size,
                              uint32_t val)
{
    g4mh_intercpu_port_t *p = (g4mh_intercpu_port_t *)ctx;
    g4mh_ipir_t *ip = (g4mh_ipir_t *)p->state;
    unsigned n, pe, reg;
    uint8_t bits;

    (void)size;

    if (!ipir_decode(off, p->pe, &n, &pe, &reg)) {
        return EMU_FAULT_NONE;
    }
    if (pe >= G4MH_INTERCPU_PES) {
        return EMU_FAULT_NONE;
    }
    bits = (uint8_t)(val & 0x3Fu);

    switch (reg) {
    case G4MH_IPIR_EN:
        ip->en[n][pe] = bits;
        break;

    case G4MH_IPIR_REQ:
        /* Writing 0 to a bit is ignored -- this register only sets. */
        for (unsigned x = 0; x < G4MH_INTERCPU_PES; x++) {
            if (((bits >> x) & 1u) != 0u) {
                ipir_request(ip, n, pe, x);
            }
        }
        break;

    case G4MH_IPIR_FCLR:
        for (unsigned x = 0; x < G4MH_INTERCPU_PES; x++) {
            if (((bits >> x) & 1u) != 0u) {
                ipir_flag_clear(ip, n, pe, x);
            }
        }
        break;

    case G4MH_IPIR_RCLR:
        for (unsigned x = 0; x < G4MH_INTERCPU_PES; x++) {
            if (((bits >> x) & 1u) != 0u) {
                ipir_req_clear(ip, n, pe, x);
            }
        }
        break;

    default:
        break;                      /* FLG is read-only */
    }
    return EMU_FAULT_NONE;
}

const emu_dev_ops_t g4mh_ipir_ops = {
    .read  = ipir_read,
    .write = ipir_write,
};
