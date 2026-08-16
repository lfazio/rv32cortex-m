# Floating point in the JIT

What each backend emits, what it declines, and why the split is where it
is. The rule the rest of this project works to is in CLAUDE.md: *there is
one FP implementation and both backends reach it*. This page is the
exception to that rule, its boundary, and the evidence for where the
boundary sits.

## The rule, and why it exists

SoftFloat is the FP unit. Everything that rounds, classifies or reports a
flag goes to `rv_hart_fp`, from the interpreter and from the JIT alike.

That was not the first arrangement. The JIT once emitted host FP
instructions for the whole OP-FP group, which was a *second*
implementation of semantics the core already owned, and the two disagreed
exactly where the architecture is fussiest — NaN propagation, subnormals,
and which of `fflags` an operation may raise. Measured on the official
suite, `rv32i/F`, same guest binary: interpreter 78/78, JIT **55/78**.
Routing everything to the helper made it 78/78 both ways, and the JIT was
*still* ahead of the interpreter on FP work, because the block stays
whole and only the arithmetic becomes a call.

So the default answer to "should this go to the host FPU" is no, and the
burden of proof is on the exception.

## The exception: the four exact operations

Add, subtract, multiply, divide and square root are the operations IEEE
754 specifies **exactly** — one correctly rounded result, no
implementation freedom. A compliant host computes the same bits SoftFloat
does, bit for bit, for every finite input.

The 23 failures above were therefore never about *arithmetic*. They were
about everything around it, and there are exactly two things:

| | what differs | who owns it now |
|---|---|---|
| NaN | x86 propagates an operand's payload and quietens a signalling NaN; RISC-V has one NaN, `0x7FC00000`, always | `canonicalise_nan` after every arithmetic op |
| flags | MXCSR's order is IE,DE,ZE,OE,UE,PE; RISC-V's is NX,UF,OF,DZ,NV; and the host's are sticky across whatever ran before | MXCSR framed per block, mapped through `g_fe_map`, handed over once at the exit |

Both are now emitted rather than hoped for, which is what makes the
exception sound. Neither is expensive: the NaN check is a `ucomiss` and a
not-taken branch, and the MXCSR framing is per *block* and skipped
entirely for blocks with no float in them.

Everything else stays on the helper. `FMIN`/`FMAX` return the wrong
operand for a NaN on every host; the conversions need a per-host fixup;
`FCLASS` has no host instruction; the fused multiply-adds have three
operands where the IR has two. Those are where hosts genuinely disagree,
and a helper call is a translation — the block stays whole either way.

## The rounding mode is resolved at translation

`aux` carries an `EMU_IR_FRM_*` on every operation whose result depends
on one, and a guest's *dynamic* mode is resolved by the frontend when the
block is built. That is deliberate: it lets a backend decline a mode it
has no encoding for rather than silently rounding some other way. Neither
host has RISC-V's ties-away (`RMM`), and an earlier run-time table mapped
it to ties-even with nothing noticing.

A block is therefore *specialised* on `frm`, which is why `frm` is in
`rv_ir_gen_key` — see [`staleness.md`](staleness.md). The block also sets
the mode rather than inheriting it, or `emu_ir_can_lower`'s answer would
be a guess about the caller.

## NaN boxing

With D, FLEN is 64 and a single-precision value in an `f` register must
carry all-ones in its upper half; one that does not is not a single at
all and reads as the canonical NaN.

`EMU_IR_FGET`/`FPUT` move 32 bits, so for a while this meant every FP
load, store and move went to the helper — the alternative was writing an
unboxed register and reading one, and neither could tell. `EMU_IR_FP_BOX`
in `aux` is what brings them back: set on a get it checks the upper half,
set on a put it fills it.

**It is per instruction, not a property of the register file**, and the
asymmetry is the architecture's:

| | reads | writes |
|---|---|---|
| `FLW` | — | boxed |
| `FSW` | **raw** | — |
| `FMV.X.W` | **raw** | — |
| `FMV.W.X` | — | boxed |
| `fmul.s` | boxed | boxed |

