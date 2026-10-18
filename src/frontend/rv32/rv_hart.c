/* SPDX-License-Identifier: Apache-2.0 */
/*
 * rv_hart.c - Hart lifecycle, trap entry, and permission-checked memory.
 */

#include "rv32/rv_hart.h"
#include "rv32/rv_decode.h"
#include "rv32/rv_jit.h"

#include <string.h>

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

uint32_t rv_hart_misa(void)
{
    uint32_t misa = MISA_MXL_32 | MISA_EXT('I');
#if RV_EXT_M
    misa |= MISA_EXT('M');
#endif
#if RV_EXT_A
    misa |= MISA_EXT('A');
#endif
#if RV_EXT_C
    misa |= MISA_EXT('C');
#endif
#if RV_EXT_F
    misa |= MISA_EXT('F');
#endif
#if RV_EXT_D
    misa |= MISA_EXT('D');
#endif
    /*
     * B is exactly Zba, Zbb and Zbs -- the three the 2024 ratification
     * folded into it. Zbc is deliberately absent from that list: it was
     * moved out of B before ratification and remains a separate extension,
     * so implementing it neither grants nor withholds this bit.
     *
     * Advertising it matters. A guest that checks misa.B and finds it clear
     * will take its fallback paths for byte swaps, counts and single-bit
     * work, which is the opposite of what a core implementing all three
     * wants. Anything built with -march=..._zba_zbb_zbs and run against a
     * core reporting no B is the same disagreement rv32mi/csr exists to
     * catch, just in the other direction.
     */
#if RV_EXT_ZBA && RV_EXT_ZBB && RV_EXT_ZBS
    misa |= MISA_EXT('B');
#endif
#if RV_EXT_S
    misa |= MISA_EXT('S');
#endif
#if RV_EXT_U
    misa |= MISA_EXT('U');
#endif
    return misa;
}

void rv_hart_init(rv_hart_t *h, emu_bus_t *bus, uint32_t hartid)
{
    memset(h, 0, sizeof(*h));
    h->bus = bus;
    h->hartid = hartid;
    rv_hart_reset(h, 0u);
}

void rv_hart_reset(rv_hart_t *h, uint32_t reset_pc)
{
    memset(h->x, 0, sizeof(h->x));
    h->pc = reset_pc;
    /* A new guest earns the peripheral window afresh. */
    h->pt_armed = false;

    /*
     * Reset values per the privileged spec: the only architecturally
     * required state is that the hart starts in M-mode with interrupts
     * globally disabled and mcause cleared.
     */
    h->mstatus = 0u;
    h->mie = 0u;
    h->mip = 0u;
    h->mtvec = 0u;
    h->mscratch = 0u;
    h->mepc = 0u;
    h->mcause = 0u;
    h->mtval = 0u;
    h->mcountinhibit = 0u;
    h->mcycle = 0u;
    h->minstret = 0u;
#if RV_EXT_ZIHPM
    memset(h->mhpmcounter, 0, sizeof(h->mhpmcounter));
    memset(h->mhpmevent, 0, sizeof(h->mhpmevent));
#if RV_EXT_SSCOFPMF
    memset(h->mhpmeventh, 0, sizeof(h->mhpmeventh));
#endif
    rv_hpm_rearm(h); /* derived: recomputed, never cleared by hand */
#endif
    h->split_valid = false;

#if RV_EXT_F
    memset(h->f, 0, sizeof(h->f));
    h->fcsr = 0u;
    /* Report the FPU as Initial so software can use it without first
     * writing mstatus; the spec leaves the reset value implementation
     * defined and leaving it Off would make every FP instruction trap. */
    h->mstatus |= (1u << MSTATUS_FS_SHIFT);
#endif

    h->mcounteren = 0u;
    h->menvcfg = 0u;
#if RV_EXT_S
    h->senvcfg = 0u;
#endif

#if RV_EXT_SV32
    h->satp = 0u;
    h->vm_active = false;
    rv_mmu_flush(h);
#endif

#if RV_EXT_S
    h->stvec = 0u;
    h->sscratch = 0u;
    h->sepc = 0u;
    h->scause = 0u;
    h->stval = 0u;
    h->scounteren = 0u;
    h->medeleg = 0u;
    h->mideleg = 0u;
#endif

#if RV_EXT_SDTRIG
    h->tselect = 0u;
    memset(h->tdata1, 0, sizeof(h->tdata1));
    memset(h->tdata2, 0, sizeof(h->tdata2));
    h->trig_active = false;
#endif

#if RV_EXT_PMP
    memset(h->pmpcfg, 0, sizeof(h->pmpcfg));
    memset(h->pmpaddr, 0, sizeof(h->pmpaddr));
    h->pmp_active = false;
#endif

#if RV_HAVE_DTLB
    /*
     * The PMP and translation it was built from are gone, and so may the
     * region table be: a reload rebuilds the bus before it resets. Its
     * own counters go too, so a reloaded guest's line is its own.
     */
    rv_dtlb_flush(h);
    h->dtlb_fills = 0u;
    h->dtlb_flushes = 0u;
    h->itlb_fills = 0u;
#endif

#if RV_EXT_A
    h->resv_valid = false;
    h->resv_addr = 0u;
#endif

    h->priv = RV_PRIV_M;
    h->state = EMU_STATE_RUNNING;

    /*
     * Last, and after every flag it summarises.
     *
     * fetch_guard is derived state -- the OR of trig_active, pmp_active
     * and vm_active -- and its invariant is that only
     * rv_hart_refresh_fetch_guard writes it. This function clears all
     * three by hand, so without this line the guard keeps whatever value
     * the *previous* guest left it with.
     *
     * That is not a wrong answer in this direction: the interpreter takes
     * its slow fetch path, finds all three flags false and permits. It is
     * the JIT that pays, because rv_jit_bind points the framework's
     * `blocked` at this word -- so a board that reloads a guest after one
     * that armed PMP runs the new guest **entirely interpreted**, for
     * ever. Measured on hardware before the fix: uploading `hello` after
     * `isatest` moved `interp` by exactly its 618 retired instructions
     * while `blks/xlat` and `blk entr` did not move at all.
     */
    rv_hart_refresh_fetch_guard(h);

#if RV_LAZY_IRQ_CHECK
    /* Force one evaluation after reset rather than assuming the state. */
    h->irq_dirty = true;
#endif

#if EMU_ENABLE_STATS
    h->trap_count = 0u;
    h->insn_retired_lo = 0u;
#endif
}

void rv_hart_boot(rv_hart_t *h, const emu_boot_info_t *info)
{
    /*
     * a0 is the hart id in every RISC-V boot convention there is.
     *
     * a1 is where they diverge, and the divergence is the point. The
     * supervisor and kernel convention -- what OpenSBI, U-Boot and Linux
     * all expect -- is *the address of a flattened device tree*. The
     * bare-metal guests in this tree have no device tree and were given
     * the RAM size there instead, which is this project's own convention
     * and is what `hello` and `coremark` read.
     *
     * So a1 follows the image: a device tree when one was supplied, the
     * RAM size when not. Passing 0 to a kernel would be a null pointer it
     * dereferences before it can print anything.
     */
    h->x[10] = h->hartid;
    h->x[11] = (info->dtb != 0u) ? info->dtb : info->ram_size;

    /*
     * The stack goes below the device tree, not at the top of RAM, or the
     * first thing a guest pushes overwrites the tree it was just handed.
     * 16-byte aligned, as the psABI requires.
     */
    const uint32_t top =
        (info->dtb != 0u) ? info->dtb : (info->ram_base + info->ram_size);

    h->x[2] = top & ~15u;
}

