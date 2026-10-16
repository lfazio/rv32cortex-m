#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
#
# ppc-build-quake.sh - build Quake as an e200z7 guest.
#
#   scripts/ppc-build-quake.sh <quake-src-dir> <pak0.pak> [out.elf]
#
# The PowerPC counterpart of the `guest-quake` target (RV32) and of
# g4mh-build-quake.sh, and a script for the reason ppc-build-doom.sh is
# one: the guest links a runtime this tree's CMake does not configure --
# picolibc and compiler-rt from scripts/ppc-build-runtime.sh, run first.
#
# **Quake is GPL-2.0 and this tree is Apache-2.0.** Nothing of it lives
# here; this file describes a build and does not contain one. The board
# -- port/boards/rv32cortexm/, whose sys.c and libc.c carry a PowerPC
# branch beside the RISC-V and CC-RH ones -- lives in the port itself.
#
# Big-endian needs nothing here, unlike DOOM: Quake reads every file
# format through LittleLong and friends, chosen at compile time from
# __BYTE_ORDER__, which GCC defines for this target -- id shipped it on
# big-endian machines. There are no baked tables to convert.
#
# **The e200z7 has an FPU, and GCC cannot use it.** EFPU2 is single
# precision in the GPRs (efsadd, efsmul, ...), and GCC dropped its SPE
# back end in version 9, so a GCC build is soft-float: every float
# operation a call into compiler-rt. Profiled mid-demo that was 63% of
# everything the guest executed, __addsf3 alone 35%. LLVM still has the
# back end, with `-mefpu2` for exactly this core's half of it -- single
# precision in hardware, double through the same compiler-rt calls,
# because EFPU2 has no double -- and its calling convention is the
# soft-float one, so the GCC-built picolibc and compiler-rt link
# unchanged. So the engine is compiled with clang by default:
#
#   PPC_FPU=efpu2   clang --target=powerpc-unknown-eabi -mcpu=e500
#                   -mspe -mefpu2; PPC_CLANG names the compiler
#   PPC_FPU=soft    the cross GCC, -msoft-float, as the runtime is
#
# Plain -mspe would not do: it emits efd* for double, which this core
# does not have.
#
# **The PAK goes in through the assembler, not the compiler.** The
# RV32 build turns it into a C array, which is 93 MB of source for an
# 18.7 MB file; `.incbin` takes the bytes as they are, as rlink's
# -Binary does for G4MH. The length is a word beside it, because fio.c
# reads quake_pak_len and sizeof on an extern array is not available.

set -eu

ROOT=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
SRC=${1:?usage: ppc-build-quake.sh <quake-src-dir> <pak0.pak> [out.elf]}
PAK=${2:?usage: ppc-build-quake.sh <quake-src-dir> <pak0.pak> [out.elf]}
OUT=${3:-$ROOT/build/guest/quake-ppc.elf}
RT=${PPC_RUNTIME:-$ROOT/build/ppc-runtime}

CC=${PPC_CC:-powerpc64-linux-gnu-gcc}
FLAGS="-m32 -mbig-endian -mcpu=8540 -msoft-float -msdata=none -mlong-double-64"
FPU=${PPC_FPU:-efpu2}

if [ ! -f "$RT/lib/libc.a" ] || [ ! -f "$RT/lib/libbuiltins.a" ]; then
    echo "ppc-build-quake: no runtime at $RT -- run scripts/ppc-build-runtime.sh" >&2
    exit 2
fi
if [ ! -f "$SRC/winquake/host.c" ]; then
    echo "ppc-build-quake: no Quake source at $SRC" >&2
    exit 2
fi
if ! grep -q '__powerpc__' "$SRC/port/boards/rv32cortexm/sys.c" 2>/dev/null; then
    echo "ppc-build-quake: the port's rv32cortexm board has no PowerPC branch" >&2
    echo "                 ($SRC/port/boards/rv32cortexm/sys.c)" >&2
    exit 2
fi
if [ ! -f "$PAK" ]; then
    echo "ppc-build-quake: no game data at $PAK" >&2
    echo "                 the shareware pak0.pak is enough" >&2
    exit 2
fi

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

case $FPU in
efpu2)
    CLANG=${PPC_CLANG:-clang}
    if ! command -v "$CLANG" >/dev/null 2>&1; then
        echo "ppc-build-quake: no $CLANG for the EFPU2 build" >&2
        echo "                 set PPC_CLANG, or PPC_FPU=soft for GCC" >&2
        exit 2
    fi
    COMPILE="$CLANG --target=powerpc-unknown-eabi -mcpu=e500 -mspe -mefpu2"
    CCINC=$("$CLANG" -print-resource-dir)/include
    DIALECT="-Wno-error=implicit-function-declaration -Wno-error=implicit-int
             -Wno-error=int-conversion -Wno-error=incompatible-pointer-types
             -Wno-error=incompatible-function-pointer-types"
    ;;
soft)
    COMPILE="$CC $FLAGS"
    CCINC=$("$CC" -print-file-name=include)
    DIALECT=-fpermissive
    ;;
*)
    echo "ppc-build-quake: PPC_FPU is efpu2 or soft, not $FPU" >&2
    exit 2
    ;;
esac
CFLAGS="-O2 -fno-pic -fsigned-char -std=gnu99 $DIALECT -w
        -fno-common -nostdinc -isystem $CCINC -isystem $RT/include
        -DWINQUAKE_ENABLE_LOGGING -DWINQUAKE_LOGGING_EXTERNAL
        -I$SRC/include -I$SRC/winquake"

#
# The four excluded files are alternative network drivers. net_none.c is
# *kept*: it is the driver table for a build with none, which
# net_main.c indexes unconditionally.
#
echo "compiling Quake ($FPU)"
n=0
for f in "$SRC"/winquake/*.c "$SRC"/port/*.c \
         "$SRC"/port/boards/rv32cortexm/*.c; do
    b=$(basename "$f" .c)
    case $b in
    net_bsd|net_dgrm|net_udp|net_vcr)
        continue
        ;;
    esac
    # shellcheck disable=SC2086
    $COMPILE $CFLAGS -c "$f" -o "$WORK/$b.o"
    n=$((n + 1))
done
echo "  $n translation units"

paklen=$(wc -c <"$PAK")
cp "$PAK" "$WORK/pak0.bin"
cat >"$WORK/pak.S" <<EOF
    .section .rodata.pak, "a"
    .balign 4
    .globl  quake_pak
quake_pak:
    .incbin "$WORK/pak0.bin"
    .balign 4
    .globl  quake_pak_len
quake_pak_len:
    .long   $paklen
EOF
# shellcheck disable=SC2086
"$CC" $FLAGS -c "$WORK/pak.S" -o "$WORK/pak.o"
echo "  PAK is $paklen bytes"

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
echo "  emu-host --jit --ram 0x4000000 $OUT"
