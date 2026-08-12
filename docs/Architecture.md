# Architecture

A retargetable 32-bit ISA emulator for ARM Cortex-M hosts. Three axes,
independent of each other:

| axis | what it decides | selected by | values |
|---|---|---|---|
| **platform** | where the emulator runs | `EMU_PLATFORM` | `host`, `stm32f446` |
| **frontend** | what it emulates | `EMU_FRONTEND_*` | `rv32`, `g4mh` |
| **backend** | how it executes | per frontend | interpreter, Thumb-2 JIT |

Any platform can host any frontend, and each frontend brings its own
backends. That is the whole point of the split: the bus, the passthrough
window onto real peripherals, the console, the ELF loader and both
platforms are written once and know nothing about which instruction set
the guest runs.

```
┌──────────────────────────────────────────────────────────┐
│  platform    host runner  |  STM32F446 firmware          │
├──────────────────────────────────────────────────────────┤
│  frontend    emu_cpu_ops_t ──▶ rv32 | g4mh               │
├──────────────────────────────────────────────────────────┤
│  backend     emu_backend_t ──▶ interpreter | Thumb-2 JIT │
├──────────────────────────────────────────────────────────┤
│  runtime     bus · regions · passthrough · devices · ELF │
└──────────────────────────────────────────────────────────┘
```

## Where things live

```
include/emu/      the frontend contract, and the ISA-agnostic runtime's API
include/rv32/     RISC-V frontend headers
include/g4mh/     RH850 G4MH frontend headers
src/emu/          bus, passthrough, NS16550 console, ELF loader, registry
src/frontend/
  rv32/           hart, decode, CSRs, traps, interpreter, Thumb-2 JIT,
                  CLINT, APLIC
  g4mh/           core, decode, interpreter, INTC1/INTC2
src/platform/
  host/           native runner (frontend-neutral)
  stm32f446/      Nucleo-F446RE firmware, ST HAL, linker script
```

Vendor reference PDFs are in `docs/arm/`, `docs/riscv/`, `docs/st/` and
`docs/renesas/`. The per-platform and per-frontend notes are in
`docs/<platform>/README.md` and `docs/<platform>/<frontend>/README.md`.

## The frontend contract

[`include/emu/emu_cpu.h`](../include/emu/emu_cpu.h) is the interface, and
the note at the top of it is the thing to read before adding a frontend or
a member. The rule it lives by:

> `run` executes a whole budget (4096 instructions) behind one indirect
> call, and every other hook is either setup or fires on a trap.

Nothing in that table may end up on a per-instruction path. One extra
*direct* branch on the fetch path measured 9.3% on CoreMark; an indirect
call there would cost more than the entire abstraction saves.

There is also no register-file layout in the contract. `rv_hart_t` keeps
`x[]` and `pc` at offset 0 because Thumb-2 encodes small displacements off
a base register in 16 bits, and a shared header prefix would push them out
of range. So `emu_cpu_t` is opaque, the frontend casts it back, and state
is reached through `reg_read`/`reg_write` when a platform genuinely needs
it.

### What a frontend provides

| group | members |
|---|---|
| identity | `name`, `desc`, `elf_machine` |
| lifecycle | `instance`, `init`, `reset`, `boot` |
| execution | `run`, `invalidate`, `step` |
| its own devices | `add_devices`, `set_irq`, `set_unmask_hook`, `advance_time`, `set_time` |
| platform services | `set_syscall`, `set_trace`, `set_cache`, `halt` |
| introspection | `status`, `nregs`, `reg_name`, `reg_read`, `reg_write`, `dump`, `disasm` |

Two seams are worth calling out because they are what keep the platforms
architecture-neutral:

- **`emu_syscall_t`** — the frontend unpacks its own calling convention
  (a7/a0–a3 on RISC-V, the `TRAP` vector and r6–r9 on G4MH) into an
  ABI-neutral struct, so the newlib `write`/`exit` pair both platforms need
  for their test harnesses is written once.
- **`dump`** — the frontend formats its own state, because only it knows
  what its registers are called and which status registers matter after a
  fault. This used to be duplicated in both platforms and kept in step by
  hand.

### Bus faults

`src/emu/` deals in regions, permissions and access widths. It has no idea
what an architecture calls the resulting fault, so an access reports an
`emu_fault_t` — which *kind* of access was refused — and the frontend maps
it (`rv_exc_from_fault`, `g4mh_exc_from_fault`). `EMU_FAULT_NONE` is 0 so
the hot path tests against zero.

## Adding a frontend

1. `include/<isa>/` — public headers, `<isa>_` prefixed
2. `src/frontend/<isa>/` — state, decoder, interpreter, its own devices
3. one `emu_cpu_ops_t`, declared and listed in `src/emu/emu_cpu.c`
4. `option(EMU_FRONTEND_<ISA> ...)` and a `target_sources` block in
   `CMakeLists.txt`
5. tests in `tests/unit/`, guarded by `EMU_FRONTEND_<ISA>`
6. notes in `docs/host/<isa>/` and `docs/stm32f446/<isa>/`

