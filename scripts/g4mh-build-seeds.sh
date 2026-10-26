#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
#
# g4mh-build-seeds.sh - assemble the G4MH fuzz seeds with Renesas CC-RH.
#
#   scripts/g4mh-build-seeds.sh [--check]
#
# Each tests/fuzz/seeds/g4mh-jit/NAME.asm becomes NAME.bin beside it,
# linked at 0x2000_0000 where fuzz-g4mh-jit runs its program. The .bin
# files are committed, as tests/guest/g4mh's are, because CC-RH needs the
# `ccrh` image (docs/renesas/Dockerfile) and a checkout cannot assume it;
# scripts/fuzz.sh reads the .bin files and never the sources.
#
# **Rebuild and commit the .bin files when an .asm changes.** `--check`
# rebuilds them all and compares against what is committed, without
# replacing anything.

set -eu

ROOT=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
IMAGE=${CCRH_IMAGE:-ccrh:latest}
CCRH_BIN=/usr/local/Renesas/CC-RH/V2.08.00/bin
DIR=$ROOT/tests/fuzz/seeds/g4mh-jit

check_only=0
if [ "${1:-}" = "--check" ]; then
    check_only=1
fi

if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
    echo "g4mh-build-seeds: no such image: $IMAGE" >&2
    echo "build it from docs/renesas/Dockerfile, or set CCRH_IMAGE" >&2
    exit 2
fi

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cp "$DIR"/*.asm "$work"/

# One container for the lot: each start is seconds, each assembly is not.
docker run --rm -v "$work":/w -w /w "$IMAGE" sh -c \
    "PATH=$CCRH_BIN:\$PATH; for f in *.asm; do n=\${f%.asm}; \
     asrh -Xcommon=rh850 -Xcpu=g4mh \$f && \
     rlink \$n.obj -form=binary -output=\$n.bin -start=.text/20000000 \
     || exit 1; done" >"$work/log" 2>&1 || {
    echo "g4mh-build-seeds: CC-RH failed:" >&2
    cat "$work/log" >&2
    exit 1
}

status=0
for src in "$DIR"/*.asm; do
    n=$(basename "$src" .asm)
    if [ ! -s "$work/$n.bin" ]; then
        echo "g4mh-build-seeds: no output for $n; CC-RH said:" >&2
        cat "$work/log" >&2
        exit 1
    fi
    if [ "$check_only" -eq 1 ]; then
        if ! cmp -s "$work/$n.bin" "$DIR/$n.bin"; then
            echo "g4mh-build-seeds: $n.bin differs from a fresh build" >&2
            status=1
        fi
    else
        cp "$work/$n.bin" "$DIR/$n.bin"
        echo "g4mh-build-seeds: wrote $n.bin ($(wc -c <"$DIR/$n.bin") bytes)"
    fi
done
[ "$check_only" -eq 0 ] || [ "$status" -ne 0 ] ||
    echo "g4mh-build-seeds: every .bin is up to date"
exit "$status"