/* ------------------------------------------------------------------ */
/* Traps                                                               */
/* ------------------------------------------------------------------ */

#if RV_EXT_S
/*
 * Does this trap go to S-mode?
 *
 * Only if it is delegated *and* it came from S or U. Delegation cannot
 * move a trap taken in M-mode to a less privileged handler -- that would
 * hand the supervisor the machine's own faults -- so the privilege test is
 * not a shortcut for the common case, it is half the rule.
 */
static bool trap_to_s(const rv_hart_t *h, uint32_t cause)
{
    if (h->priv > RV_PRIV_S) {
        return false;
    }
    const uint32_t code = cause & ~RV_CAUSE_INTERRUPT;
    const uint32_t deleg =
        (cause & RV_CAUSE_INTERRUPT) ? h->mideleg : h->medeleg;
    return code < 32u && ((deleg >> code) & 1u) != 0u;
}
#endif

#if RV_EXT_C
/*
 * The instruction an illegal-instruction trap reports, as the guest wrote
 * it.
 *
 * Sstvala puts the faulting instruction in tval. A 16-bit one reaches
 * execution expanded to its 32-bit equivalent, so any rule that refuses
 * it after decode named the expansion: `c.flwsp ft0, 0(sp)` with
 * mstatus.FS off reported 0x00012007, the `flw` it becomes, where the
 * guest wrote 0x6002. Only the decoder's own refusal of a reserved
 * encoding had the parcel to hand.
 *
 * Every illegal-instruction trap comes through here with h->pc on the
 * instruction, from the interpreter and from a JIT helper alike, so the
 * parcel is read back here, on the trap path, rather than kept beside
 * the expansion on the fetch path, where a live register costs every
 * instruction. It was fetched a moment ago, so the read succeeds; if it
 * somehow does not, the value handed in stands.
 */
static EMU_COLD_NOINLINE uint32_t illegal_tval(rv_hart_t *h, uint32_t tval)
{
    uint32_t pa = h->pc;
    uint16_t parcel;

#if RV_EXT_SV32
    if (h->vm_active &&
        rv_mmu_translate(h, h->pc, EMU_ACC_FETCH, &pa) != RV_EXC_NONE) {
        return tval;
    }
#endif
    if (emu_bus_fetch16(h->bus, pa, &parcel) != EMU_FAULT_NONE ||
        rv_is_32bit(parcel)) {
        return tval;
    }
    return parcel;
}
#endif

void rv_hart_trap(rv_hart_t *h, uint32_t cause, uint32_t tval)
{
    uint32_t tvec;
#if RV_EXT_ZIHPM
    /* The mode the event happened in, for Sscofpmf's filters. */
    const uint32_t from = h->priv;
#endif

    /*
     * An access that crossed a page boundary and was refused in the
     * second page: its caller has just handed over the address it asked
     * for, and the address that matters is the one recorded. Applied only
     * if this trap is for that access -- same address, not an interrupt
     * -- and cleared either way, so a record nobody collected cannot
     * outlive the next trap.
     */
    if (EMU_UNLIKELY(h->split_valid)) {
        if ((cause & RV_CAUSE_INTERRUPT) == 0u && tval == h->split_req) {
            tval = h->split_at;
        }
        h->split_valid = false;
    }
#if RV_EXT_C
    if (cause == RV_EXC_ILLEGAL_INSN) {
        tval = illegal_tval(h, tval);
    }
#endif

#if RV_EXT_S
    if (trap_to_s(h, cause)) {
        h->sepc = h->pc;
        h->scause = cause;
        h->stval = tval;

        /*
         * The same enable stack as the machine one, one level down: SPIE
         * takes the old SIE, SIE clears, SPP records where we came from.
         * SPP is a single bit because the only modes that can reach an
         * S-mode handler are S and U.
         */
        const uint32_t sie_was = (h->mstatus & MSTATUS_SIE) ? MSTATUS_SPIE : 0u;
        h->mstatus =
            (h->mstatus & ~(MSTATUS_SIE | MSTATUS_SPIE | MSTATUS_SPP)) |
            sie_was | ((h->priv == RV_PRIV_S) ? MSTATUS_SPP : 0u);

        h->priv = RV_PRIV_S;
        tvec = h->stvec;
    } else
#endif
    {
        h->mepc = h->pc;
        h->mcause = cause;
        h->mtval = tval;

        /*
         * Push the interrupt-enable stack: MPIE takes the old MIE, MIE
         * clears, and MPP records the privilege we came from.
         */
        const uint32_t mie_was = (h->mstatus & MSTATUS_MIE) ? MSTATUS_MPIE : 0u;
        h->mstatus =
            (h->mstatus & ~(MSTATUS_MIE | MSTATUS_MPIE | MSTATUS_MPP_MASK)) |
            mie_was | ((uint32_t)h->priv << MSTATUS_MPP_SHIFT);

        h->priv = RV_PRIV_M;
        tvec = h->mtvec;
    }

#if RV_EXT_U
    /*
     * Refreshing PMP is not bookkeeping: whether PMP has to be consulted at
     * all depends on the privilege level, because below M matching no entry
     * denies rather than permits. A trap into S-mode raises privilege
     * without reaching M, so this is as load-bearing there as anywhere.
     */
#if RV_EXT_PMP
    rv_pmp_refresh(h);
#endif
#if RV_EXT_SV32
    rv_mmu_refresh(h);
#endif
#endif

#if EMU_HAVE_JIT
    /*
     * The JIT's context, for the same reason: it carries the privilege.
     * The interpreter fallback re-derives it after every instruction, but
     * a trap is not always an instruction the fallback ran -- rv_ir_load
     * raises one from inside a translated block and the interrupt hook
     * between blocks -- and the next lookup is for the handler.
     */
    h->jit_ctx = rv_jit_ctx_key(h);
#endif

    uint32_t base = tvec & ~MTVEC_MODE_MASK;
    if ((tvec & MTVEC_MODE_MASK) == MTVEC_MODE_VECTORED &&
        (cause & RV_CAUSE_INTERRUPT)) {
        /* Vectored mode applies to interrupts only; exceptions use base. */
        base += 4u * (cause & ~RV_CAUSE_INTERRUPT);
    }
    h->pc = base;

    /*
     * A trap breaks any outstanding LR/SC sequence. The spec allows an
     * implementation to keep the reservation, but dropping it is simpler to
     * reason about and guarantees forward progress cannot depend on it.
     */
#if RV_EXT_A
    h->resv_valid = false;
#endif

#if RV_EXT_ZIHPM
    /* One load and a branch, per trap, for a guest counting nothing. */
    if (EMU_UNLIKELY(h->hpm_armed != 0u)) {
        rv_hpm_note_trap(h, cause, from);
    }
#endif

#if EMU_ENABLE_STATS
    h->trap_count++;
#endif
}

