#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
#
# g4mh-build-doom.sh - build DOOM as a G4MH guest, with Renesas CC-RH.
#
#   scripts/g4mh-build-doom.sh <doom-src-dir> [out.bin]
#
# The G4MH counterpart of the `guest-doom` target in tests/guest/doom.cmake,
# and a script rather than CMake for the reason g4mh-build-guest.sh is:
# CC-RH is a separate toolchain with its own driver, assembler and linker,
# and teaching CMake a second toolchain inside a build already configured
# for another one buys nothing a script does not.
#
# **DOOM is GPL-2.0 and this tree is Apache-2.0.** Nothing of DOOM's lives
# here; this file describes a build and does not contain one. The port
# layer -- i_platform_g4mh.c, the shared i_video_emu.c, and the four POSIX
# shim headers CC-RH does not ship -- lives in the DOOM port itself,
# because a file implementing DOOM's interface and linking into DOOM's
# binary is part of that work.
#
# Two stages, and the first is not optional: run scripts/doom-gentables.sh
# once on the host before this, or the baked tables and the shrunken WAD
# this links do not exist.
#
# What CC-RH needs that GCC did not, all of it discovered by trying:
#
#   -lang=c99          its default is C89, where a declaration after a
#                      statement is an error. DOOM needs no -fpermissive
#                      equivalent: a compiler that still speaks C89
#                      accepts 1993 C without complaint, which GCC 14
#                      does not.
#   -Xpreinclude       stdint.h, which DrawFunctions.h uses without
#                      including, and the alloca substitute. The option
#                      does not search -I, so it takes a full path.
#   rhs8n.lib          the 64-bit-double C library. Linking the 4-byte
#                      one against code compiled for 8 is a hard error
#                      naming neither -- "size of double conflicts".
#   libmalloc.lib      malloc is not in the main library.
#
# The empty-array rewrite below is the one edit made to a generated file,
# and it is made to a *copy*: the table generator emits `static const
# unsigned char tcd_2[] = {};` for textures with no data, which is a GCC
# extension. Nothing takes sizeof any of them -- they are reached only as
# pointers through a table -- so a single zero byte changes no behaviour.

set -eu

CCRH_BIN=${CCRH_BIN:-/usr/local/Renesas/CC-RH/V2.08.00/bin}
CCRH_LIB=${CCRH_LIB:-/usr/local/Renesas/CC-RH/V2.08.00/lib/v850e3v5}

ROOT=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
SRC=${1:?usage: g4mh-build-doom.sh <doom-src-dir> [out.bin]}/src
OUT=${2:-$ROOT/build/doom-g4mh.bin}

if [ ! -x "$CCRH_BIN/ccrh" ]; then
    echo "g4mh-build-doom: no ccrh at $CCRH_BIN" >&2
    echo "                 set CCRH_BIN, or see docs/renesas/Dockerfile" >&2
    exit 2
fi
PATH=$CCRH_BIN:$PATH
export PATH

if [ ! -f "$SRC/d_main.c" ]; then
    echo "g4mh-build-doom: no DOOM source at $SRC" >&2
    exit 2
fi

if [ ! -f "$SRC/i_platform_g4mh.c" ]; then
    echo "g4mh-build-doom: the port has no G4MH platform layer" >&2
    echo "                 ($SRC/i_platform_g4mh.c)" >&2
    exit 2
fi

for f in support/rawwad_use.c support/baked_texture_data.c \
         support/baked_map_data.c; do
    if [ ! -f "$SRC/$f" ]; then
        echo "g4mh-build-doom: $f is missing -- run scripts/doom-gentables.sh" >&2
        exit 2
    fi
done

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
cd "$WORK"

FLAGS="-Xcommon=rh850 -Xcpu=g4mh -Odefault -lang=c99
       -Xpreinclude=$SRC/g4mh-include/g4mh_prelude.h
       -I$SRC -I$SRC/g4mh-include -DNORMALUNIX -DEMU_GUEST"

echo "assembling the start-up"
cp "$ROOT/tests/guest/g4mh/crt0.asm" .
asrh -Xcommon=rh850 -Xcpu=g4mh crt0.asm

#
# Only one video driver may be linked -- they define the same nine
# functions -- and i_net.c/i_sound.c/os_generic.c want sockets, a sound
# device and pthreads. The rv32 platform layer addresses a CLINT this
# frontend has not got.
#
echo "compiling DOOM"
n=0
for f in "$SRC"/*.c; do
    b=$(basename "$f" .c)
    case $b in
    i_video|i_video_console|XDriver|i_net|i_sound|os_generic|i_platform_rv32)
        continue
        ;;
    esac
    # shellcheck disable=SC2086
    ccrh $FLAGS -c "$f" >/dev/null
    n=$((n + 1))
done

sed 's/\[\] = {};/[] = {0};/' "$SRC/support/baked_texture_data.c" \
    > baked_texture_data.c
for f in baked_texture_data.c "$SRC/support/rawwad_use.c" \
         "$SRC/support/baked_map_data.c"; do
    # shellcheck disable=SC2086
    ccrh $FLAGS -c "$f" >/dev/null
    n=$((n + 1))
done
echo "  $n translation units"

#
# .bss is placed well clear of the image rather than immediately after
# it, so the two never have to be reasoned about together: the image is
# about 6 MB of compiled-in WAD and .bss is another 1.6 MB. Both fit in
# the 64 MB the runner is told to give the guest below.
#
echo "linking"
rlink ./*.obj \
    -library="$CCRH_LIB/rhs8n.lib,$CCRH_LIB/libmalloc.lib" \
    -entry=_entry -form=binary -output=doom.bin \
    -start=.text_entry,.text,.const,.data/80000000 \
    -start=.bss/82000000 2>&1 | grep -vi 'evaluation period' || true

if [ ! -f doom.bin ]; then
    echo "g4mh-build-doom: link produced nothing" >&2
    exit 1
fi

mkdir -p "$(dirname "$OUT")"
cp doom.bin "$OUT"
echo "wrote $OUT ($(wc -c <"$OUT") bytes)"
echo
echo "run it with:"
echo "  emu-host --frontend g4mh --load 0x80000000 --ram 0x4000000 \\"
echo "           --jit --timer-hz 6 $OUT"
