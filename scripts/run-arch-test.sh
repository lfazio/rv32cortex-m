#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# run-arch-test.sh - Validate the core against the official RISC-V
# architecture test suite (github.com/riscv/riscv-arch-test).
#
# The current suite is self-checking: each generated test reports through
# RVMODEL_IO_WRITE_STR and terminates with RVMODEL_HALT_PASS/FAIL, printing
# an "RVCP-SUMMARY: TEST PASSED/FAILED" line and setting the exit status.
# No reference model is needed, so nothing here depends on Spike or Sail.
#
# Our DUT description lives in tests/arch-test/ and is copied into the
# suite checkout at run time, which keeps it version controlled with the
# emulator rather than inside a third-party clone.
#
# Usage:
#   scripts/run-arch-test.sh [--extensions I,M,...] [--jobs N] [-v]

set -uo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
suite="${ARCH_TEST_DIR:-$here/build/arch-test}"
runner="${EMU_HOST:-$here/build/host/emu-host}"

cfg_name="rv32cortex-m-rv32"
cfg_src="$here/tests/arch-test/$cfg_name"

# The test suites to build, which ACT selects by *directory name* under
# tests/ -- not by required extension, however much the option is called
# --extensions. That distinction is the whole reason the privileged tests
# went unnoticed: `U` matches no directory and silently builds nothing,
# while the U-mode PMP tests live in a directory called PMPU. What each
# test actually requires is declared in its own header and checked against
# the UDB config, so naming a suite here only offers it for selection.
#
# The privileged suites are named here too, and naming them is the only
# way they are offered: PMPU, and the Sv families now that there is paging.
# Each test states its own REQUIRED_EXTENSIONS, so the Sv39/Sv48/Sv57
# members of these directories are filtered out against the UDB config and
# only the Sv32 ones build -- naming a suite offers it, it does not force
# it. Svadu is deliberately absent: this core implements Svade, which is
# the opposite choice about who sets A and D.
extensions="${ARCH_TEST_EXTENSIONS:-I,M,Zmmul,Zaamo,Zalrsc,Zca,Zicsr,Zicntr,Zifencei,Zicbom,Zicboz,Zbb,Zba,Zbc,Zbs,Zacas,F,D,Zcb,PMPU,Sv,Svbare,Svade,ExceptionsSv,SvPMP}"
jobs="$(nproc)"
verbose=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --extensions) extensions="$2"; shift 2 ;;
        --jobs|-j) jobs="$2"; shift 2 ;;
        -v|--verbose) verbose="--verbose"; shift ;;
        -h|--help) sed -n '2,20p' "$0"; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done

if [[ ! -x "$runner" ]]; then
    echo "error: host runner not found at $runner" >&2
    echo "       cmake -B build/host -DRV32_PLATFORM=host && cmake --build build/host" >&2
    exit 1
fi
command -v uv >/dev/null || { echo "error: uv is required by the suite's build" >&2; exit 1; }

# ACT runs the Sail golden model to compute the expected results it bakes
# into each self-checking test, so the model has to be on PATH. Upstream
# publishes a prebuilt binary; fetch it once if it is not already here.
sail_dir="${SAIL_DIR:-$here/build/sail}"
if [[ ! -x "$sail_dir/bin/sail_riscv_sim" ]] && ! command -v sail_riscv_sim >/dev/null; then
    echo "==> fetching the Sail RISC-V model"
    mkdir -p "$sail_dir"
    curl -fsSL -o "$here/build/sail.tgz" \
        "https://github.com/riscv/sail-riscv/releases/download/${SAIL_VERSION:-0.13.1}/sail-riscv-Linux-$(uname -m).tar.gz" \
        || { echo "error: could not download the Sail model" >&2; exit 1; }
    tar xzf "$here/build/sail.tgz" -C "$sail_dir" --strip-components=1 || exit 1
fi
export PATH="$sail_dir/bin:$PATH"

# The framework drives the RISC-V Unified Database, which is a Ruby gem
# fetched through Bundler. Gems installed with --user-install land outside
# the default PATH, so add that directory if it exists.
if command -v ruby >/dev/null; then
    export PATH="$(ruby -e 'print Gem.user_dir')/bin:$PATH"
fi
if ! command -v bundle >/dev/null; then
    echo "error: bundler not found; install it with:" >&2
    echo "         gem install --user-install bundler" >&2
    exit 1
fi
# Without this bundler installs into the system gem directory, which needs
# root. Keep the gems inside the build tree instead.
export BUNDLE_PATH="${BUNDLE_PATH:-$suite/.bundle}"

if [[ ! -d "$suite/tests" ]]; then
    echo "==> cloning riscv-arch-test into $suite"
    mkdir -p "$(dirname "$suite")"
    git clone --depth 1 -q https://github.com/riscv/riscv-arch-test.git "$suite" || exit 1
fi

# Install our DUT description into the suite's config tree.
dest="$suite/config/cores/rv32cortex-m/$cfg_name"
mkdir -p "$dest"
cp "$cfg_src"/* "$dest/"

echo "==> building test ELFs for $extensions"
(
    cd "$suite" || exit 1
    uv run act "config/cores/rv32cortex-m/$cfg_name/test_config.yaml" \
        --workdir work \
        --test-dir tests \
        --jobs "$jobs" \
        --extensions "$extensions" \
        --keep-going \
        $verbose
) || echo "warning: some tests failed to build; running what did" >&2

elfs="$suite/work/$cfg_name/elfs"
if [[ ! -d "$elfs" ]]; then
    echo "error: no ELFs were produced at $elfs" >&2
    exit 1
fi
echo "==> $(find "$elfs" -name '*.elf' | wc -l) ELFs built"

echo "==> running against $runner"
(
    cd "$suite" || exit 1
    # 8 MiB of guest RAM matches RAM_LENGTH in our link.ld. The instruction
    # cap is a safety net: a test that never reaches HALT would otherwise
    # spin forever.
    ./run_tests.py --jobs "$jobs" \
        "$runner --quiet --ram 0x800000 --max-insn 500000000" \
        "work/$cfg_name/elfs"
)