rv_exc_t rv_hart_pending_irq(const rv_hart_t *h)
{
    uint32_t active = h->mip & h->mie;
    if (active == 0u) {
        return RV_EXC_NONE;
    }

    /*
     * mstatus.MIE gates M-mode interrupts *only while running in M-mode*.
     * Below M they are always enabled, because a less privileged mode is
     * not allowed to mask the machine's own interrupts by clearing a bit it
     * cannot even see. Testing MIE unconditionally -- which is all that was
     * needed while M was the only mode -- means a guest that MRETs to U
     * with MPIE clear never takes another timer interrupt and simply hangs.
     */
    if (h->priv == RV_PRIV_M && (h->mstatus & MSTATUS_MIE) == 0u) {
        /* Delegated interrupts are not a way around this: an S-mode
         * interrupt does not preempt M-mode either. */
        return RV_EXC_NONE;
    }

#if RV_EXT_S
    /*
     * A delegated interrupt is destined for S-mode, so it obeys SIE rather
     * than MIE and only while running in S; in M-mode it cannot be taken at
     * all, and in U-mode it is always enabled. Undelegated interrupts stay
     * with M and are already enabled by the test above.
     */
    if (h->priv == RV_PRIV_M) {
        active &= ~h->mideleg;
    } else if (h->priv == RV_PRIV_S && (h->mstatus & MSTATUS_SIE) == 0u) {
        active &= ~h->mideleg;
    }
    if (active == 0u) {
        return RV_EXC_NONE;
    }
#endif

    /* Priority order from the privileged spec: MEI, MSI, MTI, then SEI,
     * SSI, STI, then LCOFI. Machine interrupts outrank supervisor ones
     * throughout. */
    if (active & MIP_MEIP) {
        return RV_INT_M_EXT;
    }
    if (active & MIP_MSIP) {
        return RV_INT_M_SOFT;
    }
    if (active & MIP_MTIP) {
        return RV_INT_M_TIMER;
    }
#if RV_EXT_S
    if (active & MIP_SEIP) {
        return RV_INT_S_EXT;
    }
    if (active & MIP_SSIP) {
        return RV_INT_S_SOFT;
    }
#if RV_EXT_SSCOFPMF
    /* Below every standard interrupt (Sscofpmf, "LCOFI"). */
    if (active & MIP_STIP) {
        return RV_INT_S_TIMER;
    }
    return RV_INT_LCOF;
#else
    return RV_INT_S_TIMER;
#endif
#else
    return RV_INT_M_TIMER;
#endif
}

void rv_hart_set_irq(rv_hart_t *h, unsigned cause, bool level)
{
    const uint32_t bit = 1u << cause;
    if (level) {
        h->mip |= bit;
    } else {
        h->mip &= ~bit;
    }

#if RV_LAZY_IRQ_CHECK
    /*
     * Dirty unconditionally rather than only when mip changed: this is
     * called from device code and, on some platforms, from an ARM
     * interrupt handler, and an extra evaluation costs far less than
     * reasoning about whether a redundant-looking write really was.
     */
    h->irq_dirty = true;
#endif
}

/* ------------------------------------------------------------------ */
/* Memory access                                                       */
/* ------------------------------------------------------------------ */

/* An exception, and the address it is about. Cold by construction. */
static EMU_ALWAYS_INLINE rv_exc_t mem_fault(uint32_t *tval, uint32_t va,
                                            rv_exc_t exc)
{
    *tval = va;
    return exc;
}

/*
 * An access that is not one aligned bus transaction: a doubleword, or
 * anything that runs across a page boundary.
 *
 * **Everything that can refuse it is asked about all of it before any of
 * it happens.** Three things can -- translation, PMP and the bus -- and
 * each is asked per *part*, where a part is what lies in one page: with
 * paging on, two virtually adjacent pages are two unrelated pieces of
 * physical memory, and the second has its own translation, its own
 * permissions and possibly nothing behind it at all.
 *
 * That is two separate defects this replaces, and they were the same
 * mistake at two widths:
 *
 *   - FSD was two word stores. A fault on the second -- a page not yet
 *     mapped, a PMP entry over the upper word -- arrived with the first
 *     already written.
 *   - A misaligned word access was translated once, at its first byte,
 *     and then read or written as four *physically* contiguous bytes. Run
 *     across a page boundary it reached into whatever physical page
 *     happened to follow, not the page the guest had mapped next, and it
 *     never faulted when that next page was not there. RV32_MISALIGNED
 *     is on by default, so every Linux boot ran with it.
 *
 * `val` is the value for a store and receives it for a load, little-
 * endian in both directions. A fault names the part it is in: for one
 * that crosses a page, the *second* page's address when that is where
 * the refusal came from, which is the address a fault handler needs.
 */
static rv_exc_t mem_wide(rv_hart_t *h, uint32_t va, uint32_t size,
                         emu_access_t acc, uint64_t *val, uint32_t *tval)
{
    const bool store = (acc == EMU_ACC_STORE);
    const rv_exc_t refused =
        store ? RV_EXC_STORE_ACCESS_FAULT : RV_EXC_LOAD_ACCESS_FAULT;
    const uint8_t need = store ? EMU_PERM_W : EMU_PERM_R;
    uint32_t pa[2] = {va, 0u};
    uint32_t len[2] = {size, 0u};
    unsigned parts = 1u;

#if RV_EXT_SV32
    if (h->vm_active) {
        const uint32_t room = 0x1000u - (va & 0xFFFu);

        if (room < size) {
            len[0] = room;
            len[1] = size - room;
            parts = 2u;
        }
        for (unsigned i = 0; i < parts; i++) {
            const uint32_t part = va + ((i != 0u) ? len[0] : 0u);
            const rv_exc_t e = rv_mmu_translate(h, part, acc, &pa[i]);

            if (e != RV_EXC_NONE) {
                return mem_fault(tval, part, e);
            }
        }
    }
#endif

    for (unsigned i = 0; i < parts; i++) {
        const uint32_t part = va + ((i != 0u) ? len[0] : 0u);

#if RV_EXT_PMP
        if (h->pmp_active && !rv_pmp_check(h, pa[i], len[i], acc)) {
            return mem_fault(tval, part, refused);
        }
#endif
        /* The bus, asked without doing anything: a region, of the right
         * kind, holding the whole part. */
        const emu_region_t *const r = emu_bus_find(h->bus, pa[i]);

        if (r == NULL || (r->perm & need) == 0u ||
            (pa[i] - r->base) + len[i] > r->size) {
            return mem_fault(tval, part, refused);
        }
    }

    /*
     * Nothing can refuse it now. Each part goes out in the largest
     * naturally aligned transactions that fit, which for an aligned
     * doubleword is the two words it always was.
     */
    uint64_t v = store ? *val : 0u;
    unsigned done = 0u;

    for (unsigned i = 0; i < parts; i++) {
        for (uint32_t off = 0u; off < len[i];) {
            const uint32_t a = pa[i] + off;
            uint32_t n = 4u;
            emu_fault_t f;

            while (n > 1u && ((a & (n - 1u)) != 0u || off + n > len[i])) {
                n >>= 1;
            }
            if (store) {
#if RV_EXT_A
                if (h->resv_valid && (a & ~3u) == h->resv_addr) {
                    h->resv_valid = false; /* as rv_hart_store does */
                }
#endif
                f = emu_bus_write(h->bus, a, n, (uint32_t)(v >> (done * 8u)));
            } else {
                uint32_t w = 0u;

                f = emu_bus_read(h->bus, a, n, &w);
                v |= (uint64_t)w << (done * 8u);
            }
            if (EMU_UNLIKELY(f != EMU_FAULT_NONE)) {
                /* A device that takes the region and not the width. */
                return mem_fault(tval, va + done, rv_exc_from_fault(f));
            }
            off += n;
            done += n;
        }
    }
    if (!store) {
        *val = v;
    }
    return RV_EXC_NONE;
}

