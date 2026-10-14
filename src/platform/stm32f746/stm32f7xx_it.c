/* SPDX-License-Identifier: Apache-2.0 */
/*
 * stm32f7xx_it.c - Cortex-M exception handlers.
 *
 * ST's startup file declares every handler weak and aliased to a default
 * infinite loop, so only the ones that need real behaviour are defined
 * here. The fault handlers are kept as deliberate traps: a fault in the
 * emulator itself is a bug worth stopping on, not something to recover
 * from, and stopping leaves the state intact for a debugger.
 */

#include "stm32f7xx_hal.h"

#include "FreeRTOS.h"
#include "task.h"

/* The port's tick, exported under this name by every port in use here
 * (cmake/freertos.cmake renames the ARMv8-M port's). */
void xPortSysTickHandler(void);

void NMI_Handler(void)
{
    for (;;) {
    }
}

void HardFault_Handler(void)
{
    /*
     * Most likely cause while bringing up a guest: the passthrough window
     * let a guest access reach an address the ARM bus rejects. The bus
     * permission table in main.c is what narrows that down.
     */
    for (;;) {
    }
}

void MemManage_Handler(void)
{
    for (;;) {
    }
}

void BusFault_Handler(void)
{
    for (;;) {
    }
}

void UsageFault_Handler(void)
{
    for (;;) {
    }
}

/*
 * SVC_Handler and PendSV_Handler are FreeRTOS's own (FreeRTOSConfig.h
 * maps the port's names onto them, or the ARMv8-M port defines them), and
 * must be: V11 checks the vector table points straight at them.
 */

void DebugMon_Handler(void)
{
}

#if EMU_PC_PROFILE
/*
 * A PC profile, sampled by the tick: -DEMU_PC_PROFILE=<samples> keeps the
 * interrupted pc of the first that many ticks, at 1 kHz. Read
 * g_pc_profile_n words of g_pc_profile out over the probe after a run and
 * bucket them by symbol -- it says which host code is hot, which a cycle
 * count cannot. Compiled out by default.
 */
__attribute__((used)) uint32_t g_pc_profile[EMU_PC_PROFILE];
__attribute__((used)) volatile uint32_t g_pc_profile_n;

__attribute__((used)) void pc_profile_take(const uint32_t *frame)
{
    const uint32_t n = g_pc_profile_n;

    if (n < (uint32_t)EMU_PC_PROFILE) {
        g_pc_profile[n] = frame[6]; /* the stacked pc */
        g_pc_profile_n = n + 1u;
    }
}

__attribute__((used)) void systick_body(void);

/* Find the exception frame on whichever stack it was pushed to. */
__attribute__((naked)) void SysTick_Handler(void)
{
    __asm volatile("tst lr, #4\n"
                   "ite eq\n"
                   "mrseq r0, msp\n"
                   "mrsne r0, psp\n"
                   "push {r4, lr}\n"
                   "bl pc_profile_take\n"
                   "pop {r4, lr}\n"
                   "b systick_body\n");
}

void systick_body(void)
#else
void SysTick_Handler(void)
#endif
{
    /*
     * Shared: the HAL's millisecond, which its timeout loops depend on,
     * and the kernel's tick once the scheduler runs. Both at 1 kHz --
     * configTICK_RATE_HZ is chosen so.
     */
    HAL_IncTick();
    if (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED) {
        xPortSysTickHandler();
    }
}
