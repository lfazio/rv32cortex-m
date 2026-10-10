/* SPDX-License-Identifier: Apache-2.0 */
/*
 * armv7m_cpu.h - ARMv7E-M as a *guest*.
 *
 * The fourth frontend, and the one with a motive the others did not
 * have: **it is how the Thumb-2 backend becomes testable without
 * hardware.** That backend can only be validated by flashing a board
 * and reading a UART -- three of the JIT bugs this project has found
 * were live for months because no host suite could reach them. With a
 * Cortex-M guest, an x86 host can run the firmware that contains the
 * Thumb-2 emitter, and the emitted code executes inside this
 * interpreter.
 *
 * **Named armv7m, not thumb2, and the prefix matters.** `src/backend/
 * thumb2/` owns every `t2_` symbol in the tree -- t2_add, t2_emit32,
 * t2_patch_branch -- and those are an *encoder*. This is a decoder.
 *
 * **The reference is a real Cortex-M7, not the manual alone.** Where the
 * architecture leaves a choice to the implementation, this frontend makes
 * the one the Nucleo-F746ZG's core makes -- four priority bits, eight MPU
 * regions, 128 interrupt lines, FPv5 single precision, CPUID 0x410FC271
 * -- because tests/armv7m-diff runs the same programs on that board and
 * compares. A choice made differently would be a mismatch the comparison
 * could not tell from a bug.
 */
#ifndef ARMV7M_CPU_H
#define ARMV7M_CPU_H

#include "emu/emu_bus.h"
#include "emu/emu_cpu.h"

#include "armv7m/armv7m_decode.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ARMV7M_NREGS 16u /* r0-r12, sp, lr, pc */
#define ARMV7M_SP 13u
#define ARMV7M_LR 14u
#define ARMV7M_PC 15u

/* APSR, in the bit positions the architecture puts them. */
#define ARMV7M_N (1u << 31)
#define ARMV7M_Z (1u << 30)
#define ARMV7M_C (1u << 29)
#define ARMV7M_V (1u << 28)
#define ARMV7M_Q (1u << 27)
#define ARMV7M_GE_SHIFT 16
#define ARMV7M_GE_MASK (0xFu << ARMV7M_GE_SHIFT)
#define ARMV7M_APSR_MASK 0xF80F0000u

/* EPSR.T -- Thumb state. Clearing it is a UsageFault, not a mode switch:
 * an ARMv7-M core has no ARM state to switch to. */
#define ARMV7M_T (1u << 24)

/* IPSR: the number of the exception being handled, 0 in Thread mode. */
#define ARMV7M_IPSR_MASK 0x1FFu

/*
 * ITSTATE, and it lives **in xpsr at the two places the architecture
 * puts it** rather than in a field of its own: IT[1:0] in bits 26:25 and
 * IT[7:2] in bits 15:10. An exception stacks xPSR whole, ITSTATE
 * included, so keeping one copy is what makes an interrupted IT block
 * resume intact without anything remembering to compose it.
 */
#define ARMV7M_IT_LO_MASK (3u << 25)
#define ARMV7M_IT_HI_MASK (0x3Fu << 10)

static inline uint32_t armv7m_it_get(uint32_t xpsr)
{
    return ((xpsr >> 25) & 3u) | (((xpsr >> 10) & 0x3Fu) << 2);
}

static inline uint32_t armv7m_it_put(uint32_t xpsr, uint32_t it)
{
    return (xpsr & ~(ARMV7M_IT_LO_MASK | ARMV7M_IT_HI_MASK)) |
           ((it & 3u) << 25) | (((it >> 2) & 0x3Fu) << 10);
}

/*
 * In an IT block exactly when the low four bits are non-zero -- the
 * architecture's own test, and not "the condition field is non-zero":
 * EQ is condition 0b0000, so a block led by `IT EQ` would read as no
 * block at all.
 */
static inline bool armv7m_in_it(uint32_t xpsr)
{
    return (armv7m_it_get(xpsr) & 0x0Fu) != 0u;
}

/* CONTROL */
#define ARMV7M_CONTROL_NPRIV 1u
#define ARMV7M_CONTROL_SPSEL 2u
#define ARMV7M_CONTROL_FPCA 4u

