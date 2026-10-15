/* SPDX-License-Identifier: Apache-2.0 */
/*
 * emu_ir_bytes.c - emitted bytes per IR operation. See emu_ir_bytes.h.
 */

#include "emu/emu_ir_bytes.h"

#if EMU_JIT_BYTE_STATS

#include "emu/emu_ir.h"
#include "emu/emu_types.h"

#include <string.h>

/*
 * By enumerator, so a reordered enum still names each row correctly. An
 * operation added without a name is printed by number rather than
 * dropped: a row that silently vanishes reads as an operation nothing
 * emits.
 */
static const char *const k_names[EMU_IR_OP_COUNT] = {
    [EMU_IR_NOP] = "NOP",
    [EMU_IR_GET] = "GET",
    [EMU_IR_PUT] = "PUT",
    [EMU_IR_CONST] = "CONST",
    [EMU_IR_MOV] = "MOV",
    [EMU_IR_ADD] = "ADD",
    [EMU_IR_SUB] = "SUB",
    [EMU_IR_AND] = "AND",
    [EMU_IR_OR] = "OR",
    [EMU_IR_XOR] = "XOR",
    [EMU_IR_SHL] = "SHL",
    [EMU_IR_SHR] = "SHR",
    [EMU_IR_SAR] = "SAR",
    [EMU_IR_ROTL] = "ROTL",
    [EMU_IR_MUL] = "MUL",
    [EMU_IR_MULHS] = "MULHS",
    [EMU_IR_MULHU] = "MULHU",
    [EMU_IR_DIVS] = "DIVS",
    [EMU_IR_DIVU] = "DIVU",
    [EMU_IR_REMS] = "REMS",
    [EMU_IR_REMU] = "REMU",
    [EMU_IR_MAC] = "MAC",
    [EMU_IR_ADDI] = "ADDI",
    [EMU_IR_ANDI] = "ANDI",
    [EMU_IR_ORI] = "ORI",
    [EMU_IR_XORI] = "XORI",
    [EMU_IR_SHLI] = "SHLI",
    [EMU_IR_SHRI] = "SHRI",
    [EMU_IR_SARI] = "SARI",
    [EMU_IR_ROTLI] = "ROTLI",
    [EMU_IR_NEG] = "NEG",
    [EMU_IR_NOT] = "NOT",
    [EMU_IR_BSWAP32] = "BSWAP32",
    [EMU_IR_BSWAP16] = "BSWAP16",
    [EMU_IR_HSWAP] = "HSWAP",
    [EMU_IR_CLZ] = "CLZ",
    [EMU_IR_CTZ] = "CTZ",
    [EMU_IR_POPCNT] = "POPCNT",
    [EMU_IR_BEXT] = "BEXT",
    [EMU_IR_BSET] = "BSET",
    [EMU_IR_BCLR] = "BCLR",
    [EMU_IR_BINV] = "BINV",
    [EMU_IR_BITOP_SET] = "BITOP_SET",
    [EMU_IR_BITOP_CLR] = "BITOP_CLR",
    [EMU_IR_BITOP_INV] = "BITOP_INV",
    [EMU_IR_BITOP_TST] = "BITOP_TST",
    [EMU_IR_SEXT8] = "SEXT8",
    [EMU_IR_SEXT16] = "SEXT16",
    [EMU_IR_ZEXT8] = "ZEXT8",
    [EMU_IR_ZEXT16] = "ZEXT16",
    [EMU_IR_LOAD] = "LOAD",
    [EMU_IR_STORE] = "STORE",
    [EMU_IR_SETF] = "SETF",
    [EMU_IR_GETCOND] = "GETCOND",
    [EMU_IR_SETCC] = "SETCC",
    [EMU_IR_SELECT] = "SELECT",
    [EMU_IR_FGET] = "FGET",
    [EMU_IR_FPUT] = "FPUT",
    [EMU_IR_FADD] = "FADD",
    [EMU_IR_FSUB] = "FSUB",
    [EMU_IR_FMUL] = "FMUL",
    [EMU_IR_FDIV] = "FDIV",
    [EMU_IR_FSQRT] = "FSQRT",
    [EMU_IR_FMA] = "FMA",
    [EMU_IR_FMIN] = "FMIN",
    [EMU_IR_FMAX] = "FMAX",
    [EMU_IR_FSGNJ] = "FSGNJ",
    [EMU_IR_FCMP] = "FCMP",
    [EMU_IR_FCVT_TO_I] = "FCVT_TO_I",
    [EMU_IR_FCVT_FROM_I] = "FCVT_FROM_I",
    [EMU_IR_FCLASS] = "FCLASS",
    [EMU_IR_HELPER] = "HELPER",
    [EMU_IR_HELPER_TRAP] = "HELPER_TRAP",
    [EMU_IR_RETIRE] = "RETIRE",
    [EMU_IR_SETPC] = "SETPC",
    [EMU_IR_EXIT] = "EXIT",
    [EMU_IR_EXIT_IF] = "EXIT_IF",
};

