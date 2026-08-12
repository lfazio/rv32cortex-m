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

if [ ! -c "$DEV" ]; then
    echo "$DEV is not a character device -- is the board plugged in?" >&2
    echo "a Nucleo-144 usually appears as /dev/ttyACM1, a Nucleo-64 as ACM0" >&2
    exit 1
fi

# Undo any previous run first. This is not tidiness: a SLIP interface is
# created by the line discipline and has no rtnl_link_ops, so `ip link
# delete` cannot remove one and a stale sl0 outlives the slattach that
# made it. The next attach then gets sl1, while the address stays on the
# dead sl0 -- so the link is configured, `ip addr` looks right, and every
# packet goes nowhere. Flush every sl* rather than just sl0, because by
# then the address may be on any of them.
pkill -x slattach 2>/dev/null || true
for n in /sys/class/net/sl*; do
    [ -e "$n" ] || continue
    ip addr flush dev "$(basename "$n")" 2>/dev/null || true
done

before=$(ls /sys/class/net | grep '^sl[0-9]' | sort || true)

# Set the line here, not with slattach's -s.
#
# net-tools slattach maps -s through a fixed table of Bxxx constants that
# stops below this rate, so `-s 921600` does not fall back to something
# sensible -- it fails the open outright:
#
#   slattach: tty_open: cannot set 921600 bps!
#
# and no interface is created at all. stty goes through termios2 and
# takes arbitrary rates.
#
# cs8 matters just as much and fails far more quietly. slattach clears
# CSIZE without setting it, which reads back as cs5, and five data bits
# against the board's eight is not a link that works badly -- every frame
# misassembles, SLIP discards the lot, and the board looks dead while it
# is talking perfectly. `stty raw` does not touch CSIZE either, so the
# obvious thing to try changes nothing.
stty -F "$DEV" "$BAUD" cs8 -parenb -cstopb clocal -crtscts raw -echo

# -L is what makes this work on a USB virtual COM port: it sets CLOCAL, so
# the line is not waiting for a carrier that a VCP never asserts. Without
# it slattach blocks in open() and nothing happens at all.
/usr/sbin/slattach -L -p slip -s"$BAUD" "$DEV" &
SLPID=$!

# slattach creates the interface asynchronously, and names it itself, so
# wait for one to appear that was not there before rather than assuming
# sl0.
IFACE=""
i=0
while [ -z "$IFACE" ]; do
    for n in $(ls /sys/class/net | grep '^sl[0-9]' | sort); do
        echo "$before" | grep -qx "$n" || { IFACE="$n"; break; }
    done
    [ -n "$IFACE" ] && break
    i=$((i + 1))
    if [ "$i" -gt 50 ]; then
        echo "no SLIP interface appeared; is slattach installed (net-tools)?" >&2
        kill $SLPID 2>/dev/null || true
        exit 1
    fi
    sleep 0.1
done

# Again, after slattach: attaching the line discipline rewrites termios,
# so the settings above do not survive it on their own.
stty -F "$DEV" "$BAUD" cs8 -parenb -cstopb clocal -crtscts raw

# A point-to-point address rather than a subnet: there is exactly one host
# at the other end and no routing to do.
ip addr add "$HOST" peer "$BOARD" dev "$IFACE"
ip link set "$IFACE" mtu "$MTU" up

# Loud, because a mis-framed line is silent. If this does not say cs8 the
# link will not pass a single packet.
echo "line: $(stty -F "$DEV" -a | tr ';' '\n' | grep -oE 'cs[5-8]|speed [0-9]+ baud' | tr '\n' ' ')"

echo "$IFACE up: $HOST -> $BOARD at $BAUD on $DEV (slattach pid $SLPID)"
echo "  ping $BOARD"
echo "  telnet $BOARD"
