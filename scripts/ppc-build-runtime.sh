#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
#
# ppc-build-runtime.sh - a C library and a compiler runtime for the
# e200z7's compiled guests.
#
#   scripts/ppc-build-runtime.sh [out-dir]      # default build/ppc-runtime
#
# The PowerPC guests are built with Debian's powerpc64 cross compiler in
# 32-bit mode, and nothing 32-bit ships beside it: no libgcc, so no
# 64-bit division and no soft-float double, and no C library. The small
# guests live without either (-nostdlib); a game does not.
#
# Both are fetched and built here, never vendored, and neither is GPL:
#
#   picolibc            BSD-style; the C library the RV32 guests already
#                       use, as Debian's picolibc-riscv64-unknown-elf
#   compiler-rt         Apache-2.0 WITH LLVM-exception; the builtins
#   (lib/builtins)      only -- integer helpers and soft-float -- which
#                       is what GCC calls where libgcc would be
#
# Built -m32 -mbig-endian -mcpu=8540 -msoft-float -msdata=none, the
# compiled guests' flags, and -mlong-double-64 so a long double is a
# double rather than IBM's 128-bit pair, which neither library builds for
# this target. picolibc without thread-local storage: errno is a global,
# because nothing sets up r2 as a thread pointer.
#
# Output: <out>/include, and <out>/lib/{libc.a,libm.a,libbuiltins.a}.
# Link a guest with -lc -lm libbuiltins.a -lc and the start-up in
# tests/guest/ppc/crt0.S; it must define __heap_start and __heap_end for
# picolibc's sbrk, or supply its own.
#
# **And picolibc a second time, for the core's FPU**, in <out>/efpu2/
# (include/ and lib/libc.a), when a clang that can target EFPU2 is
# there -- PPC_CLANG names it. GCC cannot emit the e200z7's FPU, so
# the first build's sinf and logf are compiler-rt calls from end to
# end; this one's are efsmul and efsadd. Its C is clang's, with
# -mefpu2, and its calling convention the soft-float one, so it links
# with the same libbuiltins.a, which still does every double. Its
# assembly is the cross GCC's, as in the first build, for two reasons:
# setjmp.S includes GCC's ppc-asm.h and uses a `bclr+` clang's
# assembler rejects, and clang defines __SPE__, under which setjmp
# saves the registers with evstdd -- an SPE instruction this core
# model raises as illegal. EFPU2 uses only the low half of a GPR, so
# the 32-bit save GCC assembles is the right one. The clang half of
# libc.a contains no ev* and no efd* (the second would be double
# precision, which EFPU2 lacks); check with objdump -M e500 after a
# picolibc update.

set -eu

ROOT=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
OUT=${1:-$ROOT/build/ppc-runtime}
SRC=$OUT/src

PICOLIBC_REPO=https://github.com/picolibc/picolibc.git
PICOLIBC_TAG=${PICOLIBC_TAG:-1.8.10}
LLVM_REPO=https://github.com/llvm/llvm-project.git
LLVM_TAG=${LLVM_TAG:-llvmorg-19.1.7}

CC=${PPC_CC:-powerpc64-linux-gnu-gcc}
AR=${PPC_AR:-powerpc64-linux-gnu-ar}
FLAGS="-m32 -mbig-endian -mcpu=8540 -msoft-float -msdata=none -mlong-double-64"

for tool in "$CC" "$AR" meson ninja git; do
    command -v "$tool" >/dev/null 2>&1 || {
        echo "ppc-build-runtime: $tool not found" >&2
        exit 2
    }
done

mkdir -p "$SRC"

if [ ! -f "$SRC/picolibc/meson.build" ]; then
    echo "fetching picolibc $PICOLIBC_TAG"
    git clone -q --depth 1 -b "$PICOLIBC_TAG" "$PICOLIBC_REPO" "$SRC/picolibc"
fi
if [ ! -f "$SRC/llvm/compiler-rt/lib/builtins/int_lib.h" ]; then
    echo "fetching compiler-rt's builtins from $LLVM_TAG"
    git clone -q --depth 1 --filter=blob:none --sparse -b "$LLVM_TAG" \
        "$LLVM_REPO" "$SRC/llvm"
    git -C "$SRC/llvm" sparse-checkout set compiler-rt/lib/builtins
fi

# ------------------------------------------------------------------
# compiler-rt: the generic C builtins GCC reaches for on this target
# ------------------------------------------------------------------
#
# 64-bit division, shifts and compares; bit counts; soft-float double
# and single, conversions between them and the integers; and fp_mode.c,
# the rounding-mode hooks the soft-float arithmetic consults (nearest,
# no exceptions -- this core's floats in software have no FPSCR).

BI=$SRC/llvm/compiler-rt/lib/builtins
OBJ=$OUT/builtins-obj
rm -rf "$OBJ"
mkdir -p "$OBJ" "$OUT/lib" "$OUT/include"

BUILTINS="
  divdi3 udivdi3 moddi3 umoddi3 udivmoddi4 divmoddi4 divsi3 udivsi3
  modsi3 umodsi3 ashldi3 ashrdi3 lshrdi3 muldi3 negdi2 cmpdi2 ucmpdi2
  popcountsi2 popcountdi2 clzsi2 clzdi2 ctzsi2 ctzdi2 ffssi2 ffsdi2
  paritysi2 paritydi2 bswapsi2 bswapdi2
  adddf3 subdf3 muldf3 divdf3 comparedf2 negdf2 powidf2
  fixdfsi fixunsdfsi fixdfdi fixunsdfdi floatsidf floatunsidf floatdidf
  floatundidf extendsfdf2 truncdfsf2
  addsf3 subsf3 mulsf3 divsf3 comparesf2 negsf2 powisf2
  fixsfsi fixunssfsi fixsfdi fixunssfdi floatsisf floatunsisf floatdisf
  floatundisf
  fp_mode
