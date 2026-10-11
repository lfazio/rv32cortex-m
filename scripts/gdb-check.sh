#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# gdb-check.sh - attach a real gdb to the stub, once per frontend, and
# check what it sees.
#
# Usage:
#   scripts/gdb-check.sh [BUILD_DIR]        (default build/host)
#
# Runs every frontend the runner in BUILD_DIR was built with and that
# has a guest image there, and skips the others by name. Needs
# gdb-multiarch. Exits 0 if every frontend it ran passed, 1 if one
# failed, and 77 -- ctest's skip -- if it could run nothing at all.
#
# Each check is chosen so that it can fail. A register written through
# gdb and read back cannot -- a byte-swapped encoding is swapped both
# ways and round-trips perfectly -- so the things compared here are each
# against something the stub did not produce:
#
#   pc at a breakpoint   against the ELF's own symbol, which gdb reads
#                        from the file in the file's byte order. This
#                        is what catches the register byte order.
#   the stop reply       carries the pc ("expedites" it) so gdb need not
#                        ask. The number it is sent under is compared
#                        with gdb's own remote number for `pc`, from
#                        `maint print remote-registers`. Comparing the
#                        register's value before and after a cache flush
#                        does *not* work: gdb 17, given a register it did
#                        not need, fetches the whole `g` packet anyway and
#                        overwrites it, so the stub sent the pc as `fp` to
#                        every G4MH session and nothing looked wrong.
#   a written register   read back after a flush, so it has really gone
#                        through P and come back through p/g.
#   stepi                the pc moves, by an instruction's length.
#   memory at the pc     read from the target and from the file, which
#                        `trust-readonly-sections` switches between.
#
# The flat G4MH image has no symbols and no file for gdb to compare
# with, so its breakpoint is an address and its memory is compared with
# the image's bytes read here.

set -u

