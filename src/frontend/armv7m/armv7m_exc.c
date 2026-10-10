/* SPDX-License-Identifier: Apache-2.0 */
/*
 * armv7m_exc.c - the ARMv7-M exception model, the two stacks, and the
 * memory path every access takes: MPU, then bus.
 *
 * Written from the ARM ARM's own pseudocode (B1.5) rather than from a
 * description of it -- ExecutionPriority, PushStack, ExceptionTaken,
 * ExceptionReturn, PopStack and DeActivate each have a function here of
 * the same shape. The priority rules are where a paraphrase goes wrong:
 * FAULTMASK, PRIMASK and BASEPRI do not *gate* exceptions, they raise the
 * execution priority, and escalation to HardFault is nothing more than
 * a synchronous fault discovering it cannot preempt.
 */

#include "armv7m/armv7m_cpu.h"

#include "emu/emu_bus.h"

#include <string.h>

typedef armv7m_exc_t X;
#define OK ARMV7M_X_NONE

/* CFSR: MMFSR in byte 0, BFSR in byte 1, UFSR in the top half. */
#define MMFSR_IACCVIOL (1u << 0)
#define MMFSR_DACCVIOL (1u << 1)
#define MMFSR_MUNSTKERR (1u << 3)
#define MMFSR_MSTKERR (1u << 4)
#define MMFSR_MLSPERR (1u << 5)
#define MMFSR_MMARVALID (1u << 7)
#define BFSR_IBUSERR (1u << 8)
#define BFSR_PRECISERR (1u << 9)
#define BFSR_UNSTKERR (1u << 11)
#define BFSR_STKERR (1u << 12)
#define BFSR_LSPERR (1u << 13)
#define BFSR_BFARVALID (1u << 15)
#define UFSR_UNDEFINSTR (1u << 16)
#define UFSR_INVSTATE (1u << 17)
#define UFSR_INVPC (1u << 18)
#define UFSR_NOCP (1u << 19)
#define UFSR_UNALIGNED (1u << 24)
#define UFSR_DIVBYZERO (1u << 25)

#define HFSR_VECTTBL (1u << 1)
#define HFSR_FORCED (1u << 30)

#define SHCSR_MEMFAULTENA (1u << 16)
#define SHCSR_BUSFAULTENA (1u << 17)
#define SHCSR_USGFAULTENA (1u << 18)

#define CCR_NONBASETHRDENA (1u << 0)
#define CCR_STKALIGN (1u << 9)
#define CCR_BP (1u << 18)

#define FPCCR_ASPEN (1u << 31)
#define FPCCR_LSPEN (1u << 30)
#define FPCCR_LSPACT (1u << 0)
#define FPCCR_USER (1u << 1)
#define FPCCR_THREAD (1u << 3)
#define FPCCR_HFRDY (1u << 4)
#define FPCCR_MMRDY (1u << 5)
#define FPCCR_BFRDY (1u << 6)
#define FPCCR_MONRDY (1u << 8)

#define MPU_CTRL_ENABLE 1u
#define MPU_CTRL_HFNMIENA 2u
#define MPU_CTRL_PRIVDEFENA 4u

/* ------------------------------------------------------------------ */
/* Bitmaps                                                             */
/* ------------------------------------------------------------------ */

static inline bool bit_get(const uint32_t *m, uint32_t n)
{
    return ((m[n >> 5] >> (n & 31u)) & 1u) != 0u;
}

static inline void bit_put(uint32_t *m, uint32_t n, bool on)
{
    if (on) {
        m[n >> 5] |= 1u << (n & 31u);
    } else {
        m[n >> 5] &= ~(1u << (n & 31u));
    }
}

bool armv7m_is_pending(const armv7m_cpu_t *c, uint32_t exc)
{
    return exc < ARMV7M_NEXC && bit_get(c->pending, exc);
}

bool armv7m_is_active(const armv7m_cpu_t *c, uint32_t exc)
{
    return exc < ARMV7M_NEXC && bit_get(c->active, exc);
}

