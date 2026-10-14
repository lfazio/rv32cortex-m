/* SPDX-License-Identifier: Apache-2.0 */
/*
 * port.c - FreeRTOS SMP on POSIX threads.
 *
 * The kernel's POSIX port runs one task at a time, in every release up
 * to V11.3.1; its only SMP ports are the RP2040 and XCORE.AI. This is the
 * same model -- one host thread per task, a task not running is a thread
 * waiting on its own event -- with configNUMBER_OF_CORES of them running
 * at once. Written for this tree; the kernel's POSIX port (MIT) is the
 * pattern, not the source.
 *
 * What a "core" is here, in the kernel's terms:
 *
 *   - its ID is a thread-local, set by whoever resumes the thread onto a
 *     core and read back by the thread when it wakes;
 *   - its interrupts are SIG_YIELD and SIG_TICK, masked by a
 *     thread-local flag rather than the signal mask (see t_irq_off), and
 *     enabling them services whatever is pending before it returns --
 *     the kernel's demand that a pending yield be serviced
 *     *immediately* (prvCheckForRunStateChange asserts it);
 *   - an interrupt from another core, portYIELD_CORE, is a flag and a
 *     pthread_kill to the thread on that core; the handler acts only if
 *     the flag is set, so a signal that arrives late is harmless;
 *   - the two kernel locks are recursive spinlocks owned by core.
 *
 * The one ordering problem worth stating. A core that has just switched
 * tasks can be asked to yield before the port has recorded which thread
 * now runs there. If the request lands on the old thread and the flag is
 * then cleared by the switch, the new task never yields and the kernel
 * asserts the first time it enters a critical section. So both "which
 * thread is on this core" and "no yield is pending" are written in
 * portTASK_SWITCH_HOOK, which vTaskSwitchContext calls with both kernel
 * locks held -- the same locks every yield request is made under. A
 * request is therefore either before the switch, and satisfied by it, or
 * after, and addressed to the new thread.
 *
 * Idle tasks do not spin: the idle hooks sleep for at most a millisecond
 * or until a signal, then yield. Preemption is on, so a task that becomes
 * ready interrupts an idle core rather than waiting for it to look; the
 * millisecond is a backstop, not the mechanism.
 */

#define _GNU_SOURCE

#include "FreeRTOS.h"
#include "task.h"

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#if configNUMBER_OF_CORES < 2
#error "freertos_smp/port.c is for configNUMBER_OF_CORES > 1"
#endif

#define PORT_MIN_THREAD_STACK (2u * 1024u * 1024u)

#define SIG_YIELD SIGUSR2 /* another core asks this one to yield */
#define SIG_TICK SIGALRM /* the tick, to core 0 */
#define SIG_RESUME SIGUSR1 /* the main thread: the scheduler ended */

/*
 * How long a waiter spins before it sleeps. A round of a multicore guest
 * hands every core's thread off several times, and a handoff to a thread
 * that is *sleeping* costs the host scheduler's wakeup latency each time:
 * measured at about 66 us a round, against about 14 us of translated work
 * per core at the default quantum -- the parallel run was slower than the
 * serial one. A waiter that is still spinning sees its flag in a cache
 * line's time. Past the spin it sleeps, so a long wait costs no CPU.
 */
#ifndef PORT_SPIN_NS
#define PORT_SPIN_NS 200000L
#endif

/*
 * A one-shot wakeup: event_signal before event_wait is not lost.
 *
 * The signaller sets `set` and then looks at `sleeping`; the waiter sets
 * `sleeping` and then looks at `set`. Both sequentially consistent, so at
 * least one of them sees the other's store -- the waiter does not sleep
 * past a signal, and a signaller that misses a sleeper is followed by a
 * waiter that sees `set`.
 */
typedef struct {
    atomic_int set;
    atomic_int sleeping;
    pthread_mutex_t m;
    pthread_cond_t c;
} port_event_t;

