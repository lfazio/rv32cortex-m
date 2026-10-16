/* SPDX-License-Identifier: Apache-2.0 */
/*
 * FreeRTOSConfig.h - one configuration for the four ports this tree uses.
 *
 * FreeRTOS runs the emulator as one task per emulated core (emu_os.c).
 * What that asks of the kernel is small, and the settings below are
 * chosen for it rather than taken from a demo:
 *
 *   - **Cooperative** on every single-core port (an SMP host is the
 *     exception, and says why below). configUSE_PREEMPTION is 0 and time
 *     slicing is off,
 *     so a task leaves the CPU only where it blocks -- at the round
 *     barrier between quanta. That is what keeps a multicore guest's
 *     interleaving exactly what the single-threaded round-robin produced
 *     (core 0, 1, 2 in every round), and what makes the run reproducible
 *     at every quantum. A preemptive scheduler would switch cores in the
 *     middle of a quantum and the guest's total order would depend on the
 *     tick.
 *   - **Static.** No heap: every task, stack and TCB is in .bss, so a
 *     board's RAM is decided at link time and the guest gets the rest.
 *   - **Nothing else.** No software timers, no queues used, no idle hook.
 *
 * EMU_OS_PORT_POSIX / _POSIX_SMP / _ARMV7M / _ARMV8M come from
 * cmake/freertos.cmake.
 */
#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

#if !defined(__ASSEMBLER__)
#include <stdint.h>
#endif

/* ------------------------------------------------------------------ */
/* The scheduler                                                       */
/* ------------------------------------------------------------------ */

#if defined(EMU_OS_PORT_POSIX_SMP)
/*
 * **SMP, on a multicore host** (src/platform/host/freertos_smp): one
 * FreeRTOS core per emulated core, so a round's quanta run in parallel.
 * Preemption has to be on here, and it is not a change of policy: with
 * it off the kernel never asks an idle core to pick up a task that has
 * just been released, so each idle core would have to spin looking. The
 * cores' tasks share one priority above idle and time slicing stays off,
 * so they never preempt each other -- only idle tasks are ever
 * preempted. There is no order to assert: see emu_os_system_step.
 */
#define configUSE_PREEMPTION 1
#define configRUN_MULTIPLE_PRIORITIES 1
#define configUSE_CORE_AFFINITY 0
#else
#define configUSE_PREEMPTION 0
#endif
#define configUSE_TIME_SLICING 0
#define configUSE_PORT_OPTIMISED_TASK_SELECTION 0
#define configMAX_PRIORITIES 4
#define configMAX_TASK_NAME_LEN 12
#define configTICK_TYPE_WIDTH_IN_BITS TICK_TYPE_WIDTH_32_BITS
#define configSTACK_DEPTH_TYPE uint32_t
#define configIDLE_SHOULD_YIELD 1
#define configUSE_TASK_NOTIFICATIONS 1
/* Two: the rounds' own, and EMU_OS_NOTIFY_NET for the IP stack's task,
 * so a hand-off to it can never be counted as a core finishing. */
#define configTASK_NOTIFICATION_ARRAY_ENTRIES 2
#if defined(EMU_OS_PORT_POSIX_SMP)
#define configNUMBER_OF_CORES EMU_OS_MAX_CORES
#else
#define configNUMBER_OF_CORES 1
#endif

/* No heap at all: a dynamic allocation is a link error, which is the
 * point. */
#define configSUPPORT_STATIC_ALLOCATION 1
#define configSUPPORT_DYNAMIC_ALLOCATION 0
/*
 * Not the kernel's: in V11.1.0 it allocates the software-timer task's
 * stack whether or not there are timers, which on a board is RAM taken
 * from the guest for nothing. emu_os.c supplies the idle task's alone.
 */
#define configKERNEL_PROVIDED_STATIC_MEMORY 0

#define configUSE_TIMERS 0
#define configUSE_MUTEXES 0
#define configUSE_RECURSIVE_MUTEXES 0
#define configUSE_COUNTING_SEMAPHORES 0
#define configUSE_QUEUE_SETS 0
#define configQUEUE_REGISTRY_SIZE 0
#define configUSE_CO_ROUTINES 0
#if defined(EMU_OS_PORT_POSIX_SMP)
/* Idle cores sleep in their hooks instead of spinning (emu_os.c). */
#define configUSE_IDLE_HOOK 1
#define configUSE_PASSIVE_IDLE_HOOK 1
#else
#define configUSE_IDLE_HOOK 0
#endif
#define configUSE_TICK_HOOK 0
#define configUSE_MALLOC_FAILED_HOOK 0
#define configUSE_TRACE_FACILITY 0
#define configGENERATE_RUN_TIME_STATS 0
#define configUSE_NEWLIB_REENTRANT 0

