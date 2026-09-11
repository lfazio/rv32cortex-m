#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
#
# g4mh-build-quake.sh - build Quake as a G4MH guest, with Renesas CC-RH.
#
#   scripts/g4mh-build-quake.sh <quake-src-dir> <pak0.pak> [out.bin]
#
# The G4MH counterpart of the `guest-quake` target, and a script for the
# same reason g4mh-build-doom.sh is one: CC-RH is a separate toolchain
# with its own driver, assembler and linker, and teaching CMake a second
# toolchain inside a build already configured for another buys nothing.
#
# **Quake is GPL-2.0 and this tree is Apache-2.0.** Nothing of it lives
# here; this file describes a build and does not contain one. The board
# -- port/boards/rv32cortexm/ -- lives in the port itself.
#
# What CC-RH needs beyond the obvious, all of it found by trying:
#
#   -lang=c99          its default is C89, where a declaration after a
#                      statement is an error.
#   ccrh-include/      POSIX headers it does not ship. It has a complete
#                      C library and no POSIX at all, because RH850 is an
#                      automotive part with no operating system under it.
#   rhs8n.lib          the 64-bit-double C library. Linking the 4-byte
#                      one against code compiled for 8 is a hard error
#                      naming neither: "size of double conflicts".
#   libmalloc.lib      malloc is not in the main library.
#   libsetjmp.lib      and setjmp/longjmp are not either. Quake's error
#                      recovery is a longjmp out of Host_Error.
#
# **The PAK goes in through the linker, not the compiler.** The obvious
# route -- the same bin2c array the RISC-V build uses -- produces 93 MB
# of C for an 18.7 MB file, and CC-RH dies on it after a minute with
# "Memory allocation fault (icode(parse))". rlink takes the binary
# directly with -Binary=, which costs nothing and is what that option is
# for. Its length comes from a one-line generated file, because the
# linker defines the symbol but not the size.

set -eu

CCRH_BIN=${CCRH_BIN:-/usr/local/Renesas/CC-RH/V2.08.00/bin}
CCRH_LIB=${CCRH_LIB:-/usr/local/Renesas/CC-RH/V2.08.00/lib/v850e3v5}

ROOT=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
SRC=${1:?usage: g4mh-build-quake.sh <quake-src-dir> <pak0.pak> [out.bin]}
PAK=${2:?usage: g4mh-build-quake.sh <quake-src-dir> <pak0.pak> [out.bin]}
OUT=${3:-$ROOT/build/quake-g4mh.bin}

if [ ! -x "$CCRH_BIN/ccrh" ]; then
    echo "g4mh-build-quake: no ccrh at $CCRH_BIN" >&2
    echo "                  set CCRH_BIN, or see docs/renesas/Dockerfile" >&2
    exit 2
fi
PATH=$CCRH_BIN:$PATH
export PATH

if [ ! -f "$SRC/winquake/host.c" ]; then
    echo "g4mh-build-quake: no Quake source at $SRC" >&2
    exit 2
fi

if [ ! -f "$SRC/port/boards/rv32cortexm/display.c" ]; then
    echo "g4mh-build-quake: the port has no rv32cortexm board" >&2
    exit 2
fi

if [ ! -f "$PAK" ]; then
    echo "g4mh-build-quake: no game data at $PAK" >&2
    echo "                  the shareware pak0.pak is enough" >&2
    exit 2
fi

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
cd "$WORK"

FLAGS="-Xcommon=rh850 -Xcpu=g4mh -Odefault -lang=c99
       -I$SRC/include -I$SRC/winquake
       -I$SRC/port/boards/rv32cortexm/ccrh-include
       -DWINQUAKE_ENABLE_LOGGING -DWINQUAKE_LOGGING_EXTERNAL"

echo "assembling the start-up"
cp "$ROOT/tests/guest/g4mh/crt0.asm" .
asrh -Xcommon=rh850 -Xcpu=g4mh crt0.asm

#
# The four excluded files are alternative network drivers. net_none.c is
# *kept*: the name reads as "no networking, omit it" and it is really
# the driver table for a build with none, which net_main.c indexes
# unconditionally.
#
echo "compiling Quake"
n=0
for f in "$SRC"/winquake/*.c "$SRC"/port/*.c \
         "$SRC"/port/boards/rv32cortexm/*.c; do
    b=$(basename "$f" .c)
    case $b in
    net_bsd|net_dgrm|net_udp|net_vcr)
        continue
        ;;
    esac
    # shellcheck disable=SC2086
    ccrh $FLAGS -c "$f" >/dev/null
    n=$((n + 1))
done
echo "  $n translation units"

#
# The linker defines the array's symbol; nothing defines its length, and
# sizeof on an extern array is not available to the C that reads it.
#
paklen=$(wc -c < "$PAK")
echo "const unsigned int quake_pak_len = ${paklen}u;" > pak_len.c
ccrh $FLAGS -c pak_len.c >/dev/null
echo "  PAK is $paklen bytes"

#
# .bss goes just above the image -- 0x8140_0000, where 19 MB of image
# ends around 0x8121_3000 -- rather than at a round 32 MB. The heap is
# 24 MB and the engine's statics another few, so starting .bss higher
# pushes its end past the 64 MB of guest RAM the runner is given.
#
# The underscore on _quake_pak is the assembler's spelling of the C name
# quake_pak, which the manual states for exactly this option.
#
echo "linking"
cp "$PAK" pak0.bin
rlink ./*.obj \
    -Binary=pak0.bin\(.pakdata:4/DATA,_quake_pak\) \
    -library="$CCRH_LIB/rhs8n.lib,$CCRH_LIB/libmalloc.lib,$CCRH_LIB/libsetjmp.lib" \
    -entry=_entry -form=binary -output=quake.bin \
    -start=.text_entry,.text,.const,.pakdata,.data/80000000 \
    -start=.bss/81400000 \
    -list="$WORK/quake.map" -show=symbol 2>&1 |
    grep -vi 'evaluation period' || true

#
# The map goes beside the image. There is no ELF here -- the output is a
# flat binary -- so it is the only way to turn an address from a trap
# report back into a function name, which is the first thing anyone
# debugging this will need.
#
cp "$WORK/quake.map" "$(dirname "$OUT")/$(basename "$OUT" .bin).map" \
    2>/dev/null || true

if [ ! -f quake.bin ]; then
    echo "g4mh-build-quake: link produced nothing" >&2
    exit 1
fi

mkdir -p "$(dirname "$OUT")"
cp quake.bin "$OUT"
echo "wrote $OUT ($(wc -c <"$OUT") bytes)"
echo
echo "run it with:"
echo "  emu-host --frontend g4mh --load 0x80000000 --ram 0x4000000 \\"
echo "           --jit $OUT"
