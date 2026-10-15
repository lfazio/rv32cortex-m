/* SPDX-License-Identifier: Apache-2.0 */
/*
 * emu_ir_bytes.h - where the emitted bytes go, per IR operation.
 *
 * Each backend notes the bytes `lower_one` emitted for every operation it
 * lowers; ir_translate brackets the block and keeps the notes only if the
 * whole block lowered, so a translation that overflowed or declined
 * leaves nothing behind. What is left over in a block -- the frame, the
 * exit stubs, the FP setup -- is counted as `frame`.
 *
 * It was thirty lines around the call to lower_one, written on x86-64,
 * measured once and not kept: "a memory access is a hundred bytes" stood
 * in docs/backend/x86_64.md as the reason to change the Thumb-2 backend,
 * and nobody had measured Thumb-2 at all. Code size is what sets JIT
 * performance on the board -- the cache is the guest's RAM -- so the
 * instrument has to run there, which is why the totals go to the
 * session report rather than to stderr.
 *
 * Counted per *translation*, not per block entry. On a board that
 * retranslates, a hot block is counted every time it is built again,
 * which is the right weighting for "what fills the cache" and the wrong
 * one for "what runs".
 *
 * Off by default (EMU_JIT_BYTE_STATS); everything here compiles away.
 */
#ifndef EMU_IR_BYTES_H
#define EMU_IR_BYTES_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef EMU_JIT_BYTE_STATS
#define EMU_JIT_BYTE_STATS 0
#endif

#if EMU_JIT_BYTE_STATS

/* A block is about to be lowered: forget any notes not committed. */
void emu_ir_bytes_begin(void);

/* `op` (an emu_ir_op_t) emitted `bytes`. */
void emu_ir_bytes_op(uint32_t op, uint32_t bytes);

/* The block lowered whole: `guest` instructions in `bytes` of code. */
void emu_ir_bytes_commit(uint32_t guest, uint32_t bytes);

typedef struct emu_ir_bytes_row {
    const char *name;
    uint64_t count;
    uint64_t bytes;
} emu_ir_bytes_row_t;

typedef struct emu_ir_bytes_totals {
    uint64_t blocks;
    uint64_t guest; /* guest instructions translated */
    uint64_t bytes; /* everything emitted for them */
    uint64_t frame; /* bytes no single operation emitted */
} emu_ir_bytes_totals_t;

/*
 * The totals, and the operation rows sorted by bytes, largest first.
 * Returns the number of rows written, at most `max`; rows with no count
 * are left out.
 */
uint32_t emu_ir_bytes_report(emu_ir_bytes_totals_t *totals,
                             emu_ir_bytes_row_t *rows, uint32_t max);

#endif /* EMU_JIT_BYTE_STATS */

#ifdef __cplusplus
}
#endif

#endif /* EMU_IR_BYTES_H */
