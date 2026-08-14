# Multicore G4MH

**Status: phases 1–3 implemented and tested; phase 0 partly done.** The
atomics, the system layer, three cores, per-core buses and INTC routing are
in and pass 212 checks at `-DG4MH_PE_COUNT=3`. What is missing from phase 0
is the guest toolchain and the compiled-code instructions
(`PREPARE`/`DISPOSE`, `LD.BU`/`LD.HU`, `CALLT`), so the only multicore
programs that exist are hand-assembled.

Run it with:

```sh
cmake -B build/mc -DEMU_PLATFORM=host -DEMU_FRONTEND_G4MH=ON \
      -DG4MH_PE_COUNT=3
cmake --build build/mc && ./build/mc/tests/unit/rv32-unit
./build/mc/rv32-host --frontend g4mh --cores 3 --quantum 8 <image>
```

The design below is what was built, kept as written because the reasoning
is the part worth preserving.

---


The RH850/U2B6 has three G4MH cores, PE0 to PE2. This is a plan to model
them, and an argument for one execution model over the alternatives.

**Host platform only.** Each PE has its own local RAM — 64 KiB on the part
— so three cores need ~192 KiB of local RAM plus shared RAM before any
emulator state, against 128 KiB of SRAM total on the F446. On a PC it is
nothing. The firmware keeps the single-core path, and the core count
becomes a build knob (`G4MH_PE_COUNT`, 3 on host, 1 on the target) rather
than a property of the frontend.

## Execution model

### The recommendation: one quantum-parameterised round-robin, yielding on synchronisation

```
for (;;) {
    for (pe = 0; pe < n; pe++) {
        if (idle(pe) && nothing pending for pe) continue;
        run(pe, quantum, &did);
    }
    advance shared time once per round, not once per core;
    if (every pe halted) break;
}
```

Three properties make this the right starting point, and they are worth
stating because each rules out an alternative:

**It is deterministic.** The same run twice produces the same interleaving,
so a failure is reproducible. That matters more here than anywhere else in
this project: there is no G4MH reference model and no toolchain, so the
only way to trust a result is to be able to re-run it and bisect it. Every
threaded design gives this up.

**One mechanism covers both speed and race hunting.** `quantum = 1` is
instruction-interleaved lockstep — the finest interleaving there is, and
the mode that actually finds guest races. `quantum = 1024` is fast. Same
scheduler, one parameter. A test suite that passes at 1, 16 and 1024 is
saying something real; one that changes behaviour with the quantum has
found either a guest race (good) or an emulator bug (bad).

**It maps onto the existing seam with no hot-path cost.** `run(cpu, budget,
retired)` already *is* a quantum. The contract's rule — one indirect call
per budget, nothing per instruction — is unchanged. Scheduling happens
between budgets, where it is free.

### The refinement that makes it work: yield on failed synchronisation

Plain round-robin livelocks under spin-waits. Core A takes a lock, core B
spins on it, and B burns its entire quantum spinning on a value only A can
change. At `quantum = 1024` that is 1024 wasted instructions per round, and
a guest with a tight producer/consumer handshake crawls.

The fix is cheap and is the one piece of real design here: **a core that
fails a synchronisation primitive has told you it is waiting, so let it
say so.** Add a run reason:

```c
EMU_RUN_YIELD,   /* made no progress waiting on another core */
```

returned when `STC.W` fails its reservation, when `CAXI` compares unequal,
or on an explicit `SNOOZE`. The scheduler moves on immediately. This costs
nothing when cores are not contending and turns the pathological case into
the good one.

### Alternatives, and why not

