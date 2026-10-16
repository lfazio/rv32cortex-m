#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
#
# check-host-net.sh - the board's network checks, against the host
# runner, with no root.
#
#   scripts/check-host-net.sh [build-dir]    # default build/hnet
#   scripts/check-host-net.sh --board DEV build-dir
#   scripts/check-host-net.sh [--board DEV] build-dir -- CMD [ARG...]
#
# The build must be -DEMU_NET=ON -DEMU_NET_LINK=slip. The checks are
# scripts/check-board-net.sh's: ping, a guest uploaded over TFTP and
# graded from the telnet console, and gdb. What this adds is the other
# end of the wire.
#
# With --board the build is a board's: it is flashed, and the bridge
# runs to DEV, the board's UART -- open it by /dev/serial/by-id/, since
# the ttyACM numbers swap. EMU_PROBE names the probe when there are two.
#
# With `-- CMD`, CMD runs in the namespace instead of the checks, with
# EMU_NET_ADDR set and nothing flashed: a suite through the upload shim,
#
#   scripts/check-host-net.sh --board DEV build/f746slip -- \
#       env EMU_HOST=scripts/emu-board scripts/run-riscv-tests.sh
#
# Why this exists
# ---------------
# docs/network.md said the host's copy of the stack was built by the
# matrix and run by nothing, "because pppd needs root". So the one place
# lwIP could be debugged with a debugger was never exercised, and the
# recipe documented for it -- `emu-host --ppp <guest>` -- did not work:
# --ppp took the guest for a device name and printed the usage.
#
# Neither half of a link needs root inside a namespace of its own.
# `unshare -rn` gives a network namespace, and in it a TUN device;
# scripts/slip-tun.py bridges that to the runner's pty, as it bridges
# the board's UART. PPP would need pppd, which wants the real root, so
# this is SLIP -- the stack, the telnet console, TFTP and the gdb stub
# are the same code either way, and src/net/ is what is under test.
#
# Exit 77 where namespaces are not available, so a caller can tell
# "skipped" from "failed".

set -eu

ROOT=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
DEV=
if [ "${1:-}" = --board ]; then
    DEV=${2:?usage: check-host-net.sh --board DEV build-dir}
    shift 2
fi
BUILD=${1:-$ROOT/build/hnet}
BUILD=$(CDPATH='' cd -- "$BUILD" && pwd)
[ $# -gt 0 ] && shift
if [ "${1:-}" = -- ]; then
    shift
fi

cache() {
    sed -n "s/^$1:[A-Z]*=\(.*\)$/\1/p" "$BUILD/CMakeCache.txt"
}

if [ -z "$DEV" ] && [ ! -x "$BUILD/emu-host" ]; then
    echo "check-host-net: no $BUILD/emu-host" >&2
    exit 1
fi
if [ "$(cache EMU_NET)" != ON ] || [ "$(cache EMU_NET_LINK)" != slip ]; then
    echo "check-host-net: $BUILD is not -DEMU_NET=ON -DEMU_NET_LINK=slip" >&2
    exit 1
fi

ADDR=$(cache EMU_NET_ADDR)
PEER=$(cache EMU_NET_PEER)

#
# Into a namespace of our own, once. Root inside it is root over its
# network only; nothing here can touch the machine's interfaces.
#
if [ "${CHECK_HOST_NET_INSIDE:-}" != 1 ]; then
    if ! unshare -rn true 2>/dev/null; then
        echo "check-host-net: cannot create a user and network namespace;" \
             "skipped" >&2
        exit 77
    fi
    if [ -n "$DEV" ]; then
        exec env CHECK_HOST_NET_INSIDE=1 unshare -rn "$0" --board "$DEV" \
            "$BUILD" -- "$@"
    fi
    exec env CHECK_HOST_NET_INSIDE=1 unshare -rn "$0" "$BUILD" -- "$@"
fi

work=$(mktemp -d)
emu_pid=
slip_pid=
# shellcheck disable=SC2329 # invoked by the trap below
cleanup() {
    [ -n "$slip_pid" ] && kill "$slip_pid" 2>/dev/null
    [ -n "$emu_pid" ] && kill "$emu_pid" 2>/dev/null
    wait 2>/dev/null
    rm -rf "$work"
}
trap cleanup EXIT

ip link set lo up
ip tuntap add mode tun name emu0
ip addr add "$PEER" peer "$ADDR" dev emu0
ip link set emu0 up

#
# Any guest to start with: check-board-net.sh uploads the one it grades.
# The runner keeps serving the link once the guest has finished, which
# is the state a harness finds a board in.
#
: >"$work/emu.err"
if [ -n "$DEV" ]; then
    pty=$DEV
else
    # As a board runs: its cap, EMU_MAX_INSN, and the state dump every
    # run ends with -- where run-riscv-tests.sh reads a parked verdict.
    "$BUILD/emu-host" --ppp --dump --max-insn 100000000 \
        "$BUILD/guest/hello.bin" \
        >"$work/emu.out" 2>"$work/emu.err" &
    emu_pid=$!

    pty=
    n=0
    while [ -z "$pty" ] && [ $n -lt 50 ]; do
        pty=$(sed -n 's/^emu: slip on \(.*\)$/\1/p' "$work/emu.err")
        n=$((n + 1))
        [ -z "$pty" ] && sleep 0.1
    done
    if [ -z "$pty" ]; then
        echo "check-host-net: the runner named no pty" >&2
        sed 's/^/    /' "$work/emu.err" "$work/emu.out" | head -20 >&2
        exit 1
    fi
fi

python3 "$ROOT/scripts/slip-tun.py" "$pty" --iface emu0 \
    --host "$PEER" --board "$ADDR" >"$work/slip.out" 2>&1 &
slip_pid=$!

rc=0
if [ $# -gt 0 ]; then
    n=0
    while ! ping -c1 -W1 "$ADDR" >/dev/null 2>&1 && [ $n -lt 20 ]; do
        n=$((n + 1))
    done
    EMU_NET_ADDR=$ADDR "$@" || rc=$?
    exit "$rc"
fi

# A board is flashed first; the bridge keeps the UART open across it.
flash=--no-flash
[ -n "$DEV" ] && flash=

# shellcheck disable=SC2086
EMU_BOARD_BUILD=$BUILD EMU_NET_ADDR=$ADDR \
    "$ROOT/scripts/check-board-net.sh" $flash || rc=$?

if [ "$rc" -ne 0 ]; then
    echo "--- runner stderr ---"
    sed 's/^/    /' "$work/emu.err" | head -20
    echo "--- bridge ---"
    sed 's/^/    /' "$work/slip.out" | head -20
fi
exit "$rc"
