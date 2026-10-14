/* SPDX-License-Identifier: Apache-2.0 */
/*
 * emu_os.c - the runner as FreeRTOS tasks. See emu_os.h.
 *
 * Three things live here and nowhere else: the first task, the core
 * tasks, and the barrier between rounds. Nothing in src/emu/ knows there
 * is an operating system; a frontend still runs a whole quantum behind
 * one indirect call, and the kernel is only ever entered between quanta.
 */

#include "emu_os.h"

#include "emu_console.h"
#include "emu_debug.h"

#include "FreeRTOS.h"
#include "task.h"

#if defined(EMU_OS_PORT_POSIX)
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#endif

/* ------------------------------------------------------------------ */
/* Stacks                                                              */
/* ------------------------------------------------------------------ */

/*
 * What the runner and a core need.
 *
 * On a board the first task runs the run loop, the translator and the
 * console -- everything the main stack used to carry, which now serves
 * interrupts only -- and its stack is reserved by the linker script (see
 * below). A core task's is .bss here, and exists only in a build that
 * can run more than one core. Measured on the F746: 1,312 bytes for
 * CoreMark under the JIT, under 900 for a G4MH core.
 *
 * On the host these are pthread stack sizes. The static buffer holds
 * only the port's thread record at its top and the rest is never
 * touched, so it costs address space and nothing else.
 *
 * emu_os_report prints how much each used.
 */
#ifndef EMU_OS_MAIN_STACK_BYTES
#define EMU_OS_MAIN_STACK_BYTES (8u * 1024u * 1024u) /* the host's */
#endif

#ifndef EMU_OS_CORE_STACK_BYTES
#if defined(EMU_OS_PORT_POSIX)
#define EMU_OS_CORE_STACK_BYTES (1u * 1024u * 1024u)
#else
#define EMU_OS_CORE_STACK_BYTES (4u * 1024u)
#endif
#endif

#define MAIN_WORDS (EMU_OS_MAIN_STACK_BYTES / sizeof(StackType_t))
#define CORE_WORDS (EMU_OS_CORE_STACK_BYTES / sizeof(StackType_t))

/* Equal priorities, so the order a round's tasks become ready in is the
 * order they run in -- see emu_os_system_step. */
#define EMU_OS_PRIORITY (tskIDLE_PRIORITY + 1u)

static StaticTask_t g_main_tcb;
static TaskHandle_t g_main;

#if defined(EMU_OS_PORT_POSIX)
static StackType_t g_main_stack[MAIN_WORDS];
#define MAIN_STACK g_main_stack
#define MAIN_STACK_WORDS ((uint32_t)MAIN_WORDS)
#else
/*
 * A board's linker script reserves it at the top of RAM, beside the
 * interrupt stack, so that nothing in .bss moves for it -- see
 * stm32f746zgtx.ld for what moving it cost.
 */
extern StackType_t __task_stack_start[];
extern StackType_t __task_stack_end[];
#define MAIN_STACK __task_stack_start
#define MAIN_STACK_WORDS ((uint32_t)(__task_stack_end - __task_stack_start))
#endif

static emu_os_body_fn g_body;
static int g_argc;
static char **g_argv;

/* ------------------------------------------------------------------ */
/* The core tasks                                                      */
/* ------------------------------------------------------------------ */

typedef struct {
    TaskHandle_t handle;
    StaticTask_t tcb;
    uint32_t did;
    bool live;
    unsigned pos; /* where in the round this core actually ran */
} core_task_t;

/*
 * How many cores this build can run, from cmake/freertos.cmake: one
 * unless a frontend with several is compiled in (G4MH at G4MH_PE_COUNT).
 * Not EMU_MAX_CORES, which is 4 everywhere -- a stack per possible core
 * would be 12 KiB of a board's RAM taken from every single-core guest.
 */
#ifndef EMU_OS_MAX_CORES
#define EMU_OS_MAX_CORES EMU_MAX_CORES
#endif

/* Core 0 runs on whichever task calls the round; these are 1..N-1. */
static core_task_t g_core[EMU_OS_MAX_CORES];
#if EMU_OS_MAX_CORES > 1
static StackType_t g_core_stack[EMU_OS_MAX_CORES - 1u][CORE_WORDS];
#endif

/*
 * The round in progress. Written by the coordinator before it releases
 * the cores and read by them after; the task notification that releases
 * each one is the ordering between the two, on a single core and, by
 * the kernel's own guarantees, on SMP.
 */
static emu_system_t *g_round_sys;
static uint32_t g_round_quantum;
static TaskHandle_t g_coordinator;

/*
 * Rounds that went through the core tasks, as against inline. Printed by
 * emu_os_report, because a multicore result identical to the
 * single-threaded one is exactly what a round that never used the tasks
 * would also produce.
 */
static uint64_t g_task_rounds;
static unsigned g_task_cores;

/* Position in the round of the next core to run -- see the check in
 * emu_os_system_step. Single-core kernels only: under SMP the round has
 * no order, and this would be a race. */
#if configNUMBER_OF_CORES == 1
static unsigned g_round_pos;
#endif

/* --serial: every round inline, in core order. */
static bool g_serial;