#if RV_EXT_D
/*
 * Sdtrig matches an access by its address, and a doubleword used to be
 * two accesses with two addresses. Both are still offered, so a trigger
 * set on the upper word of a double fires as it did.
 */
static bool wide_trigger(const rv_hart_t *h, uint32_t addr, emu_access_t acc)
{
#if RV_EXT_SDTRIG
    return h->trig_active &&
           (rv_trig_check(h, addr, acc) || rv_trig_check(h, addr + 4u, acc));
#else
    (void)h;
    (void)addr;
    (void)acc;
    return false;
#endif
}

rv_exc_t rv_hart_load64(rv_hart_t *h, uint32_t addr, uint64_t *out,
                        uint32_t *tval)
{
#if !RV_MISALIGNED_OK
    /* Eight bytes are aligned to eight. Two aligned words are not. */
    if (EMU_UNLIKELY((addr & 7u) != 0u)) {
        return mem_fault(tval, addr, RV_EXC_LOAD_MISALIGNED);
    }
#endif
    if (EMU_UNLIKELY(wide_trigger(h, addr, EMU_ACC_LOAD))) {
        return mem_fault(tval, addr, RV_EXC_BREAKPOINT);
    }
    return mem_wide(h, addr, 8u, EMU_ACC_LOAD, out, tval);
}

rv_exc_t rv_hart_store64(rv_hart_t *h, uint32_t addr, uint64_t val,
                         uint32_t *tval)
{
#if !RV_MISALIGNED_OK
    if (EMU_UNLIKELY((addr & 7u) != 0u)) {
        return mem_fault(tval, addr, RV_EXC_STORE_MISALIGNED);
    }
#endif
    if (EMU_UNLIKELY(wide_trigger(h, addr, EMU_ACC_STORE))) {
        return mem_fault(tval, addr, RV_EXC_BREAKPOINT);
    }
    return mem_wide(h, addr, 8u, EMU_ACC_STORE, &val, tval);
}
#endif /* RV_EXT_D */

#if RV_MISALIGNED_OK && RV_EXT_SV32
/*
 * A word or halfword that runs across a page boundary. Out of line and
 * cold: the two callers below test for it inside the vm_active branch.
 *
 * The caller of rv_hart_load will trap with the address it asked for.
 * If the refusal came from the second page, that is the wrong address,
 * and this is where it is recorded for rv_hart_trap to apply.
 */
EMU_COLD_NOINLINE static rv_exc_t split_access(rv_hart_t *h, uint32_t va,
                                               uint32_t size, emu_access_t acc,
                                               uint64_t *val)
{
    uint32_t at = va;
    const rv_exc_t e = mem_wide(h, va, size, acc, val, &at);

    if (e != RV_EXC_NONE && at != va) {
        h->split_req = va;
        h->split_at = at;
        h->split_valid = true;
    }
    return e;
}

EMU_COLD_NOINLINE static rv_exc_t load_split(rv_hart_t *h, uint32_t va,
                                             uint32_t size, bool sign_extend,
                                             uint32_t *out)
{
    uint64_t wide = 0u;
    const rv_exc_t e = split_access(h, va, size, EMU_ACC_LOAD, &wide);

    if (e == RV_EXC_NONE) {
        *out = (sign_extend && size < 4u)
                   ? (uint32_t)emu_sext((uint32_t)wide, size * 8u)
                   : (uint32_t)wide;
    }
    return e;
}

EMU_COLD_NOINLINE static rv_exc_t store_split(rv_hart_t *h, uint32_t va,
                                              uint32_t size, uint32_t val)
{
    uint64_t wide = val;

    return split_access(h, va, size, EMU_ACC_STORE, &wide);
}
#endif /* RV_MISALIGNED_OK && RV_EXT_SV32 */

/*
 * Whether a data access has to be translated or checked at all. False for
 * every guest that stays in M-mode without locking a PMP entry, which is
 * the case rv_hart_load and rv_hart_store are shaped for.
 */
static EMU_ALWAYS_INLINE bool data_guarded(const rv_hart_t *h)
{
    bool g = false;
#if RV_EXT_SV32
    g = g | h->vm_active;
#endif
#if RV_EXT_PMP
    g = g | h->pmp_active;
#endif
    return g;
}

#if RV_HAVE_DTLB
/*
 * The data TLB's entry for `va` at the hart's data privilege, or NULL --
 * the same probe both JIT backends emit. Misaligned is a miss: an entry
 * vouches for aligned accesses, which cannot leave its page.
 */
static EMU_ALWAYS_INLINE const emu_dtlb_entry_t *
dtlb_probe(const rv_hart_t *h, uint32_t va, uint32_t size, bool store)
{
    const uint32_t priv = rv_hart_data_priv(h);
    const uint32_t vpn = va >> EMU_DTLB_PAGE_SHIFT;

    if (priv == RV_PRIV_M || (va & (size - 1u)) != 0u) {
        return NULL;
    }
    const emu_dtlb_entry_t *const e =
        &h->dtlb[rv_dtlb_table(priv)][vpn & (RV_DTLB_ENTRIES - 1u)];

    return ((store ? e->write : e->read) == vpn) ? e : NULL;
}
#endif

/*
 * A load under paging or PMP. Out of line so that the unguarded path --
 * every access of an M-mode guest -- keeps the frame it had; not cold,
 * because under an operating system it is every access.
 */
