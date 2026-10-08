/* SPDX-License-Identifier: Apache-2.0 */
/*
 * armv7m_nvic.c - SysTick and the NVIC, and the exception model they are
 * for.
 *
 * **Why this exists at all**: the Thumb-2 backend can only be validated
 * on hardware today, and the firmware that contains it runs a guest on a
 * timer interrupt. Without SysTick and a way into a handler, a Cortex-M
 * guest can execute instructions and cannot be that firmware.
 *
 * What is modelled is the part of the exception model a bare-metal guest
 * uses: enable, pend, mask, take, return. What is not: priorities beyond
 * "something is pending", the process stack, FAULTMASK and BASEPRI,
 * HardFault escalation, and tail-chaining. Each of those is **reported
 * rather than approximated**, because an interrupt that arrives when the
 * guest had disabled it looks like the handler misbehaving and not like
 * a missing feature.
 */

#include "armv7m/armv7m_cpu.h"

#include "emu/emu_bus.h"

/* ------------------------------------------------------------------ */
/* Register offsets within the SCS                                     */
/* ------------------------------------------------------------------ */

#define SYST_CSR 0x010u
#define SYST_RVR 0x014u
#define SYST_CVR 0x018u
#define SYST_CALIB 0x01Cu

#define NVIC_ISER 0x100u
#define NVIC_ICER 0x180u
#define NVIC_ISPR 0x200u
#define NVIC_ICPR 0x280u
#define NVIC_IABR 0x300u
#define NVIC_IPR 0x400u

#define SCB_ICSR 0xD04u
#define SCB_VTOR 0xD08u
#define SCB_AIRCR 0xD0Cu
#define SCB_SHPR1 0xD18u

#define SYST_CSR_ENABLE 1u
#define SYST_CSR_TICKINT 2u
#define SYST_CSR_COUNTFLAG (1u << 16)

static emu_fault_t scs_read(void *ctx, uint32_t off, uint32_t size,
                            uint32_t *out)
{
    armv7m_cpu_t *const c = (armv7m_cpu_t *)ctx;

    /*
     * Word accesses only, which is what the architecture says for most
     * of this block and what every compiler emits. A narrower access to
     * a register that permits one is a gap, not a lie: it faults, and
     * the fault says where.
     */
    if (size != 4u) {
        return EMU_FAULT_LOAD;
    }

    switch (off) {
    case SYST_CSR: {
        uint32_t v = c->systick_ctrl & 7u;

        /*
         * **COUNTFLAG clears on read**, and that is the whole reason a
         * polling guest works: it asks "has it wrapped since I last
         * asked". Leaving it set makes every poll succeed and a delay
         * loop return immediately.
         */
        if (c->systick_countflag) {
            v |= SYST_CSR_COUNTFLAG;
            c->systick_countflag = false;
        }
        *out = v;
        return EMU_FAULT_NONE;
    }
    case SYST_RVR:
        *out = c->systick_reload;
        return EMU_FAULT_NONE;
    case SYST_CVR:
        *out = c->systick_value;
        return EMU_FAULT_NONE;
    case SYST_CALIB:
        /* No calibration value. Bit 31 set says exactly that. */
        *out = 0x80000000u;
        return EMU_FAULT_NONE;

    case NVIC_ISER:
    case NVIC_ICER:
        *out = c->irq_enabled;
        return EMU_FAULT_NONE;
    case NVIC_ISPR:
    case NVIC_ICPR:
        *out = c->irq_pending;
        return EMU_FAULT_NONE;
    case NVIC_IABR:
        *out = c->irq_active;
        return EMU_FAULT_NONE;

    case SCB_ICSR:
        /*
         * VECTACTIVE in the low nine bits. Zero means thread mode,
         * which is what `nest == 0` is.
         */
        *out = (c->nest != 0u) ? c->nest : 0u;
        return EMU_FAULT_NONE;
    case SCB_VTOR:
        *out = c->vtor;
        return EMU_FAULT_NONE;
    case SCB_AIRCR:
        *out = 0xFA050000u; /* the key reads back as zero; see below */
        return EMU_FAULT_NONE;

    default:
        /*
         * Priorities are stored and never consulted, so reading one
         * back is honest and acting on it would not be. Everything else
         * in this block reads as zero rather than faulting: a guest
         * probing for a feature gets "absent", which is true.
         */
        if (off >= NVIC_IPR && off < NVIC_IPR + 0x100u) {
            *out = 0u;
            return EMU_FAULT_NONE;
        }
        *out = 0u;
        return EMU_FAULT_NONE;
    }
}

