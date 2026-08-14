# Open work

Moved out of README.md. Status lines here are claims about the code, so
they go stale: `scripts/report-figures.sh` regenerates the ones a host can
check, and anything measured on hardware carries the commit it was
measured at.

## Roadmap

Ordered by what would most repay the effort.

**Performance.** The JIT is 19.2× native at a 48 KB code cache and the remaining cost is structural
rather than a missing optimisation:

- [x] **Chain across loop back edges** — *done*. A backward branch to the block's
      own start now branches within the translated code instead of returning
      to the dispatcher. `bench` went from 158,025 block entries to 40,285 and
      **28.66 → 18.68 cycles per guest instruction, 35%**; CoreMark 33.35 →
      31.04, 7%. `RV_JIT_LOOP_CAP` bounds interrupt latency at 64 guest
      instructions, since delivery happens between blocks.

      That bound was not real until later: the cap compared the wrong
      register (see *A silently malformed compare*, below), so chained loops
      ran to completion in a single block entry. Fixing it cost tight loops
      about 10% and cost `bench` 1% -- 18.68 to 18.88 cycles per guest
      instruction -- which is what bounded interrupt latency actually costs.

      With the bound real, the cap was tuned. `RV_JIT_LOOP_CAP` is now
      **128**; see *Tuning the loop cap* below.

      Getting there took three wrong versions, all of which *ran correctly*
      and miscounted retired instructions — which matters, because that count
      feeds `mcycle`, `minstret` and the run budget, and because dividing host
      cycles by an inflated count once produced an apparent 2.75× win that was
      pure artefact. What fixed it: chain only to the block start, so one
      constant is right on the first pass and every iteration; put the
      accumulation *before* the conditional split, so both paths account for
      the same instructions; and have each exit add only what it retired since
      that point. **The instruction count is the first thing to check when a
      JIT change looks too good.**

- [x] **Fewer helper calls for memory** — *done*. The passthrough window is
      inlined alongside guest RAM, worth **2.2-3.1x** on driver-shaped access
      (see *Driver performance* above). Two things had to be true first: the
      window is an identity map, so the emitted access is a bare load from the
      address register; and its read-only sub-ranges are few and small enough
      to test as holes punched out of one range check.

      It is armed by the guest rather than always emitted, because the code is
      not free -- about 18 bytes per load and 48 per store, which grew
      CoreMark's translated image past the 48 KB cache and cost it 53%. What
      is left here is a *third* window for the guest ROM, which would only
      matter for an execute-in-place guest; the images built here run from
      RAM, so it would be unmeasurable.

**ISA.** What is left is either small or deliberately excluded:

- [x] **`FCVT.W.S` / `FCVT.WU.S` in the JIT** — *done*. `VCVTR` plus a
      compare-against-self for the NaN case, seven instructions; 22 per
      `isatest` run move off the helper. Covered by 25 new self-test checks
      spanning NaN of both signs, both infinities, over- and under-range,
      every rounding mode the translation claims, dynamic rounding, and the
      exception flags — the one pre-existing check (`10.0` with `rtz`) would
      have passed with the NaN fixup deleted entirely.
- [x] **`FMIN`/`FMAX`, `FCLASS` in the JIT** — *done*, by routing them to a
      helper rather than open-coding them. No ARMv7-M equivalent exists, and
      hand-rolling RISC-V's NaN, signalling-NaN and signed-zero rules would
      cost 25–35 emitted instructions each in the resource that turned out to
      set JIT performance overall. The win was never avoiding the call: it is
      that these no longer end the block. 146 interpreted instructions and
      755 block entries on the self-test become 122 and 732.

      Covered by 19 new checks pinning the cases an inline version would get
      wrong — two NaNs giving the canonical NaN rather than either input,
      `-0.0` ranking below `+0.0` for both operations in both operand orders,
      quiet against signalling NaN for the invalid flag, and every one of
      `FCLASS`'s ten bits.
- [x] **`RMM` in the JIT** — *resolved*, by declining it correctly rather
      than by translating it. No ARMv7-M rounding mode expresses ties-away,
      so it stays on the helper; what changed is that it now reliably gets
      there.

      Blocks are **specialised on `frm`**. A `dyn` instruction is resolved at
      translation against the `frm` then in force, so `dyn` under `frm=RMM`
      is declined exactly as a static `rmm` always was. Previously the mode
      was resolved at run time through a packed table whose `RMM` entry was
      `RN`, so such a guest got ties-to-even where it asked for ties-away —
      silently, and only under the JIT.

      The flush that makes specialisation safe costs nothing on the hot
      path. `frm` moves only on a CSR write, the translator declines
      `SYSTEM` entirely, so every write to it lands on the interpreter
      fallback — and that is the only place the check runs. It is skipped
      altogether unless some cached block actually resolved a `dyn`, so a
      guest with no FP never flushes. Specialising also deletes the
      ten-instruction table lookup from the front of every dynamically
      rounded FP operation.

      Caught by six new self-test checks that execute one `fcvt.w.s ..., dyn`
      at one address under five modes in turn: two fail without the fix
      (`2.5` under `RMM` gives 2, not 3), and the rest fail if a block is not
      rebuilt when `frm` changes. The tie value matters — `3.5` rounds to 4
      under both modes and would have passed throughout.
