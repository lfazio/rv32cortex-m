#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
board-run.py - flash a firmware build onto its board and capture one run.

    scripts/board-run.py BUILD_DIR [SECONDS]

Flashes BUILD_DIR's firmware through its own `flash` target, resets the
board under reset, and reads the serial console until the run's
`emu-result` line (or SECONDS, default 90). Everything the board printed
goes to stdout, after two lines this script adds:

    flash rc=0 (and reset)
    WALL 14.01 s from the banner to done

WALL is the host's clock on the same run. The firmware reports its own
cycle count, and **a board figure is believed only when the two agree**:
a 32-bit counter once made the JIT look 5x faster while the run took
twice as long (CLAUDE.md).

Three ways a capture lies, each guarded:

  - a failed flash leaves the *previous* firmware running and printing
    plausible results: the flash is retried, and a failure exits 3
    rather than capturing anything;
  - the old firmware can print a whole run while the new one downloads:
    the console is drained after the flash and the board is reset
    explicitly, so only the new image's run is read;
  - more than one banner in the capture means more than one run: said
    on a line of its own.

Where the board is comes from the build: EMU_PROBE in its
CMakeCache.txt names the probe (set it whenever more than one is
attached, or probe-rs waits for a choice it cannot get), and
EMU_PLATFORM the chip. The console is the probe's virtual COM port,
opened by /dev/serial/by-id -- ttyACM numbers swap when a probe
re-enumerates. Override any of them with BOARD_PROBE, BOARD_CHIP and
BOARD_CONSOLE.

BOARD_AT=SECS:CMD runs the shell command CMD SECS after the banner's
`backend` line arrives, while the capture goes on -- something done to
the board in the middle of a run, as scripts/check-nmi-button.sh
presses the user button through the probe. Its exit status is reported
after WALL, as `AT SECS rc=N`, and its output goes to stderr.

