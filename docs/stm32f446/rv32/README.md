# stm32f446 / rv32

RV32 on the Nucleo-F446RE, with the Thumb-2 JIT: the pair the project
began with, and the one most of its early lessons were learned on.

**Most of what is measured on this page is history.** It was taken on
this part under the hand-written Thumb-2 translator, which the shared IR
backends replaced. The lessons hold; the figures, the function names and
several of the options do not, and each is marked. Current notes:
[../../frontend/rv32.md](../../frontend/rv32.md),
[../../backend/thumb2.md](../../backend/thumb2.md),
[../../jit/tuning.md](../../jit/tuning.md).

Devices this frontend adds to the guest map
([`include/rv32/rv_memmap.h`](../../../include/rv32/rv_memmap.h)):

| guest address | what |
|---|---|
| `0x0200_0000` | ACLINT MSWI (msip) |
| `0x0200_4000` | ACLINT MTIMER (mtimecmp, mtime) — occupies the legacy CLINT window |
| `0x0C00_0000` | APLIC, direct delivery, 128 sources |

An APLIC source number is the NVIC line number; see the platform page.

## Validation

The host suites cannot exercise the Thumb-2 lowering. The check is
flashing `isatest` and reading the UART:

```sh
cmake -B build/stm32f446 -DEMU_PLATFORM=stm32f446 \
      -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake \
      -DCMAKE_BUILD_TYPE=Release -DRV32_GUEST=isatest
cmake --build build/stm32f446 --target flash
```

This has caught real bugs, including an inlined store that skipped the
LR/SC reservation break. Build `-DEMU_JIT=OFF` to run the same guest on
the interpreter.

`isatest` is a correctness check and a poor stress test: it never fills
the code cache and finishes in a fraction of a second, so it cannot meet
a defect in eviction or one that needs twenty seconds to appear. Both
kinds have existed. Follow it with CoreMark at enough iterations to do
both.

## Things that have bitten

Each of these was found here. The names in them are the hand-written
translator's and no longer exist; the rule is what survives.

- **Every translate-time read of mutable hart state is a staleness bug
  until proven otherwise.** `frm`, `mstatus.FS` and the PMP bounds were
  all baked into translated code. All three defects were invisible to
  both host suites. See [../../jit/staleness.md](../../jit/staleness.md)
  for where each lives now.
- **Watching a flag is not watching the configuration.** The PMP flush
  compared `pmp_active`, but what a block bakes in is the *bounds*.
  Snapshot what was baked, not what enabled it.
- **A translate-time legality check is only half a guard; the block
  outlives it.** A block built while `mstatus.FS` was on keeps executing
  after the guest turns the FPU off.
- **A register that does not fit a Thumb-2 encoding assembles as a
  different instruction, not an error.** A 16-bit `CMP` handed r8 became
  `CMP r0, r1`, so the loop cap never applied.
- **Branch range is a silent cliff.** Loop chaining was emitted only when
  the back edge fitted the 16-bit conditional branch.
- **An A/B that only half-reverts the fix reads as a passing test.**
  Disable *every* site, and confirm the failure names the mechanism.
- **What you decline costs more than what you translate badly.** Ending a
  block for an untranslatable instruction fragments hot code; route it
  through a helper call instead.

## Tuning, as measured then

| knob | finding |
|---|---|
| the code cache | dominated everything; see the platform page |
| the loop cap (`EMU_JIT_LOOP_CAP` today) | 128 is the knee. 64/128/256: CoreMark 31.39/31.16/31.25 (noise), bench 18.88/18.39/18.13, mmiobench 24.40/23.46/22.99. Each doubling returns half the previous and doubles worst-case latency. Do not tune on CoreMark alone. |
| inlining the peripheral window | 2.2–3.1× to drivers, −53% to compute if always on; armed after 64 passthrough accesses instead. **Not in the IR backends**, which inline guest RAM only |

## To do

- **Zacas.** `amocas.d` operates on even-odd register pairs and its
  targeted checks read the low half back in the high half's register;
  whether the fault is the pair handling or the test's asm constraints is
  not established.
- **Run this board again.** Nothing here has been re-measured on the F446
  since the IR backends replaced the translator these notes describe.

## Investigate

- **Fusing `LUI`+`ADDI`.** Measured at 0.2% of CoreMark pairs and 0.00%
  for `AUIPC`+`ADDI`, so almost certainly not worth it — but that was one
  guest built one way. A guest with large constants or position-independent
  code would look different.
- **Indexed addressing.** `LDR Rt,[Rn,Rm,LSL #n]` would serve address
  generation feeding a load; measured 0.0–2.9% of pairs.
- **Widening the block.** Blocks average 4.12 guest instructions against
  a cap of 64, so the limit is control flow, not the cap.

## Discarded

All measured on the hand-written translator.

- **A guest-register cache in r8-r10: 15.5% slower.** Reads per block said
  it should win. It did not: a cached read is `MOV` where an uncached one
  is `LDR` — one instruction either way — while write-through adds an
  instruction per write and three more registers hit every PUSH/POP.
- **Eliding the register-file round trip**, as an option on that
  translator (the IR backends do a one-instruction version of it
  unconditionally, and the options are gone). 24–33% of
  adjacent executed pairs are data dependent (measured with
  `EMU_PAIR_STATS`), and each emits `STR` then an immediate `LDR` of the
  same slot. Removing them works and is correct — 243/243 on hardware,
  10,708 loads and 7,714 stores removed on `bench` — and buys nothing:

  | `bench`, 48 KB cache | cycles/insn | KIPS | code |
  |---|---|---|---|
  | off | 18.32 | 9824 | 48028 |
  | on | 18.29 | 9837 | 47408 |

  0.16% apart, inside the ±3% noise, for 1.3% less code. At the 12 KB
  default it is far *worse* — 112.70 with the load elision, 127.16 with
  both, against 104.01 — because that configuration re-translates 4671
  times with 855 compactions and pays the added bookkeeping on every one.
  Same lesson as the register cache: at ~18 host cycles per guest
  instruction, removing one host instruction from a subset of them is not
  where the time goes.
- **A last-block cache in front of the hash lookup: 1.2% slower.** Block
  dispatch is already a shift, mask, load and compare.
- **Moving the PMP/frm/FS checks into the dispatch loop.** CoreMark enters
  blocks 2.9M times a run; the interpreter fallback is where the check is
  both cheap and correct.
