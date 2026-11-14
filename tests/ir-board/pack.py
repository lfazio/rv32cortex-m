#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""pack.py - a fuzz-ir corpus as one blob for the replay firmware.

    tests/ir-board/pack.py CORPUS_DIR OUT [FIRST [COUNT]]

Each input, in name order, as a little-endian 16-bit length and its
bytes; a length of 0 ends the blob. FIRST and COUNT take a slice, for a
run that resumes past a case that faulted. Prints how many it packed.
"""
import os
import struct
import sys


def main():
    src, out = sys.argv[1], sys.argv[2]
    first = int(sys.argv[3]) if len(sys.argv) > 3 else 0
    count = int(sys.argv[4]) if len(sys.argv) > 4 else None
    names = sorted(n for n in os.listdir(src)
                   if os.path.isfile(os.path.join(src, n)))
    names = names[first:] if count is None else names[first:first + count]
    blob = bytearray()
    n = 0
    for name in names:
        with open(os.path.join(src, name), "rb") as f:
            data = f.read()
        if not data or len(data) > 0xFFFF:
            continue
        blob += struct.pack("<H", len(data)) + data
        n += 1
    blob += struct.pack("<H", 0)
    with open(out, "wb") as f:
        f.write(blob)
    print(n)


if __name__ == "__main__":
    main()