static uint64_t now_ns(void)
{
    struct timespec t;

    (void)clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}

static void cpu_relax(void);

static void event_init(port_event_t *e)
{
    atomic_init(&e->set, 0);
    atomic_init(&e->sleeping, 0);
    (void)pthread_mutex_init(&e->m, NULL);
    (void)pthread_cond_init(&e->c, NULL);
}

static void event_signal(port_event_t *e)
{
    atomic_store(&e->set, 1);
    if (atomic_load(&e->sleeping) != 0) {
        (void)pthread_mutex_lock(&e->m);
        (void)pthread_cond_signal(&e->c);
        (void)pthread_mutex_unlock(&e->m);
    }
}

static void event_wait(port_event_t *e)
{
    const uint64_t until = now_ns() + (uint64_t)PORT_SPIN_NS;

    for (unsigned i = 0u;; i++) {
        if (atomic_exchange(&e->set, 0) != 0) {
            return;
        }
        if ((i & 63u) == 63u && now_ns() >= until) {
            break;
        }
        cpu_relax();
    }
    (void)pthread_mutex_lock(&e->m);
    atomic_store(&e->sleeping, 1);
    while (atomic_exchange(&e->set, 0) == 0) {
        (void)pthread_cond_wait(&e->c, &e->m);
    }
    atomic_store(&e->sleeping, 0);
    (void)pthread_mutex_unlock(&e->m);
}

typedef struct {
    pthread_t pthread;
    TaskFunction_t code;
    void *params;
    port_event_t ev;
    /* The core this thread is being resumed onto; written by the
     * resumer before event_signal, read by the thread after waking. */
    volatile BaseType_t core;
} thread_t;

/* The thread record sits at the top of the task's stack buffer, which
 * the TCB's first member points at -- as in the kernel's POSIX port. */
static thread_t *thread_of(TaskHandle_t task)
{
    StackType_t *top = *(StackType_t **)task;

    return (thread_t *)(top + 1);
}

static __thread BaseType_t t_core;
static __thread unsigned t_in_isr;

/* Which thread each core runs. Written by that core with the kernel
 * locks held (vPortTaskSwitchHook); read by other cores and the tick
 * thread, hence release and acquire. */
static _Atomic(thread_t *) s_running[configNUMBER_OF_CORES];
static atomic_bool s_yield_pending[configNUMBER_OF_CORES];

/*
 * Set while a core's idle task is spinning in vPortIdleWait, watching
 * s_yield_pending itself: a yield request to it needs no signal. The
 * requester stores pending and then reads this; the idle loop clears
 * this and then reads pending -- both sequentially consistent, so a
 * request is either seen by the loop or signalled.
 */
static atomic_bool s_idle_polling[configNUMBER_OF_CORES];

static sigset_t s_irq; /* SIG_YIELD and SIG_TICK */
static pthread_t s_main;
static volatile bool s_end;
static pthread_t s_tick_thread;
static atomic_bool s_tick_run;

static void fatal(const char *what, int err)
{
    fprintf(stderr, "freertos smp port: %s: %s\n", what, strerror(err));
    abort();
}

/* ------------------------------------------------------------------ */
/* Cores and interrupts                                                */
/* ------------------------------------------------------------------ */

BaseType_t xPortGetCoreID(void)
{
    return t_core;
}

BaseType_t xPortCheckIfInISR(void)
{
    return t_in_isr != 0u ? pdTRUE : pdFALSE;
}

/*
 * A core's interrupts are masked in software: a thread-local flag, which
 * a handler finding set leaves its work pending for. It was the signal
 * mask, and the kernel masks around every critical section -- 87
 * pthread_sigmask system calls a round, measured, several of them with
 * the kernel's locks held, so the other cores spun on those locks too.
 *
 * Enabling services what is pending before it returns, which is the
 * kernel's requirement (see prvCheckForRunStateChange). That includes a
 * yield whose signal is still in flight: the requester sets
 * s_yield_pending before it signals, under the kernel locks, so enabling
 * looks at the flag rather than waiting for the signal.
 *
 * Every thread starts masked: a thread is interruptible only once it has
 * been put on a core.
 */
