#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
Every emu_console_printf format against the console's buffer.

emu_console_printf formats into a fixed buffer and vsnprintf cuts what
does not fit, so a format whose widest output is longer loses its end --
and its last newline, which glues the next line onto it. The banner lost
its end that way once the ISA string grew, the `-- ir --` report once it
grew three counters, and the JIT statistics' interp/declined/blk entr
call once a long run's counts reached ten digits: the thrash line read
as part of the one before.

So the widest output is computed, not hoped for: a %u is ten
characters, a %d eleven, a %x eight, a %llu twenty, a field width at
least its width. A %s is the caller's -- a name, a path -- and is
listed with the width it would need to be under, not counted.

The buffer's size is read from emu_console.c, so the check follows it.

    scripts/check-console-width.py          # fail on a format over
    scripts/check-console-width.py --list   # every format and its width
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent

# widest output of each conversion, by its letter and length modifier
WIDEST = {"u": 10, "d": 11, "i": 11, "x": 8, "X": 8, "o": 11, "c": 1,
          "lu": 20, "ld": 20, "lx": 16, "llu": 20, "lld": 20, "llx": 16,
          "zu": 20, "p": 18}
CONV = re.compile(r"%([-+ #0]*)([0-9]*)(?:\.([0-9]+))?(hh|h|ll|l|z|j|t)?([a-zA-Z%])")
CALL = re.compile(r"emu_console_printf\s*\(")
LIT = re.compile(r'\s*"((?:[^"\\]|\\.)*)"')


def buffer_size():
    src = (ROOT / "src/platform/common/emu_console.c").read_text()
    body = src[src.index("void emu_console_printf"):]
    m = re.search(r"char\s+buf\[(\d+)\]", body)
    if m is None:
        sys.exit("check-console-width: no `char buf[N]` in emu_console_printf")
    return int(m.group(1))


def unescape_len(lit):
    return len(re.sub(r"\\(x[0-9a-fA-F]+|[0-7]{1,3}|.)", "_", lit))


def formats():
    for path in sorted((ROOT / "src").rglob("*.c")):
        text = path.read_text(errors="replace")
        for m in CALL.finditer(text):
            pos, parts = m.end(), []
            while True:
                lm = LIT.match(text, pos)
                if lm is None:
                    break
                parts.append(lm.group(1))
                pos = lm.end()
            if not parts:
                continue  # not a literal format: nothing to compute
            line = text.count("\n", 0, m.start()) + 1
            yield path.relative_to(ROOT), line, "".join(parts)


def widest(fmt):
    """(characters at most, number of %s), %s counted as nothing"""
    n, strings, last = 0, 0, 0
    for c in CONV.finditer(fmt):
        n += unescape_len(fmt[last:c.start()])
        last = c.end()
        flags, width, prec, mod, letter = c.groups()
        width = int(width) if width else 0
        if letter == "%":
            n += 1
        elif letter == "s":
            strings += 1
            n += width
        elif letter in "fFeEgG":
            n += max(width, 24)  # none on the board; generous on the host
        else:
            key = (mod or "") + letter
            if key not in WIDEST:
                sys.exit(f"check-console-width: no width for %{key} in {fmt!r}")
            n += max(width, WIDEST[key] + (1 if "+" in flags or " " in flags else 0))
    return n + unescape_len(fmt[last:]), strings


def main():
    size = buffer_size()
    limit = size - 1  # vsnprintf keeps one for the NUL
    over, listed = [], []
    for path, line, fmt in formats():
        n, strings = widest(fmt)
        listed.append((n, strings, path, line))
        if n > limit:
            over.append((n, path, line))
    if "--list" in sys.argv:
        for n, strings, path, line in sorted(listed, reverse=True):
            tail = f", {strings} %s in {limit - n} left" if strings else ""
            print(f"{n:4} {path}:{line}{tail}")
    for n, path, line in over:
        print(f"{path}:{line}: up to {n} characters, the console's buffer holds {limit}")
    print(f"check-console-width: {len(listed)} formats, {len(over)} over {limit}")
    return 1 if over else 0


if __name__ == "__main__":
    sys.exit(main())
