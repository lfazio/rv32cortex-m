#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
#
# Run DOOM's host generation stage, which must happen before a guest
# image can be built.
#
# The port is a **two-stage build** and nothing says so where you would
# look. `emdoom.gentables.initial` is compiled *for the host* with
# -DGENERATE_BAKED, run natively, and writes three things the target
# build then compiles in:
#
#   support/baked_texture_data.c   pre-computed texture tables
#   support/baked_map_data.c       pre-computed map tables
#   support/rawwad_use.[ch]        the WAD, shrunk to what is reachable
#
# Build the target with GENERATE_BAKED still set and it does not use
# those tables -- it *regenerates* them, printing every texture and then
# calling fopen to write them out. On a bare-metal guest fopen jumps
# through a null pointer and traps for ever. The flag's own comment in
# stubs.h says "Don't do this on target hardware!!!".
#
#   scripts/doom-gentables.sh [doom-source-dir]
#
# Needs a 32-bit host toolchain: DOOM is 32-bit only, so `gcc -m32` must
# link, which on Debian means gcc-multilib.

set -eu

ROOT=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
SRC=${1:-$ROOT/build/doom-src}/src

if [ ! -f "$SRC/d_main.c" ]; then
    echo "error: no DOOM source at $SRC" >&2
    echo "       configure with -DEMU_DOOM=ON first, or pass the path" >&2
    exit 1
fi

if ! echo 'int main(void){return 0;}' | cc -m32 -x c - -o /dev/null 2>/dev/null; then
    echo "error: gcc cannot link -m32; DOOM is 32-bit only" >&2
    echo "       Debian: sudo apt install gcc-multilib" >&2
    exit 1
fi

cd "$SRC"

#
# **The full WAD stands in for support/rawwad_begin.c**, which the
# Makefile links here and which does not exist in a checkout: it is
# produced by support/shrinkwad, whose lump-map path is behind `#if 0`
# in the shipped source, so that tool writes nothing and the documented
# chain cannot start.
#
# Substituting rawwad.c is not a workaround but the right input:
# w_wad.c under GENERATE_BAKED includes support/rawwad.h, which declares
# exactly the array rawwad.c defines. The generator is meant to read the
# whole WAD -- shrinking it is what its *output* is for.
#
# i_video_console.c rather than i_video.c, so the generator needs no
# X11; and the rv32 files are excluded because they address memory that
# exists only inside the emulator.
#
SRCS=$(ls ./*.c | grep -vE '/(i_video|i_video_rv32|i_platform_rv32|XDriver)\.c$')

# The four rawdraw hooks, which i_video.c would otherwise supply.
STUB=$(mktemp /tmp/cnfg_stub_XXXXXX.c)
trap 'rm -f "$STUB"' EXIT
cat > "$STUB" <<'EOF'
#include <stdint.h>
uint32_t CNFGColor(uint32_t c){(void)c;return 0;}
void CNFGTackPixel(short a,short b){(void)a;(void)b;}
void CNFGTackSegment(short a,short b,short c,short d){(void)a;(void)b;(void)c;(void)d;}
void CNFGTackRectangle(short a,short b,short c,short d){(void)a;(void)b;(void)c;(void)d;}
EOF

echo "building the generator (32-bit host)"
# shellcheck disable=SC2086
cc -m32 -std=gnu99 -fpermissive -w -O1 \
   -DNORMALUNIX -DLINUX -DGENERATE_BAKED -DGENERATE_BAKED_INITIAL \
   -I. $SRCS support/rawwad.c "$STUB" -o gentables -lm -lpthread

#
# No -strikemap: it drops maps from the tables, and a sprite is recorded
# only if the generator *reached* it, so striking maps silently shrinks
# the WAD past what the engine's sprite table still demands.
#
echo "generating tables"
./gentables > support/bakedoutput.txt

echo "shrinking the WAD"
cd support
grep -a ADD_SPRITE bakedoutput.txt > add_sprites.txt
cat augment_sprites.txt >> add_sprites.txt
./shrinkwad stripchoice.txt add_sprites.txt rawwad_use.c rawwad_use.h >/dev/null

echo
echo "generated:"
for f in baked_texture_data.c baked_map_data.c rawwad_use.c rawwad_use.h; do
    [ -f "$f" ] && printf '  %-24s %s bytes\n' "$f" "$(wc -c <"$f")"
done
echo
echo "now build the guest:  cmake --build <dir> --target guest-doom"