static __thread volatile sig_atomic_t t_irq_off = 1;
static __thread volatile sig_atomic_t t_tick_due;

static void yield_body(void);
static void tick_body(void);

static void service_pending(void)
{
    for (;;) {
        if (atomic_load_explicit(&s_yield_pending[t_core],
                                 memory_order_acquire)) {
            t_irq_off = 1;
            yield_body();
            t_irq_off = 0;
            continue;
        }
        if (t_tick_due != 0) {
            t_tick_due = 0;
            t_irq_off = 1;
            tick_body();
            t_irq_off = 0;
            continue;
        }
        return;
    }
}

void vPortDisableInterrupts(void)
{
    t_irq_off = 1;
    atomic_signal_fence(memory_order_seq_cst);
}

void vPortEnableInterrupts(void)
{
    atomic_signal_fence(memory_order_seq_cst);
    t_irq_off = 0;
    atomic_signal_fence(memory_order_seq_cst);
    service_pending();
}

UBaseType_t xPortSetInterruptMask(void)
{
    const UBaseType_t was = (t_irq_off != 0) ? 1u : 0u;

    vPortDisableInterrupts();
    return was;
}

void vPortClearInterruptMask(UBaseType_t was_masked)
{
    if (was_masked == 0u) {
        vPortEnableInterrupts();
    }
}

/* ------------------------------------------------------------------ */
/* The kernel's two locks                                              */
/* ------------------------------------------------------------------ */

static void cpu_relax(void)
{
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__) || defined(__arm__)
    __asm__ volatile("yield");
#endif
}

typedef struct {
    atomic_long owner; /* core, or -1 */
    unsigned count; /* only touched by the owner */
} port_lock_t;

static port_lock_t s_lock[2] = {{-1, 0u}, {-1, 0u}};

void vPortRecursiveLock(unsigned which, BaseType_t acquire)
{
    port_lock_t *const l = &s_lock[which];
    const long me = (long)t_core;

    if (acquire) {
        if (atomic_load_explicit(&l->owner, memory_order_relaxed) == me) {
            l->count++;
            return;
        }
        for (unsigned spins = 0u;; spins++) {
            long expected = -1;

            if (atomic_compare_exchange_weak_explicit(&l->owner, &expected, me,
                                                      memory_order_acquire,
                                                      memory_order_relaxed)) {
                break;
            }
            /* The holder may be a thread the host has descheduled. */
            if ((spins & 255u) == 255u) {
                (void)sched_yield();
            } else {
                cpu_relax();
            }
        }
        l->count = 1u;
        return;
    }
    configASSERT(atomic_load_explicit(&l->owner, memory_order_relaxed) == me);
    if (--l->count == 0u) {
        atomic_store_explicit(&l->owner, -1, memory_order_release);
    }
}

/* ------------------------------------------------------------------ */
/* Switching                                                           */
/* ------------------------------------------------------------------ */

void vPortTaskSwitchHook(void *tcb)
{
    const BaseType_t core = t_core;

    atomic_store_explicit(&s_running[core], thread_of((TaskHandle_t)tcb),
                          memory_order_release);
    atomic_store_explicit(&s_yield_pending[core], false, memory_order_relaxed);
}

/*
 * Choose this core's next task and, if it is another, hand the core to
 * its thread and wait to be handed one. Called with interrupts disabled
 * and outside any critical section -- vTaskSwitchContext asserts the
 * second and takes the kernel locks itself.
 */
