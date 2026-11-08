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
    tests/armv7m-diff/run.py --suite raw --count 0 --regs 300 --from x.elf
                                                # sp and pc in register fields
    tests/armv7m-diff/run.py --suite raw --count 0 --resume \
        --from-fuzz build/fuzz/corpus/armv7m-jit  # a fuzzer's encodings,
                                                # past any case the board
                                                # does not survive

`--rerun` is what makes the board's answers a regression suite: once a
directory holds `emu.bin` and `board.out`, any later emulator -- or the
same one under `--emu-args=--jit` -- is checked against the silicon
without the silicon being attached.

The system suite's last test locks the core up, after DIFF-END: run.py
waits for LOCKUP-BEGIN and reads DHCSR, CFSR, HFSR and FPCCR through
the probe, with no reset, into a `t lazy-lockup` line -- the emulator's
from its state dump. `--define NAME` builds both sides with a macro:
SYS_LOCKUP_PLAIN, _MPU, _NMI, _HOLE and _HFRDY choose other lockups
(sys.c says which).

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


# --define: macros for both builds, the emulator's and the board's.
DEFINES = []


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
    flags = list(CFLAGS) + ["-D" + d for d in DEFINES]
    if suite == "sys":
        # sys.c moves values through s0 itself; floats still travel in core
        # registers, so nothing else about the build changes.
        flags[flags.index("-mfloat-abi=soft")] = "-mfloat-abi=softfp"
        flags.append("-mfpu=fpv5-sp-d16")
    sh(["arm-none-eabi-gcc"] + flags + srcs + body + ["-o", elf])
    sh(["arm-none-eabi-objcopy", "-O", "binary", elf, elf[:-4] + ".bin"])
    return elf