/*
 * Overflow is checked on every switch, by the canary at the bottom of the
 * stack. Switches happen once per round, so this costs nothing that
 * matters and turns a too-small stack into a report rather than a
 * corrupted neighbour.
 */
#define configCHECK_FOR_STACK_OVERFLOW 2
#define configRECORD_STACK_HIGH_ADDRESS 1

#define INCLUDE_vTaskDelay 0
#define INCLUDE_vTaskDelete 0
#define INCLUDE_vTaskSuspend 0
#define INCLUDE_xTaskGetSchedulerState 1
#define INCLUDE_xTaskGetCurrentTaskHandle 1
#define INCLUDE_uxTaskGetStackHighWaterMark 1

#if !defined(__ASSEMBLER__)
void emu_os_assert_failed(const char *file, int line);
#endif
#define configASSERT(x)                                                        \
    do {                                                                       \
        if (!(x)) {                                                            \
            emu_os_assert_failed(__FILE__, __LINE__);                          \
        }                                                                      \
    } while (0)

/* ------------------------------------------------------------------ */
/* The ports                                                           */
/* ------------------------------------------------------------------ */

#if defined(EMU_OS_PORT_POSIX) || defined(EMU_OS_PORT_POSIX_SMP)

/*
 * The host. The tick is a SIGALRM sent to whichever task's thread is
 * running, and nothing here waits on time, so it is slow: every tick is
 * a signal that interrupts the emulator mid-instruction.
 */
#define configCPU_CLOCK_HZ 1000000u
#define configTICK_RATE_HZ 100u
#define configMINIMAL_STACK_SIZE 4096u

#elif defined(EMU_OS_PORT_ARMV7M) || defined(EMU_OS_PORT_ARMV8M)

#if !defined(__ASSEMBLER__)
extern uint32_t SystemCoreClock;
#endif

/*
 * 1 kHz, because SysTick is shared with the HAL: the handler advances
 * HAL_GetTick too, and every HAL timeout assumes a millisecond tick. The
 * clocks are configured before the scheduler starts (board_early_init),
 * so SystemCoreClock is final when the port programs SysTick from it.
 */
#define configCPU_CLOCK_HZ (SystemCoreClock)
#define configTICK_RATE_HZ 1000u
#define configMINIMAL_STACK_SIZE 128u

/* STM32: four priority bits. The kernel's own interrupts at the lowest. */
#define configPRIO_BITS 4
#define configLIBRARY_LOWEST_INTERRUPT_PRIORITY 15
#define configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY 5
#define configKERNEL_INTERRUPT_PRIORITY                                        \
    (configLIBRARY_LOWEST_INTERRUPT_PRIORITY << (8 - configPRIO_BITS))
#define configMAX_SYSCALL_INTERRUPT_PRIORITY                                   \
    (configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY << (8 - configPRIO_BITS))

#if defined(EMU_OS_PORT_ARMV7M)
/*
 * The v7-M ports export their exception handlers under their own names;
 * these make them *the* handlers, which is what V11's installation check
 * (configCHECK_HANDLER_INSTALLATION) verifies against the vector table.
 * A wrapper in stm32*_it.c that called them would fail it.
 */
#define vPortSVCHandler SVC_Handler
#define xPortPendSVHandler PendSV_Handler
#endif

#if defined(EMU_OS_PORT_ARMV8M)
/*
 * The N6 firmware is one secure image; there is no non-secure side to
 * hand tasks to. The M55's FPU and MVE registers are saved per task --
 * -mcpu=cortex-m55 lets the compiler use Helium anywhere.
 */
#define configENABLE_TRUSTZONE 0
#define configRUN_FREERTOS_SECURE_ONLY 1
#define configENABLE_FPU 1
#define configENABLE_MVE 1
#define configENABLE_MPU 0
#endif

#else
#error "FreeRTOSConfig.h: no EMU_OS_PORT_* defined -- see cmake/freertos.cmake"
#endif

#endif /* FREERTOS_CONFIG_H */