/* ---- exception numbers ---- */
#define ARMV7M_EXC_RESET 1u
#define ARMV7M_EXC_NMI 2u
#define ARMV7M_EXC_HARDFAULT 3u
#define ARMV7M_EXC_MEMMANAGE 4u
#define ARMV7M_EXC_BUSFAULT 5u
#define ARMV7M_EXC_USAGEFAULT 6u
#define ARMV7M_EXC_SVCALL 11u
#define ARMV7M_EXC_DEBUGMON 12u
#define ARMV7M_EXC_PENDSV 14u
#define ARMV7M_EXC_SYSTICK 15u
#define ARMV7M_EXC_EXTERNAL 16u

/*
 * 128 interrupt lines, which is what the F746's ICTR reports (INTLINESNUM
 * 3: four banks of 32). The part wires 98 of them; the rest pend and
 * enable like any other, which is also what the board does.
 */
#define ARMV7M_NIRQ 128u

/*
 * ...of which the F746 wires 98, and the rest are RAZ/WI: writing all
 * ones to ISER3 reads back 0x00000003 on the board. ICTR still says four
 * banks, because that is the register's granularity.
 */
#define ARMV7M_NIRQ_IMPL 98u
#define ARMV7M_NEXC (ARMV7M_EXC_EXTERNAL + ARMV7M_NIRQ)
#define ARMV7M_NEXC_WORDS ((ARMV7M_NEXC + 31u) / 32u)

/* Four priority bits, the top four of each byte; the rest read as zero. */
#define ARMV7M_PRIO_BITS 4u
#define ARMV7M_PRIO_MASK 0xF0u

/* Eight MPU regions. */
#define ARMV7M_MPU_REGIONS 8u

/*
 * What an instruction tells the run loop when it cannot complete.
 *
 * Zero is success. Everything else is an exception the instruction
 * raised, and the run loop turns it into one -- with the fault status
 * bits and addresses already recorded in the cpu by whoever detected it.
 */
typedef enum {
    ARMV7M_X_NONE = 0,
    ARMV7M_X_UNDEF,      /* UsageFault, UFSR.UNDEFINSTR            */
    ARMV7M_X_INVSTATE,   /* UsageFault, UFSR.INVSTATE              */
    ARMV7M_X_INVPC,      /* UsageFault, UFSR.INVPC                 */
    ARMV7M_X_NOCP,       /* UsageFault, UFSR.NOCP                  */
    ARMV7M_X_UNALIGNED,  /* UsageFault, UFSR.UNALIGNED             */
    ARMV7M_X_DIVBYZERO,  /* UsageFault, UFSR.DIVBYZERO             */
    ARMV7M_X_MEMMANAGE,  /* MMFSR bits already set                 */
    ARMV7M_X_BUSFAULT,   /* BFSR bits already set                  */
    ARMV7M_X_SVC,        /* SVCall, raised by SVC                  */
    ARMV7M_X_BKPT,       /* BKPT: the emulator's "stop here"       */
    ARMV7M_X_LOCKUP,     /* a fault nothing can take               */
} armv7m_exc_t;

