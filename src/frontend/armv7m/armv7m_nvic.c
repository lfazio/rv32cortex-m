/* SPDX-License-Identifier: Apache-2.0 */
/*
 * armv7m_nvic.c - the System Control Space: NVIC, SCB, SysTick, MPU and
 * the FP control registers, at 0xE000E000.
 *
 * **Only privileged code may touch it**, and the architecture says how
 * an unprivileged access fails: a BusFault, precise, not a quiet zero.
 * The one exception is STIR with CCR.USERSETMPEND. `c->access_priv` is
 * set by the memory path for the duration of the access, which is the
 * only way a device callback can know who is asking.
 *
 * Byte and halfword accesses are permitted exactly where the ARM ARM
 * permits them -- the priority registers and the fault status register
 * -- because that is where CMSIS uses them: `NVIC_SetPriority` writes
 * one byte of NVIC_IPR, and a frontend that took words only would turn
 * the most ordinary interrupt set-up into a fault.
 */

#include "armv7m/armv7m_cpu.h"

#include "emu/emu_bus.h"

#define SCS_ICTR 0x004u
#define SCS_ACTLR 0x008u
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
#define SCB_CPUID 0xD00u
#define SCB_ICSR 0xD04u
#define SCB_VTOR 0xD08u
#define SCB_AIRCR 0xD0Cu
#define SCB_SCR 0xD10u
#define SCB_CCR 0xD14u
#define SCB_SHPR1 0xD18u
#define SCB_SHCSR 0xD24u
#define SCB_CFSR 0xD28u
#define SCB_HFSR 0xD2Cu
#define SCB_DFSR 0xD30u
#define SCB_MMFAR 0xD34u
#define SCB_BFAR 0xD38u
#define SCB_AFSR 0xD3Cu
#define SCB_CPACR 0xD88u
#define MPU_TYPE 0xD90u
#define MPU_CTRL 0xD94u
#define MPU_RNR 0xD98u
#define MPU_RBAR 0xD9Cu
#define MPU_RASR 0xDA0u
#define NVIC_STIR 0xF00u
#define FP_FPCCR 0xF34u
#define FP_FPCAR 0xF38u
#define FP_FPDSCR 0xF3Cu
#define FP_MVFR0 0xF40u
#define FP_MVFR1 0xF44u
#define FP_MVFR2 0xF48u

#define SYST_CSR_ENABLE 1u
#define SYST_CSR_TICKINT 2u
#define SYST_CSR_COUNTFLAG (1u << 16)

/*
 * The identification registers, as a Cortex-M7 r0p1 with FPv5-SP reports
 * them -- the values tests/armv7m-diff reads back from the F746.
 */
#define CPUID_M7_R0P1 0x410FC271u
#define MVFR0_FPV5_SP 0x10110021u
#define MVFR1_FPV5_SP 0x11000011u
#define MVFR2_FPV5_SP 0x00000040u

/* CFSR, HFSR and the rest are write-one-to-clear. */
#define CCR_WRITABLE 0x0003031Bu

/* The lines that exist, per 32-bit bank; the rest are RAZ/WI. */
static uint32_t impl_mask(uint32_t bank)
{
    const uint32_t first = 32u * bank;

    if (first >= ARMV7M_NIRQ_IMPL) {
        return 0u;
    }
    if (ARMV7M_NIRQ_IMPL - first >= 32u) {
        return 0xFFFFFFFFu;
    }
    return (1u << (ARMV7M_NIRQ_IMPL - first)) - 1u;
}

/* ------------------------------------------------------------------ */
/* Pending and active, as the SHCSR and ICSR show them                 */
/* ------------------------------------------------------------------ */

