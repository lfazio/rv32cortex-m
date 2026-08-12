#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
slip-tun.py - the host end of the board's SLIP link, without root.

Why this exists rather than slattach:

  * slattach's -s maps through a fixed table of Bxxx constants that stops
    below 921600, and it does not fall back -- it fails the open outright
    ("tty_open: cannot set 921600 bps!") and creates no interface.

  * slattach clears CSIZE without setting it, so the line comes up at cs5.
    Five data bits against the board's eight means every frame
    misassembles and SLIP discards all of it, silently: the board looks
    dead while it is talking perfectly. `stty raw` does not touch CSIZE
    either, so the obvious thing to try changes nothing.

  * It needs root every session, and cannot be given the privilege any
    other way. The kernel's slip_open() calls capable(CAP_NET_ADMIN),
    which checks the *initial* user namespace, so `unshare -Ur -n` does
    not grant it however complete the capability set inside looks:

        uid=0  CapEff: 000001ffffffffff
        SLIP_set_disc(1): Operation not permitted

    File capabilities do not help either, for the same reason once the
    process is in a child namespace.

A TUN device sidesteps all of it. Root creates one *once*, owned by the
user; after that opening it needs no privilege at all, and SLIP framing
is thirty lines of Python. The serial port is configured here with
termios, so the character size and the speed are ours to get right and
cannot be quietly rewritten by something else attaching to the line.

Usage:

    scripts/slip-tun.py --setup            # once, re-runs itself via sudo
    scripts/slip-tun.py                    # every session, no root

Undo the one-time setup with:

    sudo ip link delete rvslip0
