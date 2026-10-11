/* SPDX-License-Identifier: Apache-2.0 */
/*
 * rv_config.h - Compile-time configuration of the RISC-V frontend.
 *
 * Everything here describes the guest architecture: which extensions are
 * implemented, how the privileged features behave, and how the RV32
 * backends are built. Knobs that belong to the emulator rather than to the
 * ISA -- the bus region table, the run budget, the diagnostics -- live in
 * emu/emu_config.h and are shared with every other frontend.
 *
 * Every knob may be overridden from the build system (-DRV_xxx=...). The
 * defaults target a Cortex-M4/M7 class device with a few tens of KiB to
 * spare for the guest.
 */
#ifndef RV32_RV_CONFIG_H
#define RV32_RV_CONFIG_H

#include "emu/emu_config.h"

/* ------------------------------------------------------------------ */
/* ISA selection                                                       */
/* ------------------------------------------------------------------ */

/* RV32I is always present. These select the optional extensions. */
#ifndef RV_EXT_M
#define RV_EXT_M 1 /* integer multiply / divide */
#endif
#ifndef RV_EXT_A
#define RV_EXT_A 1 /* atomics (LR/SC + AMO) */
#endif
#ifndef RV_EXT_C
#define RV_EXT_C 1 /* compressed 16-bit instructions */
#endif
/* Single-precision floating point, on SoftFloat. D is below. */
#ifndef RV_EXT_F
#define RV_EXT_F 1
#endif
#ifndef RV_EXT_ZICSR
#define RV_EXT_ZICSR 1 /* CSR access instructions */
#endif
#ifndef RV_EXT_ZICNTR
#define RV_EXT_ZICNTR 1 /* cycle / time / instret counters */
#endif
/*
 * Double precision. FLEN becomes 64 and single-precision values are
 * NaN-boxed; see the note on rv_hart_t.f.
 *
 * The arithmetic is Berkeley SoftFloat, the same as F, and the JIT gets
 * it for free: both backends route every FP operation to rv_hart_fp
 * rather than emitting host instructions, for the reason CLAUDE.md
 * records -- a host FPU asked to be a RISC-V FPU failed 23 of the 78 F
 * tests.
 */
#ifndef RV_EXT_D
#define RV_EXT_D 1
#endif
#if RV_EXT_D && !RV_EXT_F
#error "RV_EXT_D requires RV_EXT_F"
#endif
/*
 * Zacas: amocas.w, and amocas.d over even-odd register pairs.
 *
 * On, and passing the architecture suite's two Zacas tests on both
 * backends. Two comments stood here for a long time saying it was *off*
 * because it was unfinished -- one that amocas.d was not implemented, one
 * that it was implemented and wrong -- above a define that had been 1
 * since the Sail configuration was corrected: the failures had been the
 * golden model trapping on amocas because guest RAM was declared
 * `AMOArithmetic`, and never this code. docs/TODO.md repeated the claim.
 * A comment about a default is checked by reading the default.
 */
#ifndef RV_EXT_ZACAS
#define RV_EXT_ZACAS 1
#endif
/*
 * Zicond: czero.eqz and czero.nez. Two instructions, lowered natively by
 * the translator as a compare, a negate and an AND -- there is no branch
 * in it on either host.
 */
#ifndef RV_EXT_ZICOND
#define RV_EXT_ZICOND 1
#endif
/*
 * Zalasr: load-acquire and store-release (lb/lh/lw.aq[rl],
 * sb/sh/sw.[aq]rl). One hart and no store buffer, so the ordering they
 * ask for is the ordering there already is; what they add over a plain
 * load and store is that the address must be naturally aligned whatever
 * RV_MISALIGNED_OK says, and that the encodings exist.
 */
#ifndef RV_EXT_ZALASR
#define RV_EXT_ZALASR RV_EXT_A
#endif
/*
 * Zawrs: wrs.nto and wrs.sto, the polite way to wait for a reservation
 * to be broken. See the SYSTEM case in rv_interp.c for what "wait" means
 * with one hart.
 */
#ifndef RV_EXT_ZAWRS
#define RV_EXT_ZAWRS RV_EXT_A
#endif
/*
 * Zihpm: mhpmcounter3-31, their event selectors and the unprivileged
 * shadows. Real counters rather than the read-only zeros the privileged
 * spec permits; what they can count is in rv_csr.h. 352 bytes of hart
 * state, at the end of it.
 */
#ifndef RV_EXT_ZIHPM
#define RV_EXT_ZIHPM RV_EXT_ZICNTR
#endif
/*
 * Each of the three leans on state another extension owns -- the
 * reservation for the first two, mcounteren and mcountinhibit for the
 * third -- and says so here rather than failing to compile somewhere
 * that does not mention the reason.
 */
