/* SPDX-License-Identifier: Apache-2.0 */
/*
 * rv_aplic.c - APLIC, direct delivery mode, one interrupt domain, one hart.
 *
 * Written against "The RISC-V Advanced Interrupt Architecture", version
 * 20250312, in docs/riscv/. Section numbers below refer to it.
 *
 * The reason this device exists is that an interrupt is the one thing the
 * passthrough window cannot carry. A guest driver reaches a real STM32
 * peripheral by using its address, but when that peripheral raises an
 * interrupt the ARM NVIC vectors into the *emulator*. Something has to turn
 * that into a guest trap, and this is it: the platform's handler calls
 * rv_aplic_raise, which sets a pending bit and drives MEIP.
 *
 * What is not implemented, and why:
 *
 *   MSI delivery mode   targets an IMSIC, which needs S-mode CSRs this core
 *                       does not have. DM is WARL, so it reads back 0.
 *   child domains       there is one domain, so sourcecfg.D is read-only 0
 *                       and writing a value with D=1 zeroes the register,
 *                       which is what 4.5.2 requires of a leaf domain.
 *   setipnum_le/be      byte order is fixed little-endian, so domaincfg.BE
 *                       is read-only 0 and these add nothing.
 *   genmsi              MSI mode only.
 */

#include "rv32/rv_aplic.h"
#include "rv32/rv_hart.h"
#include "rv32/rv_csr.h"

/* A source is usable only if sourcecfg.SM says it is active. */
static bool src_active(const rv_aplic_t *a, uint32_t i)
{
    return i != 0u && i < RV_APLIC_SOURCES &&
           (a->sourcecfg[i] & 0x7u) != RV_APLIC_SM_INACTIVE;
}

/*
 * The highest-priority pending-and-enabled source, in the format topi and
 * claimi both use: source in bits 25:16, priority in bits 7:0. Zero when
 * there is nothing to deliver.
 *
 * Lower IPRIO is higher priority, ties going to the lower source number,
 * which is 4.8.1.4. An IPRIO of zero is treated as one: zero would mean a
 * priority higher than any expressible threshold, and 4.5.16.1 leaves the
 * value reserved.
 */
static uint32_t aplic_topi(const rv_aplic_t *a)
{
    uint32_t best_src = 0u;
    uint32_t best_prio = 0u;

    for (uint32_t i = 1u; i < RV_APLIC_SOURCES; i++) {
        const uint32_t w = i / 32u, b = 1u << (i % 32u);
        if ((a->pending[w] & a->enabled[w] & b) == 0u || !src_active(a, i)) {
            continue;
        }
        uint32_t prio = a->target[i];
        if (prio == 0u) {
            prio = 1u;
        }
        if (best_src == 0u || prio < best_prio) {
            best_src = i;
            best_prio = prio;
        }
    }
    if (best_src == 0u) {
        return 0u;
    }
    /*
     * ithreshold gates delivery: only priorities strictly below it qualify,
     * and zero disables the filter entirely (4.8.1.3).
     */
    if (a->ithreshold != 0u && best_prio >= a->ithreshold) {
        return 0u;
    }
    return (best_src << 16) | (best_prio & 0xFFu);
}

/*
 * Recompute MEIP. Both domaincfg.IE and idelivery gate delivery without
 * touching any other state, which 4.5.1 is explicit about -- so topi and
 * claimi keep reporting the same thing while delivery is off.
 */
static void aplic_update(rv_aplic_t *a)
{
    const bool deliver =
        (a->domaincfg & RV_APLIC_DOMAINCFG_IE) != 0u &&
        a->idelivery != 0u &&
        (aplic_topi(a) != 0u || a->iforce != 0u);

    if (a->hart != NULL) {
        rv_hart_set_irq(a->hart, RV_INT_M_EXT, deliver);
    }
}