typedef struct armv7m_cpu {
    /*
     * Hot state first. **pc is r[15], not a separate field**: in this
     * architecture it is a general register that happens to be the
     * program counter, and every instruction that writes it is a branch.
     *
     * r[13] is the *active* stack pointer. The banked pair is msp/psp,
     * and whichever is active is mirrored here -- so an ordinary
     * instruction reads and writes sp like any register, and only a
     * change of mode or of CONTROL.SPSEL has to swap.
     */
    uint32_t r[ARMV7M_NREGS];
    uint32_t xpsr;

    emu_bus_t *bus;
    uint32_t coreid;
    emu_state_t state;

    uint32_t msp;
    uint32_t psp;
    uint32_t control;

    /*
     * BASEPRI keeps all eight bits it is written -- the board reads back
     * 0x13 after writing 0x13 -- but only the implemented four take part
     * in masking: 0x13 masks priority 0x10 exactly as 0x10 does, measured.
     *
     * The three masks, each changing the *execution priority* rather
     * than gating anything directly: PRIMASK raises it to 0, FAULTMASK to
     * -1, BASEPRI to its own value. That is what lets FAULTMASK turn a
     * configurable fault into a HardFault -- the fault can no longer
     * preempt, so it escalates -- instead of being a second switch beside
     * the first.
     */
    uint32_t primask;
    uint32_t faultmask;
    uint32_t basepri;

    /* ---- the exception model ---- */
    uint32_t pending[ARMV7M_NEXC_WORDS];
    uint32_t active[ARMV7M_NEXC_WORDS];
    uint32_t enabled[ARMV7M_NIRQ / 32u]; /* external lines only */
    uint8_t prio[ARMV7M_NEXC];           /* configured, top 4 bits */
    /* Level-sensitive lines as the devices drive them. */
    uint32_t line[ARMV7M_NIRQ / 32u];

    /* ---- the System Control Block ---- */
    uint32_t vtor;
    uint32_t prigroup;
    uint32_t scr;
    uint32_t ccr;
    uint32_t shcsr; /* the enable bits; pending/active are derived */
    uint32_t cfsr;
    uint32_t hfsr;
    uint32_t mmfar;
    uint32_t bfar;
    uint32_t cpacr;

    /* ---- SysTick ---- */
    uint32_t systick_ctrl;
    uint32_t systick_reload;
    uint32_t systick_value;
    bool systick_countflag;

    /* ---- MPU ---- */
    uint32_t mpu_ctrl;
    uint32_t mpu_rnr;
    uint32_t mpu_rbar[ARMV7M_MPU_REGIONS];
    uint32_t mpu_rasr[ARMV7M_MPU_REGIONS];

    /*
     * The local exclusive monitor: open after an LDREX, closed by a
     * STREX, a CLREX or an exception. Whether a STREX to a *different*
     * address than the LDREX succeeds is IMPLEMENTATION DEFINED, and the
     * board says it does: the Cortex-M7's local monitor keeps no address,
     * measured by the c_excl cases in tests/armv7m-diff (`ldrex [r8,
     * #196]; strex [r8, #204]` returns 0). `excl_addr` is kept for
     * reports only.
     */
    bool excl_open;
    uint32_t excl_addr;

    /* ---- the FPU (FPv5, single precision) ---- */
    uint32_t s[32];
    uint32_t fpscr;
    uint32_t fpccr;
    uint32_t fpcar;
    uint32_t fpdscr;

    /*
     * Set by the memory helper for the duration of one bus access: the
     * System Control Space reads it to refuse unprivileged accesses,
     * which the architecture faults rather than ignores.
     */
    bool access_priv;

    uint64_t retired;

    /* The per-instruction trace hook; only called under EMU_ENABLE_TRACE. */
    emu_trace_fn trace;
    void *trace_user;

    /*
     * ---- what a translated block depends on ----
     *
     * `jit_ctx` is what a block is *for*: the Thumb bit, whether an IT
     * block is open, Handler mode and privilege. Blocks built under
     * different values coexist; see armv7m_jit_ctx.
     *
     * `jit_gen` is what makes every block wrong at once: it moves when
     * an MPU register is written, because a block records that its
     * instructions could be fetched.
     *
     * The `smc_*` fields say where translated code that the guest can
     * *write* came from. Nothing on this architecture tells the core
     * that instructions changed -- with no cache enabled a Cortex-M
     * executes what it just stored -- so a store into that range has to
     * be noticed here. `smc_span` is zero until a block is built from
     * RAM, which is what keeps the test on the store path to one
     * compare for every guest that runs from flash.
     */
    uint64_t jit_ctx;
    uint32_t jit_gen;

    /*
     * N, Z, C and V while translated code is running -- one word each,
     * read lazily, instead of four bits packed into xPSR:
     *
     *   N   bit 31 of jit_nf
     *   Z   jit_zf == 0
     *   C   jit_cf, which is 0 or 1
     *   V   bit 31 of jit_vf
     *
     * So an instruction that sets N and Z from its result stores the
     * result twice and computes nothing, and `bne` is a compare of a
     * word against zero. Packed, every flag-setting instruction whose
     * flags *might* be observed -- by the next load faulting, say -- was
     * seventeen operations of shifting and masking.
     *
     * `jit_split` says which copy is current. Set, these four are and
     * xPSR's top nibble is stale; clear, xPSR is and these are stale.
     * Every path from translated code into C that reads the flags packs
     * first, and every path back unpacks -- see armv7m_flags_pack. The
     * interpreter never sees the split form and was not changed for it.
     */
    uint32_t jit_nf;
    uint32_t jit_zf;
    uint32_t jit_cf;
    uint32_t jit_vf;
    bool jit_split;

    /*
     * Set whenever something happened that could make an exception
     * takeable, and cleared by the dispatcher when it looks and finds
     * none. While it is clear the dispatcher does not look.
     *
     * "Could" is deliberately wide: a pending bit set, any write to the
     * System Control Space, anything the interpreter did. What it must
     * never be is narrow -- a missed setter is an interrupt that waits
     * for an unrelated event -- and the test for that is CoreMark's
     * clock, which is a SysTick handler and stops counting if one is
     * lost.
     */
    volatile bool irq_maybe;
    uint32_t smc_lo;
    uint32_t smc_span;
    uint32_t smc_base;     /* what the bitmap is relative to          */
    uint32_t smc_bits[32]; /* one bit per 4 KiB page above smc_base   */
    bool smc_wide;         /* code outside the bitmap's 4 MiB: no map */
    /*
     * Non-zero while any translated code came from writable memory. An
     * inlined store tests this byte and takes the checked path instead,
     * because the inlined one would not notice it had written over an
     * instruction.
     */
    uint8_t smc_watch;

    /*
     * Why the core stopped, if it stopped -- a lockup, or the emulator's
     * own BKPT convention. Exceptions an instruction raises are *taken*,
     * through the vector table, exactly as on the board; only what no
     * handler can take ends up here.
     */
    uint32_t fault_pc;
    uint32_t fault_insn;
    bool faulted;
    bool lockup;
} armv7m_cpu_t;

