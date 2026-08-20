#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
#
# ppp-up.sh - the host end of the board's PPP link.
#
#   scripts/ppp-up.sh                 # /dev/ttyACM0 at 921600
#   scripts/ppp-up.sh /dev/ttyACM1
#   scripts/ppp-up.sh /dev/ttyACM0 115200
#
# Ctrl-C brings the link down.
#
# Why this needs root, and slip-tun.py does not
# ---------------------------------------------
# slip-tun.py avoids privilege by owning a TUN device created once: SLIP
# is a framing rule, so the host end is thirty lines of Python and the
# kernel's SLIP line discipline never enters into it.
#
# PPP is a negotiation, and pppd is the thing that implements it. It
# refuses to run for a non-root user outright -- `geteuid() != 0` is
# checked in main() and the message is "must be root to run pppd, since
# it is not setuid-root". File capabilities do not help: the test is on
# the effective uid, not on CAP_NET_ADMIN. So this re-execs itself under
# sudo rather than pretending otherwise.
#
# That is the trade PPP asks for, and it buys the two ends *agreeing*:
# the addresses are negotiated through IPCP rather than compiled into the
# firmware and configured again here, and a link that goes down is
# detected instead of silently swallowing frames.
#
# **Only one link layer may be configured at a time.** rvslip0 and ppp0
# both come up as 192.168.7.1 peer 192.168.7.2 -- the same addresses, by
# design, so the same host scripts work either way -- and the kernel
# routes to whichever interface was created first. A leftover rvslip0,
# *down and carrierless*, therefore wins silently: pppd negotiates
# perfectly and nothing crosses. This script takes rvslip0 down for you
# and says so, because the symptom otherwise reads as a dead board.

set -eu

DEV=${1:-/dev/ttyACM0}
BAUD=${2:-921600}
HOST_IP=${EMU_NET_PEER:-192.168.7.1}
BOARD_IP=${EMU_NET_ADDR:-192.168.7.2}

if [ ! -e "$DEV" ]; then
    echo "ppp-up: $DEV does not exist -- is the board plugged in?" >&2
    exit 1
fi

if ! command -v pppd >/dev/null 2>&1; then
    echo "ppp-up: pppd not found (apt install ppp)" >&2
    exit 1
fi

if [ "$(id -u)" -ne 0 ]; then
    echo "ppp-up: pppd must run as root; re-execing under sudo"
    exec sudo -- "$0" "$DEV" "$BAUD"
fi

#
# The SLIP interface, if slip-tun.py ever set one up. Down rather than
# deleted: `ip link set rvslip0 up` puts it back, where deleting it would
# mean re-running slip-tun.py --setup.
#
if ip link show rvslip0 >/dev/null 2>&1; then
    if ip -o addr show rvslip0 | grep -q "$BOARD_IP"; then
        echo "ppp-up: taking rvslip0 down -- it claims $BOARD_IP too, and"
        echo "        the kernel would route there instead of ppp0"
        ip link set rvslip0 down
    fi
fi

echo "ppp0 <-> $DEV at $BAUD 8N1"
echo "  ping $BOARD_IP"
echo "  telnet $BOARD_IP"
echo "  tftp put <image>"
echo

#
# local        no modem control lines: a Nucleo's VCP has no DCD, and
#              without this pppd waits for a carrier that never comes.
# nocrtscts    no hardware flow control; none is wired.
# nodefaultroute
#              **do not replace the host's default route.** pppd does
#              this by default, which is right for a dial-up link and
#              would take this machine off the network for a debug cable.
# noauth       the peer is a cable, not a network.
# nodetach     stay in the foreground so Ctrl-C ends the session and the
#              negotiation is visible.
#
exec pppd "$DEV" "$BAUD" \
    noauth nodetach local nocrtscts nodefaultroute \
    "${HOST_IP}:${BOARD_IP}"