#if RV_EXT_ZALASR && !RV_EXT_A
#error "RV_EXT_ZALASR requires RV_EXT_A"
#endif
#if RV_EXT_ZAWRS && !RV_EXT_A
#error "RV_EXT_ZAWRS requires RV_EXT_A"
#endif
#if RV_EXT_ZIHPM && !RV_EXT_ZICNTR
#error "RV_EXT_ZIHPM requires RV_EXT_ZICNTR"
#endif
/*
 * Zihintpause and Zihintntl have no gate, deliberately. Both are hints
 * carved out of encodings the base ISA already executes as no-ops --
 * `pause` is a FENCE with an otherwise unused predecessor set, and
 * `ntl.*` are ADDs into x0 -- so there is nothing a macro could switch
 * off, and one that existed only to edit the ISA string would be a flag
 * nothing reads.
 */
#ifndef RV_EXT_ZBB
#define RV_EXT_ZBB 1 /* basic bit manipulation */
#endif
#ifndef RV_EXT_ZCB
#define RV_EXT_ZCB 1 /* extra compressed loads/stores and ALU ops */
#endif
#ifndef RV_EXT_ZBA
#define RV_EXT_ZBA 1 /* address generation: sh1add/sh2add/sh3add */
#endif
#ifndef RV_EXT_ZBC
#define RV_EXT_ZBC 1 /* carry-less multiply */
#endif
#ifndef RV_EXT_ZBS
#define RV_EXT_ZBS 1 /* single-bit: bset/bclr/binv/bext */
#endif
/*
 * Physical memory protection. Costs nothing until a guest locks an entry:
 * an unlocked entry does not restrict M-mode, so the access path skips the
 * check entirely while none is locked. See src/core/rv_pmp.c.
 */
/*
 * User mode. A second privilege level is what makes PMP able to deny
 * anything at all: in M-mode an unlocked entry restricts nothing and
 * matching no entry permits, and both invert below M.
 *
 * S-mode is separate (RV_EXT_S) and implies this one: the privileged spec
 * has no configuration with S and without U.
 */
#ifndef RV_EXT_U
#define RV_EXT_U 1
#endif

/*
 * Supervisor mode.
 *
 * Address translation is Bare only: satp.MODE is WARL and accepts nothing
 * but 0. Sv32 would put a page-table walk on the fetch and access paths,
 * which is the most expensive place in this emulator to add anything --
 * the measurements in CLAUDE.md are all about exactly that -- and it is a
 * separate piece of work rather than a corner of this one. Everything that
 * does not need a page table is here: the third privilege level, the S
 * bank of CSRs, trap delegation, SRET, and the TVM/TW/TSR traps.
 */
#ifndef RV_EXT_S
#define RV_EXT_S 1
#endif
#if RV_EXT_S && !RV_EXT_U
#error "S-mode requires U-mode"
#endif

/*
 * Sv32 address translation.
 *
 * A two-level page walk on the fetch and access paths, which is the most
 * expensive place in this emulator to put anything -- so it is gated on a
 * single flag the way PMP is, and backed by a TLB. With satp in Bare mode,
 * or in M-mode, the cost is one predictable branch.
 */
#ifndef RV_EXT_SV32
#define RV_EXT_SV32 RV_EXT_S
#endif
#if RV_EXT_SV32 && !RV_EXT_S
#error "Sv32 requires S-mode"
#endif

/*
 * Translation lookaside buffer, direct mapped, one entry per 4 KiB page.
 *
 * Not an optimisation to add later: without it every guest load costs two
 * more loads to walk the table, and every fetch likewise. Sized in entries;
 * each is 12 bytes.
 */
/* Sv32 pages are 4 KiB. */
#define RV_PAGE_SIZE 4096u

#ifndef RV_TLB_ENTRIES
#define RV_TLB_ENTRIES 32u
#endif

#ifndef RV_EXT_PMP
#define RV_EXT_PMP 1
#endif
#ifndef RV_PMP_ENTRIES
#define RV_PMP_ENTRIES 16u
#endif

/*
 * Sdtrig debug triggers. Like PMP, free until software arms one.
 */
#ifndef RV_EXT_SDTRIG
#define RV_EXT_SDTRIG 1
#endif
#ifndef RV_TRIG_COUNT
#define RV_TRIG_COUNT 2u
#endif

#ifndef RV_EXT_ZICBOM
#define RV_EXT_ZICBOM 1 /* cbo.clean / cbo.inval / cbo.flush */
#endif
#ifndef RV_EXT_ZICBOZ
#define RV_EXT_ZICBOZ 1 /* cbo.zero */
#endif

/*
 * Cache block size reported through the Zicboz CSR and used as the
 * granule for every CBO. 32 bytes matches the Cortex-M7 L1 line size.
 * Must be a power of two.
 */
#ifndef RV_CACHE_BLOCK_SIZE
#define RV_CACHE_BLOCK_SIZE 32u
#endif

/* ------------------------------------------------------------------ */
/* Memory access                                                       */
/* ------------------------------------------------------------------ */

