#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
#
# Bring up the host end of the board's SLIP link.
#
#   sudo ./scripts/slip-up.sh [device] [baud]
#
# then:
#
#   ping 192.168.7.2          # the link works
#   telnet 192.168.7.2        # the console
#
# Run it *after* the board has printed its "net SLIP on this port" line.
# Before that the firmware is still writing text to the UART, and slattach
# will feed it to the kernel as malformed IP -- harmless, but it fills the
# log and looks like a fault.
#
# Undo with: sudo pkill slattach

set -e

DEV=${1:-/dev/ttyACM0}
BAUD=${2:-921600}

BOARD=${EMU_NET_ADDR:-192.168.7.2}
HOST=${EMU_NET_PEER:-192.168.7.1}

# 1500 to match SLIP_MAX_SIZE in src/net/lwipopts.h. SLIP carries no length
# field and cannot negotiate, so a mismatch is not an error either end can
# report: it shows up as ping working and TFTP stalling, because only the
# large packets are affected.
MTU=1500

if [ "$(id -u)" != "0" ]; then
    echo "needs root: creating a network interface and setting an address" >&2
    exit 1
fi

if [ ! -c "$DEV" ]; then
    echo "$DEV is not a character device -- is the board plugged in?" >&2
    echo "a Nucleo-144 usually appears as /dev/ttyACM1, a Nucleo-64 as ACM0" >&2
    exit 1
fi

# -L is what makes this work on a USB virtual COM port: it sets CLOCAL, so
# the line is not waiting for a carrier that a VCP never asserts. Without
# it slattach blocks in open() and nothing happens at all.
slattach -L -p slip -s "$BAUD" "$DEV" &
SLPID=$!

# slattach creates the interface asynchronously; there is nothing to
# configure until it exists.
i=0
while [ ! -d /sys/class/net/sl0 ]; do
    i=$((i + 1))
    if [ "$i" -gt 50 ]; then
        echo "sl0 never appeared; is slattach installed (net-tools)?" >&2
        kill $SLPID 2>/dev/null || true
        exit 1
    fi
    sleep 0.1
done

# A point-to-point address rather than a subnet: there is exactly one host
# at the other end and no routing to do.
ip addr add "$HOST" peer "$BOARD" dev sl0
ip link set sl0 mtu "$MTU" up

echo "sl0 up: $HOST -> $BOARD at $BAUD on $DEV (slattach pid $SLPID)"
echo "  ping $BOARD"
echo "  telnet $BOARD"
