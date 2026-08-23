#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
#
# check-board-net.sh - use the board's network, rather than building it.
#
#   scripts/check-board-net.sh            # build, flash, then exercise
#   scripts/check-board-net.sh --no-flash # against whatever is on it now
#
# Why this exists
# ---------------
# The build matrix has a row for the F746 with a network and it only ever
# *compiled* it. The whole EMU_NET path then stopped working across a
# refactor -- the board printed its banner and hung, and the host's copy
# would not even compile -- while every row reported ok. A row that builds
# a link is not a row that has a link.
#
# So this drives it end to end and fails by name:
#
#   ping      the stack is up and answering
#   telnet    the console came through the handover -- the board's own
#             output is the only thing that can say the guest ran, since
#             the UART is carrying IP by then
#   PASS      and it ran *correctly*
#   result    emu-result, the terminator a harness reads. Asserting this
#             rather than "no failure" is what makes running a
#             precondition of passing: a guest that stopped after twenty
#             instructions prints no terminator and cannot pass.
#   upload    tftp put, and the new guest actually runs -- which is the
#             feature the network exists for, and the one that broke
#             silently twice
#
# The link must be up before this runs, because bringing it up needs root
# and a test that asks for a password does not get run. scripts/ppp-up.sh
# passes `persist`, so it survives the reflash in the middle of this and
# one sudo covers a whole session.

set -eu

ROOT=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
BUILD=${EMU_BOARD_BUILD:-$ROOT/build/mx-f746-net}
ADDR=${EMU_NET_ADDR:-192.168.7.2}
PROBE=${EMU_PROBE:-}
FLASH=1
fails=0

while [ $# -gt 0 ]; do
    case "$1" in
    --no-flash) FLASH=0; shift ;;
    -h|--help) sed -n '3,12p' "$0"; exit 0 ;;
    *) echo "check-board-net: unknown option: $1" >&2; exit 2 ;;
    esac
done

say()  { printf '  %-22s %s\n' "$1" "$2"; }
bad()  { say "$1" "FAIL -- $2"; fails=$((fails + 1)); }
ok()   { say "$1" "ok${2:+ -- $2}"; }

# ---------------------------------------------------------------------
# Build and flash
# ---------------------------------------------------------------------

if [ "$FLASH" -eq 1 ]; then
    probe_arg=
    [ -n "$PROBE" ] && probe_arg="-DEMU_PROBE=$PROBE"

    # shellcheck disable=SC2086
    cmake -S "$ROOT" -B "$BUILD" \
        -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake \
        -DEMU_PLATFORM=stm32f746 -DEMU_NET=ON $probe_arg >/dev/null
    cmake --build "$BUILD" -j"$(nproc)" >/dev/null

    if ! cmake --build "$BUILD" --target flash >/dev/null 2>&1; then
        echo "check-board-net: flash failed -- with more than one probe" >&2
        echo "  attached, pass -DEMU_PROBE=<vid:pid:serial> or set" >&2
        echo "  EMU_PROBE; probe-rs refuses to guess and a failed flash" >&2
        echo "  leaves the *previous* firmware running, so the next" >&2
        echo "  check would grade an image that was never loaded." >&2
        exit 1
    fi

    # The board reboots and pppd redials; `persist` is what makes that
    # happen without a human.
    n=0
    while [ $n -lt 40 ]; do
        ping -c1 -W1 "$ADDR" >/dev/null 2>&1 && break
        n=$((n + 1))
        sleep 1
    done
fi

# ---------------------------------------------------------------------
# Exercise it
# ---------------------------------------------------------------------

echo "check-board-net: $ADDR"

if ping -c2 -W2 "$ADDR" >/dev/null 2>&1; then
    ok ping
else
    bad ping "no answer -- is scripts/ppp-up.sh running?"
    echo "check-board-net: $fails failure(s)"
    exit 1
fi

log=$(mktemp)
up=$(mktemp)
trap 'rm -f "$log" "$up"' EXIT

#
# **Upload a guest, and grade *that* run.**
#
# Not whatever the board happens to be running: with --no-flash that is
# the last thing anyone sent it, and the first version of this script
# duly reported "no PASS from the guest" about a `hello` left over from a
# previous session. Uploading a known image makes the check independent
# of the board's state and exercises the reload path -- which is the one
# that has broken silently twice -- in the same pass.
#
img=$BUILD/guest/isatest.bin
if [ ! -f "$img" ]; then
    echo "check-board-net: no $img to upload" >&2
    exit 1
fi

timeout 60 nc "$ADDR" 23 >"$log" 2>/dev/null &
nc_pid=$!

sleep 3
printf 'binary\nput %s isatest\nquit\n' "$img" |
    timeout 30 tftp "$ADDR" >"$up" 2>&1 || true

wait $nc_pid 2>/dev/null || true

# Strip the telnet negotiation and the guest's own control bytes.
text=$(tr -d '\000-\010\013\014\016-\037' <"$log")

# Everything after the reload: the run being graded.
run=$(printf '%s\n' "$text" | sed -n '/running uploaded image/,$p')

if [ -z "$run" ]; then
    bad upload "the board never reported taking it"
    run=$text
else
    ok upload "the board took it and restarted"
fi

printf '%s\n' "$run" | grep -q 'PASS' \
    && ok guest "ran and passed" \
    || bad guest "no PASS from the uploaded guest"

printf '%s\n' "$run" | grep -qE '^  retired +[1-9]' \
    && ok retired "the new guest executed" \
    || bad retired "the new guest retired nothing"

printf '%s\n' "$run" | grep -q 'emu-result' \
    && ok terminator "the harness line is there" \
    || bad terminator "no emu-result -- the run did not finish"

#
# Reported, not asserted. These are cumulative since boot and a single
# dropped byte across a session of reflashes is not a fault -- while a
# threshold picked out of the air is the kind of number that gets
# believed. What a dropped byte that *mattered* would do is break the
# upload, and that is asserted above.
#
printf '%s\n' "$run" | sed -n 's/^  \(rx drops .*\)$/  \1/p' |
    while read -r l; do say counters "$l"; done

if [ "$fails" -eq 0 ]; then
    echo "check-board-net: all checks passed"
else
    echo "check-board-net: $fails failure(s)"
    echo "--- console ---"
    echo "$text" | sed 's/^/    /' | head -40
fi
exit $((fails == 0 ? 0 : 1))