static void switch_on_this_core(void)
{
    const BaseType_t core = t_core;
    const TaskHandle_t before = xTaskGetCurrentTaskHandleForCore(core);

    vTaskSwitchContext(core);

    const TaskHandle_t after = xTaskGetCurrentTaskHandleForCore(core);

    if (after == before) {
        return;
    }
    thread_t *const self = thread_of(before);
    thread_t *const next = thread_of(after);

    next->core = core;
    event_signal(&next->ev);
    event_wait(&self->ev);
    t_core = self->core;
}

void vPortYield(void)
{
    const UBaseType_t was = xPortSetInterruptMask();

    switch_on_this_core();
    vPortClearInterruptMask(was);
}

void vPortYieldFromISR(void)
{
    switch_on_this_core(); /* in a handler: interrupts are already off */
}

void vPortYieldCore(BaseType_t core)
{
    /* With both kernel locks held: see vPortTaskSwitchHook. */
    atomic_store(&s_yield_pending[core], true);
    if (atomic_load(&s_idle_polling[core])) {
        return; /* it is looking: see s_idle_polling */
    }
    thread_t *const t =
        atomic_load_explicit(&s_running[core], memory_order_acquire);

    if (t != NULL) {
        (void)pthread_kill(t->pthread, SIG_YIELD);
    }
}

static void yield_body(void)
{
    if (!atomic_exchange_explicit(&s_yield_pending[t_core], false,
                                  memory_order_relaxed)) {
        return; /* late: whatever it asked for has happened */
    }
    t_in_isr++;
    switch_on_this_core();
    t_in_isr--;
}

static void tick_body(void)
{
    t_in_isr++;
    const UBaseType_t saved = taskENTER_CRITICAL_FROM_ISR();
    const BaseType_t switch_here = xTaskIncrementTick();

    taskEXIT_CRITICAL_FROM_ISR(saved);
    if (switch_here != pdFALSE) {
        switch_on_this_core();
    }
    t_in_isr--;
}

/* Masked, the request stays in s_yield_pending for enabling to find. */
static void yield_handler(int sig)
{
    (void)sig;
    if (t_irq_off != 0) {
        return;
    }
    t_irq_off = 1;
    yield_body();
    t_irq_off = 0;
}

static void tick_handler(int sig)
{
    (void)sig;
    if (t_irq_off != 0) {
        t_tick_due = 1;
        return;
    }
    t_irq_off = 1;
    tick_body();
    t_irq_off = 0;
}

void vPortIdleWait(void)
{
    /*
     * Spin first, for the reason PORT_SPIN_NS gives: interrupts are on, so
     * a yield request is serviced the moment its signal lands, and a
     * running thread takes a signal far sooner than a sleeping one wakes.
     * Then sleep, so a core with nothing to do for long costs nothing.
     */
    const BaseType_t core = t_core;
    const uint64_t until = now_ns() + (uint64_t)PORT_SPIN_NS;
    bool asked = false;

    /*
     * And while it spins it says so, so that a request needs no signal
     * at all -- the signal and its handler were most of what a round
     * still cost after the spinning waits.
     */
    atomic_store(&s_idle_polling[core], true);
    while (!(asked = atomic_load(&s_yield_pending[core])) && now_ns() < until) {
        for (unsigned i = 0u; i < 64u; i++) {
            cpu_relax();
        }
    }
    atomic_store(&s_idle_polling[core], false);
    if (!asked && !atomic_load(&s_yield_pending[core])) {
        const struct timespec ms = {0, 1000000L};

        (void)nanosleep(&ms, NULL); /* a signal ends it early */
    }
    taskYIELD();
}

/* ------------------------------------------------------------------ */
/* Tasks                                                               */
/* ------------------------------------------------------------------ */

static void *thread_start(void *arg)
{
    thread_t *const t = arg;

    event_wait(&t->ev); /* masked: see t_irq_off */
    t_core = t->core;
    vPortEnableInterrupts();
    t->code(t->params);
    configASSERT(pdFALSE); /* a task function must not return */
    return NULL;
}

