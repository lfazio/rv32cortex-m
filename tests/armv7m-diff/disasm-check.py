#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""disasm-check.py - the ARMv7-M disassembler, checked by assembling it.

    tests/armv7m-diff/disasm-check.py build/armv7m-diff/emu.elf [more.elf ...]

Every instruction in the ELF's code ranges is disassembled by
`armv7m-dis`, the text is fed back through `arm-none-eabi-as`, and the
bytes that come out must be the bytes that went in.

That is a stronger statement than comparing two disassemblers' text, and
it needs no second disassembler: a mnemonic, a register, an immediate, a
branch target or a width qualifier that is wrong produces a different
encoding or none. It is the assembler -- the tool every guest in this
tree is already built with -- stating what the text means.

**It cannot see an instruction the decoder does not know**, because
`.inst 0x....` assembles to itself. So the count of those is reported
separately and is a failure unless `--allow-inst N` says how many the
image really contains: in code the assembler produced from mnemonics
there should be none.

Code is told from data by the ELF's own `$t`/`$d` mapping symbols. A
literal pool sits inside a function, so disassembling a flat image prints
confident nonsense for every constant -- which would be reported here as
the disassembler's fault.
"""

import argparse
import collections
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))

AS = ["arm-none-eabi-as", "-mcpu=cortex-m7", "-mfpu=fpv5-d16", "-mthumb"]

# Mnemonics whose last operand is an absolute address in the text and has
# to become location-relative to be assembled anywhere but in place.
TARGET = re.compile(r"^(b|bl|cbz|cbnz|adr)(eq|ne|cs|cc|mi|pl|vs|vc|hi|ls|ge|lt|gt|le)?"
                    r"(\.[nw])?\s")


def run(cmd, **kw):
    return subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                          universal_newlines=True, **kw)


def code_ranges(elf):
    """[(name, addr, file offset, size)] and [(lo, hi)] of Thumb code."""
    secs = {}
    out = run(["arm-none-eabi-readelf", "-SW", elf]).stdout
    for line in out.splitlines():
        m = re.match(r"\s*\[\s*(\d+)\]\s+(\S+)\s+PROGBITS\s+([0-9a-f]+)\s+"
                     r"([0-9a-f]+)\s+([0-9a-f]+)\s+\S+\s+(\S+)", line)
        if m and "X" in m.group(6):
            secs[int(m.group(1))] = (m.group(2), int(m.group(3), 16),
                                     int(m.group(4), 16), int(m.group(5), 16))
    marks = collections.defaultdict(list)
    out = run(["arm-none-eabi-readelf", "-sW", elf]).stdout
    for line in out.splitlines():
        f = line.split()
        if len(f) >= 8 and re.match(r"^\$[tda](\..*)?$", f[7]) and f[6].isdigit():
            ndx = int(f[6])
            if ndx in secs:
                marks[ndx].append((int(f[1], 16) & ~1, f[7][1]))
    ranges = []
    for ndx, (_, addr, _, size) in secs.items():
        ms = sorted(set(marks[ndx]))
        for i, (a, kind) in enumerate(ms):
            end = ms[i + 1][0] if i + 1 < len(ms) else addr + size
            if kind == "t" and end > a:
                ranges.append((a, end))
    return list(secs.values()), sorted(ranges)


def flat_image(elf, secs):
    base = min(a for _, a, _, _ in secs)
    top = max(a + s for _, a, _, s in secs)
    img = bytearray(top - base)
    data = open(elf, "rb").read()
    for _, addr, off, size in secs:
        img[addr - base:addr - base + size] = data[off:off + size]
    return base, bytes(img)


def it_blocks(insns):
    """For each instruction, the [start, end) of the IT block it is in."""
    span = [(i, i + 1) for i in range(len(insns))]
    i = 0
    while i < len(insns):
        m = re.match(r"^it([te]*)\s", insns[i][3])
        if m:
            n = 1 + len(m.group(1))
            end = min(len(insns), i + 1 + n)
            # Only when the members really do follow at the next addresses.
            ok = all(insns[j + 1][0] == insns[j][0] + insns[j][1]
                     for j in range(i, end - 1))
            if ok:
                for j in range(i, end):
                    span[j] = (i, end)
                i = end
                continue
        i += 1
    return span


def source_line(addr, text):
    text = text.split("  ;")[0].rstrip()
    if TARGET.match(text):
        m = re.search(r"0x([0-9a-f]{8})$", text)
        if m:
            off = int(m.group(1), 16) - addr
            text = text[:m.start()] + (". + %d" % off if off >= 0
                                       else ". - %d" % -off)
    return text


def raw_line(length, enc):
    return ".inst.w 0x%s" % enc if length == 4 else ".inst.n 0x%s" % enc


def check(elf, dis, tmp, verbose):
    secs, ranges = code_ranges(elf)
    if not ranges:
        sys.exit("%s: no $t mapping symbols in an executable section" % elf)
    base, img = flat_image(elf, secs)
    binp = os.path.join(tmp, "image.bin")
    open(binp, "wb").write(img)
    r = subprocess.run([dis, binp, "0x%x" % base],
                       input="".join("%x %x\n" % x for x in ranges),
                       stdout=subprocess.PIPE, universal_newlines=True)
    if r.returncode != 0:
        sys.exit("armv7m-dis failed on %s" % elf)
    insns = []
    for line in r.stdout.splitlines():
        a, n, enc, text = line.split("\t")
        insns.append((int(a, 16), int(n), enc, text))
    if not insns:
        sys.exit("%s: nothing disassembled" % elf)
    span = it_blocks(insns)

    unknown = [i for i, x in enumerate(insns) if x[3].startswith(".inst")]
    lines = [source_line(a, t) for a, _, _, t in insns]
    rejected = {}   # index -> the assembler's message
    src = os.path.join(tmp, "re.s")
    obj = os.path.join(tmp, "re.o")
    for _ in range(40):
        with open(src, "w") as f:
            f.write(".syntax unified\n.thumb\n.text\n")
            for (a, _, _, _), text in zip(insns, lines):
                f.write(".org 0x%x\n%s\n" % (a - base, text))
        r = run(AS + ["-o", obj, src])
        errs = [re.match(r".*?:(\d+): Error: (.*)", l)
                for l in r.stderr.splitlines()]
        errs = [(int(m.group(1)), m.group(2)) for m in errs if m]
        if not errs:
            if r.returncode != 0:
                sys.exit("assembler failed with no parsable error:\n" + r.stderr)
            break
        new = 0
        for lineno, msg in errs:
            # Three header lines, then an .org and an instruction each.
            k, is_insn = divmod(lineno - 4, 2)
            if not is_insn:
                # The .org could not move backwards: the instruction
                # before it came out wider than the encoding it printed.
                k -= 1
                msg = "assembles wider than the encoding: " + msg
            if k < 0 or k >= len(insns) or k in rejected:
                continue
            rejected[k] = msg
            new += 1
            for j in range(*span[k]):
                lines[j] = raw_line(insns[j][1], insns[j][2])
        if new == 0:
            sys.exit("assembler errors that name no instruction:\n" + r.stderr)
    else:
        sys.exit("the assembler was still rejecting lines after 40 rounds")

    out = os.path.join(tmp, "re.bin")
    r = run(["arm-none-eabi-objcopy", "-O", "binary", "-j", ".text", obj, out])
    if r.returncode != 0:
        sys.exit("objcopy failed: " + r.stderr)
    got = open(out, "rb").read()
    wrong = []
    for i, (a, n, enc, text) in enumerate(insns):
        if i in rejected:
            continue
        o = a - base
        b = got[o:o + n]
        have = "".join("%02x%02x" % (b[j + 1], b[j]) for j in range(0, len(b), 2))
        if have != enc and "[pc, #-0]" in text and len(have) == 8:
            # Minus zero against the pc -- U clear, offset zero -- is what
            # the linker's veneers use (`ldr.w pc, [pc, #-0]`) and what
            # the assembler cannot be made to write: it reads `#-0` as
            # `#0` there. Checked instead of skipped: everything but the
            # U bit has to agree.
            have = "%04x%s" % (int(have[:4], 16) & ~0x0080, have[4:])
        if have != enc:
            wrong.append((i, have))

    def show(title, items, fmt):
        if not items:
            return
        by = collections.Counter(insns[i][3].split()[0] for i in items_idx(items))
        print("  %s: %d  (%s)" % (title, len(items), ", ".join(
            "%s %d" % kv for kv in by.most_common(12))))
        for it in list(items)[:verbose]:
            print("      " + fmt(it))

    def items_idx(items):
        return [it[0] if isinstance(it, tuple) else it for it in items]

    print("%s: %d instructions in %d code ranges" % (os.path.relpath(elf, ROOT),
                                                      len(insns), len(ranges)))
    show("not decoded", unknown,
         lambda i: "%08x  %s" % (insns[i][0], insns[i][3]))
    show("rejected by the assembler", sorted(rejected.items()),
         lambda kv: "%08x  %-9s %-36s %s" % (insns[kv[0]][0], insns[kv[0]][2],
                                           insns[kv[0]][3], kv[1]))
    show("assembled to another encoding", wrong,
         lambda iw: "%08x  %-9s %-36s -> %s" % (insns[iw[0]][0], insns[iw[0]][2],
                                              insns[iw[0]][3], iw[1]))
    return len(insns), len(unknown), len(rejected), len(wrong)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("elf", nargs="+")
    ap.add_argument("--dis", default=os.path.join(ROOT, "build", "m7f",
                                                  "armv7m-dis"))
    ap.add_argument("--allow-inst", type=int, default=0,
                    help="how many undecoded instructions the images contain")
    ap.add_argument("-v", "--verbose", type=int, default=12,
                    help="examples to print per kind of failure")
    a = ap.parse_args()
    tot = [0, 0, 0, 0]
    with tempfile.TemporaryDirectory() as tmp:
        for elf in a.elf:
            for i, v in enumerate(check(elf, a.dis, tmp, a.verbose)):
                tot[i] += v
    print("total %d instructions: %d not decoded, %d rejected, %d re-encoded "
          "differently" % tuple(tot))
    ok = tot[0] > 0 and tot[1] <= a.allow_inst and tot[2] == 0 and tot[3] == 0
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