/*
 * Clear pending bits and tell the platform about it.
 *
 * A pending bit going from one to zero is the guest saying it has dealt
 * with the source, and on a platform bridging a real interrupt line that is
 * the only safe moment to unmask it again: the line was masked when the
 * host handler ran, because nothing on the host side can service the
 * device -- only the guest's driver can, through the passthrough window.
 */
static void aplic_clear_pending(rv_aplic_t *a, uint32_t word, uint32_t mask)
{
    const uint32_t cleared = a->pending[word] & mask;

    a->pending[word] &= ~mask;
    if (cleared != 0u && a->eoi != NULL) {
        for (uint32_t b = 0u; b < 32u; b++) {
            if ((cleared & (1u << b)) != 0u) {
                a->eoi(a->eoi_ctx, word * 32u + b);
            }
        }
    }
}

/* Only sources whose mode is active can become pending (4.7). */
static void aplic_set_pending(rv_aplic_t *a, uint32_t word, uint32_t mask)
{
    for (uint32_t b = 0u; b < 32u; b++) {
        const uint32_t i = word * 32u + b;
        if ((mask & (1u << b)) != 0u && src_active(a, i)) {
            a->pending[word] |= 1u << b;
        }
    }
}

static rv_exc_t aplic_read(void *ctx, uint32_t off, uint32_t size,
                           uint32_t *out)
{
    rv_aplic_t *a = (rv_aplic_t *)ctx;

    /*
     * "Only naturally aligned 32-bit simple reads and writes are supported
     * within an interrupt domain's control region" (4.5). Anything else is
     * an access fault rather than a silently narrowed access.
     */
    if (size != 4u || (off & 3u) != 0u) {
        return RV_EXC_LOAD_ACCESS_FAULT;
    }

    *out = 0u;

    if (off == RV_APLIC_DOMAINCFG) {
        *out = RV_APLIC_DOMAINCFG_RO | (a->domaincfg & RV_APLIC_DOMAINCFG_IE);
        return RV_EXC_NONE;
    }
    if (off >= RV_APLIC_SOURCECFG && off < RV_APLIC_SOURCECFG + 4u * 1023u) {
        const uint32_t i = ((off - RV_APLIC_SOURCECFG) / 4u) + 1u;
        if (i < RV_APLIC_SOURCES) {
            *out = a->sourcecfg[i] & 0x7u;
        }
        return RV_EXC_NONE;
    }
    if (off >= RV_APLIC_TARGET && off < RV_APLIC_TARGET + 4u * 1023u) {
        const uint32_t i = ((off - RV_APLIC_TARGET) / 4u) + 1u;
        if (i < RV_APLIC_SOURCES && src_active(a, i)) {
            *out = a->target[i];
        }
        return RV_EXC_NONE;
    }

    if (off >= RV_APLIC_SETIP && off < RV_APLIC_SETIP + 4u * 32u) {
        const uint32_t w = (off - RV_APLIC_SETIP) / 4u;
        *out = (w < RV_APLIC_WORDS) ? a->pending[w] : 0u;
        return RV_EXC_NONE;
    }
    if (off >= RV_APLIC_SETIE && off < RV_APLIC_SETIE + 4u * 32u) {
        const uint32_t w = (off - RV_APLIC_SETIE) / 4u;
        *out = (w < RV_APLIC_WORDS) ? a->enabled[w] : 0u;
        return RV_EXC_NONE;
    }

    switch (off) {
    case RV_APLIC_IN_CLRIP:
        /*
         * Reads the *rectified input* of each source, not the pending bit.
         * With every source driven by rv_aplic_raise rather than by a wire
         * the emulator can sample, the pending bit is the best available
         * answer for a level source and zero is right for an edge one.
         */
        for (uint32_t i = 1u; i < 32u; i++) {
            const uint32_t sm = a->sourcecfg[i] & 0x7u;
            if ((sm == RV_APLIC_SM_LEVEL_HIGH || sm == RV_APLIC_SM_LEVEL_LOW) &&
                (a->pending[0] & (1u << i)) != 0u) {
                *out |= 1u << i;
            }
        }
        return RV_EXC_NONE;
    default:
        break;
    }

    if (off >= RV_APLIC_IDC && off < RV_APLIC_IDC + 32u) {
        switch (off - RV_APLIC_IDC) {
        case RV_APLIC_IDC_IDELIVERY:  *out = a->idelivery; break;
        case RV_APLIC_IDC_IFORCE:     *out = a->iforce; break;
        case RV_APLIC_IDC_ITHRESHOLD: *out = a->ithreshold; break;
        case RV_APLIC_IDC_TOPI:       *out = aplic_topi(a); break;
        case RV_APLIC_IDC_CLAIMI: {
            /*
             * claimi reads topi and clears that source's pending bit as a
             * side effect (4.8.1.5). Reading it when nothing is pending
             * clears iforce instead, which is how a spurious interrupt is
             * dismissed.
             */
            const uint32_t topi = aplic_topi(a);
            if (topi == 0u) {
                a->iforce = 0u;
            } else {
                const uint32_t src = topi >> 16;
                aplic_clear_pending(a, src / 32u, 1u << (src % 32u));
            }
            *out = topi;
            aplic_update(a);
            break;
        }
        default: break;
        }
        return RV_EXC_NONE;
    }

    return RV_EXC_NONE;                 /* reserved: read-only zero */
}