StackType_t *pxPortInitialiseStack(StackType_t *top, StackType_t *end,
                                   TaskFunction_t code, void *params)
{
    thread_t *const t = (thread_t *)(top + 1) - 1;
    pthread_attr_t attr;
    size_t bytes;
    int rc;

    top = (StackType_t *)t - 1;
    bytes = (size_t)(top + 1 - end) * sizeof(*top);
    /*
     * The task's buffer only carries the thread record; the thread runs
     * on a stack of its own, and glibc takes the thread's static TLS out
     * of the top of it. The emulator keeps per-core state there
     * (EMU_PER_CORE), so a FreeRTOS-sized stack -- 32 KB for an idle
     * task -- is not enough to start the thread. Address space, not
     * memory: untouched pages are never committed.
     */
    if (bytes < PORT_MIN_THREAD_STACK) {
        bytes = PORT_MIN_THREAD_STACK;
    }
    t->code = code;
    t->params = params;
    t->core = 0;
    event_init(&t->ev);

    (void)pthread_attr_init(&attr);
    (void)pthread_attr_setstacksize(&attr, bytes);
    rc = pthread_create(&t->pthread, &attr, thread_start, t);
    (void)pthread_attr_destroy(&attr);
    if (rc != 0) {
        fatal("pthread_create", rc);
    }
    return top;
}

/* ------------------------------------------------------------------ */
/* The scheduler                                                       */
/* ------------------------------------------------------------------ */

static void *tick_thread(void *arg)
{
    (void)arg;
    while (atomic_load(&s_tick_run)) {
        thread_t *const t =
            atomic_load_explicit(&s_running[0], memory_order_acquire);

        if (t != NULL) {
            (void)pthread_kill(t->pthread, SIG_TICK);
        }
        (void)usleep(portTICK_RATE_MICROSECONDS);
    }
    return NULL;
}

static void install(int sig, void (*fn)(int))
{
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = fn;
    sa.sa_flags = SA_RESTART;
    sa.sa_mask = s_irq; /* a handler runs with interrupts off */
    if (sigaction(sig, &sa, NULL) != 0) {
        fatal("sigaction", errno);
    }
}

BaseType_t xPortStartScheduler(void)
{
    sigset_t resume;
    int sig;

    s_main = pthread_self();
    install(SIG_YIELD, yield_handler);
    install(SIG_TICK, tick_handler);

    /* The main thread is no core: it starts them and waits. */
    vPortDisableInterrupts();
    sigemptyset(&resume);
    sigaddset(&resume, SIG_RESUME);
    (void)pthread_sigmask(SIG_BLOCK, &resume, NULL);

    for (BaseType_t k = 0; k < configNUMBER_OF_CORES; k++) {
        thread_t *const t = thread_of(xTaskGetCurrentTaskHandleForCore(k));

        t->core = k;
        atomic_store_explicit(&s_running[k], t, memory_order_release);
    }
    atomic_store(&s_tick_run, true);
    if (pthread_create(&s_tick_thread, NULL, tick_thread, NULL) != 0) {
        fatal("pthread_create", errno);
    }
    for (BaseType_t k = 0; k < configNUMBER_OF_CORES; k++) {
        event_signal(&atomic_load(&s_running[k])->ev);
    }

    while (!s_end) {
        (void)sigwait(&resume, &sig);
    }
    return 0;
}

void vPortEndScheduler(void)
{
    atomic_store(&s_tick_run, false);
    s_end = true;
    (void)pthread_kill(s_main, SIG_RESUME);
}

/*
 * The set has to exist before the first task's thread is created, which
 * is before the scheduler starts.
 */
__attribute__((constructor)) static void port_init(void)
{
    sigemptyset(&s_irq);
    sigaddset(&s_irq, SIG_YIELD);
    sigaddset(&s_irq, SIG_TICK);
}
