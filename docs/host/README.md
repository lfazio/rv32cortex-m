# Platform: host

The same core the firmware runs, on a development machine. This is where
the instruction-level tests run: iterating here is far faster than
reflashing, and any divergence between host and target is a bug in the
platform layer, not the frontend.

Nothing in `src/platform/host/` names a guest architecture. The session
— argv, the image, the run loop, the report — is
`src/platform/common/`, shared with the firmware; what is here is the
board the host pretends to be.

## Guest memory map

The same map as the firmware, so guest images are portable between the two:

| guest address | kind | what |
|---|---|---|
| `0x1000_0000` | MMIO | NS16550 console, onto stdout — and from stdin |
| `0x2000_0000` | ROM | the image, where a guest linked for flash expects it |
| `0x3000_0000` | MMIO, RAM | framebuffer, keyboard and mouse; an SDL window with `-DEMU_SDL=ON` |
| `0x4000_0000` | RAM | the peripheral window, backed by plain memory |
| `0x8000_0000` | RAM | guest RAM, `--ram` bytes, default 1 MiB |

The host has no ARM peripherals to pass through to, so the peripheral
window is ordinary memory. Guest drivers still run; they just talk to
nothing. Sized to `0x24000` so it reaches RCC at `0x4002_3800` — a driver's
first act is to ungate its own clock, and a window that stops short of RCC
faults on the first store every real guest driver makes.

virtio-mmio devices — block, network, console, input, 9p — are added on
request from `0x1000_1000` upwards; see the README.

## Choosing a frontend

```
--frontend NAME     explicit
(else)              from the image's ELF e_machine
(else)              the first frontend compiled in
```

A flat binary says nothing about its architecture, so it gets the
default. An ELF says more than its machine: PowerPC takes its instruction
encoding from a segment flag, which is why its compiled guests are
loaded as ELF.

## Choosing a backend

`--jit` selects the translating backend, for any frontend; without it,
the interpreter. Here the JIT emits x86-64, and it exists for coverage
rather than speed ([../backend/x86_64.md](../backend/x86_64.md)): it is
what lets the suites and `ctest` run every translator, the IR, the
passes and the framework on a host.

The report at the end of a run says what was translated, entered,
interpreted, declined and overflowed. Read it before believing a pass.

## Time

Guest time is the host's monotonic clock, in microseconds, handed to the
frontend once per slice. It used to be a count of retired instructions,
which is reproducible and wrong for anything that measures time: a Linux
kernel booted with timestamps past 1,500 seconds during driver init and
its watchdogs fired on a machine running perfectly. `--timer-hz` divides
the clock, which is how a guest is deliberately given a slower one — DOOM
wants it.

So a benchmark that times itself here measures this machine, and two
runs of it differ. For a reproducible figure count instructions
(`retired` in the report), and time a run from outside to compare
backends.

A frontend may have a clock of its own that this does not drive: the
PowerPC time base counts instructions unless the guest selects the
platform clock, which is why its guests print the same tick count on
every run.

## System calls

A subset of the newlib ABI — `write` (64) and `exit` (93) — which is what a
bare-metal cross-gcc's crt0 and the standard test harnesses emit. The
frontend unpacks its own calling convention into `emu_syscall_t`, so the
handler is written once and serves any frontend. Anything else falls
through to the architectural trap, so guest software with its own handler
keeps working — and so does a guest with its own *kernel*: the hook
answers only from the most privileged mode, because a call from below
belongs to whatever the guest installed.

## Debugging

`--gdb [port]` serves the guest over RSP, `--dump` prints the register
file on exit, and `-DEMU_ENABLE_TRACE=ON` adds `--trace-skip` and
`--trace-count`. See [../gdb.md](../gdb.md). Read pc deltas in a trace,
not the disassembly: an instruction that changes the pc without retiring
is a trap.

## What the host cannot test

**The Thumb-2 lowering.** Every translator and everything above the
emitter runs here; what turns IR into ARM instructions compiles only for
ARM. Its *encoders* are checked on the host against the assembler, and
executed inside the ARMv7-M frontend, but a decision about which register
or which branch is validated by flashing — see
[../backend/thumb2.md](../backend/thumb2.md).

**A small code cache.** The host's JIT tables are sized so that nothing
is ever evicted, which is right for finding translator bugs and means
the eviction, compaction and overflow paths do not run. A defect in
exactly that path was live on the board for three weeks. A runner can be
built with the microcontroller's sizes, and
`test_overflow_compaction_unlinks` reaches the path with a stub
translator in every build.

**A long run.** The firmware's clock is a 32-bit cycle counter that
wraps in seconds, and for a long time nothing read it correctly. This
runner had the same defect at 71.6 minutes and nobody waited that long.

## Investigate

- **A reference for G4MH and PowerPC.** RV32 has three and ARMv7-M has
  a board; the other two are held against encoders and models, which
  cannot catch a shared misreading of the manual.

## Discarded

- **A second translator per host.** See
  [../Architecture.md](../Architecture.md).