| model | why not |
|---|---|
| **Instruction-interleaved lockstep only** | it is `quantum = 1`; no reason to build it separately, and as the *only* mode it pays 3× dispatch overhead forever and forecloses a G4MH JIT |
| **One host thread per core** | needs a real memory model. The guest's stores must become visible to other cores in an order its barriers can constrain; emulating an RH850 (weak) on x86 (TSO) makes the emulator *stronger* than the target and hides guest bugs. And it discards determinism, which is the one thing making the missing reference model survivable. There is also no forcing function: three interpreted cores on a PC are fast enough. |
| **Event-driven with a global timeline** | the right answer if cores ran at different clocks or the model had to be cycle-accurate. Neither is true here, and it costs a priority queue on every scheduling decision. |

Nothing below precludes threads later. Keeping all state per-core, with
sharing only through explicitly shared bus regions, means the move is
adding locks around the shared devices rather than a rewrite. That is a
design constraint, not an accident — see "Keep the door open".

## What has to change

### 1. The SELF alias — the interesting problem

`0xFFFC_0000` (INTC1 SELF) and the local-RAM SELF window mean **the same
guest address resolves to different memory depending on which core is
executing**. The bus is currently one flat region table with no notion of a
current core.

| option | hot-path cost | survives threading |
|---|---|---|
| **(a) one bus per core**, shared regions pointing at the same backing memory, SELF regions pointing at that core's | **none** | yes |
| (b) one bus, indirection on SELF regions | a load or branch, but only on SELF accesses | yes |
| (c) one bus, re-point SELF regions on context switch | none | **no** — mutable global state |

**Take (a).** It costs nothing on the access path, and as a side effect
each core gets its own fast-path caches — `fetch_base/span/host` and
`data_base/span/host` in `emu_bus_t` are per-bus mutable state, so three
cores sharing one bus would thrash each other's caches on every switch.
With (a) that problem never exists.

Construction is a shared region list plus a per-core overlay, not three
hand-written maps. Watch `EMU_MAX_REGIONS`: per core that is ram, rom,
periph, uart, intc1-self, intc1-pe0..2, intc2, ostm, lram-self, lram-pe0..2
— about 14 against a limit of 16. Raise it to 24.

### 2. Contract changes

Small, and mostly already anticipated:

- `emu_cpu_ops_t` gains `unsigned ncores` so a platform can discover the
  count. `instance(index)` already takes the index and already returns NULL
  past the end.
- `add_devices(cpu, bus)` splits. Per-core devices (INTC1, local RAM) are
  added to that core's bus; shared devices (INTC2, OSTM, the console, the
  passthrough window) are added once to all of them.
  → `add_core_devices(cpu, bus, index)` and `add_shared_devices(bus)`.
- `set_irq` and `advance_time` move from the core to the system. An
  interrupt arrives at a *channel*; which PE takes it is INTC2's `EIBD`
  decision, not the platform's. Time is one clock, and advancing it per
  core would run it 3× fast.
- New `emu_system_t` holding the cores, their buses and the scheduler.
  `emu_core_t` stays as the per-core binding.

### 3. Atomics — the actual blocker

**Multicore without atomics is not useful**, and G4MH's are all in the
"not implemented" list: `CAXI`, `LDL.W`/`STC.W`, `SYNCM`/`SYNCP`/`SYNCE`.

The good news is that under sequential round-robin they are *easy*. Only
one core runs at a time and nothing preempts inside an instruction, so
`CAXI` is trivially atomic. `LDL.W`/`STC.W` needs a reservation per core
and a rule that any core's store to the reserved line clears every other
core's reservation.

That last part lands on the store path, which is where this project has
learned to be careful. The RV32 side already pays a single-core version
(`h->resv_valid && (addr & ~3u) == h->resv_addr`); the N-core version is N
checks. Do it the way `fetch_guard` and `pmp_active` are done: **one flag
saying any reservation is outstanding anywhere**, tested first, with the
walk behind it. Guests that never take a reservation pay one predictable
branch.

### 4. Interrupt routing

`EIBD` is stored today and does not route. It needs to:

- `g4mh_intc_pending(ic, pe)` — highest-priority pending channel bound to
  that PE, rather than the current global answer.
