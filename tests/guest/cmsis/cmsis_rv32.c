/* SPDX-License-Identifier: Apache-2.0 */
/*
 * cmsis_rv32.c - the parts of the CMSIS-Core shim that need state.
 *
 * The trap handler here is what replaces the Cortex-M vector table. On ARM
 * the hardware reads a function pointer out of a table and branches to it;
 * here one handler runs for every interrupt and dispatches on what the
 * APLIC reports.
 */

#include "cmsis_rv32.h"

DWT_Type_shadow cmsis_rv32_dwt;

uint32_t SystemCoreClock = 180000000u;

/* One slot per APLIC source, which is one per NVIC line. */
#define CMSIS_RV32_SOURCES 128u
static void (*g_handlers[CMSIS_RV32_SOURCES])(void);

/* SysTick period, in mtime ticks. Zero when SysTick is not running. */
static uint32_t g_systick_period;

uint32_t cmsis_rv32_cycles(void)
{
    const uint32_t c = CMSIS_RV32_CSRR("cycle");
    cmsis_rv32_dwt.CYCCNT = c;
    return c;
}

__attribute__((weak)) void SysTick_Handler(void) { }

void NVIC_SetHandler(IRQn_Type irqn, void (*handler)(void))
{
    if ((int)irqn >= 0 && (uint32_t)irqn < CMSIS_RV32_SOURCES) {
        g_handlers[irqn] = handler;
    }
}

void NVIC_SystemReset(void)
{
    /* Nothing here can reset the board; stopping is the honest answer, and
     * it leaves the state intact for a debugger. */
    __disable_irq();
    for (;;) {
    }
}

/* ------------------------------------------------------------------ */
/* SysTick                                                             */
/* ------------------------------------------------------------------ */

static void systick_rearm(void)
{
    /*
     * Read low, high, then low again: mtime is 64-bit and the emulator
     * advances it underneath, so a carry between the two halves would
     * otherwise produce a value that never existed.
     */
    uint32_t hi, lo;
    do {
        hi = CLINT_MTIME_HI_;
        lo = CLINT_MTIME_LO_;
    } while (hi != CLINT_MTIME_HI_);

    uint64_t next = (((uint64_t)hi << 32) | lo) + g_systick_period;

    /*
     * Write the high half to all-ones first. mtimecmp is compared as a
     * whole, so lowering the low half before raising the high one can leave
     * a moment where the comparison is already satisfied and an interrupt
     * fires immediately.
     */
    CLINT_MTIMECMP_HI_ = 0xFFFFFFFFu;
    CLINT_MTIMECMP_LO_ = (uint32_t)next;
    CLINT_MTIMECMP_HI_ = (uint32_t)(next >> 32);
}

uint32_t SysTick_Config(uint32_t ticks)
{
    if (ticks == 0u) {
        return 1u;
    }
    /*
     * `ticks` is a countdown at the core clock; mtime runs at 1 MHz. HAL
     * asks for SystemCoreClock/1000, so this recovers the millisecond it
     * meant rather than the number of core cycles it counted.
     */
    const uint32_t per_us = SystemCoreClock / 1000000u;
    g_systick_period = (per_us == 0u) ? ticks : (ticks / per_us);
    if (g_systick_period == 0u) {
        g_systick_period = 1u;
    }

    systick_rearm();
    CMSIS_RV32_CSRS("mie", MIE_MTIE_);
    return 0u;
}

/* ------------------------------------------------------------------ */
/* The trap handler                                                    */
/* ------------------------------------------------------------------ */

/*
 * What the vector table used to do.
 *
 * The order for an external interrupt is deliberate: topi names the source
 * without clearing anything, so the driver's handler runs -- and clears its
 * peripheral's flag -- before claimi clears the pending bit. Clearing
 * pending is what tells the firmware the guest is done and the host
 * interrupt line may be unmasked, so claiming first would unmask a line
 * whose source is still asserted and earn a second interrupt for every real
 * one.
 */
__attribute__((interrupt("machine"), aligned(4), used))
static void cmsis_rv32_trap(void)
{
    const uint32_t cause = CMSIS_RV32_CSRR("mcause");

    if ((cause & 0x80000000u) == 0u) {
        /* An exception, not an interrupt. Nothing here can meaningfully
         * recover, and looping leaves mepc and mtval for a debugger. */
        for (;;) {
        }
    }

    switch (cause & 0xFFu) {
    case 7u:                            /* machine timer */
        systick_rearm();
        SysTick_Handler();
        break;

    case 11u: {                         /* machine external */
        const uint32_t source = APLIC_TOPI_ >> 16;
        if (source != 0u && source < CMSIS_RV32_SOURCES &&
            g_handlers[source] != 0) {
            g_handlers[source]();
        }
        (void)APLIC_CLAIMI_;
        break;
    }

    default:
        break;
    }
}

void cmsis_rv32_init(void)
{
    CMSIS_RV32_CSRW("mtvec", (uint32_t)&cmsis_rv32_trap);

    for (uint32_t i = 0; i < CMSIS_RV32_SOURCES; i++) {
        g_handlers[i] = 0;
    }

    /* Bring the interrupt controller up: domain enabled, delivery on. */
    APLIC_DOMAINCFG_ = 0x100u;
    APLIC_IDELIVERY_ = 1u;

    /* Arm the two causes the shim serves. Global enable stays with the
     * caller, exactly as __enable_irq does on Cortex-M. */
    CMSIS_RV32_CSRS("mie", MIE_MEIE_);

    cmsis_rv32_dwt.CTRL = DWT_CTRL_CYCCNTENA_Msk;
}
