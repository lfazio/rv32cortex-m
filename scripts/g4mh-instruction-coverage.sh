#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
#
# g4mh-instruction-coverage.sh - which G4MH instructions does this
# emulator actually execute?
#
#   scripts/g4mh-instruction-coverage.sh [--emu build/g4/emu-host]
#
# Assembles scripts/g4mh-all-instructions.asm with Renesas CC-RH, then
# *runs* each encoding on its own and reports whether the interpreter
# executed it or raised RIE.
#
# Why running it is the only honest test
# --------------------------------------
# Two cheaper instruments were tried first and both lied, in the same
# direction and for the same reason -- they cannot represent the thing
# being asked about:
#
#   - **The disassembler** knows 74 mnemonics against the interpreter's
#     far larger set, so diffing the manual against it reported ~190
#     gaps, nearly all of them implemented. docs already say the
#     disassembler is not a decoder; this is what that costs when it is
#     used as a coverage instrument.
#   - **Grepping the frontend for mnemonics** reported the whole
#     float-to-integer family missing, because those four groups share
#     one encoding whose rounding mode comes from a nibble -- so the
#     names CEILF.SW, FLOORF.SW and TRNCF.SW appear nowhere in the
#     source and all three work.
#
# What is left is to execute the instruction and read FEIC. RIE is
# 0x60, and the emulator's own `--dump` prints it by name, so a run
# either retires the instruction or says `reserved instruction` about
# it. That distinction is the whole output of this script.
#
# The image under each test is filled with HALT, so a branch, a JARL or
# a trap vector lands on a stop rather than running off into whatever
# follows. Only FEIC decides the verdict, never where the guest stopped.
#
# Two limits worth knowing, both about the assembler rather than the ISA:
#
#   - CC-RH V2.08 has no `-Xcpu=g4mh2`, so `LDM.MP` and `STM.MP` (which
#     the manual marks G4MH2-only) cannot be assembled here at all, and
#     neither can `PUSHSP`/`POPSP`. They are absent from the list rather
#     than reported missing -- confirm those by hand.
#   - The `.S4` SIMD group and the `LDV`/`STV`/`MOVV`/`SHFLV` vector
#     loads take `wreg` operands, a second register file this frontend
#     does not model. They are deliberately out of the list.
#
# Needs the `ccrh` image from docs/renesas/Dockerfile and a G4MH build
# of emu-host.

set -eu

IMAGE=${CCRH_IMAGE:-ccrh:latest}
CCRH_BIN=/usr/local/Renesas/CC-RH/V2.08.00/bin
here=$(dirname "$0")
src=$here/g4mh-all-instructions.asm

emu=
if [ "${1:-}" = "--emu" ]; then emu=$2; shift 2; fi
if [ -z "$emu" ]; then
    for c in build/g4/emu-host build/g4tr/emu-host build/both/emu-host; do
        [ -x "$c" ] && emu=$c && break
    done
fi
if [ -z "$emu" ] || [ ! -x "$emu" ]; then
    echo "g4mh-instruction-coverage: no G4MH emu-host; pass --emu PATH" >&2
    exit 2
fi
if ! "$emu" --help 2>&1 | grep -q g4mh; then
    echo "g4mh-instruction-coverage: $emu has no g4mh frontend" >&2
    exit 2
fi
if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
    echo "g4mh-instruction-coverage: no such image: $IMAGE" >&2
    exit 2
fi

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cp "$src" "$work/in.asm"

docker run --rm -v "$work":/w -w /w "$IMAGE" sh -c \
    "PATH=$CCRH_BIN:\$PATH; asrh -Xcommon=rh850 -Xcpu=g4mh -Xprn_path in.asm" \
    >"$work/log" 2>&1 || { cat "$work/log" >&2; exit 1; }

lst=$(ls "$work"/*.prn "$work"/*.lst 2>/dev/null | head -1) || true
if [ -z "${lst:-}" ] || [ ! -f "$lst" ]; then
    echo "g4mh-instruction-coverage: the assembler produced no listing" >&2
    cat "$work/log" >&2
    exit 1
fi

EMU="$emu" WORK="$work" python3 - "$lst" <<'PY'
import os, re, subprocess, sys

emu, work = os.environ['EMU'], os.environ['WORK']
rows, seen = [], set()
for line in open(sys.argv[1], errors='replace'):
    # Same listing shape the encoding checker parses: address, 2-8 bytes
    # of code, a line number or "--" on a continuation, then the source.
    m = re.match(r'^[0-9A-F]{8} ([0-9A-F]{4}(?:[0-9A-F]{4}){0,3})\s+(?:\d+|--)\s+(.*)$',
                 line.rstrip())
    if not m:
        continue
    hexs, src = m.group(1), m.group(2).strip()
    if not src or src.startswith((';', '.')) or src.endswith(':'):
        continue
    if src in seen:
        continue
    seen.add(src)
    rows.append((src, bytes.fromhex(hexs)))

HALT = b'\xE0\x07\x20\x01'
img = os.path.join(work, 't.bin')
ok = bad = 0
out = []
for src, code in rows:
    # A field of HALT so any branch, call or trap vector stops rather
    # than running on into the next test's bytes.
    buf = bytearray(HALT * 64)
    buf[0:len(code)] = code
    open(img, 'wb').write(bytes(buf))
    r = subprocess.run([emu, '--quiet', '--frontend', 'g4mh', '--cores', '1',
                        '--load', '0x80000000', '--max-insn', '64', '--dump', img],
                       capture_output=True, text=True, timeout=60)
    # --dump goes to stderr. Reading stdout alone made every instruction
    # look implemented -- 237 of 237, which is what prompted the A/B that
    # found this: breaking BSW on purpose changed the report not at all.
    dump = r.stdout + r.stderr
    rie = re.search(r'^\s*feic\s+0*60\b', dump, re.M) is not None
    out.append((src, code.hex().upper(), not rie))
    if rie: bad += 1
    else:   ok += 1

w = max(len(s) for s, _, _ in out)
print(f"{'instruction'.ljust(w)}  {'encoding'.ljust(16)}  status")
print('-' * (w + 28))
for src, hx, good in out:
    print(f"{src.ljust(w)}  {hx.ljust(16)}  {'ok' if good else 'RIE'}")
print()
print(f"{len(out)} instructions: {ok} executed, {bad} raised RIE")
PY