typedef struct {
    uint32_t count;
    uint32_t bytes;
} pending_t;

/* Per core: two cores translating at once must not mix their blocks. */
static EMU_PER_CORE pending_t g_pending[EMU_IR_OP_COUNT];
static EMU_PER_CORE uint32_t g_pending_bytes;

/*
 * Not per core, and so not exact under SMP, where two cores committing
 * at once can lose an increment. It is a histogram built for a single
 * core's measurement; say so rather than pay for atomics in it.
 */
static uint64_t g_count[EMU_IR_OP_COUNT];
static uint64_t g_bytes[EMU_IR_OP_COUNT];
static emu_ir_bytes_totals_t g_totals;

void emu_ir_bytes_begin(void)
{
    memset(g_pending, 0, sizeof(g_pending));
    g_pending_bytes = 0u;
}

void emu_ir_bytes_op(uint32_t op, uint32_t bytes)
{
    if (op >= EMU_IR_OP_COUNT) {
        return;
    }
    g_pending[op].count++;
    g_pending[op].bytes += bytes;
    g_pending_bytes += bytes;
}

void emu_ir_bytes_commit(uint32_t guest, uint32_t bytes)
{
    for (uint32_t i = 0; i < EMU_IR_OP_COUNT; i++) {
        g_count[i] += g_pending[i].count;
        g_bytes[i] += g_pending[i].bytes;
    }
    g_totals.blocks++;
    g_totals.guest += guest;
    g_totals.bytes += bytes;
    /* What the operations did not account for: frame, exits, FP setup. */
    if (bytes > g_pending_bytes) {
        g_totals.frame += bytes - g_pending_bytes;
    }
    emu_ir_bytes_begin();
}

uint32_t emu_ir_bytes_report(emu_ir_bytes_totals_t *totals,
                             emu_ir_bytes_row_t *rows, uint32_t max)
{
    uint32_t n = 0u;

    *totals = g_totals;
    for (uint32_t i = 0; i < EMU_IR_OP_COUNT; i++) {
        if (g_count[i] == 0u) {
            continue;
        }
        /* Insertion by bytes, largest first, keeping the top `max`. */
        uint32_t at = n;

        while (at > 0u && rows[at - 1u].bytes < g_bytes[i]) {
            at--;
        }
        if (at >= max) {
            continue;
        }
        const uint32_t last = (n < max) ? n : max - 1u;

        for (uint32_t k = last; k > at; k--) {
            rows[k] = rows[k - 1u];
        }
        rows[at].name = (k_names[i] != NULL) ? k_names[i] : "?";
        rows[at].count = g_count[i];
        rows[at].bytes = g_bytes[i];
        if (n < max) {
            n++;
        }
    }
    return n;
}

#endif /* EMU_JIT_BYTE_STATS */