def run_emu(out, emu, extra=()):
    # A flat binary carries no architecture, so a runner built with more
    # than one frontend would run it as its default -- RV32 -- and every
    # case would "differ". Name the frontend.
    # --dump: the state a lockup leaves is read from it (emu_lockup).
    r = subprocess.run([emu, "--frontend", "armv7m", "--max-insn", "400000000",
                        "--dump"] + list(extra) +
                       [os.path.join(out, "emu.bin")],
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    text = r.stdout.decode("latin1")
    with open(os.path.join(out, "emu.out"), "w") as f:
        f.write(text)
    return text


# DHCSR, CFSR, HFSR, FPCCR: what the lazy save's lockup leaves, read
# with the core locked up -- attached without a reset, which would clear
# them. S_LOCKUP is DHCSR bit 19.
LOCKUP_REGS = (0xE000EDF0, 0xE000ED28, 0xE000ED2C, 0xE000EF34)


def probe_read(probe, addr):
    cmd = ["probe-rs", "read", "--chip", "STM32F746ZGTx"]
    if probe:
        cmd += ["--probe", probe]
    out = subprocess.run(cmd + ["b32", "0x%x" % addr, "1"],
                         stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                         universal_newlines=True).stdout
    for line in out.splitlines():
        f = line.replace(":", " ").split()
        if len(f) >= 2 and f[0].lower().endswith("%x" % addr):
            return int(f[1], 16)
    return None


def lockup_line(lockup, cfsr, hfsr, fpccr):
    return "t lazy-lockup %d %08x %08x %08x\n" % (lockup, cfsr, hfsr, fpccr)


def board_lockup(probe):
    v = [probe_read(probe, a) for a in LOCKUP_REGS]
    if None in v:
        return "t lazy-lockup (the probe read nothing)\n"
    return lockup_line((v[0] >> 19) & 1, v[1], v[2], v[3])


def emu_lockup(text):
    """The same line from the emulator's state dump, once it got there."""
    import re

    if "LOCKUP-BEGIN" not in text:
        return ""
    m = re.search(r"cfsr ([0-9a-f]{8})  hfsr ([0-9a-f]{8})", text)
    f = re.search(r"fpccr ([0-9a-f]{8})", text)
    if not m or not f:
        return "t lazy-lockup (no state dump)\n"
    return lockup_line(1 if "  LOCKUP" in text else 0, int(m.group(1), 16),
                       int(m.group(2), 16), int(f.group(1), 16))


def run_board(out, port, baud, probe, timeout, lockup=False):
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
    if lockup and b"DIFF-END" in buf:
        # The last test locks the core up after DIFF-END; give it the
        # time to say it is about to, then ask the core.
        t1 = time.time()
        while b"LOCKUP-BEGIN" not in buf and time.time() - t1 < 3.0:
            buf += ser.read(4096)
        time.sleep(0.5)
        buf += ser.read(4096)
    ser.close()
    text = buf.decode("latin1").replace("\r", "")
    if lockup and "LOCKUP-BEGIN" in text:
        text += board_lockup(probe)
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


def resume_raw(a, gen, extra):
    """The raw suite, carried on past a case after which the board said
    nothing more.

    An encoding the M7 executes may move sp or pc where the harness cannot
    come back from, and it then prints nothing for that case or any after
    it: the fuzz corpus's first board run stopped at 1,557 of 4,000. That
    case is an answer -- the board did *something* the harness did not
    survive -- so it is recorded, and the cases after it are run again
    from a new image, until every case has run or been the one that
    stopped the board. Each part is a directory under --out.
    """
    skip = 0
    part = 0
    stops = []
    ok = True
    while True:
        out = os.path.join(a.out, "part%d" % part)
        os.makedirs(out, exist_ok=True)
        sh(gen + ["--out", out, "--skip", str(skip)])
        with open(os.path.join(out, "cases.txt")) as f:
            texts = [l.rstrip("\n").partition("\t")[2] for l in f]
        if not texts:
            break
        build(out, "emu", "cases")
        emu_text = run_emu(out, a.emu, extra)
        build(out, "board", "cases")
        board_text = run_board(out, a.port, a.baud, a.probe, a.timeout)
        emu, _ = parse(emu_text)
        brd, _ = parse(board_text)
        ok = compare(out, emu_text, board_text, a.limit) and ok
        ran = len(brd)
        if ran >= len(texts):
            break
        n = skip + ran
        said = ("emulator: cfsr %s fault-pc %s" % tuple(emu[ran][-2:])
                if ran in emu else "emulator: stopped too")
        stops.append((n, texts[ran], said))
        print("board stopped at case %d: %s (%s)" % (n, texts[ran], said))
        if ran in emu:
            ok = False
        skip = n + 1
        part += 1
    print("resumed %d time(s); the board stopped at %d case(s)%s"
          % (part, len(stops), ":" if stops else ""))
    for n, t, said in stops:
        print("    %d: %s -- %s" % (n, t, said))
    return ok


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
    ap.add_argument("--from-fuzz", dest="fuzz", action="append", default=[],
                    help="raw: a fuzz-armv7m-jit corpus directory, whose "
                         "32-bit encodings are asked of the board")
    ap.add_argument("--resume", action="store_true",
                    help="raw: when the board stops printing partway, "
                         "record the case it stopped at and run the rest "
                         "from a new image (one directory per part)")
    ap.add_argument("--fuzz-max", type=int, default=3000,
                    help="raw: with --from-fuzz, at most this many cases")
    ap.add_argument("--regs", type=int, default=0,
                    help="raw: corpus instructions to rerun with sp or pc "
                         "in their register fields (needs --from)")
    ap.add_argument("--regs-sp", action="store_true",
                    help="raw: with --regs, sp as a destination as well, "
                         "where the decoder calls the encoding UNDEFINED")
    ap.add_argument("--reuse", action="store_true",
                    help="compare the outputs already in --out")
    ap.add_argument("--rerun", action="store_true",
                    help="run the emulator on the emu.bin already in --out "
                         "and compare with the board.out stored beside it")
    ap.add_argument("--emu-args", default="",
                    help="extra arguments for the emulator, e.g. --jit")
    ap.add_argument("--define", action="append", default=[],
                    help="a macro for both builds, e.g. SYS_LOCKUP_PLAIN "
                         "(sys.c's other lockup)")
    ap.add_argument("--self", dest="self_check", action="store_true",
                    help="no board: run the cases interpreted and through "
                         "the JIT, and require identical output")
    a = ap.parse_args()
    DEFINES.extend(a.define)
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
        if a.suite == "sys":
            emu_text += emu_lockup(emu_text)
        board_text = open(os.path.join(a.out, "board.out"),
                          encoding="latin1").read()
        sys.exit(0 if cmp(emu_text, board_text) else 1)

    if a.reuse:
        emu_text = open(os.path.join(a.out, "emu.out")).read()
        if a.suite == "sys":
            emu_text += emu_lockup(emu_text)
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
        for d in a.fuzz:
            gen += ["--from-fuzz", d]
        if a.fuzz:
            gen += ["--fuzz-max", str(a.fuzz_max)]
        if a.regs:
            gen += ["--regs", str(a.regs)]
        if a.regs_sp:
            gen += ["--regs-sp"]
        if a.resume:
            if a.no_board or a.self_check:
                sys.exit("--resume is for a run on the board")
            gen = [x for x in gen if x not in ("--out", a.out)]
            sys.exit(0 if resume_raw(a, gen, extra) else 1)
        sh(gen)
        a.suite = "cases"   # the same harness; only the cases differ
    build(a.out, "emu", a.suite)
    if a.self_check:
        sys.exit(0 if self_check(a.out, a.emu) else 1)
    emu_text = run_emu(a.out, a.emu, extra)
    if a.suite == "sys":
        emu_text += emu_lockup(emu_text)
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
    board_text = run_board(a.out, a.port, a.baud, a.probe, a.timeout,
                           lockup=(a.suite == "sys"))
    sys.exit(0 if cmp(emu_text, board_text) else 1)


if __name__ == "__main__":
    main()