ROOT=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
BUILD=${1:-$ROOT/build/host}
case "$BUILD" in /*) ;; *) BUILD="$ROOT/$BUILD" ;; esac
EMU="$BUILD/emu-host"
GDB=${GDB:-gdb-multiarch}
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

command -v "$GDB" >/dev/null 2>&1 || { echo "gdb-check: $GDB not found" >&2; exit 77; }
[ -x "$EMU" ] || { echo "gdb-check: no runner at $EMU" >&2; exit 77; }

have=$("$EMU" --help 2>&1 | sed -n 's/.*This build has: //p' | tr -d ' ')
port=$((20000 + $$ % 20000))
failed=0
ran=0

# hex value of a "TAG v1 v2 ..." line in the gdb log, field N (1-based)
field() { awk -v t="$1" -v n="$2" '$1 == t { print $(n + 1); exit }' "$3"; }

expect_eq() { # name got want
    if [ -n "$2" ] && [ "$2" = "$3" ]; then
        printf '    ok    %-26s %s\n' "$1" "$2"
    else
        printf '    FAIL  %-26s got %s want %s\n' "$1" "${2:-<none>}" "${3:-<none>}"
        bad=1
    fi
}

expect_ne() { # name a b
    if [ -n "$2" ] && [ -n "$3" ] && [ "$2" != "$3" ]; then
        printf '    ok    %-26s %s -> %s\n' "$1" "$2" "$3"
    else
        printf '    FAIL  %-26s %s -> %s\n' "$1" "${2:-<none>}" "${3:-<none>}"
        bad=1
    fi
}

# one_frontend NAME IMAGE "EMU ARGS" SETUP BREAK REG [MEM]
#   SETUP  gdb commands before connecting (a `file`, or an architecture)
#   BREAK  a breakpoint location gdb accepts after `break`, or empty
#   REG    a general register to write, without the `$`
#   MEM    the word expected at the pc before the step, for an image gdb
#          has no file for; otherwise the file is asked
one_frontend() {
    local name=$1 image=$2 args=$3 setup=$4 brk=$5 reg=$6
    local mem_want=${7:-}
    local log="$TMP/$name.gdb.log" emulog="$TMP/$name.emu.log"

    case ",$have," in
    *,"$name",*) ;;
    *) echo "  $name: not in this build -- skipped"; return ;;
    esac
    if [ ! -f "$image" ]; then
        echo "  $name: no image at ${image#"$ROOT"/} -- skipped"
        return
    fi
    port=$((port + 1))
    ran=$((ran + 1))
    bad=0

    # shellcheck disable=SC2086
    "$EMU" $args --gdb "$port" "$image" >"$emulog" 2>&1 &
    local emu_pid=$!
    for _ in $(seq 50); do
        grep -q "waiting for a connection" "$emulog" 2>/dev/null && break
        sleep 0.1
    done

    {
        echo "set pagination off"
        echo "set confirm off"
        printf '%s\n' "$setup"
        echo "target remote :$port"
        echo 'printf "ATTACH_PC %x\n", $pc'
        if [ -n "$brk" ]; then
            echo "break *$brk"
            echo "continue"
            echo "printf \"AT_BREAK %x %x\\n\", \$pc, $brk"
        fi
        echo "maint print remote-registers"
        echo "set \$saved = \$$reg"
        echo "set \$$reg = 0x12345678"
        echo "maint flush register-cache"
        echo "printf \"REG_READBACK %x\\n\", \$$reg"
        echo "set \$$reg = \$saved"
        echo 'set $before = $pc'
        echo "set debug remote 1"
        echo "stepi"
        echo "set debug remote 0"
        echo 'printf "STEP %x %x\n", $before, $pc'
        echo 'set trust-readonly-sections off'
        echo 'printf "MEM_TARGET %x\n", *(unsigned int *)$before'
        echo 'set trust-readonly-sections on'
        echo 'printf "MEM_FILE %x\n", *(unsigned int *)$before'
        echo "detach"
    } >"$TMP/$name.cmd"

    timeout 60 "$GDB" -batch -nx -x "$TMP/$name.cmd" >"$log" 2>&1
    # Detaching resumes the guest; let it finish, then make sure.
    for _ in $(seq 100); do
        kill -0 "$emu_pid" 2>/dev/null || break
        sleep 0.1
    done
    kill "$emu_pid" 2>/dev/null
    wait "$emu_pid" 2>/dev/null

    echo "  $name: $(grep -m1 '^emu: ' "$emulog" | sed 's/^emu: //')"
    if [ -n "$brk" ]; then
        expect_eq "pc at breakpoint" "$(field AT_BREAK 1 "$log")" \
                  "$(field AT_BREAK 2 "$log")"
    fi
    # gdb's remote number for pc, and the one the stop reply used.
    local pc_rmt sent
    pc_rmt=$(awk '$1 == "pc" && $7 ~ /^[0-9]+$/ { print $7; exit }' "$log")
    sent=$(grep -o 'Packet received: T[0-9a-f][0-9a-f][0-9a-f]*:' "$log" |
           head -n 1 | sed 's/.*T..\([0-9a-f]*\):/\1/')
    [ -n "$sent" ] && sent=$((16#$sent))
    expect_eq "stop reply names pc" "$sent" "$pc_rmt"
    expect_eq "\$$reg written" "$(field REG_READBACK 1 "$log")" "12345678"
    expect_ne "stepi moves the pc" "$(field STEP 1 "$log")" \
              "$(field STEP 2 "$log")"
    if [ -n "$mem_want" ]; then
        expect_eq "memory at the pc" "$(field MEM_TARGET 1 "$log")" \
                  "$mem_want"
    else
        expect_eq "memory at the pc" "$(field MEM_TARGET 1 "$log")" \
                  "$(field MEM_FILE 1 "$log")"
    fi
    if [ "$bad" -ne 0 ]; then
        failed=$((failed + 1))
        echo "    gdb said:"
        grep -vE '^(ATTACH_PC|AT_BREAK|LAST_|REG_|STEP|MEM_)' "$log" |
            head -n 12 | sed 's/^/      /'
        echo "    the runner said:"
        grep -vE '^\s*$' "$emulog" | head -n 8 | sed 's/^/      /'
    fi
}

echo "gdb-check: $($GDB --version | head -n 1) against ${EMU#"$ROOT"/}"

one_frontend rv32 "$BUILD/guest/hello.elf" "" \
    "file $BUILD/guest/hello.elf" main a3
one_frontend armv7m "$BUILD/guest/armv7m-alu.elf" "" \
    "file $BUILD/guest/armv7m-alu.elf" main r3
one_frontend ppc "$BUILD/guest/ppc-alu.elf" "" \
    "file $BUILD/guest/ppc-alu.elf" main r3

# G4MH: a flat image, so no symbols, no breakpoint by name and no file
# for gdb to read. The step starts at the reset pc, which is where the
# image is loaded, so the word there is the image's first: read here,
# little-endian as the guest is (od reads in host order; the check is
# only run on little-endian hosts, which is every host this builds on).
G4_IMG="$ROOT/tests/guest/g4mh/guest.bin"
G4_WORD=$(od -An -tx4 -N4 "$G4_IMG" 2>/dev/null | tr -d ' ' | sed 's/^0*//')
one_frontend g4mh "$G4_IMG" "--frontend g4mh" \
    "set architecture v850:rh850" "" r6 "$G4_WORD"

if [ "$ran" -eq 0 ]; then
    echo "gdb-check: nothing to run"
    exit 77
fi
echo "gdb-check: $((ran - failed)) of $ran frontends passed"
[ "$failed" -eq 0 ]
