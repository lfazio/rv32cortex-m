#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# fetch-spike.sh - Build Spike, the RISC-V ISA simulator, into build/spike.
#
# Spike is the second reference model here, beside Sail. It is used the
# way Sail is -- as an oracle that the emulator's answers are compared
# with, by scripts/run-probe.sh -- and nothing of it is in this tree: it
# is cloned and built under build/, which is not tracked.
#
# Why a second one: Sail does not model everything this core implements.
# Zalasr is in neither Sail 0.13.1 (the suite's pin), 0.15 nor the
# 2026-10-12 nightly, and Spike has it. And where both models do have an
# extension, two readings that disagree are worth more than one: on
# Zawrs they do (see tests/arch-test/probes/zawrs.S).
#
# Licence: riscv-isa-sim is BSD-3-Clause (the Regents of the University of
# California). This project takes nothing from GPL code, not even as an
# oracle; Spike is not GPL, which is why it may be used and Unicorn and
# QEMU may not.
#
# The commit is pinned, as Sail's version is, so that a result names the
# model that produced it. Override with SPIKE_COMMIT to try another.
#
# Usage:
#   scripts/fetch-spike.sh            # build/spike/bin/spike
#   SPIKE_DIR=/elsewhere scripts/fetch-spike.sh
#
# Needs a C++17 compiler, make and dtc. About ten minutes the first time;
# nothing after that, because an existing build at the pinned commit is
# left alone.

set -euo pipefail

here=$(cd -- "$(dirname -- "$0")/.." && pwd)
commit=${SPIKE_COMMIT:-f8a060b7bea8da6f738f193ed1b66b665978d092}
prefix=${SPIKE_DIR:-$here/build/spike}
src=$here/build/spike-src

stamp=$prefix/.commit
if [[ -x $prefix/bin/spike && -f $stamp && $(cat "$stamp") == "$commit" ]]; then
    echo "spike: $prefix/bin/spike, at ${commit:0:12}"
    exit 0
fi

command -v dtc >/dev/null || { echo "error: Spike needs dtc (device-tree-compiler)" >&2; exit 1; }

if [[ ! -d $src/.git ]]; then
    echo "==> cloning riscv-isa-sim"
    git clone -q https://github.com/riscv-software-src/riscv-isa-sim.git "$src"
fi
git -C "$src" fetch -q origin "$commit" 2>/dev/null || true
git -C "$src" checkout -q "$commit"

echo "==> building Spike ${commit:0:12} (logs in $src/build)"
mkdir -p "$src/build"
(
    cd "$src/build"
    # Boost only serves the remote-bitbang and socket front ends, which
    # nothing here uses.
    ../configure --prefix="$prefix" --without-boost --without-boost-asio \
        --without-boost-regex >configure.log 2>&1
    make -j"$(nproc)" >make.log 2>&1
    make install >install.log 2>&1
) || { echo "error: Spike did not build; see $src/build/*.log" >&2; exit 1; }

echo "$commit" >"$stamp"
echo "spike: $prefix/bin/spike, at ${commit:0:12}"
