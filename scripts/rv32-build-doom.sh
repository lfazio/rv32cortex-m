#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
#
# rv32-build-doom.sh - build DOOM as an RV32 guest, and the runner for it.
#
#   scripts/rv32-build-doom.sh [doom-src-dir] [out.bin]
#
# The RV32 counterpart of g4mh-build-doom.sh and ppc-build-doom.sh. Those
# are scripts because their compilers are not this build's; RV32's is,
# and the recipe is tests/guest/doom.cmake. What the recipe could not do
# alone is the order: it needs EMU_DOOM, a guest RAM of 8 MiB, the source,
# and the host generation stage (scripts/doom-gentables.sh) run against
# that source *between* the configure and the image -- four things to
# remember, in three commands, before anything runs. This is the one
# command.
#
# **DOOM is GPL-2.0 and this tree is Apache-2.0.** Nothing of DOOM's lives
# here; this file describes a build and does not contain one. Without a
# source directory the recipe fetches the port (DOOM_REPO, DOOM_BRANCH in
# the cache) into the build directory.
#
# The generator runs once per source tree. Its *last* output is the shrunk
# WAD, support/rawwad_use.c, so a tree that has one is left alone; the
# tables it writes earlier are not the marker, because a run that dies
# part way leaves them behind. DOOM_REGEN=1 runs it again -- after
# changing the port's own sources, say.
#
# Environment:
#   DOOM_BUILD   build directory (default build/doom)
#   DOOM_REGEN   1 to rerun the host generation stage
#   EMU_SDL      ON or OFF; default ON when pkg-config finds sdl3
#
# Needs riscv64-unknown-elf-gcc and, for the host stage, gcc-multilib:
# DOOM is 32-bit only, so the generator is built with `gcc -m32`.

set -eu

ROOT=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
SRC=${1:-}
OUT=${2:-}
BUILD=${DOOM_BUILD:-$ROOT/build/doom}

if [ -n "$SRC" ]; then
    SRC=$(CDPATH='' cd -- "$SRC" && pwd)
    if [ ! -f "$SRC/src/d_main.c" ]; then
        echo "rv32-build-doom: no DOOM source at $SRC" >&2
        exit 2
    fi
fi

if [ -z "${EMU_SDL:-}" ]; then
    EMU_SDL=OFF
    if pkg-config --exists sdl3 2>/dev/null; then
        EMU_SDL=ON
    fi
fi

mkdir -p "$BUILD"
echo "==> configure $BUILD"
cmake -S "$ROOT" -B "$BUILD" -DEMU_PLATFORM=host -DCMAKE_BUILD_TYPE=Release \
    -DEMU_DOOM=ON -DEMU_GUEST_RAM_KIB=8192 -DEMU_SDL="$EMU_SDL" \
    ${SRC:+-DDOOM_DIR="$SRC"} >"$BUILD/configure.log" 2>&1 ||
    { echo "rv32-build-doom: configure failed; see $BUILD/configure.log" >&2; exit 1; }

# The source the recipe settled on: the one given, or the one it fetched.
DOOM_SRC=$(sed -n 's/^DOOM_DIR:PATH=//p' "$BUILD/CMakeCache.txt")
if [ ! -f "$DOOM_SRC/src/d_main.c" ]; then
    echo "rv32-build-doom: no DOOM source at $DOOM_SRC after configuring" >&2
    exit 1
fi

if [ "${DOOM_REGEN:-0}" = "1" ] || [ ! -f "$DOOM_SRC/src/support/rawwad_use.c" ]; then
    echo "==> host stage: baking tables into $DOOM_SRC/src/support"
    "$ROOT/scripts/doom-gentables.sh" "$DOOM_SRC"
    # The recipe asks the same question at configure time and leaves the
    # guest-doom target out until the answer is yes: ask it again.
    echo "==> configure again, now that the tables exist"
    cmake "$BUILD" >>"$BUILD/configure.log" 2>&1 ||
        { echo "rv32-build-doom: configure failed; see $BUILD/configure.log" >&2; exit 1; }
else
    echo "==> host stage: already run for $DOOM_SRC (DOOM_REGEN=1 to redo)"
fi

echo "==> build guest-doom and emu-host"
cmake --build "$BUILD" --target guest-doom emu-host >"$BUILD/build.log" 2>&1 ||
    { echo "rv32-build-doom: build failed; see $BUILD/build.log" >&2; exit 1; }

IMG=$BUILD/guest/doom.bin
if [ ! -f "$IMG" ]; then
    echo "rv32-build-doom: the build ran and $IMG is not there" >&2
    exit 1
fi
if [ -n "$OUT" ]; then
    cp "$IMG" "$OUT"
    IMG=$OUT
fi

echo
echo "image: $IMG ($(wc -c <"$IMG") bytes)"
echo "run:   $BUILD/emu-host --jit --ram 0x2000000 --timer-hz 6 $IMG"
