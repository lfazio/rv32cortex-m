# Platform: host

The same core the firmware runs, on a development machine. This is where
the instruction-level tests run: iterating here is far faster than
reflashing, and any divergence between host and target is a bug in the
platform layer, not the frontend.

Nothing in `src/platform/host/main.c` names a guest architecture. It builds
a bus, opens a core through `emu_cpu_ops_t`, and runs it.

## Guest memory map

The same map as the firmware, so guest images are portable between the two:

| guest address | kind | what |
|---|---|---|
| `0x1000_0000` | MMIO | NS16550 console, onto stdout |
| `0x4000_0000` | RAM | the peripheral window, backed by plain memory |
| `0x8000_0000` | RAM | guest RAM, `--ram` bytes, default 1 MiB |

The host has no ARM peripherals to pass through to, so the peripheral
window is ordinary memory. Guest drivers still run; they just talk to
nothing. Sized to `0x24000` so it reaches RCC at `0x4002_3800` — a driver's
first act is to ungate its own clock, and a window that stops short of RCC
faults on the first store every real guest driver makes.

## Choosing a frontend

```
--frontend NAME     explicit
(else)              from the image's ELF e_machine
(else)              the first frontend compiled in
```

A flat binary says nothing about its architecture, so it gets the default.

## Time

Guest time advances with executed instructions rather than with a wall
clock: there is nothing to track, and a deterministic time base makes runs
reproducible. One tick per instruction matches the rate the cycle counter
advances at, which is what the architecture suite's Sail config declares.
`--timer-hz` changes the divisor.

## System calls

A subset of the newlib ABI — `write` (64) and `exit` (93) — which is what a
bare-metal cross-gcc's crt0 and the standard test harnesses emit. The
frontend unpacks its own calling convention into `emu_syscall_t`, so the
handler is written once and serves any frontend. Anything else falls
through to the architectural trap, so guest software with its own handler
keeps working.

## What the host cannot test

**The JIT.** `RV_ENABLE_JIT` is forced to 0 off an ARMv7-M Thumb-2 host,
because the backend emits ARM machine code and calls it — on x86 that is
not merely useless but fatal. Every JIT change has to be validated by
flashing. See [`docs/stm32f446/rv32/README.md`](../stm32f446/rv32/README.md).

## To do

- **Interactive input.** `host_rx` always returns -1; there is no way to
  type at a guest.
- **A debug monitor.** `emu_cpu_ops_t` has `step`, `reg_read`/`reg_write`
  and `dump` specifically so one could be written, and none exists.
- **GDB stub.** Same reason, larger job.

## Investigate

- **Running the RISC-V suites against a second frontend.** There is no
  equivalent suite for G4MH, which is the single biggest gap in trusting
  that frontend.

## Discarded

- **Building the ELF loader into the firmware.** Parsing program headers on
  a part with 512 KiB of flash buys nothing a linker script has not already
  done. The firmware carries a flat binary.
