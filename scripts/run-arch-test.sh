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
#   scripts/run-arch-test.sh [--misaligned] [--extensions I,M,...] [--jobs N] [-v]
#
# --misaligned validates the *default* build, RV32_MISALIGNED=ON, against
# a description derived for it, and adds the four Misalign suites that the
# ordinary description cannot be offered.

set -uo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
suite="${ARCH_TEST_DIR:-$here/build/arch-test}"

# The runner is built here, into a directory of its own, and that is not
# tidiness.
#
# **The suite validates a machine against a description of that machine**,
# and tests/arch-test/*/{*.yaml,sail.json} both declare MISALIGNED_LDST
# false -- "this core reports misaligned accesses rather than splitting
# them". The emulator's own default became the opposite (RV32_MISALIGNED
# ON, which picolibc's word-at-a-time strcmp needs), so an ordinary build
# is no longer the core this description describes, and the four
# ExceptionsSv tests fail with "DUT generated too many traps".
#
# Nothing said so. The default flipped in 38a8fd1, the suite kept
# reporting 378/378 for days because it was running a binary from before
# it, and the failure only appeared when that stale binary was rebuilt.
# So the flag is applied *here*, where it cannot drift from the config it
# has to agree with -- and the build runs in the same command as the
# tests, which is this tree's standing rule for any runner a script
# defaults to.
#
# EMU_HOST still overrides, for bisecting or for testing a binary built
# elsewhere. It is on the caller to match the config when they do.
runner="${EMU_HOST:-}"

#
# Extra arguments for every test, and the reason it exists is `--jit`.
#
# **Nothing else in this tree validates a translating backend under the
# privileged features.** The suite runs the interpreter, so the Sv32,
# PMP and U-mode tests -- the ones that exist precisely because those
# paths are subtle -- say nothing about the JIT. And the JIT declines to
# run at all when any of them is armed, so today it passes them by not
# executing: `EMU_EXTRA_ARGS=--jit scripts/run-arch-test.sh` is a
# baseline that proves the *harness* works, before the backend is
# changed to actually translate under them.
#
EMU_EXTRA_ARGS="${EMU_EXTRA_ARGS:-}"
build_runner=1
[[ -n "${EMU_HOST:-}" ]] && build_runner=0

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
#
# Zicond, the two hint extensions and Zihpm each have a directory.
# ZihintntlZca is a *separate* one -- the compressed forms of the same
# four hints -- and is the half a decoder is likelier to get wrong, so
# naming Zihintntl alone would have offered half the suite and reported
# all of it passing. Zawrs and Zalasr have no directory at all: the
# emulator implements both and this suite says nothing about either.
#
# **And fifteen more that went unnamed for as long as this list has
# existed**: the combination directories. `Zcb` was here and `ZcbM` and
# `ZcbZbb` were not; `F` and `D` without `Zcf` and `Zcd`; `PMPU` alone of
# eight PMP directories; `Sv` without `SvZicbo`. Each is a suite this
# core is eligible for, 99 tests between them, and 13 of those failed
# the first time they ran -- one of them a defect in the JIT's optimiser
# that deleted a device read. A total of 378 was a statement about thirty
# directory names.
#
# So, when the checkout moves: `ls tests/rv32i tests/priv tests/priv/pmp/*`
# and account for every directory, by adding it here or by saying below
# why not.
#
#   Zabha, ZacasZabha, Zbkb, Zbkc, Zbkx, Zcmop, Zfa*, Zfh, ZfhD,
#   Zfbfmin, Zimop, Zknd, Zkne, Zknh, Zksed, Zksh    not implemented --
#                                                    Zfh is the half
#                                                    arithmetic; Zfhmin
#                                                    and ZfhminD are named
#   Misalign, MisalignD, MisalignF, MisalignZca      --misaligned: this
#                                                    description says
#                                                    MISALIGNED_LDST false
#   Svadu, SvaduPMP, Svnapot, Svpbmt                 not implemented;
#                                                    Svade is the choice
extensions="${ARCH_TEST_EXTENSIONS:-I,M,Zmmul,Zaamo,Zalrsc,Zca,Zicsr,Zicntr,Zifencei,Zicbom,Zicbop,Zicboz,Zicond,Zihintntl,ZihintntlZca,Zihintpause,Zihpm,Zbb,Zba,Zbc,Zbs,Zacas,F,D,Zcb,ZcbM,ZcbZbb,Zcf,Zcd,PMPU,PMPF,PMPS,PMPSm,PMPZaamo,PMPZalrsc,PMPZca,PMPZicbo,Sv,Svbare,Svade,SvZicbo,ExceptionsSv,ExceptionsSvZaamo,ExceptionsSvZalrsc,SvPMP,SvPMPZicbo,Zfhmin,ZfhminD,Svinval}"
[[ -n "${ARCH_TEST_EXTENSIONS:-}" ]] && ext_given=1 || ext_given=0
jobs="$(nproc)"
verbose=""
misaligned=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --extensions) extensions="$2"; ext_given=1; shift 2 ;;
        --misaligned) misaligned=1; shift ;;
        --jobs|-j) jobs="$2"; shift 2 ;;
        -v|--verbose) verbose="--verbose"; shift ;;
        -h|--help) sed -n '2,21p' "$0"; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done