void emu_os_set_serial(bool serial)
{
    g_serial = serial;
}

static void core_task(void *arg)
{
    const unsigned k = (unsigned)(uintptr_t)arg;
    core_task_t *const t = &g_core[k];

    for (;;) {
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
#if configNUMBER_OF_CORES == 1
        t->pos = g_round_pos++;
#endif
        t->did =
            emu_core_slice(&g_round_sys->core[k], g_round_quantum, &t->live);
        (void)xTaskNotifyGive(g_coordinator);
    }
}

static void ensure_core_tasks(unsigned ncores)
{
#if EMU_OS_MAX_CORES > 1
    static const char *const k_name[] = {"core0", "core1", "core2", "core3",
                                         "core4", "core5", "core6", "core7"};

    for (unsigned k = 1u; k < ncores; k++) {
        if (g_core[k].handle != NULL) {
            continue;
        }
        g_core[k].handle = xTaskCreateStatic(
            core_task, k_name[k & 7u], CORE_WORDS, (void *)(uintptr_t)k,
            EMU_OS_PRIORITY, g_core_stack[k - 1u], &g_core[k].tcb);
    }
#else
    (void)ncores;
    (void)core_task;
#endif
}

uint32_t emu_os_system_step(emu_system_t *sys, uint32_t quantum, bool *all_idle)
{
    const unsigned n = sys->ncores;

    /*
     * One core: the round-robin itself, inline. Every single-core guest
     * -- which is every guest but G4MH's multicore builds -- pays the
     * scheduler nothing. And a system with more cores than this build
     * has tasks for runs the same way rather than not at all -- and so
     * does every round under --serial.
     */
    if (n <= 1u || n > EMU_OS_MAX_CORES || g_serial) {
        return emu_system_step(sys, quantum, all_idle);
    }

    ensure_core_tasks(n);
    g_task_rounds++;
    g_task_cores = n;
    g_round_sys = sys;
    g_round_quantum = quantum;
    g_coordinator = xTaskGetCurrentTaskHandle();

    /*
     * Release the others, then run core 0 here. Under the cooperative
     * scheduler the released tasks are ready but not running: this task
     * keeps the CPU until it blocks below, and they then run in the
     * order they were released. So a round is core 0, 1, 2 ... -- the
     * order emu_system_step runs them in, which is what makes the two
     * interchangeable and every multicore result reproducible.
     *
     * Under SMP each release puts a core's task on an idle FreeRTOS core
     * at once, and the round's quanta run in parallel. The notifications
     * are still the ordering between this task's writes above and the
     * cores' reads, and the wait below is still the barrier.
     */
#if configNUMBER_OF_CORES == 1
    g_round_pos = 0u;
#endif
    for (unsigned k = 1u; k < n; k++) {
        (void)xTaskNotifyGive(g_core[k].handle);
    }

    bool live = false;
#if configNUMBER_OF_CORES == 1
    const unsigned pos0 = g_round_pos++;
#endif
    uint32_t total = emu_core_slice(&sys->core[0], quantum, &live);
    bool idle = !live;

    for (unsigned done = 0u; done < n - 1u;) {
        done += (unsigned)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    }

#if configNUMBER_OF_CORES == 1
    /*
     * **The order is checked, not assumed**, every round. Nothing a
     * guest prints can hold it: a multicore test is written to give the
     * same answer at any interleaving, and running core 0 *last* instead
     * of first is a rotation of the round that produced byte-identical
     * output and an identical instruction trace for barrier3 at quanta
     * 1, 4 and 64. A scheduler change -- a priority, preemption, a port
     * that orders its ready list differently -- would be equally
     * invisible, and the reproducibility the multicore design rests on
     * would be gone. An SMP kernel runs the round in parallel and has no
     * order to check.
     */
    configASSERT(pos0 == 0u);
    for (unsigned k = 1u; k < n; k++) {
        configASSERT(g_core[k].pos == k);
    }
#endif
    for (unsigned k = 1u; k < n; k++) {
        total += g_core[k].did;
        idle = idle && !g_core[k].live;
    }
    if (all_idle != NULL) {
        *all_idle = idle;
    }
    return total;
}

/* ------------------------------------------------------------------ */
/* The first task                                                      */
/* ------------------------------------------------------------------ */

static void main_task(void *arg)
{
    (void)arg;

#if defined(EMU_OS_PORT_POSIX)
    /*
     * The POSIX port's tick is SIGALRM, delivered to whichever task's
     * thread is running, and its handler is installed without
     * SA_RESTART: a read() or write() it lands in fails with EINTR. This
     * runner does file and terminal I/O from the task -- a disk image, a
     * pty, stdout -- and stdio does not retry. Adding the flag to the
     * port's own disposition keeps its handler and makes the interrupted
     * calls restart; what still returns early (poll, nanosleep) is
     * already used here only in loops that tolerate it.
     */
    struct sigaction sa;

    if (sigaction(SIGALRM, NULL, &sa) == 0) {
        sa.sa_flags |= SA_RESTART;
        (void)sigaction(SIGALRM, &sa, NULL);
    }
#endif

    int status = g_body(g_argc, g_argv);

#if defined(EMU_OS_PORT_POSIX)
    /*
     * The scheduler owns the main thread, so main() cannot return the
     * status. exit() from here flushes stdio and ends every thread; the
     * port has no state worth tearing down.
     */
    (void)fflush(stdout);
    (void)fflush(stderr);
    exit(status);
#else
    /*
     * A board has nowhere to return to. main() used to return into the
     * reset handler's `bx lr` and hard-fault; this parks the same way a
     * fatal error does, serving the link if there is one.
     */
    for (;;) {
        emu_board_fatal(&status);
    }
#endif
}

