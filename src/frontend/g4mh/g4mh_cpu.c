/* SPDX-License-Identifier: Apache-2.0 */
/*
 * g4mh_cpu.c - Core lifecycle, exception entry, and checked memory.
 */

#include "g4mh/g4mh_decode.h"
#include "g4mh/g4mh_cpu.h"
#include "g4mh/g4mh_intc.h"
#include "g4mh/g4mh_memmap.h"

#include <string.h>

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

#if G4MH_EXT_MPU
/*
 * One MPU per core. Not shared: MPM, MPIDX and the entries are all
 * per-PE state on a real part, and sharing them would make one core's
 * MPIDX select which entry another core's LDSR wrote.
 */
static g4mh_mpu_t g_mpu[G4MH_PE_COUNT];
#endif

void g4mh_cpu_init(g4mh_cpu_t *c, emu_bus_t *bus, uint32_t coreid)
{
    memset(c, 0, sizeof(*c));
    c->bus = bus;
    c->coreid = coreid;
#if G4MH_EXT_MPU
    c->mpu = &g_mpu[coreid < G4MH_PE_COUNT ? coreid : 0u];
    c->fast_store_never = 1u;
#endif
    g4mh_cpu_reset(c, 0u);
}

void g4mh_cpu_reset(g4mh_cpu_t *c, uint32_t reset_pc)
{
    memset(c->r, 0, sizeof(c->r));
    memset(c->sr, 0, sizeof(c->sr));
    c->pc = reset_pc;

    /*
     * Reset state per the G4MH manual: interrupts disabled, no exception
     * in progress, machine mode. PSW.ID set is the important half -- a
     * core coming out of reset must not take an interrupt before its
     * handler table exists.
     */
    c->psw = G4MH_PSW_ID;
    c->sr[0][G4MH_SR_PSW] = c->psw;

    /*
     * SPID from the core id, so MPIDn discriminates between PEs rather
     * than every core presenting 0 and the SPID group being a no-op --
     * the product's own reset value, which section 3.10.7 leaves to the
     * part. SVLOCK resets clear, with the rest of sr.
     */
    c->sr[1][G4MH_SR_SPID] = c->coreid & 0x1Fu;
#if G4MH_EXT_MPU
    c->spid = c->sr[1][G4MH_SR_SPID];
#endif

#if G4MH_EXT_MPU
    /*
     * MPM resets to zero, so protection is off and every entry disabled.
     * Reset here as well as at init because a warm reset must not leave
     * a previous guest's areas in force -- the flag and the entries have
     * to move together or a reset would disable checking while leaving
     * the entries that a later MPM write would suddenly apply.
     */
    if (c->mpu != NULL) {
        g4mh_mpu_reset(c->mpu);
    }
    c->mpu_active = false;
#endif
    g4mh_jit_ctx_refresh(c);
    c->jit_gen++;

    /*
     * RBASE is where the reset vector and the exception handlers live.
     * The architecture leaves the reset value to the part; taking it from
     * the reset pc means a guest that never writes RBASE still has its
     * handlers at a defined place -- the top of its own image -- rather
     * than at address zero.
     */
    c->sr[1][G4MH_SR_RBASE] = reset_pc;
    c->sr[1][G4MH_SR_EBASE] = reset_pc;

    /*
     * The PE number, which is what real startup code branches on: every PE
     * comes out of reset at the same address running the same image, so
     * without this a multicore guest cannot tell the cores apart.
     *
     * PE n reports n. The U2B manual §3.2.2.10 has a program identify its
     * core by reading PEID, "a unique number within a multi-processor
     * system", and Table 3.1 numbers the CPUs from 0 -- so the coreid goes
     * in unmodified rather than being placed in a field.
     */
    c->sr[2][G4MH_SR_HTCFG0] = c->coreid;

    /*
     * The priority ceiling comes up permissive-but-bounded: PLMR resets
     * to 16, so priorities 0..15 are acceptable and 16..63 are not, and
     * INTCFG to ULNR = 0xF with EPL and ISPC clear. Leaving PLMR at zero
     * -- which memset would -- masks *every* priority, and a guest that
     * never writes it would take no interrupt at all while every EIC
     * register said it should.
     */
    c->sr[G4MH_SELID_INT][G4MH_SR_PLMR] = G4MH_PLMR_RESET;
    c->sr[G4MH_SELID_INT][G4MH_SR_INTCFG] = G4MH_INTCFG_RESET;

    g4mh_ll_drop(c);
    c->retired = 0u;
    /*
     * **Only PE0 starts at reset release.** The others are held until
     * PE0 asserts their bit in BOOTCTRL (U2B 11.4.79) -- so a multicore
     * guest has a start-up sequence, which is what a real one needs and
     * what this model used to let it skip by starting every PE at once.
     */
    c->state = (c->coreid == 0u) ? EMU_STATE_RUNNING : EMU_STATE_HELD;
    /* Force one evaluation after reset rather than assuming the state. */
    c->irq_dirty = true;

#if EMU_ENABLE_STATS
    c->exc_count = 0u;
#endif
}

void g4mh_cpu_boot(g4mh_cpu_t *c, uint32_t ram_base, uint32_t ram_size)
{
    /*
     * Each PE its own stack. They share the RAM, and handing every PE the
     * same top -- which this did until a guest with real functions ran on
     * three of them -- put each PE's frames on top of the others': a
     * quantum boundary in the middle of a call was enough to corrupt one,
     * and running the cores in parallel made it certain. PE0 keeps the
     * top, so a single-core guest sees no change; PE k starts k strides
     * below it. The bottom half of RAM is left for the image and its
     * data. The RH850 ABI wants 4-byte alignment; 8 costs nothing.
     */
    const uint32_t stride = (ram_size / (2u * G4MH_PE_COUNT)) & ~0xFFFu;

    c->r[G4MH_REG_SP] = (ram_base + ram_size - c->coreid * stride) & ~7u;
    c->r[6] = c->coreid;
    c->r[7] = ram_size;
}

/* ------------------------------------------------------------------ */
/* Exceptions                                                          */
/* ------------------------------------------------------------------ */

bool g4mh_exc_is_fe(g4mh_exc_t cause)
{
    /*
     * FE-level causes are the ones the core cannot be asked to ignore:
     * a system error, a protection violation, a reserved instruction, a
     * misaligned access, an unusable coprocessor. Everything else --
     * TRAP, SYSCALL, the interrupts, and the FP exceptions -- is EI
     * level.
     *
     * **FPE and FXE are EI level**: table 3.103 of R01UH0923EJ0130 saves
     * them to EIPC/EIPSW/EIIC, and table 4.5 of the virtualization manual
     * R01UH0865EJ0140 to HMEI, with NP kept -- two documents agreeing.
     * This listed FPP here, so an FP exception wrote FEPC and set NP.
     *
     * By the code alone: MIP and MDP carry detail in the top half.
     */
    cause = G4MH_EXC_CODE(cause);
    switch (cause) {
    case G4MH_EXC_SYSERR:
    case G4MH_EXC_MIP:
    case G4MH_EXC_MDP:
    case G4MH_EXC_MDP_TABLE:
    case G4MH_EXC_RIE:
    case G4MH_EXC_MAE:
    case G4MH_EXC_UCPOP:
    case G4MH_EXC_UCPOP + 1u: /* CU1, the FXU             */
    case G4MH_EXC_UCPOP + 2u: /* CU2, reserved            */
    case G4MH_EXC_PIE:
        return true;
    default:
        /* FETRAP is the one *software-raised* FE-level cause: that is the
         * whole point of it beside TRAP, which is EI level. FEINT is the
         * asynchronous one, and is FE level for the reason that makes it
         * useful -- PSW.ID does not reach it. */
        return (cause >= G4MH_EXC_FETRAP && cause < G4MH_EXC_FETRAP + 0x10u) ||
               (cause >= G4MH_EXC_FEINT && cause < G4MH_EXC_FEINT + 0x10u);
    }
}

/*
 * Where a handler lives.
 *
 * RBASE normally, EBASE when PSW.EBV is set -- which is the mechanism a
 * system uses to move its vector table after boot without relocating the
 * reset vector. The offset within the table is 0x10 per exception class
 * for the synchronous causes.
 */

