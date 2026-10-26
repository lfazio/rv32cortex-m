#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""fuzz-dict.py - assemble the fuzz dictionaries' tokens.

    scripts/fuzz-dict.py [--check] [rv32|armv7m|g4mh|ppc ...]

tests/fuzz/dict/<ISA>-jit.dict is a libFuzzer dictionary in which every
token is written as assembly on the `#:` line above it, instructions
separated by ` | `:

    #: lui t0, 0x2000
    clint_msip="\\xb7\\x02\\x00\\x02"

This assembles each `#:` line with the toolchain scripts/fuzz.sh builds
that target's seeds with -- CC-RH, in its docker image, for G4MH -- and
rewrites the token below it. A token with no `#:` line (a configuration
word) is kept as written. With no ISA, all four. --check rewrites
nothing and fails if any token differs from what its assembly gives.

    scripts/fuzz-dict.py --compose DIR ISA

writes the dictionary's `#!` line -- token names, in order -- to
DIR/ISA-jit-taken as one input, from the tokens as committed, and
assembles nothing. It is the claim that the tokens compose into a run
that reaches a device and takes an interrupt both backends compare;
the fuzz tree's fuzz-ISA-jit-dict test runs it and checks the stats.

The dictionaries are committed, as the G4MH seeds are, because CC-RH
needs the `ccrh` image (docs/renesas/Dockerfile) and a checkout cannot
assume it; scripts/fuzz.sh reads the .dict files and never assembles.
**Run this when a `#:` line changes**, and --check before committing.

A token must not need the linker: one assembled with a relocation in it
would hold bytes the linker never filled in, so a relocation fails the
token (GNU toolchains; CC-RH links each token at 0x2000_0000 as the
program is, and a token here never names a symbol).
"""

import os
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DICT_DIR = os.path.join(ROOT, "tests", "fuzz", "dict")
ISAS = ["rv32", "armv7m", "g4mh", "ppc"]
TOKEN = re.compile(r'^([A-Za-z0-9_]+)="(.*)"\s*$')


def cache_var(name):
    """A toolchain path from the fuzz build's cache, as fuzz.sh reads it."""
    build = os.environ.get("FUZZ_BUILD", os.path.join(ROOT, "build", "fuzz"))
    try:
        with open(os.path.join(build, "CMakeCache.txt")) as f:
            for line in f:
                if line.startswith(name + ":"):
                    return line.split("=", 1)[1].strip()
    except OSError:
        pass
    return None


def gnu(isa):
    """(gcc, flags, prologue) for a GNU-assembled target."""
    if isa == "rv32":
        gcc = (os.environ.get("RISCV_GCC") or cache_var("RISCV_GCC")
               or shutil.which("riscv64-unknown-elf-gcc"))
        # No C: a token is the 32-bit form it is written as, never a
        # compressed one the assembler chose.
        return gcc, ["-march=rv32ima_zicsr", "-mabi=ilp32"], ""
    if isa == "armv7m":
        return (shutil.which("arm-none-eabi-gcc"),
                ["-mcpu=cortex-m7", "-mthumb"], ".syntax unified\n.thumb\n")
    if isa == "ppc":
        gcc = (os.environ.get("PPC_GCC") or cache_var("PPC_GCC")
               or shutil.which("powerpc64-linux-gnu-gcc"))
        return gcc, ["-m32", "-Wa,-a32,-mbig,-mregnames,-me500mc"], ""
    raise ValueError(isa)


def assemble_gnu(isa, sources, work):
    gcc, flags, prologue = gnu(isa)
    if not gcc:
        sys.exit(f"fuzz-dict: no assembler for {isa}")
    prefix = gcc[: -len("gcc")]
    out = []
    for i, src in enumerate(sources):
        s = os.path.join(work, f"tok{i}.S")
        o = os.path.join(work, f"tok{i}.o")
        b = os.path.join(work, f"tok{i}.bin")
        with open(s, "w") as f:
            f.write(prologue + src + "\n")
        r = subprocess.run([gcc, *flags, "-c", "-o", o, s],
                           capture_output=True, text=True)
        if r.returncode != 0:
            sys.exit(f"fuzz-dict: {isa}: `{src}` did not assemble:\n"
                     f"{r.stderr}")
        rel = subprocess.run([prefix + "readelf", "-r", o],
                             capture_output=True, text=True).stdout
        if "Relocation section" in rel:
            sys.exit(f"fuzz-dict: {isa}: `{src}` needs the linker:\n{rel}")
        subprocess.run([prefix + "objcopy", "-O", "binary", "-j", ".text",
                        o, b], check=True)
        with open(b, "rb") as f:
            out.append(f.read())
    return out


