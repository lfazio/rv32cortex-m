/* SPDX-License-Identifier: Apache-2.0 */
/*
 * rv_config.h - Compile-time configuration for the RV32 core.
 *
 * Every knob may be overridden from the build system (-DRV_xxx=...). The
 * defaults target a Cortex-M4/M7 class device with a few tens of KiB to
 * spare for the guest.
 */
#ifndef RV32_RV_CONFIG_H
#define RV32_RV_CONFIG_H

/* ------------------------------------------------------------------ */
/* ISA selection                                                       */
/* ------------------------------------------------------------------ */

/* RV32I is always present. These select the optional extensions. */
#ifndef RV_EXT_M
#  define RV_EXT_M      1   /* integer multiply / divide */
#endif
#ifndef RV_EXT_A
#  define RV_EXT_A      1   /* atomics (LR/SC + AMO) */
#endif
#ifndef RV_EXT_C
#  define RV_EXT_C      1   /* compressed 16-bit instructions */
#endif
#ifndef RV_EXT_ZICSR
#  define RV_EXT_ZICSR  1   /* CSR access instructions */
#endif
#ifndef RV_EXT_ZICNTR
#  define RV_EXT_ZICNTR 1   /* cycle / time / instret counters */
#endif
#ifndef RV_EXT_ZICBOM
#  define RV_EXT_ZICBOM 1   /* cbo.clean / cbo.inval / cbo.flush */
#endif
#ifndef RV_EXT_ZICBOZ
#  define RV_EXT_ZICBOZ 1   /* cbo.zero */
#endif

/*
 * Cache block size reported through the Zicboz CSR and used as the
 * granule for every CBO. 32 bytes matches the Cortex-M7 L1 line size.
 * Must be a power of two.
 */
#ifndef RV_CACHE_BLOCK_SIZE
#  define RV_CACHE_BLOCK_SIZE 32u
#endif

/* ------------------------------------------------------------------ */
/* Bus configuration                                                   */
/* ------------------------------------------------------------------ */

/*
 * Maximum number of regions in a bus region table. Passthrough platforms
 * need more than they first appear to: protecting individual peripheral
 * registers means splitting a window into several entries.
 */
#ifndef RV_MAX_REGIONS
#  define RV_MAX_REGIONS 16
#endif

/*
 * Misaligned load/store support. The RISC-V spec permits an implementation
 * to either handle misaligned accesses in hardware or raise a misaligned
 * exception. We raise, which matches most embedded RISC-V cores and keeps
 * the memory path branch-free. Set to 1 to emulate them by splitting.
 */
#ifndef RV_MISALIGNED_OK
#  define RV_MISALIGNED_OK 0
#endif

/* ------------------------------------------------------------------ */
/* Execution engine                                                    */
/* ------------------------------------------------------------------ */

/*
 * Allow a platform to intercept ECALL before it traps. Used by the host
 * test harness for exit/console services; costs one predictable branch per
 * ECALL and nothing elsewhere.
 */
#ifndef RV_ENABLE_ECALL_HOOK
#  define RV_ENABLE_ECALL_HOOK 1
#endif

/*
 * How many instructions rv_backend_t::run executes before returning to the
 * host so it can service ARM-side work (timers, USB, RTOS ticks).
 */
#ifndef RV_DEFAULT_BUDGET
#  define RV_DEFAULT_BUDGET 4096u
#endif

/* ------------------------------------------------------------------ */
/* Debug / diagnostics                                                 */
/* ------------------------------------------------------------------ */

/* Build the disassembler (costs ~3 KiB of flash; useful for tracing). */
#ifndef RV_ENABLE_DISASM
#  define RV_ENABLE_DISASM 1
#endif

/* Per-instruction trace hook. Slow; enable only when chasing a bug. */
#ifndef RV_ENABLE_TRACE
#  define RV_ENABLE_TRACE 0
#endif

/* Count executed instructions / traps into the hart for `stats`. */
#ifndef RV_ENABLE_STATS
#  define RV_ENABLE_STATS 1
#endif

/* ------------------------------------------------------------------ */
/* Identification (read back through the M-mode ID CSRs)               */
/* ------------------------------------------------------------------ */

#ifndef RV_MVENDORID
#  define RV_MVENDORID 0u          /* 0 = non-commercial implementation */
#endif
#ifndef RV_MARCHID
#  define RV_MARCHID   0u
#endif
#ifndef RV_MIMPID
#  define RV_MIMPID    0x00010000u /* rv32cortex-m v1.0 */
#endif

#endif /* RV32_RV_CONFIG_H */
