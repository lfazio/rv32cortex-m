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

timeout 90 nc "$ADDR" 23 >"$log" 2>/dev/null &
nc_pid=$!

sleep 3

#
# **An upload the client abandons must run nothing.** One block and
# then silence, which is what a client refused mid-transfer leaves: the
# server gives up on it and closes the transfer the way it closes a
# finished one, and the board used to commit the 512 bytes and run them.
# Wait for the board to say it dropped them, then upload for real --
# which also shows a dropped transfer leaves the server usable.
#
python3 - "$ADDR" <<'EOF'
import socket, struct, sys
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.settimeout(5)
s.sendto(b"\x00\x02abandoned\x00octet\x00", (sys.argv[1], 69))
ack, peer = s.recvfrom(516)
if ack[:4] == b"\x00\x04\x00\x00":
    s.sendto(struct.pack(">HH", 3, 1) + b"\x13" * 512, peer)
EOF
n=0
while [ $n -lt 200 ] && ! grep -q 'upload abandoned' "$log"; do
    sleep 0.1
    n=$((n + 1))
done
if grep -q 'running uploaded image, 512 bytes' "$log"; then
    bad abandoned "the board ran the one block it was sent"
elif grep -q 'upload abandoned after 512 bytes' "$log"; then
    ok abandoned "one block, dropped and not run"
else
    bad abandoned "the board never said it dropped the transfer"
fi

printf 'binary\nput %s isatest\nquit\n' "$img" |
    timeout 30 tftp "$ADDR" >"$up" 2>&1 || true

#
# Until the uploaded run has printed its terminator, not until the
# timeout: a minute of waiting on every pass was most of what this
# cost. The half second after it lets the counters behind it arrive.
#
n=0
while [ $n -lt 550 ] && kill -0 $nc_pid 2>/dev/null; do
    if sed -n '/running uploaded image/,$p' "$log" | grep -q 'emu-result'; then
        sleep 0.5
        break
    fi
    sleep 0.1
    n=$((n + 1))
done
kill $nc_pid 2>/dev/null || true
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
# The debugger, over the same link. It reads the pc of the guest that
# just finished: a number only the stub can supply, so an answer is the
# stub answering. RV32, because that is what was uploaded.
#
if command -v gdb-multiarch >/dev/null 2>&1; then
    # shellcheck disable=SC2016 # $pc and $1 are gdb's, not the shell's
    g=$(timeout 30 gdb-multiarch -batch -nx \
        -ex 'set architecture riscv:rv32' \
        -ex "target remote $ADDR:1234" \
        -ex 'p/x $pc' -ex detach 2>&1 || true)
    # shellcheck disable=SC2016
    pc=$(printf '%s\n' "$g" | sed -n 's/^\$1 = \(0x[0-9a-f]*\)$/\1/p')
    if [ -n "$pc" ]; then
        ok gdb "pc $pc"
    else
        bad gdb "no answer from the stub on $ADDR:1234"
    fi
else
    say gdb "skipped -- no gdb-multiarch"
fi

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
    #
    # The run that was graded, not the head of the capture: that is the
    # previous guest's output, which is the part that cannot explain a
    # failure of this one.
    #
    echo "--- console, from the upload ---"
    printf '%s\n' "$run" | sed 's/^/    /' | head -80
fi
exit $((fails == 0 ? 0 : 1))