"
n=0
for f in $BUILTINS; do
    # shellcheck disable=SC2086
    "$CC" $FLAGS -O2 -ffreestanding -fno-builtin -fno-pic -I"$BI" \
        -c "$BI/$f.c" -o "$OBJ/$f.o"
    n=$((n + 1))
done
rm -f "$OUT/lib/libbuiltins.a"
"$AR" rcs "$OUT/lib/libbuiltins.a" "$OBJ"/*.o
echo "compiler-rt: $n builtins"

# ------------------------------------------------------------------
# picolibc
# ------------------------------------------------------------------
#
# `powerpc` as the CPU family, not meson's `ppc`: picolibc maps the
# 64-bit names to its machine directory and has no entry for the 32-bit
# one, so it refuses `ppc` as unsupported.

CROSS=$OUT/cross-ppc32-e200.txt
cat >"$CROSS" <<EOF
[binaries]
c = ['$CC', $(echo "$FLAGS" | sed "s/\([^ ]*\)/'\1',/g") '-fno-pic', '-static']
ar = '$AR'
nm = '${CC%gcc}nm'
strip = '${CC%gcc}strip'

[host_machine]
system = 'none'
cpu_family = 'powerpc'
cpu = 'ppc'
endian = 'big'

[properties]
skip_sanity_check = true
EOF

rm -rf "$OUT/picolibc-build"
meson setup "$OUT/picolibc-build" "$SRC/picolibc" --cross-file "$CROSS" \
    --prefix="$OUT" -Dbuildtype=release -Dmultilib=false -Dpicocrt=false \
    -Dsemihost=false -Dtests=false -Dthread-local-storage=false \
    -Dnewlib-global-errno=true -Dformat-default=double -Dio-long-long=true \
    -Dspecsdir=none >"$OUT/picolibc-configure.log" 2>&1 || {
    tail -20 "$OUT/picolibc-configure.log" >&2
    exit 1
}
ninja -C "$OUT/picolibc-build" >"$OUT/picolibc-build.log" 2>&1 || {
    tail -20 "$OUT/picolibc-build.log" >&2
    exit 1
}
ninja -C "$OUT/picolibc-build" install >/dev/null 2>&1
echo "picolibc: $OUT/lib/libc.a $OUT/lib/libm.a"

# ------------------------------------------------------------------
# picolibc for EFPU2 -- see the note at the top
# ------------------------------------------------------------------

CLANG=${PPC_CLANG:-clang}
EFPU2="--target=powerpc-unknown-eabi -mcpu=e500 -mspe -mefpu2"
echo 'float f(float a) { return a * a; }' >"$OUT/efpu2-probe.c"
# shellcheck disable=SC2086
if ! command -v "$CLANG" >/dev/null 2>&1 ||
    ! "$CLANG" $EFPU2 -c "$OUT/efpu2-probe.c" -o "$OUT/efpu2-probe.o" 2>/dev/null; then
    echo "picolibc for EFPU2: skipped, $CLANG cannot target it"
    echo "wrote $OUT"
    exit 0
fi

# One compiler for meson, which picks by file: C to clang, a .S to the
# cross GCC with the first build's flags in place of clang's. The
# linker is named only so that meson's detection succeeds; nothing here
# links.
WRAP=$OUT/cc-efpu2.sh
LD=$(command -v "${CC%gcc}ld")
cat >"$WRAP" <<EOF
#!/bin/sh
case " \$* " in
*".S "*)
    a=""
    for x in "\$@"; do
        case "\$x" in
        --target=*|-mcpu=e500|-mspe|-mefpu2|--ld-path=*|-ffp-contract=off) ;;
        *) a="\$a '\$x'" ;;
        esac
    done
    eval exec $CC $FLAGS "\$a" ;;
*) exec $CLANG $EFPU2 -mlong-double-64 -ffp-contract=off \\
        --ld-path=$LD "\$@" ;;
esac
EOF
chmod +x "$WRAP"

CROSS2=$OUT/cross-ppc32-e200-efpu2.txt
sed "s|^c = .*|c = ['$WRAP', '-fno-pic', '-static']|" "$CROSS" >"$CROSS2"

rm -rf "$OUT/picolibc-efpu2-build" "$OUT/efpu2"
meson setup "$OUT/picolibc-efpu2-build" "$SRC/picolibc" --cross-file "$CROSS2" \
    --prefix="$OUT/efpu2" -Dbuildtype=release -Dmultilib=false -Dpicocrt=false \
    -Dsemihost=false -Dtests=false -Dthread-local-storage=false \
    -Dnewlib-global-errno=true -Dformat-default=double -Dio-long-long=true \
    -Dspecsdir=none >"$OUT/picolibc-efpu2-configure.log" 2>&1 || {
    tail -20 "$OUT/picolibc-efpu2-configure.log" >&2
    exit 1
}
ninja -C "$OUT/picolibc-efpu2-build" >"$OUT/picolibc-efpu2-build.log" 2>&1 || {
    tail -20 "$OUT/picolibc-efpu2-build.log" >&2
    exit 1
}
ninja -C "$OUT/picolibc-efpu2-build" install >/dev/null 2>&1
echo "picolibc for EFPU2: $OUT/efpu2/lib/libc.a"
echo "wrote $OUT"
