# Validation

Each frontend is held against something that is not this code, and what
that something is differs for each — which is most of what this page is
about.

| frontend | reference | where |
|---|---|---|
| RV32 | the official architecture suite (Sail computes the expected values), the Berkeley suite | below |
| G4MH | **none.** Renesas' assembler as a second encoder, and compiled guests | [frontend/g4mh.md](frontend/g4mh.md) |
| ARMv7-M | **a Cortex-M7**: the same programs on the Nucleo-F746ZG and on the emulator | [frontend/armv7m.md](frontend/armv7m.md) |
| PowerPC | binutils for every encoding; a Python model on unbounded integers; exact rational arithmetic for the FP unit. **No e200** | [frontend/ppc.md](frontend/ppc.md) |

```sh
./scripts/run-arch-test.sh       # official riscv-arch-test
./scripts/run-riscv-tests.sh     # Berkeley suite
./scripts/build-matrix.sh --test # every configuration, and its ctest
./scripts/report-figures.sh      # the host figures, regenerated
```

## RV32: two suites, and run both

They cover different things. A regression that only the Berkeley suite
catches will sit unnoticed if only arch-test is run — which is exactly
what happened to `rv32mi/csr` when F was added — and it runs the other
way too: four `ExceptionsSv` tests failed for six days while riscv-tests
stayed at 77/77, because its guests never make a misaligned access.

Keep the suites' `-march` in step with what `misa` advertises:
`rv32mi/csr` deliberately fails when built without F and run on a core
reporting F, and that failure looks like an emulator bug until you
disassemble the test.

Both are wired to CMake targets as well:

```sh
cmake --build build/host --target arch-test        # official RISC-V suite
cmake --build build/host --target arch-test-quick  # base integer only
cmake --build build/host --target riscv-tests      # Berkeley suite
cmake --build build/host --target validate         # everything
```

Current state:

