#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""rawgen.py - encodings, rather than instructions, for the board diff.

gen.py writes mnemonics and lets the assembler encode them, so everything
it produces is an instruction: it can say whether the frontend executes
an instruction correctly, and nothing at all about the rest of the
encoding space. That rest is most of it -- 150 million of the 402 million
32-bit encodings are UNDEFINED -- and it is where the frontend's two
decoders were found to disagree, and where both were found wrong
together.

So this writes `.inst.w` and asks the board. Two sources of encodings:

  random     uniform over the 32-bit space
  mutants    an instruction the assembler really produced, with one bit
             flipped -- which is how to land on a should-be-zero bit, the
             neighbouring row of a decode table, or a register number one
             past the end of the bank, none of which a uniform draw finds
             in any useful number

**An arbitrary encoding cannot simply be executed.** It might be a store
through a random register, a branch, a write to CONTROL or a WFI that
never wakes. So each candidate is put through the frontend's own decoder
first -- `armv7m-dis`, as text -- and is kept only if what the decoder
believes makes it safe to run:

  undecoded          expected to fault and do nothing
  register only      every operand in r0-r12 or the FP bank
  load or store      through a base register this case points at
                     diff_mem, immediate offset, never the base itself

That makes the decoder a filter here and not the thing under test: the
answer on every line is still the board's. Where the decoder is wrong
about an encoding being safe, the board says so by faulting somewhere
other than the probe, and the harness reports the address.

    rawgen.py --seed N --count N --dis armv7m-dis --from a.elf --out DIR

`--enc f3af8000,e8510f00:r1` adds named encodings without the filter --
for a question the filter would refuse to ask, such as what a hint or an
exclusive load does with a should-be bit wrong. `:rN` points that
register at diff_mem first. Each is repeated a few times, because a case
gets its registers from its index and one run of an encoding shows one
set of operands. The filter is what keeps a run from wandering off, so
whoever names an encoding is vouching for it.
"""

import argparse
import importlib.util
import os
import random
import re
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))


def load(name):
    spec = importlib.util.spec_from_file_location(
        name.replace("-", "_"), os.path.join(HERE, name + ".py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


gen = load("gen")
dcheck = load("disasm-check")

# Operations that only move values between registers. Everything else --
# branches, system instructions, hints that wait, anything with a list --
# is left out by not being named.
REG_ONLY = re.compile(
    r"^(and|bic|orr|orn|eor|add|adc|sbc|sub|rsb|mov|mvn|tst|teq|cmn|cmp|"
    r"lsl|lsr|asr|ror|rrx|movw|movt|addw|subw|"
    r"mul|mla|mls|[su]mull|[su]mlal|umaal|[su]div|"
    r"smla[bt][bt]|smul[bt][bt]|smlad|smladx|smuad|smuadx|smlaw[bt]|smulw[bt]|"
    r"smlsd|smlsdx|smusd|smusdx|smmla|smmlar|smmul|smmulr|smmls|smmlsr|"
    r"usad8|usada8|smlal[bt][bt]|smlald|smlaldx|smlsld|smlsldx|"
    r"[su]sat|[su]sat16|qadd|qdadd|qsub|qdsub|sbfx|ubfx|bfi|bfc|"
    r"[su]xt[bh]|[su]xtb16|[su]xta[bh]|[su]xtab16|"
    r"rev|rev16|revsh|rbit|clz|sel|pkhbt|pkhtb|"
    r"(s|q|sh|u|uq|uh)(add8|add16|asx|sub8|sub16|sax)|"
    r"v(add|sub|mul|div|nmul|mla|mls|nmla|nmls|fma|fms|fnma|fnms|sqrt|abs|"
    r"neg|mov|cmp|cmpe|cvt|cvtr|cvtb|cvtt|cvta|cvtn|cvtp|cvtm|"
    r"rinta|rintn|rintp|rintm|rintr|rintz|rintx|maxnm|minnm|"
    r"seleq|selvs|selge|selgt|mrs|msr))"
    r"s?(\.w|\.f32|\.32|(\.[fsu](16|32)){2})?$")

MEM = re.compile(
    r"^(ldr|ldrb|ldrh|ldrsb|ldrsh|str|strb|strh|ldrt|ldrbt|ldrht|ldrsbt|"
    r"ldrsht|strt|strbt|strht|ldrd|strd|vldr|vstr)(\.w)?$")


def candidates(rng, n_random, sources):
    out = []
    for _ in range(n_random):
        w0 = (rng.choice([0x1D, 0x1E, 0x1F]) << 11) | rng.getrandbits(11)
        out.append(((w0 << 16) | rng.getrandbits(16), "random"))
    if sources:
        # Evenly over the *forms* seen, not over the instructions: a
        # corpus is mostly the harness's own prologue, and drawing from
        # it uniformly would mutate `str.w r0, [r12, #n]` ten thousand
        # times.
        forms = {}
        for enc, text in sources:
            forms.setdefault(text.split()[0], []).append((enc, text))
        names = sorted(forms)
        for _ in range(len(out)):
            enc, text = rng.choice(forms[rng.choice(names)])
            bit = rng.randrange(32)
            out.append((enc ^ (1 << bit), "%s ^ bit %d" % (text.split("  ;")[0],
                                                          bit)))
    rng.shuffle(out)
    return out


def corpus(elfs, dis):
    """Every 32-bit instruction in the ELFs' code ranges, with its text."""
    out = []
    for elf in elfs:
        secs, ranges = dcheck.code_ranges(elf)
        base, img = dcheck.flat_image(elf, secs)
        tmp = elf + ".rawgen.bin"
        open(tmp, "wb").write(img)
        r = subprocess.run([dis, tmp, "0x%x" % base],
                           input="".join("%x %x\n" % x for x in ranges),
                           stdout=subprocess.PIPE, universal_newlines=True)
        os.unlink(tmp)
        for line in r.stdout.splitlines():
            _, n, enc, text = line.split("\t")
            if n == "4" and not text.startswith(".inst"):
                out.append((int(enc, 16), text))
    return out