/* ------------------------------------------------------------------ */
/* Decoding                                                            */
/* ------------------------------------------------------------------ */

/*
 * The System Control Space, at the address every Cortex-M puts it. Fixed
 * by the architecture rather than by a part, which is why it is here and
 * not in a platform header.
 */
#define ARMV7M_SCS_BASE 0xE000E000u
#define ARMV7M_SCS_SIZE 0x1000u

#define ARMV7M_EXC_RETURN_MASK 0xF0000000u

extern const struct emu_dev_ops armv7m_scs_ops;

/* ---- shared between the interpreter, the exception model and the SCS */

/* Thread mode is IPSR == 0. */
static inline bool armv7m_handler_mode(const armv7m_cpu_t *c)
{
    return (c->xpsr & ARMV7M_IPSR_MASK) != 0u;
}

static inline bool armv7m_privileged(const armv7m_cpu_t *c)
{
    return armv7m_handler_mode(c) ||
           (c->control & ARMV7M_CONTROL_NPRIV) == 0u;
}

/* Memory, through the MPU and the bus. `unpriv` forces an unprivileged
 * check, which is what LDRT/STRT do. */
#define ARMV7M_ACC_READ 0u
#define ARMV7M_ACC_WRITE 1u
#define ARMV7M_ACC_FETCH 2u

armv7m_exc_t armv7m_mem_read(armv7m_cpu_t *c, uint32_t addr, uint32_t size,
                             bool unpriv, uint32_t *out);
armv7m_exc_t armv7m_mem_write(armv7m_cpu_t *c, uint32_t addr, uint32_t size,
                              bool unpriv, uint32_t v);
armv7m_exc_t armv7m_mem_fetch16(armv7m_cpu_t *c, uint32_t addr, uint16_t *out);

void armv7m_reset_state(armv7m_cpu_t *c);
int32_t armv7m_exec_priority(const armv7m_cpu_t *c);
void armv7m_set_pending(armv7m_cpu_t *c, uint32_t exc, bool on);
bool armv7m_is_pending(const armv7m_cpu_t *c, uint32_t exc);
bool armv7m_is_active(const armv7m_cpu_t *c, uint32_t exc);
uint32_t armv7m_pending_exc(const armv7m_cpu_t *c, bool *preempts);
uint32_t armv7m_rettobase(const armv7m_cpu_t *c);
bool armv7m_take(armv7m_cpu_t *c, uint32_t exc, uint32_t return_pc);
armv7m_exc_t armv7m_exc_return(armv7m_cpu_t *c, uint32_t exc_return);
void armv7m_raise(armv7m_cpu_t *c, armv7m_exc_t x, uint32_t pc);
void armv7m_set_sp(armv7m_cpu_t *c, uint32_t which, uint32_t v);
uint32_t armv7m_get_sp(const armv7m_cpu_t *c, uint32_t which);
void armv7m_write_control(armv7m_cpu_t *c, uint32_t v);
void armv7m_systick_tick(armv7m_cpu_t *c, uint32_t insns);
void armv7m_set_irq(armv7m_cpu_t *c, uint32_t source, bool level);

/* The coprocessor space -- the FPU. Returns the exception to raise. */
armv7m_exc_t armv7m_fpu_exec(armv7m_cpu_t *c, uint16_t w0, uint16_t w1,
                             uint32_t pc);
bool armv7m_fpu_enabled(const armv7m_cpu_t *c);