int emu_os_start(emu_os_body_fn body, int argc, char **argv)
{
    g_body = body;
    g_argc = argc;
    g_argv = argv;

    g_main = xTaskCreateStatic(main_task, "main", MAIN_STACK_WORDS, NULL,
                               EMU_OS_PRIORITY, MAIN_STACK, &g_main_tcb);
    vTaskStartScheduler();

    /* Only reached if the scheduler could not start -- no idle task. */
    return 1;
}

/* ------------------------------------------------------------------ */
/* Reporting                                                           */
/* ------------------------------------------------------------------ */

static unsigned used_bytes(TaskHandle_t t, uint32_t size)
{
    const uint32_t free_bytes = (uint32_t)uxTaskGetStackHighWaterMark(t) *
                                (uint32_t)sizeof(StackType_t);

    return (unsigned)((free_bytes < size) ? size - free_bytes : 0u);
}

void emu_os_report(void)
{
    if (g_task_rounds != 0u) {
        char n[21];

        emu_console_printf("  os       %u cores as tasks, %s rounds\n",
                           g_task_cores,
                           emu_u64_str(n, (unsigned long long)g_task_rounds));
    }
#if !defined(EMU_OS_PORT_POSIX)
    /*
     * Boards only: on the host the "stack" is the port's thread record
     * and the real one is a pthread's, which the kernel cannot measure.
     */
    const uint32_t main_bytes =
        MAIN_STACK_WORDS * (uint32_t)sizeof(StackType_t);

    emu_console_printf("  os       stack main %u of %u bytes",
                       used_bytes(g_main, main_bytes), (unsigned)main_bytes);
    for (unsigned k = 1u; k < EMU_OS_MAX_CORES; k++) {
        if (g_core[k].handle != NULL) {
            emu_console_printf(
                ", core%u %u of %u", k,
                used_bytes(g_core[k].handle, EMU_OS_CORE_STACK_BYTES),
                (unsigned)EMU_OS_CORE_STACK_BYTES);
        }
    }
    emu_console_puts("\n");
#else
    (void)used_bytes;
#endif
}

/* ------------------------------------------------------------------ */
/* The kernel's callbacks                                              */
/* ------------------------------------------------------------------ */

/*
 * The idle task, which runs only when every task is blocked -- never, in
 * a round, since one of them is always running a core. Its stack is the
 * port's minimum.
 */
void vApplicationGetIdleTaskMemory(StaticTask_t **tcb, StackType_t **stack,
                                   configSTACK_DEPTH_TYPE *words)
{
    static StaticTask_t idle_tcb;
    static StackType_t idle_stack[configMINIMAL_STACK_SIZE];

    *tcb = &idle_tcb;
    *stack = idle_stack;
    *words = configMINIMAL_STACK_SIZE;
}

#if configNUMBER_OF_CORES > 1
/*
 * SMP: an idle task per FreeRTOS core. They run whenever a core has no
 * emulated core to run -- between rounds, and on cores past the guest's
 * -- and sleep in the hooks rather than spin, so an idle host core costs
 * the machine nothing (the port's vPortIdleWait).
 */
void vApplicationGetPassiveIdleTaskMemory(StaticTask_t **tcb,
                                          StackType_t **stack,
                                          configSTACK_DEPTH_TYPE *words,
                                          BaseType_t index)
{
    static StaticTask_t idle_tcb[configNUMBER_OF_CORES - 1];
    static StackType_t idle_stack[configNUMBER_OF_CORES - 1]
                                 [configMINIMAL_STACK_SIZE];

    *tcb = &idle_tcb[index];
    *stack = idle_stack[index];
    *words = configMINIMAL_STACK_SIZE;
}

void vApplicationIdleHook(void)
{
    vPortIdleWait();
}

void vApplicationPassiveIdleHook(void)
{
    vPortIdleWait();
}
#endif

void vApplicationStackOverflowHook(TaskHandle_t task, char *name)
{
    (void)task;
    emu_console_printf("\nfatal: FreeRTOS task '%s' overflowed its stack\n",
                       name);
    for (;;) {
        int status = 1;

        emu_board_fatal(&status);
#if defined(EMU_OS_PORT_POSIX)
        exit(status);
#endif
    }
}

void emu_os_assert_failed(const char *file, int line)
{
    emu_console_printf("\nfatal: FreeRTOS assertion at %s:%d\n", file, line);
    for (;;) {
        int status = 1;

        emu_board_fatal(&status);
#if defined(EMU_OS_PORT_POSIX)
        exit(status);
#endif
    }
}