def disassemble(encs, dis, tmpdir):
    path = os.path.join(tmpdir, "rawgen.bin")
    with open(path, "wb") as f:
        for e in encs:
            f.write(struct.pack("<HH", e >> 16, e & 0xFFFF))
    r = subprocess.run([dis, path, "0x10000000"],
                       input="10000000 %x\n" % (0x10000000 + 4 * len(encs)),
                       stdout=subprocess.PIPE, universal_newlines=True)
    lines = r.stdout.splitlines()
    if len(lines) != len(encs):
        sys.exit("armv7m-dis returned %d lines for %d encodings"
                 % (len(lines), len(encs)))
    return [l.split("\t")[3].split("  ;")[0].strip() for l in lines]


def classify(enc, text):
    """A gen.Case for this encoding, or None if it is not safe to run."""
    line = [".inst.w 0x%08x" % enc]
    if text.startswith(".inst"):
        c = gen.Case(line, text="%08x  (not decoded)" % enc, fp=True)
        c.raw = True
        return c
    mnem, _, ops = text.partition(" ")
    if re.search(r"\b(sp|lr|pc|r13|r14|r15)\b", ops):
        return None
    if "[" not in ops and "{" not in ops and REG_ONLY.match(mnem):
        c = gen.Case(line, text="%08x  %s" % (enc, text), fp=True)
        c.raw = True
        return c
    m = MEM.match(mnem)
    if not m:
        return None
    # rt[, rt2], [rn, #off]  |  [rn, #off]!  |  [rn], #off
    a = re.match(r"^((?:[rsd]\d+, )+)\[(r\d+)(?:, #(-?\d+))?\](!?)(?:, #(-?\d+))?$",
                 ops)
    if not a:
        return None
    targets = [t for t in a.group(1).split(", ") if t]
    base = a.group(2)
    # Rt == Rt2 is let through for a load: nothing about it can go
    # anywhere, and whether the core accepts it is exactly the question.
    if base in targets or (len(set(targets)) != len(targets)
                           and not mnem.startswith("ldrd")):
        return None
    pre = int(a.group(3) or 0)
    post = int(a.group(5) or 0)
    at = gen.MEM_BASE + pre
    lo = max(0, min(at, at + post) - 16)
    hi = min(16384, max(at, at + post) + 24)
    b = int(base[1:])
    c = gen.Case(line, text="%08x  %s" % (enc, text), fp=True,
                 setup=["ldr %s, =diff_mem+%d" % (base, gen.MEM_BASE)],
                 addr=1 << b, mem=(lo, hi))
    c.raw = True
    return c


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seed", type=lambda x: int(x, 0), default=1)
    ap.add_argument("--count", type=int, default=2000)
    ap.add_argument("--dis", required=True)
    ap.add_argument("--from", dest="src", action="append", default=[],
                    help="an ELF whose instructions are mutated; repeatable")
    ap.add_argument("--enc", default="",
                    help="encodings to run unfiltered: hex[:rN],...")
    ap.add_argument("--repeat", type=int, default=4)
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    rng = random.Random(a.seed)
    sources = corpus(a.src, a.dis) if a.src else []
    cases = []
    kinds = {"undecoded": 0, "register": 0, "memory": 0}
    seen = set()
    named = []
    for item in [x for x in a.enc.split(",") if x]:
        h, _, base = item.partition(":")
        enc = int(h, 16)
        text = disassemble([enc], a.dis, a.out)[0]
        for _ in range(a.repeat):
            c = gen.Case([".inst.w 0x%08x" % enc], fp=True,
                         text="%08x  %s    <- named" % (enc, text))
            if base:
                c.setup = ["ldr %s, =diff_mem+%d" % (base, gen.MEM_BASE)]
                c.mem = (gen.MEM_BASE - 16, gen.MEM_BASE + 24)
                # The base is printed relative to diff_mem -- the two
                # images put it at different addresses -- *unless* the
                # instruction loads over it, when what it holds afterwards
                # is a value. Marking that one as an address printed a
                # correct load as differing by exactly the distance
                # between the two images' RAM, 0xA0000000.
                dests = text.split("[")[0].split(None, 1)[-1]
                loads_base = (text.startswith(("ldr", "ldm", "pop")) and
                              re.search(r"\b%s\b" % base, dests))
                if not loads_base:
                    c.addr = 1 << int(base[1:])
            c.raw = True
            named.append(c)
    while len(cases) < a.count:
        cand = [c for c in candidates(rng, 4 * a.count, sources)
                if c[0] not in seen and (c[0] >> 27) in (0x1D, 0x1E, 0x1F)]
        if not cand:
            break
        texts = disassemble([c[0] for c in cand], a.dis, a.out)
        for (enc, why), text in zip(cand, texts):
            if enc in seen:
                continue
            seen.add(enc)
            c = classify(enc, text)
            if c is None:
                continue
            c.text += "    <- " + why
            # Half of what survives a uniform draw is undecoded; cap it so
            # the image is not mostly that.
            k = ("undecoded" if "(not decoded)" in c.text
                 else "memory" if c.setup else "register")
            if k == "undecoded" and kinds[k] >= a.count // 2:
                continue
            kinds[k] += 1
            cases.append(c)
            if len(cases) >= a.count:
                break
    rng.shuffle(cases)
    cases = named + cases
    gen.emit(cases, a.seed, a.out)
    print("raw cases: %d  (%s)" % (len(cases), ", ".join(
        "%s %d" % kv for kv in sorted(kinds.items()))))


if __name__ == "__main__":
    main()