/*
 * The direct vector offset for a user interrupt.
 *
 * **By priority, never by channel.** RBASE.RINT collapses all of them
 * onto 0x100; otherwise they are spaced 0x10 apart by priority, and
 * priorities at or above 16 share priority 15's slot (R01UH0923EJ0130
 * table 3.106, note 2). So this axis gives at most sixteen landing pads
 * however many channels exist -- per-channel granularity is what the
 * table reference method is for, and it is an indirection rather than
 * more vectors.
 */
static uint32_t direct_vector_offset(uint32_t base, unsigned priority)
{
    if ((base & G4MH_BASE_RINT) != 0u) {
        return 0x0100u;
    }
    return 0x0100u + (uint32_t)(priority < 16u ? priority : 15u) * 0x10u;
}

static uint32_t handler_address(const g4mh_cpu_t *c, g4mh_exc_t cause)
{
    const uint32_t base = (c->psw & G4MH_PSW_EBV) ? c->sr[1][G4MH_SR_EBASE]
                                                  : c->sr[1][G4MH_SR_RBASE];

    /* RBASE/EBASE carry flags in the low bits; the table is 512-aligned. */
    const uint32_t table = base & ~0x1FFu;

    cause = G4MH_EXC_CODE(cause); /* MIP and MDP carry detail above */
    if (cause >= G4MH_EXC_EIINT_BASE && cause < G4MH_EXC_SYSCALL) {
        /*
         * Only reached for the direct vector method -- the table
         * reference case is resolved in g4mh_cpu_exception, because it
         * reads memory and can fault, which a pure address computation
         * cannot express. Priority is re-derived rather than passed:
         * this is off the delivery path's hot loop and the alternative
         * is a parameter every other caller would have to invent.
         */
        unsigned pri = 15u;
        if (c->intc != NULL) {
            const uint32_t ch = cause - G4MH_EXC_EIINT_BASE;
            (void)g4mh_intc_chan_priority(c->intc, ch, &pri);
        }
        return table + direct_vector_offset(base, pri);
    }
    if (cause >= G4MH_EXC_TRAP0 && cause < G4MH_EXC_TRAP0 + 0x10u) {
        return table + 0x0040u; /* TRAP 0..15                    */
    }
    if (cause >= G4MH_EXC_TRAP1 && cause < G4MH_EXC_TRAP1 + 0x10u) {
        return table + 0x0050u; /* TRAP 16..31                   */
    }
    if (cause >= G4MH_EXC_FETRAP && cause < G4MH_EXC_FETRAP + 0x10u) {
        return table + 0x0030u; /* FETRAP 1..15                  */
    }
    if (cause >= G4MH_EXC_FEINT && cause < G4MH_EXC_FEINT + 0x10u) {
        return table + 0x00F0u; /* FEINT 0..15                   */
    }
    /*
     * R01UH0923EJ0130 table 3.106, and confirmed against the vector table
     * in Renesas' own board package, which is the thing a real guest is
     * built around. Three of these were wrong and each sent an exception
     * to a *different exception's* handler rather than nowhere:
     *
     *   MIP/MDP  0x30 -> 0x90    was landing on FETRAP
     *   MAE      0x60 -> 0xC0    was landing on RIE
     *
     * That is the worst shape a vector bug takes: the handler exists, so
     * something runs and the guest carries on reporting the wrong cause,
     * rather than failing where the mistake is. 0x20, 0xB0 and 0xD0 are
     * reserved, 0xE0 is FENMI, which this frontend does not raise.
     */
    switch (cause) {
    case G4MH_EXC_SYSERR:
        return table + 0x0010u;
    case G4MH_EXC_RIE:
        return table + 0x0060u;
    case G4MH_EXC_FPP:
        return table + 0x0070u; /* FPE/FXE           */
    case G4MH_EXC_UCPOP: /* 80H-82H, one per CUn (table 3.103) */
    case G4MH_EXC_UCPOP + 1u:
    case G4MH_EXC_UCPOP + 2u:
        return table + 0x0080u;
    case G4MH_EXC_MIP:
        return table + 0x0090u;
    case G4MH_EXC_MDP:
    case G4MH_EXC_MDP_TABLE:
        return table + 0x0090u;
    case G4MH_EXC_PIE:
        return table + 0x00A0u;
    case G4MH_EXC_MAE:
        return table + 0x00C0u;
    default:
        /*
         * Nothing else reaches here: SYSCALL vectors through SCBP in the
         * interpreter and never asks. An unknown cause is a system error,
         * which is at least a slot a guest is expected to have populated
         * -- 0x90 used to be the fallback, which quietly made every
         * unknown cause look like a protection violation.
         */
        return table + 0x0010u;
    }
}

g4mh_exc_t g4mh_cpu_irq_vector(g4mh_cpu_t *c, uint32_t channel,
                               uint32_t *out)
{
    const uint32_t base = (c->psw & G4MH_PSW_EBV) ? c->sr[1][G4MH_SR_EBASE]
                                                  : c->sr[1][G4MH_SR_RBASE];

    /*
     * Three things have to agree before the table is read: the channel
     * must ask for it, and the base register in force must not have DV
     * set, which overrides the controller (R01UH0923EJ0130, figure 3.22
     * step <1>). Getting the override backwards would make DV *select*
     * the table rather than refuse it.
     */
    if ((base & G4MH_BASE_DV) != 0u ||
        !g4mh_intc_chan_is_table(c->intc, channel)) {
        unsigned pri = 15u;
        (void)g4mh_intc_chan_priority(c->intc, channel, &pri);
        *out = (base & ~0x1FFu) + direct_vector_offset(base, pri);
        return G4MH_EXC_NONE;
    }

    /*
     * "Exception handler address read position = INTBP + channel number
     * x 4 bytes", and the word read from there *is* the handler address
     * -- not a base to add to. INTBP's low nine bits are always zero.
     */
    const uint32_t at = (c->sr[1][G4MH_SR_INTBP] & ~0x1FFu) + channel * 4u;
    uint32_t v;
    const g4mh_exc_t e = g4mh_load_sv(c, at, &v);

    if (e != G4MH_EXC_NONE) {
        /*
         * MDP while reading the table. The manual is specific: acceptance
         * is cancelled, no response goes to the controller, and the
         * request stays pending to be taken again after the handler
         * returns. That is why the caller acknowledges the channel only
         * once this has succeeded -- moving the ack earlier would drop
         * the interrupt on a fault that is meant to be retried.
         *
         * Its own code, 95H, with the detail of a supervisor read: "these
         * are executed only in supervisor mode" (table 3.115 note 6). It
         * was 91H with no detail, the code an instruction's access gets.
         */
        return G4MH_EXC_MDP_TABLE | (e & ~0xFFFFu);
    }
    *out = v;
    return G4MH_EXC_NONE;
}

/*
 * MEI for an MDP or MAE (tables 3.37 and 3.38): read back from the
 * instruction at `pc`, which is the one that faulted -- both are
 * resumable, so FEPC is that instruction -- except for the table read of
 * an interrupt, which has no instruction. On the exception path only, so
 * neither backend pays for it per access; and one decoder for both.
 */
static EMU_NOINLINE uint32_t mei_for(const g4mh_cpu_t *c, g4mh_exc_t cause,
                                     uint32_t pc)
{
    uint16_t hw[4] = {0u, 0u, 0u, 0u};
    uint32_t mei;

    if (G4MH_EXC_CODE(cause) == G4MH_EXC_MDP_TABLE) {
        return G4MH_MEI(0u, 0u, 2u, 0u, G4MH_MEI_TABLE, 0u);
    }
    if (c->bus == NULL) { /* a core raised by hand, with no memory */
        return c->sr[2][G4MH_SR_MEI];
    }
    for (unsigned i = 0; i < 4u; i++) {
        if (emu_bus_fetch16(c->bus, pc + 2u * i, &hw[i]) != EMU_FAULT_NONE) {
            break;
        }
    }
    if (!g4mh_mei(hw, c->mei_reg, &mei)) {
        /* Nothing that reaches memory: leave MEI as it was. */
        return c->sr[2][G4MH_SR_MEI];
    }
    return mei;
}