/*
 * Misaligned load/store support. The RISC-V spec permits an implementation
 * to either handle misaligned accesses in hardware or raise a misaligned
 * exception. We raise, which matches most embedded RISC-V cores and keeps
 * the memory path branch-free. Set to 1 to emulate them by splitting.
 */
#ifndef RV_MISALIGNED_OK
#define RV_MISALIGNED_OK 0
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
#define RV_ENABLE_ECALL_HOOK 1
#endif

/*
 * Place the interpreter's run loop in a .ramfunc section so it executes
 * from SRAM rather than flash. On a part with wait states (5 at 180 MHz on
 * the STM32F446) the dispatch loop is the hottest code in the system and
 * pays them on every miss of the flash accelerator's small cache.
 *
 * Costs a few KiB of RAM that would otherwise go to the guest, and needs a
 * link script that places .ramfunc in RAM with a flash load address, so it
 * is off by default and enabled per platform.
 */
#ifndef RV_INTERP_RAMFUNC
#define RV_INTERP_RAMFUNC 0
#endif

/*
 * Re-evaluate interrupt delivery only when something could have changed
 * it, rather than on every instruction. See rv_hart_t::irq_dirty.
 */
#ifndef RV_LAZY_IRQ_CHECK
#define RV_LAZY_IRQ_CHECK 1
#endif

/*
 * Build the Thumb-2 JIT backend. Requires an ARM host and a RAM code
 * cache; the interpreter remains available and handles every encoding the
 * translator does not cover, so this is a speed option, not a feature one.
 */
/*
 * Count how often a block reads the registers a per-block cache would
 * hold. Translation-time only, but it is measurement scaffolding rather
 * than a feature, so it is off unless asked for.
 */
#ifndef RV_JIT_HOT_REG_STATS
#define RV_JIT_HOT_REG_STATS 0
#endif

/*
 * Seven macros used to be defaulted here: RV_JIT_LOOP_CHAIN,
 * RV_JIT_LOOP_CAP, RV_JIT_INLINE_PERIPH, RV_JIT_PT_MAX_HOLES,
 * RV_JIT_PT_ARM_AT, RV_JIT_ELIDE_LD and RV_JIT_ELIDE_ST, each under a
 * long note on what it was worth.
 *
 * They configured the hand-written Thumb-2 translator. That translator
 * was replaced by the shared IR backends and nothing has read any of
 * them since -- while four CMake options went on feeding them and the
 * documents went on quoting what they bought. A flag nothing reads is
 * the marker of a mechanism that is not there.
 *
 * What is live: the loop cap is EMU_JIT_LOOP_CAP, in emu/emu_jit.h,
 * because it is the framework's and every frontend's; back-edge
 * chaining and the reload elision are unconditional in the backends;
 * and the inlined peripheral window does not exist in the IR backends
 * at all -- only guest RAM is inlined (emu_ir_fastmem_t). The
 * measurements those notes carried are in docs/jit/tuning.md, marked as
 * belonging to the translator they were taken on.
 */

/*
 * Whether there is a JIT is `EMU_HAVE_JIT`, in emu/emu_jit.h. There is no
 * RV32 spelling of it, deliberately.
 *
 * There were three: `RV_ENABLE_JIT` came from CMake, and
 * `RV_JIT_X86_64`/`RV_JIT_THUMB2` were derived here from the *host alone*
 * -- so `-DEMU_JIT=OFF` still declared a backend that nothing compiled and
 * the link failed. Reconciling "asked for" with "possible" has to happen
 * in exactly one place, and that place is not per frontend: the same
 * x86-64 emitter serves RV32 and G4MH, so the question was never an RV32
 * question. See BUILD.md for the whole table.
 *
 * Getting it wrong used to be invisible for a reason worth keeping: the
 * only thing stopping a Thumb-2 emitter being selected on x86 was that the
 * host platform happened never to select it. A capability that depends on
 * nobody exercising it is not a capability that is off.
 */
#include "emu/emu_jit.h"

/* ------------------------------------------------------------------ */
/* Debug / diagnostics                                                 */
/* ------------------------------------------------------------------ */

/* Build the disassembler (costs ~3 KiB of flash; useful for tracing). */
#ifndef RV_ENABLE_DISASM
#define RV_ENABLE_DISASM 1
#endif

/* ------------------------------------------------------------------ */
/* Identification (read back through the M-mode ID CSRs)               */
/* ------------------------------------------------------------------ */

#ifndef RV_MVENDORID
#define RV_MVENDORID 0u /* 0 = non-commercial implementation */
#endif
#ifndef RV_MARCHID
#define RV_MARCHID 0u
#endif
#ifndef RV_MIMPID
#define RV_MIMPID 0x00010000u /* rv32cortex-m v1.0 */
#endif

#endif /* RV32_RV_CONFIG_H */
