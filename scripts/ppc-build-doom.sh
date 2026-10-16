#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
#
# ppc-build-doom.sh - build DOOM as an e200z7 guest.
#
#   scripts/ppc-build-doom.sh <doom-src-dir> [out.elf]
#
# The PowerPC counterpart of the `guest-doom` target (RV32) and of
# g4mh-build-doom.sh, and a script for the reason that one is: the guest
# is built against a runtime this tree's CMake does not configure --
# picolibc and compiler-rt from scripts/ppc-build-runtime.sh, run first.
#
# **DOOM is GPL-2.0 and this tree is Apache-2.0.** Nothing of DOOM's
# lives here; this file describes a build and does not contain one. The
# port layer -- i_platform_ppc.c and the shared i_video_emu.c -- lives in
# the DOOM port, because a file implementing DOOM's interface and linking
# into DOOM's binary is part of that work.
#
# **The first big-endian target the port has had**, and that is most of
# what this script does beyond compiling:
#
#   * DOOM's own SHORT() and LONG() swap the WAD's little-endian fields
#     wherever __BIG_ENDIAN__ is defined, which GCC defines here -- and
#     m_swap.c had the inverted guard id's released source carries, so
#     the port's copy needs the fix to link at all.
#   * The baked tables were dumped from a little-endian host. Most of
#     them are C integers and mean the same on either, but two families
#     are raw structure bytes: each texture_t (three shorts after the
#     name, then three ints a patch) and each node_t (twelve ints, two
#     shorts). Those are byte-swapped field by field into copies here,
#     as g4mh-build-doom.sh rewrites a copy of the empty arrays -- the
#     generated files in the port are left as they are.
#
# Run it with the window and the input the RV32 build uses:
#
#   emu-host --jit --ram 0x2000000 --timer-hz 6 <out.elf>

set -eu

ROOT=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
SRC=${1:?usage: ppc-build-doom.sh <doom-src-dir> [out.elf]}/src
OUT=${2:-$ROOT/build/guest/doom-ppc.elf}
RT=${PPC_RUNTIME:-$ROOT/build/ppc-runtime}

CC=${PPC_CC:-powerpc64-linux-gnu-gcc}
FLAGS="-m32 -mbig-endian -mcpu=8540 -msoft-float -msdata=none -mlong-double-64"

if [ ! -f "$RT/lib/libc.a" ] || [ ! -f "$RT/lib/libbuiltins.a" ]; then
    echo "ppc-build-doom: no runtime at $RT -- run scripts/ppc-build-runtime.sh" >&2
    exit 2
fi
if [ ! -f "$SRC/d_main.c" ]; then
    echo "ppc-build-doom: no DOOM source at $SRC" >&2
    exit 2
fi
if [ ! -f "$SRC/i_platform_ppc.c" ]; then
    echo "ppc-build-doom: the port has no PowerPC platform layer" >&2
    echo "                ($SRC/i_platform_ppc.c)" >&2
    exit 2
fi
for f in support/rawwad_use.c support/baked_texture_data.c \
         support/baked_map_data.c; do
    if [ ! -f "$SRC/$f" ]; then
        echo "ppc-build-doom: $f is missing -- run scripts/doom-gentables.sh" >&2
        exit 2
    fi
done

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

echo "converting the baked tables to big-endian"
python3 - "$SRC/support" "$WORK" <<'PYEOF'
import re, struct, sys

src, out = sys.argv[1], sys.argv[2]
ARRAY = re.compile(r'(static const unsigned char (\w+)\[\] = \{)([^}]*)(\};)')