- [x] **APLIC** — *implemented*, direct delivery mode, one domain, one hart,
      at `0x0C00_0000`. Written against the AIA specification 20250312 in
      `docs/riscv/`. `domaincfg`, `sourcecfg`, the pending and enable
      bitmaps with their `*num` forms, `target` priorities, and an IDC with
      `idelivery`/`iforce`/`ithreshold`/`topi`/`claimi`. 24 self-test checks
      drive it through `setipnum`, so they run on the host as well as the
      board. MSI delivery is not implemented and will not be: it targets an
      IMSIC, which needs S-mode CSRs this core does not have.

      **The NVIC bridge is the part that matters** and is what makes an
      interrupt from real silicon reachable by a guest driver. An interrupt
      is the one thing the passthrough window cannot carry: the NVIC vectors
      into the emulator, with the guest nowhere in sight. The handshake is
      forced by the fact that nothing on the host side can service the
      device — only the guest's driver can — so the line is masked on entry
      and unmasked only when the guest clears the APLIC pending bit. One
      table entry and one handler adds a peripheral, the way `g_periph_map`
      works for addresses. TIM6 is wired as the first line, and the
      `irqtest` guest drives it end to end on the board: **100 real timer
      interrupts taken by guest code**, `mcause 0x8000000b`, with the guest
      clearing TIM6's own flag through the passthrough window. The count is
      the assertion — one interrupt would only prove the line was unmasked
      at reset, whereas every further one requires the mask-and-unmask
      handshake to have completed. A bridge that masked and never unmasked
      delivers exactly one.
- [x] **ACLINT** — *implemented*. MSWI and MTIMER are separate bus devices
      sharing one set of state, mapped at `0x0200_0000` and `0x0200_4000` —
      exactly the window the legacy CLINT occupied. The split is a renaming
      rather than a redesign: the de-facto SiFive layout already *is* those
      two devices at fixed relative offsets, `mtime` at `0x4000 + 0x7FF8`
      being the same `0x0200_BFF8` it always was. Guests written for either
      layout work unchanged, which the self-test's timer checks demonstrate
      by passing untouched. What ACLINT adds, and what this gains, is that a
      platform may place the two devices independently.
- [x] **U-mode** — implemented and exercised by the suite's `PMPU` tests,
      all 10 of the 10 this core qualifies for. `MPP` is real on trap entry
      and `MRET`, `ECALL` from U has its own cause, `MPRV` borrows `MPP`'s
      privilege for data accesses, and `misa.U` is set.

      The gate that had to come first was: **inlining a memory access is
      only sound while nothing can deny it**, which is true in M-mode with no
      locked PMP entry and false below M, where matching no entry denies. So
      `pmp_active` depends on the privilege level rather than only on the
      entry configuration — in the *core*, because the interpreter skips
      `rv_pmp_check` on the same flag and had the identical hole.

      Running the privileged tests then found three defects that no M-mode
      guest could reach, described under *Things that have bitten* below.
      Execute permission needed a check in *both* backends and for
      different reasons: the interpreter checks it per fetch, while the JIT
      reads the guest's instruction bytes at translate time and emits
      nothing at all for a fetch, so it has to refuse to translate what PMP
      will not let the guest run. `pmpx-exec-noeffect` in `isatest` is what
      tells those two apart — with the translator's check reverted, the
      store at the top of a no-execute region runs on hardware.
- [x] **S-mode and Sv32** — a third privilege level, the supervisor CSR
      bank, trap delegation, `SRET`, the TVM/TW/TSR traps, and two-level
      Sv32 paging with 4 MiB megapages behind a 32-entry direct-mapped TLB.
      The host has no MMU of its own, so all of this is soft — which is the
      point: the guest gets a page table the ARM part cannot provide.

      A and D are checked and never written (Svade), because writing them
      means an atomic read-modify-write of guest memory from inside the
      walk, for a hart with nothing to race against. Software sets them in
      its fault handler.

      The cost is gated the way PMP is. `vm_active` is false whenever satp
      is Bare or every relevant privilege is M, so a guest that never
      enables paging pays one predictable branch, folded into the same
      `fetch_guard` that already covered PMP and Sdtrig.
- [ ] **V** — the largest remaining item and RAM-hungry: `VLEN=128` alone costs
      512 B of register file on a part with 128 KiB. Needs a `VLEN` budget
      decision before any code.
- [ ] **D**, and therefore **Zcd** — *not planned*. The Cortex-M4F and M7 FPUs are
  single precision, so D would be entirely soft-float on the intended targets,
  and Zcd is the compressed double load/stores it would need.

**Measured and rejected**, kept here so they are not retried blind:

| Idea | Result |
|---|---|
| Interpreter loop in SRAM | **slower** — 162 vs 122 cycles, and 8 KiB off the guest |
| Guest registers cached in `r8`–`r10` | **15.5% slower** — a cached read is `MOV` where an uncached one is `LDR`, one instruction either way |
| PMP mapped onto the ARM MPU | **not possible** — the MPU cannot distinguish a guest access from an emulator access, because the JIT's inlined load *is* both |

---
