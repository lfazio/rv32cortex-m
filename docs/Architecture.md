# Architecture

A retargetable 32-bit ISA emulator for ARM Cortex-M hosts. Three axes,
independent of each other:

| axis | what it decides | selected by | values |
|---|---|---|---|
| **platform** | where the emulator runs | `EMU_PLATFORM` | `host`, `stm32f446`, `stm32f746`, `stm32n6` |
| **frontend** | what it emulates | `EMU_GUEST_ARCH_*` | `RV32`, `G4MH`, `PPC`, `ARMV7M` |
| **backend** | how it executes | `EMU_JIT`, and `--jit` on the host | interpreter; JIT through the shared IR |

Any platform can host any frontend, and every frontend has both
backends. That is the whole point of the split: the bus, the passthrough
window onto real peripherals, the console, the ELF loader, the IR and
its two emitters, and every platform are written once and know nothing
about which instruction set the guest runs.

```
┌──────────────────────────────────────────────────────────────────────┐
│  platform    host runner | STM32F446 | STM32F746 | STM32N6           │
├──────────────────────────────────────────────────────────────────────┤
│  frontend    emu_cpu_ops_t ──▶ rv32 | g4mh | ppc | armv7m            │
├──────────────────────────────────────────────────────────────────────┤
│  backend     emu_backend_t ──▶ interpreter                           │
│                              | translator ─▶ IR ─▶ Thumb-2 | x86-64  │
├──────────────────────────────────────────────────────────────────────┤
│  runtime     bus · regions · passthrough · devices · ELF · gdb stub  │
└──────────────────────────────────────────────────────────────────────┘
```

## Where things live

```
include/emu/      the frontend contract, and the ISA-agnostic runtime's API
include/<isa>/    one frontend's public headers: rv32, g4mh, ppc, armv7m
src/emu/          bus, passthrough, NS16550 console, framebuffer and input,
                  ELF loader, gdb stub, the frontend registry -- and the
                  IR (emu_ir.c), the JIT framework (emu_jit.c) and the
                  glue between them (emu_ir_jit.c)
  virtio/         the virtio transport and TinyEMU's devices behind it
src/backend/
  thumb2/         IR -> ARMv7E-M
  x86_64/         IR -> x86-64
  common/         an IR interpreter, the differential checker's reference
src/frontend/
  rv32/           hart, CSRs, PMP, Sv32, Sdtrig, decoder, interpreter,
                  IR translator, FP unit, CLINT, APLIC
  g4mh/           core, decoder, interpreter, IR translator, FP unit, MPU,
                  INTC, the timers, inter-PE interrupts and barriers
  ppc/            core, decoder, disassembler, interpreter, IR translator,
                  the embedded FP unit
  armv7m/         core, decoder, disassembler, interpreter, IR translator,
                  exceptions, NVIC and SysTick, FP unit
  softfloat/      Berkeley SoftFloat, which is every frontend's FP unit
src/platform/
  common/         the session: argv, images, the run loop, reporting
  host/           native runner, SDL display, perf counters
  stm32/          what the three boards share
  stm32f446/ stm32f746/ stm32n6/   clocks, linker scripts, vendor glue
src/net/          lwIP over SLIP or PPP: telnet, gdb, TFTP
tests/
  unit/           host unit tests, per layer and per frontend
  guest/          programs that run inside the emulator, per frontend
  arch-test/      the DUT description for the official RISC-V suite
  armv7m-diff/    the same programs on a Cortex-M7 and on the emulator
  ppc-check/      the PowerPC decoder against binutils, its semantics
                  against a Python model, its FP unit against rationals
  tools/          disassemblers and the FP units as commands
third_party/      TinyEMU's virtio devices, vendored unmodified
```

Vendor reference PDFs are under `docs/arm/`, `docs/riscv/`, `docs/st/`,
`docs/renesas/` and `docs/nxp/`. Per-platform and per-frontend notes are
in `docs/<platform>/` and `docs/frontend/`.

## The frontend contract

[`include/emu/emu_cpu.h`](../include/emu/emu_cpu.h) is the interface, and
the note at the top of it is the thing to read before adding a frontend or
a member. The rule it lives by:

> `run` executes a whole budget (4096 instructions) behind one indirect
> call, and every other hook is either setup or fires on a trap.

Nothing in that table may end up on a per-instruction path. One extra
*direct* branch on the fetch path measured 9.3% on CoreMark; an indirect
call there would cost more than the entire abstraction saves.

There is also no register-file layout in the contract. `emu_cpu_t` is
opaque, the frontend casts it back, and state is reached through
`reg_read`/`reg_write` when a platform genuinely needs it. The JIT learns
where a frontend's registers, pc and flags live from `emu_ir_target_t`,
stated once by the frontend.

### What a frontend provides

| group | members |
|---|---|
| identity | `name`, `desc`, `elf_machine`, `ncores` |
| lifecycle | `instance`, `init`, `reset`, `boot`, `set_image` |
| execution | `run`, `step`, `halt`, `invalidate`, `select_backend` |
| its own devices | `add_shared_devices`, `add_core_devices`, `set_irq`, `set_unmask_hook`, `advance_time`, `set_time` |
| platform services | `set_syscall`, `set_trace`, `set_cache` |
| introspection | `status`, `nregs`, `reg_name`, `reg_read`, `reg_write`, `dump`, `report`, `disasm`, `gdb_target` |

Three seams are worth calling out because they are what keep the
platforms architecture-neutral:

