/* SPDX-License-Identifier: Apache-2.0 */
/*
 * sys/types.h - empty, for the same reason as sys/times.h beside it.
 *
 * dhry.h includes it immediately before <sys/times.h>, and nothing in the
 * benchmark uses anything from it. Left empty rather than passed through
 * to the toolchain's, which on newlib pulls in a hosted environment's
 * worth of declarations -- including the clock_t that makes the real
 * sys/times.h collide with Dhrystone's own prototype.
 */
#ifndef GUEST_SYS_TYPES_H
#define GUEST_SYS_TYPES_H

#endif /* GUEST_SYS_TYPES_H */
