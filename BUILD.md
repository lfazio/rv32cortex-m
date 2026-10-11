# Build gates

Every compile-time switch, what it selects, and which of them can be
combined. The recipes themselves are in [README.md](README.md); this file
is about the *gates* — what exists, what is derived from what, and which
combinations are checked.

**`scripts/build-matrix.sh` builds every combination that is supposed to
work.** Run it before believing a change is portable. It exists because
two configurations had stopped compiling and nothing said so, which is the
fourth time this project has found a capability broken by being behind a
flag nobody set.

```sh
scripts/build-matrix.sh --list     # the table, and what each row covers
scripts/build-matrix.sh            # build all of it
scripts/build-matrix.sh --test     # and run ctest where there is one
scripts/build-matrix.sh host       # just the host rows
scripts/build-matrix.sh f746-g4    # one row
```

## The three axes

They are independent, and that is the point of the layout: `src/emu/` has
no platform *and no ISA* in it, so a frontend and a platform meet only at
`emu_cpu_ops_t`.

| axis | question | option |
|---|---|---|
| platform | where the emulator runs | `EMU_PLATFORM=host\|stm32f446\|stm32f746\|stm32n6` |
| guest | which ISA it emulates | `EMU_GUEST_ARCH_{RV32,G4MH,PPC,ARMV7M}` |
| backend | how it executes | `EMU_JIT=ON\|OFF`, and `--jit` on the host runner |

More than one guest may be on at once; the host runner picks with
`--frontend` or from the ELF header. Firmware normally builds exactly
one, because a frontend costs flash only when it is on.

## The JIT gates, and why there is only one answer

This is the part that went wrong, so it is written out in full. There are
**two questions and one derived answer**:

| macro | set by | means |
|---|---|---|
| `EMU_JIT` | CMake option | was a JIT *asked for* |
| `EMU_JIT_REQUESTED` | CMake, from `EMU_JIT` | the same, visible to the preprocessor |
| `EMU_HOST_JIT_X86_64` | `emu_jit.h`, from the compiler **and** the request | this build can emit x86-64 |
| `EMU_HOST_JIT_THUMB2` | `emu_jit.h`, likewise | this build can emit Thumb-2 |
| `EMU_HAVE_JIT` | `emu_jit.h` | **is there a JIT** — the one thing to test |

**Test `EMU_HAVE_JIT`.** The host macros exist to select *which* emitter
compiles; nothing else should be spelling out the disjunction.

There used to be five more names — `RV_ENABLE_JIT`, `RV_JIT_X86_64`,
`RV_JIT_THUMB2`, `EMU_IR_JIT_ON_THUMB2` and `G4MH_HAVE_JIT` — each defined
in a different header from a slightly different premise. Two of them were keyed on the
*host alone*, ignoring the request, so `-DEMU_JIT=OFF` still declared a
backend that nothing compiled. `f746-rv32-nojit` and `host-nojit` both
failed to build, for months, because nobody built them.

What a JIT-off build must not contain follows from that, and CMake now
enforces it: each frontend's `<isa>_ir.c` is its IR *translator* and
calls `emu_ir_can_lower`, which only a backend lowering file defines — so
all four are listed under `if(EMU_JIT)`.

**Building a configuration is not running it.** The firmware passed
`--jit` to its own argument parser unconditionally, and that parser
refuses `--jit` in a build without one: every `-DEMU_JIT=OFF` image
built, linked, and hard-faulted before its console existed. The matrix
row for that configuration had been green since it was added.

## Per guest

### RV32 — `EMU_GUEST_ARCH_RV32`

The reference frontend: three external models disagree with it
(riscv-arch-test, the Berkeley suite, Sail).

| gate | default | notes |
|---|---|---|
| `RV32_EXT_M/A/C/F/D` | ON | `D` requires `F`; `F` forces `Zcf` |
| `RV32_EXT_ZBA/ZBB/ZBC/ZBS` | ON | bit manipulation |
| `RV32_EXT_ZCB` | ON | in the *emulator* it is a small win; in guest **codegen** it costs ~9% |
| `RV_EXT_ZICOND`, `RV_EXT_ZIHPM`, `RV_EXT_ZALASR`, `RV_EXT_ZAWRS`, `RV_EXT_ZACAS` | 1 | **header gates, not CMake options** -- `include/rv32/rv_config.h`; override with a compile definition. Zalasr and Zawrs follow `A`, Zihpm follows Zicntr, and each `#error`s without it. Zihintpause and Zihintntl have no gate: there is nothing to switch off |
| `RV32_EXT_PMP`, `RV32_EXT_SDTRIG` | ON | both are free until a guest arms them, and both are on the fetch path — measure the interpreter after touching either |
| `RV32_ENABLE_DISASM` | | the disassembler is **not** a decoder; it lags |
| `RV32_INTERP_IN_RAM` | OFF | measured *slower* on the F446 |
| `RV32_LAZY_IRQ` | ON | |
| `EMU_PAIR_STATS` | OFF | measurement scaffolding: adjacent executed pairs, any frontend |

### G4MH — `EMU_GUEST_ARCH_G4MH`

**No reference model.** Hand-written unit tests, CC-RH as a second
*encoder*, and a compiled guest. Nothing will tell you an *answer* is
wrong.

| gate | default | notes |
|---|---|---|
| `G4MH_EXT_FPU` | | single and double, on SoftFloat |
| `G4MH_EXT_MPU` | ON | not an option a real part omits |
| `G4MH_MPU_ENTRIES` | 32 | the out-of-range guard is **unreachable at 32**: MPIDX is five bits. Build 8 to exercise it |
| `G4MH_PE_COUNT` | 1 | `> 1` was untested for years and the first run found three defects |
| `G4MH_CRAM_KIB`, `G4MH_LRAM_KIB` | 128, 64 | **per PE for LRAM.** See below |

