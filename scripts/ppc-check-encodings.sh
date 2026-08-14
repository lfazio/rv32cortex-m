#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
#
# ppc-check-encodings.sh - assemble PowerPC instructions with binutils and
# print the fields this emulator decodes them into.
#
# The counterpart of g4mh-check-encodings.sh, and it exists for the same
# reason: a second, independent encoder is the only thing that can say a
# hand-written opcode constant is wrong. G4MH went three sessions without
# one and paid for it -- CMPF.S had its condition and its target bit the
# wrong way round, and every hand-written test passed because they all
# used the one value where the two readings coincide.
#
# Unlike CC-RH this needs no container: Debian's binutils-powerpc-linux-gnu
# assembles both classic Book E and VLE, and objdump -Mvle disassembles
# them back.
#
# Usage:
#   scripts/ppc-check-encodings.sh [file.s]        # VLE (the default)
#   PPC_MODE=booke scripts/ppc-check-encodings.sh  # classic 32-bit
#
# With no argument it assembles a built-in set covering the encodings the
# frontend decodes. It is also how the VLE length rule was *derived* --
# read the byte counts in the output rather than trusting a manual
# diagram, because a wrong length is not a wrong answer, it is a
# desynchronised instruction stream and every instruction after it is
# garbage.
#
# The rule, tabulated from the assembler across thirteen values of the
# first four bits:
#
#     top4  0 2 4 6 8 9 A B C D E   -> 16-bit
#     top4  1 3 5 7                 -> 32-bit
#
# so an instruction is 32-bit exactly when (top4 & 0x9) == 0x1: bit 3
# clear and bit 0 set. Note that 0x9, 0xB and 0xD are 16-bit despite
# having bit 0 set, which is why the mask is not simply `top4 & 1`.

set -eu

AS=${PPC_AS:-powerpc-linux-gnu-as}
OBJDUMP=${PPC_OBJDUMP:-powerpc-linux-gnu-objdump}
MODE=${PPC_MODE:-vle}

if ! command -v "$AS" >/dev/null 2>&1; then
    echo "ppc-check-encodings: $AS not found" >&2
    echo "install binutils-powerpc-linux-gnu" >&2
    exit 2
fi

case "$MODE" in
vle)   asflags="-mvle -mbig"; ddflags="-Mvle" ;;
booke) asflags="-mbig -mpower4"; ddflags="-Mbooke" ;;
*)     echo "ppc-check-encodings: PPC_MODE must be vle or booke" >&2; exit 2 ;;
esac

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

if [ $# -ge 1 ]; then
    cp "$1" "$work/in.s"
else
    cat > "$work/in.s" <<'ASM'
	.text
	# --- VLE 16-bit: the se_ forms a compiler emits constantly ---
	se_mr    3, 4
	se_add   3, 4
	se_sub   3, 4
	se_cmp   3, 4
	se_li    3, 5
	se_lwz   3, 0(4)
	se_stw   3, 0(4)
	se_blr
	se_b     .
	se_bclri 3, 4
	# --- VLE 32-bit: the e_ forms ---
	e_add16i 3, 4, 100
	e_lwz    3, 8(4)
	e_stw    3, 8(4)
	e_li     3, 1000
	e_cmpi   0, 3, 7
	e_b      .
	e_bl     .
	# --- classic Book E encodings VLE also accepts ---
	add      3, 4, 5
	se_addi  3, 4
	or       3, 4, 5
	and      3, 4, 5
	cmpw     3, 4
	mflr     3
	mtlr     3
	mfmsr    3
	mtmsr    3
	se_rfi
	se_rfci
ASM
fi

# shellcheck disable=SC2086
"$AS" $asflags "$work/in.s" -o "$work/in.o" 2>&1 | head -20

if [ ! -f "$work/in.o" ]; then
    echo "ppc-check-encodings: the assembler produced no object" >&2
    exit 1
fi

# shellcheck disable=SC2086
"$OBJDUMP" -d $ddflags "$work/in.o" > "$work/in.dis"

python3 - "$work/in.dis" <<'PY'
import re, sys

print("%-24s %-12s %s" % ("source", "bytes", "as this emulator decodes it"))
print("-" * 78)
for line in open(sys.argv[1], errors="replace"):
    # "   0:\t7c 64 2a 14 \tadd     r3,r4,r5"
    m = re.match(r'^\s*[0-9a-f]+:\t((?:[0-9a-f]{2} )+)\s*\t(.*)$', line.rstrip())
    if not m:
        continue
    raw, src = m.group(1).split(), m.group(2).strip()
    b = bytes(int(x, 16) for x in raw)
    hexs = ''.join('%02X' % x for x in b)

    # Big-endian: the first byte is the most significant.
    w0 = (b[0] << 8) | b[1]
    op6 = (w0 >> 10) & 0x3F          # primary opcode, bits 0:5
    top4 = (w0 >> 12) & 0xF          # bits 0:3 -- the VLE length selector

    if len(b) == 2:
        print("%-24s %-12s %2d-bit  op=0x%02X top4=0x%X  se_ form"
              % (src, hexs, len(b) * 8, op6, top4))
        continue

    w = int.from_bytes(b[:4], 'big')
    print("%-24s %-12s %2d-bit  op=0x%02X top4=0x%X  "
          "rD/rS=%-2d rA=%-2d rB=%-2d xo=0x%03X"
          % (src, hexs, len(b) * 8, op6, top4,
             (w >> 21) & 0x1F, (w >> 16) & 0x1F, (w >> 11) & 0x1F,
             (w >> 1) & 0x3FF))
PY
