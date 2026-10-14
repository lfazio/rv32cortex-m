/* SPDX-License-Identifier: Apache-2.0 */
/*
 * emu_lock.c - the one object every emu_lock owner is named by. See
 * include/emu/emu_lock.h.
 */
#include "emu/emu_lock.h"

#if EMU_SMP
_Thread_local char emu_lock_token;
#else
/* Nothing to define: without EMU_SMP the locks are empty. */
typedef int emu_lock_unused_t;
#endif
