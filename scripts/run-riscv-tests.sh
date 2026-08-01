#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# run-riscv-tests.sh - Validate the core against the official riscv-tests
# ISA suite (github.com/riscv-software-src/riscv-tests).
#
# The suite's machine-mode ("-p") tests signal their result with the newlib
# exit syscall: ecall with a7=93 and a0=0 for pass, or a0=(testnum<<1)|1
# for the numbered sub-test that failed. The host runner implements that
# ABI, so the process exit status is the test result directly.
#
# Usage:
#   scripts/run-riscv-tests.sh [--suite rv32ui,rv32um,...] [--keep] [-v]
#
# Set RISCV_TESTS to reuse an existing checkout; otherwise it is cloned
# into the build directory on first run.

set -uo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build="${BUILD_DIR:-$here/build/riscv-tests}"
runner="${RV32_HOST:-$here/build/host/rv32-host}"
tests="${RISCV_TESTS:-$build/src}"

# Must match what misa advertises. rv32mi/csr deliberately fails if it is
# built without F and then run on a core that reports F, which is exactly
# the mismatch this had before -- the failure was the test doing its job,
# not a bug in the emulator.
march="${RV_MARCH:-rv32imafc_zicsr_zifencei}"
mabi="${RV_MABI:-ilp32}"
suites="rv32ui rv32um rv32ua rv32uc rv32mi"
verbose=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --suite) suites="${2//,/ }"; shift 2 ;;
        -v|--verbose) verbose=1; shift ;;
        -h|--help) sed -n '2,20p' "$0"; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done

if [[ ! -x "$runner" ]]; then
    echo "error: host runner not found at $runner" >&2
    echo "       build it first:  cmake -B build/host && cmake --build build/host" >&2
    exit 1
fi

GCC="${RISCV_GCC:-riscv64-unknown-elf-gcc}"
OBJCOPY="${RISCV_OBJCOPY:-riscv64-unknown-elf-objcopy}"
command -v "$GCC" >/dev/null || { echo "error: $GCC not found" >&2; exit 1; }

if [[ ! -d "$tests/isa" ]]; then
    echo "==> cloning riscv-tests into $tests"
    mkdir -p "$(dirname "$tests")"
    git clone --depth 1 --recurse-submodules -q \
        https://github.com/riscv-software-src/riscv-tests.git "$tests" || exit 1
fi

out="$build/out"
mkdir -p "$out"

# Flags taken from the suite's own isa/Makefile.
gccopts=(-static -mcmodel=medany -fvisibility=hidden -nostdlib -nostartfiles
         -march="$march" -mabi="$mabi"
         -I"$tests/env/p" -I"$tests/isa/macros/scalar"
         -T"$tests/env/p/link.ld")

total=0; passed=0; failed=0; skipped=0
declare -a failures=()

for suite in $suites; do
    dir="$tests/isa/$suite"
    [[ -d "$dir" ]] || { echo "skipping $suite (not present)"; continue; }

    echo
    echo "=== $suite ==="
    for src in "$dir"/*.S; do
        name="$(basename "$src" .S)"
        elf="$out/$suite-p-$name.elf"
        bin="$out/$suite-p-$name.bin"

        if ! "$GCC" "${gccopts[@]}" "$src" -o "$elf" 2>"$out/$name.buildlog"; then
            skipped=$((skipped + 1))
            printf '  %-24s SKIP (does not build)\n' "$name"
            [[ $verbose -eq 1 ]] && sed 's/^/      /' "$out/$name.buildlog"
            continue
        fi
        "$OBJCOPY" -O binary "$elf" "$bin" || continue

        total=$((total + 1))
        # The image is linked to run at 0x80000000 with the entry at +0.
        result="$("$runner" --quiet --max-insn 2000000 --load 0x80000000 "$bin" 2>&1)"
        rc=$?

        if [[ $rc -eq 0 ]]; then
            passed=$((passed + 1))
            [[ $verbose -eq 1 ]] && printf '  %-24s ok\n' "$name"
        else
            failed=$((failed + 1))
            failures+=("$suite/$name")
            # a0 = (testnum << 1) | 1, so the failing sub-test is rc >> 1.
            printf '  %-24s FAIL (exit %d, sub-test %d)\n' "$name" "$rc" "$((rc >> 1))"
            [[ -n "$result" && $verbose -eq 1 ]] && echo "$result" | sed 's/^/      /'
        fi
    done
done

echo
echo "======================================"
printf 'total %d   passed %d   failed %d   skipped %d\n' \
       "$total" "$passed" "$failed" "$skipped"
if [[ ${#failures[@]} -gt 0 ]]; then
    echo "failing:"
    printf '  %s\n' "${failures[@]}"
fi
echo "======================================"

[[ $failed -eq 0 ]]