- INTC1 channels 0–31 are already per-PE by construction.
- Broadcast (the U2Bx forwards an INTC2 `EIINTn` to `EIINT4-7` of every
  PE's INTC1) is a phase-2 item; note it exists so the model is not
  designed in a way that excludes it.

### 5. Determinism as a tested property

Worth an explicit test rather than an assumption: run the same image twice
and assert identical per-core retired counts. It is two lines and it is the
guard on everything else.

## Phasing

**Phase 0 — prerequisites.** None of this is worth starting first.
- A G4MH toolchain. Still the blocker for any real guest.
- `PREPARE`/`DISPOSE`, `LD.BU`/`LD.HU`, `CALLT` — compiled code does not
  run without them.
- `CAXI`, `LDL.W`/`STC.W`, `SYNC*` — multicore is meaningless without them.

**Phase 1 — plumbing, still single core. ✅ done.**
`emu_system_t`, `ncores`, per-core bus construction, the `add_devices`
split into `add_shared_devices`/`add_core_devices`, system-level irq and
time.
*Validated as claimed:* rv32 keeps 178/230 and 77/77, both firmware configs
still link, and the g4mh checks went 191 → 196 only because the contract
test now asserts the core count.

**Phase 2 — three cores, round-robin. ✅ done.**
`instance(0..2)`, per-PE INTC1 with the SELF alias resolving per core,
`emu_system_step` as the scheduler, `--cores` and `--quantum` on the host
runner.
*Validated:* `test_mc_dispatch` — each core reads its PE number from
`HTCFG0`, writes a distinguishable value to its own slot and halts, and all
three land. `test_mc_quantum_invariance` runs the same program at quantum
1, 8 and 1024 and asserts identical results *and* identical total retired
counts, which is the determinism guarantee made testable.

Local RAM with SELF aliasing is **not** done — the per-core bus that would
carry it is, so it is a region to add rather than a design change.

**Phase 3 — synchronisation. ✅ done.**
`CAXI`, `LDL.W`/`STC.W`, `CLL`, the `SYNC*` barriers, the cross-core
reservation tracker, `EMU_RUN_YIELD`, and `EIBD.PEID` routing so an INTC2
channel goes to the PE it is bound to.
*Validated:* `test_mc_reservation` — core 0 takes a reservation, `SNOOZE`s
so core 1 runs, core 1 stores to the word, and core 0's `STC.W` must then
fail. Paired with `test_mc_reservation_succeeds`, which runs the same
sequence with nothing to interfere and asserts it *does* store — without
that pair, an implementation whose `STC.W` never works would pass.

Confirmed by breaking it: with the cross-core clear disabled, the test
fails naming the mechanism — core 0's `STC.W` reports success (1, not 0)
and its 7 overwrites core 1's 9.

A full spinlock passing a token PE0 → PE1 → PE2 → PE0 is still to write.

**Phase 4 — not done, and deliberately.**
Threads. Do not start this without a workload that is actually too slow.

## Keep the door open

Three constraints that cost nothing now and are expensive to retrofit:

1. **No global mutable scheduler state.** Everything per-core, sharing only
   through bus regions that are explicitly shared.
2. **The reservation table is the only cross-core mutable structure.** If a
   second one appears, that is the moment to reconsider threading, because
   it is the moment locking stops being trivial.
3. **The frontend contract stays budget-granular.** If anything in
   `emu_cpu_ops_t` ever needs calling per instruction to make multicore
   work, the design has gone wrong — see the note at the top of
   `emu_cpu.h`.

## What this does not solve

- **Memory ordering.** Sequential execution gives the guest a total order,
  which is *stronger* than the RH850 guarantees. Guest code with missing
  barriers will work here and fail on silicon. That is a known and accepted
  limitation of the model, not an oversight — and it is the single best
  argument for eventually doing threads, if the goal ever becomes finding
  real concurrency bugs rather than running real software.
- **Timing.** Round-robin gives no relationship between a core's progress
  and wall-clock or cycle counts. Anything timing-dependent in the guest is
  not being modelled.
