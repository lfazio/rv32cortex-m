#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
#
# g4mh-build-coremark.sh - CoreMark for G4MH, with Renesas CC-RH.
#
#   scripts/g4mh-build-coremark.sh <coremark-src> [iterations] [out.bin]
#
# <coremark-src> is the checkout CMake fetches (build/<dir>/coremark-src);
# the port is tests/guest/coremark's, the same as every other frontend's,
# with a branch for the LTSC. Iterations default to 40, the count the
# ARMv7-M guest uses: about eleven million instructions, enough to run
# past a 32 KB code cache many times over and short enough for a board.
#
# CC-RH runs in the `ccrh` image (docs/renesas/Dockerfile), as for
# scripts/g4mh-build-guest.sh; the result is a flat binary linked at
# 0x80000000, guest RAM, with .bss after .data -- a board copies the image
# there and gives the guest nothing else.
#
# Run it with:
#   emu-host --frontend g4mh --load 0x80000000 [--jit] <out.bin>
set -eu
ROOT=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
IMAGE=${CCRH_IMAGE:-ccrh:latest}
CCRH_BIN=/usr/local/Renesas/CC-RH/V2.08.00/bin
CCRH_LIB=/usr/local/Renesas/CC-RH/V2.08.00/lib/v850e3v5
src=${1:?usage: g4mh-build-coremark.sh <coremark-src> [iterations] [out.bin]}
iter=${2:-40}
out=${3:-$ROOT/build/g4mh-coremark.bin}

if [ ! -f "$src/core_main.c" ]; then
    echo "g4mh-build-coremark: no CoreMark at $src" >&2
    exit 2
fi
if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
    echo "g4mh-build-coremark: no such image: $IMAGE" >&2
    echo "build it from docs/renesas/Dockerfile, or set CCRH_IMAGE" >&2
    exit 2
fi

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
for f in core_main.c core_list_join.c core_matrix.c core_state.c \
         core_util.c coremark.h; do
    cp "$src/$f" "$work/"
done
cp "$ROOT/tests/guest/coremark/core_portme.c" \
   "$ROOT/tests/guest/coremark/core_portme.h" \
   "$ROOT/tests/guest/g4mh/crt0.asm" "$work/"

docker run --rm -v "$work":/w -w /w "$IMAGE" sh -c \
    "PATH=$CCRH_BIN:\$PATH; set -e; \
     asrh -Xcommon=rh850 -Xcpu=g4mh crt0.asm; \
     for f in core_main core_list_join core_matrix core_state core_util \
              core_portme; do \
         ccrh -Xcommon=rh850 -Xcpu=g4mh -Odefault -lang=c99 \
              -DPERFORMANCE_RUN=1 -DITERATIONS=$iter -c \$f.c; \
     done; \
     rlink ./*.obj -library=$CCRH_LIB/rhs8n.lib -entry=_entry \
         -form=binary -output=coremark.bin \
         -start=.text_entry,.text,.const,.data,.bss/80000000" \
    >"$work/log" 2>&1 || {
    echo "g4mh-build-coremark: CC-RH failed:" >&2
    grep -vi 'evaluation period' "$work/log" >&2
    exit 1
}
if [ ! -f "$work/coremark.bin" ]; then
    echo "g4mh-build-coremark: link produced nothing" >&2
    grep -vi 'evaluation period' "$work/log" >&2
    exit 1
fi
mkdir -p "$(dirname "$out")"
cp "$work/coremark.bin" "$out"
echo "wrote $out ($(wc -c <"$out") bytes), $iter iterations"