/*
 * `handler` is passed in rather than derived because the table reference
 * method resolves it with a memory read that can fault -- so the caller
 * has to have done it, and been able to give up, before any of the entry
 * state below is written. Everything else calls the wrapper.
 */
static void exception_to(g4mh_cpu_t *c, g4mh_exc_t cause, uint32_t ret_pc,
                         uint32_t handler)
{
    const bool fe = g4mh_exc_is_fe(cause);

    switch (G4MH_EXC_CODE(cause)) {
    case G4MH_EXC_MDP:
    case G4MH_EXC_MAE:
    case G4MH_EXC_MDP_TABLE:
        c->sr[2][G4MH_SR_MEI] = mei_for(c, cause, ret_pc);
        break;
    default:
        break;
    }

    if (fe) {
        c->sr[0][G4MH_SR_FEPC] = ret_pc;
        c->sr[0][G4MH_SR_FEPSW] = c->psw;
        c->sr[0][G4MH_SR_FEIC] = cause;
        /*
         * NP is what makes an FE exception non-reentrant: with it set, a
         * second FE exception is a system error rather than a silent
         * overwrite of FEPC. EP marks that this is an exception rather
         * than an interrupt, which is what RETI reads to know which pair
         * of save registers to restore from.
         */
        c->psw |= G4MH_PSW_NP | G4MH_PSW_EP | G4MH_PSW_ID;
        c->psw &= ~G4MH_PSW_UM;
    } else {
        c->sr[0][G4MH_SR_EIPC] = ret_pc;
        c->sr[0][G4MH_SR_EIPSW] = c->psw;
        c->sr[0][G4MH_SR_EIIC] = cause;
        c->psw |= G4MH_PSW_ID;
        c->psw &= ~G4MH_PSW_UM;
        /*
         * EP distinguishes a synchronous exception from an interrupt at
         * the same level. TRAP sets it; an EI interrupt does not, and
         * that is the only difference in how the two are entered.
         */
        if (cause < G4MH_EXC_EIINT_BASE || cause >= G4MH_EXC_SYSCALL) {
            c->psw |= G4MH_PSW_EP;
        } else {
            c->psw &= ~G4MH_PSW_EP;
        }
    }

    /*
     * **Into supervisor mode, at either level.** "When an exception is
     * acknowledged in User mode, the authority operating mode changes to
     * Supervisor mode" (R01UH0865EJ0140 2.1.3.3), and each instruction
     * that raises one says PSW.UM <- 0. Only SYSCALL did here, so every
     * other exception taken in user mode ran its handler as user -- which
     * a fetch-protection fault turns into the same fault again, in the
     * handler, for ever.
     */
    c->sr[0][G4MH_SR_PSW] = c->psw;
    g4mh_jit_ctx_refresh(c);
    c->pc = handler;
    c->irq_dirty = true;

    /*
     * An exception breaks any outstanding reservation. The architecture
     * permits keeping it, but dropping it is simpler to reason about and
     * guarantees forward progress cannot depend on a handler leaving it
     * intact -- the same call the RISC-V side makes for LR/SC.
     */
    g4mh_ll_drop(c);

#if EMU_ENABLE_STATS
    c->exc_count++;
#endif
}

/*
 * The ordinary entry point: derive the handler and go. Every cause but a
 * table-reference interrupt resolves without touching memory, so nothing
 * here can fail.
 */
void g4mh_cpu_exception(g4mh_cpu_t *c, g4mh_exc_t cause, uint32_t ret_pc)
{
    exception_to(c, cause, ret_pc, handler_address(c, cause));
}

void g4mh_cpu_exception_at(g4mh_cpu_t *c, g4mh_exc_t cause, uint32_t ret_pc,
                           uint32_t handler)
{
    exception_to(c, cause, ret_pc, handler);
}

/*
 * The priority ceiling: may an EI interrupt of priority `pri` be
 * acknowledged?
 *
 * Figure 3.17 of the U2B manual is the shape of it -- ISPR *or*
 * PSW.EIMASK, chosen by INTCFG.EPL, and then PLMR, with a mask from
 * either one refusing. Both threshold fields count acceptable levels
 * rather than masked ones, so the test is `pri < value`; reading them
 * the other way is off by one everywhere and admits priority 63, which
 * the architecture never acknowledges.
 */
static bool int_ceiling_admits(const g4mh_cpu_t *c, unsigned pri)
{
    const uint32_t cfg = c->sr[G4MH_SELID_INT][G4MH_SR_INTCFG];
    const uint32_t plm =
        c->sr[G4MH_SELID_INT][G4MH_SR_PLMR] & G4MH_PLMR_PLM_MASK;

    /* PLMR applies in both modes. */
    if (pri >= plm) {
        return false;
    }

    if ((cfg & G4MH_INTCFG_EPL) != 0u) {
        /* 64-priority mode: ISPR is disabled and EIMASK is the ceiling. */
        const uint32_t eimask =
            (c->psw & G4MH_PSW_EIMASK_MASK) >> G4MH_PSW_EIMASK_SHIFT;
        return pri < eimask;
    }

    /*
     * 16-priority mode. "While a bit in this register is set to 1, same
     * or lower priority interrupts are masked" -- so any ISP bit at a
     * level numerically <= pri refuses.
     *
     * A priority of 16 or above is refused by *any* set bit (note 5 to
     * table 3.44), which falls out of the same mask once every bit below
     * 16 is considered: the mask below is all sixteen when pri >= 15.
     */
    const uint32_t ispr = c->sr[G4MH_SELID_INT][G4MH_SR_ISPR] & 0xFFFFu;
    const unsigned n = (pri < G4MH_ISPR_LEVELS) ? (pri + 1u) : G4MH_ISPR_LEVELS;
    const uint32_t blocking = (n >= 32u) ? 0xFFFFFFFFu : ((1u << n) - 1u);

    return (ispr & blocking) == 0u;
}

int g4mh_cpu_pending_irq(const g4mh_cpu_t *c)
{
    unsigned ignored;
    return g4mh_cpu_pending_irq_pri(c, &ignored);
}

int g4mh_cpu_pending_irq_pri(const g4mh_cpu_t *c, unsigned *priority)
{
    /*
     * PSW.ID masks EI interrupts and PSW.NP masks them during an FE
     * exception. Either alone is enough to refuse delivery, and testing
     * them together costs one AND.
     */
    if ((c->psw & (G4MH_PSW_ID | G4MH_PSW_NP)) != 0u) {
        return -1;
    }
    if (c->intc == NULL) {
        return -1;
    }

    /*
     * The controller picks the highest-priority unmasked channel; the
     * ceiling then decides whether the *core* will take it. Asking about
     * the best candidate alone is sufficient rather than a shortcut:
     * every other pending channel has a numerically larger EIP, so a
     * ceiling that refuses this one refuses those too.
     */
    unsigned pri = 64u;
    const int ch = g4mh_intc_pending_pri(c->intc, c->coreid, &pri);

    if (ch < 0 || !int_ceiling_admits(c, pri)) {
        return -1;
    }
    *priority = pri;
    return ch;
}

/*
 * Take an interrupt of priority `pri`: raise the ceiling so that nothing
 * of the same or lower priority preempts the handler.
 *
 * Called from the run loop after g4mh_cpu_exception, because the ceiling
 * update is part of *acknowledging* rather than of vectoring -- an
 * exception is not an interrupt and must not touch it.
 */
