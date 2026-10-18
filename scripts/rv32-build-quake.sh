#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
#
# rv32-build-quake.sh - build Quake as an RV32 guest, and the runner for it.
#
#   scripts/rv32-build-quake.sh <quake-src-dir|-> <pak0.pak> [out.bin]
#
# The RV32 counterpart of g4mh-build-quake.sh and ppc-build-quake.sh, with
# the same arguments. `-` for the source lets the recipe fetch the port
# (QUAKE_REPO, QUAKE_BRANCH in the cache) into the build directory.
#
# RV32's compiler is this build's, so the recipe is tests/guest/quake.cmake;
# what it needs and does not default is the game data and two sizes, both
# of which it checks and skips over rather than failing on:
#
#   EMU_GUEST_ROM_MIB=32    the PAK is 18.7 MiB and lands in .rodata
#   EMU_GUEST_RAM_KIB=32768 the heap is a 16 MiB static array
#
# **Quake is GPL-2.0 and this tree is Apache-2.0**, and the PAK is id
# Software's: the shareware one is freely redistributable and still not
# this repository's to ship. Nothing of either lives here.
#
# Environment:
#   QUAKE_BUILD  build directory (default build/quake)
#   EMU_SDL      ON or OFF; default ON when pkg-config finds sdl3
#
# Needs riscv64-unknown-elf-gcc.

set -eu

ROOT=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
SRC=${1:?usage: rv32-build-quake.sh <quake-src-dir|-> <pak0.pak> [out.bin]}
PAK=${2:?usage: rv32-build-quake.sh <quake-src-dir|-> <pak0.pak> [out.bin]}
OUT=${3:-}
BUILD=${QUAKE_BUILD:-$ROOT/build/quake}

if [ "$SRC" = "-" ]; then
    SRC=
else
    SRC=$(CDPATH='' cd -- "$SRC" && pwd)
    if [ ! -f "$SRC/winquake/host.c" ]; then
        echo "rv32-build-quake: no Quake source at $SRC" >&2
        exit 2
    fi
fi
if [ ! -f "$PAK" ]; then
    echo "rv32-build-quake: no game data at $PAK" >&2
    exit 2
fi
PAK=$(CDPATH='' cd -- "$(dirname -- "$PAK")" && pwd)/$(basename -- "$PAK")

if [ -z "${EMU_SDL:-}" ]; then
    EMU_SDL=OFF
    if pkg-config --exists sdl3 2>/dev/null; then
        EMU_SDL=ON
    fi
fi

mkdir -p "$BUILD"
echo "==> configure $BUILD"
cmake -S "$ROOT" -B "$BUILD" -DEMU_PLATFORM=host -DCMAKE_BUILD_TYPE=Release \
    -DEMU_QUAKE=ON -DQUAKE_PAK="$PAK" \
    -DEMU_GUEST_ROM_MIB=32 -DEMU_GUEST_RAM_KIB=32768 -DEMU_SDL="$EMU_SDL" \
    ${SRC:+-DQUAKE_DIR="$SRC"} >"$BUILD/configure.log" 2>&1 ||
    { echo "rv32-build-quake: configure failed; see $BUILD/configure.log" >&2; exit 1; }

echo "==> build guest-quake and emu-host"
cmake --build "$BUILD" --target guest-quake emu-host >"$BUILD/build.log" 2>&1 ||
    { echo "rv32-build-quake: build failed; see $BUILD/build.log" >&2; exit 1; }

IMG=$BUILD/guest/quake.bin
if [ ! -f "$IMG" ]; then
    echo "rv32-build-quake: the build ran and $IMG is not there" >&2
    grep -m3 -i 'quake' "$BUILD/configure.log" >&2 || true
    exit 1
fi
if [ -n "$OUT" ]; then
    cp "$IMG" "$OUT"
    IMG=$OUT
fi

echo
echo "image: $IMG ($(wc -c <"$IMG") bytes)"
echo "run:   $BUILD/emu-host --jit --ram 0x4000000 $IMG"
