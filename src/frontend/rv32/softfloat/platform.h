/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * platform.h - Build configuration for Berkeley SoftFloat.
 *
 * SoftFloat expects the host to supply this. Everything the emulator
 * targets -- x86-64 for the host build, ARMv6-M through ARMv8.1-M for the
 * firmware -- is little-endian with a usable 64-bit integer type, so the
 * configuration is the same for all of them.
 */
#ifndef RV32_SOFTFLOAT_PLATFORM_H
#define RV32_SOFTFLOAT_PLATFORM_H

#define LITTLEENDIAN 1

/*
 * The 64-bit paths are faster where a 64-bit multiply is cheap and smaller
 * where it is not. This core only uses the f32 entry points, so the choice
 * barely matters; INLINE_LEVEL is kept low to favour size on a part where
 * the emulator shares flash with the guest image.
 */
#define SOFTFLOAT_FAST_INT64 1
#define INLINE_LEVEL 1
#define INLINE static inline

/*
 * SoftFloat's rounding mode, flags and tininess mode are globals. Where
 * cores run as threads (EMU_SMP) they must be each thread's, and
 * cmake/freertos.cmake defines THREAD_LOCAL for the whole of emucore so
 * that every declaration of them agrees with the definition; anywhere
 * else they are the one global they always were.
 */
#ifndef THREAD_LOCAL
#define THREAD_LOCAL
#endif

#endif /* RV32_SOFTFLOAT_PLATFORM_H */