#
# The two builds, each with the description that is true of it. The
# misaligned one is *derived* from the ordinary one by
# tests/arch-test/derive-misaligned.py, which asserts every line it
# changes: the two differ in one property, and two hand-kept copies of the
# Sail config would not stay that way.
#
# Its own build directory and its own work directory under the suite, so
# neither run can be handed the other's binary or ELFs.
#
if [[ "$misaligned" == 1 ]]; then
    misaligned_flag=ON
    host_dir="$here/build/arch-test-host-misaligned"
    cfg_name="rv32cortex-m-rv32-misaligned"
    [[ "$ext_given" == 0 ]] &&
        extensions="$extensions,Misalign,MisalignD,MisalignF,MisalignZca"
else
    misaligned_flag=OFF
    host_dir="$here/build/arch-test-host"
fi
[[ -z "$runner" ]] && runner="$host_dir/emu-host"

if [[ "$build_runner" == 1 ]]; then
    echo "==> building the runner (RV32_MISALIGNED=$misaligned_flag, as the config declares)"
    cmake -S "$here" -B "$host_dir" \
          -DCMAKE_BUILD_TYPE=Release -DRV32_MISALIGNED=$misaligned_flag >/dev/null || exit 1
    # By exit status, not by grepping for "error": a "No rule to make
    # target" contains neither that word nor any other this script could
    # match, and a failed build leaves the previous binary at exactly the
    # path the tests are about to use.
    cmake --build "$host_dir" --target emu-host \
          -j"$(nproc)" >/dev/null || exit 1
fi

if [[ ! -x "$runner" ]]; then
    echo "error: host runner not found at $runner" >&2
    echo "       cmake -B build/host && cmake --build build/host" >&2
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

# The suite at the commit every figure here was taken with. Unpinned, a
# fresh tree took upstream's head, which on 2026-10-30 required Sail
# 0.16 where this host has 0.13.1, and arch-test-I failed in every row of
# a build matrix run from a new worktree -- for a reason that was not the
# emulator's. Move the pin on purpose, with Sail, and rerun both modes.
ARCH_TEST_REF=${ARCH_TEST_REF:-d261a799762ffb94eea486d4adfe7632006d2b4a}
if [[ ! -d "$suite/tests" ]]; then
    echo "==> fetching riscv-arch-test $ARCH_TEST_REF into $suite"
    mkdir -p "$suite"
    { git -C "$suite" init -q &&
        git -C "$suite" remote add origin \
            https://github.com/riscv/riscv-arch-test.git &&
        git -C "$suite" fetch --depth 1 -q origin "$ARCH_TEST_REF" &&
        git -C "$suite" checkout -q FETCH_HEAD; } || exit 1
fi
have=$(git -C "$suite" rev-parse HEAD 2>/dev/null || echo none)
if [[ "$have" != "$ARCH_TEST_REF" ]]; then
    echo "warning: $suite is at $have, not the pinned $ARCH_TEST_REF" >&2
fi

# Install our DUT description into the suite's config tree.
dest="$suite/config/cores/rv32cortex-m/$cfg_name"
mkdir -p "$dest"
if [[ "$misaligned" == 1 ]]; then
    python3 -I "$here/tests/arch-test/derive-misaligned.py" "$cfg_src" "$dest" ||
        exit 1
else
    cp "$cfg_src"/* "$dest/"
fi

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

#
# Run the suites that were named, not the directory.
#
# run_tests.py executes every ELF under the directory it is given, and
# the work directory keeps everything any earlier run built. So
# `--extensions I` -- which is what the `arch-test-I` ctest asks for --
# ran 39 tests from a fresh checkout and all 496 after one full run, and
# passed or failed on whichever it happened to hold. The name of a test
# was a statement about the history of a directory.
#
# So the run gets a directory of its own, holding links to the ELFs of
# exactly the named suites -- a suite being the directory an ELF sits
# in, which is how ACT selects them too. A named suite with nothing in it
# is reported: naming offers a suite and the description decides, and a
# suite that is named and silently empty is the thing this script exists
# to make visible.
#
run_dir="$suite/work/run/$cfg_name"
rm -rf "$run_dir"
mkdir -p "$run_dir/elfs"
declare -A named=() found=()
IFS=, read -ra _names <<< "$extensions"
for n in "${_names[@]}"; do
    named["$n"]=1
done
while IFS= read -r -d '' elf; do
    dir="$(dirname "$elf")"
    s="$(basename "$dir")"
    [[ -n "${named[$s]:-}" ]] || continue
    rel="${elf#"$elfs"/}"
    mkdir -p "$run_dir/elfs/$(dirname "$rel")"
    ln -s "$elf" "$run_dir/elfs/$rel"
    found["$s"]=$(( ${found[$s]:-0} + 1 ))
done < <(find "$elfs" -name '*.elf' -print0)
total=0
empty=""
for n in "${_names[@]}"; do
    total=$(( total + ${found[$n]:-0} ))
    [[ -z "${found[$n]:-}" ]] && empty="$empty $n"
done
echo "==> $total ELFs in the ${#_names[@]} named suites" \
     "($(find "$elfs" -name '*.elf' | wc -l) in the work directory)"
[[ -n "$empty" ]] && echo "warning: named, and nothing built:$empty" >&2
if [[ "$total" == 0 ]]; then
    echo "error: none of the named suites has a test to run" >&2
    exit 1
fi

echo "==> running against $runner"
(
    cd "$suite" || exit 1
    # 8 MiB of guest RAM matches RAM_LENGTH in our link.ld. The instruction
    # cap is a safety net: a test that never reaches HALT would otherwise
    # spin forever.
    ./run_tests.py --jobs "$jobs" \
        "$runner $EMU_EXTRA_ARGS --quiet --ram 0x800000 --max-insn 500000000" \
        "work/run/$cfg_name/elfs"
)