**The RAM arithmetic is what breaks the firmware.** 128 KiB of cluster
RAM plus 64 KiB of local RAM *per PE* is 320 KiB at three PEs — the
F746's entire memory, with nothing left for the emulator. The link fails
with `cannot move location counter backwards`, which names the symptom
and not the cause. The 3-PE matrix row therefore carries
`-DG4MH_CRAM_KIB=64 -DG4MH_LRAM_KIB=16`.

`EMU_MAX_REGIONS` is computed from `G4MH_PE_COUNT` — it must be, and a
design note asking a maintainer to raise it by hand is a constant that
does not get raised.

### PowerPC e200z7 — `EMU_GUEST_ARCH_PPC`

**Big-endian**, the only one here. See the note by `EMU_BUS_ORDER` in
`emu_bus.h` for which region kinds that does *not* apply to.
**No hardware reference**: binutils, a Python model and exact rational
arithmetic stand in for one ([docs/frontend/ppc.md](docs/frontend/ppc.md)).

| gate | default | notes |
|---|---|---|
| `PPC_GUEST` | `alu` | the guest a firmware embeds: `alu`, `sys`, `coremark`, `crypto`. Embedded as an ELF, because the encoding (VLE or Book E) is a flag on its segment |
| `PPC_ENABLE_DISASM` | ON | checked against binutils, both directions |

Needs SoftFloat, as every frontend with an FP unit does. Blocks are
capped at 16 guest instructions when the host is Thumb-2 and 64
otherwise; that is in `ppc_ir.c`, with the measurements.

### ARMv7-M — `EMU_GUEST_ARCH_ARMV7M`

A Cortex-M guest on a Cortex-M host, **checked against a Cortex-M7**:
`tests/armv7m-diff/` runs the same programs on the Nucleo-F746ZG and on
the emulator.

| gate | default | notes |
|---|---|---|
| `ARMV7M_ENABLE_DISASM` | ON | tested by assembling what it prints |
| `ARMV7M_COREMARK_ITERATIONS` | 40 | its own count, not `COREMARK_ITERATIONS` |

It has not been built into firmware and flashed.

## Per platform

| platform | needs | notes |
|---|---|---|
| `host` | — | x86-64 Linux; the JIT emits x86-64 |
| `stm32f446` | `-DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake` | Cortex-M4: **no caches**, no DWT software lock |
| `stm32f746` | the same | Cortex-M7: caches and a DWT lock, both of which the JIT depends on |
| `stm32n6` | the same | Cortex-M55, Armv8.1-M: no internal flash, and megabytes of guest RAM |

The toolchain file is not optional for any firmware platform, and
CMake's error for its absence names the option rather than the cause — so
a first configure without it leaves a *poisoned* build directory that
keeps failing after the option is added. Delete the directory.

Whether the part has caches cannot be derived — both `-mcpu=cortex-m4`
and `-mcpu=cortex-m7` define `__ARM_ARCH_7EM__` — so it is not a flag at
all: `board_sync_icache` is weak and a no-op, and the F746 overrides it
with the real clean-to-PoU and I-cache invalidate. There *was* a flag,
`RV_ARM_HAS_CACHES`, and it outlived the code it gated: the hand-written
Thumb-2 backend that read it was removed, its replacement shipped with no
cache maintenance at all, and the flag stayed defined and unread. Grep
for the readers of a flag, not for the flag.

## Shared

| gate | default | notes |
|---|---|---|
| `EMU_ENABLE_TRACE` | OFF | per-instruction hook. **Read pc deltas, not the disassembly** |
| `EMU_ENABLE_STATS` | ON | `xlat`/`interp` — read the ratio before believing a pass |
| `EMU_JIT_CODE_BYTES` | 32768 | the code cache on a microcontroller. A threshold, not a dial: below a guest's translated working set the JIT is **slower** than the interpreter, and at the default that includes CoreMark — [docs/jit/tuning.md](docs/jit/tuning.md) |
| `EMU_JIT_LOOP_CAP` | 128 | interrupt-latency knob. CoreMark **cannot observe it** |
| `EMU_JIT_DIFF` | OFF | checks each block against the IR interpreter — and see its `diff_declined` counter before trusting silence |
| `EMU_NET` | ON on the F746 | lwIP over PPP or SLIP: telnet, gdb, TFTP. **The UART stops being a console** |
| `EMU_NATIVE_COREMARK` | OFF | CoreMark natively on the ARM, for the baseline |

**Four JIT options are gone**: `EMU_JIT_LOOP_CHAIN`,
`EMU_JIT_INLINE_PERIPH`, `EMU_JIT_ELIDE_LD` and `EMU_JIT_ELIDE_ST`. They
configured the hand-written Thumb-2 translator, and for as long as the IR
backends have existed they were declared, forwarded and defaulted — and
read by nothing. Setting one changed no byte of the build. The audit that
found them is one loop: for every option, grep for a reader of the macro
it becomes.

## Cache variables outlive the tree

`EMU_JIT_CODE_BYTES`, `RV_GUEST_MARCH` and `COREMARK_ITERATIONS` are all
cache variables, and every performance figure in this repository was once
measured with a 48 KB code cache inherited from an old build directory
while the declared default was 12 KB. `rm -rf build/` changed the numbers
by 68% with no code change. **Before quoting a measurement, check
`CMakeCache.txt` for what actually built it.**