static uint32_t shcsr_read(const armv7m_cpu_t *c)
{
    uint32_t v = c->shcsr & 0x00070000u;

    v |= armv7m_is_active(c, ARMV7M_EXC_MEMMANAGE) ? (1u << 0) : 0u;
    v |= armv7m_is_active(c, ARMV7M_EXC_BUSFAULT) ? (1u << 1) : 0u;
    v |= armv7m_is_active(c, ARMV7M_EXC_USAGEFAULT) ? (1u << 3) : 0u;
    v |= armv7m_is_active(c, ARMV7M_EXC_SVCALL) ? (1u << 7) : 0u;
    v |= armv7m_is_active(c, ARMV7M_EXC_DEBUGMON) ? (1u << 8) : 0u;
    v |= armv7m_is_active(c, ARMV7M_EXC_PENDSV) ? (1u << 10) : 0u;
    v |= armv7m_is_active(c, ARMV7M_EXC_SYSTICK) ? (1u << 11) : 0u;
    v |= armv7m_is_pending(c, ARMV7M_EXC_USAGEFAULT) ? (1u << 12) : 0u;
    v |= armv7m_is_pending(c, ARMV7M_EXC_MEMMANAGE) ? (1u << 13) : 0u;
    v |= armv7m_is_pending(c, ARMV7M_EXC_BUSFAULT) ? (1u << 14) : 0u;
    v |= armv7m_is_pending(c, ARMV7M_EXC_SVCALL) ? (1u << 15) : 0u;
    return v;
}

static void set_active(armv7m_cpu_t *c, uint32_t exc, bool on)
{
    if (on) {
        c->active[exc >> 5] |= 1u << (exc & 31u);
    } else {
        c->active[exc >> 5] &= ~(1u << (exc & 31u));
    }
}

static void shcsr_write(armv7m_cpu_t *c, uint32_t v)
{
    c->shcsr = v & 0x00070000u;
    set_active(c, ARMV7M_EXC_MEMMANAGE, (v >> 0) & 1u);
    set_active(c, ARMV7M_EXC_BUSFAULT, (v >> 1) & 1u);
    set_active(c, ARMV7M_EXC_USAGEFAULT, (v >> 3) & 1u);
    set_active(c, ARMV7M_EXC_SVCALL, (v >> 7) & 1u);
    set_active(c, ARMV7M_EXC_DEBUGMON, (v >> 8) & 1u);
    set_active(c, ARMV7M_EXC_PENDSV, (v >> 10) & 1u);
    set_active(c, ARMV7M_EXC_SYSTICK, (v >> 11) & 1u);
    armv7m_set_pending(c, ARMV7M_EXC_USAGEFAULT, (v >> 12) & 1u);
    armv7m_set_pending(c, ARMV7M_EXC_MEMMANAGE, (v >> 13) & 1u);
    armv7m_set_pending(c, ARMV7M_EXC_BUSFAULT, (v >> 14) & 1u);
    armv7m_set_pending(c, ARMV7M_EXC_SVCALL, (v >> 15) & 1u);
}

/* The highest-priority pending exception, masks or not: VECTPENDING. */
static uint32_t vectpending(const armv7m_cpu_t *c)
{
    bool preempts;

    return armv7m_pending_exc(c, &preempts);
}

static uint32_t icsr_read(const armv7m_cpu_t *c)
{
    uint32_t v = c->xpsr & ARMV7M_IPSR_MASK;
    bool isr = false;

    v |= armv7m_rettobase(c) << 11;
    v |= (vectpending(c) & 0x1FFu) << 12;
    for (uint32_t i = 0u; i < ARMV7M_NIRQ / 32u; i++) {
        isr = isr || (c->pending[(ARMV7M_EXC_EXTERNAL / 32u) + i] != 0u);
    }
    v |= isr ? (1u << 22) : 0u;
    v |= armv7m_is_pending(c, ARMV7M_EXC_SYSTICK) ? (1u << 26) : 0u;
    v |= armv7m_is_pending(c, ARMV7M_EXC_PENDSV) ? (1u << 28) : 0u;
    v |= armv7m_is_pending(c, ARMV7M_EXC_NMI) ? (1u << 31) : 0u;
    return v;
}

static void icsr_write(armv7m_cpu_t *c, uint32_t v)
{
    if ((v & (1u << 31)) != 0u) {
        armv7m_set_pending(c, ARMV7M_EXC_NMI, true);
    }
    if ((v & (1u << 28)) != 0u) {
        armv7m_set_pending(c, ARMV7M_EXC_PENDSV, true);
    }
    if ((v & (1u << 27)) != 0u) {
        armv7m_set_pending(c, ARMV7M_EXC_PENDSV, false);
    }
    if ((v & (1u << 26)) != 0u) {
        armv7m_set_pending(c, ARMV7M_EXC_SYSTICK, true);
    }
    if ((v & (1u << 25)) != 0u) {
        armv7m_set_pending(c, ARMV7M_EXC_SYSTICK, false);
    }
}

