# The e200z7 frontend

Power ISA 2.06 embedded (Book E) with VLE — the core in the MPC57xx
family, and this project's first big-endian guest.

```sh
cmake -B build/ppc -DEMU_PLATFORM=host \
      -DEMU_FRONTEND_RV32=OFF -DEMU_FRONTEND_PPC=ON
cmake --build build/ppc
./build/ppc/emu-host --load 0x80000000 \
    build/ppc/tests/guest/ppc/isatest.bin
```

## What it decodes

**VLE, and that is now the default.** VLE and classic Book E are
different encodings of the *same bytes* — `0x48000009` is `bl` in Book E
and a 16-bit `se_` form followed by something else in VLE — so the mode
has to be chosen before the first fetch.

A real e200 chooses per page, from the VLE attribute in the TLB entry.
Until there is a TLB it is core-wide, and it is **on**: the e200z7 in an
MPC57xx executes VLE and every guest here is built `-mvle`. It used to
default to Book E, which meant the entire 16-bit half of the interpreter
was unreachable by any real guest — see the lessons below.

`tests/unit/test_ppc.c` drives both, and each of its two harnesses states
its mode explicitly rather than one relying on the default.

Implemented: the D-forms, the X-form pool (arithmetic including the
carrying forms, multiply, divide, the logical set, the register and
immediate shifts, sign extension, `cntlzw`, `mfcr`/`mtcrf`, `mfspr`/
`mtspr`, `mfmsr`/`mtmsr`), the VLE 16-bit `se_` forms, the `e_` forms
with 16- and 20-bit immediates, SCI8, the branches including the counter
forms, `sc`, the exception model with IVPR/IVOR vectoring and SRR/CSRR
save-restore, and the time base, decrementer and external input.

Not implemented, and raising a program interrupt rather than pretending:
the TLB and per-page VLE selection, SPE (MSR[SPE] is clear out of reset,
so the correct report for a unit this frontend does not have), the
floating-point unit, and cache management beyond what the bus needs.

## The guest

`tests/guest/ppc/isatest.S` — 29 checks, exit status is the failure
count, `PPC-ISATEST-END` on completion. It is what task #36's remaining
work should be measured against, because until it existed **this
frontend had never executed a program**.

**Assembly, not C, and not by choice.** Debian's `powerpc-linux-gnu-gcc`
is not built with VLE support — `-mvle` is rejected outright — while the
assembler and objdump handle VLE fine. So the compiled-guest arrangement
that found three G4MH defects in a row is unavailable here, and this is
the closest substitute: the encodings come from **binutils**, an encoder
that is not this emulator's decoder, but the *choice* of what to write
is still the author's. It catches encoding and semantic errors; it does
not catch "the form a compiler actually emits".

That gap is the one worth closing next. An NXP S32DS or CodeWarrior VLE
compiler would do for PowerPC what CC-RH did for G4MH.

### It carries a vector table, and that is the point

With IVPR and the IVORs at zero, an unimplemented encoding vectors to
address 0 — which in a flat image is the guest's own entry point. The
program silently restarts and presents as a hang. This is the defect
that cost G4MH three sessions, restated in CLAUDE.md, and it was live
here too: all three decode gaps found while writing this guest showed up
as "runs to the instruction cap with no output", each needing a trace
build and a manual bisection.

The guest now points IVOR2/3/5/6 at a handler that prints the faulting
pc and exits 99:

```
TRAP at pc=800006fc
```

which `objdump -d -Mvle` turns into an instruction in one step. Removing
`mullw` from the interpreter reproduces exactly that line, at exactly
that instruction.

## Lessons, and how each was found

Every one of these was found by the *first program to run*, and none by
the 353 unit checks that were passing throughout.

**A mode flag whose only writer is a unit test is not a mode flag.**
`vle` defaulted to false and the sole assignment in the tree was
`tests/unit/test_ppc.c` reaching into the struct. Every `se_` test
passed; none of them could have run outside a unit test, and no guest
had any way to turn VLE on. A capability nobody can exercise is not a
capability — this project's own rule, in a new place.

**Enumerate the slot, against the assembler.** Two groups had holes, and
in both the missing entries sat *between* implemented neighbours:

| slot | implemented | missing |
|---|---|---|
| opcode 28, XO | `e_cmp16i`, `e_cmpl16i`, `e_or2i`, `e_and2i.`, `e_or2is`, `e_lis`, `e_and2is.` | `e_add2i.`, `e_add2is`, `e_mull2i`, `e_cmph16i`, `e_cmphl16i` |
| 16-bit, `w>>9` | `se_addi`, `se_subi`, `se_bclri`, `se_bseti`, `se_btsti`, `se_srwi`, `se_slwi` | `se_cmpli`, `se_bgeni`, `se_srawi` |

`e_add2i.` is XO **0x11**, not the 0x10 the group's start suggests, so a
guessed base would have shifted every entry by one. Both tables came
from assembling the whole group and reading the encodings back.

**A two-bit field read as one bit aliases half the instructions onto the
other half.** `e_bc`'s BO32 selects condition-true, condition-false,
`bdnz` and `bdz`. Only its low bit was read, so `e_bdnz` decoded as
`e_bge` — branch while CR0[LT] is clear, which is always — and a counted
loop never terminated. Not a declined encoding and not a trap: a silent
wrong answer, and the first loop anyone wrote found it.

**The whole multiply and divide family was absent.** A compiler emits
`mullw` for `a * b` and there is no avoiding it; the X-form pool had 12
of ~30 common instructions.

**`--dump` prints nothing for this frontend**, because `emu_cpu_ops_t`'s
`dump` is not filled in. Still open, and worth fixing: it is the
introspection a divergence hunt reaches for first.

**An expectation written from the shape of a number will disagree with a
correct implementation.** One of the 29 checks failed after everything
else was right: `0xFFFFFF9C / 7` is `0x24924916`, and the test said
`0x24924924` — a plausible repeating pattern, guessed rather than
computed. The emulator was right and the test was wrong.

**A test declared is not a test that can run.** `ctest` in a PowerPC-only
tree queued `arch-test-I`, `guest-isatest` and `guest-isatest-jit` — all
RISC-V — because they were gated on the guest *image* existing rather
than on the RV32 frontend being compiled in. The images are built
whenever the RISC-V toolchain is present, which is independent of the
frontend. With no `--max-insn` the RV32 image ran as VLE until something
killed it. Found by running `ctest` in a configuration nobody had run it
in.
