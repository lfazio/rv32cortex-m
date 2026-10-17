/* SPDX-License-Identifier: Apache-2.0 */
/*
 * ppc_intc.c - the interrupt controller: priorities, the LIFO, and the
 * two handshakes with the core.
 *
 * MPC5675K Reference Manual, Rev. 10, chapter 32. Where it says nothing,
 * the conservative reading, stated so a test can hold it:
 *
 *   - **once asserted, the request stays asserted until acknowledged**,
 *     with the vector and priority latched when it asserted -- 32.4.1:
 *     "the INTC has no spurious vector support", so a source that
 *     negates in between is still the one acknowledged. Only a write of
 *     INTC_CPR re-decides it, which is what the priority ceiling
 *     protocol (32.5.5) needs: raise PRI, and what is at or below it
 *     does not interrupt;
 *   - **an IACKR read with no request asserted has no side effect.** The
 *     chapter describes the read as the acknowledgement of a request;
 *     with none there is nothing to push or install;
 *   - **INTC_EOIR pops the LIFO.** Two sentences of the chapter say an
 *     *SSCIR* write pops it (32.3.2.2, and the exception in 32.2.1.2);
 *     32.3.2.4, 32.4.2.2 and the handlers of 32.5.2 all say EOIR, and
 *     an SSCIR write that lowered the priority would make every
 *     software-settable request reorder the ISRs. Read as a slip.
 */

#include "ppc/ppc_intc.h"
#include "ppc/ppc_cpu.h"

#include <string.h>

static bool flag_get(const ppc_intc_t *ic, uint32_t n)
{
    return ((ic->flag[n >> 5] >> (n & 31u)) & 1u) != 0u;
}

static void flag_put(ppc_intc_t *ic, uint32_t n, bool on)
{
    if (on) {
        ic->flag[n >> 5] |= 1u << (n & 31u);
    } else {
        ic->flag[n >> 5] &= ~(1u << (n & 31u));
    }
}

/*
 * 32.4.2.1: the highest priority among the asserted requests, and of
 * those the lowest vector -- ascending order with a strict compare keeps
 * the first. Priority 0 never wins, because it is never above CPR.
 */
static uint32_t arbitrate(const ppc_intc_t *ic, uint32_t *pri)
{
    uint32_t best = 0u;
    uint32_t best_pri = 0u;

    for (uint32_t w = 0u; w < (PPC_INTC_NSRC + 31u) / 32u; w++) {
        uint32_t bits = ic->flag[w];

        while (bits != 0u) {
            const uint32_t n = w * 32u + (uint32_t)__builtin_ctz(bits);
            const uint32_t p = ic->psr[n];

            bits &= bits - 1u;
            if (p > best_pri) {
                best_pri = p;
                best = n;
            }
        }
    }
    *pri = best_pri;
    return best;
}

/* Assert the request if the arbitration's winner is above CPR. */
static void update(ppc_intc_t *ic)
{
    uint32_t pri;
    uint32_t vec;

    if (ic->req) {
        return;
    }
    vec = arbitrate(ic, &pri);
    if (pri > ic->cpr) {
        ic->req = true;
        ic->intvec = vec;
        ic->intpri = pri;
        if (ic->core != NULL) {
            ic->core->irq_dirty = true;
        }
    }
}

static void push(ppc_intc_t *ic, uint32_t pri)
{
    ic->lifo[ic->lifo_head] = (uint8_t)pri;
    ic->lifo_head = (ic->lifo_head + 1u) % PPC_INTC_LIFO;
    if (ic->lifo_depth < PPC_INTC_LIFO) {
        ic->lifo_depth++;
    }
}

static uint32_t pop(ppc_intc_t *ic)
{
    if (ic->lifo_depth == 0u) {
        return 0u; /* "the LIFO will pop 0s" */
    }
    ic->lifo_depth--;
    ic->lifo_head = (ic->lifo_head + PPC_INTC_LIFO - 1u) % PPC_INTC_LIFO;
    return ic->lifo[ic->lifo_head];
}

/* Push, install the request's priority, negate -- then look again. */
static void acknowledge(ppc_intc_t *ic)
{
    push(ic, ic->cpr);
    ic->cpr = ic->intpri;
    ic->req = false;
    update(ic);
}

void ppc_intc_reset(ppc_intc_t *ic)
{
    ic->bcr = 0u;
    ic->cpr = PPC_INTC_PRI_MAX;
    ic->vtba = 0u;
    ic->intvec = 0u;
    ic->intpri = 0u;
    ic->req = false;
    memset(ic->lifo, 0, sizeof(ic->lifo));
    ic->lifo_head = 0u;
    ic->lifo_depth = 0u;
    memset(ic->psr, 0, sizeof(ic->psr));
    memset(ic->flag, 0, sizeof(ic->flag));
}

void ppc_intc_set_source(ppc_intc_t *ic, uint32_t source, bool level)
{
    if (source < PPC_INTC_NSW || source >= PPC_INTC_NSRC) {
        return;
    }
    flag_put(ic, source, level);
    if (level) {
        update(ic);
    }
}

bool ppc_intc_hw_vectored(const ppc_intc_t *ic)
{
    return (ic->bcr & PPC_INTC_BCR_HVEN) != 0u;
}

uint32_t ppc_intc_ack(ppc_intc_t *ic)
{
    const uint32_t vec = ic->intvec;

    if (ic->req) {
        acknowledge(ic);
    }
    return vec;
}

/* Figure 32-4: VTES moves INTVEC up a bit and takes it from VTBA. */
static uint32_t iackr(const ppc_intc_t *ic)
{
    if ((ic->bcr & PPC_INTC_BCR_VTES) != 0u) {
        return (ic->vtba & 0xFFFFF000u) | (ic->intvec << 3);
    }
    return (ic->vtba & 0xFFFFF800u) | (ic->intvec << 2);
}