A store and an `FMV.X.W` move *bits*; putting either through the
unboxing read would turn an unboxed register into a canonical NaN, and
"no interpretation" is the whole instruction. `rv_fpu.c` makes the same
distinction in the same places through `fr32`/`fw32`.

## What each backend answers

`emu_ir_can_lower(op, aux)` is the query, and a frontend that gets `false`
routes the operation to a helper rather than letting the block be
declined — declining costs the whole block, a helper call costs one call.

| | x86-64 | Thumb-2 |
|---|---|---|
| FGET, FPUT, FSGNJ | yes, boxed or raw | yes, boxed or raw |
| FADD/FSUB/FMUL/FDIV/FSQRT | RNE only | RNE only |
| FCMP | yes | no |
| FCVT_TO_I / FCVT_FROM_I | signed only | no |
| FMIN, FMAX, FCLASS | no | no |

Thumb-2 gets the NaN convention from `FPSCR.DN`, which makes ARM's
default NaN the canonical one for every NaN-producing operation — so it
needs no per-operation canonicalisation, only the right FPSCR. It also
clears `FZ`, because both guests define subnormals as ordinary values.

The x86-64 block does the same job with an explicit MXCSR constant,
`0x1F80`: sticky bits clear, RC nearest, FTZ and DAZ clear, masks set.
**Stated rather than derived.** Deriving it from the caller's MXCSR got
the first two right and left FTZ and DAZ to whatever the process happened
to be running under — which is `0x1F80` for a plain C program and is not
for one linked against a library that has set FTZ.

## Measured

Whetstone at `WHET_LOOPS=100`, best of twelve, three interleaved rounds:

| | host wall | step |
|---|---|---|
| everything on the helper | 120 ms | |
| FLW/FSW/FMV lowered | 110 ms | −8% |
| `fmul.s` as well | 94 ms | −15% |
| `fadd.s` as well | 87 ms | −8.5% |

CoreMark unchanged at 8 ms and Dhrystone reporting 2128.3 either way is
what says the framing is not paid by blocks with no float. `fptest` is
too short to resolve.

Against the same source compiled natively for x86-64 the JIT is 130×,
but most of that gap is newlib's software transcendentals against
glibc's on an ISA that has `sqrtss` — see
[`../performance.md`](../performance.md) for why that number is not an
emulation-overhead figure. The one that is: **125 million guest
instructions a second, 99.5% of them translated.**

**Which of the frontend's instructions are lowered is a two-entry table
in `rv_ir.c`, and it is meant to stay that way.** FSUB.S is funct7 0x04
and FDIV.S is 0x0C, and neither is in it. Adding one is a line; adding
one without measuring is how this got to 55/78 the first time. **One at
a time, each against the F suite, each timed** — not the whole table on
the argument that the host has an FPU.

**Under about 10%, an A/B needs interleaved rounds.** Best of five had
Dhrystone — which contains no floating point at all — moving 77 ms to
71 ms between two binaries differing only in FP lowering. Fifteen
samples in three rounds gives 72-74 for both, and the guest-side figure
is 2128.3 to the digit. Layout noise on this host is real; CLAUDE.md
records ±3% here and 10% on the board.

## Two things about validating this

**`xlat` and `interp` cannot see the difference.** A helper call is a
translation and so is a native lowering, so both arms of the A/B report
the same counts — `xlat 4229 interp 2558` to the digit either way, which
reads exactly like "the code never ran". It took a direct counter in the
emission path to say 210 `fmul.s` were being lowered. A perfect null
result is the loudest signal in this project, and the first thing to
check when you get one is whether the *instrument* can represent what you
are looking for.

**The suite passes either way until the awkward input is in it.**
Removing the canonicalisation fails exactly the tests for the operations
that are lowered and nothing else — five `F-fmul.s` when only `fmul.s`
was lowered, then ten when `fadd.s` joined it, the five `F-fadd.s` being
the new ones. That the failure set grows by exactly the operation added
is worth more than the pass: it says the canonicalisation is reached on
the new path and not merely present. Dropping the box fails 87.

All of it confirmed by reverting, which is the only thing that says a
test covers what it claims.