def assemble_ccrh(sources, work):
    image = os.environ.get("CCRH_IMAGE", "ccrh:latest")
    bindir = "/usr/local/Renesas/CC-RH/V2.08.00/bin"
    if subprocess.run(["docker", "image", "inspect", image],
                      capture_output=True).returncode != 0:
        sys.exit(f"fuzz-dict: no such image: {image}; build it from "
                 "docs/renesas/Dockerfile, or set CCRH_IMAGE")
    for i, src in enumerate(sources):
        with open(os.path.join(work, f"tok{i}.asm"), "w") as f:
            f.write('    .section ".text", text\n' + src + "\n")
    # One container for the lot: each start is seconds.
    script = (f"PATH={bindir}:$PATH; for f in tok*.asm; do n=${{f%.asm}}; "
              "asrh -Xcommon=rh850 -Xcpu=g4mh $f && "
              "rlink $n.obj -form=binary -output=$n.bin "
              "-start=.text/20000000 || exit 1; done")
    r = subprocess.run(["docker", "run", "--rm", "-v", f"{work}:/w", "-w",
                        "/w", image, "sh", "-c", script],
                       capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit(f"fuzz-dict: CC-RH failed:\n{r.stdout}{r.stderr}")
    out = []
    for i in range(len(sources)):
        with open(os.path.join(work, f"tok{i}.bin"), "rb") as f:
            out.append(f.read())
    return out


def escape(data):
    return "".join(f"\\x{b:02x}" for b in data)


def parse(path):
    """The dictionary's lines, and (token line, assembly) for each `#:`."""
    with open(path) as f:
        lines = f.read().split("\n")
    todo = []
    pending = None
    for i, line in enumerate(lines):
        if line.startswith("#:"):
            if pending is not None:
                sys.exit(f"fuzz-dict: {path}:{pending[0] + 1}: "
                         "a `#:` line with no token after it")
            pending = (i, line[2:].strip())
        elif TOKEN.match(line):
            if pending is not None:
                todo.append((i, pending[1]))
                pending = None
        elif line.strip() and not line.startswith("#"):
            sys.exit(f"fuzz-dict: {path}:{i + 1}: not a token: {line}")
    if pending is not None:
        sys.exit(f"fuzz-dict: {path}:{pending[0] + 1}: "
                 "a `#:` line with no token after it")
    return lines, todo


def compose(isa, out_dir):
    """The `#!` sequence, as one input, from the committed tokens."""
    path = os.path.join(DICT_DIR, f"{isa}-jit.dict")
    lines, _ = parse(path)
    tokens = {}
    seq = None
    for line in lines:
        m = TOKEN.match(line)
        if m:
            tokens[m.group(1)] = bytes(
                int(h, 16) for h in re.findall(r"\\x([0-9a-fA-F]{2})",
                                               m.group(2)))
        elif line.startswith("#!"):
            seq = line[2:].split()
    if not seq:
        sys.exit(f"fuzz-dict: {path} has no `#!` sequence")
    data = b""
    for name in seq:
        if name not in tokens or not tokens[name]:
            sys.exit(f"fuzz-dict: {path}: `#!` names {name}, "
                     "which is not an assembled token")
        data += tokens[name]
    os.makedirs(out_dir, exist_ok=True)
    with open(os.path.join(out_dir, f"{isa}-jit-taken"), "wb") as f:
        f.write(data)
    return True


def one(isa, check):
    path = os.path.join(DICT_DIR, f"{isa}-jit.dict")
    lines, todo = parse(path)

    sources = ["\n".join("    " + s.strip() for s in asm.split(" | "))
               for _, asm in todo]
    work = tempfile.mkdtemp()
    try:
        if isa == "g4mh":
            data = assemble_ccrh(sources, work)
        else:
            data = assemble_gnu(isa, sources, work)
    finally:
        shutil.rmtree(work, ignore_errors=True)

    stale = 0
    for (i, asm), d in zip(todo, data):
        if not d:
            sys.exit(f"fuzz-dict: {isa}: `{asm}` assembled to nothing")
        name = TOKEN.match(lines[i]).group(1)
        want = f'{name}="{escape(d)}"'
        if lines[i] != want:
            stale += 1
            if check:
                print(f"fuzz-dict: {isa}: {name} differs from `{asm}`:\n"
                      f"  have {lines[i]}\n  want {want}")
            lines[i] = want
    if check:
        return stale == 0
    with open(path, "w") as f:
        f.write("\n".join(lines))
    print(f"fuzz-dict: {isa}: {len(todo)} tokens assembled, {stale} changed")
    return True


def main(argv):
    if argv[:1] == ["--compose"]:
        if len(argv) != 3 or argv[2] not in ISAS:
            sys.exit("usage: fuzz-dict.py --compose DIR ISA")
        return 0 if compose(argv[2], argv[1]) else 1
    check = "--check" in argv
    isas = [a for a in argv if a != "--check"] or ISAS
    for isa in isas:
        if isa not in ISAS:
            sys.exit(f"fuzz-dict: no target '{isa}' ({', '.join(ISAS)})")
    ok = all([one(isa, check) for isa in isas])
    if check and ok:
        print("fuzz-dict: every token matches its assembly")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