void g4mh_cpu_ack_priority(g4mh_cpu_t *c, unsigned pri)
{
    const uint32_t cfg = c->sr[G4MH_SELID_INT][G4MH_SR_INTCFG];

    if ((cfg & G4MH_INTCFG_EPL) != 0u) {
        /*
         * 64-priority mode. "When the CPU acknowledges an interrupt, its
         * interrupt priority is stored" -- and since EIMASK admits
         * `p < EIMASK`, storing the priority itself is what blocks the
         * same level as well as lower ones. EIPSW already holds the old
         * PSW, so EIRET restores the previous ceiling for free.
         */
        c->psw = (c->psw & ~G4MH_PSW_EIMASK_MASK) |
                 ((pri & 0x3Fu) << G4MH_PSW_EIMASK_SHIFT);
        c->sr[0][G4MH_SR_PSW] = c->psw;
        return;
    }

    /*
     * 16-priority mode, and only when INTCFG.ISPC leaves the automatic
     * update on -- with ISPC set the guest is doing its own ceiling
     * through PLMR and ISPR becomes its to write.
     *
     * "If the priority of the acknowledged interrupt is 16 to 63,
     * neither bit of the ISP is set to 1." Those interrupts are still
     * acknowledged; they simply record nothing, which is why a later one
     * at the same level is not blocked by this one.
     */
    if ((cfg & G4MH_INTCFG_ISPC) == 0u && pri < G4MH_ISPR_LEVELS) {
        c->sr[G4MH_SELID_INT][G4MH_SR_ISPR] |= 1u << pri;
    }
}

/*
 * EIRET: drop the ceiling this handler raised.
 *
 * "If PSW.EP is 0 when the EIRET instruction is executed, the bit with
 * the highest priority among the ISP15 to ISP0 bits that are set is
 * cleared" -- the *highest*, not one remembered from entry. That is what
 * makes nesting work without the hardware storing a stack: the bits are
 * the stack.
 *
 * EIMASK needs nothing here; EIRET restores the whole PSW from EIPSW and
 * the field comes back with it.
 */
void g4mh_cpu_eiret_priority(g4mh_cpu_t *c)
{
    const uint32_t cfg = c->sr[G4MH_SELID_INT][G4MH_SR_INTCFG];

    if ((cfg & (G4MH_INTCFG_EPL | G4MH_INTCFG_ISPC)) != 0u) {
        return;
    }
    if ((c->psw & G4MH_PSW_EP) != 0u) {
        return; /* returning from an exception, not an int */
    }

    uint32_t ispr = c->sr[G4MH_SELID_INT][G4MH_SR_ISPR] & 0xFFFFu;
    if (ispr == 0u) {
        return;
    }
    /* The lowest set bit is the highest priority. */
    c->sr[G4MH_SELID_INT][G4MH_SR_ISPR] = ispr & (ispr - 1u);
}

bool g4mh_cpu_pending_fe(const g4mh_cpu_t *c)
{
    /*
     * **PSW.ID is not consulted, and that is the whole difference.** An
     * FE-level interrupt is refused only while PSW.NP is set -- that is,
     * while an FE handler is already running -- which is what makes it
     * useful for a timing-protection timer: a guest that has masked EI
     * interrupts still gets it.
     *
     * Checked before the EI path by the run loop, since FE outranks EI.
     */
    if ((c->psw & G4MH_PSW_NP) != 0u || c->intc == NULL) {
        return false;
    }
    return g4mh_intc_fe_pending(c->intc, c->coreid);
}

/* ------------------------------------------------------------------ */
/* Reservations                                                        */
/* ------------------------------------------------------------------ */

/*
 * Every core, so a store can break the others' reservations. The only
 * cross-core mutable state in the model -- see docs/host/g4mh/multicore.md;
 * if a second one ever appears, that is the moment threading stops being
 * a locking exercise and becomes a redesign.
 */
static g4mh_cpu_t *g_cores[G4MH_PE_COUNT];
static unsigned g_core_count;

emu_lock_t g4mh_sys_lock = EMU_LOCK_INIT;

/*
 * How many cores currently hold one. Kept as a count rather than derived
 * by walking, because g4mh_ll_break is called from the store path and the
 * common answer is "none" -- one predictable branch for a guest that never
 * takes a reservation, which is most of them.
 */
static unsigned g_ll_held;

void g4mh_ll_register(g4mh_cpu_t *c)
{
    /*
     * Indexed by core id, not appended.
     *
     * Appending is only correct if this is called exactly once per core
     * for the life of the program, and it is not: every emu_system_open
     * re-inits every core, and a reload does it again. The table filled
     * with whatever registered first -- including the same core several
     * times over, since most callers open core 0 alone -- and once
     * g_core_count reached G4MH_PE_COUNT every later registration was
     * silently dropped. A core that registered after that point was
     * invisible to g4mh_ll_break, so a store from it broke nobody's
     * reservation and every LL/SC in the guest quietly succeeded.
     *
     * By index it is idempotent, which is what a function called once
     * per core per open has to be.
     */
    if (c->coreid < G4MH_PE_COUNT) {
        g_cores[c->coreid] = c;
        if (c->coreid >= g_core_count) {
            g_core_count = c->coreid + 1u;
        }
    }
}

/*
 * Read without the lock, on every store: a hint, and under threads a
 * stale one is harmless -- a reservation taken a moment ago is checked
 * by STC.W's compare-and-swap whatever this said. Relaxed atomics, which
 * are plain loads and stores on every host here.
 */
bool g4mh_any_reservation(void)
{
    return __atomic_load_n(&g_ll_held, __ATOMIC_RELAXED) != 0u;
}

static void ll_held_add(int d)
{
    __atomic_store_n(
        &g_ll_held, __atomic_load_n(&g_ll_held, __ATOMIC_RELAXED) + (unsigned)d,
        __ATOMIC_RELAXED); /* only ever under the lock */
}

void g4mh_ll_break(uint32_t addr)
{
    const uint32_t word = addr & ~3u;

    emu_lock(&g4mh_sys_lock);
    for (unsigned i = 0; i < g_core_count; i++) {
        g4mh_cpu_t *c = g_cores[i];
        if (c->ll_valid && c->ll_addr == word) {
            c->ll_valid = false;
            ll_held_add(-1);
        }
    }
    emu_unlock(&g4mh_sys_lock);
}

/* Take a reservation on `addr` for `c`, replacing any it already held. */
void g4mh_ll_take(g4mh_cpu_t *c, uint32_t addr)
{
    emu_lock(&g4mh_sys_lock);
    if (!c->ll_valid) {
        ll_held_add(1);
    }
    c->ll_valid = true;
    c->ll_addr = addr & ~3u;
    emu_unlock(&g4mh_sys_lock);
}

/* Drop `c`'s reservation, if it has one. */
void g4mh_ll_drop(g4mh_cpu_t *c)
{
    emu_lock(&g4mh_sys_lock);
    if (c->ll_valid) {
        c->ll_valid = false;
        ll_held_add(-1);
    }
    emu_unlock(&g4mh_sys_lock);
}

/*
 * MDP, with the detail table 3.115 puts in the cause's top half: which
 * permission was refused, in the mode the access was made in, and what
 * kind of instruction made it (`kind`: G4MH_EXC_D_RMW, _BL or 0). An
 * instruction that both reads and writes sets a bit for each it was
 * refused -- the table's notes 1 and 2 say the R bit for "a read
 * violation" and the W bit for "a write violation", and a read-modify-
 * write can be both; that both appear together is this file's reading.
 */
g4mh_exc_t g4mh_mdp(const g4mh_cpu_t *c, bool rd_bad, bool wr_bad,
                    uint32_t kind)
{
    const bool um = (c->psw & G4MH_PSW_UM) != 0u;
    uint32_t d = kind;

    if (rd_bad) {
        d |= um ? G4MH_EXC_D_UR : G4MH_EXC_D_SR;
    }
    if (wr_bad) {
        d |= um ? G4MH_EXC_D_UW : G4MH_EXC_D_SW;
    }
    return G4MH_EXC_MDP | d;
}

#if G4MH_EXT_MPU
/*
 * May the instruction at `pc` execute: every halfword of it in an area
 * that permits execution, in the mode given.
 *
 * Not one area: "the instruction may be executed if both areas are
 * enabled. If one area is disabled, however, the instruction cannot be
 * executed even if the other area is enabled" (3.2.5.1(3)(d)). Checking
 * the first halfword alone was argued safe because every area bound is
 * 4-byte aligned -- and a 32-bit instruction at ...FE has its second
 * halfword in the next word all the same, so it executed from an area
 * that refused it. A 48-bit one always spans two words.
 *
 * The length comes from the code, so this reads it. Only ever called
 * with the MPU on; a halfword the bus cannot fetch is left to the fetch
 * that follows, which reports it.
 */
