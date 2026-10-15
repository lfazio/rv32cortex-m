#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""run-probe.py - run one probe on the emulator and on the reference models.

A probe (tests/arch-test/probes/*.S) is a short bare-metal program that
asks a question the architecture suite does not -- an extension with no
suite, or a case the suite does not reach -- and leaves its answers in
registers. This builds it once, runs it on the emulator and on whichever
of Sail and Spike it names and are present, and compares the registers it
lists:

    scripts/run-probe.py tests/arch-test/probes/zalasr.S
    scripts/run-probe.py --emu build/host/emu-host --emu-arg=--jit PROBE.S

Each probe says what it needs in its header comment, one line each:

    probe-march:      the -march to assemble it with
    probe-models:     which of `sail` and `spike` model what it asks
    probe-spike-isa:  Spike's --isa (Spike only makes a CSR writable when
                      the extension that owns it is named)
    probe-regs:       the registers that hold the answers
    probe-emu-cflags: defines for the emulator's build only, for a case
                      that cannot finish on it (Zawrs' parking wait)
    probe-permitted:  MODEL REG... -- a difference the specification
                      allows, with the reason after the dashes

A difference that is not permitted fails. A permitted one is printed and
passes: two readings the specification allows are a result, not a defect,
and the probe's comment says which. Exits 77 -- ctest's skip -- when none
of the models the probe names is installed, or the toolchain is missing.

Where the models come from: Sail from scripts/run-arch-test.sh, which
fetches it into build/sail; Spike from scripts/fetch-spike.sh, into
build/spike. SAIL and SPIKE override either path. Neither is in the tree.
"""

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ABI = ["zero", "ra", "sp", "gp", "tp", "t0", "t1", "t2", "s0", "s1",
       "a0", "a1", "a2", "a3", "a4", "a5", "a6", "a7", "s2", "s3", "s4",
       "s5", "s6", "s7", "s8", "s9", "s10", "s11", "t3", "t4", "t5", "t6"]
SKIP = 77


def headers(path):
    h = {}
    for line in open(path):
        m = re.match(r"\s*\*\s+probe-([a-z-]+):\s*(.*?)\s*$", line)
        if m:
            h.setdefault(m.group(1), []).append(m.group(2))
    return h


def build(src, march, out, cflags):
    gcc = os.environ.get("RISCV_GCC", "riscv64-unknown-elf-gcc")
    cmd = [gcc, "-march=" + march, "-mabi=ilp32", "-nostdlib", "-T",
           os.path.join(ROOT, "tests/arch-test/rv32cortex-m-rv32/link.ld"),
           *cflags, src, "-o", out]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit("probe: build failed\n" + r.stderr)


def run_emu(emu, args, elf):
    r = subprocess.run([emu, "--ram", "0x800000", "--max-insn", "100000",
                        "--dump", *args, elf],
                       capture_output=True, text=True, timeout=120)
    regs = {}
    for line in (r.stdout + r.stderr).splitlines():
        # "  s0   00000000  s1   00000007  a0   00000000  a1   00800000"
        if re.fullmatch(r"\s+(?:[a-z][a-z0-9]*\s+[0-9a-f]{8}\s*){2,}", line):
            for name, val in re.findall(r"([a-z][a-z0-9]*)\s+([0-9a-f]{8})", line):
                if name in ABI:
                    regs[name] = val
    return regs


def run_sail(sail, elf, tmp):
    trace = os.path.join(tmp, "sail.trace")
    # Sail calls more traps than xRETs a loop and stops; a probe whose
    # handler resumes with a jump -- sstvala.S, under TSR -- is not one.
    subprocess.run([sail, "--config",
                    os.path.join(ROOT, "tests/arch-test/rv32cortex-m-rv32/sail.json"),
                    "--disable-trap-loop-detection",
                    "--trace-gpr", "--use-abi-names", "--inst-limit", "5000",
                    "--trace-output", trace, elf],
                   capture_output=True, text=True, timeout=120)
    regs = {}
    for line in open(trace, errors="replace"):
        m = re.match(r"\s*([a-z][a-z0-9]*) <- 0x([0-9a-fA-F]+)", line)
        if m and m.group(1) in ABI:
            regs[m.group(1)] = m.group(2).lower()[-8:]
    return regs


def run_spike(spike, isa, elf):
    r = subprocess.run([spike, "--isa=" + isa, "-m0x80000000:0x800000",
                        "-l", "--log-commits", elf],
                       capture_output=True, text=True, timeout=120)
    regs = {}
    for line in (r.stdout + r.stderr).splitlines():
        for n, val in re.findall(r"\bx(\d+)\s+0x([0-9a-f]+)", line):
            regs[ABI[int(n)]] = val[-8:]
    return regs


def find(env, default):
    p = os.environ.get(env, default)
    return p if p and os.access(p, os.X_OK) else None


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("probe")
    ap.add_argument("--emu", default=os.path.join(ROOT, "build/host/emu-host"))
    ap.add_argument("--emu-arg", action="append", default=[])
    a = ap.parse_args()

    h = headers(a.probe)
    regs = " ".join(h.get("regs", [])).split()
    models = " ".join(h.get("models", [])).split()
    if not regs or not models:
        sys.exit("probe: no probe-regs or probe-models in " + a.probe)
    permitted = {}
    for p in h.get("permitted", []):
        words = p.split("--")[0].split()
        permitted.setdefault(words[0], set()).update(words[1:])

    gcc = os.environ.get("RISCV_GCC", "riscv64-unknown-elf-gcc")
    if shutil.which(gcc) is None:
        print("probe: skipped, no " + gcc)
        return SKIP
    tools = {
        "sail": find("SAIL", os.path.join(ROOT, "build/sail/bin/sail_riscv_sim")),
        "spike": find("SPIKE", os.path.join(ROOT, "build/spike/bin/spike")),
    }
    present = [m for m in models if tools.get(m)]
    if not present:
        print("probe: skipped, none of %s is installed "
              "(scripts/run-arch-test.sh fetches Sail, "
              "scripts/fetch-spike.sh builds Spike)" % ", ".join(models))
        return SKIP

    with tempfile.TemporaryDirectory() as tmp:
        march = (h.get("march") or ["rv32ima_zicsr"])[0]
        elf = os.path.join(tmp, "probe.elf")
        build(a.probe, march, elf, [])
        emu_elf = elf
        if h.get("emu-cflags"):
            emu_elf = os.path.join(tmp, "probe-emu.elf")
            build(a.probe, march, emu_elf, " ".join(h["emu-cflags"]).split())

        got = {"emu": run_emu(a.emu, a.emu_arg, emu_elf)}
        for m in present:
            if m == "sail":
                got[m] = run_sail(tools[m], elf, tmp)
            else:
                isa = (h.get("spike-isa") or ["rv32imac_zicsr"])[0]
                got[m] = run_spike(tools[m], isa, elf)

    cols = ["emu"] + present
    print("%-5s " % "" + " ".join("%-10s" % c for c in cols))
    bad = 0
    for r in regs:
        row = [got[c].get(r, "00000000") for c in cols]
        notes = []
        for c, v in zip(cols[1:], row[1:]):
            if v != row[0]:
                if r in permitted.get(c, set()):
                    notes.append(c + " differs, permitted")
                else:
                    notes.append(c + " DIFFERS")
                    bad += 1
        print("%-5s " % r + " ".join("%-10s" % v for v in row) +
              ("  " + "; ".join(notes) if notes else ""))
    if not got["emu"]:
        print("probe: the emulator printed no registers")
        return 1
    print("probe: %s" % ("PASS" if bad == 0 else "%d unexplained difference(s)" % bad))
    return 0 if bad == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
