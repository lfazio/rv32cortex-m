/* SPDX-License-Identifier: Apache-2.0 */
/*
 * emu_lock.h - mutual exclusion for state more than one core can reach.
 *
 * Only an SMP host runs emulated cores at the same time (EMU_SMP, set by
 * cmake/freertos.cmake). Everywhere else a core's quantum is never
 * interrupted by another core's, and these compile to nothing: a board
 * pays not even the atomic, and ARMv6-M -- which has no exclusive
 * loads -- never sees one.
 *
 * A spinlock rather than anything that sleeps, because what it guards is
 * a register access or an interrupt decision: a few hundred host
 * instructions at most, and a holder cannot block. Take it at every
 * public way in -- a device's bus handler, an exported function a core
 * calls -- and nesting takes care of itself.
 */
#ifndef EMU_LOCK_H
#define EMU_LOCK_H

#ifndef EMU_SMP
#define EMU_SMP 0
#endif

#if EMU_SMP

#include <stdatomic.h>
#include <stddef.h>

/*
 * Recursive, by owner: a device's handler can raise an interrupt in
 * another device that takes the same lock, and that is the shape of the
 * calls rather than a mistake in them. The owner is the address of a
 * thread-local defined once, in emu_lock.c -- a static inside this
 * header would be a different object in every file that includes it.
 */
extern _Thread_local char emu_lock_token;

typedef struct {
    _Atomic(const void *) owner;
    unsigned depth; /* touched only by the owner */
} emu_lock_t;

#define EMU_LOCK_INIT {NULL, 0u}

static inline void emu_lock(emu_lock_t *l)
{
    const void *const me = &emu_lock_token;

    if (atomic_load_explicit(&l->owner, memory_order_relaxed) == me) {
        l->depth++;
        return;
    }
    for (;;) {
        const void *expected = NULL;

        if (atomic_compare_exchange_weak_explicit(&l->owner, &expected, me,
                                                  memory_order_acquire,
                                                  memory_order_relaxed)) {
            break;
        }
#if defined(__x86_64__) || defined(__i386__)
        __builtin_ia32_pause();
#elif defined(__aarch64__)
        __asm__ volatile("yield");
#endif
    }
    l->depth = 1u;
}

static inline void emu_unlock(emu_lock_t *l)
{
    if (--l->depth == 0u) {
        atomic_store_explicit(&l->owner, NULL, memory_order_release);
    }
}

#else

typedef struct {
    char unused;
} emu_lock_t;

#define EMU_LOCK_INIT {0}

static inline void emu_lock(emu_lock_t *l)
{
    (void)l;
}

static inline void emu_unlock(emu_lock_t *l)
{
    (void)l;
}

#endif /* EMU_SMP */

#endif /* EMU_LOCK_H */