EMU_NOINLINE bool g4mh_fetch_permitted(const g4mh_cpu_t *c, uint32_t pc,
                                       bool user)
{
    uint16_t w0, w1;

    if (!g4mh_mpu_permits(c->mpu, pc, 2u, G4MH_MPU_FETCH, user, c->spid)) {
        return false;
    }
    if (emu_bus_fetch16(c->bus, pc, &w0) != EMU_FAULT_NONE ||
        g4mh_insn_len(w0) == 2u ||
        emu_bus_fetch16(c->bus, pc + 2u, &w1) != EMU_FAULT_NONE) {
        return true;
    }
    const uint32_t len = !g4mh_insn_is_48(w0, w1) ? 4u
                         : g4mh_insn_is_64(w0, w1) ? 8u
                                                   : 6u;

    for (uint32_t off = 2u; off < len; off += 2u) {
        if (!g4mh_mpu_permits(c->mpu, pc + off, 2u, G4MH_MPU_FETCH, user,
                              c->spid)) {
            return false;
        }
    }
    return true;
}
#endif

uint8_t *g4mh_rmw_ptr(g4mh_cpu_t *c, uint32_t addr, uint32_t size, bool write,
                      g4mh_exc_t *exc)
{
    *exc = G4MH_EXC_NONE;
    if (EMU_UNLIKELY((addr & (size - 1u)) != 0u)) {
        c->sr[2][G4MH_SR_MEA] = addr;
        *exc = G4MH_EXC_MAE;
        return NULL;
    }
#if G4MH_EXT_MPU
    if (EMU_UNLIKELY(c->mpu_active)) {
        const bool um = (c->psw & G4MH_PSW_UM) != 0u;
        const bool rd_bad =
            !g4mh_mpu_permits(c->mpu, addr, size, G4MH_MPU_READ, um, c->spid);
        const bool wr_bad = write && !g4mh_mpu_permits(c->mpu, addr, size,
                                                       G4MH_MPU_WRITE, um,
                                                       c->spid);

        if (rd_bad || wr_bad) {
            c->sr[2][G4MH_SR_MEA] = addr;
            /* Only SET1/NOT1/CLR1 and CAXI raise what this reports:
             * STC's caller takes the ordinary store on a refusal. */
            *exc = g4mh_mdp(c, rd_bad, wr_bad, G4MH_EXC_D_RMW);
            return NULL;
        }
    }
#else
    (void)write;
#endif
    const emu_region_t *const r = emu_bus_find(c->bus, addr);

    if (r == NULL || r->kind != EMU_MEM_RAM || r->host == NULL ||
        addr - r->base > r->size - size) {
        return NULL; /* not plain RAM: the ordinary, locked path */
    }
    return (uint8_t *)r->host + (addr - r->base);
}

/* ------------------------------------------------------------------ */
/* Memory                                                              */
/* ------------------------------------------------------------------ */

g4mh_exc_t g4mh_load(g4mh_cpu_t *c, uint32_t addr, uint32_t size,
                     bool sign_extend, uint32_t *out)
{
    if (EMU_UNLIKELY((addr & (size - 1u)) != 0u)) {
        c->sr[2][G4MH_SR_MEA] = addr;
        return G4MH_EXC_MAE;
    }
#if G4MH_EXT_MPU
    /*
     * One predicted branch for a guest that never enables the MPU. The
     * check is *after* the alignment test on purpose: MAE and MDP are
     * both FE-level and a misaligned access to a protected area has to
     * report one of them, and the architecture detects the misalignment
     * as part of forming the access.
     */
    if (EMU_UNLIKELY(c->mpu_active) &&
        !g4mh_mpu_permits(c->mpu, addr, size, G4MH_MPU_READ,
                          (c->psw & G4MH_PSW_UM) != 0u, c->spid)) {
        c->sr[2][G4MH_SR_MEA] = addr;
        return g4mh_mdp(c, true, false, 0u);
    }
#endif

    uint32_t v;
    const emu_fault_t f = emu_bus_read(c->bus, addr, size, &v);
    if (EMU_UNLIKELY(f != EMU_FAULT_NONE)) {
        c->sr[2][G4MH_SR_MEA] = addr;
        return g4mh_exc_from_fault(f);
    }

    if (sign_extend && size < 4u) {
        v = (uint32_t)emu_sext(v, size * 8u);
    }
    *out = v;
    return G4MH_EXC_NONE;
}

g4mh_exc_t g4mh_store(g4mh_cpu_t *c, uint32_t addr, uint32_t size, uint32_t val)
{
    if (EMU_UNLIKELY((addr & (size - 1u)) != 0u)) {
        c->sr[2][G4MH_SR_MEA] = addr;
        return G4MH_EXC_MAE;
    }
#if G4MH_EXT_MPU
    /*
     * Refused before the bus sees it: "the result of the access which is
     * judged as prohibited is not reflected in memory or I/O devices".
     * Checking after the write and rolling back would be visible to a
     * peripheral, which is the whole reason the order matters.
     */
    if (EMU_UNLIKELY(c->mpu_active) &&
        !g4mh_mpu_permits(c->mpu, addr, size, G4MH_MPU_WRITE,
                          (c->psw & G4MH_PSW_UM) != 0u, c->spid)) {
        c->sr[2][G4MH_SR_MEA] = addr;
        return g4mh_mdp(c, false, true, 0u);
    }
#endif

    const emu_fault_t f = emu_bus_write(c->bus, addr, size, val);
    if (EMU_UNLIKELY(f != EMU_FAULT_NONE)) {
        c->sr[2][G4MH_SR_MEA] = addr;
        return g4mh_exc_from_fault(f);
    }

    /*
     * Any store, from any core, breaks a reservation covering the word.
     * Gated on the global count so this is one predictable branch when no
     * core is in an LL/SC sequence, which is the overwhelmingly common
     * case and the reason the count exists at all.
     */
    if (EMU_UNLIKELY(g4mh_any_reservation())) {
        g4mh_ll_break(addr);
    }
    return G4MH_EXC_NONE;
}

/*
 * A table read made as supervisor: the MPU is asked about mode SV and
 * PSW is left as it was, so an exception from it saves the PSW the
 * program had. Only the mode moves; the SPID is the core's.
 */
g4mh_exc_t g4mh_load_sv(g4mh_cpu_t *c, uint32_t addr, uint32_t *out)
{
    const uint32_t psw = c->psw;
    g4mh_exc_t e;

    c->psw = psw & ~G4MH_PSW_UM;
    e = g4mh_load(c, addr, 4u, false, out);
    c->psw = psw;
    return e;
}

/*
 * LD.DW and ST.DW: one eight-byte access, as the MPU sees it.
 *
 * "It is necessary that the associated memory access is encompassed
 * entirely in a single access enabled area. No accesses that span over
 * two or more areas are allowed even when the access enabled areas are
 * allocated in contiguous spaces" -- with LD.DW and ST.DW named as where
 * that happens (3.2.5.1(3)(d), table 3.112), and one spanning
 * 0xFFFFFFFF to 0 raises MDP even under an area covering everything
 * (table 3.114). Two word accesses each checked alone allowed both, and
 * a refused second word left ST.DW's first written.
 *
 * Word alignment, not doubleword: no MAE on a word boundary (the
 * caution under LD.DW). The bus sees two words, low first.
 */
static bool dw_refused(g4mh_cpu_t *c, uint32_t addr, g4mh_mpu_acc_t acc)
{
#if G4MH_EXT_MPU
    if (EMU_UNLIKELY(c->mpu_active) &&
        !g4mh_mpu_permits(c->mpu, addr, 8u, acc, (c->psw & G4MH_PSW_UM) != 0u,
                          c->spid)) {
        c->sr[2][G4MH_SR_MEA] = addr;
        return true;
    }
#else
    (void)c;
    (void)addr;
    (void)acc;
#endif
    return false;
}