EMU_NOINLINE static rv_exc_t load_guarded(rv_hart_t *h, uint32_t addr,
                                          uint32_t size, bool sign_extend,
                                          uint32_t *out)
{
    uint32_t v;

#if RV_HAVE_DTLB
    const uint32_t va = addr;
    const emu_dtlb_entry_t *const e = dtlb_probe(h, va, size, false);

    if (e != NULL) {
        const uint8_t *const p = (const uint8_t *)(va + e->addend);

        if (size == 1u) {
            v = *p;
        } else if (size == 2u) {
            uint16_t w;

            memcpy(&w, p, 2u);
            v = w;
        } else {
            memcpy(&v, p, 4u);
        }
        goto done;
    }
#endif
#if RV_EXT_SV32
    /*
     * Translate before PMP: PMP describes physical memory, so it has to
     * see the address the bus will.
     */
    if (h->vm_active) {
#if RV_MISALIGNED_OK
        /*
         * Across a page boundary, which only a misaligned access can
         * be. Asked here, under vm_active, so a guest that never turns
         * paging on does not pay for the question.
         */
        if (EMU_UNLIKELY((addr & 0xFFFu) + size > 0x1000u)) {
            return load_split(h, addr, size, sign_extend, out);
        }
#endif
        const rv_exc_t texc = rv_mmu_translate(h, addr, EMU_ACC_LOAD, &addr);
        if (EMU_UNLIKELY(texc != RV_EXC_NONE)) {
            return texc;
        }
    }
#endif
#if RV_EXT_PMP
    if (h->pmp_active && !rv_pmp_check(h, addr, size, EMU_ACC_LOAD)) {
        return RV_EXC_LOAD_ACCESS_FAULT;
    }
#endif

    const emu_fault_t f = emu_bus_read(h->bus, addr, size, &v);
    if (EMU_UNLIKELY(f != EMU_FAULT_NONE)) {
        return rv_exc_from_fault(f);
    }
#if RV_HAVE_DTLB
    rv_dtlb_fill(h, va, addr);
done:
#endif
    if (sign_extend && size < 4u) {
        v = (uint32_t)emu_sext(v, size * 8u);
    }
    *out = v;
    return RV_EXC_NONE;
}

/*
 * Alignment is checked here rather than in the bus so the correct
 * misaligned-vs-access-fault cause can be reported, and so the bus can
 * assume aligned accesses on its fast paths.
 */
rv_exc_t rv_hart_load(rv_hart_t *h, uint32_t addr, uint32_t size,
                      bool sign_extend, uint32_t *out)
{
#if !RV_MISALIGNED_OK
    if (EMU_UNLIKELY((addr & (size - 1u)) != 0u)) {
        return RV_EXC_LOAD_MISALIGNED;
    }
#endif

#if RV_EXT_SDTRIG
    if (EMU_UNLIKELY(h->trig_active) && rv_trig_check(h, addr, EMU_ACC_LOAD)) {
        return RV_EXC_BREAKPOINT;
    }
#endif
    if (EMU_UNLIKELY(data_guarded(h))) {
        return load_guarded(h, addr, size, sign_extend, out);
    }

    uint32_t v;
    const emu_fault_t f = emu_bus_read(h->bus, addr, size, &v);
    if (EMU_UNLIKELY(f != EMU_FAULT_NONE)) {
        return rv_exc_from_fault(f);
    }

    if (sign_extend && size < 4u) {
        v = (uint32_t)emu_sext(v, size * 8u);
    }
    *out = v;
    return RV_EXC_NONE;
}

#if RV_EXT_A

bool rv_amo_valid(uint32_t funct5)
{
    switch (funct5) {
    case RV_AMO_ADD:
    case RV_AMO_SWAP:
    case RV_AMO_LR:
    case RV_AMO_SC:
    case RV_AMO_XOR:
    case RV_AMO_OR:
    case RV_AMO_AND:
    case RV_AMO_MIN:
    case RV_AMO_MAX:
    case RV_AMO_MINU:
    case RV_AMO_MAXU:
        return true;
#if RV_EXT_ZACAS
    case RV_AMO_CAS:
        return true;
#endif
    default:
        return false;
    }
}

rv_exc_t rv_hart_amo(rv_hart_t *h, uint32_t funct5, uint32_t rd, uint32_t addr,
                     uint32_t src)
{
    /* Every AMO requires a naturally aligned address. */
    if (EMU_UNLIKELY((addr & 3u) != 0u)) {
        return (funct5 == RV_AMO_LR) ? RV_EXC_LOAD_MISALIGNED
                                     : RV_EXC_STORE_MISALIGNED;
    }

    /*
     * **Translate and check, exactly as rv_hart_load and rv_hart_store
     * do.** This did neither: every path below went straight to the bus
     * with whatever address it was handed.
     *
     * With satp Bare that is invisible, because a virtual address is a
     * physical one -- which is every guest in this tree and every
     * architecture test that does not page, so nothing could see it. Turn
     * paging on and every atomic fails: a Linux guest dies on its first
     * `amoswap.w` with a store access fault at a kernel virtual address,
     * and because the fault handler's own locking is atomic, it faults
     * again on the same address for ever.
     *
     * It was also a **PMP bypass**. Below M, an atomic reached memory the
     * entries denied. That is the same shape as the JIT's inlined store
     * writing guest RAM without checking, and it is the reason to state
     * the rule rather than the fix: anything that reaches emu_bus_read or
     * emu_bus_write from this file owes the address a translation and a
     * PMP check first.
     *
     * LR is a load; every other form is reported as a store, which is
     * what the architecture says an AMO's faults are. Checking W is
     * enough for the read as well, because the walk already rejects the
     * write-without-read encoding.
     */
    const emu_access_t amo_acc =
        (funct5 == RV_AMO_LR) ? EMU_ACC_LOAD : EMU_ACC_STORE;

#if RV_EXT_SV32
    if (EMU_UNLIKELY(h->vm_active)) {
        const rv_exc_t texc = rv_mmu_translate(h, addr, amo_acc, &addr);

        if (EMU_UNLIKELY(texc != RV_EXC_NONE)) {
            return texc;
        }
    }
#endif
#if RV_EXT_PMP
    if (EMU_UNLIKELY(h->pmp_active) && !rv_pmp_check(h, addr, 4u, amo_acc)) {
        return (funct5 == RV_AMO_LR) ? RV_EXC_LOAD_ACCESS_FAULT
                                     : RV_EXC_STORE_ACCESS_FAULT;
    }
#endif

    if (funct5 == RV_AMO_LR) {
        uint32_t v;
        const emu_fault_t f = emu_bus_read(h->bus, addr, 4u, &v);
        if (EMU_UNLIKELY(f != EMU_FAULT_NONE)) {
            return rv_exc_from_fault(f);
        }
        h->resv_addr = addr;
        h->resv_valid = true;
        if (rd != 0u) {
            h->x[rd] = v;
        }
        return RV_EXC_NONE;
    }

    if (funct5 == RV_AMO_SC) {
        /*
         * **Whether the store could land is asked before whether it
         * will.** A failing SC writes nothing, so it never reached the
         * bus -- and an address with no memory behind it, which a real
         * store reports as an access fault, came back as an ordinary
         * "reservation lost". Translation and PMP were already checked
         * above for exactly this reason; the bus was the third thing
         * that can refuse and the one that was missed.
         */
        const emu_region_t *const r = emu_bus_find(h->bus, addr);

        if (EMU_UNLIKELY(r == NULL || (r->perm & EMU_PERM_W) == 0u)) {
            return RV_EXC_STORE_ACCESS_FAULT;
        }
        if (!h->resv_valid || h->resv_addr != addr) {
            if (rd != 0u) {
                h->x[rd] = 1u; /* non-zero: the store did not occur */
            }
            return RV_EXC_NONE;
        }
        const emu_fault_t f = emu_bus_write(h->bus, addr, 4u, src);
        h->resv_valid = false;
        if (EMU_UNLIKELY(f != EMU_FAULT_NONE)) {
            return rv_exc_from_fault(f);
        }
        if (rd != 0u) {
            h->x[rd] = 0u; /* zero: success */
        }
        return RV_EXC_NONE;
    }

    uint32_t old;
    emu_fault_t f = emu_bus_read(h->bus, addr, 4u, &old);
    if (EMU_UNLIKELY(f != EMU_FAULT_NONE)) {
        /* An AMO reports load and store faults alike as store. */
        return RV_EXC_STORE_ACCESS_FAULT;
    }

#if RV_EXT_ZACAS
    if (funct5 == RV_AMO_CAS) {
        /*
         * amocas.w: rd is both the comparand and the destination, so its
         * old value must be read before rd is written. The store happens
         * only on a match, but rd is updated either way.
         */
        if (old == h->x[rd]) {
            f = emu_bus_write(h->bus, addr, 4u, src);
            if (EMU_UNLIKELY(f != EMU_FAULT_NONE)) {
                return rv_exc_from_fault(f);
            }
        }
        if (rd != 0u) {
            h->x[rd] = old;
        }
        return RV_EXC_NONE;
    }
#endif

    uint32_t val;
    switch (funct5) {
    case RV_AMO_ADD:
        val = old + src;
        break;
    case RV_AMO_SWAP:
        val = src;
        break;
    case RV_AMO_XOR:
        val = old ^ src;
        break;
    case RV_AMO_OR:
        val = old | src;
        break;
    case RV_AMO_AND:
        val = old & src;
        break;
    case RV_AMO_MIN:
        val = ((int32_t)old < (int32_t)src) ? old : src;
        break;
    case RV_AMO_MAX:
        val = ((int32_t)old > (int32_t)src) ? old : src;
        break;
    case RV_AMO_MINU:
        val = (old < src) ? old : src;
        break;
    default:
        val = (old > src) ? old : src;
        break; /* MAXU */
    }

    f = emu_bus_write(h->bus, addr, 4u, val);
    if (EMU_UNLIKELY(f != EMU_FAULT_NONE)) {
        return rv_exc_from_fault(f);
    }
    /* rd takes the value read, written after the store in case rd == rs2. */
    if (rd != 0u) {
        h->x[rd] = old;
    }
    return RV_EXC_NONE;
}