/* ------------------------------------------------------------------ */
/* Bytes: the priorities and the fault status register                 */
/* ------------------------------------------------------------------ */

/* Is this offset one of the byte-addressable ones, and which prio byte. */
static bool prio_byte(uint32_t off, uint32_t *exc)
{
    if (off >= NVIC_IPR && off < NVIC_IPR + ARMV7M_NIRQ_IMPL) {
        *exc = ARMV7M_EXC_EXTERNAL + (off - NVIC_IPR);
        return true;
    }
    if (off >= SCB_SHPR1 && off < SCB_SHPR1 + 12u) {
        *exc = 4u + (off - SCB_SHPR1);
        return true;
    }
    return false;
}

static bool byte_read(armv7m_cpu_t *c, uint32_t off, uint8_t *out)
{
    uint32_t exc;

    if (prio_byte(off, &exc)) {
        /* Reserved system slots read as zero. */
        const bool reserved = (exc >= 7u && exc <= 10u) || exc == 13u;

        *out = reserved ? 0u : c->prio[exc];
        return true;
    }
    if (off >= SCB_CFSR && off < SCB_CFSR + 4u) {
        *out = (uint8_t)(c->cfsr >> (8u * (off - SCB_CFSR)));
        return true;
    }
    return false;
}

static bool byte_write(armv7m_cpu_t *c, uint32_t off, uint8_t v)
{
    uint32_t exc;

    if (prio_byte(off, &exc)) {
        const bool reserved = (exc >= 7u && exc <= 10u) || exc == 13u;

        if (!reserved) {
            c->prio[exc] = v & ARMV7M_PRIO_MASK;
        }
        return true;
    }
    if (off >= SCB_CFSR && off < SCB_CFSR + 4u) {
        c->cfsr &= ~((uint32_t)v << (8u * (off - SCB_CFSR)));
        return true;
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* Words                                                               */
/* ------------------------------------------------------------------ */

static uint32_t nvic_bank(const armv7m_cpu_t *c, const uint32_t *bits,
                          uint32_t bank)
{
    (void)c;
    if (bank >= ARMV7M_NIRQ / 32u) {
        return 0u;
    }
    return bits[bank];
}

/* The pending and active arrays are indexed by exception number; the
 * NVIC banks by interrupt number, 16 below. */
static uint32_t irq_bank(const uint32_t *exc_bits, uint32_t bank)
{
    if (bank >= ARMV7M_NIRQ / 32u) {
        return 0u;
    }
    {
        const uint32_t first = ARMV7M_EXC_EXTERNAL + 32u * bank;
        const uint32_t lo = exc_bits[first >> 5] >> (first & 31u);
        const uint32_t hi = ((first & 31u) != 0u)
                                ? exc_bits[(first >> 5) + 1u] << (32u - (first & 31u))
                                : 0u;

        return lo | hi;
    }
}

static void irq_bank_write(armv7m_cpu_t *c, uint32_t bank, uint32_t v, bool set)
{
    if (bank >= ARMV7M_NIRQ / 32u) {
        return;
    }
    v &= impl_mask(bank);
    for (uint32_t i = 0u; i < 32u; i++) {
        if ((v >> i) & 1u) {
            armv7m_set_pending(c, ARMV7M_EXC_EXTERNAL + 32u * bank + i, set);
        }
    }
}

static bool word_read(armv7m_cpu_t *c, uint32_t off, uint32_t *out)
{
    const uint32_t r = off & ~3u;

    switch (r) {
    case SCS_ICTR:
        *out = ARMV7M_NIRQ / 32u - 1u;
        return true;
    case SCS_ACTLR:
        *out = 0u;
        return true;
    case SYST_CSR: {
        uint32_t v = c->systick_ctrl & 7u;

        /* COUNTFLAG clears when read -- the bit a polling loop needs. */
        if (c->systick_countflag) {
            v |= SYST_CSR_COUNTFLAG;
            c->systick_countflag = false;
        }
        *out = v;
        return true;
    }
    case SYST_RVR:
        *out = c->systick_reload;
        return true;
    case SYST_CVR:
        *out = c->systick_value;
        return true;
    case SYST_CALIB:
        *out = 0u;
        return true;
    case SCB_CPUID:
        *out = CPUID_M7_R0P1;
        return true;
    case SCB_ICSR:
        *out = icsr_read(c);
        return true;
    case SCB_VTOR:
        *out = c->vtor;
        return true;
    case SCB_AIRCR:
        *out = 0xFA050000u | (c->prigroup << 8);
        return true;
    case SCB_SCR:
        *out = c->scr;
        return true;
    case SCB_CCR:
        *out = c->ccr;
        return true;
    case SCB_SHCSR:
        *out = shcsr_read(c);
        return true;
    case SCB_HFSR:
        *out = c->hfsr;
        return true;
    case SCB_DFSR:
    case SCB_AFSR:
        *out = 0u;
        return true;
    case SCB_MMFAR:
        *out = c->mmfar;
        return true;
    case SCB_BFAR:
        *out = c->bfar;
        return true;
    case SCB_CPACR:
        *out = c->cpacr;
        return true;
    case MPU_TYPE:
        *out = ARMV7M_MPU_REGIONS << 8;
        return true;
    case MPU_CTRL:
        *out = c->mpu_ctrl;
        return true;
    case MPU_RNR:
        *out = c->mpu_rnr;
        return true;
    case MPU_RBAR:
    case MPU_RBAR + 8u:
    case MPU_RBAR + 16u:
    case MPU_RBAR + 24u:
        *out = (c->mpu_rbar[c->mpu_rnr] & ~0x1Fu) | c->mpu_rnr;
        return true;
    case MPU_RASR:
    case MPU_RASR + 8u:
    case MPU_RASR + 16u:
    case MPU_RASR + 24u:
        *out = c->mpu_rasr[c->mpu_rnr];
        return true;
    case NVIC_STIR:
        *out = 0u;
        return true;
    case FP_FPCCR:
        *out = c->fpccr;
        return true;
    case FP_FPCAR:
        *out = c->fpcar;
        return true;
    case FP_FPDSCR:
        *out = c->fpdscr;
        return true;
    case FP_MVFR0:
        *out = MVFR0_FPV5_SP;
        return true;
    case FP_MVFR1:
        *out = MVFR1_FPV5_SP;
        return true;
    case FP_MVFR2:
        *out = MVFR2_FPV5_SP;
        return true;
    default:
        break;
    }
    if (r >= NVIC_ISER && r < NVIC_ISER + 0x40u) {
        *out = nvic_bank(c, c->enabled, (r - NVIC_ISER) >> 2);
        return true;
    }
    if (r >= NVIC_ICER && r < NVIC_ICER + 0x40u) {
        *out = nvic_bank(c, c->enabled, (r - NVIC_ICER) >> 2);
        return true;
    }
    if (r >= NVIC_ISPR && r < NVIC_ISPR + 0x40u) {
        *out = irq_bank(c->pending, (r - NVIC_ISPR) >> 2);
        return true;
    }
    if (r >= NVIC_ICPR && r < NVIC_ICPR + 0x40u) {
        *out = irq_bank(c->pending, (r - NVIC_ICPR) >> 2);
        return true;
    }
    if (r >= NVIC_IABR && r < NVIC_IABR + 0x40u) {
        *out = irq_bank(c->active, (r - NVIC_IABR) >> 2);
        return true;
    }
    {
        uint8_t b[4];

        for (uint32_t i = 0u; i < 4u; i++) {
            if (!byte_read(c, r + i, &b[i])) {
                *out = 0u; /* reserved and unimplemented: RAZ */
                return true;
            }
        }
        *out = (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) |
               ((uint32_t)b[3] << 24);
        return true;
    }
}

static void mpu_region_write(armv7m_cpu_t *c, uint32_t rbar)
{
    if ((rbar & 0x10u) != 0u) { /* VALID: the region field selects */
        c->mpu_rnr = rbar & (ARMV7M_MPU_REGIONS - 1u);
    }
    c->mpu_rbar[c->mpu_rnr] = rbar & ~0x1Fu;
    c->jit_gen++;
}

static bool word_write(armv7m_cpu_t *c, uint32_t off, uint32_t v)
{
    const uint32_t r = off & ~3u;

    switch (r) {
    case SYST_CSR:
        c->systick_ctrl = v & 7u;
        return true;
    case SYST_RVR:
        c->systick_reload = v & 0x00FFFFFFu;
        return true;
    case SYST_CVR:
        /* Any write clears the counter and COUNTFLAG. */
        c->systick_value = 0u;
        c->systick_countflag = false;
        return true;
    case SCB_ICSR:
        icsr_write(c, v);
        return true;
    case SCB_VTOR:
        c->vtor = v & 0xFFFFFF80u;
        return true;
    case SCB_AIRCR:
        if ((v >> 16) == 0x05FAu) {
            c->prigroup = (v >> 8) & 7u;
            if ((v & (1u << 2)) != 0u) {
                c->state = EMU_STATE_HALTED; /* SYSRESETREQ */
            }
        }
        return true;
    case SCB_SCR:
        c->scr = v & 0x16u;
        return true;
    case SCB_CCR: {
        const uint32_t was = c->ccr;

        c->ccr = (c->ccr & ~CCR_WRITABLE) | (v & CCR_WRITABLE) | (1u << 9) |
                 (1u << 18);
        /*
         * UNALIGN_TRP decides whether a translated load may be inlined,
         * and DIV_0_TRP whether a translated divide may be a divide.
         */
        if (((was ^ c->ccr) & ((1u << 3) | (1u << 4))) != 0u) {
            c->jit_gen++;
        }
        return true;
    }
    case SCB_SHCSR:
        shcsr_write(c, v);
        return true;
    case SCB_HFSR:
        c->hfsr &= ~v;
        return true;
    case SCB_MMFAR:
        c->mmfar = v;
        return true;
    case SCB_BFAR:
        c->bfar = v;
        return true;
    case SCB_CPACR:
        /* CP10 and CP11 only; the rest of the field is RAZ/WI. */
        c->cpacr = v & 0x00F00000u;
        return true;
    /*
     * A translated block records that its instructions could be fetched,
     * under the MPU as it stood. Writing any of these may change that,
     * so each moves the generation and every block goes. On the write,
     * not on a flag: a region can be redrawn while the MPU stays
     * enabled, and watching the enable bit would miss it.
     */
    case MPU_CTRL:
        c->mpu_ctrl = v & 7u;
        c->jit_gen++;
        return true;
    case MPU_RNR:
        c->mpu_rnr = v & (ARMV7M_MPU_REGIONS - 1u);
        return true;
    case MPU_RBAR:
    case MPU_RBAR + 8u:
    case MPU_RBAR + 16u:
    case MPU_RBAR + 24u:
        mpu_region_write(c, v);
        return true;
    case MPU_RASR:
    case MPU_RASR + 8u:
    case MPU_RASR + 16u:
    case MPU_RASR + 24u:
        c->mpu_rasr[c->mpu_rnr] = v & 0x173FFF3Fu;
        c->jit_gen++;
        return true;
    case NVIC_STIR:
        if ((v & 0x1FFu) < ARMV7M_NIRQ_IMPL) {
            armv7m_set_pending(c, ARMV7M_EXC_EXTERNAL + (v & 0x1FFu), true);
        }
        return true;
    case FP_FPCCR:
        c->fpccr = (c->fpccr & ~0xC0000000u) | (v & 0xC0000000u);
        return true;
    case FP_FPCAR:
        c->fpcar = v & ~7u;
        return true;
    case FP_FPDSCR:
        c->fpdscr = v & 0x07C00000u;
        return true;
    default:
        break;
    }
    if (r >= NVIC_ISER && r < NVIC_ISER + 0x40u) {
        const uint32_t bank = (r - NVIC_ISER) >> 2;

        if (bank < ARMV7M_NIRQ / 32u) {
            c->enabled[bank] |= v & impl_mask(bank);
        }
        return true;
    }
    if (r >= NVIC_ICER && r < NVIC_ICER + 0x40u) {
        const uint32_t bank = (r - NVIC_ICER) >> 2;

        if (bank < ARMV7M_NIRQ / 32u) {
            c->enabled[bank] &= ~v;
        }
        return true;
    }
    if (r >= NVIC_ISPR && r < NVIC_ISPR + 0x40u) {
        irq_bank_write(c, (r - NVIC_ISPR) >> 2, v, true);
        return true;
    }
    if (r >= NVIC_ICPR && r < NVIC_ICPR + 0x40u) {
        irq_bank_write(c, (r - NVIC_ICPR) >> 2, v, false);
        return true;
    }
    {
        bool any = false;

        for (uint32_t i = 0u; i < 4u; i++) {
            any = byte_write(c, r + i, (uint8_t)(v >> (8u * i))) || any;
        }
        (void)any;
        return true; /* reserved: WI */
    }
}

/* ------------------------------------------------------------------ */
/* The device                                                          */
/* ------------------------------------------------------------------ */

static emu_fault_t scs_read(void *ctx, uint32_t off, uint32_t size,
                            uint32_t *out)
{
    armv7m_cpu_t *const c = (armv7m_cpu_t *)ctx;

    if (!c->access_priv) {
        return EMU_FAULT_LOAD;
    }
    if (size == 4u) {
        return word_read(c, off, out) ? EMU_FAULT_NONE : EMU_FAULT_LOAD;
    }
    {
        uint32_t v = 0u;

        for (uint32_t i = 0u; i < size; i++) {
            uint8_t b = 0u;

            if (!byte_read(c, off + i, &b)) {
                return EMU_FAULT_LOAD; /* word-only register */
            }
            v |= (uint32_t)b << (8u * i);
        }
        *out = v;
        return EMU_FAULT_NONE;
    }
}

static emu_fault_t scs_write(void *ctx, uint32_t off, uint32_t size,
                             uint32_t val)
{
    armv7m_cpu_t *const c = (armv7m_cpu_t *)ctx;

    /* Enables, priorities, pending bits, the masks' neighbours: any of
     * these can make an exception takeable. See irq_maybe. */
    c->irq_maybe = true;
    if (!c->access_priv) {
        /* STIR is the one register an unprivileged guest may be given. */
        if (!(off == NVIC_STIR && size == 4u && (c->ccr & 2u) != 0u)) {
            return EMU_FAULT_STORE;
        }
    }
    if (size == 4u) {
        return word_write(c, off, val) ? EMU_FAULT_NONE : EMU_FAULT_STORE;
    }
    for (uint32_t i = 0u; i < size; i++) {
        if (!byte_write(c, off + i, (uint8_t)(val >> (8u * i)))) {
            return EMU_FAULT_STORE;
        }
    }
    return EMU_FAULT_NONE;
}

const struct emu_dev_ops armv7m_scs_ops = {
    .read = scs_read,
    .write = scs_write,
};

/* ------------------------------------------------------------------ */
/* SysTick and the interrupt lines                                     */
/* ------------------------------------------------------------------ */

/*
 * The 24-bit counter, decremented once per retired instruction. It
 * reloads from RVR on the cycle after it reaches zero and pends SysTick
 * if TICKINT is set.
 */
void armv7m_systick_tick(armv7m_cpu_t *c, uint32_t insns)
{
    if ((c->systick_ctrl & SYST_CSR_ENABLE) == 0u) {
        return;
    }
    while (insns-- != 0u) {
        if (c->systick_value == 0u) {
            c->systick_value = c->systick_reload;
            continue;
        }
        c->systick_value--;
        if (c->systick_value == 0u) {
            c->systick_countflag = true;
            if ((c->systick_ctrl & SYST_CSR_TICKINT) != 0u) {
                armv7m_set_pending(c, ARMV7M_EXC_SYSTICK, true);
            }
        }
    }
}

/*
 * A device's interrupt line. Level-sensitive, as the NVIC treats every
 * external input: asserting it pends the interrupt, and if it is still
 * asserted when the handler returns it pends again.
 */
void armv7m_set_irq(armv7m_cpu_t *c, uint32_t source, bool level)
{
    if (source >= ARMV7M_NIRQ_IMPL) {
        return;
    }
    if (level) {
        c->line[source >> 5] |= 1u << (source & 31u);
        armv7m_set_pending(c, ARMV7M_EXC_EXTERNAL + source, true);
    } else {
        c->line[source >> 5] &= ~(1u << (source & 31u));
    }
}
