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
| platform | where the emulator runs | `EMU_PLATFORM=host\|stm32f446\|stm32f746` |
| guest | which ISA it emulates | `EMU_GUEST_ARCH_{RV32,G4MH,PPC}` |
| backend | how it executes | `EMU_JIT=ON\|OFF` |

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
enforces it: `rv_ir.c` and `g4mh_ir.c` are the IR *translators* and call
`emu_ir_can_lower`, which only a backend lowering file defines — so they
are listed under `if(EMU_JIT)`.

## Per guest

### RV32 — `EMU_GUEST_ARCH_RV32`

The reference frontend: three external models disagree with it
(riscv-arch-test, the Berkeley suite, Sail).

| gate | default | notes |
|---|---|---|
| `RV32_EXT_M/A/C/F/D` | ON | `D` requires `F`; `F` forces `Zcf` |
| `RV32_EXT_ZBA/ZBB/ZBC/ZBS` | ON | bit manipulation |
| `RV32_EXT_ZCB` | ON | in the *emulator* it is a small win; in guest **codegen** it costs ~9% |
| `RV32_EXT_PMP`, `RV32_EXT_SDTRIG` | ON | both are free until a guest arms them, and both are on the fetch path — measure the interpreter after touching either |
| `RV32_ENABLE_DISASM` | | the disassembler is **not** a decoder; it lags |
| `RV32_INTERP_IN_RAM` | OFF | measured *slower* on the F446 |
| `RV32_LAZY_IRQ` | ON | |
| `RV32_PAIR_STATS` | OFF | measurement scaffolding: adjacent executed pairs |

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

## Per platform

| platform | needs | notes |
|---|---|---|
| `host` | — | x86-64 Linux; the JIT emits x86-64 |
| `stm32f446` | `-DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake` | Cortex-M4: **no caches**, no DWT software lock |
| `stm32f746` | the same | Cortex-M7: caches and a DWT lock, both of which the JIT depends on |

The toolchain file is not optional for either firmware platform, and
CMake's error for its absence names the option rather than the cause — so
a first configure without it leaves a *poisoned* build directory that
keeps failing after the option is added. Delete the directory.

`RV_ARM_HAS_CACHES` is set by the platform and cannot be derived: both
`-mcpu=cortex-m4` and `-mcpu=cortex-m7` define `__ARM_ARCH_7EM__`.

## Shared

| gate | default | notes |
|---|---|---|
| `EMU_ENABLE_TRACE` | OFF | per-instruction hook. **Read pc deltas, not the disassembly** |
| `EMU_ENABLE_STATS` | ON | `xlat`/`interp` — read the ratio before believing a pass |
| `EMU_JIT_CODE_BYTES` | 12288 | dominates JIT performance; at the default the JIT can *lose* to the interpreter |
| `EMU_JIT_LOOP_CAP` | 128 | interrupt-latency knob. CoreMark **cannot observe it** |
| `EMU_JIT_DIFF` | OFF | checks each block against the IR interpreter — and see its `diff_declined` counter before trusting silence |
| `EMU_NET` | | lwIP, SLIP, TFTP on the F746 |

## Cache variables outlive the tree

`EMU_JIT_CODE_BYTES`, `RV_GUEST_MARCH` and `COREMARK_ITERATIONS` are all
cache variables, and every performance figure in this repository was once
measured with a 48 KB code cache inherited from an old build directory
while the declared default was 12 KB. `rm -rf build/` changed the numbers
by 68% with no code change. **Before quoting a measurement, check
`CMakeCache.txt` for what actually built it.**