static rv_exc_t aplic_write(void *ctx, uint32_t off, uint32_t size,
                            uint32_t val)
{
    rv_aplic_t *a = (rv_aplic_t *)ctx;

    if (size != 4u || (off & 3u) != 0u) {
        return RV_EXC_STORE_ACCESS_FAULT;
    }

    if (off == RV_APLIC_DOMAINCFG) {
        /* DM and BE are WARL and this implementation supports neither. */
        a->domaincfg = val & RV_APLIC_DOMAINCFG_IE;
        aplic_update(a);
        return RV_EXC_NONE;
    }

    if (off >= RV_APLIC_SOURCECFG && off < RV_APLIC_SOURCECFG + 4u * 1023u) {
        const uint32_t i = ((off - RV_APLIC_SOURCECFG) / 4u) + 1u;
        if (i < RV_APLIC_SOURCES) {
            uint32_t sm = val & 0x7u;
            /* A leaf domain zeroes the register rather than delegating. */
            if ((val & (1u << 10)) != 0u) {
                sm = RV_APLIC_SM_INACTIVE;
            }
            /* SM is WARL; 2 and 3 are not defined, so they become Inactive. */
            if (sm == 2u || sm == 3u) {
                sm = RV_APLIC_SM_INACTIVE;
            }
            a->sourcecfg[i] = (uint8_t)sm;
            if (sm == RV_APLIC_SM_INACTIVE) {
                /* An inactive source keeps neither pending nor enable. */
                a->pending[i / 32u] &= ~(1u << (i % 32u));
                a->enabled[i / 32u] &= ~(1u << (i % 32u));
            }
            aplic_update(a);
        }
        return RV_EXC_NONE;
    }

    if (off >= RV_APLIC_TARGET && off < RV_APLIC_TARGET + 4u * 1023u) {
        const uint32_t i = ((off - RV_APLIC_TARGET) / 4u) + 1u;
        if (i < RV_APLIC_SOURCES) {
            a->target[i] = (uint8_t)(val & 0xFFu);
            aplic_update(a);
        }
        return RV_EXC_NONE;
    }

    if (off >= RV_APLIC_SETIP && off < RV_APLIC_SETIP + 4u * 32u) {
        const uint32_t w = (off - RV_APLIC_SETIP) / 4u;
        if (w < RV_APLIC_WORDS) { aplic_set_pending(a, w, val); }
        aplic_update(a);
        return RV_EXC_NONE;
    }
    if (off >= RV_APLIC_IN_CLRIP && off < RV_APLIC_IN_CLRIP + 4u * 32u) {
        const uint32_t w = (off - RV_APLIC_IN_CLRIP) / 4u;
        if (w < RV_APLIC_WORDS) { aplic_clear_pending(a, w, val); }
        aplic_update(a);
        return RV_EXC_NONE;
    }
    if (off >= RV_APLIC_SETIE && off < RV_APLIC_SETIE + 4u * 32u) {
        const uint32_t w = (off - RV_APLIC_SETIE) / 4u;
        if (w < RV_APLIC_WORDS) { a->enabled[w] |= (w == 0u) ? (val & ~1u) : val; }
        aplic_update(a);
        return RV_EXC_NONE;
    }
    if (off >= RV_APLIC_CLRIE && off < RV_APLIC_CLRIE + 4u * 32u) {
        const uint32_t w = (off - RV_APLIC_CLRIE) / 4u;
        if (w < RV_APLIC_WORDS) { a->enabled[w] &= ~val; }
        aplic_update(a);
        return RV_EXC_NONE;
    }

    switch (off) {
    case RV_APLIC_SETIPNUM:
        if (val < RV_APLIC_SOURCES) {
            aplic_set_pending(a, val / 32u, 1u << (val % 32u));
        }
        break;
    case RV_APLIC_CLRIPNUM:
        if (val < RV_APLIC_SOURCES && val != 0u) {
            aplic_clear_pending(a, val / 32u, 1u << (val % 32u));
        }
        break;
    case RV_APLIC_SETIENUM:
        if (val < RV_APLIC_SOURCES && val != 0u) {
            a->enabled[val / 32u] |= 1u << (val % 32u);
        }
        break;
    case RV_APLIC_CLRIENUM:
        if (val < RV_APLIC_SOURCES && val != 0u) {
            a->enabled[val / 32u] &= ~(1u << (val % 32u));
        }
        break;
    default:
        if (off >= RV_APLIC_IDC && off < RV_APLIC_IDC + 32u) {
            switch (off - RV_APLIC_IDC) {
            case RV_APLIC_IDC_IDELIVERY:  a->idelivery = val & 1u; break;
            case RV_APLIC_IDC_IFORCE:     a->iforce = val & 1u; break;
            case RV_APLIC_IDC_ITHRESHOLD: a->ithreshold = val & 0xFFu; break;
            default: break;             /* topi and claimi are read-only */
            }
        }
        break;
    }

    aplic_update(a);
    return RV_EXC_NONE;
}

