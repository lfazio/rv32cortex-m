/* SPDX-License-Identifier: Apache-2.0 */
/*
 * cmsis_rv32.h - the CMSIS-Core API, for guest code running on RV32.
 *
 * Vendor peripheral drivers are written against two things: a register map,
 * and CMSIS-Core. The register map needs no help here -- the passthrough
 * window means `GPIOB->ODR` is the same address it always was -- but
 * CMSIS-Core is Cortex-M architecture, and none of it survives the move to
 * RISC-V. This supplies the same names on top of what does exist.
 *
 *   NVIC_*        -> the APLIC
 *   SysTick       -> the ACLINT timer
 *   DWT->CYCCNT   -> the cycle CSR
 *   __disable_irq -> mstatus.MIE
 *   __DSB / __ISB -> fence / fence.i
 *
 * The NVIC mapping is direct because an APLIC source number *is* the host's
 * interrupt number: `NVIC_EnableIRQ(TIM6_DAC_IRQn)` enables source 54,
 * which is the line the firmware bridges for TIM6. A driver keeps the
 * constant ST's headers gave it, and nothing translates between two
 * numbering spaces.
 *
 * One addition CMSIS has no equivalent for. On Cortex-M the vector table
 * connects an interrupt to `TIM6_DAC_IRQHandler` by linker symbol; there is
 * no vector table here, so a handler is registered with NVIC_SetHandler.
 * That is the one line a ported driver has to gain.
 */
#ifndef CMSIS_RV32_H
#define CMSIS_RV32_H

#include <stdint.h>

#ifndef __IO
#  define __IO volatile
#endif
#ifndef __I
#  define __I  volatile const
#endif
#ifndef __O
#  define __O  volatile
#endif

#define __STATIC_INLINE static inline
#define __ASM           __asm__
#define __INLINE        inline
#define __WEAK          __attribute__((weak))

/* Cortex-M4 has four priority bits; keeping the value keeps HAL's
 * NVIC_EncodePriority arithmetic meaningful. */
#ifndef __NVIC_PRIO_BITS
#  define __NVIC_PRIO_BITS 4u
#endif

/* ------------------------------------------------------------------ */
/* Devices                                                             */
/* ------------------------------------------------------------------ */

#define CMSIS_RV32_APLIC_BASE   0x0C000000u
#define CMSIS_RV32_CLINT_BASE   0x02000000u

#define APLIC_(off)         (*(__IO uint32_t *)(CMSIS_RV32_APLIC_BASE + (off)))
#define APLIC_DOMAINCFG_    APLIC_(0x0000u)
#define APLIC_SOURCECFG_(i) APLIC_(0x0004u + 4u * ((uint32_t)(i) - 1u))
#define APLIC_SETIPNUM_     APLIC_(0x1CDCu)
#define APLIC_CLRIPNUM_     APLIC_(0x1DDCu)
#define APLIC_SETIENUM_     APLIC_(0x1EDCu)
#define APLIC_CLRIENUM_     APLIC_(0x1FDCu)
#define APLIC_SETIP_(k)     APLIC_(0x1C00u + 4u * (k))
#define APLIC_TARGET_(i)    APLIC_(0x3004u + 4u * ((uint32_t)(i) - 1u))
#define APLIC_IDELIVERY_    APLIC_(0x4000u)
#define APLIC_TOPI_         APLIC_(0x4018u)
#define APLIC_CLAIMI_       APLIC_(0x401Cu)

#define APLIC_SM_EDGE_RISE_ 4u

/* ACLINT MTIMER: mtimecmp at its base, mtime 0x7FF8 beyond it. */
#define CLINT_MTIMECMP_LO_  (*(__IO uint32_t *)(CMSIS_RV32_CLINT_BASE + 0x4000u))
#define CLINT_MTIMECMP_HI_  (*(__IO uint32_t *)(CMSIS_RV32_CLINT_BASE + 0x4004u))
#define CLINT_MTIME_LO_     (*(__IO uint32_t *)(CMSIS_RV32_CLINT_BASE + 0xBFF8u))
#define CLINT_MTIME_HI_     (*(__IO uint32_t *)(CMSIS_RV32_CLINT_BASE + 0xBFFCu))

