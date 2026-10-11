/* SPDX-License-Identifier: Apache-2.0 */
/*
 * stm32n6xx_it.c - Cortex-M exception handlers.
 *
 * ST's startup file declares every handler weak and aliased to a default
 * infinite loop, so only the ones that need real behaviour are defined
 * here. The fault handlers are kept as deliberate traps: a fault in the
 * emulator itself is a bug worth stopping on, not something to recover
 * from, and stopping leaves the state intact for a debugger.
 */

#include "stm32n6xx_hal.h"

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

void SysTick_Handler(void)
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
