# Open work

Only what is still to do. Finished work is described in the document it
changed -- a frontend's in [frontend/](frontend/), the JIT's in
[jit/](jit/README.md), the scheduler's in [os.md](os.md), Linux's in
[linux.md](linux.md), the games' in [games.md](games.md), the N6 port's
in [stm32n6/](stm32n6/README.md) -- and what was learned the hard way
is in CLAUDE.md, as rules, and [lessons.md](lessons.md) at length.
Status lines here are claims about the code, so they go stale:
`scripts/report-figures.sh` regenerates the ones a host can check, and
anything measured on hardware carries the commit it was measured at.

## Needs a board

- [ ] **Board figures that predate `emu_cycles.h`**: any from a run
      longer than 19.9 s at 216 MHz (23.9 s on the F446) is a whole wrap
      short. The second half of `performance.md` and both `stm32f446/`
      pages have not been measured again, and are marked as history.

## Testing

What the fuzzers do not reach yet ([fuzzing.md](fuzzing.md)):

- [ ] **Instruction streams**: Spike as a third side for RV32 (set aside
      until late in the project, 2026-10-26); timers, and whether an ISB
      or an interrupt raised by a store is honoured in time
      ([fuzzing.md](fuzzing.md#devices-and-interrupts)); self-modifying
      code on RV32, G4MH and PowerPC, where a sync instruction must sit
      between the store and the fetch.
- [ ] **The IR**: what Thumb-2 alone refuses (305 inputs of the corpus
      replayed on the board), and inputs only a fuzzer guided by the
      Thumb-2 lowering would build; `pass_mac`, which x86-64 does not
      lower; the FP exception flags, which the reference does not compute.
- [ ] **Translation and protection**: PowerPC VLE pages, `lwarx`/`stwcx.`,
      `dcbz` and the cache instructions under the MMU, and a TLB change on
      one core retiring another's blocks; RV32 compressed code, and
      anything but loads and stores, in `fuzz-rv32-vm`; G4MH's MPU on a
      second core (a harness that keeps the first core's blocks);
      ARMv7-M's exception frames, whose stack `fuzz-armv7m-mpu` keeps open
      (the board's stacking answers are held by
      `test_stacking_board_answers` instead).
- [ ] **What arrives from outside the guest**: TFTP's packet parsing and
      SLIP's framing (lwIP's code), and virtio-9p, whose fuzzing would be
      the host filesystem's -- a scratch directory in a mount namespace
      would make it safe.
- [ ] **ARMv7-M on the board**: the corpus's sequences -- each encoding
      is asked alone, in the harness's state, not in the program that
      reached it ([fuzzing.md](fuzzing.md#the-corpus-on-the-board)).

## JIT

- [ ] **PowerPC's interpreter 0.6-1.0% slower with CR as fields**
      ([frontend/ppc.md](frontend/ppc.md#the-jit)), on the M7 only:
      callgrind counts fewer host instructions, and the field's offset
      and shift decoded once made it slower still
      (`wip/ppc-bc-decoded-field`). Untried: a `bc` handler per field,
      its word at a constant offset as CR's was; or a PC profile of
      both builds (`-DEMU_PC_PROFILE`) to see which lines grew.
- [ ] **The dot-product kernels left**
      ([jit/autovectorisation.md](jit/autovectorisation.md)): Helium's
      `VMLADAVA` on the N6, which needs MVE's contiguous and gather
      `VLDRH`, `VIDUP`, a multiply by scalar and `VMLADAVA` encoded, and
      the corpus replay (`ir-board.sh`, F746-only) on the N6 to test it.
- [ ] **G4MH's arithmetic after the flags**: with SETF a store, profile
      G4MH CoreMark's translated time by host mnemonic again
      ([backend/thumb2.md](backend/thumb2.md#by-mnemonic-powerpc-and-g4mh))
      and take what is largest.
- [ ] **PowerPC's arithmetic, what is left**: a `cmpw`'s summary
      overflow from XER (3 of its instructions), and byte swaps for a
      big-endian guest in little-endian RAM, about 10% of its translated
      time -- which only RAM kept in the host's order for words (an
      address XOR for halves and bytes) would drop.
- [ ] **The peripheral window for PowerPC**, only for a little-endian
      window it does not have (inlined on Thumb-2 for RV32, ARMv7-M and
      G4MH: [performance.md](performance.md#driver-performance-the-passthrough-window),
      [frontend/g4mh.md](frontend/g4mh.md#the-peripheral-window)).
- [ ] **The thrash policy's two equilibria**
      ([jit/tuning.md](jit/tuning.md#a-long-run-at-32-kb-two-equilibria)):
      RV32 CoreMark x2000 at 32 KB on the F746 runs at 16 host cycles an
      instruction for 163M instructions and at 28-31 for the next 290M,
      the same work in every iteration. Backing off the retry, forgetting
      a quarter at a time and never forgetting each won one regime and lost
      another (`wip/jit-thrash-retry-variants`), and a hill climb keeping
      the better mark set lost too, by translations and by time
      (`wip/jit-thrash-retry-keep`). Left: a period that does not start at
      a forget, or a cost model the host can reproduce. At 3e0df70 the
      x2000 run is 26.45 (632 compactions) where 3fb2264 was 24.16 (516),
      and the inline dot kernels are the difference: with each a call,
      21.97 (422) at both layouts, by the wall clock too -- 17% at 32 KB
      against their 0.9% at 64 KB. Whether that is their bytes or the
      equilibrium they tip the run into is the next question
      ([jit/tuning.md](jit/tuning.md#a-long-run-at-32-kb-two-equilibria)).
- [ ] **Why the dispatcher's linking fix alone is slower under
      pressure**: without the stop before a known entry it cost RV32
      CoreMark x650 12.5% at 32 KB, G4MH's 5.7% and RV32 Whetstone 18%
      (894 links against 548, twice the evictions). Nothing ships in that
      state, but whatever retention does wrong with more chained blocks
      may cost the stop's figures too
      ([backend/thumb2.md](backend/thumb2.md#what-a-block-shares-with-the-blocks-already-there)).
- [ ] **At 8 KB the JIT is still slower than the interpreter** on
      Whetstone (8.67 s against 6.23), most of it interpreted (8.6M of
      10.7M instructions); never forgetting the thrash marks helped there
      (8.07 s) and cost 16 KB 20%
      ([jit/tuning.md](jit/tuning.md#emu_jit_hot_threshold--translating-only-hot-code)).
- [ ] **Why an ITCM move costs ARMv7-M's JIT build 1.4-1.9%**: the six
      helpers and `rd_u`/`wr_u` each did, so the last are placed only where
      there is no JIT ([frontend/armv7m.md](frontend/armv7m.md#what-the-board-corrected)).
      What the interpreter's profile leaves: the loop's state, pending and
      Thumb-bit tests, and the fetch's MPU and execute-never test.
- [ ] **ITCM and ROM headroom**: a G4MH firmware has under 1 KB of the
      F746's ITCM left, and the N6's vector build 2 KiB of its ROM; the
      next thing placed there may not fit.
- [ ] **DBGMCU_CR.DBG_SLEEP on the F446**, which every F746 firmware
      sets (the N6 needs none, [stm32n6/README.md](stm32n6/README.md));
      the F446 is not attached. With the counter running in Sleep, the
      park loop could sleep with lwIP up on the F746 too (`board_idle`),
      untried over SLIP.

## Linux

From the last measurements in [linux.md](linux.md).

- [ ] **FENCE.I flushes the whole cache**, not worth making local yet
      (at most 0.3 s of a 3.15 s boot, [linux.md](linux.md#fencei-what-it-costs-and-where-it-comes-from)):
      it would take invalidation by physical page and every write path
      tracked, DMA and inlined M-mode stores included.
- [ ] **Every process translates its own copy of libc.** The key is
      satp, so identical physical pages in two address spaces are two sets
      of blocks; keying on the physical page would share them, at the cost
      of a translation on every lookup.
- [ ] **A display under Linux.** The framebuffer device exists
      ([games.md](games.md)); nothing in the device tree describes it.

## Platforms

- [ ] **A tinyusb network device over USB**
      (https://github.com/hathach/tinyusb): lwIP runs on a FreeRTOS task
      of its own ([network.md](network.md#on-a-task-of-its-own)), which a
      USB device's events would wake.
- [ ] **PCIe over USB**: the design of an emulated PCIe peripheral
      reached over USB, and of a PCIe-over-USB device firmware deployed
      on each board to exercise the peripherals it has. The use case is:
      develop firmware on pc while using real peripherals on boards.

## Guests

- [ ] **ARMv7-M**, from [frontend/armv7m.md](frontend/armv7m.md#not-done):
      the whole F746 firmware as a guest (it needs an STM32F746 machine
      model, which is a direction change), the IR and register allocator
      executed in a guest, and the board's answers kept as fixtures.

## RV32

- [ ] **The vector extension past Zve32f**: Zve64x/f/d, then V and its
      `misa` bit ([frontend/rv32-vector.md](frontend/rv32-vector.md)); the
      UDB and Sail configurations wait for an arch-test pin with vector
      suites. Under the JIT, Zve32f's min and max, compares, conversions,
      FP reductions and `vfmv` are still the helper's.
- [ ] **The vector unit's SIMD lowering, what is next**
      ([backend/thumb2.md](backend/thumb2.md#the-guests-vector-registers-the-dsp-extension)):
      .wv narrowing (a count a lane: AVX2's on x86-64, an extraction a
      lane on Thumb-2); Thumb-2's widening from bytes, narrowing to bytes
      and unsigned clip (USAT reads signed); on the N6's Helium, the
      masked single-width operations (`VPSEL` on `mve_active`'s P0); and a
      loop over an m4 or m8 group's words on Thumb-2 -- vecbench runs the
      same at a 32 KB buffer as at 64.
- [ ] **SM4's affine maps with GFNI**: `gf2p8affineqb` would do each of
      `sm4ed`'s and `sm4ks`'s maps in one instruction
      ([backend/x86_64.md](backend/x86_64.md#aes-ni-and-pclmulqdq)), but
      no CPU here has GFNI to test it on.
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

- [ ] **Simple drivers for the RH850/U2B6**, from its reference manual
      (LTSC, OSTM, KCRC and WDTB are done: [frontend/g4mh.md](frontend/g4mh.md)):
  - [ ] Option bytes, and their effect on the clocks and start-up. They
        sit in flash at a defined address.
  - [ ] The clock tree.
  - [ ] GPIO/ON (section 2), emulated on top of the STM32 GPIO HAL, the
        state of each output sent over a UDP/IP connection.
  - [ ] The ECM (section 56), so a guest can configure what each error
        does and a WDTB error can reach FENMI. FENC's external NMI has
        a source now, `--nmi-at` and the F746's button.
- [ ] **TAUD on the RH850/U2B6**, built on the STM32 timers that remain
      free and plugged into the PCIe framework.
- [ ] **The G4MH and RH850 CPU system** (section 3), what is left: the
      debug level -- `DBTRAP`, `DBRET`, `DBPC`, `DBPSW` -- which no manual
      in `docs/renesas/` describes, the E2 emulator manual (R20UT3538)
      included; it needs a description of the G4MH's debug functions
      themselves.
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