/* ------------------------------------------------------------------ */
/* CSR access                                                          */
/* ------------------------------------------------------------------ */

#define CMSIS_RV32_CSRR(name) ({                        \
    uint32_t v_;                                        \
    __asm__ volatile ("csrr %0, " name : "=r"(v_));     \
    v_; })
#define CMSIS_RV32_CSRW(name, v) \
    __asm__ volatile ("csrw " name ", %0" :: "r"((uint32_t)(v)))
#define CMSIS_RV32_CSRS(name, v) \
    __asm__ volatile ("csrs " name ", %0" :: "r"((uint32_t)(v)))
#define CMSIS_RV32_CSRC(name, v) \
    __asm__ volatile ("csrc " name ", %0" :: "r"((uint32_t)(v)))

#define MSTATUS_MIE_    (1u << 3)
#define MIE_MTIE_       (1u << 7)
#define MIE_MEIE_       (1u << 11)

/* ------------------------------------------------------------------ */
/* Intrinsics                                                          */
/* ------------------------------------------------------------------ */

__STATIC_INLINE void __enable_irq(void)  { CMSIS_RV32_CSRS("mstatus", MSTATUS_MIE_); }
__STATIC_INLINE void __disable_irq(void) { CMSIS_RV32_CSRC("mstatus", MSTATUS_MIE_); }

__STATIC_INLINE uint32_t __get_PRIMASK(void)
{
    return (CMSIS_RV32_CSRR("mstatus") & MSTATUS_MIE_) ? 0u : 1u;
}

__STATIC_INLINE void __set_PRIMASK(uint32_t p)
{
    if (p != 0u) { __disable_irq(); } else { __enable_irq(); }
}

/*
 * FENCE orders memory; FENCE.I orders instruction fetch against it. DSB and
 * ISB are the closest equivalents, and DMB is a plain FENCE. None of them is
 * strictly required on this single-hart in-order core, but a driver that
 * omits them elsewhere is a driver that breaks elsewhere.
 */
__STATIC_INLINE void __DSB(void) { __asm__ volatile ("fence" ::: "memory"); }
__STATIC_INLINE void __DMB(void) { __asm__ volatile ("fence" ::: "memory"); }
__STATIC_INLINE void __ISB(void) { __asm__ volatile ("fence.i" ::: "memory"); }
__STATIC_INLINE void __NOP(void) { __asm__ volatile ("nop"); }
__STATIC_INLINE void __WFI(void) { __asm__ volatile ("wfi"); }
__STATIC_INLINE void __WFE(void) { __asm__ volatile ("nop"); }
__STATIC_INLINE void __SEV(void) { __asm__ volatile ("nop"); }

__STATIC_INLINE uint32_t __CLZ(uint32_t v)
{
    return (v == 0u) ? 32u : (uint32_t)__builtin_clz(v);
}

__STATIC_INLINE uint32_t __REV(uint32_t v)
{
    return __builtin_bswap32(v);
}

__STATIC_INLINE uint32_t __RBIT(uint32_t v)
{
    /* Zbb has no bit reverse; this is the standard shift-and-mask ladder. */
    v = ((v & 0xAAAAAAAAu) >> 1) | ((v & 0x55555555u) << 1);
    v = ((v & 0xCCCCCCCCu) >> 2) | ((v & 0x33333333u) << 2);
    v = ((v & 0xF0F0F0F0u) >> 4) | ((v & 0x0F0F0F0Fu) << 4);
    return __builtin_bswap32(v);
}

