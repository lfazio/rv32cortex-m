/* SPDX-License-Identifier: Apache-2.0 */
/*
 * ppc_swt.c - the Software Watchdog Timer.
 *
 * MPC5675K Reference Manual, Rev. 10, chapter 52. Where the chapter says
 * nothing, or says two things, the reading taken:
 *
 *   - **the service sequence is 0xA602, 0xB480.** Table 52-7 gives that
 *     and gives 0xC520, 0xD928 as the soft lock's unlock; SWT_CR[KEY]'s
 *     description and one paragraph of 52.4 give 0xC520, 0xD928 for the
 *     service as well. The same paragraph says "the service sequence
 *     logic ignores unlock sequence writes", which would mean nothing if
 *     the two were the same values -- so the register table it is;
 *   - **off out of reset.** Table 52-2 has SWT_CR[WEN] set, and 52.4
 *     says the reset value "is device-specific": on this part a flash
 *     user option decides it for SWT_0, and SWT_1 "is always off until
 *     enabled by software". Here it is the option's "off", so a guest
 *     that never heard of the watchdog is not reset 261,600 instructions
 *     in. The rest of the reset value stands, the soft lock included;
 *   - **both clocks are the processor clock.** SWT_CR[CSL] chooses the
 *     system or the oscillator clock; there is no oscillator here, and
 *     the system clock is one tick an instruction, as for the STM;
 *   - **"incorrect values written to the service register when
 *     enabled"** are any write that is neither the service sequence's
 *     next value nor part of an unlock -- a second word of either with no
 *     first, included -- and such a write clears both halves. Disabled,
 *     they are ignored;
 *   - **the window applies to both service writes**, not the unlock;
 *   - MAPn, STP and FRZ are held and do nothing: one master per core,
 *     no stop mode, and a debugger that stops guest time altogether.
 */

#include "ppc/ppc_swt.h"
#include "ppc/ppc_cpu.h"
#include "ppc/ppc_intc.h"

static const char *const k_timeout[2] = {"SWT_0 timeout", "SWT_1 timeout"};
static const char *const k_invalid[2] = {"SWT_0 invalid access",
                                         "SWT_1 invalid access"};

static bool enabled(const ppc_swt_t *w)
{
    return (w->cr & PPC_SWT_CR_WEN) != 0u;
}

static bool locked(const ppc_swt_t *w)
{
    return (w->cr & (PPC_SWT_CR_HLK | PPC_SWT_CR_SLK)) != 0u;
}

static uint32_t period(const ppc_swt_t *w)
{
    return (w->to > PPC_SWT_TO_MIN) ? w->to : PPC_SWT_TO_MIN;
}

static void request(ppc_swt_t *w)
{
    if (w->intc != NULL) {
        ppc_intc_set_source(w->intc, PPC_SWT_IRQ0 + w->index,
                            (w->ir & PPC_SWT_IR_TIF) != 0u);
    }
}

static void system_reset(ppc_swt_t *w, const char *const why[2])
{
    w->reset = true;
    if (w->core != NULL) {
        ppc_cpu_system_reset(w->core, why[w->index & 1u]);
    }
}

void ppc_swt_reset(ppc_swt_t *w)
{
    w->cr = PPC_SWT_CR_RESET;
    w->ir = 0u;
    w->to = PPC_SWT_TO_RESET;
    w->wn = 0u;
    w->sk = 0u;
    w->count = 0u;
    w->expired = false;
    w->svc_half = false;
    w->unl_half = false;
    w->reset = false;
    request(w);
}

uint32_t ppc_swt_until(const ppc_swt_t *w)
{
    /* Never 0 while counting: every load is at least 0x100. */
    return (enabled(w) && !w->reset) ? w->count : 0xFFFFFFFFu;
}

void ppc_swt_advance(ppc_swt_t *w, uint32_t ticks)
{
    if (!enabled(w) || w->reset) {
        return;
    }
    /*
     * Expiry is the counter reaching zero. A slice can span more than
     * one period -- the interrupt reloads it -- so this is a loop, and
     * the second expiry inside one slice is still the reset.
     */
    while (ticks >= w->count) {
        ticks -= w->count;
        if ((w->cr & PPC_SWT_CR_ITR) != 0u && !w->expired) {
            w->expired = true;
            w->ir |= PPC_SWT_IR_TIF;
            request(w);
            w->count = period(w);
        } else {
            w->count = 0u;
            system_reset(w, k_timeout);
            return;
        }
    }
    w->count -= ticks;
}

/*
 * 52.3: an invalid access is a bus error, or with RIA a system reset --
 * "if WEN = 1".
 */
static emu_fault_t invalid(ppc_swt_t *w, emu_fault_t f)
{
    if ((w->cr & PPC_SWT_CR_RIA) != 0u && enabled(w)) {
        system_reset(w, k_invalid);
        return EMU_FAULT_NONE;
    }
    return f;
}

