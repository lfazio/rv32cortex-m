/* SPDX-License-Identifier: Apache-2.0 */
/*
 * portmacro.h - FreeRTOS SMP on POSIX threads: one host thread per
 * FreeRTOS task, as in the kernel's own POSIX port, and up to
 * configNUMBER_OF_CORES of them running at once. See port.c, and
 * docs/os.md for why this exists: the kernel's POSIX port is single-core
 * in every release to V11.3.1.
 *
 * The types are the POSIX port's, so nothing compiled against one sees
 * a difference.
 */
#ifndef PORTMACRO_H
#define PORTMACRO_H

#include <limits.h>
#include <stdint.h>

#define portCHAR char
#define portFLOAT float
#define portDOUBLE double
#define portLONG long
#define portSHORT short
#define portSTACK_TYPE unsigned long
#define portBASE_TYPE long
#define portPOINTER_SIZE_TYPE intptr_t

typedef portSTACK_TYPE StackType_t;
typedef long BaseType_t;
typedef unsigned long UBaseType_t;
typedef unsigned long TickType_t;

#define portMAX_DELAY ((TickType_t)ULONG_MAX)
#define portTICK_TYPE_IS_ATOMIC 1

#define portSTACK_GROWTH (-1)
#define portHAS_STACK_OVERFLOW_CHECKING (1)
#define portTICK_PERIOD_MS ((TickType_t)1000 / configTICK_RATE_HZ)
#define portTICK_RATE_MICROSECONDS ((TickType_t)1000000 / configTICK_RATE_HZ)
#define portBYTE_ALIGNMENT 8

/* ------------------------------------------------------------------ */
/* Yields                                                              */
/* ------------------------------------------------------------------ */

void vPortYield(void);
void vPortYieldFromISR(void);
void vPortYieldCore(BaseType_t xCoreID);

#define portYIELD() vPortYield()
#define portEND_SWITCHING_ISR(xSwitchRequired)                                 \
    do {                                                                       \
        if ((xSwitchRequired) != pdFALSE) {                                    \
            vPortYieldFromISR();                                               \
        }                                                                      \
    } while (0)
#define portYIELD_FROM_ISR(x) portEND_SWITCHING_ISR(x)
#define portYIELD_CORE(x) vPortYieldCore(x)

/* ------------------------------------------------------------------ */
/* Interrupts: a thread's signal mask                                  */
/* ------------------------------------------------------------------ */

void vPortDisableInterrupts(void);
void vPortEnableInterrupts(void);
UBaseType_t xPortSetInterruptMask(void);
void vPortClearInterruptMask(UBaseType_t uxWasMasked);

#define portDISABLE_INTERRUPTS() vPortDisableInterrupts()
#define portENABLE_INTERRUPTS() vPortEnableInterrupts()
#define portSET_INTERRUPT_MASK() xPortSetInterruptMask()
#define portCLEAR_INTERRUPT_MASK(x) vPortClearInterruptMask(x)
#define portSET_INTERRUPT_MASK_FROM_ISR() xPortSetInterruptMask()
#define portCLEAR_INTERRUPT_MASK_FROM_ISR(x) vPortClearInterruptMask(x)

/* ------------------------------------------------------------------ */
/* SMP                                                                 */
/* ------------------------------------------------------------------ */

#define portCRITICAL_NESTING_IN_TCB 1
#define portENTER_CRITICAL() vTaskEnterCritical()
#define portEXIT_CRITICAL() vTaskExitCritical()
#define portENTER_CRITICAL_FROM_ISR() vTaskEnterCriticalFromISR()
#define portEXIT_CRITICAL_FROM_ISR(x) vTaskExitCriticalFromISR(x)

BaseType_t xPortGetCoreID(void);
BaseType_t xPortCheckIfInISR(void);
void vPortRecursiveLock(unsigned uxLock, BaseType_t xAcquire);

#define portGET_CORE_ID() xPortGetCoreID()
#define portCHECK_IF_IN_ISR() xPortCheckIfInISR()
#define portGET_TASK_LOCK() vPortRecursiveLock(0u, 1)
#define portRELEASE_TASK_LOCK() vPortRecursiveLock(0u, 0)
#define portGET_ISR_LOCK() vPortRecursiveLock(1u, 1)
#define portRELEASE_ISR_LOCK() vPortRecursiveLock(1u, 0)

/*
 * Called by vTaskSwitchContext with both kernel locks held, after it has
 * chosen this core's next task: where the port records which thread a
 * core runs. See port.c for why it has to be here.
 */
void vPortTaskSwitchHook(void *pxTCB);
#define portTASK_SWITCH_HOOK(pxTCB) vPortTaskSwitchHook(pxTCB)

/*
 * What an idle task does instead of spinning: sleep until a yield
 * request or a millisecond, then yield. Called from the idle hooks.
 */
void vPortIdleWait(void);

#define portTASK_FUNCTION_PROTO(vFunction, pvParameters)                       \
    void vFunction(void *pvParameters) __attribute__((noreturn))
#define portTASK_FUNCTION(vFunction, pvParameters)                             \
    void vFunction(void *pvParameters)

#define portMEMORY_BARRIER() __atomic_thread_fence(__ATOMIC_SEQ_CST)
#define portSOFTWARE_BARRIER() __asm volatile("" ::: "memory")

#define portCONFIGURE_TIMER_FOR_RUN_TIME_STATS()
#define portGET_RUN_TIME_COUNTER_VALUE() 0u

#endif /* PORTMACRO_H */