/*
 * Exclusive access. This one is not an approximation: ARM's load-exclusive
 * and store-exclusive are what RISC-V's A extension calls LR and SC, down
 * to the return convention -- STREX and SC.W both yield zero on success.
 * The reservation granule and the rules about what breaks it differ, but no
 * HAL use depends on that: they are all a read-modify-write of one word.
 */
__STATIC_INLINE uint32_t __LDREXW(volatile uint32_t *addr)
{
    uint32_t v;
    __asm__ volatile ("lr.w %0, (%1)" : "=r"(v) : "r"(addr) : "memory");
    return v;
}

__STATIC_INLINE uint32_t __STREXW(uint32_t value, volatile uint32_t *addr)
{
    uint32_t fail;
    __asm__ volatile ("sc.w %0, %2, (%1)"
                      : "=&r"(fail) : "r"(addr), "r"(value) : "memory");
    return fail;
}

__STATIC_INLINE uint8_t __LDREXB(volatile uint8_t *addr)
{
    /* No byte-wide LR: read the containing word and extract. Sufficient
     * because every HAL user of this pairs it with a STREXB on the same
     * byte and retries on failure. */
    volatile uint32_t *w = (volatile uint32_t *)((uintptr_t)addr & ~3u);
    const uint32_t sh = ((uintptr_t)addr & 3u) * 8u;
    return (uint8_t)(__LDREXW(w) >> sh);
}

__STATIC_INLINE uint32_t __STREXB(uint8_t value, volatile uint8_t *addr)
{
    volatile uint32_t *w = (volatile uint32_t *)((uintptr_t)addr & ~3u);
    const uint32_t sh = ((uintptr_t)addr & 3u) * 8u;
    uint32_t cur = __LDREXW(w);
    cur = (cur & ~(0xFFu << sh)) | ((uint32_t)value << sh);
    return __STREXW(cur, w);
}

__STATIC_INLINE void __CLREX(void)
{
    /* An SC to a location no LR reserved drops the reservation. */
    uint32_t scratch = 0u;
    (void)__STREXW(0u, &scratch);
}

/* ------------------------------------------------------------------ */
/* Cycle counter                                                       */
/* ------------------------------------------------------------------ */

/*
 * DWT's cycle counter, as the `cycle` CSR. CYCCNT is presented as a
 * function-like macro rather than a struct field because there is no
 * peripheral to point at -- code doing `DWT->CYCCNT` reads it unchanged,
 * and code enabling the trace unit finds the enables harmlessly writable.
 */
typedef struct {
    __IO uint32_t CTRL;
    __IO uint32_t CYCCNT;
} DWT_Type_shadow;

extern DWT_Type_shadow cmsis_rv32_dwt;
#define DWT (&cmsis_rv32_dwt)
#define DWT_CTRL_CYCCNTENA_Msk 1u

/* Reading the shadow updates it from the CSR first; see cmsis_rv32.c. */
uint32_t cmsis_rv32_cycles(void);

/* ------------------------------------------------------------------ */
/* NVIC, on the APLIC                                                  */
/* ------------------------------------------------------------------ */

/*
 * IRQn_Type comes from the vendor header, where negative values name
 * Cortex-M system exceptions. Those have no APLIC source, so they are
 * ignored rather than mapped to something arbitrary.
 */
#ifndef CMSIS_RV32_HAS_IRQN
typedef int IRQn_Type;
#endif

__STATIC_INLINE void NVIC_EnableIRQ(IRQn_Type irqn)
{
    if ((int)irqn < 0) {
        return;
    }
    /* A source must be active before it can be pending or enabled. */
    APLIC_SOURCECFG_(irqn) = APLIC_SM_EDGE_RISE_;
    APLIC_SETIENUM_ = (uint32_t)irqn;
}

__STATIC_INLINE void NVIC_DisableIRQ(IRQn_Type irqn)
{
    if ((int)irqn >= 0) {
        APLIC_CLRIENUM_ = (uint32_t)irqn;
    }
}