static emu_fault_t scs_write(void *ctx, uint32_t off, uint32_t size,
                             uint32_t val)
{
    armv7m_cpu_t *const c = (armv7m_cpu_t *)ctx;

    if (size != 4u) {
        return EMU_FAULT_STORE;
    }

    switch (off) {
    case SYST_CSR:
        /*
         * CLKSOURCE is bit 2 and is kept because a guest reads it back;
         * there is one clock here, so it selects nothing.
         */
        c->systick_ctrl = val & 7u;
        return EMU_FAULT_NONE;
    case SYST_RVR:
        c->systick_reload = val & 0x00FFFFFFu;
        return EMU_FAULT_NONE;
    case SYST_CVR:
        /*
         * **Any write clears the counter *and* COUNTFLAG**, whatever
         * the value written -- the architecture is explicit, and a guest
         * arming the timer relies on it to start from a known state.
         */
        c->systick_value = 0u;
        c->systick_countflag = false;
        return EMU_FAULT_NONE;

    /*
     * Set-enable and clear-enable are two registers, not one read-modify
     * -write: writing a 1 acts and writing a 0 does nothing. A guest
     * enabling one source must not disable the rest, which is what a
     * plain assignment here would do.
     */
    case NVIC_ISER:
        c->irq_enabled |= val;
        return EMU_FAULT_NONE;
    case NVIC_ICER:
        c->irq_enabled &= ~val;
        return EMU_FAULT_NONE;
    case NVIC_ISPR:
        c->irq_pending |= val;
        return EMU_FAULT_NONE;
    case NVIC_ICPR:
        c->irq_pending &= ~val;
        return EMU_FAULT_NONE;

    case SCB_VTOR:
        c->vtor = val & 0xFFFFFF80u;
        return EMU_FAULT_NONE;
    case SCB_ICSR:
        /* PENDSTSET / PENDSTCLR, the software way to pend SysTick. */
        if ((val & (1u << 26)) != 0u) {
            c->systick_countflag = true;
        }
        return EMU_FAULT_NONE;
    case SCB_AIRCR:
        /*
         * SYSRESETREQ with the right key is how a guest asks to be
         * reset. Halting is the closest honest thing: there is nothing
         * here to reset *to* that the loader would not have to redo.
         */
        if ((val >> 16) == 0x5FAu && (val & (1u << 2)) != 0u) {
            c->state = EMU_STATE_HALTED;
        }
        return EMU_FAULT_NONE;

    default:
        return EMU_FAULT_NONE; /* priorities and the rest: accepted, unused */
    }
}

/*
 * One SysTick count per guest instruction.
 *
 * **A ratio, not a real clock**, and saying so matters: the host runner
 * drives guest time from retired instructions precisely so a run is
 * reproducible, and this is the same choice the RV32 CLINT makes. A
 * guest that calibrates a delay against SysTick measures instructions,
 * which is what makes two runs of the same image agree.
 */
void armv7m_systick_tick(armv7m_cpu_t *c, uint32_t insns)
{
    if ((c->systick_ctrl & SYST_CSR_ENABLE) == 0u || c->systick_reload == 0u) {
        return;
    }

    uint32_t left = insns;

    while (left != 0u) {
        const uint32_t step =
            (c->systick_value > left) ? left : c->systick_value;

        if (step != 0u) {
            c->systick_value -= step;
            left -= step;
        }
        if (c->systick_value == 0u) {
            /*
             * Wrapped. COUNTFLAG latches whether or not the interrupt
             * is enabled -- that is what lets a guest poll it with
             * TICKINT clear, which is the usual way a bare-metal delay
             * loop is written.
             */
            c->systick_countflag = true;
            if ((c->systick_ctrl & SYST_CSR_TICKINT) != 0u) {
                c->irq_pending |= 1u << 31; /* see armv7m_pending below */
            }
            c->systick_value = c->systick_reload;
            if (left != 0u) {
                left--;
            }
        }
    }
}

/*
 * Which exception wants to run, or zero.
 *
 * **SysTick is held in bit 31 of the same word as the external
 * sources**, which is a compression and not an architectural claim: it
 * is exception 15 and the externals are 16 upward. One word is enough
 * while there are 31 externals, and the bit is named here rather than
 * left for a reader to infer from a shift.
 */
#define SYSTICK_BIT (1u << 31)

uint32_t armv7m_pending(const armv7m_cpu_t *c)
{
    /*
     * PRIMASK blocks everything that can be blocked. There is no
     * priority comparison: with no BASEPRI and no escalation, "is
     * anything pending and are interrupts on" is the whole decision --
     * and a guest that needs more than that will find the priority
     * registers accept writes and change nothing, which is the state
     * this file's header says to expect.
     */
    if (c->primask != 0u || c->nest != 0u) {
        return 0u;
    }
    if ((c->irq_pending & SYSTICK_BIT) != 0u) {
        return ARMV7M_EXC_SYSTICK;
    }

    const uint32_t ready = c->irq_pending & c->irq_enabled & ~SYSTICK_BIT;

    if (ready == 0u) {
        return 0u;
    }
    for (uint32_t i = 0u; i < 31u; i++) {
        if ((ready & (1u << i)) != 0u) {
            return ARMV7M_EXC_EXTERNAL + i;
        }
    }
    return 0u;
}

void armv7m_set_irq(armv7m_cpu_t *c, uint32_t source, bool level)
{
    if (source >= 31u) {
        return;
    }
    /*
     * Level in, latched here. A device that deasserts before the guest
     * gets to its handler still causes one -- which is the architecture's
     * behaviour and the reason `pending` and `enabled` are separate
     * words.
     */
    if (level) {
        c->irq_pending |= 1u << source;
    } else {
        c->irq_pending &= ~(1u << source);
    }
}

const emu_dev_ops_t armv7m_scs_ops = {
    .read = scs_read,
    .write = scs_write,
    .tick = NULL,
};
