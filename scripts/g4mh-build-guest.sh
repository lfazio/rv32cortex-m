#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
#
# g4mh-build-guest.sh - compile a G4MH guest with Renesas CC-RH.
#
#   scripts/g4mh-build-guest.sh tests/guest/g4mh/barrier3.c [out.bin]
#
# CC-RH is the only compiler that emits G4MH, and it is not something a
# checkout can assume: it needs the `ccrh` image described in
# docs/renesas/Dockerfile. That is why the resulting .bin is committed
# beside its source rather than built by CMake -- the same arrangement
# guest.bin already has, and the reason tests/guest/ppc/ can build from
# source while this cannot.
#
# **Rebuild and commit the .bin when the .c changes.** Nothing checks
# that they agree, which is a real hazard: a stale binary passes every
# test while the source says something else. `--check` re-builds and
# diffs against the committed file without replacing it, which is what
# CI would run if this had any.

set -eu

ROOT=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
IMAGE=${CCRH_IMAGE:-ccrh:latest}
CCRH_BIN=/usr/local/Renesas/CC-RH/V2.08.00/bin

check_only=0
if [ "${1:-}" = "--check" ]; then
    check_only=1
    shift
fi

src=${1:?usage: g4mh-build-guest.sh [--check] <source.c> [out.bin]}
out=${2:-${src%.c}.bin}

if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
    echo "g4mh-build-guest: no such image: $IMAGE" >&2
    echo "build it from docs/renesas/Dockerfile, or set CCRH_IMAGE" >&2
    exit 2
fi

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cp "$src" "$work/in.c"

#
# The vector table, linked first.
#
# The emulator starts executing at the load address and a flat binary
# carries no entry point, so whatever the linker happens to place first
# is what runs. CC-RH placed a static helper there and `main` landed at
# +0x5c, so the program began in the middle of puts_ and ran to the
# instruction cap. Passing --entry to the runner would fix that run and
# break on the next edit, since the offset moves with the code; a stub in
# its own section, named first in -start, cannot move.
#
# It is a *table* rather than a branch because a trap only reports if
# something catches it. RBASE resets to the load address, so these
# offsets are the architecture's handler addresses -- and a guest without
# them sends, say, a reserved instruction to load+0x60, which in a flat
# image is ordinary code twenty bytes further on. Execution carries on
# with the faulting instruction skipped and nothing written anywhere,
# which is worse than a hang because the guest keeps producing plausible
# output. That has cost this project three separate multi-session hunts,
# on two different frontends, and it is the reason every G4MH guest now
# carries this whether it wants it or not.
#
# Self-contained on purpose: it calls nothing in the guest, because it is
# shared by every guest and cannot depend on a symbol any one of them
# happens to define. So it writes to the NS16550 directly, which is the
# same console the C code uses.
#
# 32 slots of 16 bytes is 512, which is exactly RBASE's alignment.
#
#
# The vector table and C start-up, which every G4MH guest gets. It lived
# here as a heredoc until a second script needed the same thing; what it
# contains and why each line is in it is at the top of the file.
#
cp "$ROOT/tests/guest/g4mh/crt0.asm" "$work/entry.asm"

#
# -Xcpu=g4mh picks the core; -Osize keeps the image small. The section
# order in -start is what puts the stub at 0x80000000, which is the host
# runner's default --load address.
#
docker run --rm -v "$work":/w -w /w "$IMAGE" sh -c \
    "PATH=$CCRH_BIN:\$PATH; \
     asrh -Xcommon=rh850 -Xcpu=g4mh entry.asm && \
     ccrh -Xcommon=rh850 -Xcpu=g4mh -Osize -c in.c -oin.obj && \
     rlink entry.obj in.obj -entry=_entry -form=binary -output=out.bin \
        -start=.text_entry,.text,.const,.data/80000000" \
    >"$work/log" 2>&1 || {
    echo "g4mh-build-guest: CC-RH failed:" >&2
    cat "$work/log" >&2
    exit 1
}

if [ ! -f "$work/out.bin" ]; then
    echo "g4mh-build-guest: no output produced; CC-RH said:" >&2
    cat "$work/log" >&2
    exit 1
fi

if [ "$check_only" -eq 1 ]; then
    if cmp -s "$work/out.bin" "$out"; then
        echo "g4mh-build-guest: $out is up to date"
    else
        echo "g4mh-build-guest: $out differs from a fresh build of $src" >&2
        exit 1
    fi
else
    cp "$work/out.bin" "$out"
    echo "g4mh-build-guest: wrote $out ($(wc -c <"$out") bytes)"
fi