#if RV_EXT_ZACAS
rv_exc_t rv_hart_amocas_d(rv_hart_t *h, uint32_t rd, uint32_t rs2,
                          uint32_t addr)
{
    /* A 64-bit atomic needs 8-byte alignment, not 4. */
    if (EMU_UNLIKELY((addr & 7u) != 0u)) {
        return RV_EXC_STORE_MISALIGNED;
    }

    /*
     * The same translation and check as rv_hart_amo. One translation
     * covers both halves: the address is 8-byte aligned, so an 8-byte
     * access cannot straddle a page.
     */
#if RV_EXT_SV32
    if (EMU_UNLIKELY(h->vm_active)) {
        const rv_exc_t texc =
            rv_mmu_translate(h, addr, EMU_ACC_STORE, &addr);

        if (EMU_UNLIKELY(texc != RV_EXC_NONE)) {
            return texc;
        }
    }
#endif
#if RV_EXT_PMP
    if (EMU_UNLIKELY(h->pmp_active) &&
        !rv_pmp_check(h, addr, 8u, EMU_ACC_STORE)) {
        return RV_EXC_STORE_ACCESS_FAULT;
    }
#endif

    uint32_t lo, hi;
    if (emu_bus_read(h->bus, addr, 4u, &lo) != EMU_FAULT_NONE ||
        emu_bus_read(h->bus, addr + 4u, 4u, &hi) != EMU_FAULT_NONE) {
        return RV_EXC_STORE_ACCESS_FAULT;
    }

    /* x0 names a pair that reads as zero in both halves. */
    const uint32_t cmp_lo = (rd == 0u) ? 0u : h->x[rd];
    const uint32_t cmp_hi = (rd == 0u) ? 0u : h->x[rd + 1u];

    if (lo == cmp_lo && hi == cmp_hi) {
        const uint32_t new_lo = (rs2 == 0u) ? 0u : h->x[rs2];
        const uint32_t new_hi = (rs2 == 0u) ? 0u : h->x[rs2 + 1u];
        emu_fault_t f = emu_bus_write(h->bus, addr, 4u, new_lo);
        if (EMU_UNLIKELY(f != EMU_FAULT_NONE)) {
            return rv_exc_from_fault(f);
        }
        f = emu_bus_write(h->bus, addr + 4u, 4u, new_hi);
        if (EMU_UNLIKELY(f != EMU_FAULT_NONE)) {
            return rv_exc_from_fault(f);
        }
    }

    /* The loaded value goes to rd either way. */
    if (rd != 0u) {
        h->x[rd] = lo;
        h->x[rd + 1u] = hi;
    }
    return RV_EXC_NONE;
}
#endif /* RV_EXT_ZACAS */

#if RV_EXT_ZALASR
/*
 * Zalasr: lb/lh/lw.aq[rl] and sb/sh/sw.[aq]rl.
 *
 * A load-acquire is a load and a store-release is a store. What the
 * extension adds is an ordering guarantee against other harts, and with
 * one hart and no store buffer the order things happen in is already
 * the order they were asked for -- so the work here is the encoding and
 * the two ways these differ from lw and sw:
 *
 *   - **the ordering bit is part of the opcode.** A load must have aq
 *     set and a store must have rl; the other bit is optional. The
 *     encodings with the mandatory bit clear are reserved, as is a load
 *     with a non-zero rs2 field or a store with a non-zero rd, and a
 *     reserved encoding here is an illegal instruction.
 *   - **the address must be naturally aligned, always.** rv_hart_load
 *     and rv_hart_store emulate a misaligned access when
 *     RV_MISALIGNED_OK is set, which is right for lw and not available
 *     to an atomic: so the test is made here, before either is called.
 *
 * Everything else -- translation, PMP, a trigger, the reservation a
 * store breaks, which fault a device reports -- is those two functions,
 * called rather than copied. Loads sign-extend, as lb and lh do; there
 * is no unsigned form.
 */
static rv_exc_t rv_hart_zalasr(rv_hart_t *h, uint32_t insn, uint32_t *tval)
{
    const bool store = ((rv_funct7(insn) >> 2) == RV_AMO_STORE_REL);
    const bool aq = (insn & (1u << 26)) != 0u;
    const bool rl = (insn & (1u << 25)) != 0u;
    const uint32_t f3 = rv_funct3(insn);
    const uint32_t addr = h->x[rv_rs1(insn)];

    /* 8, 16 and 32 bits; funct3 3 is ld.aq/sd.rl and RV64 only. */
    if (EMU_UNLIKELY(f3 > 2u)) {
        return RV_EXC_ILLEGAL_INSN;
    }

    const uint32_t size = 1u << f3;

    if (store) {
        if (EMU_UNLIKELY(!rl || rv_rd(insn) != 0u)) {
            return RV_EXC_ILLEGAL_INSN;
        }
        *tval = addr;
        if (EMU_UNLIKELY((addr & (size - 1u)) != 0u)) {
            return RV_EXC_STORE_MISALIGNED;
        }
        return rv_hart_store(h, addr, size, h->x[rv_rs2(insn)]);
    }

    if (EMU_UNLIKELY(!aq || rv_rs2(insn) != 0u)) {
        return RV_EXC_ILLEGAL_INSN;
    }
    *tval = addr;
    if (EMU_UNLIKELY((addr & (size - 1u)) != 0u)) {
        return RV_EXC_LOAD_MISALIGNED;
    }

    uint32_t v;
    const rv_exc_t exc = rv_hart_load(h, addr, size, true, &v);

    if (exc == RV_EXC_NONE && rv_rd(insn) != 0u) {
        h->x[rv_rd(insn)] = v;
    }
    return exc;
}
#endif /* RV_EXT_ZALASR */

