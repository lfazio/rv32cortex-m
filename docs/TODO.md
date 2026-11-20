# Open work

Only what is still to do. Finished work is described in the document it
changed -- a frontend's in [frontend/](frontend/), the JIT's in
[jit/](jit/README.md), the scheduler's in [os.md](os.md), Linux's in
[linux.md](linux.md), the games' in [games.md](games.md), the N6 port's
in [stm32n6/](stm32n6/README.md) -- and what was learned the hard way
is in CLAUDE.md, as rules, and [lessons.md](lessons.md) at length.
Status lines here are claims about the code, so they go stale: `scripts/report-figures.sh` regenerates the ones a host can
check, and anything measured on hardware carries the commit it was
measured at.

## Needs a board

- [ ] **Board figures that predate `emu_cycles.h`.** Any of them from a
      run longer than 19.9 s at 216 MHz (23.9 s on the F446) is a whole
      wrap short, and the documents do not record run lengths. The ones
      in `jit/tuning.md`, `backend/thumb2.md`, `frontend/ppc.md` and the
      first section of `performance.md` were measured again at ce02bd0. The second half of
      `performance.md` and both `stm32f446/` pages are not, and are
      marked as history.

## Testing

- [ ] **Fuzz testing.** The first target exists for every frontend
      and has paid: `fuzz-<isa>-jit`, the interpreter against the
      x86-64 JIT, and `fuzz-ir` for the IR alone ([fuzzing.md](fuzzing.md)):
      eighteen defects, eight of them in an interpreter, one a whole
      class of ARMv7-M encodings the board then answered. The rest of this item is still open. Before
      it, the tree had generation, not fuzzing:
      `tests/armv7m-diff/rawgen.py` (raw encodings against the F746),
      the property tests over whole encoding spaces (ARMv7-M's decoder
      over 402,712,576 encodings, G4MH's length over 65,536 halfwords),
      `tests/ppc-check/` against binutils and a Python model, the
      Spike/Sail probes, and `ir_diff_ref`. None is coverage-guided, and
      none reaches what a guest controls beyond the instruction word.
      clang 21 links `-fsanitize=fuzzer,address` on the host, so each of
      these is a libFuzzer target behind an option, smallest oracle
      first:
  - **Instruction streams, interpreter against JIT**, per frontend:
    the same bytes run both ways and compared on the register file,
    memory, trap state and the counters. **Done for all four**
    (`tests/fuzz/fuzz_harness.h` and a target each). What is left:
    Spike as a third side for RV32 (BSD-3, CLAUDE.md) -- **set aside
    until late in the project**, by decision (2026-10-26); timers, and
    whether an ISB or an
    interrupt raised by a store is honoured *in time* -- both set aside
    today ([fuzzing.md](fuzzing.md#devices-and-interrupts)); and
    self-modifying code on RV32, G4MH and PowerPC, which architecturally
    needs a sync instruction (FENCE.I, SYNCI, `isync`) between the store
    and the fetch, so a random program would compare only where it
    happened to write one -- ARMv7-M's, which the frontend detects
    without one, is fuzzed (`CFG_SMC`: two defects). Interrupt
    controllers are mapped for every frontend, and
    G4MH and PowerPC run two cores ([fuzzing.md](fuzzing.md#more-than-one-core)).
    No GPL emulator, not even as a reference.
  - **The IR**: done for the passes and the x86-64 lowering
    (`fuzz-ir`), and for the Thumb-2 lowering on the inputs that corpus
    holds, replayed on the F746 (`scripts/ir-board.sh`: 3,263 compared,
    none different, [fuzzing.md](fuzzing.md#the-ir-corpus-on-the-board)).
    Not reached: what Thumb-2 alone refuses -- 305 of those inputs --
    and inputs a fuzzer guided by the Thumb-2 lowering would build,
    which only fuzzing on a board could; `pass_mac`,
    which forms a multiply-accumulate only where the backend lowers
    one, and x86-64 does not; and the FP exception flags, which the
    reference does not compute.
  - **Translation and protection**: done for RV32 -- Sv32, PMP and
    the data TLB against a walk written from the spec
    (`fuzz-rv32-mmu`), which found a PPN past the bus read as an alias
    and an M-mode access half in an unlocked PMP entry let through.
    G4MH's MPU (`fuzz-g4mh-mpu`, from the U2B manual: seven defects,
    all found writing the reference), ARMv7-M's (`fuzz-armv7m-mpu`,
    from the ARM ARM and the F746: none), PowerPC's TLB with the JIT's
    blocks kept across TLB, MSR and PID changes (`fuzz-ppc-tlb`: none),
    and RV32's fetch side and the JIT's inline data TLB probe, code
    running in S and U (`fuzz-rv32-vm`: none). Not reached by any of
    them: PowerPC VLE pages, `lwarx`/`stwcx.`, `dcbz` and the cache
    instructions under the MMU, and a TLB change on one core retiring
    another's blocks; RV32 compressed code and anything but loads and
    stores, which `fuzz-rv32-vm` compares between the backends only;
    G4MH's MPUs on a second core -- fuzz-g4mh-mpu discards every block
    before each step, because each step rewrites its code, so a second
    core's MPU against blocks the first built needs a harness that
    keeps them -- and ARMv7-M's exception
    frames, whose stack `fuzz-armv7m-mpu` keeps open to every access --
    the board's answers on stacking faults (`stk-*`, `unstk-*`) are
    held by `test_stacking_board_answers` instead.
  - **What arrives from outside the guest**: done for the ELF loader,
    the gdb stub, the upload paths and the virtio rings (`fuzz-elf`,
    `fuzz-gdb`, `fuzz-upload`, `fuzz-virtio`: sixteen defects,
    [fuzzing.md](fuzzing.md#what-arrives-from-outside-the-guest)), and
    the telnet console (`fuzz-telnet`: one). Not reached: TFTP's packet
    parsing and SLIP's framing, both lwIP's own code and not this
    tree's, and virtio-9p, whose fuzzing would be the host's
    filesystem's -- a scratch directory in a mount namespace would make
    it safe to try.
  - **ARMv7-M on the board**: done for fuzz-armv7m-jit's corpus
    (`run.py --suite raw --from-fuzz ... --resume`: 4,000 encodings,
    two differing, [fuzzing.md](fuzzing.md#the-corpus-on-the-board)).
    Not reached: the corpus's sequences -- each encoding is asked alone,
    in the harness's state, not in the program and configuration that
    reached it.

      Every target needs a mutant it finds before it counts, as every
      test here does: a fuzzer that runs for an hour and reports nothing
      can mean the code is right or that the target never reaches it.

## JIT

- [ ] **The flag ISAs' flags on Thumb-2**, what is left
      ([backend/thumb2.md](backend/thumb2.md#then-the-flags)): G4MH's
      branches compare a subtract's operands now and PowerPC's a
      compare's, and PowerPC's CR field is one three-way compare
      (`EMU_IR_CMP3`). ARMv7-M's flags are one APSR-layout word now,
      written by SETF with ARM's carry (`flags_arm_carry`) -- one store
      where there were four; pairing the four into STRDs had made its
      code 2.9% smaller and CoreMark 0.97% slower
      (`wip/t2-strd-flag-stores`). Profiled again
      after the changes that took 16-30% off these three (ARMv7-M 13.40
      to 11.31, PowerPC 17.69 to 12.46, G4MH 18.74 to 14.30): ARMv7-M's
      stores to the cpu are 17.2% of its translated time, PowerPC's
      arithmetic 38%, G4MH's 28.5%
      ([backend/thumb2.md](backend/thumb2.md#the-profile-again-after-all-of-it)).
      By mnemonic, 2026-11-20
      ([backend/thumb2.md](backend/thumb2.md#by-mnemonic-powerpc-and-g4mh)):
      G4MH's SETF -- MRS, two UBFX/BFI pairs, the PSW read and written
      -- is about 27% of its translated time, and 87% of the SETFs left
      after the dead-flag pass are there for the block's exit, not for a
      load that might fault; only flags kept as operands and derived when
      read would remove them. PowerPC's `cmpw` was 13 instructions, 4 of
      them the CR's read-modify-write for one field -- a word a field
      now, CoreMark 6.1-6.3% faster -- and 3 the summary overflow; its
      byte swaps are about 10%, which only RAM kept in the host's order
      for words (an address XOR for halves and bytes) would drop.
- [ ] **The peripheral window for PowerPC**, only for a little-endian
      window it does not have. It is inlined on Thumb-2 for RV32
      (2.5-7x on driver code,
      [performance.md](performance.md#driver-performance-the-passthrough-window)),
      ARMv7-M (5.5-13x) and G4MH (2.5-4.2x,
      [frontend/g4mh.md](frontend/g4mh.md#the-peripheral-window)).
- [ ] **Autovectorisation, what is left**
      ([jit/autovectorisation.md](jit/autovectorisation.md)): strcpy's
      kernel is on (Dhrystone on the F746 19-20% faster for RV32, 15%
      for ARMv7-M; off on the F446, whose guest RAM it costs), and the
      dot product, emitted in the block on Thumb-2 (RV32 CoreMark 0.89%
      faster), and the mapped product (`matrix_mul_matrix_bitextract`,
      0.45% more). Left: Helium's `VMLADAVA` on the
      N6; PowerPC's dot products, byte-swapped, which the emitted loop
      declines; ARMv7-M's, whose sticky Q a kernel would compute per
      step. The copy and the fill stay off.
- [ ] **PowerPC's interpreter 0.6-1.0% slower with CR as fields**
      ([frontend/ppc.md](frontend/ppc.md#the-jit)): a `bc` now finds its
      field (`bi >> 2`) and shift (`3 - (bi & 3)`) at every execution;
      decoded once, into the decode cache's entry, it would be a load
      and a shift as before. The JIT is 6.2-6.4% faster with them.
- [ ] **The thrash policy's two equilibria**
      ([jit/tuning.md](jit/tuning.md#a-long-run-at-32-kb-two-equilibria)):
      RV32 CoreMark x2000 at 32 KB on the F746 runs at 16 host cycles an
      instruction for its first 163M instructions and at 28-31 for the
      next 290M, with the same work in every iteration -- the marks a
      retry rebuilds land in a worse set and stay there. Backing off the
      retry, forgetting a quarter at a time and never forgetting each
      won one regime and lost another on the host
      (`wip/jit-thrash-retry-variants`). Untried: keep the marks a retry
      replaced, and put them back when the next window compacts more.
- [ ] **Why the dispatcher's linking fix alone is slower under
      pressure.** Linking a later block's exit made all four CoreMarks
      3.6-8.0% faster at their default buffers, but without the stop
      before a known entry it cost RV32 CoreMark x650 12.5% at 32 KB,
      G4MH's 5.7% and RV32 Whetstone 18%: 894 links against 548, twice
      the evictions and twice the pcs given up on. Every frontend stops
      now, which more than wins it back, so nothing ships in that state
      -- but whatever retention does wrong with more chained blocks may
      be costing the stop's figures too
      ([backend/thumb2.md](backend/thumb2.md#what-a-block-shares-with-the-blocks-already-there)).
- [ ] **DBGMCU_CR.DBG_SLEEP on the F446.** Every F746 firmware sets
      it, and the N6 needs none -- its counter runs through a WFI and the
      probe reaches it without (`scripts/check-wfi-attach.sh TREE
      stm32n6`, [stm32n6/README.md](stm32n6/README.md)). The F446 is not
      attached. And with the counter running in Sleep, the park loop
      could sleep with lwIP up on the F746 too (`board_idle`), untried
      over SLIP.
- [ ] **At 8 KB the JIT is still slower than the interpreter** on
      Whetstone: 8.67 s against the interpreter's 6.23, where 16 KB is
      ahead now (5.99) -- the hot threshold at 8, armed at the first
      compaction, and the stretch a cold pc runs remembered
      ([jit/tuning.md](jit/tuning.md#emu_jit_hot_threshold--translating-only-hot-code)).
      What is left at 8 KB is mostly interpreted (8.6M of 10.7M
      instructions); the one setting that helped there, never
      forgetting the thrash marks (8.07 s), cost 16 KB 20%.
- [ ] **Why an ITCM move costs ARMv7-M's JIT build 1.4-1.9%**: the
      six helpers and `rd_u`/`wr_u` each did, which is why the last are
      placed only where there is no JIT
      ([frontend/armv7m.md](frontend/armv7m.md#what-the-board-corrected)).
      The interpreter itself is below 375f9bf's 143.44 now (135.8),
      from the run loop's per-instruction work, not a decode cache.
      What the profile leaves: the loop's state, pending and Thumb-bit
      tests, and the fetch's MPU and execute-never test.
- [ ] **PowerPC's FP unit is a call per instruction**: all four
      operations that are most of an FP program compute their common
      case inline in the helper the block calls, and Whetstone under the
      JIT is 59.3 to 35.4 host cycles an instruction for them on the F746
      ([frontend/ppc.md](frontend/ppc.md#what-uses-it-and-what-it-costs)).
      What is left is the block's call into the helper, and beside it
      the dispatcher -- 12.9% of Whetstone's profile, where function
      returns could not chain; most go through the computed jumps' stub
      now, and blocks that ran out of chain slots or stopped at their
      length cap chain too (block entries 425,461 to 81,348, Whetstone
      35.01 to 29.24 host cycles an instruction,
      [jit/README.md](jit/README.md#chaining-compaction-and-committed-code)).
      Discarded: the
      arithmetic in the block, 19% slower on Thumb-2 for the temps it
      spent (`wip/ppc-efsmul-in-block`), and -- measured again without
      the temps, 2026-11-17 -- for its bytes: the call is about 22
      cycles (11% of Whetstone at most), and 80 bytes more at each FP
      site cost 20.7%, the buffer no longer fitting the DTCM
      ([frontend/ppc.md](frontend/ppc.md#what-uses-it-and-what-it-costs)).
      And one call for an adjacent pair, measured 2026-11-18: 11%
      slower on the F746, the dispatch on the operation in the helper
      costing more than the call it saved (`wip/ppc-efs2-pair`). The
      call stays one per instruction.

## Linux

From the last measurements in [linux.md](linux.md).

- [ ] **FENCE.I flushes the whole cache**, and measured, it is not
      worth making local yet ([linux.md](linux.md#fencei-what-it-costs-and-where-it-comes-from)):
      at most 0.3 s of a 3.15 s initramfs boot, and the flushes are the
      kernel patching its own, already translated, text -- so it would
      take invalidation by physical page and every write path tracked,
      DMA and inlined M-mode stores included, for that.
- [ ] **Every process translates its own copy of libc.** The key is
      satp, so identical physical pages in two address spaces are two
      sets of blocks. Keying on the physical page would share them, at
      the cost of a translation on every lookup.
- [ ] **A display under Linux.** The framebuffer device exists
      ([games.md](games.md)); nothing in the device tree describes it to
      a kernel.

## Platforms

- [ ] **A tinyusb network device over USB**
      (https://github.com/hathach/tinyusb). What it was waiting for is
      done: lwIP runs on a FreeRTOS task of its own, given a turn by the
      run loop when it has work
      ([network.md](network.md#on-a-task-of-its-own)). A USB device's
      events would wake that task, which today only the run loop does.
- [ ] **PCIe over USB**: the design of an emulated PCIe peripheral
      reached over USB, and of a PCIe-over-USB device firmware deployed
      on each board to exercise the peripherals it has. The use case is:
      develop firmware on pc while using real peripherals on boards.

## Guests

- [ ] **ARMv7-M**, from [frontend/armv7m.md](frontend/armv7m.md#not-done):
      the whole F746 firmware as a guest (it needs an STM32F746 machine
      model, which is a direction change), the IR and register allocator
      executed in a guest, the JIT's remaining items, and the board's
      answers kept as fixtures.

## RV32

- [ ] **A vector instruction after a declined one is the helper's.**
      The translator lowers a vector instruction only in a block that
      set vtype itself; a declined instruction -- a CSR write, which
      `fsrm` before vector FP code and `csrw vxrm` before fixed-point
      code are -- is interpreted in a batch that runs on past the
      `vsetvli` after it, and the block that then starts mid-sequence
      sends every vector instruction to the helper (`fpedge-v` met it,
      [frontend/rv32-vector.md](frontend/rv32-vector.md#single-precision-under-the-jit)).
      So does a loop whose `vsetvli` a compiler hoisted out of it. A
      guard at the block's first vector instruction -- vtype read and
      compared with the translation's, a side exit where they differ --
      would let any block assume it; count how many vector instructions
      reach the helper this way in real code first. Counted, 2026-11-18,
      with the guard written: the vector helper's calls under the JIT
      on coremark-v, vecbench, vfbench, vecops, vecfp, vecmask,
      vecround and fpedge-v were the same with and without it but for
      vecfp's 70 to 64, and host instructions rose by the guard's cost.
      Not kept.

- [ ] **The vector extension past Zve32f**: Zve32x and Zve32f are whole
      behind `-DRV32_EXT_ZVE32X=ON` and `-DRV32_EXT_ZVE32F=ON` and held
      to Spike by six probes; next Zve64x/f/d, then V and its `misa` bit
      ([frontend/rv32-vector.md](frontend/rv32-vector.md)). The UDB and
      Sail configurations wait for an arch-test pin with vector suites.
      Zve32f's arithmetic is lowered (`EMU_IR_VFOP`: SSE on x86-64, the
      fused forms and other modes by a call; VFP on Thumb-2 under RNE);
      min and max, compares, conversions, the FP reductions and `vfmv`
      are still the helper's under the JIT.
- [ ] **The vector unit's SIMD lowering, past its first set**: unmasked
      single-width integer operations lower after an inline vsetvli
      (`EMU_IR_VOP`), to SSE2/SSE4.1 on x86-64 and the DSP extension on
      Thumb-2, and unit-stride loads and stores that fill a group copy
      inline (`EMU_IR_VMEM`); the averages under rnu, the
      multiply-adds, the widening multiply-adds and the narrowing
      shifts and clips too, and the single-width forms masked, with
      `vmerge` (`EMU_IR_VOPM`), the reductions (`EMU_IR_VRED`) and
      unmasked strided accesses (`EMU_IR_VMEMS`), the averages and clips
      under every vxrm mode, every form masked, Thumb-2's 16-bit and
      unsigned widening multiply-adds. Next: .wv narrowing (a count a
      lane: AVX2's on x86-64, an extraction a lane on Thumb-2);
      Thumb-2's widening from bytes, narrowing to bytes and unsigned clip
      (registers; USAT reads signed). The N6's MVE (Helium) lowers the
      unmasked single-width operations and the whole-group copies
      (vecbench 18% fewer cycles,
      [backend/thumb2.md](backend/thumb2.md#helium-mve-on-the-n6)), and
      the reductions; next there, by vecbench's kernels measured alone
      ([frontend/rv32-vector.md](frontend/rv32-vector.md#on-the-n6-by-kernel)):
      the masked single-width operations (`VPSEL` on `mve_active`'s
      P0). Done there: the widening multiply-adds and narrowing shifts
      and clips (vecbench 19.83 to 18.72). Discarded: on the F746's DSP
      path, the skip of the tail blend that MVE took (vecbench 17.99 to
      17.98 and 17.99 at two layouts, 2.5 KB more code: a compare and a
      taken branch a word cost the M7 what two DTCM loads and three ALU
      operations do); the vector
      FP in MVE, which flushes subnormals whatever FPSCR says and was
      at most 5.6% of vfbench. Not yet: a loop over an m4 or m8 group's
      words on Thumb-2 -- vecbench runs the same at a 32 KB buffer as at
      64 ([backend/thumb2.md](backend/thumb2.md#the-guests-vector-registers-the-dsp-extension))
      ([frontend/rv32-vector.md](frontend/rv32-vector.md#the-two-backends)).
- [ ] **SM4's affine maps with GFNI.** `sm4ed` and `sm4ks` are native
      under the x86-64 JIT now, AES's S-box between two maps of two
      `pshufb` each ([backend/x86_64.md](backend/x86_64.md#aes-ni-and-pclmulqdq));
      `gf2p8affineqb` would do each map in one instruction, but no CPU
      here has GFNI to test it on.
- [ ] **An emulated GTM** for the STM32F746ZG and the STM32N657, plugged
      into the PCIe framework.
- [ ] Propose a plan to extend rv32 to handle rv64, and implement it. The plan should include a way to
      handle the 64-bit registers in the IR and the JIT, and a way to
      handle the 64-bit memory accesses in the frontend and the JIT.
- [ ] Implement the profiles: RVI20, RVA20 (in riscv-spec.pdf)

## PowerPC

- [ ] Implement vectorial extensions
- [ ] **Anything to compare it with.** No e200 is attached and no
      other emulator is consulted, so a shared misreading of the manual
      is invisible to every check there is. The only second document,
      the MPC5675K reference manual, agrees on the MMU except where it
      describes a different core: its e200z7 has 64 TLB entries and
      SPE, this frontend's e200z759n3 32 and EFPU2
      ([frontend/ppc.md](frontend/ppc.md#the-mmu)). Whether to model the
      MPC5675K's core, whose peripherals are already here, is open.
- [ ] Use CTU and FlexPWM to implement the MPC5675K's GTM equivalent, and plug it into the PCIe framework.

## armvX-m

- [ ] Implement neon extensions if available
- [ ] **ARMv6-M in the ARMv7-M frontend**, which becomes armvX-m.
- [ ] **ARMv8-M in the armvX-m frontend**, with the N657's own map as
      ARMv7-M has the F746's (`emu_cpu_ops_t::guest_map`,
      [memory.md](memory.md#armv7-m)): its TCMs, its flash, its SRAM.

## m68k

- [ ] Documents: @docs/motorola
- [ ] https://fr.wikipedia.org/wiki/Atari_ST
- [ ] https://fr.wikipedia.org/wiki/Amiga_3000
- [ ] https://fr.wikipedia.org/wiki/Motorola_68881
- [ ] **The 680x0 frontend**, which is the only one that has not been
      started. The first step is to read the manuals and write a
      description of what the guest sees, and what the host must do to
      make it happen. The second step is to implement the interpreter,
      which is the only part that can be tested without a board. The
      third step is to implement the JIT, which is the only part that
      can be tested with a board. The fourth step is to implement the
      MMU, which is the only part that can be tested with a board. The
      fifth step is to implement the peripherals, which are the only
      parts that can be tested with a board.
- [ ] Emulate an atari 1040STE, with its 68000 CPU, 68881 FPU, and 8520 CIA chips. The emulator should be able to run TOS and GEM applications, and should support the following features:
  - [ ] Memory mapping: The emulator should map the Atari's memory layout, including RAM, ROM, and I/O registers.
  - [ ] CPU emulation: The emulator should accurately emulate the 68000 CPU, including its instruction set, addressing modes, and exception handling.
  - [ ] FPU emulation: The emulator should accurately emulate the 68881 FPU, including its instruction set and exception handling.
  - [ ] CIA emulation: The emulator should accurately emulate the 8520 CIA chips, including their timers, interrupts, and I/O ports.
  - [ ] Video emulation: The emulator should accurately emulate the Atari's video hardware, including its display modes, color palette, and raster interrupts.
  - [ ] Sound emulation: The emulator should accurately emulate the Atari's sound hardware, including its audio channels and sound effects.

## RH850 / G4MH

- [ ] **Simple drivers for the RH850/U2B6**, from its reference manual:
  - [ ] Option bytes, and their effect on the clocks and start-up. They
        sit in flash at a defined address.
  - [ ] The clock tree.
  - [x] LTSC (section 44) -- modelled in `g4mh_ltsc.c`, driven by the
        runner's `set_time`; DOOM's clock runs off it.
  - [x] OSTM (section 34) -- `g4mh_ostm.c`, the U2B6's three units at
        their own addresses and channels, replacing the stand-in.
  - [ ] GPIO/ON (section 2), emulated on top of the STM32 GPIO HAL, the
        state of each output sent over a UDP/IP connection.
  - [x] KCRC (section 57) -- `g4mh_kcrc.c`, the four units, held to
        every row of the manual's setting table.
  - [x] WDTB (section 33) -- `g4mh_wdtb.c`, one per PE, with the
        ECM's reset configuration deciding what an error does.
  - [ ] The ECM (section 56), so a guest can configure what each error
        does and a WDTB error can reach FENMI. FENC's external NMI has
        a source now, `--nmi-at` and the F746's button.
- [ ] **TAUD on the RH850/U2B6**, built on the STM32 timers that remain
      free and plugged into the PCIe framework.
- [ ] **The G4MH and RH850 CPU system** (section 3), finished. Done:
      register banks and `RESBANK`, the system registers' writable
      bits, `CFALI` and `CHBII` against the MPU, FENMI and FENC
      ([frontend/g4mh.md](frontend/g4mh.md#register-banks)), the FXU on
      PE0 and the FPU's rules ([frontend/g4mh.md](frontend/g4mh.md#the-fxu)).
      The NMI pin is driven by `--nmi-at N` (`emu_cpu_ops_t::set_nmi`)
      and, on the F746, by the user button (`-DEMU_NMI_BUTTON=ON`).
      Left: the debug level -- `DBTRAP`, `DBRET`, `DBPC`, `DBPSW` -- which
      no manual in `docs/renesas/` describes. The E2 emulator manual
      (R20UT3538, added 2026-11-06) is the probe's hardware and does not
      either; what is needed is a description of the G4MH's debug
      functions themselves.
- [ ] **An emulated GTM** for the STM32F746ZG and the STM32N657, plugged
      into the PCIe framework.
- [ ] **The readings only an RH850 can settle.** Every G4MH result rests
      on Renesas' documents, cross-checked where a second one exists
      ([frontend/g4mh.md](frontend/g4mh.md#what-rests-on-the-manuals-alone));
      what none settles: a read-modify-write refused for both read and
      write setting both detail bits, DISPOSE's order, LD.DW's odd
      register in MEI, `SVLOCK` binding `LDM.MP`, and the reserved
      encodings. A U2B board, asked the way `tests/armv7m-diff/` asks
      the F746, is the way to close it.