emu_run_reason_t armv7m_run(armv7m_cpu_t *c, uint32_t budget,
                            uint32_t *retired);

/* ------------------------------------------------------------------ */
/* For a translating backend                                           */
/* ------------------------------------------------------------------ */

/*
 * A block's identity beyond its address. Each of these changes what the
 * same bytes at the same pc *do*, without making any existing block
 * wrong:
 *
 *   T        clear, the next fetch is an INVSTATE fault and no block may
 *            run at all
 *   IT       inside an IT block the instructions are conditional, and a
 *            16-bit ALU instruction stops setting flags
 *   handler  BX to 0xFxxxxxxx is an exception return there and a branch
 *            to nowhere in Thread mode
 *   priv     which MPU permissions a fetch is checked against
 *
 * Deliberately *not* the flags, the exception number or the rest of
 * xPSR: a key that included them would give every block as many copies
 * as there are flag values.
 */
#define ARMV7M_CTX_T 1u
#define ARMV7M_CTX_IT 2u
#define ARMV7M_CTX_HANDLER 4u
#define ARMV7M_CTX_PRIV 8u

static inline uint64_t armv7m_jit_ctx(const armv7m_cpu_t *c)
{
    return ((c->xpsr & ARMV7M_T) ? ARMV7M_CTX_T : 0u) |
           (armv7m_in_it(c->xpsr) ? ARMV7M_CTX_IT : 0u) |
           (armv7m_handler_mode(c) ? ARMV7M_CTX_HANDLER : 0u) |
           (armv7m_privileged(c) ? ARMV7M_CTX_PRIV : 0u);
}

/*
 * The flags, from the split form translated code keeps into xPSR, and
 * back. Both are no-ops when the form they would produce is already the
 * current one, so a caller does not have to know which it is.
 */
static inline void armv7m_flags_pack(armv7m_cpu_t *c)
{
    if (c->jit_split) {
        c->xpsr = (c->xpsr & 0x0FFFFFFFu) | (c->jit_nf & 0x80000000u) |
                  ((c->jit_zf == 0u) ? 0x40000000u : 0u) |
                  ((c->jit_cf != 0u) ? 0x20000000u : 0u) |
                  ((c->jit_vf >> 31) << 28);
        c->jit_split = false;
    }
}

static inline void armv7m_flags_unpack(armv7m_cpu_t *c)
{
    if (!c->jit_split) {
        c->jit_nf = c->xpsr;
        c->jit_zf = (c->xpsr & 0x40000000u) ? 0u : 1u;
        c->jit_cf = (c->xpsr >> 29) & 1u;
        c->jit_vf = (c->xpsr & 0x10000000u) << 3;
        c->jit_split = true;
    }
}

/*
 * Would a fetch at `addr` succeed? The translator's question, and it
 * must be asked without armv7m_mem_fetch16's side effect: that one
 * records IACCVIOL or IBUSERR in CFSR, which is right for an instruction
 * the core is about to execute and wrong for one a translator merely
 * looked at while deciding where a block ends.
 */
bool armv7m_fetch_probe(const armv7m_cpu_t *c, uint32_t addr, uint16_t *out);

/*
 * One instruction, exactly as the run loop executes it: the IT condition,
 * the instruction, ITAdvance, and the exception if it raised one. What
 * it leaves out is everything *around* an instruction -- the retired
 * count, SysTick, pending interrupts -- which a translated block
 * accounts for itself.
 *
 * Returns true if the instruction completed, in which case r15 is where
 * execution continues; false if it faulted or stopped the core.
 */
bool armv7m_step_insn(armv7m_cpu_t *c, uint16_t w0, uint16_t w1, uint32_t pc);

/* LDR and STR as the interpreter performs them: MemU, with CCR.UNALIGN_TRP. */
armv7m_exc_t armv7m_load_u(armv7m_cpu_t *c, uint32_t addr, uint32_t size,
                           uint32_t *out);
armv7m_exc_t armv7m_store_u(armv7m_cpu_t *c, uint32_t addr, uint32_t size,
                            uint32_t v);

/* A store landed where translated code came from. */
void armv7m_smc_hit(armv7m_cpu_t *c, uint32_t addr);

static inline void armv7m_wrote(armv7m_cpu_t *c, uint32_t addr)
{
    if (EMU_UNLIKELY((addr - c->smc_lo) < c->smc_span)) {
        armv7m_smc_hit(c, addr);
    }
}

#ifdef __cplusplus
}
#endif

#endif /* ARMV7M_CPU_H */