Nothing in `src/emu/` or `src/platform/` should need editing beyond step 3.
That is the property to check when the contract changes:

```sh
cmake -B build/g4mh -DRV32_PLATFORM=stm32f446 \
      -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake \
      -DEMU_FRONTEND_RV32=OFF -DEMU_FRONTEND_G4MH=ON
cmake --build build/g4mh
```

If that links, the seams hold.

## Building

```sh
# host: development and both RISC-V test suites
cmake -B build/host -DRV32_PLATFORM=host -DCMAKE_BUILD_TYPE=Release
cmake --build build/host && ctest --test-dir build/host -L fast

# both frontends, so the runner can pick with --frontend
cmake -B build/both -DRV32_PLATFORM=host -DEMU_FRONTEND_G4MH=ON

# firmware
cmake -B build/stm32f446 -DRV32_PLATFORM=stm32f446 \
      -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake \
      -DCMAKE_BUILD_TYPE=Release -DRV32_GUEST=isatest
cmake --build build/stm32f446 --target flash
```

`rv32-host` picks a frontend from `--frontend`, else from the image's ELF
`e_machine`, else the first compiled in. A flat binary says nothing about
its architecture, so it gets the default.

## To do

- **G4MH on the STM32 platform has never been run.** It links; no guest has
  executed on it, because there is no RH850 toolchain here.
- **A second host platform.** Everything is written for it, nothing proves
  it: the passthrough policy table and the NVIC bridge are the only
  STM32-specific parts, and both are tables.
- **Multi-core.** `instance(index)` takes an index and every frontend
  answers only 0. The RISC-V side has `hartid` plumbed through; the G4MH
  side would need INTC1 per PE (the U2B6 has three). Planned in
  [`host/g4mh/multicore.md`](host/g4mh/multicore.md), host platform only.

## Investigate

- **Whether the frontend contract costs anything measurable.** It should
  not — one indirect call per 4096 instructions — but it has never been
  A/B'd against the pre-split code.
- **A G4MH Thumb-2 JIT.** The emitter in `rv_jit_thumb2.c` is ~40%
  ISA-neutral (the Thumb-2 encoders); the translator is not. Whether
  splitting the emitter out is worth it depends on that second frontend
  actually needing speed.

## Discarded

- **A common struct prefix for `emu_cpu_t`.** It would push `x[]`/`pc` out
  of the Thumb-2 5-bit displacement range and cost an instruction on every
  guest register access. Opaque pointer plus a cast instead.
- **Per-instruction hooks in the contract.** See the note at the top of
  `emu_cpu.h`; this is the one rule the interface must not break.

## JIT backends

Three axes, three files, so a new one only writes the part that is new:

| | owns | files |
|---|---|---|
| framework | code buffer, block table, hash, flush, dispatch, stats | `emu/emu_jit.{h,c}` |
| host | how to spell an instruction, and the ABI | `emu/emu_x86_64.{h,c}` |
| frontend | which guest instructions translate, and to what | `frontend/<isa>/*_jit_*.c` |

A frontend supplies `emu_jit_ops_t`: `translate`, plus small accessors for
pc, state, counters and interrupts. Two of its fields carry the lessons
this project paid for:

- **`generation`** — everything the translator bakes into a block, as one
  value. The framework compares it every dispatch and flushes when it
  moves. Nearly every staleness bug found here was a block outliving
  something it was specialised on: a rounding mode resolved at
  translation, an FPU switched off afterwards, a PMP entry locked
  afterwards, a page table edited under a block keyed on virtual
  addresses. Returning a constant is only correct for a translator that
  reads nothing, and both current backends do return NULL — as a claim,
  not an omission.
- **`may_run`** — state the emitted code cannot check for itself, as
  opposed to state it bakes in. RV32 answers false whenever PMP, Sdtrig or
  paging is armed, because each needs a per-fetch check that translated
  code does not perform.

The framework also owns two things that were bugs before it existed: a
block retiring zero instructions still costs the budget one, so a trap on
a block's first instruction cannot spin the dispatch loop forever; and
`emu_jit_rewind` exists because a translator cannot always tell it must
decline before it has emitted part of an instruction.

### Coverage, not speed

`emu_jit_stats_t` is printed by the host whenever anything was translated,
for any frontend. Read `interp` against the retired count before believing
a passing suite: a backend that declines everything and falls back passes
every test while proving nothing, and that has happened here — the first
x86-64 run interpreted 92% of the self-test and looked perfectly healthy.

### What exists

| frontend | host | state |
|---|---|---|
| RV32 | Thumb-2 | its own file; predates the framework and keeps compaction, loop chaining and the PMP/passthrough specialisations the framework has no notion of |
| RV32 | x86-64 | on the framework |
| G4MH | x86-64 | on the framework |

The Thumb-2 backend is deliberately not ported. It is 3,700 lines whose
value is in exactly the specialisations the framework does not model, and
the only way to test it is to flash a board — a bad trade against a
refactor whose benefit is tidiness. The framework was shaped so it could
adopt it later.