| | result | runs on |
|---|---|---|
| `riscv-arch-test`, interpreter | **510 / 510** | host |
| `riscv-arch-test`, `EMU_EXTRA_ARGS=--jit` | **510 / 510** — the whole suite through translated code | host, x86-64 |
| `riscv-arch-test --misaligned`, both backends | **523 / 523** — the default build, `RV32_MISALIGNED=ON`, and the four `Misalign*` suites it qualifies for | host |
| `riscv-tests`, interpreter | **77 / 77** | host |
| `riscv-tests`, `--jit` through an `EMU_HOST` wrapper | **77 / 77** | host, x86-64 |
| `ctest`, the default host tree -- all four frontends (`host-all`) | **56 tests**: unit, the guests of every frontend, most of them again under `--jit`, and `gdb-check`. 20 of them failed in this configuration until their flat images named a frontend: a `.bin` carries no architecture and runs as RV32 | host |
| `ctest`, RV32 alone | 20 tests | host |
| `scripts/gdb-check.sh`, the `gdb-check` ctest | a real gdb attached to all four frontends, interpreted and under `--jit`: pc at a breakpoint against the ELF symbol, the stop reply's register number against gdb's, a register round trip, `stepi` landing on the next instruction, a breakpoint ahead hit, memory -- **8 of 8 sessions** | host, x86-64, gdb-multiarch 17.2 |
| the ARMv7-M guests, both backends | all eight pass, each with the host's retired count to the instruction | Nucleo-F746ZG |
| `isatest`, JIT | **937 checks**, 96,995 retired -- the count the x86-64 host gives, which interprets 2,710 of them (2026-10-25) | Nucleo-F746ZG and Nucleo-F446RE, Thumb-2 |
| `isatest`, interpreter | **937 checks**, 96,883 retired, as on the host | Nucleo-F446RE |
| `fuzz-rv32-jit`, ten minutes ([fuzzing.md](fuzzing.md)) | 157,768 inputs, 150,680 compared between the interpreter and the JIT, no divergence once four defects were fixed; 7.8M translations and 9.8M block entries, so the JIT ran | host, x86-64, clang 21 under ASan and UBSan |
| `fuzz-armv7m-jit`, twenty minutes | 666,479 inputs, 622,915 compared, no divergence once three defects were fixed; 827,853 translations, 12.1M block entries (2026-10-26) | host, x86-64, ASan and UBSan |
| `fuzz-g4mh-jit`, twenty minutes | 134,211 inputs, 119,843 compared, no divergence once one defect was fixed; 12.5M translations, 32.8M block entries | host, x86-64, ASan and UBSan |
| `fuzz-ppc-jit`, twenty minutes | 1,845,065 inputs, 1,771,551 compared, no divergence once one defect was fixed; 113 ended in a timer interrupt and are not compared; 2.1M translations, 112M block entries | host, x86-64, ASan and UBSan |
| `fuzz-ir`, ten minutes on four workers ([fuzzing.md](fuzzing.md#the-ir-with-no-frontend)) | 5,658,718 random IR blocks, 5,595,340 compared between the reference on the block as built, the reference after the passes, and the x86-64 lowering; no divergence once three defects were fixed (two in the reference, one in `pass_reg_traffic`); eight mutants found (2026-10-26) | host, x86-64, ASan and UBSan |
| `fuzz-rv32-mmu`, ten minutes on four workers ([fuzzing.md](fuzzing.md#translation-and-protection-rv32)) | 6,196,844 inputs, 65.9M loads and stores through Sv32, PMP and the data TLB against a walk written from the spec, 17.6M data TLB entries checked; no divergence once two defects were fixed (a PPN past the bus, an M-mode straddle of an unlocked PMP entry); thirteen mutants found (2026-10-27) | host, x86-64, ASan and UBSan |
| `fuzz-g4mh-mpu`, ten minutes on four workers ([fuzzing.md](fuzzing.md#protection-g4mh)) | 1,107,316 inputs, 8.39M instructions against the U2B manual's MPU on both backends -- 3.43M completed, 1.34M MIP, 1.55M MDP (670,524 by the MPU), 2.08M MAE; 38,501 fetches across a word into another area's decision, 560,779 setting checks; no divergence once seven defects were fixed, all found writing the reference; 24 mutants found (2026-10-30) | host, x86-64, ASan and UBSan |
| `fuzz-armv7m-mpu`, ten minutes on four workers ([fuzzing.md](fuzzing.md#protection-armv7-m)) | 1,256,489 inputs, 12.6M instructions against the ARM ARM's MPU pseudocode on both backends -- 4.67M completed, 1.61M MemManage (1.28M IACCVIOL, 687,199 DACCVIOL), 766,738 HardFaults escalated, 702,575 unaligned accesses across a region's end; no divergence and no defect; 15 mutants found (2026-10-30) | host, x86-64, ASan and UBSan |
| `fuzz-ppc-tlb`, ten minutes on four workers ([fuzzing.md](fuzzing.md#translation-powerpc)) | 987,541 inputs, 8.65M runs against chapter 10 of the e200z7 manual on both backends, the JIT's blocks kept across 11.5M `tlbwe`s, MSR and PID changes -- 2.26M completed, 4.19M DTLB and 271,761 ITLB errors, 763,048 DSI, 259,063 ISI; 1.35M accesses through a mapping not the identity; 2.83M translations entered 8.12M times; no divergence and no defect; 19 mutants found (2026-10-30) | host, x86-64, ASan and UBSan |
| `fuzz-rv32-vm`, ten minutes on four workers ([fuzzing.md](fuzzing.md#fetch-and-the-jits-data-tlb-rv32)) | 2,224,770 inputs, 44.3M runs of S- and U-mode code on both backends, 43.5M held to the spec -- 5.60M completed, 28.1M fetch faults (494,286 on an instruction's second half), 8.09M data faults; 805,446 compared between the backends only, 3 set aside unfinished; 5.72M translations entered 15.6M times across PTE, satp, SUM/MXR and PMP changes; no divergence and no defect; 8 mutants found, 2 equivalent (2026-10-30) | host, x86-64, ASan and UBSan |
| `fuzz-g4mh-mpu`, ten minutes, after MEI and privilege | 222,450 inputs, 2.40M instructions on both backends -- 414,391 MDP and 613,142 MAE, each with MEI compared against table 3.38's row for what the reference encoded, 72,350 user-mode HALTs taking PIE; no divergence; 3 mutants of MEI and MS found (2026-10-30) | host, x86-64, ASan and UBSan |
| G4MH privilege and MEI, unit tests | `test_mei` (60 encodings, both backends), `test_sr_permissions` (49 cases from the tables, as data) and seven more; 28 mutants -- each fix reverted or rule broken, by `scripts/unit-mutant.sh` -- each failed by them, and MS's (2026-10-30) | host |
| PowerPC, a little-endian page (`cases.py` modes `bl`, `vl`) | 3,000 cases a build through buf's `MAS2[E]` alias and its big-endian self -- every width, byte-reversed forms, any offset, reservations, `stmw`/`lmw` -- against a model that knows only byte order, on both backends; the straddle into a big-endian page takes its DSI with ESR[BO] (11.16.5); six mutants found. Beside fuzz-ppc-tlb, which holds the plain loads, stores and multiples on such pages already: these add the byte-reversed, indexed and reservation forms and VLE, and Book E code fetched from such a page, and VLE code refused there (ISI, ESR[BO]) (2026-11-06) | host |
| G4MH FPU rules and the FXU, unit tests ([frontend/g4mh.md](frontend/g4mh.md#the-fxu)) | `test_fp_rules` (39 cases from the U2B's FP tables), `test_fxu_instructions` (all 59, CC-RH's encodings with results worked out by hand), `test_fxu_loads_stores` (19), the flags per way and FXE, UCPOP on a PE without the FXU, every one on both backends with the JIT asserted to have translated it; 66 mutants by `scripts/unit-mutants.py`, 65 failed by them and one equivalent, removed (2026-11-06) | host |
| G4MH instruction coverage, CU0 and CU1 set first | 310 of 310 CC-RH encodings executed, the FXU's 59 included; the same script against ac8d44f, 249 -- the declined CVTF.HS and CVTF.SH, which UCPOP hid from the old count, among the 61 (2026-11-06) | host |
| `fuzz-armv7m-mpu` and `fuzz-armv7m-jit`, ten minutes each, after imprecise stores | 280,671 inputs, 3.35M instructions against the reference, 620,638 stores' BusFaults taken after them on both backends, BFAR asserted for every precise one; 196,590 inputs compared between the interpreter and the JIT, 35.7M retired; no divergence once the interpreter fallback stopped its block too; 4 mutants found (2026-10-30) | host, x86-64, ASan and UBSan |
| BusFaults into nothing (`sys.c` `bus-*`) | a load's precise, BFAR the first byte outside whichever byte it started at, as the emulator had it; a store's **imprecise** -- IMPRECISERR, no BFAR, taken after the instruction after it -- which the emulator now models; 82 of 82 system tests, interpreted and `--rerun --emu-args=--jit` (2026-10-30) | Nucleo-F746ZG |
| stacking and unstacking faults (`sys.c` `stk-*`, `unstk-*`), and a store's bus error that cannot be taken yet (`bus-str-basepri`, `-in-handler`, `-faultmask`, `-disabled`) | a frame into nothing is imprecise and the exception entered; a refused frame is MSTKERR, checked with the interrupted code's privilege, its MemManage ranked against the original; a failed pop tail-chains with the frame left in place, read with the returning code's privilege. Three of these the emulator had wrong and now models; 100 of 100 system tests, interpreted and `--rerun --emu-args=--jit` (2026-10-31) | Nucleo-F746ZG |
| the lazy FP save where it fails (`sys.c` `lazy-*`), tail-chaining, FPCCR's writable bits (`fpccr-writable`), and a fault pending while disabled (`fault-pend-disabled`) | the save checked with the reservation's privilege and stopped at the first refused word, MLSPERR; the exception MMRDY's, taken at the FP instruction or pended, and LSPACT kept only if a MemManage could be taken now; a bus error a store's, imprecise; a pending exception at a return tail-chained; FPCCR's status bits writable; a disabled fault waits. Five of these the emulator had wrong and now models; 114 of 114 system tests, interpreted and `--rerun --emu-args=--jit`; `test_lazy_board_answers` holds them, 19 mutants (`tests/mutants/armv7m-lazy.py`) all found; then `fuzz-armv7m-jit` (95,703 inputs) and `fuzz-armv7m-mpu` (152,427), five minutes each, no divergence (2026-11-06) | Nucleo-F746ZG |
| the user button as the NMI pin (`-DEMU_NMI_BUTTON=ON`), pressed through the probe by `scripts/check-nmi-button.sh`: into ARMv7-M CoreMark running, and into G4MH's `guest.bin` parked in HALT | the NMI taken from a running guest (ipsr 2, no result), and a parked one waited for and woken (FENMI, cause E0H) -- which G4MH did not do: a pending FENMI left the core "not wakeable" (`test_fenmi_wakes_halt`, a mutant found). With the idle rule removed, the run ends before the press (2026-11-07) | Nucleo-F746ZG |
| the lazy save's lockup (`sys.c`'s last test, after `DIFF-END`), read through the probe from the locked-up core, and five other lockups and its HFRDY-set sibling (`run.py --define SYS_LOCKUP_*`) | the refused save at -2 with HFRDY clear locks up with FORCED and IACCVIOL beside MLSPERR, LSPACT cleared; IACCVIOL in no other lockup asked; with HFRDY set, a HardFault pends instead. The emulator had FORCED and IACCVIOL wrong; 115 of 115 in each of six runs, interpreted and under the JIT, 23 mutants found (2026-11-07) | Nucleo-F746ZG |
| G4MH under the JIT: CC-RH's LOOP, ZXB/SXB/ZXH/SXH, MULH, MULHI, SUBR and the 48-bit jumps lowered, and the peripheral window ([frontend/g4mh.md](frontend/g4mh.md#the-peripheral-window)) | `test_jit_cc_rh_loops_and_extensions` on both backends, CC-RH's encodings, the retired count compared; 19 mutants (`tests/mutants/g4mh-jit-ccrh.py`) all found; fuzz-g4mh-jit 5 minutes twice, no divergence, two mutants found by it; on the F746 CoreMark valid at the same CRC and retired count, the three-PE SMP guest's line, and `mmiobench`'s 64 hardware checks before and after the window arms (2026-11-07) | host, Nucleo-F746ZG |
| G4MH under the JIT: BN/BP/BV/BNV (the IR's MI, PL, VS, VC), JARL [reg1], DI/EI; and an interrupt's entry no longer counted as retired for ARMv7-M and G4MH | `test_jit_sign_overflow_jarl_ei` on both backends, 14 mutants (`tests/mutants/g4mh-jit-branches.py`) all found; CoreMark on the F746 at the same CRC, 215 instructions interpreted under the JIT; ARMv7-M CoreMark retires 11,144,363 under both backends, host and board; the three-PE SMP guest's line (2026-11-07) | host, Nucleo-F746ZG |
| The JIT's charge to the budget: an exception taken partway through a block, and ARMv7-M's imprecise store ([jit/README.md](jit/README.md#chaining-compaction-and-committed-code)); G4MH's mmiobench asking its refused stores ([frontend/g4mh.md](frontend/g4mh.md#the-peripheral-window)) | `test_block_charged_its_helpers` (the dispatcher, with a real x86-64 block), `test_refused_store_stepped_over` and `test_refused_store_fault_loop` (G4MH, both backends), `same-on-both-backends.py` on four guests that resume from a fault (`guest-stepover-both`, `guest-ppc-stepover-both`, `guest-armv7m-stepover-both`, `guest-g4mh-mmiobench-both`); seven mutants -- each frontend's counter, both of ARMv7-M's paths, the dispatcher's sum, G4MH's bind -- each fails its test; every fuzz target 60 s, clean; on the F746 the step-over guests retire 1,277 (ARMv7-M) and 878 (RV32) on both backends, and G4MH's mmiobench passes 336 checks on both (2026-11-08) | host, Nucleo-F746ZG |
| ARMv7-M's mmiobench, its refused stores asked as the M7 answers them: one imprecise BusFault, IMPRECISERR without BFARVALID ([frontend/armv7m.md](frontend/armv7m.md#the-peripheral-window)) | 160 checks, 0 failures, both backends on the F746; at 187bce6 the same guest failed 96, on both, since 2215c8e made stores imprecise (2026-11-08) | Nucleo-F746ZG |
| ARMv7-M guests in the M7's own map: the image at `0x0800_0000`, a 16 KiB ITCM at zero, RAM from the DTCM's `0x2000_0000` (`emu_cpu_ops_t::guest_map`, [memory.md](memory.md#armv7-m)) | `guest-armv7m-tcm-both` (code run from the ITCM, rewritten and run again, the DTCM's edges), its mutant with no ITCM failing it; every host test (121), gdb-check's ARMv7-M sessions breaking at `0x0800_0130`; fuzz-armv7m-jit moved with it, its nine regressions doing what they did (one rebased); on the F746 `alu`, `itblock`, `nvic`, `t2exec`, `hello`, `tcm` and `mmiobench` under the JIT each print their pass line, CoreMark interpreted validates at 11,144,363 retired (2026-11-08) | host, Nucleo-F746ZG |
| ARMv7-M's self-modifying code, held exactly: a store into translated code stops the block that made it and reruns the store from its encoding before the store (`smc_stop`) | fuzz-armv7m-jit with the program in writable RAM (`CFG_SMC`, half of all inputs): two defects, each kept as a regression; ten minutes twice after the fixes, clean (111,671 inputs, 99,914 compared) | host |
| The telnet console's byte stream against RFC 854 (`fuzz-telnet`) | IAC IAC as data, found writing the reference and by the first run; four mutants each found in seconds; two minutes clean, 364,877 runs | host |
| ARMv7-M's floor: a block that faults on its first instruction charged nothing (`emu_jit_ops_t::fault_not_charged`) | `guest-armv7m-faulthead-both`, 785 on both backends where it was 785 against 806, and a fault that cannot be taken ending the run in lockup; its mutant failing it; fuzz-armv7m-jit six minutes, 115,707 inputs, nothing spinning | host |
| the NMI pin from outside (`--nmi-at`, `emu_cpu_ops_t::set_nmi`) | G4MH takes FENMI (cause E0H at slot E0H), ARMv7-M its NMI (IPSR 2), both backends; RV32 says it has no pin; three mutants -- G4MH's hook removed, ARMv7-M's emptied, the slice clamp gone -- fail 2, 2 and 4 of the 5 tests (2026-11-07) | host |
| the JIT's code buffer first in the F746's DTCM, the hash tables pinned ([performance.md](performance.md#the-jit-buffer-in-dtcm)) | `isatest` passes; CoreMark x650 at `EMU_JIT_CODE_BYTES=32768` validates (`crcfinal 0x25e3`, 1,335 compactions, 43.3 s by the host); CoreMark under the JIT on all four frontends and Dhrystone at two `.bss` layouts, the retired counts and CRCs unchanged; every F746 configuration links under the DTCM assertion (2026-11-06) | Nucleo-F746ZG, Thumb-2 |
| unaligned accesses across an MPU region's end (`sys.c` `mpu-ua-*`) | MemManage at the first refused byte, a store's bytes before it written -- the emulator agrees, interpreted and under the JIT; 70 of 70 system tests (2026-10-30) | Nucleo-F746ZG |
| ARMv7-M, sp and pc as operands ([frontend/armv7m.md](frontend/armv7m.md#what-the-board-corrected)) | 1,935 register variants and 69 named encodings asked: every one the board refuses, both decoders now refuse; what still differs is a value derived from sp or a code address, which the two images place differently. 36 answers held by `test_sp_pc_board_answers` | Nucleo-F746ZG |
| ARMv7-M's two decoders, `ARMV7M_UNDEF_FULL=1` | all 402,712,576 encodings agree, 215,972,741 of them UNDEFINED | host |
| after the sp/pc rules and the fuzzers' fixes (2026-10-26) | ARMv7-M `alu`, `t2exec`, `crypto` and CoreMark (`crcfinal 0x65c5`, 11,145,477 retired, the host's count) under the JIT and `alu` interpreted; PowerPC `sys` 184 checks and `intc` 49, no failures, and CoreMark valid at 12,170,464 retired, the host's count | Nucleo-F746ZG, Thumb-2 |
| `probes/instret.S` | a trapping instruction does not retire: Sail and Spike agree on minstret; mcycle follows Spike's answer, Sail's is permitted | host, against Sail and Spike |
| CoreMark x650 at a 32 KB cache, after the chaining fix | "Correct operation validated", `crcfinal 0x25e3`, 8,485 compactions; 78.56 s by the host, 79 by the firmware | Nucleo-F746ZG |
| `isatest` with the Thumb-2 chaining fix reverted | fails `fs-dirty-flw-chained` (got 2, want 3) and nothing else -- the board had the defect | Nucleo-F746ZG |
| The dispatcher links only after a return through an unlinked exit (`EMU_JIT_RET_UNLINKED`), searching the chain for it; every translator stops before a known entry ([jit/README.md](jit/README.md#chaining-compaction-and-committed-code), [backend/thumb2.md](backend/thumb2.md#what-a-block-shares-with-the-blocks-already-there)) | `test_chain_links_a_later_exit`, `test_chain_no_link_across_an_interrupt` (its two guards each pass alone, fail together), mutants of the flag and of the unlink target found; 121 host tests; arch-test 510/510 and riscv-tests 77/77 under `--jit`; fuzz-ir, fuzz-rv32/armv7m/g4mh/ppc-jit and fuzz-rv32-vm two minutes each, clean; on the F746 CoreMark under the JIT 3.6-8.0% faster on all four at two offsets, the retired count and CRC the same; at 32 KB RV32 CoreMark x650 valid (`crcfinal 0x25e3`, 33.70 s by the host's clock, past the counter's wrap) and G4MH's 8.9% faster, RV32 Whetstone 2.08 s against 3.80 (2026-11-08) | host, Nucleo-F746ZG |
| DBGMCU_CR.DBG_SLEEP in every F746 build (`scripts/check-wfi-attach.sh`, `-DEMU_WFI_CHECK=ON`) | from inside the parked firmware, the register cleared at boot: 53,999,852 cycles in 250 ms of WFI and 250 wake-ups; the probe then reads S_SLEEP, CYCCNT at 216 MHz by the host's clock; with the firmware's line removed 43,558 cycles and no probe access, twice of two (2026-11-08) | Nucleo-F746ZG |
| fuzz-armv7m-jit's corpus on the M7 ([fuzzing.md](fuzzing.md#the-corpus-on-the-board)) | 4,000 of its 12,322 encodings, the board resumed once past the case it did not survive: 3,998 the same, two differing -- both the shifted `ADD.W sp, sp, Rm` the M7 executes; asked on purpose after, every sp writer drops bits [1:0] on the board and kept them here (2026-11-08) | Nucleo-F746ZG |
| sp as the M7 writes it: ADD/SUB sp, sp, Rm with any shift, bits [1:0] dropped at every writer ([frontend/armv7m.md](frontend/armv7m.md#what-the-board-corrected)) | `test_sp_write_board_answers` (14 sequences) and 9 new `test_sp_pc_board_answers` rows; `guest-armv7m-spwrite-both`; five mutants, each found; both decoders equal over all 402,712,576 encodings (215,965,802 UNDEFINED); 122 host tests; fuzz-armv7m-jit 2 and 3 minutes and fuzz-armv7m-mpu 2, clean; the corpus and all 31 probes replayed (`run.py --rerun`) agree; on the F746 `spwrite` passes on both backends and `alu`, `itblock`, `nvic`, `t2exec`, `tcm` and `mmiobench` under the JIT; CoreMark under the JIT unchanged, interpreted +0.94% and +1.01% (2026-11-08) | host, Nucleo-F746ZG |
| RV32 scalar cryptography, Zkn and Zks ([frontend/rv32.md](frontend/rv32.md#scalar-cryptography-zkn-and-zks)) | the architecture suite 544/544, its 34 new tests among them, interpreted and under the JIT, and 37/37 Zbc and K tests with the native lowering turned off (`EMU_HOST_CRYPTO=0`); `probe-kcrypto` and `-jit`, every S-box entry against Sail and Spike, three mutants found; `isatest` +8 checks on both backends; riscv-tests 77/77 under the JIT; fuzz-ir and fuzz-rv32-jit two minutes each, clean, and a `clmulr` mutant found by fuzz-ir at input #243 (2026-11-11). `clmul`, `clmulh` and `clmulr` reach `EMU_IR_CLMUL` under the JIT: Zbc and Zbkc 5/5 with and without the native lowering, and a mutant of its `clmulr` shift fails `Zbc-clmulr-00` alone; fuzz-rv32-jit three minutes, clean; 123 host tests. On the F746 `isatest` passes, CoreMark x650 at 32 KB is valid (`crcfinal 0x25e3`, 34.46 s, the ratio and the host's clock agreeing) and CoreMark under the JIT is 0.98% faster than before the round at two offsets, the retired count and CRC the same -- once Thumb-2 stopped promising CLMUL (below); on the N6, whose ROM and RAM split moved to 216K/295K for it, `isatest` passes with the same 108,593 retired; all 36 configurations of the matrix (2026-11-14) | host, x86-64 with AES-NI, Nucleo-F746ZG, Nucleo-N657X0-Q |
| A lowering's refusal of what `emu_ir_can_lower` promised is counted and printed as `unkept` (`emu_ir_note_refused`; [jit/README.md](jit/README.md)) | `test_unkept_counted`; on the F746 a mutant with Thumb-2's CLMUL case removed printed `unkept 55 refused after emu_ir_can_lower promised them (first op 38 aux 0)` -- 38 is CLMUL -- beside 910,195 instructions interpreted where the fix leaves 1, and `board-ab.sh` marks such a side `UNKEPT` (2026-11-14) | host, Nucleo-F746ZG |
| `EMU_IR_AESSB` and `EMU_IR_CLMUL` on x86-64 (AES-NI, PCLMULQDQ) | `test_lower_crypto_ops`: every byte both ways and 72 operand pairs, against references computed in the test; two mutants found (255 and 68 checks) (2026-11-11) | host |
| the N6's WFI without DBG_SLEEP ([stm32n6/README.md](stm32n6/README.md#wfi-keeps-its-clocks-here)) | `check-wfi-attach.sh TREE stm32n6`: 150,000,030 cycles in 250 ms of WFI with the bit set, 150,000,001 never written; probe-rs reads DHCSR S_SLEEP from the parked core (2026-11-11) | Nucleo-N657X0-Q |
| ARMv7-M's interpreter: six helpers in ITCM, compares off the sp/pc path ([frontend/armv7m.md](frontend/armv7m.md#what-the-board-corrected)) | interpreted CoreMark 2.6-4.7% faster at four .text offsets, under the JIT 0.24% faster, outputs and retired counts the same; all host tests and both ARMv7-M decoders equal over every encoding (2026-11-11). The round's final tree against its base: interpreted ARMv7-M CoreMark 1.90% and 3.68% faster at offsets 0 and 64, PowerPC CoreMark under the JIT unchanged (19.88, 19.89), G4MH's +0.18% and +0.27%, every row's retired count, CRC and validation the same (2026-11-14) | Nucleo-F746ZG, host |
| PowerPC efsmul's common case first in its helper ([frontend/ppc.md](frontend/ppc.md#what-uses-it-and-what-it-costs)) | fuzz-ppc-jit holds it against the interpreter's general path: `efsmul-edges.S` (22 products at the fast case's edges) and a third configuration word with FINXE; six mutants -- the tie rule, the low word's sticky bits, the exponent bound, FINXE, FRMC, FG as FX -- each found, five by the seed within 13 s; fuzz-ppc-jit three minutes clean; 123 host tests, the PowerPC ones 44/44. On the F746 against c866eca Whetstone under the JIT 15.1% faster at two offsets, the wall clock agreeing (2.25 s to 1.92), interpreted Whetstone and CoreMark under the JIT unchanged, retired counts the same (2026-11-14) | host, Nucleo-F746ZG |
| PowerPC efsadd and efssub's common case first in their helper ([frontend/ppc.md](frontend/ppc.md#what-uses-it-and-what-it-costs)) | `efsadd-edges.S` (31 cases) against the interpreter's general path; eight mutants, six found by the seed within 6 s, the exponent bound once a tie at 254 into overflow was added, one equivalent by arithmetic; fuzz-ppc-jit three minutes clean; 125 host tests. On the F746 against a7e50fd Whetstone under the JIT 21.5% faster at two offsets, the wall clock agreeing (1.92 s to 1.50), interpreted Whetstone and CoreMark under the JIT unchanged, retired counts the same (2026-11-14) | host, Nucleo-F746ZG |
| The same arithmetic in the block, under a predicated helper (`wip/ppc-efsmul-in-block`), **discarded** | correct by the same seed and mutants, fuzz-ir finding a helper lowering that ignored its predicate in 23 s; on the F746 Whetstone under the JIT 19% slower, block entries +38% (2026-11-14) | host, Nucleo-F746ZG |
| Zkr: `seed` and mseccfg, the bits the platform's ([frontend/rv32.md](frontend/rv32.md#zkr-the-entropy-source)) | `probe-zkr`, 18 cases, equal on the emulator, Sail and Spike, four mutants each failing it on its case; `test_seed` (the four answers and no source, a refused access not polling, mseccfg's mask); `isatest` `test_zkr` on the host's RDSEED, on getrandom (`EMU_HOST_RDSEED=0`), under the JIT, and on the F746 (both backends) and the N6, each RNG through the HAL; fuzz-rv32-jit two minutes clean, its `seed-csr.S` finding a source restarted once instead of per side in 14 s; arch-test 544/544 and riscv-tests 77/77 on both backends; 125 host tests (`probe-zkr` and `-jit` among them), and 118 in a `-DRV32_EXT_ZKR=OFF` build; Linux boots with `zkr` declared, OpenSBI opening it to S (2026-11-14) | host, Nucleo-F746ZG, Nucleo-N657X0-Q |
| isatest's retired count under the JIT, explained | `minstret` marked after each of its 38 tests on both backends: only `test_zihpm` and `test_timer_interrupt` differ, and in each the wait for the timer interrupt runs 11 more iterations under the JIT (491 against 502, 158 against 169) -- the interrupt taken at a block boundary, which the architecture permits; called twice or moved later, `test_zkr` changes the gap only as layout does (2026-11-14) | host |
| A profiled PowerPC run and the plain one ([performance.md](performance.md)) | the same run: profiled CoreMark under the JIT 19.97 against 19.96, 410 translations and 2 compactions each; 32 KB less guest RAM alone (`EMU_LAYOUT_PAD_RAM`) 19.98, and at a 32 KB buffer 218.90 against 218.89 with 10,116 translations each (2026-11-14) | Nucleo-F746ZG |
| SM4 under the x86-64 JIT, `EMU_IR_SM4SB` ([backend/x86_64.md](backend/x86_64.md#aes-ni-and-pclmulqdq)) | `scripts/sm4-affine.py` derives the affine maps around AES's S-box from the definitions and checks all 256 bytes; ctest `sm4-affine` holds the tables to it (a byte changed fails it); `test_lower_crypto_ops` all 256 bytes against SM4's algebraic form, two lowering mutants failing 255 checks each; `probe-kcrypto` and `-jit` equal to Sail and Spike, a mutant of `sm4ed`'s linear map failing the JIT's alone; arch-test Zksed and Zksh 4/4 interpreted, under the JIT and with `EMU_HOST_CRYPTO=0`; fuzz-ir two minutes clean and a wrong second map found at input #5,945; fuzz-rv32-jit three minutes clean; 126 host tests; `isatest` on the F746, where SM4 stays the helper's (2026-11-14) | host, x86-64 with AES-NI and SSSE3, Nucleo-F746ZG |
| ARMv7-M's `rd_u` and `wr_u` in ITCM where there is no JIT ([frontend/armv7m.md](frontend/armv7m.md#what-the-board-corrected)) | interpreted CoreMark 3.02% and 1.31% faster at two offsets, under the JIT 0.00% at both, retired counts and CRCs the same; the two other forms measured beside it, both costing the JIT 1.7-1.9% (2026-11-14) | Nucleo-F746ZG |
| x86-64 memory slow paths through shared stubs ([backend/x86_64.md](backend/x86_64.md#the-data-tlb-probe)) | CoreMark's code -6.0%, host instructions within 0.03% on five rows, outputs equal; arch-test 544/544 and riscv-tests 77/77 under the JIT; Linux at 64 MB and at 4 MB (22 compactions against 28); 121 host tests at a 256 KB buffer (`host-evict`), where the first, relative-call version fails six; a wrong spec byte fails 61 test_ir checks; fuzz-rv32, -armv7m, -g4mh and -ppc-jit a minute each, clean (2026-11-14) | host |
| PowerPC's FP fast cases inlined into the IR wrappers ([frontend/ppc.md](frontend/ppc.md#what-uses-it-and-what-it-costs)) | on the F746 against ba8bc8a Whetstone under the JIT 5.8% and 6.0% faster at two offsets, the wall clock agreeing (1.50 s to 1.41), CoreMark under the JIT -0.05% at both, retired counts the same; 49 PowerPC host tests; fuzz-ppc-jit two minutes clean and a mutant in the moved code found by the seed in 18 s (2026-11-14) | host, Nucleo-F746ZG |
| PowerPC efsdiv's common case first, by long division in base 2^8 ([frontend/ppc.md](frontend/ppc.md#what-uses-it-and-what-it-costs)) | `efsdiv-edges.S` (25 cases, each claim checked against exact rationals); six mutants, five found within 11 s and one equivalent (no quotient carries into the next binade); fuzz-ppc-jit two minutes clean; 49 PowerPC host tests. On the F746 against e04aa83 Whetstone under the JIT 10.9% and 11.0% faster at two offsets, the wall clock agreeing (1.50 s to 1.35), CoreMark under the JIT -0.15% (2026-11-14) | host, Nucleo-F746ZG |
| Cold code interpreted to its jump under the hot threshold ([jit/tuning.md](jit/tuning.md#emu_jit_hot_threshold--translating-only-hot-code)) | host CoreMark's translations fall with the threshold (279, 206, 185, 167) where one instruction at a time they rose (1,060 at 2), same CRC; riscv-tests 77/77, isatest and Dhrystone at 4; on the F746 RV32 Whetstone at 8 KB 16% faster at 8, CoreMark at 32 KB a third faster at 4, CRCs the same; at the default buffers CoreMark 1.9-4.2% faster at 4 on three frontends and Whetstone 14% slower, so the default stays 1 (2026-11-14) | host, Nucleo-F746ZG |
| The hot threshold armed at the first compaction, 8 by default, and a cold stretch run in one call ([jit/tuning.md](jit/tuning.md#emu_jit_hot_threshold--translating-only-hot-code)) | `test_hot_threshold_waits_for_compaction`, `..._after_compaction` and `test_cold_run_learns_its_stretch` (14 interpreter calls for three passes of a ten-instruction stretch, where stepping made 30), with four mutants -- no hint, never armed, armed regardless, not re-armed on a set -- each named by them; the JIT's tests pinned to 1, which the default of 8 broke in two chaining tests; host ctest 127/127 at 1 and at 8; on the F746 CoreMark x120 at 32 KB 6.58 s to 2.62 at two layouts, CRC 0xd340, Whetstone at 16 KB 7.56 to 5.99 and at 8 KB 12.69 to 8.67, the same retired counts; at the default buffers four frontends within the layout's own movement (2026-11-14) | host, Nucleo-F746ZG |
| Computed jumps through a stub when they are plain jumps ([jit/README.md](jit/README.md#computed-jumps-the-stub-emu_jit_ibtc)) | host ctest 123/123, riscv-tests 77/77 and arch-test 557/557 under the JIT, gdb-check 8 of 8 sessions, nine guests' output and retired counts unchanged; the fuzzers 30-60 s each with a call-and-return seed per frontend reaching the stub (17,827 to 26,511 jumps sent on, the PowerPC seed 69 of 72); mutants: the x86-64 stub's context compare (found in 21 s) and pc compare (7 s, by `returns-sharing-a-slot.S` only), G4MH's EI and ARMv7-M's ISB marked as jumps (test_g4mh's "EI four times", `guest-armv7m-isbpend-both` -- which passed until the store and the ISB were one block); on the F746 isatest, CoreMark x650 at 32 KB valid, and four frontends' CoreMark 0.45-7.5% faster at two layouts (2026-11-14) | host, Nucleo-F746ZG |
| Linux 6.12 under OpenSBI, `scripts/run-linux.sh` | boots to userspace; the test init passes **17 checks** from U-mode on the extensions the device tree declares | host, x86-64 JIT |
| CoreMark, 120 iterations | `crcfinal 0xd340` on the interpreter and on the JIT at four cache sizes | Nucleo-F746ZG |

`run-arch-test.sh` **builds its own runner**, with `-DRV32_MISALIGNED=OFF`,
because the suite validates the core against a description of it and
that description says misaligned accesses trap. A stale runner at the
path a script defaults to once hid a real regression for six days.
`--misaligned` builds the *default* configuration instead, and runs it
against a description derived for it; see below.

### What the host suites do not cover

**The Thumb-2 lowering**, which compiles only for ARM. The x86-64 backend
exists so that everything *above* it — each translator, the IR, the
passes, the framework — runs under the suites on a host; what turns IR
into Thumb-2 is validated by flashing a guest and reading the UART.

That gap is not theoretical. Every defect below was found on hardware,
and none could have been caught by a signature-checking suite on a host:

| defect | how it presented |
|---|---|
| inlined store ignored PMP | a protected region was writable under the JIT only |
| inlined store skipped the LR/SC reservation break | a later `SC` wrongly succeeded |
| loop cap compared the wrong register | *no* wrong answer — chained loops simply ran unbounded |
| loop chaining dropped past a branch's reach | *no* wrong answer — one loop shape ran 2.4× slower |
| `rm=dyn` resolved `RMM` as round-to-nearest | ties rounded to even where the guest asked for away |
| `mstatus.FS` decided at translation | FP ran after the guest turned the FPU off |
| PMP flush watched a flag, not the configuration | a store landed in memory PMP had been told to deny |
| a shift by zero encoded as a shift by 32 | two architecture tests, on the board only |
| a compaction caused by an overflow unlinked nothing | a hard fault, or a hang: RV32 CoreMark at 120 iterations never finished |
| the cycle counter wrapped | *no* wrong answer — guest time restarted every 19.9 s, and every long run's performance figure was a wrap short |

Three of those produced no wrong answer at all, only numbers that made
no sense. Three were staleness: a decision taken when a block was
translated, still in force after the state behind it changed. The
self-test grew from 148 checks to 298 chasing them (it is 748 now, the
rest being the extensions, the suites below and the B extension), and the checks that
matter are the ones that re-execute *one* instruction at *one* address
after changing the state it was compiled against — a fresh call site is
translated against the current configuration and proves nothing.

The last two rows are from the framework and the platform rather than
from the emitter, and they are why "run it on the board" means a guest
large enough to fill the code cache and a run long enough to wrap the
counter: `isatest` does neither, and passed throughout both.

**The B extension's 230 checks are the board's only view of its
Thumb-2 lowering.** The architecture suite covers Zba/Zbb/Zbc/Zbs
properly -- and, since they are translated, now covers the translator
too, under `--jit` -- but only on x86-64. So `isatest` holds every one
of them to `tests/guest/bitmanip-expect.py`, a model written from the
specification with unbounded integers, at the inputs where the two
hosts disagree with the guest (a count of zero, an amount of 32, the
sign boundary), with every register aliasing. Thirty-four mutants of
the lowering, its helper, the shared arithmetic and the interpreter
were run against it: every one that changes an answer fails on the
backend it changes. Five pass everywhere, and none of them can change
an answer -- `bexti` and `bext` through an arithmetic shift are still
one bit after the `& 1`, a rotation by `32 - 0` is masked to 0 by both
emitters, `ror` by zero written as `v | v` is `v`, and one was a
mutant of the helper that, as written, could not alter its result.

## Floating point (F and D)

Both are implemented — the 64-bit register file with NaN-boxing,
`fcsr`/`frm`/`fflags`, `mstatus.FS`, all of OP-FP in both widths, the
fused multiply-adds, the loads and stores, and `Zcf` and `Zcd`, which
`C` on RV32 is defined to include alongside F and D.

There is one implementation. **Berkeley SoftFloat is the FP unit** — not
an option, and a missing checkout is a configure error rather than a
fallback. It is the library the RISC-V FP spec was written against, and
the fit is exact rather than convenient: its rounding modes are
numerically identical to `frm` and its exception flags to `fflags`, so
neither needs translating, and its `mulAdd` is a genuine single-rounding
fused multiply-add.

It was once a build option beside a host-FPU path through `<fenv.h>`,
and that path was the default. It passed 172 of 224 F tests: the flags
it could report were the ones the hardware happened to raise, which
differ from RISC-V's rules on the fused multiply-adds and around
subnormals. Worse, the documented way to select SoftFloat named a
variable that did not exist, so the check that would have shown the
difference changed nothing and reported nothing. That history is why
`scripts/check-doc-flags.sh` exists.

The JIT lowers the five IEEE-exact single-precision operations to the
host FPU and sends the rest to the same SoftFloat routines the
interpreter uses; see [jit/floating-point.md](jit/floating-point.md).

**Zcb is implemented and passes 7/7.** It reuses the `funct6=100111` slot that
RV64 spends on `c.subw`/`c.addw`: bits [6:5] select `c.mul` or a group of unary
operations, and the byte/halfword accesses sit in quadrant 0 under `funct3=100`.
Three of the unary ops (`c.sext.b`, `c.zext.h`, `c.sext.h`) expand to Zbb
instructions, which is why the spec makes Zcb depend on Zbb — without it there
would be nothing to expand them into.

## Official RISC-V Architecture Test Suite — 510/510

[`riscv/riscv-arch-test`](https://github.com/riscv/riscv-arch-test), the RVCP
suite governed by RISC-V International. Modern versions are self-checking: the
build runs the **Sail golden model** to compute expected results and bakes them
into each test, which then reports `RVCP-SUMMARY: TEST PASSED/FAILED` and sets
its exit status.

Our device description lives in
[`tests/arch-test/`](../tests/arch-test/rv32cortex-m-rv32) — a UDB
configuration, a Sail model configuration, the `RVMODEL_*` macros and a linker
script — and is version controlled with the emulator rather than inside a
third-party clone. `scripts/run-arch-test.sh` fetches the suite, the Sail model
and the UDB gems, then builds and runs.

Prerequisites beyond the normal toolchain: `uv`, Ruby, and Bundler
(`gem install --user-install bundler`).

**What 510 covers is what the script names.** ACT selects suites by
*directory name*, so a suite nobody names is a suite nobody runs,
whatever the core implements. The total stood at 378 for months, and
that was a statement about thirty directory names:

| | tests | how it came to be named |
|---|---|---|
| the thirty | 378 | |
| Zicond, Zihintpause, Zihintntl, `ZihintntlZca`, Zihpm | 13 | the extensions were added |
| `Zcf`, `Zcd`, `ZcbM`, `ZcbZbb`, seven more PMP directories, `SvZicbo`, `SvPMPZicbo`, two `ExceptionsSv*` | 99 | listing the checkout to find `ZihintntlZca` |
| Zicbop | 6 | claimed -- three in its own directory, and three that had been sitting in `SvZicbo` and `PMPZicbo`, already named, waiting for the description to say the extension was there |
| Zfhmin, ZfhminD, Svinval | 14 | the extensions were added. Sscofpmf, after them, has no directory: [frontend/rv32.md](frontend/rv32.md#the-small-extensions) has the probe that stands in for one |

That last row is both halves of the mechanism in one line. Naming a
directory *offers* its tests; each test's own requirements, checked
against the UDB description, decide whether it is built. A suite can be
named and still be silently short.

The 99 were for things this core had implemented for a long time.
**Thirteen of them failed the first time they ran**, and every one was
a defect in the emulator; they are in the table at the end of this
page. Both groups were confirmed the usual way, by breaking the
emulator and watching exactly the right tests fail.

**A build without PMP is a configuration the matrix checks**
(`host-nopmp`, `-DRV32_EXT_PMP=OFF`). `isatest` and `sv32bench` hung
there the first time it was built: a test relied on a PMP fetch fault to
leave S-mode, and both wrote PMP CSRs that do not exist. The guests take
the option now, as they take `RV32_MISALIGNED`, and without PMP
`isatest` checks instead that reading and writing a PMP CSR is an
illegal instruction.

When the checkout moves, list `tests/rv32i`, `tests/priv` and
`tests/priv/pmp/pmp32` and account for every directory: the comment
above the list in `scripts/run-arch-test.sh` names each one that is
left out and why.

### The default build, and the four suites only it can take

The description above is of an `RV32_MISALIGNED=OFF` core, and the
emulator's default is ON -- picolibc's word-at-a-time `strcmp` reads
`lw` off an odd pointer. So for as long as that default has stood, the
build everybody runs was described by nothing, and `Misalign`,
`MisalignD`, `MisalignF` and `MisalignZca` -- thirteen tests, each
requiring `MISALIGNED_LDST: true` -- could not be offered at all. That
mattered more than the count suggests: the misaligned path had just been
found wrong under paging, by a hand-written test, with nothing in the
suite able to see it.

`scripts/run-arch-test.sh --misaligned` builds the default configuration
into `build/arch-test-host-misaligned` and runs it against a second
description, `rv32cortex-m-rv32-misaligned`, which
`tests/arch-test/derive-misaligned.py` *generates* from the first. The
two differ in one property and agree in every other, and two hand-kept
copies of an 18 KB Sail config would stop agreeing the first time one of
them was edited; the script asserts the match count of every line it
changes, so a base edit that moves one fails there instead of yielding a
description that still says "raises". What it changes:

| | value | because |
|---|---|---|
| `MISALIGNED_LDST` | true; Sail `load_store: None` globally and in every region | loads and stores complete, and the bus hands a device a misaligned access as it is |
| `MISALIGNED_MAX_ATOMICITY_GRANULE_SIZE` | 0 | `mem_wide` splits an access into aligned bus transactions, and per page under Sv32 |
| `MISALIGNED_SPLIT_STRATEGY` | `sequential_bytes` | every part is checked before any is performed, and a fault names the lowest part refused |
| `MISALIGNED_LDST_EXCEPTION_PRIORITY` | high, unchanged | AMOs and LR/SC still raise, and check alignment first; UDB allows high only with a zero granule |

**523/523 on both backends**: the 510 and the thirteen. The thirteen were
then run against the OFF build and all thirteen failed, which is what
says they test the property and not merely run.

Four extensions the core implements have **no suite at all**: Zalasr,
Zawrs, Sscofpmf and Zkt. The first three are held by probes against
the reference models -- [below](#probes-and-the-reference-models) --
and Zkt, a promise about timing, by an examination of every path its
instructions take ([frontend/rv32.md](frontend/rv32.md#profiles)). And
three of the defects the 99 led to are held by no architecture test
either, only by `isatest`: the suite is a floor.

The run is of the suites named and nothing else. It used not to be:
`run_tests.py` executes every ELF under the directory it is given, and
the work directory keeps whatever any earlier run built, so
`--extensions I` -- the `arch-test-I` ctest -- ran 39 tests from a fresh
checkout and 496 after one full run, and its name was a statement about
the history of a directory. The script now hands it a directory of
links to the named suites' ELFs, prints how many the work directory
holds besides, warns about a named suite that built nothing, and fails
if none of them built anything.

## Probes and the reference models

A probe is a short bare-metal program in `tests/arch-test/probes/` that
asks a question no suite asks, and leaves the answers in registers.
`scripts/run-probe.py` builds it, runs it on the emulator and on every
reference model it names that is installed, and compares the registers
it lists:

```sh
./scripts/fetch-spike.sh                              # once: build/spike
./scripts/run-probe.py tests/arch-test/probes/zalasr.S
ctest -L probe                                        # all of them, both backends
```

| probe | asks | Sail | Spike |
|---|---|---|---|
| `zawrs.S` | WRS.NTO and WRS.STO in M and below M under TW, with and without a reservation | agrees | differs on one case, permitted |
| `zalasr.S` | every load-acquire and store-release, sign extension, misaligned, unmapped, the eight reserved encodings, an SC after a store-release | has no Zalasr | differs on one register, permitted |
| `sscofpmf.S` | which bits of `mhpmevent3h` exist, `mideleg`, `sie`, `scountovf` and `sip` from S, STI against LCOFI | agrees | agrees |
| `sstvala.S` | `stval` for 23 traps from S to S under Sv32: 16- and 32-bit illegal instructions, ones illegal by FS, TVM, TW, TSR or `mcounteren`, page and access faults on fetch, load and store, an instruction straddling pages, misaligned LR and AMO, an S-mode trigger | agrees but for two, permitted: no Sdtrig, and WFI completes | agrees |
| `umode.S` | every privileged instruction from U-mode with TW, TSR and TVM clear | agrees | agrees |
| `pmpstraddle.S` | an M-mode doubleword half inside an unlocked PMP entry, and the same entry locked | agrees | agrees |
| `sv32ppn.S` | a leaf whose PPN reaches past 4 GiB -- PTE bit 31 and bit 30, a megapage, a pointer to a table there -- and a table inside the CLINT | agrees, but for the CLINT, permitted: a PMA, and Sail's walk refuses MMIO | agrees |
| `sdtrig.S` | when a breakpoint trigger may match: M with MIE clear and set, S with breakpoints to M or to S and SIE clear or set, U | has no Sdtrig | agrees |

**Two models, because one reading is not a check.** Sail is the suite's
golden model and does not have everything this core implements: Zalasr
is in neither 0.13.1, 0.15 nor the 2026-10-12 nightly. Spike
(riscv-isa-sim, BSD-3-Clause) has it, and where both have an extension
they can disagree with each other -- on Zawrs they do. Neither is in the
tree: Sail comes from `run-arch-test.sh`, Spike from
`scripts/fetch-spike.sh` at a pinned commit, both into `build/`. This
project takes nothing from GPL code, even as an oracle; Spike is not
GPL.

**A difference the specification allows is declared, not hidden.** Each
probe lists them (`probe-permitted:`) with the reason, and the runner
prints them and passes. Three so far are cases the specification
leaves to the implementation: whether an SC fails after the hart's own
store to the reserved word, and how long a `wrs.nto` or a WFI below M
may take before TW makes it illegal. The fourth is a model lacking the
extension: Sail has no Sdtrig. Anything else that differs fails.

**The two newest found four defects on their first run** -- MRET
executing in S-mode and U-mode, WFI parking the hart from U-mode, a
16-bit instruction's illegal trap naming its 32-bit expansion, and
triggers matching only on their `m` bit
([frontend/rv32.md](frontend/rv32.md#profiles)). Both suites passed
with all four in place.

**A model's configuration is part of its answer**, as this page already
says of `sail.json`. Spike makes `mcounteren`'s counter bits writable
only when Zicntr and Zihpm are in its `--isa`; without them S-mode's
`scountovf` read 0 there and 0x80 here, which looked like a defect in
either until Spike's CSR mask was read. Each probe names Spike's ISA.

The ctest entries skip -- not pass -- when no model the probe names is
installed: a probe with nothing to compare against has said nothing.

## riscv-tests — 77/77

The older Berkeley suite: `rv32ui`, `rv32um`, `rv32ua`, `rv32uc` and `rv32mi`.
All of it passes, none skipped — `rv32mi/pmpaddr` once PMP was implemented,
and `rv32mi/breakpoint` once Sdtrig was.

**From 2026-09-16 to 2026-10-16, 64 of the 77 could not fail.** The
tests end with an `ecall` carrying the verdict, and the `rv32u*` ones make
it from U-mode. 1babe4e made the runner's exit call M-mode only, so a
Linux process's `write(2)` reaches its kernel -- and from then on those
`ecall`s trapped to the tests' own handler, which writes the verdict to
`tohost` and loops: the suite's own protocol, which this emulator does not
watch. The run went to the cap, a capped run exits 0, and a failing test
loops in the same place with a different value. With `ADD` broken, the
script reported `rv32ui` 42 of 42.

`run-riscv-tests.sh` reads the verdict where the test leaves it now: a run
at the cap must be parked in `write_tohost`, and `gp` is what it wrote --
1 for a pass, `(sub-test << 1) | 1` for a failure, the code the `ecall`
would have carried. Anywhere else at the cap is a hang. Against the same
broken `ADD`: 15 failures, `add` at sub-test 8 among them. 77 of 77
interpreted and under the JIT, and through `scripts/emu-board` over the
host's link.

It has no `rv32uf`, so it contributes nothing to FP coverage; that comes
entirely from arch-test's `F` family. Two of its tests are load-bearing in a
way the count hides: `rv32mi/csr` fails on purpose when the runner's `-march`
disagrees with what `misa` advertises, and `rv32mi/breakpoint` is the single
test standing between the default build and the 29% interpreter gain that
compiling Sdtrig out would give.

## Bugs these suites caught

Worth recording, because each was a genuine defect:

| Found by | Defect |
|---|---|
| unit test vs. assembler ground truth | `C.ADDI4SPN` took its destination register from bits `[9:7]` instead of `[4:2]`, corrupting every guest stack-frame address computation |
| `riscv-tests` `instret_overflow` | a CSR write to `minstret` must *replace* that instruction's increment, not be followed by it |
| `riscv-arch-test` `Zicntr` | the Sail config declared a clock tick every 100 instructions while the emulator ticks every instruction |
| `riscv-arch-test` `Zacas` | the Sail config declared `atomic_support: AMOArithmetic` on guest RAM, so the golden model **trapped** on `amocas` and baked trap-derived values into the signatures — three sessions were spent looking for an emulator bug that was never there |
| `riscv-tests` `rv32mi/csr` | the suite was built without F while `misa` advertised it. The test detects exactly that mismatch and fails on purpose; the emulator was correct and the runner's `-march` was not |
| hardware `isatest` | the JIT's inlined store wrote guest RAM without consulting PMP, so a protected region was writable under the JIT and not under the interpreter |
| `riscv-arch-test` `PMPSm`, under `--jit` | **the optimiser deleted a load nothing read.** A load whose destination was overwritten later in the block was never executed: no fault, and on a device no read -- `(void)UART->DR;` did nothing. Passed interpreted, failed translated; reproduced on the F746, where an APLIC claim discarded that way left the interrupt pending |
| `riscv-arch-test` `PMPSm` | `pmpaddr` stored 32 bits where a 32-bit bus allows 30, so it reported a boundary nothing could reach |
| `riscv-arch-test` `PMPF`, `PMPZca` | `fld`/`fsd` are two word accesses, each aligned, so a double at an address 4 mod 8 never raised address-misaligned on a core built to report it |
| `riscv-arch-test` `ExceptionsSvZalrsc` | a failing `sc.w` returned "reservation lost" for an address with no memory behind it, where any store reports an access fault |
| `riscv-arch-test` `SvZicbo`, `PMPZicbo` | cbo.clean, cbo.flush and cbo.inval took the guest's *virtual* address straight to the bus, and were not checked against PMP |
| guest `isatest`, written to hold the above | `menvcfg` and `senvcfg` were stored, read back, and consulted by nothing: every cache-block operation ran at every privilege |

### What the second backend found in the first runner

`rv32mi/scall` and `rv32ui/ma_data` hung under `--jit`, and the bug was
not in the translator: it was in the host runner's instruction cap, which
had been correct for as long as there was only one backend.

The runner sized each slice as `max_insn - total`. A backend may retire
*more* than the budget it was given — the JIT executes whole translated
blocks and can only stop between them, so the last one overshoots. `total`
then passes `max_insn`, the unsigned subtraction goes below zero into a
very large number, the budget never reaches zero, and the loop never ends.
The guest kept running perfectly; the cap simply stopped existing.

The interpreter retires one instruction at a time and lands exactly on the
cap, which is why this was invisible for the entire life of the project.
It is only reachable by the two tests that *rely* on the cap to terminate
— they spin in `write_tohost` by design — and only with a backend that
retires in blocks. The ARM firmware's loop is unaffected: it runs a fixed
slice with no cap, so there is no subtraction to underflow.

### What the Sv32 tests found

Declaring Sv32 built 37 more tests than S-mode alone, and getting them to
pass took two config fixes and three code fixes. The config half is worth
recording because both entries were *correct* before paging existed:

| `sail.json` field | was | why it had to move |
|---|---|---|
| `supports_pte_read` on guest RAM | `false` | a page table lives in ordinary RAM. With it false the golden model fails the PTE *read*, reports an access fault where the architecture calls for a page fault, and bakes that into the signature |
| `xtval_nonzero.*_page_fault` | `false` | the three page-fault causes report the faulting virtual address, and the UDB config already said so — the two models have to agree |

Both are the same shape as the `Zacas` row above: a value that described
"this cannot happen" outlived the thing that made it true.

The code half was three genuine gaps, none of them reachable before there
was a page table: non-leaf PTEs did not reject the reserved `D`/`A`/`U`
bits, a PMP-denied PTE read reported a *load* access fault rather than one
matching the access that caused the walk, and `menvcfg`/`menvcfgh` were
not implemented at all — so the framework's prolog took an
illegal-instruction trap the reference did not, and every later signature
disagreed about the privilege it was in.

The RVC expansion table in [`tests/unit/test_decode.c`](../tests/unit/test_decode.c)
is assembler-derived, not hand-computed: each entry was produced by assembling
the compressed form and its 32-bit equivalent and reading both back with
`objdump`.

---
