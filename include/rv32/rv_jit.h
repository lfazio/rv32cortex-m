/* SPDX-License-Identifier: Apache-2.0 */
/*
 * rv_jit.h - what the RV32 frontend exposes of its JIT.
 *
 * There is no RV32 translator *to a host* any more. rv_ir.c lowers RV32
 * to the shared IR, src/backend/ lowers that to Thumb-2 or x86-64, and
 * emu_jit.c owns the code cache, the block table and the dispatch --
 * see docs/jit/README.md. What is left here is the backend's name, the
 * two invalidation entry points, and the keys that say what a block is
 * for and whether it is still valid.
 *
 * This header used to describe the hand-written Thumb-2 translator it
 * was written for -- no chaining, a 16-bit LDR per register access --
 * and to declare its code-buffer setter, its statistics and three table
 * sizes, none of which had a definition or a reader left.
 */
#ifndef RV32_RV_JIT_H
#define RV32_RV_JIT_H

#include "rv_types.h"
#include "rv_config.h"
#include "rv_hart.h"
#include "emu/emu_backend.h"

#ifdef __cplusplus
extern "C" {
#endif

#if EMU_HAVE_JIT

/*
 * Bytes of RAM for translated code on a microcontroller, from
 * EMU_JIT_CODE_BYTES. The fallback is only for a build that bypasses
 * CMake; the configured default is 32 KB.
 */
#ifndef RV_JIT_CODE_SIZE
#define RV_JIT_CODE_SIZE (12u * 1024u)
#endif

/* Discard every translation. Cheap; called on reset and on invalidate. */
void rv_jit_flush(void);

/*
 * Retire only the blocks translated from the guest page holding
 * `vaddr`. See rv_mmu_flush_page for why this exists and emu_jit.h for
 * how a block is retired without a reverse index of who jumps into it.
 */
void rv_jit_invalidate_page(uint32_t vaddr);

/*
 * The RV32 JIT's own statistics used to live here, duplicating the
 * framework's and adding nine counters of its own -- helper calls by
 * class, elided loads and stores, passthrough arming, and reads per block
 * of the four registers a per-block cache would hold.
 *
 * **Every one of those nine had zero writers.** They belonged to the
 * hand-written Thumb-2 backend and were left behind when the shared IR
 * framework replaced it: rv_jit_get_stats memset the struct and filled
 * only the fields emu_jit_get_stats already reports, so the firmware
 * printed `helpers muldiv 0 clmul 0 bit 0`, `pt hits 0 armed 0` and
 * `reads/blk sp=- in 0` on every run, for ever. Nine numbers that could
 * not be anything but zero, in a report whose entire purpose is to say
 * whether translation happened -- which is this tree's own rule about
 * checking that an instrument can represent the difference, printed
 * every run and read by nobody.
 *
 * emu_jit_stats_t is the whole of it now. A backend that grows counters
 * of its own should add them there if the dispatch loop can maintain
 * them, and otherwise earn a hook by having something to put in it.
 */

extern const emu_backend_t rv_backend_jit;

/*
 * What a block is *for*: the address space, the privilege, and the state
 * of the FP unit.
 *
 * Each decides what a block at a given address may legally do, and none
 * of them invalidates anything when it changes -- the kernel's blocks are
 * still the kernel's after a switch to a user process, and a block built
 * while the FP unit was on is still right whenever it is on again. So
 * they are part of a block's identity rather than part of the
 * generation, and blocks from every context coexist.
 *
 * The low word is satp with the privilege in 21:20. MODE is bit 31, ASID
 * is 30:22, and this implementation's PPN is 19:0, because the field is
 * WARL and its width follows a 32-bit physical address space -- so 21:20
 * are free and the packing is exact.
 *
 * The high word is the FP unit, and it is here because the low word was
 * full:
 *
 *   FS     an FP instruction is legal only while the unit is on, and the
 *          translator checks that once. A block built while FS was on
 *          must never run while it is off, or three instructions that
 *          must raise illegal-instruction run silently -- which has
 *          happened here before.
 *   frm    the IR resolves a "dynamic" rounding mode at translation, on
 *          purpose, so that a backend without an encoding for one --
 *          neither x86 nor ARM has ties-away -- can decline the block
 *          rather than round differently.
 *
 * **Both used to be in the generation, and that was the udev stall.**
 * Linux clears sstatus.FS on every trap into the kernel and restores it
 * at the return, so every system call and every interrupt taken from a
 * process that had touched a float flipped FS off and on -- and each
 * flip flushed the whole cache. Booting a root filesystem, udev forks
 * processes that all do: 21,949 of 22,126 flushes were FS, the cache
 * held a median of 438 blocks against ~7,000 while the kernel booted,
 * and every other block entry paid a translation. Correct throughout,
 * which is why nothing failed.
 *
 * FS is reduced to *off or not*, not carried as the two-bit field. An FP
 * operation moves it Initial or Clean to Dirty as a side effect, so
 * keeping the field would give every block a second copy for no reason.
 * The accrued flags share fcsr with frm and are not specialised on, so
 * they are not here either.
 *
 * **In this header so that rv_hart_trap can call it.** It used to be
 * static in rv_ir.c and refreshed only on the interpreter fallback, on
 * the argument that every privilege change is an instruction the
 * translator declines. A faulting load is not: rv_ir_load raises the
 * trap from inside the block, the interrupt hook raises one between
 * blocks, and in both cases the dispatch then looked the handler up
 * under the context it had trapped *from*.
 */
static inline uint64_t rv_jit_ctx_key(const rv_hart_t *h)
{
#if RV_EXT_SV32
    uint64_t key = h->satp | ((uint32_t)h->priv << 20);
#else
    uint64_t key = (uint32_t)h->priv;
#endif
#if RV_EXT_F
    const uint32_t fs_off = ((h->mstatus & MSTATUS_FS_MASK) == 0u) ? 1u : 0u;
    const uint32_t frm = (h->fcsr >> 5) & 7u;

    key |= (uint64_t)((fs_off << 3) | frm) << 32;
#endif
    return key;
}


#endif /* EMU_HAVE_JIT */

#ifdef __cplusplus
}
#endif

#endif /* RV32_RV_JIT_H */
