/* SPDX-License-Identifier: Apache-2.0 */
/*
 * rv_clint.c - Core local interruptor: mtime, mtimecmp, msip.
 */

#include "rv32/rv_clint.h"
#include "rv32/rv_hart.h"

/* ------------------------------------------------------------------ */

/* MTIP is a level signal: it is set exactly while mtime >= mtimecmp. */
static void update_mtip(rv_clint_t *c)
{
    if (c->hart != NULL) {
        rv_hart_set_irq(c->hart, RV_INT_M_TIMER, c->mtime >= c->mtimecmp);
    }
}

void rv_clint_init(rv_clint_t *c, rv_hart_t *hart)
{
    c->mtime = 0u;
    /*
     * Reset mtimecmp to all-ones rather than zero: at zero the comparison
     * would be true immediately and the guest would take a timer interrupt
     * before it ever programmed one.
     */
    c->mtimecmp = UINT64_MAX;
    c->msip = 0u;
    c->hart = hart;

    if (hart != NULL) {
        hart->mtime = &c->mtime;
    }
}

void rv_clint_set_time(rv_clint_t *c, uint64_t now)
{
    c->mtime = now;
    update_mtip(c);
}

void rv_clint_advance(rv_clint_t *c, uint32_t delta)
{
    c->mtime = c->mtime + delta;
    update_mtip(c);
}

/* ------------------------------------------------------------------ */
/* MMIO                                                                */
/* ------------------------------------------------------------------ */

/*
 * The 64-bit registers are accessed as two 32-bit halves by RV32 code, so
 * each half is handled independently. Writing the low half of mtimecmp can
 * momentarily produce a value in the past; that is the same hazard real
 * hardware has, and guests deal with it by writing -1 to the low half
 * first. We do not paper over it.
 */

static emu_fault_t clint_read(void *ctx, uint32_t off, uint32_t size, uint32_t *out)
{
    rv_clint_t *c = (rv_clint_t *)ctx;

    if (size != 4u) {
        return EMU_FAULT_LOAD;
    }

    switch (off) {
    case RV_CLINT_MSIP:            *out = c->msip & 1u; break;
    case RV_CLINT_MTIMECMP:        *out = (uint32_t)c->mtimecmp; break;
    case RV_CLINT_MTIMECMP + 4u:   *out = (uint32_t)(c->mtimecmp >> 32); break;
    case RV_CLINT_MTIME:           *out = (uint32_t)c->mtime; break;
    case RV_CLINT_MTIME + 4u:      *out = (uint32_t)(c->mtime >> 32); break;
    default:                       *out = 0u; break;   /* reserved: reads as 0 */
    }
    return EMU_FAULT_NONE;
}

static emu_fault_t clint_write(void *ctx, uint32_t off, uint32_t size, uint32_t val)
{
    rv_clint_t *c = (rv_clint_t *)ctx;

    if (size != 4u) {
        return EMU_FAULT_STORE;
    }

    switch (off) {
    case RV_CLINT_MSIP:
        c->msip = val & 1u;
        if (c->hart != NULL) {
            rv_hart_set_irq(c->hart, RV_INT_M_SOFT, c->msip != 0u);
        }
        break;

    case RV_CLINT_MTIMECMP:
        c->mtimecmp = (c->mtimecmp & 0xFFFFFFFF00000000ull) | val;
        update_mtip(c);
        break;

    case RV_CLINT_MTIMECMP + 4u:
        c->mtimecmp = (c->mtimecmp & 0xFFFFFFFFull) | ((uint64_t)val << 32);
        update_mtip(c);
        break;

    /*
     * mtime is writable on real CLINT hardware; guests use it to reset the
     * time base. Writing it re-evaluates MTIP.
     */
    case RV_CLINT_MTIME:
        c->mtime = (c->mtime & 0xFFFFFFFF00000000ull) | val;
        update_mtip(c);
        break;

    case RV_CLINT_MTIME + 4u:
        c->mtime = (c->mtime & 0xFFFFFFFFull) | ((uint64_t)val << 32);
        update_mtip(c);
        break;

    default:
        break;   /* reserved: writes ignored */
    }
    return EMU_FAULT_NONE;
}

/*
 * ACLINT views of the same state.
 *
 * ACLINT splits the legacy SiFive CLINT into separately addressable
 * devices: MSWI, holding the msip registers, and MTIMER, holding mtimecmp
 * and mtime at offset 0x7FF8. The split is a renaming rather than a
 * redesign, because the de-facto CLINT layout already *is* those two
 * devices at fixed relative offsets -- MSWI at +0x0000 and MTIMER at
 * +0x4000, which puts mtime at 0x4000 + 0x7FF8 = 0xBFF8, exactly where the
 * legacy layout has it.
 *
 * So these ops translate an offset and share every byte of state with the
 * CLINT above. Mapping the two devices at the addresses the legacy layout
 * used leaves existing guests working unchanged while giving software that
 * expects ACLINT the device boundaries it looks for, and a platform is free
 * to map them somewhere else entirely, which is the part ACLINT adds.
 */
static emu_fault_t mswi_read(void *ctx, uint32_t off, uint32_t size, uint32_t *out)
{
    return clint_read(ctx, off, size, out);
}

static emu_fault_t mswi_write(void *ctx, uint32_t off, uint32_t size, uint32_t val)
{
    return clint_write(ctx, off, size, val);
}

static emu_fault_t mtimer_read(void *ctx, uint32_t off, uint32_t size, uint32_t *out)
{
    return clint_read(ctx, off + RV_CLINT_MTIMECMP, size, out);
}

static emu_fault_t mtimer_write(void *ctx, uint32_t off, uint32_t size, uint32_t val)
{
    return clint_write(ctx, off + RV_CLINT_MTIMECMP, size, val);
}

const emu_dev_ops_t rv_aclint_mswi_ops = {
    .read  = mswi_read,
    .write = mswi_write,
    .tick  = NULL,
};

const emu_dev_ops_t rv_aclint_mtimer_ops = {
    .read  = mtimer_read,
    .write = mtimer_write,
    .tick  = NULL,
};

const emu_dev_ops_t rv_clint_ops = {
    .read  = clint_read,
    .write = clint_write,
    .tick  = NULL,
};
