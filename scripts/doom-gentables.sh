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
# Every platform layer is excluded, not just the rv32 one: they address
# memory that exists only inside the emulator, and i_video_emu.c is the
# shared driver both frontends use. The generator runs on the *host* and
# needs none of them -- i_video_console.c stands in.
SRCS=$(ls ./*.c | grep -vE '/(i_video|i_video_emu|i_video_rv32|i_platform_rv32|i_platform_g4mh|XDriver)\.c$')

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

#
# **Every sprite in sprnames[], not the ones the generator reached.**
#
# shrinkwad strips any sprite absent from this list, and the obvious
# input -- the ADD_SPRITE lines the generator prints as it plays -- is a
# record of what one play-through *touched*: 39 of 138 here. But
# R_InitSpriteDefs runs at start-up and walks the whole of sprnames,
# calling I_Error on the first entry with no lumps. So a list derived
# from play can never be sufficient, and which sprite it dies on is an
# accident of ordering rather than a clue: it was MISF, the rocket
# launcher's muzzle flash, because you would have to fire one during
# generation for it to be recorded.
#
# That is why augment_sprites.txt exists upstream -- it lists PISG, the
# pistol flash, for exactly this reason -- but hand-listing the other 98
# is the same mistake one entry at a time.
#
# The names come from info.c, which is where the engine's own table is,
# so the list cannot drift from what R_InitSpriteDefs will demand.
# shrinkwad reads "%127s %d %d %15s" and uses only the *index*; the
# first field must not be 1, which is what marks the sprite in use.
#
# It costs about a megabyte of WAD -- 3,248,919 bytes against 4,196,020
# -- and that is the whole of what sprite stripping was buying.
#
awk '
    /sprnames/      { in_names = 1 }
    in_names && /}/ { exit }
    in_names {
        while (match($0, /"[A-Z0-9]{4}"/)) {
            name = substr($0, RSTART + 1, 4)
            printf "ADD_SPRITE 2 %d %s\n", n++, name
            $0 = substr($0, RSTART + RLENGTH)
        }
    }
' ../info.c > add_sprites.txt

sprites=$(wc -l < add_sprites.txt)
if [ "$sprites" -lt 100 ]; then
    echo "error: only $sprites sprites found in info.c; shrinkwad would" >&2
    echo "       strip the rest and DOOM would die in R_InitSpriteDefs" >&2
    exit 1
fi
echo "  keeping all $sprites sprites"

./shrinkwad stripchoice.txt add_sprites.txt rawwad_use.c rawwad_use.h >/dev/null

echo
echo "generated:"
for f in baked_texture_data.c baked_map_data.c rawwad_use.c rawwad_use.h; do
    [ -f "$f" ] && printf '  %-24s %s bytes\n' "$f" "$(wc -c <"$f")"
done
echo
echo "now build the guest:  cmake --build <dir> --target guest-doom"
