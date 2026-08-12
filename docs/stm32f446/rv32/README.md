# stm32f446 / rv32

RV32IMAFC + Zba/Zbb/Zbc/Zbs + Zicbom/Zicboz + Sdtrig + PMP + U-mode, on the
Nucleo-F446RE, with the Thumb-2 JIT. This is the pair everything else in
the project is measured against.

Devices this frontend adds to the guest map
([`include/rv32/rv_memmap.h`](../../../include/rv32/rv_memmap.h)):

| guest address | what |
|---|---|
| `0x0200_0000` | ACLINT MSWI (msip) |
| `0x0200_4000` | ACLINT MTIMER (mtimecmp, mtime) — occupies the legacy CLINT window |
| `0x0C00_0000` | APLIC, direct delivery, 128 sources |

An APLIC source number is the NVIC line number; see the platform page.

## The Thumb-2 JIT

RV32 basic blocks are translated into Thumb-2 held in a RAM code cache,
eliminating the per-instruction costs the interpreter cannot avoid: the bus
call to fetch, RVC expansion, the dispatch switch, the pc write and the
counter update.

```
r4        hart pointer (callee-saved, survives helper calls)
r0-r3     scratch, and the argument registers for helper calls
lr        pushed in the prologue, popped into pc at the exit
```

Three choices worth stating:

- **The guest register file stays in memory.** `hart->x` is at offset 0, so
  each access is a single 16-bit `LDR/STR Rt,[r4,#n]` — and guest state is
  coherent at every instruction boundary, so a trap, an interrupt or a
  debugger read needs no unwinding.
- **Blocks end at every control transfer**, so interrupt latency is bounded
  by one block rather than by a chain.
- **Anything not translated is run by the interpreter.** The JIT is a fast
  path over it, not a replacement, so correctness never depends on coverage.

## Validation

The host suites **cannot** exercise any of this. The check is flashing
`isatest` (243 checks) and reading the UART:

```sh
cmake -B build/stm32f446 -DRV32_PLATFORM=stm32f446 \
      -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake \
      -DCMAKE_BUILD_TYPE=Release -DRV32_GUEST=isatest
cmake --build build/stm32f446 --target flash
```

This has caught real bugs, including an inlined store that skipped the
LR/SC reservation break. Build `-DRV32_JIT=OFF` to run the same guest on
the interpreter — with the JIT on, FP arithmetic goes to VFP and never
reaches SoftFloat, so running `isatest` both ways is a differential check
between two genuinely different FP implementations.

## Things that have bitten

- **Every translate-time read of mutable hart state is a staleness bug
  until proven otherwise.** `frm`, `mstatus.FS` and the PMP bounds are all
  baked into translated code and all flushed from `jit_note_csr` on the
  interpreter fallback, which is the single place any of them can move
  (the translator declines `SYSTEM`). Bus regions and guest instruction
  bytes are safe. All three defects were invisible to both host suites.
- **Watching a flag is not watching the configuration.** The PMP flush
  compared `pmp_active`, but what a block bakes in is the *bounds*.
  Snapshot what was baked, not what enabled it.
- **A translate-time legality check is only half a guard; the block
  outlives it.** A block built while `mstatus.FS` was on keeps executing
  after the guest turns the FPU off.
- **A register that does not fit a Thumb-2 encoding assembles as a
  different instruction, not an error.** `emit_dp_reg(DP_CMP, R8, R1)`
  became `CMP r0, r1`, so `RV_JIT_LOOP_CAP` never applied. Use
  `emit_cmp_hi` for r8 and above.
- **Branch range is a silent cliff.** Loop chaining was emitted only when
  the back edge fitted the 16-bit conditional branch; `emit_bcond_back`
  picks the encoding by reach.
- **An A/B that only half-reverts the fix reads as a passing test.**
  Disable *every* site, and confirm the failure names the mechanism.
- **What you decline costs more than what you translate badly.** Ending a
  block for an untranslatable instruction fragments hot code; route it
  through `jit_helper_alu` instead. `FMIN`/`FMAX`/`FCLASS` have no ARMv7-M
  equivalent but routing them to `rv_hart_fp` keeps the block whole.

## Tuning, measured

| knob | finding |
|---|---|
| `EMU_JIT_CODE_BYTES` | dominates everything; see the platform page |
| `RV_JIT_LOOP_CAP` | 128 is the knee. 64/128/256: CoreMark 31.39/31.16/31.25 (noise), bench 18.88/18.39/18.13, mmiobench 24.40/23.46/22.99. Each doubling returns half the previous and doubles worst-case latency (~11/22/44 µs). Do not tune on CoreMark alone. |
| `EMU_JIT_INLINE_PERIPH` | 2.2–3.1× to drivers, −53% to compute if always on; armed after 64 passthrough accesses instead |

## To do

- **Fix the translation-time cost of the elision experiment** below, then
  re-measure at 12 KB. It is the only configuration where 1.3% less code
  could matter.
- **`emit_cbz_skip4` and `chain_target` are compiled but never called.**
  Either use them or delete them.
- **Zacas.** `amocas.d` operates on even-odd register pairs and its
  targeted checks read the low half back in the high half's register;
  whether the fault is the pair handling or the test's asm constraints is
  not established.

## Investigate

- **Fusing `LUI`+`ADDI`.** Measured at 0.2% of CoreMark pairs and 0.00%
  for `AUIPC`+`ADDI`, so almost certainly not worth it — but that was one
  guest built one way. A guest with large constants or position-independent
  code would look different.
- **Indexed addressing.** `LDR Rt,[Rn,Rm,LSL #n]` would serve address
  generation feeding a load; measured 0.0–2.9% of pairs.
- **Widening the block.** `RV_JIT_MAX_BLOCK_INSNS` is 64 and blocks average
  4.12 guest instructions, so the limit is control flow, not the cap.

## Discarded

- **A guest-register cache in r8-r10: 15.5% slower.** Reads per block said
  it should win. It did not: a cached read is `MOV` where an uncached one
  is `LDR` — one instruction either way — while write-through adds an
  instruction per write and three more registers hit every PUSH/POP.
- **Eliding the register-file round trip** (`EMU_JIT_ELIDE_LD`,
  `EMU_JIT_ELIDE_ST`, both default **off**, code retained). 24–33% of
  adjacent executed pairs are data dependent (measured with
  `RV32_PAIR_STATS`), and each emits `STR` then an immediate `LDR` of the
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