/*
 * 32.3.2: any access within one 32-bit word, of any size -- including
 * the halfword in its middle two bytes. Big-endian, so the lowest
 * address is the most significant byte. False past the module or across
 * a word.
 */
static bool lane(uint32_t off, uint32_t size, uint32_t *base, uint32_t *shift)
{
    if ((size != 1u && size != 2u && size != 4u) || (off & 3u) + size > 4u ||
        off + size > PPC_INTC_SIZE) {
        return false;
    }
    *base = off & ~3u;
    *shift = (4u - (off - *base) - size) * 8u;
    return true;
}

static uint32_t mask_of(uint32_t size)
{
    return (size == 4u) ? 0xFFFFFFFFu : ((1u << (8u * size)) - 1u);
}

/* The words that exist; everything else in the 16 KiB is reserved. */
static bool mapped(uint32_t base)
{
    return base == PPC_INTC_BCR || base == PPC_INTC_CPR ||
           base == PPC_INTC_IACKR || base == PPC_INTC_EOIR ||
           base == PPC_INTC_SSCIR0_3 || base == PPC_INTC_SSCIR4_7 ||
           (base >= PPC_INTC_PSR0 && base < PPC_INTC_PSR0 + PPC_INTC_NSRC);
}

/* The byte at `off` of a byte-per-entry bank: SSCIR's CLR flags, PSR. */
static uint32_t byte_read(const ppc_intc_t *ic, uint32_t off)
{
    if (off >= PPC_INTC_PSR0) {
        const uint32_t n = off - PPC_INTC_PSR0;

        return (n < PPC_INTC_NSRC) ? ic->psr[n] : 0u;
    }
    return flag_get(ic, off - PPC_INTC_SSCIR0_3) ? PPC_INTC_SSCIR_CLR : 0u;
}

static void byte_write(ppc_intc_t *ic, uint32_t off, uint32_t v)
{
    if (off >= PPC_INTC_PSR0) {
        const uint32_t n = off - PPC_INTC_PSR0;

        if (n < PPC_INTC_NSRC) {
            ic->psr[n] = (uint8_t)(v & PPC_INTC_PRI_MAX);
        }
        return;
    }
    /* 32.3.2.5: SET sets the flag, CLR clears it, and both at once set. */
    if ((v & PPC_INTC_SSCIR_SET) != 0u) {
        flag_put(ic, off - PPC_INTC_SSCIR0_3, true);
    } else if ((v & PPC_INTC_SSCIR_CLR) != 0u) {
        flag_put(ic, off - PPC_INTC_SSCIR0_3, false);
    }
}

static emu_fault_t intc_read(void *ctx, uint32_t off, uint32_t size,
                             uint32_t *out)
{
    ppc_intc_t *const ic = (ppc_intc_t *)ctx;
    uint32_t base;
    uint32_t shift;
    uint32_t w = 0u;

    if (!lane(off, size, &base, &shift) || !mapped(base)) {
        return EMU_FAULT_LOAD;
    }
    switch (base) {
    case PPC_INTC_BCR:
        w = ic->bcr;
        break;
    case PPC_INTC_CPR:
        w = ic->cpr;
        break;
    case PPC_INTC_IACKR:
        w = iackr(ic);
        /* Software vector mode: the read is the acknowledgement. */
        if (!ppc_intc_hw_vectored(ic) && ic->req) {
            acknowledge(ic);
        }
        break;
    case PPC_INTC_EOIR:
        break; /* "reads are always 0" */
    default:
        for (uint32_t i = 0u; i < 4u; i++) {
            w |= byte_read(ic, base + i) << (24u - 8u * i);
        }
        break;
    }
    *out = (w >> shift) & mask_of(size);
    return EMU_FAULT_NONE;
}

static emu_fault_t intc_write(void *ctx, uint32_t off, uint32_t size,
                              uint32_t val)
{
    ppc_intc_t *const ic = (ppc_intc_t *)ctx;
    uint32_t base;
    uint32_t shift;

    if (!lane(off, size, &base, &shift) || !mapped(base)) {
        return EMU_FAULT_STORE;
    }
    const uint32_t m = mask_of(size) << shift;
    const uint32_t v = (val << shift) & m;

    switch (base) {
    case PPC_INTC_BCR:
        ic->bcr =
            ((ic->bcr & ~m) | v) & (PPC_INTC_BCR_VTES | PPC_INTC_BCR_HVEN);
        break;
    case PPC_INTC_CPR:
        if ((m & PPC_INTC_PRI_MAX) != 0u) {
            ic->cpr = ((ic->cpr & ~m) | v) & PPC_INTC_PRI_MAX;
            ic->req = false; /* re-decided against the new priority */
        }
        break;
    case PPC_INTC_IACKR:
        ic->vtba = ((ic->vtba & ~m) | v) & 0xFFFFF800u;
        break;
    case PPC_INTC_EOIR:
        /* Whatever is written, and however wide: one pop (32.3.2.4). */
        ic->cpr = pop(ic);
        break;
    default:
        for (uint32_t i = 0u; i < 4u; i++) {
            const uint32_t at = 24u - 8u * i;

            if (((m >> at) & 0xFFu) != 0u) {
                byte_write(ic, base + i, (v >> at) & 0xFFu);
            }
        }
        break;
    }
    update(ic);
    return EMU_FAULT_NONE;
}

const emu_dev_ops_t ppc_intc_ops = {
    .read = intc_read,
    .write = intc_write,
    .tick = NULL,
};