void armv7m_set_pending(armv7m_cpu_t *c, uint32_t exc, bool on)
{
    if (exc >= 2u && exc < ARMV7M_NEXC) {
        bit_put(c->pending, exc, on);
        if (on) {
            c->irq_maybe = true;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Reset                                                               */
/* ------------------------------------------------------------------ */

/*
 * The architectural reset values, and the F746's where the architecture
 * leaves a choice: CCR.STKALIGN set, the FPU's automatic and lazy state
 * preservation both on.
 */
void armv7m_reset_state(armv7m_cpu_t *c)
{
    memset(c->r, 0, sizeof(c->r));
    c->xpsr = ARMV7M_T;
    c->msp = 0u;
    c->psp = 0u;
    c->control = 0u;
    c->primask = 0u;
    c->faultmask = 0u;
    c->basepri = 0u;
    memset(c->pending, 0, sizeof(c->pending));
    memset(c->active, 0, sizeof(c->active));
    memset(c->enabled, 0, sizeof(c->enabled));
    memset(c->prio, 0, sizeof(c->prio));
    memset(c->line, 0, sizeof(c->line));
    c->prigroup = 0u;
    c->scr = 0u;
    /* STKALIGN and BP read as one and ignore writes on the M7: 0x00040200. */
    c->ccr = CCR_STKALIGN | CCR_BP;
    c->shcsr = 0u;
    c->cfsr = 0u;
    c->hfsr = 0u;
    c->mmfar = 0u;
    c->bfar = 0u;
    c->cpacr = 0u;
    c->systick_ctrl = 0u;
    c->systick_reload = 0u;
    c->systick_value = 0u;
    c->systick_countflag = false;
    c->mpu_ctrl = 0u;
    c->mpu_rnr = 0u;
    memset(c->mpu_rbar, 0, sizeof(c->mpu_rbar));
    memset(c->mpu_rasr, 0, sizeof(c->mpu_rasr));
    c->excl_open = false;
    c->excl_addr = 0u;
    memset(c->s, 0, sizeof(c->s));
    c->fpscr = 0u;
    c->fpccr = FPCCR_ASPEN | FPCCR_LSPEN;
    c->fpcar = 0u;
    c->fpdscr = 0u;
    c->faulted = false;
    c->lockup = false;
    c->fault_pc = 0u;
    c->fault_insn = 0u;
}

/* ------------------------------------------------------------------ */
/* The two stack pointers                                              */
/* ------------------------------------------------------------------ */

/* Which bank r[13] currently is: 1 for the process stack. */
static inline uint32_t active_bank(const armv7m_cpu_t *c)
{
    return (!armv7m_handler_mode(c) && (c->control & ARMV7M_CONTROL_SPSEL))
               ? 1u
               : 0u;
}

/* Put r[13] back into its bank before anything changes which bank it is. */
static void sp_save(armv7m_cpu_t *c)
{
    if (active_bank(c) != 0u) {
        c->psp = c->r[ARMV7M_SP];
    } else {
        c->msp = c->r[ARMV7M_SP];
    }
}

static void sp_load(armv7m_cpu_t *c)
{
    c->r[ARMV7M_SP] = (active_bank(c) != 0u) ? c->psp : c->msp;
}

uint32_t armv7m_get_sp(const armv7m_cpu_t *c, uint32_t which)
{
    if (which > 1u) {
        return 0u;
    }
    if (which == active_bank(c)) {
        return c->r[ARMV7M_SP];
    }
    return (which != 0u) ? c->psp : c->msp;
}

void armv7m_set_sp(armv7m_cpu_t *c, uint32_t which, uint32_t v)
{
    /* SP bits [1:0] read as zero and ignore writes. */
    v &= ~3u;
    if (which > 1u) {
        return;
    }
    if (which == active_bank(c)) {
        c->r[ARMV7M_SP] = v;
    }
    if (which != 0u) {
        c->psp = v;
    } else {
        c->msp = v;
    }
}

void armv7m_write_control(armv7m_cpu_t *c, uint32_t v)
{
    sp_save(c);
    c->control = (c->control & ~ARMV7M_CONTROL_NPRIV) | (v & ARMV7M_CONTROL_NPRIV);
    if (!armv7m_handler_mode(c)) {
        c->control =
            (c->control & ~ARMV7M_CONTROL_SPSEL) | (v & ARMV7M_CONTROL_SPSEL);
    }
    c->control = (c->control & ~ARMV7M_CONTROL_FPCA) | (v & ARMV7M_CONTROL_FPCA);
    sp_load(c);
}

/* ------------------------------------------------------------------ */
/* Priorities                                                          */
/* ------------------------------------------------------------------ */

static int32_t exc_prio(const armv7m_cpu_t *c, uint32_t exc)
{
    switch (exc) {
    case ARMV7M_EXC_RESET:
        return -3;
    case ARMV7M_EXC_NMI:
        return -2;
    case ARMV7M_EXC_HARDFAULT:
        return -1;
    default:
        return c->prio[exc];
    }
}

/* The group priority: the subpriority bits PRIGROUP selects, cleared. */
static int32_t group(const armv7m_cpu_t *c, int32_t p)
{
    const int32_t groupvalue = 2 << c->prigroup;

    if (p < 0) {
        return p;
    }
    return p - (p % groupvalue);
}

int32_t armv7m_exec_priority(const armv7m_cpu_t *c)
{
    int32_t highest = 256;
    int32_t boosted = 256;

    for (uint32_t w = 0u; w < ARMV7M_NEXC_WORDS; w++) {
        uint32_t bits = c->active[w];

        while (bits != 0u) {
            const uint32_t exc = w * 32u + (uint32_t)__builtin_ctz(bits);
            const int32_t p = exc_prio(c, exc);

            bits &= bits - 1u;
            if (p < highest) {
                highest = p;
            }
        }
    }
    highest = group(c, highest);
    if ((c->basepri & ARMV7M_PRIO_MASK) != 0u) {
        boosted = group(c, (int32_t)(c->basepri & ARMV7M_PRIO_MASK));
    }
    if (c->primask != 0u) {
        boosted = 0;
    }
    if (c->faultmask != 0u) {
        boosted = -1;
    }
    return (boosted < highest) ? boosted : highest;
}

/*
 * Which pending exceptions may be taken, as a mask over the word: the
 * system exceptions always, an external line only while it is enabled.
 * The enable bits are numbered by interrupt and the pending bits by
 * exception, sixteen apart, so a word of one straddles two of the other.
 */
static uint32_t takeable(const armv7m_cpu_t *c, uint32_t w)
{
    const uint32_t nbank = ARMV7M_NIRQ / 32u;
    const uint32_t lo = (w >= 1u && w - 1u < nbank) ? c->enabled[w - 1u] >> 16 : 0u;
    const uint32_t hi = (w < nbank) ? c->enabled[w] << 16 : 0u;

    return (w == 0u) ? (0xFFFFu | hi) : (lo | hi);
}

/*
 * The pending exception that would be taken next, and whether it can
 * preempt what is running now. Priority first, then the lower exception
 * number -- which is also how equal-priority interrupts are ordered on
 * the board.
 *
 * **This runs before every instruction**, so the common case -- nothing
 * pending at all -- has to cost one pass over five words and nothing
 * more. The priority walk only happens when there is something to rank.
 */
uint32_t armv7m_pending_exc(const armv7m_cpu_t *c, bool *preempts)
{
    uint32_t any = 0u;
    uint32_t best = 0u;
    int32_t best_p = 0x7FFFFFFF;

    *preempts = false;
    for (uint32_t w = 0u; w < ARMV7M_NEXC_WORDS; w++) {
        any |= c->pending[w];
    }
    if (any == 0u) {
        return 0u;
    }
    for (uint32_t w = 0u; w < ARMV7M_NEXC_WORDS; w++) {
        uint32_t bits = c->pending[w] & takeable(c, w);

        while (bits != 0u) {
            const uint32_t exc = w * 32u + (uint32_t)__builtin_ctz(bits);
            const int32_t p = exc_prio(c, exc);

            bits &= bits - 1u;
            if (p < best_p) {
                best_p = p;
                best = exc;
            }
        }
    }
    if (best != 0u) {
        *preempts = group(c, best_p) < armv7m_exec_priority(c);
    }
    return best;
}

/*
 * RETTOBASE: the exception being handled is the only one active. Zero in
 * Thread mode, where nothing is being handled -- the board reads 0 there,
 * and "at most one active" would have said 1.
 */
uint32_t armv7m_rettobase(const armv7m_cpu_t *c)
{
    uint32_t n = 0u;

    if (!armv7m_handler_mode(c)) {
        return 0u;
    }

    for (uint32_t w = 0u; w < ARMV7M_NEXC_WORDS; w++) {
        n += (uint32_t)__builtin_popcount(c->active[w]);
    }
    return (n <= 1u) ? 1u : 0u;
}

/* ------------------------------------------------------------------ */
/* The MPU                                                             */
/* ------------------------------------------------------------------ */

/*
 * The default memory map's execute-never regions: the peripheral space,
 * the device spaces and the system space. Without an MPU these are the
 * only places a fetch can be refused, and with one they still apply to
 * any access PRIVDEFENA sends to the default map.
 */
static bool default_xn(uint32_t addr)
{
    const uint32_t top = addr >> 28;

    return (top >= 0x4u && top <= 0x5u) || top >= 0xAu;
}

/*
 * Is this access permitted? `acc` is read, write or fetch.
 *
 * The highest-numbered enabled region that matches decides, with its
 * eight subregions able to drop out of it. No match is the default map
 * for privileged code when PRIVDEFENA is set, and a fault otherwise. The
 * PPB -- the SCS itself -- is never subject to the MPU, and neither is
 * any access made at priority -1 or below unless HFNMIENA says so.
 */
static bool mpu_permits(const armv7m_cpu_t *c, uint32_t addr, bool priv,
                        uint32_t acc)
{
    if ((c->mpu_ctrl & MPU_CTRL_ENABLE) == 0u ||
        (addr >= 0xE0000000u && addr < 0xE0100000u) ||
        (armv7m_exec_priority(c) < 0 &&
         (c->mpu_ctrl & MPU_CTRL_HFNMIENA) == 0u)) {
        return !(acc == ARMV7M_ACC_FETCH && default_xn(addr));
    }
    for (int32_t r = (int32_t)ARMV7M_MPU_REGIONS - 1; r >= 0; r--) {
        const uint32_t rasr = c->mpu_rasr[r];
        const uint32_t sz = (rasr >> 1) & 31u;

        if ((rasr & 1u) == 0u || sz < 4u) {
            continue;
        }
        const uint64_t size = (uint64_t)1 << (sz + 1u);
        const uint32_t base = c->mpu_rbar[r] & ~(uint32_t)(size - 1u);

        if ((uint64_t)(addr - base) >= size) {
            continue;
        }
        if (sz >= 7u) { /* subregions exist from 256 bytes up */
            const uint32_t sub = (uint32_t)((addr - base) / (size / 8u));

            if (((rasr >> 8) >> sub) & 1u) {
                continue;
            }
        }
        if (acc == ARMV7M_ACC_FETCH && ((rasr >> 28) & 1u) != 0u) {
            return false;
        }
        switch ((rasr >> 24) & 7u) {
        case 1u:
            return priv;
        case 2u:
            return priv || acc != ARMV7M_ACC_WRITE;
        case 3u:
            return true;
        case 5u:
            return priv && acc != ARMV7M_ACC_WRITE;
        case 6u:
        case 7u:
            return acc != ARMV7M_ACC_WRITE;
        default:
            return false; /* no access, and the reserved encoding */
        }
    }
    if (priv && (c->mpu_ctrl & MPU_CTRL_PRIVDEFENA) != 0u) {
        return !(acc == ARMV7M_ACC_FETCH && default_xn(addr));
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* The memory path                                                     */
/* ------------------------------------------------------------------ */

armv7m_exc_t armv7m_mem_read(armv7m_cpu_t *c, uint32_t addr, uint32_t size,
                             bool unpriv, uint32_t *out)
{
    const bool priv = !unpriv && armv7m_privileged(c);

    if (!mpu_permits(c, addr, priv, ARMV7M_ACC_READ)) {
        c->cfsr |= MMFSR_DACCVIOL | MMFSR_MMARVALID;
        c->mmfar = addr;
        return ARMV7M_X_MEMMANAGE;
    }
    c->access_priv = priv;
    if (emu_bus_read(c->bus, addr, size, out) != EMU_FAULT_NONE) {
        c->access_priv = true;
        c->cfsr |= BFSR_PRECISERR | BFSR_BFARVALID;
        c->bfar = addr;
        return ARMV7M_X_BUSFAULT;
    }
    c->access_priv = true;
    return OK;
}

armv7m_exc_t armv7m_mem_write(armv7m_cpu_t *c, uint32_t addr, uint32_t size,
                              bool unpriv, uint32_t v)
{
    const bool priv = !unpriv && armv7m_privileged(c);

    if (!mpu_permits(c, addr, priv, ARMV7M_ACC_WRITE)) {
        c->cfsr |= MMFSR_DACCVIOL | MMFSR_MMARVALID;
        c->mmfar = addr;
        return ARMV7M_X_MEMMANAGE;
    }
    /*
     * A store to the address the monitor holds is what a second core
     * would do to break a reservation; a single core only loses its own
     * by STREX, CLREX or an exception. So nothing here.
     */
    c->access_priv = priv;
    if (emu_bus_write(c->bus, addr, size, v) != EMU_FAULT_NONE) {
        c->access_priv = true;
        c->cfsr |= BFSR_PRECISERR | BFSR_BFARVALID;
        c->bfar = addr;
        return ARMV7M_X_BUSFAULT;
    }
    c->access_priv = true;
    armv7m_wrote(c, addr);
    return OK;
}

bool armv7m_fetch_probe(const armv7m_cpu_t *c, uint32_t addr, uint16_t *out)
{
    return mpu_permits(c, addr, armv7m_privileged(c), ARMV7M_ACC_FETCH) &&
           emu_bus_fetch16(c->bus, addr, out) == EMU_FAULT_NONE;
}

/* Instruction fetch: IACCVIOL leaves MMFAR alone, IBUSERR leaves BFAR. */
armv7m_exc_t armv7m_mem_fetch16(armv7m_cpu_t *c, uint32_t addr, uint16_t *out)
{
    uint32_t v = 0u;

    if (!mpu_permits(c, addr, armv7m_privileged(c), ARMV7M_ACC_FETCH)) {
        c->cfsr |= MMFSR_IACCVIOL;
        return ARMV7M_X_MEMMANAGE;
    }
    if (emu_bus_fetch16(c->bus, addr, out) != EMU_FAULT_NONE) {
        c->cfsr |= BFSR_IBUSERR;
        return ARMV7M_X_BUSFAULT;
    }
    (void)v;
    return OK;
}

/* ------------------------------------------------------------------ */
/* Lockup                                                              */
/* ------------------------------------------------------------------ */

/*
 * A fault at priority -1 or below has nowhere to go. The architecture
 * stops the core with the pc at 0xEFFFFFFE until a reset or an NMI; this
 * stops the emulator and keeps the address of what caused it, because a
 * run that ends here needs to say where.
 */
static void lockup(armv7m_cpu_t *c, uint32_t pc)
{
    c->lockup = true;
    c->faulted = true;
    c->fault_pc = pc;
    c->state = EMU_STATE_HALTED;
    c->r[ARMV7M_PC] = 0xEFFFFFFEu;
}

/* ------------------------------------------------------------------ */
/* Entry                                                               */
/* ------------------------------------------------------------------ */

static bool push_word(armv7m_cpu_t *c, uint32_t addr, uint32_t v)
{
    if (!mpu_permits(c, addr, true, ARMV7M_ACC_WRITE)) {
        c->cfsr |= MMFSR_MSTKERR;
        return false;
    }
    if (emu_bus_write(c->bus, addr, 4u, v) != EMU_FAULT_NONE) {
        c->cfsr |= BFSR_STKERR;
        return false;
    }
    armv7m_wrote(c, addr);
    return true;
}

/*
 * PushStack. The frame goes on the stack the interrupted code was
 * using, eight words -- or twenty-six with the FP state, when the
 * interrupted context had used the FPU (CONTROL.FPCA). With lazy state
 * preservation on, the FP half is only *reserved*: FPCAR records where
 * it is and the first FP instruction in the handler fills it in.
 *
 * Returns false if a store faulted; the status bit is already set.
 */
static bool push_stack(armv7m_cpu_t *c, uint32_t return_pc)
{
    const bool fp = (c->control & ARMV7M_CONTROL_FPCA) != 0u;
    const uint32_t framesize = fp ? 0x68u : 0x20u;
    const bool forcealign = fp || (c->ccr & CCR_STKALIGN) != 0u;
    const uint32_t spmask = forcealign ? ~7u : ~3u;
    const bool process = active_bank(c) != 0u;
    const uint32_t sp = c->r[ARMV7M_SP];
    const uint32_t align = (forcealign && (sp & 4u)) ? 1u : 0u;
    const uint32_t frame = (sp - framesize) & spmask;
    const uint32_t xpsr =
        (c->xpsr & ~(1u << 9)) | (align << 9);
    const uint32_t words[8] = {c->r[0],  c->r[1],         c->r[2],   c->r[3],
                               c->r[12], c->r[ARMV7M_LR], return_pc, xpsr};
    bool ok = true;

    /* The new SP is committed even if a store faults, as on silicon. */
    c->r[ARMV7M_SP] = frame;
    if (process) {
        c->psp = frame;
    } else {
        c->msp = frame;
    }
    for (uint32_t i = 0u; i < 8u && ok; i++) {
        ok = push_word(c, frame + 4u * i, words[i]);
    }
    if (fp && ok) {
        if ((c->fpccr & FPCCR_LSPEN) == 0u) {
            for (uint32_t i = 0u; i < 16u && ok; i++) {
                ok = push_word(c, frame + 0x20u + 4u * i, c->s[i]);
            }
            if (ok) {
                ok = push_word(c, frame + 0x60u, c->fpscr);
            }
        } else {
            /*
             * UpdateFPCCR: remember where the state belongs, and which
             * fault handlers could take an exception during the save.
             */
            /*
             * And which faults *could* be taken during the save, from the
             * priority at the moment the space was reserved: HFRDY, and
             * MMRDY/BFRDY for the two that must also be enabled. The
             * board set HFRDY where the first version of this did not.
             */
            const int32_t now = armv7m_exec_priority(c);
            const bool mm = (c->shcsr & SHCSR_MEMFAULTENA) != 0u &&
                            group(c, exc_prio(c, ARMV7M_EXC_MEMMANAGE)) < now;
            const bool bf = (c->shcsr & SHCSR_BUSFAULTENA) != 0u &&
                            group(c, exc_prio(c, ARMV7M_EXC_BUSFAULT)) < now;

            c->fpcar = frame + 0x20u;
            c->fpccr |= FPCCR_LSPACT;
            c->fpccr = (c->fpccr & ~(FPCCR_USER | FPCCR_THREAD | FPCCR_HFRDY |
                                     FPCCR_MMRDY | FPCCR_BFRDY | FPCCR_MONRDY)) |
                       (armv7m_privileged(c) ? 0u : FPCCR_USER) |
                       (armv7m_handler_mode(c) ? 0u : FPCCR_THREAD) |
                       ((now > -1) ? FPCCR_HFRDY : 0u) | (mm ? FPCCR_MMRDY : 0u) |
                       (bf ? FPCCR_BFRDY : 0u);
        }
    }
    if (armv7m_handler_mode(c)) {
        c->r[ARMV7M_LR] = 0xFFFFFFE1u | (fp ? 0u : 0x10u);
    } else {
        c->r[ARMV7M_LR] = 0xFFFFFFE9u | (fp ? 0u : 0x10u) |
                          ((c->control & ARMV7M_CONTROL_SPSEL) ? 4u : 0u);
    }
    return ok;
}

/*
 * ExceptionTaken: the vector, the mode, IPSR, a clear ITSTATE, the main
 * stack, the FP context marked inactive. The frame is already pushed.
 * Returns false if the vector itself could not be read.
 */
static bool exception_taken(armv7m_cpu_t *c, uint32_t exc)
{
    uint32_t vec = 0u;
    const uint32_t vtor = c->vtor & ~0x7Fu;

    if (emu_bus_read(c->bus, vtor + 4u * exc, 4u, &vec) != EMU_FAULT_NONE) {
        return false;
    }
    sp_save(c);
    c->xpsr = (c->xpsr & ~(ARMV7M_IPSR_MASK | ARMV7M_T)) | exc |
              ((vec & 1u) ? ARMV7M_T : 0u);
    c->xpsr = armv7m_it_put(c->xpsr, 0u);
    c->control &= ~(ARMV7M_CONTROL_FPCA | ARMV7M_CONTROL_SPSEL);
    sp_load(c);
    c->r[ARMV7M_PC] = vec & ~1u;
    bit_put(c->active, exc, true);
    bit_put(c->pending, exc, false);
    c->excl_open = false;
    return true;
}

/*
 * Take an exception: push, then vector. A stacking fault is a derived
 * exception, MemManage or BusFault -- taken in place of the original,
 * with the frame as far as it got -- and a bad vector is a HardFault.
 */
bool armv7m_take(armv7m_cpu_t *c, uint32_t exc, uint32_t return_pc)
{
    if (!push_stack(c, return_pc)) {
        const bool mm = (c->cfsr & MMFSR_MSTKERR) != 0u;
        const uint32_t f = mm ? ARMV7M_EXC_MEMMANAGE : ARMV7M_EXC_BUSFAULT;
        const uint32_t ena = mm ? SHCSR_MEMFAULTENA : SHCSR_BUSFAULTENA;

        bit_put(c->pending, exc, true); /* the original stays pending */
        if ((c->shcsr & ena) != 0u &&
            group(c, exc_prio(c, f)) < armv7m_exec_priority(c)) {
            exc = f;
        } else if (armv7m_exec_priority(c) > -1) {
            c->hfsr |= HFSR_FORCED;
            exc = ARMV7M_EXC_HARDFAULT;
        } else {
            lockup(c, return_pc);
            return false;
        }
    }
    if (!exception_taken(c, exc)) {
        if (exc == ARMV7M_EXC_HARDFAULT || armv7m_exec_priority(c) <= -1) {
            lockup(c, return_pc);
            return false;
        }
        c->hfsr |= HFSR_VECTTBL;
        return exception_taken(c, ARMV7M_EXC_HARDFAULT) ||
               (lockup(c, return_pc), false);
    }
    return true;
}

/*
 * A synchronous exception -- a fault, or SVC -- and the escalation rule:
 * it must be taken *now*, so if it is disabled or cannot preempt, it
 * becomes a HardFault, and if a HardFault cannot preempt either, the core
 * locks up.
 */
void armv7m_raise(armv7m_cpu_t *c, armv7m_exc_t x, uint32_t pc)
{
    uint32_t exc;
    uint32_t ena = 0u;

    switch (x) {
    case ARMV7M_X_UNDEF:
        c->cfsr |= UFSR_UNDEFINSTR;
        exc = ARMV7M_EXC_USAGEFAULT;
        break;
    case ARMV7M_X_INVSTATE:
        c->cfsr |= UFSR_INVSTATE;
        exc = ARMV7M_EXC_USAGEFAULT;
        break;
    case ARMV7M_X_INVPC:
        c->cfsr |= UFSR_INVPC;
        exc = ARMV7M_EXC_USAGEFAULT;
        break;
    case ARMV7M_X_NOCP:
        c->cfsr |= UFSR_NOCP;
        exc = ARMV7M_EXC_USAGEFAULT;
        break;
    case ARMV7M_X_UNALIGNED:
        c->cfsr |= UFSR_UNALIGNED;
        exc = ARMV7M_EXC_USAGEFAULT;
        break;
    case ARMV7M_X_DIVBYZERO:
        c->cfsr |= UFSR_DIVBYZERO;
        exc = ARMV7M_EXC_USAGEFAULT;
        break;
    case ARMV7M_X_MEMMANAGE:
        exc = ARMV7M_EXC_MEMMANAGE;
        break;
    case ARMV7M_X_BUSFAULT:
        exc = ARMV7M_EXC_BUSFAULT;
        break;
    case ARMV7M_X_SVC:
        exc = ARMV7M_EXC_SVCALL;
        break;
    default:
        lockup(c, pc);
        return;
    }
    switch (exc) {
    case ARMV7M_EXC_MEMMANAGE:
        ena = SHCSR_MEMFAULTENA;
        break;
    case ARMV7M_EXC_BUSFAULT:
        ena = SHCSR_BUSFAULTENA;
        break;
    case ARMV7M_EXC_USAGEFAULT:
        ena = SHCSR_USGFAULTENA;
        break;
    default:
        ena = 0u;
        break;
    }
    {
        const int32_t now = armv7m_exec_priority(c);
        const bool disabled = ena != 0u && (c->shcsr & ena) == 0u;

        if (disabled || group(c, exc_prio(c, exc)) >= now) {
            if (now <= -1) {
                lockup(c, pc);
                return;
            }
            c->hfsr |= HFSR_FORCED;
            exc = ARMV7M_EXC_HARDFAULT;
        }
    }
    (void)armv7m_take(c, exc, pc);
}

/* ------------------------------------------------------------------ */
/* Return                                                              */
/* ------------------------------------------------------------------ */

static void deactivate(armv7m_cpu_t *c, uint32_t exc)
{
    bit_put(c->active, exc, false);
    if (exc != ARMV7M_EXC_NMI) {
        c->faultmask = 0u;
    }
    /* A level-sensitive line still asserted pends again. */
    if (exc >= ARMV7M_EXC_EXTERNAL &&
        bit_get(c->line, exc - ARMV7M_EXC_EXTERNAL)) {
        bit_put(c->pending, exc, true);
    }
}

/*
 * The INVPC cases of ExceptionReturn: the UsageFault is taken *without* a
 * new frame -- the one being returned through is still on the stack --
 * and LR holds the EXC_RETURN value that failed.
 */
static void return_fault(armv7m_cpu_t *c, uint32_t ret_exc, uint32_t excret)
{
    deactivate(c, ret_exc);
    c->cfsr |= UFSR_INVPC;
    c->r[ARMV7M_LR] = excret;
    if ((c->shcsr & SHCSR_USGFAULTENA) != 0u &&
        group(c, exc_prio(c, ARMV7M_EXC_USAGEFAULT)) < armv7m_exec_priority(c)) {
        (void)exception_taken(c, ARMV7M_EXC_USAGEFAULT);
    } else if (armv7m_exec_priority(c) > -1) {
        c->hfsr |= HFSR_FORCED;
        (void)exception_taken(c, ARMV7M_EXC_HARDFAULT);
    } else {
        lockup(c, excret);
    }
}

static bool pop_word(armv7m_cpu_t *c, uint32_t addr, uint32_t *v)
{
    if (!mpu_permits(c, addr, true, ARMV7M_ACC_READ)) {
        c->cfsr |= MMFSR_MUNSTKERR;
        return false;
    }
    if (emu_bus_read(c->bus, addr, 4u, v) != EMU_FAULT_NONE) {
        c->cfsr |= BFSR_UNSTKERR;
        return false;
    }
    return true;
}

armv7m_exc_t armv7m_exc_return(armv7m_cpu_t *c, uint32_t excret)
{
    const uint32_t ret_exc = c->xpsr & ARMV7M_IPSR_MASK;
    uint32_t nested = 0u;

    for (uint32_t w = 0u; w < ARMV7M_NEXC_WORDS; w++) {
        nested += (uint32_t)__builtin_popcount(c->active[w]);
    }
    /* Bits 27:5 must be ones; bit 4 is the frame type. */
    if ((excret & 0x0FFFFFE0u) != 0x0FFFFFE0u || !bit_get(c->active, ret_exc)) {
        return_fault(c, ret_exc, excret);
        return OK;
    }

    bool to_thread;
    bool process;

    switch (excret & 15u) {
    case 1u:
        to_thread = false;
        process = false;
        break;
    case 9u:
        to_thread = true;
        process = false;
        break;
    case 13u:
        to_thread = true;
        process = true;
        break;
    default:
        return_fault(c, ret_exc, excret);
        return OK;
    }
    if (to_thread && nested != 1u && (c->ccr & CCR_NONBASETHRDENA) == 0u) {
        return_fault(c, ret_exc, excret);
        return OK;
    }

    deactivate(c, ret_exc);

    /* PopStack. */
    sp_save(c);
    {
        const bool fp = (excret & 0x10u) == 0u;
        const uint32_t framesize = fp ? 0x68u : 0x20u;
        const bool forcealign = fp || (c->ccr & CCR_STKALIGN) != 0u;
        const uint32_t frame = process ? c->psp : c->msp;
        uint32_t w[8];

        for (uint32_t i = 0u; i < 8u; i++) {
            if (!pop_word(c, frame + 4u * i, &w[i])) {
                /* An unstacking fault: taken from Handler mode, with the
                 * frame left where it was. */
                const bool mm = (c->cfsr & MMFSR_MUNSTKERR) != 0u;

                sp_load(c);
                armv7m_raise(c, mm ? ARMV7M_X_MEMMANAGE : ARMV7M_X_BUSFAULT,
                             excret);
                return OK;
            }
        }
        if (fp) {
            if ((c->fpccr & FPCCR_LSPACT) != 0u) {
                c->fpccr &= ~FPCCR_LSPACT; /* the registers are still live */
            } else {
                for (uint32_t i = 0u; i < 16u; i++) {
                    (void)pop_word(c, frame + 0x20u + 4u * i, &c->s[i]);
                }
                (void)pop_word(c, frame + 0x60u, &c->fpscr);
            }
        }
        c->control = (c->control & ~ARMV7M_CONTROL_FPCA) |
                     (fp ? ARMV7M_CONTROL_FPCA : 0u);

        const uint32_t sp = (frame + framesize) |
                            ((forcealign && (w[7] & (1u << 9))) ? 4u : 0u);

        if (process) {
            c->psp = sp;
        } else {
            c->msp = sp;
        }
        c->r[0] = w[0];
        c->r[1] = w[1];
        c->r[2] = w[2];
        c->r[3] = w[3];
        c->r[12] = w[4];
        c->r[ARMV7M_LR] = w[5];
        c->r[ARMV7M_PC] = w[6] & ~1u;
        /* APSR, IPSR and EPSR from the frame; bit 9 was only the aligner. */
        c->xpsr = w[7] & 0xFF0FFDFFu;
        c->control = (c->control & ~ARMV7M_CONTROL_SPSEL) |
                     (process ? ARMV7M_CONTROL_SPSEL : 0u);
        sp_load(c);
    }

    /* The IPSR the frame carried must agree with where it returned to. */
    if (!to_thread && (c->xpsr & ARMV7M_IPSR_MASK) == 0u) {
        c->cfsr |= UFSR_INVPC;
        (void)push_stack(c, c->r[ARMV7M_PC]);
        c->r[ARMV7M_LR] = excret;
        (void)exception_taken(c, ARMV7M_EXC_USAGEFAULT);
        return OK;
    }
    if (to_thread && (c->xpsr & ARMV7M_IPSR_MASK) != 0u) {
        c->cfsr |= UFSR_INVPC;
        (void)push_stack(c, c->r[ARMV7M_PC]);
        c->r[ARMV7M_LR] = excret;
        (void)exception_taken(c, ARMV7M_EXC_USAGEFAULT);
        return OK;
    }
    c->excl_open = false;
    return OK;
}
