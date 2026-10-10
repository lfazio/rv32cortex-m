#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""run.py - the ARMv7-M frontend against a real Cortex-M7.

Generates cases, builds them twice -- an emulator guest and native
Nucleo-F746ZG firmware -- runs both, and compares what each printed.
The board is the reference: a line that differs is the emulator's to
explain.

    tests/armv7m-diff/run.py --seed 1 --count 2000 [--classes dpimm,ldst]
    tests/armv7m-diff/run.py --no-board ...     # build and run the emulator only
    tests/armv7m-diff/run.py --reuse ...        # compare the last outputs again
    tests/armv7m-diff/run.py --rerun ...        # the emulator again, the
                                                # board's answers as stored
    tests/armv7m-diff/run.py --self ...         # no board: the JIT against
                                                # the interpreter

`--rerun` is what makes the board's answers a regression suite: once a
directory holds `emu.bin` and `board.out`, any later emulator -- or the
same one under `--emu-args=--jit` -- is checked against the silicon
without the silicon being attached.

Needs arm-none-eabi-gcc, probe-rs and pyserial on the host, and the
board's ST-LINK virtual COM port (USART3, 460800 baud).
"""

import argparse
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
GUEST = os.path.join(ROOT, "tests", "guest", "armv7m")

CFLAGS = ["-mthumb", "-mcpu=cortex-m7", "-mfloat-abi=soft", "-O1", "-g",
          "-ffreestanding", "-nostdlib", "-fno-builtin", "-fno-common",
          "-Wall", "-Wextra", "-I", HERE]

FIELDS = (["r%d" % i for i in range(13)] + ["apsr", "mem"] +
          ["s%d" % i for i in range(32)] + ["fpscr", "cfsr", "fault-pc"])


def sh(cmd, **kw):
    r = subprocess.run(cmd, **kw)
    if r.returncode != 0:
        sys.exit("failed: %s" % " ".join(cmd))
    return r


def build(out, target, suite="cases"):
    elf = os.path.join(out, target + ".elf")
    if target == "emu":
        srcs = [os.path.join(GUEST, "start.S"), os.path.join(HERE, "emu_con.c"),
                "-T", os.path.join(HERE, "emu.ld")]
    else:
        srcs = [os.path.join(HERE, "board_start.S"),
                os.path.join(HERE, "board_con.c"),
                "-T", os.path.join(HERE, "board.ld")]
    if suite == "sys":
        body = [os.path.join(HERE, "sys.c"), os.path.join(HERE, "sys.S")]
    else:
        body = [os.path.join(HERE, "harness.c"), os.path.join(out, "cases.S")]
    flags = list(CFLAGS)
    if suite == "sys":
        # sys.c moves values through s0 itself; floats still travel in core
        # registers, so nothing else about the build changes.
        flags[flags.index("-mfloat-abi=soft")] = "-mfloat-abi=softfp"
        flags.append("-mfpu=fpv5-sp-d16")
    sh(["arm-none-eabi-gcc"] + flags + srcs + body + ["-o", elf])
    sh(["arm-none-eabi-objcopy", "-O", "binary", elf, elf[:-4] + ".bin"])
    return elf


def run_emu(out, emu, extra=()):
    r = subprocess.run([emu, "--max-insn", "400000000"] + list(extra) +
                       [os.path.join(out, "emu.bin")],
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    text = r.stdout.decode("latin1")
    with open(os.path.join(out, "emu.out"), "w") as f:
        f.write(text)
    return text


def run_board(out, port, baud, probe, timeout):
    import serial  # pyserial; only the board run needs it

    elf = os.path.join(out, "board.elf")
    chip = ["--chip", "STM32F746ZGTx", "--connect-under-reset"]
    if probe:
        chip += ["--probe", probe]
    ser = serial.Serial(port, baud, timeout=0.2)
    ser.reset_input_buffer()
    # The flash and the reset are retried: attaching to a part that never
    # idles races it, and a failed flash leaves the *previous* firmware
    # running, which would be compared as though it were this one.
    for attempt in range(3):
        if subprocess.run(["probe-rs", "download"] + chip + [elf]).returncode == 0:
            break
        time.sleep(1)
    else:
        sys.exit("probe-rs could not flash the board")
    ser.reset_input_buffer()
    subprocess.run(["probe-rs", "reset"] + chip)
    buf = b""
    t0 = time.time()
    while time.time() - t0 < timeout:
        buf += ser.read(65536)
        if b"DIFF-END" in buf:
            buf += ser.read(4096)
            break
    ser.close()
    text = buf.decode("latin1").replace("\r", "")
    with open(os.path.join(out, "board.out"), "w") as f:
        f.write(text)
    return text


def parse(text):
    cases, extra = {}, []
    for line in text.splitlines():
        if line.startswith("c "):
            f = line.split()
            cases[int(f[1], 16)] = f[2:]
        elif line.startswith(("DIFF-FAULT", "DIFF-BEGIN", "DIFF-END")):
            extra.append(line)
    return cases, extra


def compare_sys(emu_text, board_text):
    """The system suite: one line per test, compared by name."""
    def lines(t):
        d = {}
        for l in t.splitlines():
            if l.startswith("t "):
                f = l.split()
                d[f[1]] = f[2:]
        return d
    emu, brd = lines(emu_text), lines(board_text)
    for l in board_text.splitlines():
        if l.startswith("DIFF-FAULT"):
            print("board: " + l)
    bad = 0
    for name in brd:
        if emu.get(name) != brd[name]:
            bad += 1
            print("test %s" % name)
            print("    emu   %s" % " ".join(emu.get(name, ["(missing)"])))
            print("    board %s" % " ".join(brd[name]))
    print("tests %d, differ %d, board ran %d" % (len(brd), bad, len(brd)))
    return bad == 0 and len(brd) > 0


def compare(out, emu_text, board_text, limit):
    texts = {}
    with open(os.path.join(out, "cases.txt")) as f:
        for line in f:
            i, _, t = line.rstrip("\n").partition("\t")
            texts[int(i)] = t
    emu, emu_x = parse(emu_text)
    brd, brd_x = parse(board_text)
    for line in brd_x:
        if line.startswith("DIFF-FAULT"):
            print("board: " + line)
    if "DIFF-END" not in emu_text:
        print("emulator stopped early:")
        print("\n".join(emu_text.splitlines()[-6:]))
    bad = 0
    missing = 0
    shown = 0
    for n in sorted(brd):
        if n not in emu:
            missing += 1
            continue
        if emu[n] != brd[n]:
            bad += 1
            if shown < limit:
                shown += 1
                diffs = ["%s emu %s board %s" % (FIELDS[i], a, b)
                         for i, (a, b) in enumerate(zip(emu[n], brd[n]))
                         if a != b]
                print("case %d: %s" % (n, texts.get(n, "?")))
                for d in diffs:
                    print("    " + d)
    print("compared %d, differ %d, missing from emulator %d, board ran %d"
          % (len(brd) - missing, bad, missing, len(brd)))
    return bad == 0 and missing == 0 and len(brd) > 0


def self_check(out, emu):
    """
    The same image interpreted and translated, line for line.

    The interpreter is the half that was checked against the board, so
    this is the board's verdict at one remove -- and unlike the board it
    is always attached. What it cannot do is notice both being wrong in
    the same way, which is what --rerun against stored board output is
    for.

    **A JIT that translated nothing would pass**, since it would *be* the
    interpreter. So the run's own statistics are read, and a translated
    run with no block entries is a failure whatever the output says.
    """
    import re
    interp = run_emu(out, emu)
    jit = run_emu(out, emu, ["--jit"])
    keep = lambda t: [l for l in t.splitlines()
                      if l.startswith(("c ", "t ", "DIFF-", "SYS-"))]
    a, b = keep(interp), keep(jit)
    bad = [(x, y) for x, y in zip(a, b) if x != y]
    for x, y in bad[:8]:
        print("interp " + x[:150])
        print("jit    " + y[:150])
    m = re.search(r"blk entr\s+(\d+)", jit)
    entries = int(m.group(1)) if m else 0
    m = re.search(r"lowered\s+(\d+)\s+helper\s+(\d+)\s+declined\s+(\d+)", jit)
    print("lines %d, differ %d, jit block entries %d%s"
          % (len(a), len(bad) + abs(len(a) - len(b)), entries,
             ", lowered %s helper %s declined %s" % m.groups() if m else ""))
    if "backend jit" not in jit:
        print("the --jit run did not use a JIT backend")
        return False
    return (not bad and len(a) == len(b) and len(a) > 2 and entries > 0
            and "DIFF-END" in interp)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seed", default="1")
    ap.add_argument("--count", type=int, default=2000)
    ap.add_argument("--classes", default="")
    ap.add_argument("--out", default=os.path.join(ROOT, "build", "armv7m-diff"))
    ap.add_argument("--emu", default=os.path.join(ROOT, "build", "m7f",
                                                  "emu-host"))
    ap.add_argument("--port", default="/dev/ttyACM0")
    ap.add_argument("--baud", type=int, default=460800)
    ap.add_argument("--probe", default=os.environ.get("EMU_PROBE", ""))
    ap.add_argument("--timeout", type=float, default=120.0)
    ap.add_argument("--limit", type=int, default=20)
    ap.add_argument("--no-board", action="store_true")
    ap.add_argument("--suite", choices=["cases", "sys", "raw"],
                    default="cases",
                    help="raw: encodings rather than instructions, see "
                         "rawgen.py")
    ap.add_argument("--from", dest="src", action="append", default=[],
                    help="raw: an ELF whose instructions are mutated")
    ap.add_argument("--enc", default="",
                    help="raw: encodings to run unfiltered, hex[:rN],...")
    ap.add_argument("--repeat", type=int, default=4,
                    help="raw: how many times each --enc encoding runs")
    ap.add_argument("--reuse", action="store_true",
                    help="compare the outputs already in --out")
    ap.add_argument("--rerun", action="store_true",
                    help="run the emulator on the emu.bin already in --out "
                         "and compare with the board.out stored beside it")
    ap.add_argument("--emu-args", default="",
                    help="extra arguments for the emulator, e.g. --jit")
    ap.add_argument("--self", dest="self_check", action="store_true",
                    help="no board: run the cases interpreted and through "
                         "the JIT, and require identical output")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)

    cmp = (lambda e, b: compare_sys(e, b)) if a.suite == "sys" else \
        (lambda e, b: compare(a.out, e, b, a.limit))

    extra = a.emu_args.split()

    if a.rerun:
        # The answers are only the board's if the image is the one the
        # board ran, so nothing here regenerates or rebuilds anything.
        for need in ("emu.bin", "board.out"):
            if not os.path.exists(os.path.join(a.out, need)):
                sys.exit("--rerun: no %s in %s" % (need, a.out))
        emu_text = run_emu(a.out, a.emu, extra)
        board_text = open(os.path.join(a.out, "board.out"),
                          encoding="latin1").read()
        sys.exit(0 if cmp(emu_text, board_text) else 1)

    if a.reuse:
        emu_text = open(os.path.join(a.out, "emu.out")).read()
        board_text = open(os.path.join(a.out, "board.out"),
                          encoding="latin1").read()
        sys.exit(0 if cmp(emu_text, board_text) else 1)

    if a.suite == "cases":
        gen = [sys.executable, os.path.join(HERE, "gen.py"), "--seed", a.seed,
               "--count", str(a.count), "--out", a.out]
        if a.classes:
            gen += ["--classes", a.classes]
        sh(gen)
    elif a.suite == "raw":
        gen = [sys.executable, os.path.join(HERE, "rawgen.py"), "--seed",
               a.seed, "--count", str(a.count), "--out", a.out, "--dis",
               os.path.join(os.path.dirname(a.emu), "armv7m-dis")]
        for src in a.src:
            gen += ["--from", src]
        if a.enc:
            gen += ["--enc", a.enc, "--repeat", str(a.repeat)]
        sh(gen)
        a.suite = "cases"   # the same harness; only the cases differ
    build(a.out, "emu", a.suite)
    if a.self_check:
        sys.exit(0 if self_check(a.out, a.emu) else 1)
    emu_text = run_emu(a.out, a.emu, extra)
    if a.no_board:
        if a.suite == "sys":
            print("\n".join(l for l in emu_text.splitlines()
                            if l.startswith(("t ", "SYS"))))
        else:
            lines = sum(1 for l in emu_text.splitlines() if l.startswith("c "))
            print("emulator ran %d cases" % lines)
        if "DIFF-END" not in emu_text:
            print("\n".join(emu_text.splitlines()[-6:]))
        return
    build(a.out, "board", a.suite)
    board_text = run_board(a.out, a.port, a.baud, a.probe, a.timeout)
    sys.exit(0 if cmp(emu_text, board_text) else 1)


if __name__ == "__main__":
    main()