g4mh_exc_t g4mh_load_dw(g4mh_cpu_t *c, uint32_t addr, uint32_t *lo,
                        uint32_t *hi)
{
    if (EMU_UNLIKELY((addr & 3u) != 0u)) {
        c->sr[2][G4MH_SR_MEA] = addr;
        return G4MH_EXC_MAE;
    }
    if (dw_refused(c, addr, G4MH_MPU_READ)) {
        return g4mh_mdp(c, true, false, (addr & 4u) ? G4MH_EXC_D_MS : 0u);
    }
    emu_fault_t f = emu_bus_read(c->bus, addr, 4u, lo);
    if (EMU_UNLIKELY(f != EMU_FAULT_NONE)) {
        c->sr[2][G4MH_SR_MEA] = addr;
        return g4mh_exc_from_fault(f);
    }
    f = emu_bus_read(c->bus, addr + 4u, 4u, hi);
    if (EMU_UNLIKELY(f != EMU_FAULT_NONE)) {
        c->sr[2][G4MH_SR_MEA] = addr + 4u;
        return g4mh_exc_from_fault(f);
    }
    return G4MH_EXC_NONE;
}

g4mh_exc_t g4mh_store_dw(g4mh_cpu_t *c, uint32_t addr, uint32_t lo,
                         uint32_t hi)
{
    if (EMU_UNLIKELY((addr & 3u) != 0u)) {
        c->sr[2][G4MH_SR_MEA] = addr;
        return G4MH_EXC_MAE;
    }
    if (dw_refused(c, addr, G4MH_MPU_WRITE)) {
        return g4mh_mdp(c, false, true, (addr & 4u) ? G4MH_EXC_D_MS : 0u);
    }
    /* The MPU has passed both words; the bus decides each. */
    g4mh_exc_t e = g4mh_store(c, addr, 4u, lo);
    if (e == G4MH_EXC_NONE) {
        e = g4mh_store(c, addr + 4u, 4u, hi);
    }
    return e;
}

/* ------------------------------------------------------------------ */
/* System registers                                                    */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* Performance measurement                                             */
/* ------------------------------------------------------------------ */

/*
 * Advance the enabled performance channels.
 *
 * Eight channels, each a PMCTRLn saying whether it counts and what, and a
 * PMCOUNTn holding the total. Only two events are sourced, and that is
 * deliberate: an emulator can honestly report instructions retired and
 * its own notion of cycles, and everything else on a real part -- cache
 * misses, branch mispredictions, stall cycles -- has no counterpart here.
 * Reporting zero for those is better than reporting a plausible number,
 * because a guest tuning against a fabricated miss rate would be tuning
 * against nothing.
 *
 * Called once per retired instruction from the interpreter, and only when
 * `pm_active` says some channel is enabled -- a loop over eight channels
 * on every instruction is exactly the per-instruction cost the contract
 * forbids, and the overwhelmingly common case is that none is on.
 */
void g4mh_pm_tick(g4mh_cpu_t *c, uint32_t insns)
{
    for (unsigned n = 0; n < G4MH_PM_CHANNELS; n++) {
        const uint32_t ctl = c->sr[G4MH_SELID_PM][G4MH_SR_PMCTRL0 + n];

        if ((ctl & G4MH_PMCTRL_CE) == 0u) {
            continue;
        }
        switch ((ctl >> G4MH_PMCTRL_CND_SH) & G4MH_PMCTRL_CND_MSK) {
        case G4MH_PM_CND_CYCLE:
        case G4MH_PM_CND_INSN:
            /*
             * One cycle per instruction is what this interpreter's own
             * `cycles` counter already assumes, so the two events give
             * the same number here rather than a fabricated difference.
             */
            c->sr[G4MH_SELID_PM][G4MH_SR_PMCOUNT0 + n] += insns;
            break;
        default:
            /* An event this emulator cannot source. Left alone. */
            break;
        }
    }
}

/* True if any channel is enabled. Maintained on write, not recomputed. */
static void pm_refresh(g4mh_cpu_t *c)
{
    bool on = false;

    for (unsigned n = 0; n < G4MH_PM_CHANNELS; n++) {
        if ((c->sr[G4MH_SELID_PM][G4MH_SR_PMCTRL0 + n] & G4MH_PMCTRL_CE) !=
            0u) {
            on = true;
            break;
        }
    }
    c->pm_active = on;
}

/*
 * Figure 3.3, before the register is reached: is it a coprocessor's
 * whose CUn is clear (UCPOP), then is it SV in user mode (PIE). The
 * permissions are tables 3.9, 3.43, 3.50, 3.57, 3.64, 3.78, 3.86, 3.94
 * and 3.102 of R01UH0923EJ0130; every register not named as UM there is
 * SV, and one not listed at all "is handled as having the SV
 * permission" (3.2.2.5(4)). So this lists what user mode may reach.
 */
g4mh_exc_t g4mh_sr_access(const g4mh_cpu_t *c, unsigned bank, unsigned reg,
                          bool write)
{
    unsigned cu = 3u; /* none */
    bool user_ok = false;

    switch (bank) {
    case 0u:
        if (reg == G4MH_SR_FPSR || reg == G4MH_SR_FPEPC) {
            cu = 0u; /* CU0 and SV */
        } else if (reg >= G4MH_SR_FPST && reg <= G4MH_SR_FPCFG) {
            cu = 0u;
            user_ok = true;
        } else {
            user_ok = reg == G4MH_SR_PSW || reg == G4MH_SR_CTPC ||
                      reg == G4MH_SR_CTPSW || reg == G4MH_SR_CTBP;
        }
        break;
    case 2u: /* PEID and BMID */
        user_ok = reg <= 1u;
        break;
    case G4MH_SELID_FXU:
        if (reg == 6u) { /* FXSR: CU1 and SV */
            cu = 1u;
        } else if (reg == 8u || reg == 9u || reg == 10u || reg == 12u ||
                   reg == 13u) {
            cu = 1u;
            user_ok = true;
        }
        break;
    case G4MH_SELID_PMU: /* TSCOUNTL/H: read UM, write SV */
        user_ok = !write && (reg == G4MH_SR_TSCOUNTL || reg == G4MH_SR_TSCOUNTH);
        break;
    case G4MH_SELID_PM: /* PMCTRLn, PMCOUNTn: as PMUMCTRL.PMUEn says */
    case G4MH_SELID_PMSUB: /* PMSUBCNDn */
        if ((reg & 7u) == reg ||
            (bank == G4MH_SELID_PM && reg >= G4MH_SR_PMCOUNT0 &&
             reg < G4MH_SR_PMCOUNT0 + G4MH_PM_CHANNELS)) {
            user_ok = ((c->sr[G4MH_SELID_PMU][G4MH_SR_PMUMCTRL] >> (reg & 7u)) &
                       1u) != 0u;
        }
        break;
    default:
        break;
    }

    if (cu < 3u && (c->psw & (G4MH_PSW_CU0 << cu)) == 0u) {
        return G4MH_EXC_UCPOP + cu;
    }
    if ((c->psw & G4MH_PSW_UM) != 0u && !user_ok) {
        return G4MH_EXC_PIE;
    }
    return G4MH_EXC_NONE;
}

/*
 * **Out of line, all three.** Inlined -- LDSR's checks in the
 * interpreter's switch, and GCC splitting g4mh_sr_write to put its head
 * at every call -- they grew the run loop by 1.5 KB and cost interpreted
 * CoreMark 2.2% on the F746 (91.20 to 93.20 host cycles an instruction,
 * at three layouts), for instructions CoreMark never runs.
 */
EMU_NOINLINE g4mh_exc_t g4mh_ldsr(g4mh_cpu_t *c, unsigned bank, unsigned reg,
                                  uint32_t val)
{
    const g4mh_exc_t e = g4mh_sr_access(c, bank, reg, true);

    if (e != G4MH_EXC_NONE) {
        return e;
    }
    /*
     * PSW's permission is per bit: in user mode an LDSR writes the five
     * flags and ignores the rest, with no PIE (table 3.14).
     */
    if (bank == 0u && reg == G4MH_SR_PSW && (c->psw & G4MH_PSW_UM) != 0u) {
        val = (c->psw & ~0x1Fu) | (val & 0x1Fu);
    }
    g4mh_sr_write(c, bank, reg, val);
    return G4MH_EXC_NONE;
}