def swap(name, data):
    b = bytearray(data)
    if name.startswith('textureData'):
        # texture_t: char name[8]; short width, height, patchcount;
        # then, from offset 16, texpatch_t {int originx, originy, patch}.
        if len(b) < 16:
            return b, 0     # a texture with no data, which nothing reads
        count = struct.unpack_from('<h', b, 12)[0]
        if len(b) < 16 + 12 * count:
            sys.exit(f'{name}: {len(b)} bytes for {count} patches')
        for off in (8, 10, 12):
            b[off:off + 2] = b[off:off + 2][::-1]
        for p in range(count):
            for k in range(3):
                off = 16 + 12 * p + 4 * k
                b[off:off + 4] = b[off:off + 4][::-1]
        return b, 1
    if name.startswith('__nodes'):
        # node_t: fixed_t x, y, dx, dy, bbox[2][4]; unsigned short children[2].
        if len(b) % 52:
            sys.exit(f'{name}: {len(b)} bytes is not a whole number of nodes')
        for n in range(0, len(b), 52):
            for k in range(12):
                off = n + 4 * k
                b[off:off + 4] = b[off:off + 4][::-1]
            for off in (n + 48, n + 50):
                b[off:off + 2] = b[off:off + 2][::-1]
        return b, len(b) // 52
    return b, 0

for fname in ('baked_texture_data.c', 'baked_map_data.c'):
    text = open(f'{src}/{fname}').read()
    done = [0, 0]

    def repl(m):
        data = [int(x, 16) for x in re.findall(r'0x[0-9a-fA-F]+', m.group(3))]
        b, n = swap(m.group(2), data)
        if not n:
            return m.group(0)   # bytes, or nothing to swap: as it was
        done[0] += 1
        done[1] += n
        body = ', '.join(f'0x{x:02x}' for x in b)
        return f'{m.group(1)} {body}, {m.group(4)}'

    text = ARRAY.sub(repl, text)
    # The generator writes `[] = {};` for a texture with no data, a GCC
    # extension; nothing takes sizeof it.
    text = text.replace('[] = {};', '[] = {0};')
    # Their includes are relative to support/, and the copies are not
    # there; -I names the source directory instead.
    text = text.replace('#include "../', '#include "')
    open(f'{out}/{fname}', 'w').write(text)
    print(f'  {fname}: {done[0]} arrays, {done[1]} structures swapped')
PYEOF

GCCINC=$("$CC" -print-file-name=include)
CFLAGS="$FLAGS -O2 -fno-pic -fsigned-char -std=gnu99 -fpermissive -w
        -nostdinc -isystem $GCCINC -isystem $RT/include
        -DNORMALUNIX -DEMU_GUEST -Dalloca=__builtin_alloca -I$SRC"

echo "compiling DOOM"
n=0
for f in "$SRC"/*.c; do
    b=$(basename "$f" .c)
    case $b in
    i_video|i_video_console|XDriver|i_net|i_sound|os_generic|i_platform_rv32|i_platform_g4mh)
        continue
        ;;
    esac
    # shellcheck disable=SC2086
    "$CC" $CFLAGS -c "$f" -o "$WORK/$b.o"
    n=$((n + 1))
done
for f in baked_texture_data baked_map_data; do
    # shellcheck disable=SC2086
    "$CC" $CFLAGS -c "$WORK/$f.c" -o "$WORK/$f.o"
    n=$((n + 1))
done
# shellcheck disable=SC2086
"$CC" $CFLAGS -c "$SRC/support/rawwad_use.c" -o "$WORK/rawwad_use.o"
n=$((n + 1))
echo "  $n translation units"

echo "linking"
mkdir -p "$(dirname "$OUT")"
# shellcheck disable=SC2086
"$CC" $FLAGS -fno-pic -static -nostdlib -nostartfiles -Wl,--build-id=none \
    -Wl,-z,max-page-size=0x1000 -T "$ROOT/tests/guest/ppc/bigguest.ld" \
    "$ROOT/tests/guest/ppc/crt0.S" "$WORK"/*.o \
    -L"$RT/lib" -lc -lm "$RT/lib/libbuiltins.a" -lc -o "$OUT"

echo "wrote $OUT ($(wc -c <"$OUT") bytes)"
echo
echo "run it with:"
echo "  emu-host --jit --ram 0x2000000 --timer-hz 6 $OUT"
