#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
#
# g4mh-check-encodings.sh - assemble G4MH instructions with Renesas CC-RH
# and print the fields this emulator decodes them into.
#
# Why this exists: CLAUDE.md has said since the frontend was written that
# G4MH has "no reference model and no toolchain", and that its tests are
# hand-assembled halfword arrays "deliberately not sharing an encoder with
# the interpreter -- a shared one would pass while both were wrong the
# same way". That is still true of the *semantics*. It is no longer true
# of the *encodings*: CC-RH is a second, independent encoder, and it is
# the only thing in this project that can say a hand-written opcode
# constant is wrong.
#
# It earned its place immediately. CMPF.S carries its condition in the
# reg3 field and its target CC bit in the sub-opcode's low bits; this
# emulator had the two the other way round, having inferred them from a
# manual diagram that draws the field as `0FFFF` and names neither part.
# Every hand-written test passed, because they all used fcbit 0, where the
# two readings agree.
#
#   cmpf.s 0x4, r6, r7, 3   ->  E7372624   sub=0x426  reg3=4
#   cmpf.s 0xC, r6, r7, 5   ->  E7372A64   sub=0x42A  reg3=12
#
# The second settles it: 0xC does not fit in three bits.
#
# Usage:
#   scripts/g4mh-check-encodings.sh [file.asm]
#
# With no argument it assembles the built-in set, which covers every
# floating-point encoding the frontend implements. Needs the `ccrh` image
# described in docs/renesas/Dockerfile.

set -eu

IMAGE=${CCRH_IMAGE:-ccrh:latest}
CCRH_BIN=/usr/local/Renesas/CC-RH/V2.08.00/bin

if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
    echo "g4mh-check-encodings: no such image: $IMAGE" >&2
    echo "build it from docs/renesas/Dockerfile, or set CCRH_IMAGE" >&2
    exit 2
fi

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

if [ $# -ge 1 ]; then
    cp "$1" "$work/in.asm"
else
    cat > "$work/in.asm" <<'ASM'
	.section .text, text
	.public _enc
_enc:
	; two-operand arithmetic: reg3 <- reg2 OP reg1
	addf.s   r7, r6, r10
	subf.s   r7, r6, r10
	mulf.s   r7, r6, r10
	divf.s   r7, r6, r10
	maxf.s   r7, r6, r10
	minf.s   r7, r6, r10
	; one-operand group, where reg1 is an opcode extension
	absf.s   r6, r10
	negf.s   r6, r10
	sqrtf.s  r6, r10
	recipf.s r6, r10
	rsqrtf.s r6, r10
	; fused multiply-add family
	fmaf.s   r7, r6, r10
	fmsf.s   r7, r6, r10
	fnmaf.s  r7, r6, r10
	fnmsf.s  r7, r6, r10
	; float to integer, one rounding per reg1 value, unsigned at bit 4
	roundf.sw  r6, r10
	trncf.sw   r6, r10
	ceilf.sw   r6, r10
	floorf.sw  r6, r10
	cvtf.sw    r6, r10
	roundf.suw r6, r10
	trncf.suw  r6, r10
	; integer to float
	cvtf.ws    r6, r10
	cvtf.uws   r6, r10
	; comparison: fcond in reg3, fcbit in the sub-opcode
	cmpf.s 0x00000004, r6, r7, 0
	cmpf.s 0x00000004, r6, r7, 3
	cmpf.s 0x0000000C, r6, r7, 5
	cmovf.s 3, r6, r7, r10
	trfsr 5
ASM
fi

docker run --rm -v "$work":/w -w /w "$IMAGE" sh -c \
    "PATH=$CCRH_BIN:\$PATH asrh -Xcommon=rh850 -Xcpu=g4mh in.asm -Xprn_path=." \
    2>&1 | grep -viE 'evaluation period|^$' || true

if [ ! -f "$work/in.prn" ]; then
    echo "g4mh-check-encodings: the assembler produced no listing" >&2
    exit 1
fi

# The listing is "address hex  line  source"; anything else is a comment,
# a directive or a label and carries no encoding.
python3 - "$work/in.prn" <<'PY'
import re, sys

print("%-26s %-9s %s" % ("source", "bytes", "as this emulator decodes it"))
print("-" * 78)
for line in open(sys.argv[1], errors="replace"):
    m = re.match(r'^[0-9A-F]{8} ([0-9A-F]{8})\s+\d+\s+(.*)$', line.rstrip())
    if not m:
        continue
    hexs, src = m.group(1), m.group(2).strip()
    if not src or src.startswith((';', '.')) or src.endswith(':'):
        continue
    b = bytes.fromhex(hexs)
    w0 = b[0] | (b[1] << 8)
    w1 = b[2] | (b[3] << 8)
    if (w0 >> 5) & 0x3F != 0x3F:
        print("%-26s %-9s (not the Format X/FP opcode)" % (src, hexs))
        continue
    print("%-26s %-9s reg1=%-2d reg2=%-2d sub=0x%03X reg3=%-2d fff=%d"
          % (src, hexs, w0 & 0x1F, (w0 >> 11) & 0x1F,
             w1 & 0x7FF, (w1 >> 11) & 0x1F, (w1 >> 1) & 7))
PY