static void serviced(ppc_swt_t *w)
{
    w->count = period(w);
    w->expired = false;
    w->svc_half = false;
}

/* A write of SWT_SR's WSC field: true if it was a valid one. */
static bool service_write(ppc_swt_t *w, uint32_t wsc)
{
    /* The unlock sequence, which the service logic ignores. */
    if (wsc == PPC_SWT_UNLOCK1) {
        w->unl_half = true;
        return true;
    }
    if (wsc == PPC_SWT_UNLOCK2 && w->unl_half) {
        w->unl_half = false;
        w->cr &= ~PPC_SWT_CR_SLK;
        return true;
    }
    /* Service writes are valid only in an open window (52.4). */
    if ((w->cr & PPC_SWT_CR_WND) != 0u && enabled(w) && w->count >= w->wn) {
        return false;
    }
    if ((w->cr & PPC_SWT_CR_KEY) != 0u) {
        /* Equation 52-1: each valid key becomes the next one's seed. */
        const uint32_t next = (17u * w->sk + 3u) & 0xFFFFu;

        if (wsc != next) {
            return false;
        }
        w->sk = next;
        if (w->svc_half) {
            serviced(w);
        } else {
            w->svc_half = true;
        }
        return true;
    }
    if (wsc == PPC_SWT_SERVICE1) {
        w->svc_half = true;
        return true;
    }
    if (wsc == PPC_SWT_SERVICE2 && w->svc_half) {
        serviced(w);
        return true;
    }
    return false;
}

static emu_fault_t swt_read(void *ctx, uint32_t off, uint32_t size,
                            uint32_t *out)
{
    ppc_swt_t *const w = (ppc_swt_t *)ctx;

    if (size != 4u || (off & 3u) != 0u || off > PPC_SWT_SK) {
        return invalid(w, EMU_FAULT_LOAD);
    }
    if (w->core != NULL) {
        ppc_cpu_sync_clock(w->core); /* the counter as of now */
    }
    switch (off) {
    case PPC_SWT_CR:
        *out = w->cr;
        break;
    case PPC_SWT_IR:
        *out = w->ir;
        break;
    case PPC_SWT_TO:
        *out = w->to;
        break;
    case PPC_SWT_WN:
        *out = w->wn;
        break;
    case PPC_SWT_CO:
        /* 52.3.2.6: the counter while disabled, zero while enabled. */
        *out = enabled(w) ? 0u : w->count;
        break;
    default: /* SR and SK are write-only and read as zero */
        *out = 0u;
        break;
    }
    return EMU_FAULT_NONE;
}

static emu_fault_t swt_write(void *ctx, uint32_t off, uint32_t size,
                             uint32_t val)
{
    ppc_swt_t *const w = (ppc_swt_t *)ctx;
    emu_fault_t f = EMU_FAULT_NONE;

    if (size != 4u || (off & 3u) != 0u || off > PPC_SWT_SK) {
        return invalid(w, EMU_FAULT_STORE);
    }
    if (w->core != NULL) {
        ppc_cpu_sync_clock(w->core); /* count up to this store first */
    }
    switch (off) {
    case PPC_SWT_CR:
        if (locked(w)) {
            f = invalid(w, EMU_FAULT_STORE);
            break;
        }
        if ((val & ~w->cr & PPC_SWT_CR_WEN) != 0u) {
            /* Enabling loads the counter (52.3.2.3). */
            w->count = period(w);
            w->expired = false;
        }
        w->cr = val & PPC_SWT_CR_WRITABLE;
        break;
    case PPC_SWT_IR:
        if ((val & PPC_SWT_IR_TIF) != 0u) { /* w1c */
            w->ir = 0u;
            request(w);
        }
        break;
    case PPC_SWT_TO:
    case PPC_SWT_WN:
    case PPC_SWT_SK:
        if (locked(w)) {
            f = invalid(w, EMU_FAULT_STORE);
        } else if (off == PPC_SWT_TO) {
            w->to = val;
        } else if (off == PPC_SWT_WN) {
            w->wn = val;
        } else {
            w->sk = val & 0xFFFFu;
        }
        break;
    case PPC_SWT_SR:
        if (!service_write(w, val & 0xFFFFu)) {
            w->svc_half = false;
            w->unl_half = false;
            if (enabled(w)) {
                f = invalid(w, EMU_FAULT_STORE);
            }
        }
        break;
    default: /* CO: a write to a read-only register */
        f = invalid(w, EMU_FAULT_STORE);
        break;
    }
    if (w->core != NULL) {
        ppc_cpu_soc_changed(w->core); /* the next expiry may have moved */
    }
    return f;
}

const emu_dev_ops_t ppc_swt_ops = {
    .read = swt_read,
    .write = swt_write,
    .tick = NULL,
};