"""

import argparse
import fcntl
import os
import select
import shutil
import struct
import subprocess
import sys
import termios

# <linux/if_tun.h>
TUNSETIFF = 0x400454CA
IFF_TUN = 0x0001
IFF_NO_PI = 0x1000

# <asm-generic/ioctls.h>, <linux/tty.h>
TIOCGETD = 0x5424
N_TTY = 0

# RFC 1055.
END = 0xC0
ESC = 0xDB
ESC_END = 0xDC
ESC_ESC = 0xDD

DEFAULT_DEV = "/dev/ttyACM0"
DEFAULT_BAUD = 921600
DEFAULT_IFACE = "rvslip0"
DEFAULT_HOST = "192.168.7.1"
DEFAULT_BOARD = "192.168.7.2"

# Matches SLIP_MAX_SIZE in src/net/lwipopts.h. SLIP carries no length
# field and cannot negotiate, so a mismatch is not an error either end
# can report: it shows up as ping working and TFTP stalling, because only
# the large packets are affected.
MTU = 1500


def open_serial(path, baud):
    """Open the port and set the line. Every field here has drawn blood."""
    speed = getattr(termios, "B%d" % baud, None)
    if speed is None:
        sys.exit("no termios constant for %d baud" % baud)

    fd = os.open(path, os.O_RDWR | os.O_NOCTTY)

    # The port must be a plain tty. If something left the kernel's own
    # SLIP line discipline attached -- a slattach that is still running,
    # or one that died without closing -- then the ldisc, not this
    # process, owns the byte stream: reads return nothing and tcflush
    # fails with ENOTTY, which reads as a broken device rather than as a
    # port that already has an owner.
    disc = struct.unpack("i", fcntl.ioctl(fd, TIOCGETD, struct.pack("i", 0)))[0]
    if disc != N_TTY:
        os.close(fd)
        sys.exit(
            "%s is in line discipline %d, not N_TTY: something else owns it.\n"
            "  sudo pkill slattach   # and re-run this"
            % (path, disc))

    iflag, oflag, cflag, lflag, ispeed, ospeed, cc = termios.tcgetattr(fd)

    # CS8 explicitly, and CSIZE cleared first: leaving CSIZE alone is how
    # a line ends up at cs5 and carries nothing.
    cflag &= ~termios.CSIZE
    cflag |= termios.CS8 | termios.CREAD
    # CLOCAL, because a USB VCP never asserts carrier and without it the
    # open blocks forever.
    cflag |= termios.CLOCAL
    cflag &= ~termios.PARENB          # no parity
    cflag &= ~termios.CSTOPB          # one stop bit
    cflag &= ~termios.CRTSCTS         # no flow control: there are no wires for it

    # Raw in both directions. Any translation at all corrupts SLIP, which
    # is a byte stream with 0xC0 and 0xDB as framing.
    iflag = 0
    oflag = 0
    lflag = 0

    cc = list(cc)
    cc[termios.VMIN] = 1
    cc[termios.VTIME] = 0

    termios.tcsetattr(
        fd, termios.TCSANOW,
        [iflag, oflag, cflag, lflag, speed, speed, cc])
    # Best effort: discard whatever the board said before we were
    # listening. Not worth failing over if the driver declines it.
    try:
        termios.tcflush(fd, termios.TCIOFLUSH)
    except termios.error:
        pass
    return fd


def open_tun(name):
    try:
        fd = os.open("/dev/net/tun", os.O_RDWR)
    except OSError as e:
        sys.exit("cannot open /dev/net/tun: %s" % e)
    try:
        fcntl.ioctl(fd, TUNSETIFF,
                    struct.pack("16sH22x", name.encode(), IFF_TUN | IFF_NO_PI))
    except OSError as e:
        os.close(fd)
        sys.exit(
            "cannot attach to %s: %s\n"
            "run `%s --setup` once (it needs root exactly once)"
            % (name, e, sys.argv[0]))
    return fd


def is_ipv4(buf):
    """Cheap sanity check before handing a frame to the kernel."""
    if len(buf) < 20 or (buf[0] >> 4) != 4:
        return False
    ihl = (buf[0] & 0x0F) * 4
    if ihl < 20 or ihl > len(buf):
        return False
    total = (buf[2] << 8) | buf[3]
    return 20 <= total <= len(buf)


def slip_encode(pkt):
    out = bytearray([END])
    for b in pkt:
        if b == END:
            out += bytes((ESC, ESC_END))
        elif b == ESC:
            out += bytes((ESC, ESC_ESC))
        else:
            out.append(b)
    out.append(END)
    return bytes(out)


def setup(iface, host, board, user):
    """Create the persistent TUN device. The only step that needs root."""
    if os.geteuid() != 0:
        sudo = shutil.which("sudo")
        if sudo is None:
            sys.exit("--setup needs root and sudo was not found")
        print("re-running --setup under sudo")
        os.execv(sudo, [sudo, sys.executable] + sys.argv)

    def run(*args):
        print("  " + " ".join(args))
        subprocess.run(args, check=False)

    # Owned by the user, so that from here on attaching needs no
    # privilege. Persistent, so the address survives the bridge exiting.
    run("ip", "tuntap", "add", "mode", "tun", "user", user, "name", iface)
    run("ip", "addr", "add", host, "peer", board, "dev", iface)
    run("ip", "link", "set", iface, "mtu", str(MTU), "up")
    print("\n%s is now yours; run without --setup from here on." % iface)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[1])
    ap.add_argument("device", nargs="?", default=DEFAULT_DEV)
    ap.add_argument("baud", nargs="?", type=int, default=DEFAULT_BAUD)
    ap.add_argument("--iface", default=DEFAULT_IFACE)
    ap.add_argument("--host", default=os.environ.get("EMU_NET_PEER", DEFAULT_HOST))
    ap.add_argument("--board", default=os.environ.get("EMU_NET_ADDR", DEFAULT_BOARD))
    ap.add_argument("--setup", action="store_true",
                    help="create the persistent TUN device (needs root, once)")
    args = ap.parse_args()

    if args.setup:
        setup(args.iface, args.host, args.board,
              os.environ.get("SUDO_USER") or os.environ.get("USER") or "root")
        return

    if not os.path.exists(args.device):
        sys.exit("%s does not exist -- is the board plugged in?" % args.device)

    tun = open_tun(args.iface)
    ser = open_serial(args.device, args.baud)

    print("%s <-> %s at %d 8N1" % (args.iface, args.device, args.baud))
    print("  ping %s" % args.board)
    print("  telnet %s" % args.board)
    print("  tftp put <image> rom|ram")

    rx = bytearray()
    escaped = False
    dropped = 0
    junk = 0

    while True:
        ready, _, _ = select.select([tun, ser], [], [])

        if tun in ready:
            pkt = os.read(tun, MTU + 64)
            if pkt:
                os.write(ser, slip_encode(pkt))

        if ser in ready:
            data = os.read(ser, 4096)
            for b in data:
                if escaped:
                    if b == ESC_END:
                        rx.append(END)
                    elif b == ESC_ESC:
                        rx.append(ESC)
                    else:
                        # Not a legal escape. Drop the frame rather than
                        # pass a corrupted packet up.
                        rx.clear()
                    escaped = False
                elif b == ESC:
                    escaped = True
                elif b == END:
                    if rx:
                        # Not everything arriving here is a packet. The
                        # firmware prints "net SLIP on this port" as
                        # plain text and only then hands the UART to the
                        # stack, so the first frame assembled is usually
                        # the tail of that banner -- and a TUN device in
                        # IFF_NO_PI mode takes raw IP and rejects
                        # anything else with EINVAL. Writing it blind
                        # kills the bridge at exactly the moment the
                        # link starts working, which reads as the link
                        # never having worked.
                        if is_ipv4(rx):
                            try:
                                os.write(tun, bytes(rx))
                            except OSError:
                                junk += 1
                        else:
                            junk += 1
                        rx.clear()
                elif len(rx) >= MTU:
                    # Oversized: the far end and this one disagree about
                    # the MTU, which SLIP cannot negotiate or report.
                    dropped += 1
                    rx.clear()
                else:
                    rx.append(b)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        pass