/*
 * One instruction of the AMO opcode, whole: which operation it is,
 * whether that encoding exists, the operation, and what it reports.
 *
 * **Both backends call this and neither decodes an atomic for itself.**
 * They used to -- the interpreter's switch and the translator's helper
 * carried the same dispatch "deliberately line for line", which is a
 * second copy kept in step by intention. It held for LR/SC, the AMOs and
 * amocas; a fourth group was the moment to stop relying on it.
 *
 * `tval` is what the trap records: the encoding for an illegal
 * instruction, the address for everything else.
 */
rv_exc_t rv_hart_atomic(rv_hart_t *h, uint32_t insn, uint32_t *tval)
{
    const uint32_t funct5 = rv_funct7(insn) >> 2;
    const uint32_t f3 = rv_funct3(insn);
    rv_exc_t exc;

    *tval = insn;

#if RV_EXT_ZALASR
    if (funct5 == RV_AMO_LOAD_ACQ || funct5 == RV_AMO_STORE_REL) {
        return rv_hart_zalasr(h, insn, tval);
    }
#endif

#if RV_EXT_ZACAS
    if (f3 == 3u && funct5 == RV_AMO_CAS) {
        /* amocas.d: even-odd register pairs, so an odd operand is not
         * encodable and must raise illegal instruction. */
        if (EMU_UNLIKELY((rv_rd(insn) & 1u) != 0u ||
                         (rv_rs2(insn) & 1u) != 0u)) {
            return RV_EXC_ILLEGAL_INSN;
        }
        *tval = h->x[rv_rs1(insn)];
        exc = rv_hart_amocas_d(h, rv_rd(insn), rv_rs2(insn), *tval);
        h->x[0] = 0u;
        return exc;
    }
#endif

    /* Only the 32-bit width, a defined operation, and LR takes no rs2. */
    if (EMU_UNLIKELY(f3 != 2u || !rv_amo_valid(funct5) ||
                     (funct5 == RV_AMO_LR && rv_rs2(insn) != 0u))) {
        return RV_EXC_ILLEGAL_INSN;
    }

    *tval = h->x[rv_rs1(insn)];
    exc = rv_hart_amo(h, funct5, rv_rd(insn), *tval, h->x[rv_rs2(insn)]);
    h->x[0] = 0u; /* rv_hart_amo skips rd == 0; keep x0 canonical */
    return exc;
}

#endif /* RV_EXT_A */

#if RV_EXT_ZICBOM || RV_EXT_ZICBOZ

bool rv_cbo_valid(uint32_t op)
{
    switch (op) {
#if RV_EXT_ZICBOZ
    case RV_CBO_OP_ZERO:
        return true;
#endif
#if RV_EXT_ZICBOM
    case RV_CBO_OP_INVAL:
    case RV_CBO_OP_CLEAN:
    case RV_CBO_OP_FLUSH:
        return true;
#endif
    default:
        return false;
    }
}

/*
 * May the current privilege run this operation at all -- and, for
 * cbo.inval, as what.
 *
 * menvcfg says what S-mode and U-mode may do; senvcfg narrows that for
 * U-mode. Three separate enables: CBZE for cbo.zero, CBCFE for clean and
 * flush, and the two-bit CBIE for inval, whose middle value means "it
 * runs, but as a flush" -- a supervisor that does not trust its users
 * with an operation that discards data.
 *
 * **Both registers existed, were written and read back correctly, and
 * were consulted by nothing.** Every cache-block operation ran at every
 * privilege whatever they held. The mask that says which bits are
 * writable was the only reader the fields had.
 */
static bool cbo_permitted(const rv_hart_t *h, uint32_t *op)
{
    if (h->priv == RV_PRIV_M) {
        return true;
    }

    const uint32_t m = h->menvcfg;
#if RV_EXT_S
    const uint32_t s = (h->priv == RV_PRIV_U) ? h->senvcfg : m;
#else
    const uint32_t s = m;
#endif

    switch (*op) {
    case RV_CBO_OP_ZERO:
        return (m & s & ENVCFG_CBZE) != 0u;
    case RV_CBO_OP_CLEAN:
    case RV_CBO_OP_FLUSH:
        return (m & s & ENVCFG_CBCFE) != 0u;
    default: /* cbo.inval */
        if ((m & ENVCFG_CBIE) == 0u || (s & ENVCFG_CBIE) == 0u) {
            return false;
        }
        if ((m & ENVCFG_CBIE) == ENVCFG_CBIE_FLUSH ||
            (s & ENVCFG_CBIE) == ENVCFG_CBIE_FLUSH) {
            *op = RV_CBO_OP_FLUSH;
        }
        return true;
    }
}

