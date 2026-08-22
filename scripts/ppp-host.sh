#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
#
# ppp-host.sh - the pppd end of the *host* runner's PPP link.
#
#   ./build/hnet/emu-host --ppp <guest>     # prints a /dev/pts/N
#   scripts/ppp-host.sh /dev/pts/N          # this, in another terminal
#
# The board's equivalent is ppp-up.sh and the difference is only which
# device: there it is the ST-LINK's virtual COM port, here it is the pty
# the runner allocated. pppd cannot tell them apart, which is the whole
# point of running the same stack on the host.
#
# Addresses. The runner defaults to a *different* subnet from the board
# (192.168.8.x against 192.168.7.x) so both can be up at once -- a
# development machine with a Nucleo plugged in is the normal case, and
# two links claiming the same peer address means the kernel routes to
# whichever came first while the other silently receives nothing. That
# failure reads as "the emulator's stack does not work", which is the
# wrong conclusion and an expensive one.
#
# Root: pppd checks geteuid() in main() and refuses outright for anyone
# else -- "must be root to run pppd, since it is not setuid-root". File
# capabilities do not help, because the test is on the effective uid and
# not on CAP_NET_ADMIN. So this re-execs under sudo, exactly as ppp-up.sh
# does, rather than pretending otherwise.

set -eu

DEV=${1:-}
HOST_IP=${EMU_NET_PEER:-192.168.8.1}
EMU_IP=${EMU_NET_ADDR:-192.168.8.2}

if [ -z "$DEV" ]; then
    echo "usage: $0 /dev/pts/N   (the runner prints the number)" >&2
    exit 2
fi
if [ ! -e "$DEV" ]; then
    echo "error: $DEV does not exist -- is the runner still up?" >&2
    exit 1
fi

if [ "$(id -u)" -ne 0 ]; then
    exec sudo -- "$0" "$@"
fi

echo "ppp-host: $DEV  $HOST_IP <-> $EMU_IP"
echo "ppp-host: then  telnet $EMU_IP 23   |   tftp $EMU_IP"

# nodetach so ^C brings the link down with the script; local and
# nocrtscts because a pty has no modem control lines to wait on, and
# without them pppd sits forever waiting for a carrier that cannot come.
# /usr/sbin is not on a non-root PATH on most distributions, and the
# failure -- "pppd: not found" -- reads as pppd being missing rather than
# unreachable.
PPPD=$(command -v pppd || true)
: "${PPPD:=/usr/sbin/pppd}"
[ -x "$PPPD" ] || { echo "error: pppd not found; install the ppp package" >&2; exit 1; }

exec "$PPPD" "$DEV" 921600 "$HOST_IP:$EMU_IP" \
    noauth local nocrtscts nodetach \
    ${PPP_DEBUG:+debug kdebug 7}
