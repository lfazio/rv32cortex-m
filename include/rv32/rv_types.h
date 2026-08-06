/* SPDX-License-Identifier: Apache-2.0 */
/*
 * rv_types.h - RISC-V trap causes and privilege levels.
 *
 * The ISA-agnostic vocabulary -- integer types, bit helpers, bus fault
 * kinds, run states -- lives in emu/emu_types.h. This header holds only
 * what is specific to the RISC-V frontend.
 */
#ifndef RV32_RV_TYPES_H
#define RV32_RV_TYPES_H

#include "emu/emu_types.h"

#include "rv_config.h"   /* RV_EXT_U, for RV_PRIV_LEAST below */

/* ------------------------------------------------------------------ */
/* Exception / trap causes (RISC-V privileged spec, mcause low bits)   */
/* ------------------------------------------------------------------ */

/*
 * Cause code 0 is a real exception (instruction address misaligned), so a
 * sentinel outside the encodable range marks "no trap". Functions that can
 * fault return rv_exc_t and callers branch on != RV_EXC_NONE.
 */
typedef uint32_t rv_exc_t;

#define RV_EXC_NONE                 0xFFFFFFFFu

#define RV_EXC_INSN_MISALIGNED      0u
#define RV_EXC_INSN_ACCESS_FAULT    1u
#define RV_EXC_ILLEGAL_INSN         2u
#define RV_EXC_BREAKPOINT           3u
#define RV_EXC_LOAD_MISALIGNED      4u
#define RV_EXC_LOAD_ACCESS_FAULT    5u
#define RV_EXC_STORE_MISALIGNED     6u
#define RV_EXC_STORE_ACCESS_FAULT   7u
#define RV_EXC_ECALL_U              8u
#define RV_EXC_ECALL_S              9u
#define RV_EXC_ECALL_M              11u
#define RV_EXC_INSN_PAGE_FAULT      12u
#define RV_EXC_LOAD_PAGE_FAULT      13u
#define RV_EXC_STORE_PAGE_FAULT     15u

/*
 * Map a bus fault onto the RISC-V cause that reports it.
 *
 * The bus deals in regions, permissions and access widths, and says which
 * *kind* of access it refused; naming the resulting trap is the frontend's
 * job, and this is where the RISC-V frontend does it. Only ever reached on
 * a fault -- callers test against EMU_FAULT_NONE first and translate
 * second -- so the table lookup stays off the hot path.
 */
static EMU_ALWAYS_INLINE rv_exc_t rv_exc_from_fault(emu_fault_t f)
{
    static const uint8_t cause[] = {
        [EMU_FAULT_NONE]  = 0u,   /* never read; the test below shorts it */
        [EMU_FAULT_FETCH] = RV_EXC_INSN_ACCESS_FAULT,
        [EMU_FAULT_LOAD]  = RV_EXC_LOAD_ACCESS_FAULT,
        [EMU_FAULT_STORE] = RV_EXC_STORE_ACCESS_FAULT,
    };
    return (f == EMU_FAULT_NONE) ? RV_EXC_NONE : cause[f];
}

/* Interrupt cause codes (mcause with the MSB set). */
#define RV_INT_S_SOFT               1u
#define RV_INT_S_TIMER              5u
#define RV_INT_S_EXT                9u
#define RV_INT_M_SOFT               3u
#define RV_INT_M_TIMER              7u
#define RV_INT_M_EXT                11u

#define RV_CAUSE_INTERRUPT          0x80000000u

/* ------------------------------------------------------------------ */
/* Privilege levels                                                    */
/* ------------------------------------------------------------------ */

#define RV_PRIV_U                   0u
#define RV_PRIV_S                   1u
#define RV_PRIV_M                   3u

/*
 * What MRET leaves in MPP: the least-privileged mode the implementation
 * supports, which the privileged spec requires rather than merely allows.
 */
#if RV_EXT_U
#  define RV_PRIV_LEAST             RV_PRIV_U
#else
#  define RV_PRIV_LEAST             RV_PRIV_M
#endif

#endif /* RV32_RV_TYPES_H */