The N6 loads through its build's `flash` target too, which is
scripts/n6-load.sh -- ST's gdbserver on the M55's access port, which
probe-rs cannot reach -- and that target *starts* the firmware, so
there is no reset after it: the console is drained before the flash
instead, and the run is whatever follows. Its console is the ST-LINK
V3's first interface. Needs pyserial, and probe-rs for the others.
"""

import os
import subprocess
import sys
import time

try:
    import serial
except ImportError:
    sys.exit("board-run: needs pyserial (python3 -m pip install pyserial)")

CHIPS = {
    "stm32f746": "STM32F746ZGTx",
    "stm32f446": "STM32F446RETx",
}
BAUD = 921600


def cache_var(build, name):
    try:
        with open(os.path.join(build, "CMakeCache.txt")) as f:
            for line in f:
                if line.startswith(name + ":"):
                    return line.split("=", 1)[1].strip()
    except OSError:
        pass
    return ""


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__.strip().splitlines()[2])
    build = sys.argv[1]
    secs = float(sys.argv[2]) if len(sys.argv) > 2 else 90.0

    probe = os.environ.get("BOARD_PROBE") or cache_var(build, "EMU_PROBE")
    platform = cache_var(build, "EMU_PLATFORM")
    n6 = platform == "stm32n6"
    chip = os.environ.get("BOARD_CHIP") or CHIPS.get(platform, "")
    if not probe or (not chip and not n6):
        sys.exit("board-run: %s names no probe (EMU_PROBE) or no known "
                 "platform (%r); set BOARD_PROBE / BOARD_CHIP" %
                 (build, platform))
    serialno = probe.split(":")[-1]
    console = os.environ.get("BOARD_CONSOLE") or (
        ("/dev/serial/by-id/usb-STMicroelectronics_STLINK-V3_%s-if01" if n6
         else "/dev/serial/by-id/usb-STMicroelectronics_STM32_STLink_%s-if02")
        % serialno)

    ser = serial.Serial(console, BAUD, timeout=0.01)
    if n6:
        ser.reset_input_buffer()  # the flash target starts the run

    loader = None
    if n6:
        # In the background: the load starts the firmware before it
        # returns, and the console has to be read from that moment --
        # arrival times are the wall clock, and a tty left unread drops.
        loader = subprocess.Popen(["cmake", "--build", build, "--target",
                                   "flash"], stdout=subprocess.PIPE,
                                  stderr=subprocess.STDOUT,
                                  universal_newlines=True)
    r = None
    for _ in range(0 if n6 else 3):
        r = subprocess.run(["cmake", "--build", build, "--target", "flash"],
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                           universal_newlines=True)
        if r.returncode == 0:
            break
        time.sleep(2)
    if r is not None and r.returncode != 0:
        print("FLASH FAILED rc=%d -- the board still runs the PREVIOUS "
              "image" % r.returncode)
        print(r.stdout[-400:])
        sys.exit(3)

    if n6:
        pass  # the load's result is read after the capture
    else:
        time.sleep(0.5)
        ser.reset_input_buffer()
        r = subprocess.run(["probe-rs", "reset", "--chip", chip,
                            "--connect-under-reset", "--probe", probe],
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                           universal_newlines=True)
        if r.returncode != 0:
            print("RESET FAILED rc=%d" % r.returncode)
            print(r.stdout[-400:])
            sys.exit(3)
        print("flash rc=0 (and reset)")

    at_secs, _, at_cmd = os.environ.get("BOARD_AT", "").partition(":")
    at_proc = None
    banner_at = None

    chunks = []  # (arrival time, offset into buf)
    buf = b""
    end = time.time() + secs
    while time.time() < end:
        d = ser.read(4096)
        if d:
            chunks.append((time.time(), len(buf)))
            buf += d
        if at_cmd and banner_at is None and b"backend " in buf:
            banner_at = time.time()
        if (at_cmd and at_proc is None and banner_at is not None and
                time.time() - banner_at >= float(at_secs)):
            # Not waited for here: the console keeps being read.
            at_proc = subprocess.Popen(at_cmd, shell=True,
                                       stdout=sys.stderr, stderr=sys.stderr)
        if d:
            # The run is over: a second's grace for whatever follows --
            # once *this* run's banner has come and its result after it.
            # The flash target starts the firmware before the reset above
            # starts it again, and a guest shorter than the reset takes
            # finishes that first run meanwhile: its result, with its
            # banner cleared from the line, ended a capture one second
            # after the real run's banner (RV32 CoreMark, 1.75 s).
            last = buf.rfind(b"\nemu: ")
            if (last >= 0 and buf.find(b"emu-result", last) >= 0 and
                    time.time() + 1.0 < end):
                end = time.time() + 1.0

    if loader is not None:
        out, _ = loader.communicate()
        if loader.returncode != 0:
            print("FLASH FAILED rc=%d -- the board still runs the PREVIOUS "
                  "image" % loader.returncode)
            print(out[-400:])
            sys.exit(3)
        print("flash rc=0 (the load started it)")

    def when(off):
        t = None
        for ts, o in chunks:
            if o <= off:
                t = ts
            else:
                break
        return t

    start = buf.rfind(b"backend ")
    done = buf.find(b"-- done --", start if start >= 0 else 0)
    # What came before the last banner was a run the flash started, not
    # this one: dropped, so its figures are not read as this run's.
    head = buf.rfind(b"\nemu: ")
    if head > 0 and buf[:head].strip():
        print("EARLIER %d bytes before the banner dropped: a run the flash "
              "target started" % head)
    if buf.count(b"\nemu: ") != 1:
        print("BANNERS %d -- expected exactly one run in the capture" %
              buf.count(b"\nemu: "))
    if start >= 0 and done >= 0:
        print("WALL %.2f s from the banner to done" %
              (when(done) - when(start)))
    else:
        print("WALL ?  (banner %d, done %d) -- the run did not finish "
              "inside the capture" % (start, done))
    if at_cmd:
        if at_proc is None:
            print("AT %s never ran: no banner inside the capture" % at_secs)
        else:
            print("AT %s rc=%d" % (at_secs, at_proc.wait()))
    sys.stdout.write(buf[max(head, 0):].decode("latin-1", "replace"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