EMU_NOINLINE g4mh_exc_t g4mh_stsr(const g4mh_cpu_t *c, unsigned bank,
                                  unsigned reg, uint32_t *out)
{
    const g4mh_exc_t e = g4mh_sr_access(c, bank, reg, false);

    if (e == G4MH_EXC_NONE) {
        *out = g4mh_sr_read(c, bank, reg);
    }
    return e;
}

uint32_t g4mh_sr_read(const g4mh_cpu_t *c, unsigned bank, unsigned reg)
{
    if (bank >= G4MH_SR_BANKS || reg >= G4MH_SR_PER_BANK) {
        return 0u; /* reserved: reads as zero, no exception */
    }
    if (bank == 0u && reg == G4MH_SR_PSW) {
        return c->psw;
    }
    if (bank == 1u && reg == G4MH_SR_SPIDLIST) {
        return G4MH_SPIDLIST_VALUE;
    }
#if G4MH_EXT_MPU
    if (bank == 1u && reg == G4MH_SR_SPID) {
        return c->spid;
    }
#endif
#if G4MH_EXT_MPU
    {
        uint32_t v;
        if (bank == G4MH_SR_SEL_MPU && c->mpu != NULL &&
            g4mh_mpu_sr_read(c->mpu, reg, &v)) {
            return v;
        }
    }
#endif
    return c->sr[bank][reg];
}

EMU_NOINLINE void g4mh_sr_write(g4mh_cpu_t *c, unsigned bank, unsigned reg,
                                uint32_t val)
{
    if (bank >= G4MH_SR_BANKS || reg >= G4MH_SR_PER_BANK) {
        return; /* reserved: writes dropped */
    }

    if (bank == 0u && reg == G4MH_SR_PSW) {
        /*
         * PSW is the one system register with a hot shadow, because the
         * interpreter reads its flags on every conditional and reading
         * them out of the array would cost an extra indexing step per
         * branch. Writing it can unmask interrupts, so it dirties.
         */
        c->psw = val;
        c->sr[0][G4MH_SR_PSW] = val;
        c->irq_dirty = true;
        return;
    }

    /*
     * SVLOCK.SVL: "SPID, MPM, MPLA, MPUA, MPAT, MPIDn, and MPBK" are not
     * updated even in supervisor mode, and the attempt raises nothing
     * (3.2.2.5(5)). The setting check's registers and MPIDX stay
     * writable: they change nothing an access is judged by.
     */
    if ((c->sr[1][G4MH_SR_SVLOCK] & G4MH_SVLOCK_SVL) != 0u &&
        ((bank == 1u && reg == G4MH_SR_SPID) ||
          (bank == G4MH_SR_SEL_MPU &&
          (reg == G4MH_SR_MPM || reg == G4MH_SR_MPBK ||
           (reg >= G4MH_SR_MPLA && reg <= G4MH_SR_MPAT) ||
           reg >= G4MH_SR_MPID0)))) {
        return;
    }

    if (bank == 1u) {
        switch (reg) {
        case G4MH_SR_SPID: {
            /*
             * Five bits, and only an identifier SPIDLIST lists; "if an
             * attempt is made to set an illegal system protection
             * identifier, the SPID register is not updated" (table
             * 3.24). It is what the MPU's SPID group compares, so a
             * change is a change to every block's fetch decision.
             */
            const uint32_t v = val & 0x1Fu;

            if (((G4MH_SPIDLIST_VALUE >> v) & 1u) == 0u) {
                return;
            }
            c->sr[1][G4MH_SR_SPID] = v;
#if G4MH_EXT_MPU
            if (c->spid != v) {
                c->spid = v;
                c->jit_gen++;
            }
#endif
            return;
        }
        case G4MH_SR_SPIDLIST:
            return; /* read-only, set outside the CPU */
        case G4MH_SR_SVLOCK:
            c->sr[1][G4MH_SR_SVLOCK] = val & G4MH_SVLOCK_SVL;
            return;
        default:
            break;
        }
    }

#if G4MH_EXT_MPU
    /*
     * The MPU's registers are a window onto an entry array, so they
     * cannot live in sr[][]: MPLA, MPUA and MPAT each name one slot but
     * refer to whichever of 32 entries MPIDX selects. The unit answers
     * for its own selID and reports whether it did, which keeps the list
     * of what it owns in one file.
     *
     * The generic store below still runs for a register it declines, so
     * an unimplemented selID-5 number behaves exactly as before.
     */
    if (bank == G4MH_SR_SEL_MPU && c->mpu != NULL &&
        g4mh_mpu_sr_write(c->mpu, reg, val)) {
        /*
         * MPM is the only writer of the flag the fetch and access paths
         * test, and this is the only path to MPM.
         */
        c->mpu_active = g4mh_mpu_is_active(c->mpu);
        /*
         * Any register that decides an access, not only MPM: an area's
         * bounds or attributes change what a translated block was
         * allowed to fetch. A guest reprograms the MPU at a task switch,
         * not per instruction -- but it may run the setting check per
         * system call, and that changes nothing a block depends on.
         */
        if (g4mh_mpu_reg_protects(reg)) {
            c->jit_gen++;
        }
        return;
    }
#endif

    /* CTPSW keeps PSW's five flags; bits 31:5 read zero (table 3.19). */
    if (bank == 0u && reg == G4MH_SR_CTPSW) {
        val &= 0x1Fu;
    }

    c->sr[bank][reg] = val;

    /*
     * PLMR, ISPR and INTCFG set the interrupt ceiling, and "the new PLMR
     * value is reflected in the instructions following that LDSR" (G4MH
     * manual, PLMR). Without this, an interrupt the write admitted stayed
     * pending until something unrelated dirtied the flag -- on both
     * backends, so no comparison of the two could see it; a fuzz seed did.
     */
    if (bank == G4MH_SELID_INT) {
        c->irq_dirty = true;
    }

    /*
     * Enabling a channel is the only thing that can turn counting on, so
     * the flag is maintained here rather than tested per instruction.
     * Same shape as the RV32 side's fetch_guard: the check that costs is
     * the one on the hot path, so keep it to a single load of a flag
     * whose writers are few and known.
     */
    if (bank == G4MH_SELID_PM && reg < G4MH_PM_CHANNELS) {
        pm_refresh(c);
    }
}

/*
 * SET1 / NOT1 / CLR1 / TST1, which differ only in what they write back.
 *
 * Shared between the two encodings -- Format VIII takes the bit number
 * from the opcode and the address from a disp16, Format IX takes both
 * from registers -- and between the interpreter and the JIT's helper,
 * because the semantics below are the whole instruction and having them
 * in one place is what keeps the four from drifting.
 *
 * Three things here are easy to get wrong and none of them shows up as a
 * wrong register value:
 *
 *   - Z is the *old* value of the bit, complemented, and is set before
 *     the modification. Computing it from the result makes SET1 always
 *     clear Z and CLR1 always set it.
 *   - only Z moves. CY, OV and S keep whatever the previous instruction
 *     left, which is unlike almost everything else in this file.
 *   - the access is a byte, and it is a read-modify-write even for NOT1.
 *     TST1 alone does not store, which also means it alone cannot raise
 *     a store-side fault.
 */
