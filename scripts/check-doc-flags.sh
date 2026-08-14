#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
#
# check-doc-flags.sh - every -D<OPTION> named in the docs must exist.
#
# Why this exists, in one sentence from CLAUDE.md: "The option that hid
# this named a variable that does not exist." The notes documented
# `-DEMU_FPU_SOFTFLOAT=ON` against a real `RV32_FPU_SOFTFLOAT`, so every
# default build had an FP unit failing two thirds of the F suite and the
# documented way to check that theory changed nothing and reported
# nothing -- which reads as "SoftFloat makes no difference" rather than
# as a typo.
#
# The same rot had reached README.md: its build commands said
# `-DRV32_PLATFORM=host` for an option called EMU_PLATFORM, so the
# quickstart did not work at all.
#
# A build flag quoted in prose is not a tested thing. This makes it one.
#
# Exit status is 1 if any documented flag is unknown to the build.

set -eu

ROOT=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT"

# Every name the build actually defines: options, cache variables, and
# the plain set() variables a user might reasonably pass.
known=$(mktemp)
trap 'rm -f "$known" "$used"' EXIT
# Every CMakeLists in the tree, not just the top one: RV32_GUEST and
# RV_GUEST_MARCH live under tests/guest, and a check that missed them
# would report the two most-used options in the docs as typos.
find . -name build -prune -o -path ./docs/renesas -prune -o \
     \( -name CMakeLists.txt -o -name '*.cmake' \) -print 2>/dev/null |
    xargs grep -ohE '^[[:space:]]*(option|set)\([[:space:]]*[A-Za-z_][A-Za-z0-9_]*' 2>/dev/null |
    sed -E 's/.*\(\s*//' | sort -u > "$known"
# CMake's own well-known ones, which the docs legitimately mention.
cat >> "$known" <<'EOF'
CMAKE_BUILD_TYPE
CMAKE_TOOLCHAIN_FILE
CMAKE_EXPORT_COMPILE_COMMANDS
STM32CUBE_LOCAL_DIR
STM32CUBE_FAMILY
EOF
sort -u -o "$known" "$known"

# Every -DNAME the documentation mentions, with where it was found.
# Only *our* prose. docs/renesas is a vendored board package: its
# makefiles and project files are full of -DCPU1, -DebugMode and the
# like, which are not ours to be right about.
used=$(mktemp)
docfiles=$(find README.md CLAUDE.md docs -name '*.md' \
                -not -path 'docs/renesas/*' 2>/dev/null)
# -Dmain=... is a C preprocessor define CoreMark needs, not a CMake
# option; it is the one legitimate lowercase name here.
grep -rhoE '\-D[A-Z_][A-Za-z0-9_]*' $docfiles 2>/dev/null |
    sed 's/^-D//' | sort -u > "$used"

bad=0
while IFS= read -r name; do
    if ! grep -qxF "$name" "$known"; then
        printf 'unknown build option: -D%s\n' "$name"
        grep -rln -- "-D$name" $docfiles 2>/dev/null |
            sed 's/^/    named in /'
        bad=1
    fi
done < "$used"

if [ "$bad" = 0 ]; then
    printf 'check-doc-flags: all %s documented options exist\n' \
           "$(wc -l < "$used" | tr -d ' ')"
fi
exit "$bad"
