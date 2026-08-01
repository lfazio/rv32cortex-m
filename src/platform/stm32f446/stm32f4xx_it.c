/* SPDX-License-Identifier: Apache-2.0 */
/*
 * stm32f4xx_it.c - Cortex-M exception handlers.
 *
 * ST's startup file declares every handler weak and aliased to a default
 * infinite loop, so only the ones that need real behaviour are defined
 * here. The fault handlers are kept as deliberate traps: a fault in the
 * emulator itself is a bug worth stopping on, not something to recover
 * from, and stopping leaves the state intact for a debugger.
 */

#include "stm32f4xx_hal.h"

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

void SVC_Handler(void)
{
}

void DebugMon_Handler(void)
{
}

void PendSV_Handler(void)
{
}

void SysTick_Handler(void)
{
    /* Drives HAL_GetTick(), which the HAL's timeout loops depend on. */
    HAL_IncTick();
}