g4mh_exc_t g4mh_bitop(g4mh_cpu_t *c, uint32_t op, uint32_t adr, uint32_t bit)
{
    const uint32_t mask = 1u << (bit & 7u);
    uint32_t token;

#if EMU_SMP
    /*
     * With cores really running at once the read and the write-back are
     * two accesses another core's plain store can land between, losing
     * it -- so on RAM the whole thing is one host atomic, after the same
     * checks the two accesses would make. TST1 is a read and stays one.
     */
    if (op != G4MH_BITOP_TST) {
        g4mh_exc_t ex;
        uint8_t *const hp = g4mh_rmw_ptr(c, adr, 1u, true, &ex);

        if (EMU_UNLIKELY(ex != G4MH_EXC_NONE)) {
            return ex;
        }
        if (hp != NULL) {
            const uint8_t m = (uint8_t)mask;
            const uint8_t old =
                (op == G4MH_BITOP_SET)
                    ? __atomic_fetch_or(hp, m, __ATOMIC_SEQ_CST)
                : (op == G4MH_BITOP_CLR)
                    ? __atomic_fetch_and(hp, (uint8_t)~m, __ATOMIC_SEQ_CST)
                    : __atomic_fetch_xor(hp, m, __ATOMIC_SEQ_CST);

            if ((old & m) != 0u) {
                c->psw &= ~G4MH_PSW_Z;
            } else {
                c->psw |= G4MH_PSW_Z;
            }
            if (EMU_UNLIKELY(g4mh_any_reservation())) {
                g4mh_ll_break(adr); /* it is a store, as below */
            }
            return G4MH_EXC_NONE;
        }
    }
#endif
#if G4MH_EXT_MPU
    /*
     * Both permissions before anything moves. The load below sets Z and
     * then the store can be refused -- MDP is resumable, so the
     * instruction must not have happened, and FEPSW had the new Z. And
     * the refusal is a read-modify-write's (table 3.115's RMW bit).
     */
    if (op != G4MH_BITOP_TST && EMU_UNLIKELY(c->mpu_active)) {
        g4mh_exc_t ex;

        (void)g4mh_rmw_ptr(c, adr, 1u, true, &ex);
        if (ex != G4MH_EXC_NONE) {
            return ex;
        }
    }
#endif
    g4mh_exc_t e = g4mh_load(c, adr, 1u, false, &token);
    if (EMU_UNLIKELY(e != G4MH_EXC_NONE)) {
        return e;
    }

    if ((token & mask) != 0u) {
        c->psw &= ~G4MH_PSW_Z;
    } else {
        c->psw |= G4MH_PSW_Z;
    }

    if (op == G4MH_BITOP_TST) {
        return G4MH_EXC_NONE;
    }

    uint32_t out = token;
    if (op == G4MH_BITOP_SET) {
        out |= mask;
    } else if (op == G4MH_BITOP_CLR) {
        out &= ~mask;
    } else {
        out ^= mask;
    }
    return g4mh_store(c, adr, 1u, out);
}

/*
 * ADF: reg3 = reg1 + reg2 + (condition ? 1 : 0), defining all four flags.
 *
 * Here rather than in the interpreter because the JIT reaches it too,
 * through a helper -- the same arrangement as rv_hart_amo, and for the
 * same reason: a three-input add whose carry comes from a 33-bit sum and
 * whose overflow is taken from the two *original* operands is not
 * something to derive twice.
 *
 * **The IR cannot express this and it is worth saying why.** EMU_IR_SETF
 * has two operands, so FS_ADD would have to be given `a + b` and `k`,
 * which loses a carry out of the first add and computes overflow from
 * the wrong pair. Every operand where a + b does not carry agrees with
 * the correct answer, which is most of them.
 */
void g4mh_adf(g4mh_cpu_t *c, uint32_t r1, uint32_t r2, uint32_t rd,
              uint32_t cond)
{
    const uint32_t a = c->r[r1];
    const uint32_t b = c->r[r2];
    const uint32_t k = g4mh_cond(cond, c->psw) ? 1u : 0u;
    const uint64_t wide = (uint64_t)a + (uint64_t)b + (uint64_t)k;
    const uint32_t res = (uint32_t)wide;
    uint32_t psw = c->psw & ~G4MH_PSW_FLAGS;

    if ((wide >> 32) != 0u) {
        psw |= G4MH_PSW_CY;
    }
    if (res == 0u) {
        psw |= G4MH_PSW_Z;
    }
    if ((res & 0x80000000u) != 0u) {
        psw |= G4MH_PSW_S;
    }
    if ((~(a ^ b) & (a ^ res) & 0x80000000u) != 0u) {
        psw |= G4MH_PSW_OV;
    }
    c->psw = psw;
    c->r[rd] = res;
    c->r[0] = 0u;
}

static const uint8_t k_list12_bit[12] = {
    27u, 26u, 25u, 24u, /* r20 r21 r22 r23 */
    31u, 30u, 29u, 28u, /* r24 r25 r26 r27 */
    23u, 22u, 0u,  21u /* r28 r29 r30 r31 */
};

bool g4mh_list12_has(uint32_t list, unsigned reg)
{
    return (list & (1u << k_list12_bit[reg - 20u])) != 0u;
}

/*
 * PREPARE's register save. Ascending register order, each one four bytes
 * below the last, so r20 lands highest and r31 lowest -- and DISPOSE
 * therefore walks *descending* to undo it. The manual states the two
 * orders in separate places and they are not the same; reading one and
 * assuming the other restores every register into its neighbour, which
 * is a wrong answer rather than a fault.
 */
static g4mh_exc_t prepare_save(g4mh_cpu_t *c, uint32_t list,
                               uint32_t *sp_out)
{
    uint32_t tmp = c->r[3];

    for (unsigned reg = 20u; reg <= 31u; reg++) {
        if (!g4mh_list12_has(list, reg)) {
            continue;
        }
        tmp -= 4u;
        const g4mh_exc_t e = g4mh_store(c, tmp & ~3u, 4u, c->r[reg]);
        if (EMU_UNLIKELY(e != G4MH_EXC_NONE)) {
            return g4mh_exc_block(c, e, reg);
        }
    }
    *sp_out = tmp;
    return G4MH_EXC_NONE;
}

/*
 * PREPARE's common half: save the listed registers and drop sp.
 *
 * Shared because the JIT reaches it through a helper -- the arrangement
 * this codebase uses for any semantics both backends need, so a rule
 * this fiddly exists once. The ep-loading forms add to this rather than
 * repeating it; only the wider encodings have an ep to load.
 *
 * It can fault: every register save is a store, and the caller must
 * report the exception rather than carry on.
 */
g4mh_exc_t g4mh_prepare(g4mh_cpu_t *c, uint32_t list, uint32_t imm5,
                        uint32_t *sp_out)
{
    uint32_t sp;
    const g4mh_exc_t e = prepare_save(c, list, &sp);

    if (e != G4MH_EXC_NONE) {
        return e;
    }
    sp -= imm5 << 2;
    c->r[3] = sp;
    if (sp_out != NULL) {
        *sp_out = sp;
    }
    return G4MH_EXC_NONE;
}

static g4mh_exc_t dispose_load(g4mh_cpu_t *c, uint32_t list, uint32_t imm5,
                                  uint32_t *sp_out)
{
    uint32_t tmp = c->r[3] + (imm5 << 2);

    for (unsigned reg = 32u; reg-- > 20u;) {
        if (!g4mh_list12_has(list, reg)) {
            continue;
        }
        uint32_t v;
        const g4mh_exc_t e = g4mh_load(c, tmp & ~3u, 4u, false, &v);
        if (EMU_UNLIKELY(e != G4MH_EXC_NONE)) {
            return g4mh_exc_block(c, e, reg);
        }
        c->r[reg] = v;
        tmp += 4u;
    }
    *sp_out = tmp;
    return G4MH_EXC_NONE;
}

/*
 * DISPOSE: restore the listed registers, drop sp, and read the return
 * target.
 *
 * **Descending, where PREPARE ascends.** The manual states the two
 * orders in separate places and they are not the same; reading one and
 * assuming the other restores every register into its neighbour, which
 * is a wrong answer rather than a fault.
 *
 * **The target is read after the loads**, because one of them may
 * restore the very register it comes from -- a leaf epilogue that pops
 * lp and returns through it is exactly that. sp is written after the
 * target for the same reason, in case the target register is sp.
 *
 * Shared with the JIT's helper so the ordering exists once.
 */
g4mh_exc_t g4mh_dispose(g4mh_cpu_t *c, uint32_t list, uint32_t imm5,
                        uint32_t rt, uint32_t *target_out)
{
    uint32_t sp;
    const g4mh_exc_t e = dispose_load(c, list, imm5, &sp);

    if (e != G4MH_EXC_NONE) {
        return e;
    }
    *target_out = c->r[rt] & ~1u;
    c->r[3] = sp;
    return G4MH_EXC_NONE;
}
