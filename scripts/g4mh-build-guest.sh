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
# An entry stub, linked first.
#
# The emulator starts executing at the load address and a flat binary
# carries no entry point, so whatever the linker happens to place first
# is what runs. CC-RH placed a static helper there and `main` landed at
# +0x5c, so the program began in the middle of puts_ and ran to the
# instruction cap. Passing --entry to the runner would fix that run and
# break on the next edit, since the offset moves with the code; a stub in
# its own section, named first in -start, cannot move.
#
cat > "$work/entry.asm" <<'ASM'
	.section .text_entry, text
	.public _entry
	.extern _main
_entry:
	jr _main
ASM

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