- **`emu_syscall_t`** — the frontend unpacks its own calling convention
  (a7/a0–a3 on RISC-V, the `TRAP` vector and r6–r9 on G4MH, `sc` with the
  number in r0 on PowerPC) into an ABI-neutral struct, so the
  `write`/`exit` pair every platform needs for its test harness is
  written once.
- **`dump`** — the frontend formats its own state, because only it knows
  what its registers are called and which status registers matter after a
  fault.
- **`boot`** — what the loader knows about the image arrives in
  `emu_boot_info_t`, including the image itself. `set_image` is called on
  a *reload* and never on first start, so a frontend that has to inspect
  the image — PowerPC chooses its instruction encoding from an ELF
  segment flag — does it here.

### Bus faults

`src/emu/` deals in regions, permissions and access widths. It has no idea
what an architecture calls the resulting fault, so an access reports an
`emu_fault_t` — which *kind* of access was refused — and the frontend maps
it. `EMU_FAULT_NONE` is 0 so the hot path tests against zero.

Byte order is the bus's too: a region is bytes, and a big-endian guest
states its order once ([memory.md](memory.md)).

## Backends

Each frontend has an interpreter and a translator. The translator emits
the shared IR and stops there; `src/backend/` turns IR into Thumb-2 or
x86-64 without knowing the guest, and `src/emu/emu_jit.c` owns the code
cache, block table, chaining, compaction and dispatch for all of them.

| frontend | interpreter | translator | runs under the JIT on |
|---|---|---|---|
| RV32 | `rv_interp.c` | `rv_ir.c` | x86-64, Thumb-2 (hardware) |
| G4MH | `g4mh_interp.c` | `g4mh_ir.c` | x86-64, Thumb-2 (hardware) |
| PowerPC | `ppc_interp.c` | `ppc_ir.c` | x86-64, Thumb-2 (hardware) |
| ARMv7-M | `armv7m_interp.c` | `armv7m_ir.c` | x86-64 |

The x86-64 backend exists for **coverage, not speed**: it is what lets
the suites run against translated code on a host, since the Thumb-2
lowering cannot be exercised by any host suite. The framework, the block
model and what a block may assume are in [jit/README.md](jit/README.md).

Whether there is a JIT at all is `EMU_HAVE_JIT`, derived in
`emu/emu_jit.h` from the request and the host — the one macro to test.
See [../BUILD.md](../BUILD.md).

## Adding a frontend

1. `include/<isa>/` — public headers, `<isa>_` prefixed
2. `src/frontend/<isa>/` — state, decoder, interpreter, its own devices
3. one `emu_cpu_ops_t`, declared and listed in `src/emu/emu_cpu.c`
4. `option(EMU_GUEST_ARCH_<ISA> ...)` and a `target_sources` block in
   `CMakeLists.txt`
5. tests in `tests/unit/`, guarded by `EMU_GUEST_ARCH_<ISA>`
6. for a JIT: `<isa>_ir.c`, an `emu_ir_frontend_t`, and one line in
   `src/emu/emu_ir_jit.c`
7. notes in `docs/frontend/<isa>.md`, and a row in
   `scripts/build-matrix.sh`

Nothing in `src/emu/` or `src/platform/` should need editing beyond
steps 3 and 6. That is the property to check when the contract changes,
and the build matrix checks it for every frontend:

```sh
scripts/build-matrix.sh f746-g4
scripts/build-matrix.sh f746-ppc
```

If those link, the seams hold. **A frontend that allocates its own
memory map works on a host and cannot be ported** — G4MH once modelled
3.44 MiB of flash and RAM as `.bss` on a part with 320 KiB — and nothing
says so until someone builds the firmware. See [memory.md](memory.md).

Two things every frontend here learned separately, and the fourth one
learned again:

- **Give the first guest a vector table that reports.** An unimplemented
  encoding raises an exception, and in a flat guest with no handlers the
  vector is ordinary code or the guest's own entry point: the program
  restarts or runs on, and presents as a hang or as wrong arithmetic.
- **Run a real program before believing a coverage claim.** Unit tests
  exercise the encodings someone thought to write. The first real
  programs found three defects in G4MH, three in ARMv7-M and four in
  PowerPC, each in a frontend whose unit tests were passing.

## Building

Recipes are in [../README.md](../README.md); every gate and which
combinations are checked are in [../BUILD.md](../BUILD.md).

`emu-host` picks a frontend from `--frontend`, else from the image's ELF
`e_machine`, else the first compiled in. A flat binary says nothing about
its architecture, so it gets the default.

## To do

- **Multi-core beyond G4MH.** `ncores` and `instance(index)` are in the
  contract and G4MH brings up to three PEs
  ([`host/g4mh/multicore.md`](host/g4mh/multicore.md)); every other
  frontend answers one.
- **The ARMv7-M frontend has not run on a board**, under either backend.

## Investigate

- **Whether the frontend contract costs anything measurable.** It should
  not — one indirect call per 4096 instructions — but it has never been
  A/B'd against the pre-split code.

## Discarded

- **A common struct prefix for `emu_cpu_t`.** It would cost every
  frontend its own layout for the sake of a platform that should not be
  looking. Opaque pointer plus a cast instead.
- **Per-instruction hooks in the contract.** See the note at the top of
  `emu_cpu.h`; this is the one rule the interface must not break.
- **A translator per host.** RV32 had a hand-written Thumb-2 translator
  of 3,700 lines before the IR existed, kept beside the IR path for a
  while. Maintaining two translators for one host is how a fix lands in
  the one nobody is running; it was deleted, and four build options went
  on configuring it long after it had gone.