/*
 * ARM and the APLIC agree that a lower number is a higher priority, so the
 * only adjustment is the offset: IPRIO zero is reserved, so ARM's highest
 * priority becomes one.
 */
__STATIC_INLINE void NVIC_SetPriority(IRQn_Type irqn, uint32_t priority)
{
    if ((int)irqn < 0) {
        return;
    }
    /*
     * Activate the source here too, not only in NVIC_EnableIRQ.
     *
     * The APLIC reports target[i] as zero while source i is inactive, which
     * is what the specification asks for -- but CMSIS callers set a
     * priority and *then* enable, HAL_NVIC_SetPriority before
     * HAL_NVIC_EnableIRQ being the usual pair. Leaving activation to the
     * enable would mean the priority written first read back as zero and
     * was quietly lost.
     */
    APLIC_SOURCECFG_(irqn) = APLIC_SM_EDGE_RISE_;
    APLIC_TARGET_(irqn) = (priority & 0xFFu) + 1u;
}

__STATIC_INLINE uint32_t NVIC_GetPriority(IRQn_Type irqn)
{
    if ((int)irqn < 0) {
        return 0u;
    }
    const uint32_t p = APLIC_TARGET_(irqn) & 0xFFu;
    return (p == 0u) ? 0u : (p - 1u);
}

__STATIC_INLINE void NVIC_SetPendingIRQ(IRQn_Type irqn)
{
    if ((int)irqn >= 0) {
        APLIC_SETIPNUM_ = (uint32_t)irqn;
    }
}

__STATIC_INLINE void NVIC_ClearPendingIRQ(IRQn_Type irqn)
{
    if ((int)irqn >= 0) {
        APLIC_CLRIPNUM_ = (uint32_t)irqn;
    }
}

__STATIC_INLINE uint32_t NVIC_GetPendingIRQ(IRQn_Type irqn)
{
    if ((int)irqn < 0) {
        return 0u;
    }
    return (APLIC_SETIP_((uint32_t)irqn / 32u) >> ((uint32_t)irqn % 32u)) & 1u;
}

/* Priority grouping is a Cortex-M concept; the APLIC has one flat space. */
__STATIC_INLINE void NVIC_SetPriorityGrouping(uint32_t g) { (void)g; }
__STATIC_INLINE uint32_t NVIC_GetPriorityGrouping(void) { return 0u; }

__STATIC_INLINE uint32_t NVIC_EncodePriority(uint32_t group, uint32_t pre,
                                             uint32_t sub)
{
    (void)group;
    (void)sub;
    return pre;
}

void NVIC_SystemReset(void);

/*
 * The one thing CMSIS cannot express here. On Cortex-M the vector table
 * binds an interrupt to a handler by linker symbol; with no vector table,
 * the binding is made at run time.
 */
void NVIC_SetHandler(IRQn_Type irqn, void (*handler)(void));

/* ------------------------------------------------------------------ */
/* SysTick, on the ACLINT timer                                        */
/* ------------------------------------------------------------------ */

/*
 * SysTick counts down at the core clock; mtime counts up at 1 MHz. So the
 * reload value is converted once, here, using the clock the caller believes
 * it has -- which is what SystemCoreClock is for.
 */
extern uint32_t SystemCoreClock;

uint32_t SysTick_Config(uint32_t ticks);

/* Called from the trap handler on a timer interrupt; weak so the firmware
 * being ported keeps its own definition. */
void SysTick_Handler(void);

/* ------------------------------------------------------------------ */
/* Entry                                                               */
/* ------------------------------------------------------------------ */

/*
 * Install the trap handler and bring the interrupt controller up. Call
 * before anything that enables an interrupt -- HAL_Init is the natural
 * place, and it is what SystemInit did on the ARM side.
 */
void cmsis_rv32_init(void);

#endif /* CMSIS_RV32_H */