rv_exc_t rv_hart_cbo(rv_hart_t *h, uint32_t op, uint32_t addr,
                     uint32_t *fault_addr)
{
    const uint32_t base = addr & ~(RV_CACHE_BLOCK_SIZE - 1u);

    *fault_addr = base;

    if (EMU_UNLIKELY(!cbo_permitted(h, &op))) {
        return RV_EXC_ILLEGAL_INSN;
    }

#if RV_EXT_ZICBOZ
    if (op == RV_CBO_OP_ZERO) {
        /*
         * Unlike the maintenance operations this has an architecturally
         * visible effect, so it is checked exactly as a store is -- and
         * as *one* store, of the whole block. It used to be a store per
         * word, each checked on its own, which zeroed the words below a
         * PMP boundary and then reported the one above it. The block
         * lies in one page, so one translation serves all of it.
         */
        uint32_t pa = base;

#if RV_EXT_SDTRIG
        if (EMU_UNLIKELY(h->trig_active)) {
            for (uint32_t i = 0; i < RV_CACHE_BLOCK_SIZE; i += 4u) {
                if (rv_trig_check(h, base + i, EMU_ACC_STORE)) {
                    *fault_addr = base + i;
                    return RV_EXC_BREAKPOINT;
                }
            }
        }
#endif
#if RV_EXT_SV32
        if (EMU_UNLIKELY(h->vm_active)) {
            const rv_exc_t texc =
                rv_mmu_translate(h, base, EMU_ACC_STORE, &pa);

            if (EMU_UNLIKELY(texc != RV_EXC_NONE)) {
                return texc;
            }
        }
#endif
#if RV_EXT_PMP
        if (EMU_UNLIKELY(h->pmp_active) &&
            !rv_pmp_check(h, pa, RV_CACHE_BLOCK_SIZE, EMU_ACC_STORE)) {
            return RV_EXC_STORE_ACCESS_FAULT;
        }
#endif
        const emu_region_t *const r = emu_bus_find(h->bus, pa);

        if (EMU_UNLIKELY(r == NULL || (r->perm & EMU_PERM_W) == 0u ||
                         (pa - r->base) + RV_CACHE_BLOCK_SIZE > r->size)) {
            return RV_EXC_STORE_ACCESS_FAULT;
        }

        for (uint32_t i = 0; i < RV_CACHE_BLOCK_SIZE; i += 4u) {
#if RV_EXT_A
            if (h->resv_valid && ((pa + i) & ~3u) == h->resv_addr) {
                h->resv_valid = false;
            }
#endif
            const emu_fault_t f = emu_bus_write(h->bus, pa + i, 4u, 0u);

            if (EMU_UNLIKELY(f != EMU_FAULT_NONE)) {
                *fault_addr = base + i;
                return rv_exc_from_fault(f);
            }
        }
        return RV_EXC_NONE;
    }
#endif

#if RV_EXT_ZICBOM
    {
        /*
         * **The block is named by a virtual address and protected like
         * any other memory.** This went from the guest's address
         * straight to the bus: no translation, so under Sv32 it looked
         * up a virtual address as a physical one and either faulted on
         * a page that was mapped or maintained memory that was not the
         * guest's; and no PMP, so a block a lower privilege could
         * neither read nor write was cleaned without complaint.
         * rv_hart_amo records the same omission, with the same rule:
         * anything in this file that reaches the bus owes the address a
         * translation and a PMP check first.
         *
         * The permission is the architecture's and is unusual: allowed
         * if a load *or* a store would be, and reported as a store
         * fault when neither is, whichever kind of fault it was.
         */
        uint32_t pa = base;

#if RV_EXT_SV32
        if (EMU_UNLIKELY(h->vm_active)) {
            const rv_exc_t texc = rv_mmu_translate(h, base, EMU_ACC_LOAD, &pa);

            if (EMU_UNLIKELY(texc != RV_EXC_NONE)) {
                return (texc == RV_EXC_LOAD_PAGE_FAULT)
                           ? RV_EXC_STORE_PAGE_FAULT
                           : RV_EXC_STORE_ACCESS_FAULT;
            }
        }
#endif
#if RV_EXT_PMP
        if (EMU_UNLIKELY(h->pmp_active) &&
            !rv_pmp_check(h, pa, RV_CACHE_BLOCK_SIZE, EMU_ACC_LOAD) &&
            !rv_pmp_check(h, pa, RV_CACHE_BLOCK_SIZE, EMU_ACC_STORE)) {
            return RV_EXC_STORE_ACCESS_FAULT;
        }
#endif
        if (EMU_UNLIKELY(emu_bus_find(h->bus, pa) == NULL)) {
            return RV_EXC_STORE_ACCESS_FAULT; /* nothing is there at all */
        }

        /*
         * Maintenance applies to the host memory backing the block, so a
         * guest cleaning a DMA buffer cleans the ARM cache lines that
         * really hold it. A virtual device has no backing and nothing
         * to maintain; the operation retires.
         */
        void *host = emu_bus_host_ptr(h->bus, pa, RV_CACHE_BLOCK_SIZE);

        if (host != NULL && h->cache != NULL && h->cache->maint != NULL) {
            static const emu_cache_op_t map[3] = {
                EMU_CACHE_INVAL, EMU_CACHE_CLEAN, EMU_CACHE_FLUSH};
            h->cache->maint(h->cache->ctx, host, RV_CACHE_BLOCK_SIZE, map[op]);
        }
        /* No cache maintenance configured: retiring without doing anything
         * is architecturally legal. */
        return RV_EXC_NONE;
    }
#else
    return RV_EXC_ILLEGAL_INSN;
#endif
}

#endif /* RV_EXT_ZICBOM || RV_EXT_ZICBOZ */

/* A store under paging or PMP: see load_guarded. */
EMU_NOINLINE static rv_exc_t store_guarded(rv_hart_t *h, uint32_t addr,
                                           uint32_t size, uint32_t val)
{
#if RV_HAVE_DTLB
    const uint32_t va = addr;
    /*
     * Not while a reservation is outstanding: whether this store breaks
     * it is a question about the *physical* address, which the entry
     * does not keep. The JIT's inlined store declines on the same byte.
     */
#if RV_EXT_A
    const emu_dtlb_entry_t *const e =
        h->resv_valid ? NULL : dtlb_probe(h, va, size, true);
#else
    const emu_dtlb_entry_t *const e = dtlb_probe(h, va, size, true);
#endif

    if (e != NULL) {
        uint8_t *const p = (uint8_t *)(va + e->addend);

        if (size == 1u) {
            *p = (uint8_t)val;
        } else if (size == 2u) {
            const uint16_t w = (uint16_t)val;

            memcpy(p, &w, 2u);
        } else {
            memcpy(p, &val, 4u);
        }
        return RV_EXC_NONE;
    }
#endif
#if RV_EXT_SV32
    if (h->vm_active) {
#if RV_MISALIGNED_OK
        /* Across a page boundary: see load_guarded, and mem_wide for
         * why this one matters more -- a store has something to leave
         * half done. */
        if (EMU_UNLIKELY((addr & 0xFFFu) + size > 0x1000u)) {
            return store_split(h, addr, size, val);
        }
#endif
        const rv_exc_t texc = rv_mmu_translate(h, addr, EMU_ACC_STORE, &addr);
        if (EMU_UNLIKELY(texc != RV_EXC_NONE)) {
            return texc;
        }
    }
#endif
#if RV_EXT_PMP
    if (h->pmp_active && !rv_pmp_check(h, addr, size, EMU_ACC_STORE)) {
        return RV_EXC_STORE_ACCESS_FAULT;
    }
#endif

#if RV_EXT_A
    if (EMU_UNLIKELY(h->resv_valid && (addr & ~3u) == h->resv_addr)) {
        h->resv_valid = false; /* as rv_hart_store does */
    }
#endif

    const emu_fault_t f = emu_bus_write(h->bus, addr, size, val);
    if (EMU_UNLIKELY(f != EMU_FAULT_NONE)) {
        return rv_exc_from_fault(f);
    }
#if RV_HAVE_DTLB
    rv_dtlb_fill(h, va, addr);
#endif
    return RV_EXC_NONE;
}

rv_exc_t rv_hart_store(rv_hart_t *h, uint32_t addr, uint32_t size, uint32_t val)
{
#if !RV_MISALIGNED_OK
    if (EMU_UNLIKELY((addr & (size - 1u)) != 0u)) {
        return RV_EXC_STORE_MISALIGNED;
    }
#endif

#if RV_EXT_SDTRIG
    if (EMU_UNLIKELY(h->trig_active) && rv_trig_check(h, addr, EMU_ACC_STORE)) {
        return RV_EXC_BREAKPOINT;
    }
#endif
    if (EMU_UNLIKELY(data_guarded(h))) {
        return store_guarded(h, addr, size, val);
    }

#if RV_EXT_A
    /*
     * Any store to the reserved word breaks the reservation. A single hart
     * cannot race with itself, but its own stores (and a trap handler's)
     * must still invalidate an outstanding LR.
     */
    if (EMU_UNLIKELY(h->resv_valid && (addr & ~3u) == h->resv_addr)) {
        h->resv_valid = false;
    }
#endif

    return rv_exc_from_fault(emu_bus_write(h->bus, addr, size, val));
}