const rv_dev_ops_t rv_aplic_ops = {
    .read  = aplic_read,
    .write = aplic_write,
    .tick  = NULL,
};

void rv_aplic_init(rv_aplic_t *a, struct rv_hart *hart)
{
    for (uint32_t i = 0; i < RV_APLIC_SOURCES; i++) {
        a->sourcecfg[i] = 0u;
        a->target[i] = 1u;              /* a usable default priority */
    }
    a->domaincfg = 0u;                  /* IE clear at reset (4.5.1) */
    for (uint32_t w = 0; w < RV_APLIC_WORDS; w++) {
        a->pending[w] = 0u;
        a->enabled[w] = 0u;
    }
    a->idelivery = 0u;
    a->iforce = 0u;
    a->ithreshold = 0u;
    a->hart = hart;
    a->eoi = NULL;
    a->eoi_ctx = NULL;
    aplic_update(a);
}

void rv_aplic_set_eoi(rv_aplic_t *a, rv_aplic_eoi_fn fn, void *ctx)
{
    a->eoi = fn;
    a->eoi_ctx = ctx;
}

void rv_aplic_raise(rv_aplic_t *a, uint32_t source)
{
    if (source == 0u || source >= RV_APLIC_SOURCES) {
        return;
    }
    aplic_set_pending(a, source / 32u, 1u << (source % 32u));
    aplic_update(a);
}
