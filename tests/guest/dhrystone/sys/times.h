/* SPDX-License-Identifier: Apache-2.0 */
/*
 * sys/times.h - the freestanding guest's own, shadowing the toolchain's.
 *
 * dhry.h includes <sys/times.h> whenever TIMES is selected, and this guest
 * links -nostdlib against three different toolchains. The header is found
 * here rather than in a sysroot because -I directories are searched first,
 * which is the whole mechanism: the benchmark keeps its unmodified include
 * and gets the platform's definition of the structure.
 *
 * **It deliberately does not declare times().** dhry_1.c carries its own
 * `extern int times ();` -- 1988 K&R, returning int -- while newlib's
 * header prototypes `clock_t times(struct tms *)` with clock_t a long.
 * Both visible in one translation unit is a conflicting-types error, so
 * including the real header is not an option and neither is declaring the
 * function here. The definition in dhry_portme.c returns long; the return
 * value is never read, only tms_utime is, so the mismatch is confined to a
 * value nothing looks at.
 */
#ifndef GUEST_SYS_TIMES_H
#define GUEST_SYS_TIMES_H

struct tms {
    long tms_utime;             /* the only field Dhrystone reads */
    long tms_stime;
    long tms_cutime;
    long tms_cstime;
};

#endif /* GUEST_SYS_TIMES_H */
