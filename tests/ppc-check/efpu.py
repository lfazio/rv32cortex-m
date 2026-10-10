#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""efpu.py - the e200z7's embedded FP unit against exact arithmetic.

    tests/ppc-check/efpu.py --tool build/x/ppc-fpu [--seed N] [--count N]

ppc_fpu.c computes in SoftFloat's double precision, toward zero, and
takes its sticky bit from the inexact flag. This computes the same
operations in *rationals* -- Python's Fraction, which does not round at
all -- and rounds once, itself. The two share the manual and nothing
else: no rounding code, no intermediate format, no flag logic.

What is checked, for every vector: the result, the whole of SPEFSCR
(the per-instruction status, the sticky bits, FG and FX), which
interrupt was asked for, and for a compare the CR field.

The rules are EFPU2's mode 0, from the e200z759n3 manual's chapter 5
and its tables 5-2 to 5-5:

  - Inf and NaN operands give a fixed default, a denormal counts as a
    zero of its sign, and either sets FINV;
  - overflow gives the largest normal number and underflow a zero, both
    decided *after* rounding with an unbounded exponent;
  - an enabled invalid, divide-by-zero, overflow or underflow suppresses
    the result (the data interrupt, IVOR33);
  - an enabled inexact writes the result *truncated* and reports the
    guard and sticky bits (the round interrupt, IVOR34).

Like everything else about this frontend, none of it has been run
against an e200. Where this file had to choose -- a negative value
converted to unsigned, for one -- it makes the choice ppc_fpu.c
documents, and the agreement on those vectors is two readers of one
sentence rather than evidence about the core.
"""

import argparse
import random
import subprocess
import sys
from fractions import Fraction
from math import isqrt

PMAX, NMAX = 0x7F7FFFFF, 0xFF7FFFFF
RN, RZ, RP, RM = 0, 1, 2, 3

FG, FX = 1 << 13, 1 << 12
FINV, FDBZ, FUNF, FOVF = 1 << 11, 1 << 10, 1 << 9, 1 << 8
FINXS, FINVS, FDBZS, FUNFS, FOVFS = 1 << 21, 1 << 20, 1 << 19, 1 << 18, 1 << 17
FINXE, FINVE, FDBZE, FUNFE, FOVFE = 1 << 6, 1 << 5, 1 << 4, 1 << 3, 1 << 2
STATUS = 0xFF000000 | FG | FX | FINV | FDBZ | FUNF | FOVF


def kind(x):
    e, f = (x >> 23) & 0xFF, x & 0x7FFFFF
    if e == 0:
        return "z" if f == 0 else "d"
    return "s" if e == 255 else "n"


def sgn(x):
    return x >> 31


def is_nan(x):
    return (x & 0x7FFFFFFF) > 0x7F800000


def fmax(s):
    return NMAX if s else PMAX


def fzero(s):
    return s << 31


def value(x):
    """A normal single as an exact rational."""
    m = (x & 0x7FFFFF) | 0x800000
    v = Fraction(m) * Fraction(2) ** (((x >> 23) & 0xFF) - 150)
    return -v if sgn(x) else v


def flushed(x):
    """An operand as arithmetic sees it: a denormal is a zero."""
    return Fraction(0) if kind(x) in "zd" else value(x)


class Res:
    def __init__(self):
        self.val = self.trunc = 0
        self.inv = self.dbz = self.ovf = self.unf = self.inx = False
        self.fg = self.fx = False

    def default(self, v):
        self.val = self.trunc = v
        return self


def round_up(mode, neg, g, x, odd):
    if mode == RN:
        return g and (x or odd)
    if mode == RP:
        return (g or x) and not neg
    if mode == RM:
        return (g or x) and neg
    return False


def round_single(r, q, mode, unf_by_mode, sticky=False):
    """q, a non-zero rational, to single precision; `sticky` says more
    lies below it than q shows (a square root's tail)."""
    neg = q < 0
    a = -q if neg else q
    e = a.numerator.bit_length() - a.denominator.bit_length()
    if Fraction(2) ** e > a:
        e -= 1
    scaled = a / Fraction(2) ** (e - 23)        # 2^23 <= scaled < 2^24
    t = scaled.numerator // scaled.denominator
    rem = scaled - t
    g = rem >= Fraction(1, 2)
    x = (rem - (Fraction(1, 2) if g else 0)) != 0 or sticky
    t0, e0 = t, e
    if round_up(mode, neg, g, x, t & 1):
        t += 1
        if t == 1 << 24:
            t >>= 1
            e += 1
    s = 1 if neg else 0
    r.inx, r.fg, r.fx = g or x, g, x
    if e > 127:
        r.ovf = True
        return r.default(fmax(s))
    if e < -126:
        r.unf = True
        return r.default(fzero((1 if mode == RM else 0) if unf_by_mode else s))
    r.val = (s << 31) | ((e + 127) << 23) | (t & 0x7FFFFF)
    r.trunc = (fmax(s) if e0 > 127 else fzero(s) if e0 < -126
               else (s << 31) | ((e0 + 127) << 23) | (t0 & 0x7FFFFF))
    return r


def zero_sum(sa, sb, mode):
    """The sign of an exact zero sum of two terms with these signs."""
    return fzero(sa if sa == sb else (1 if mode == RM else 0))


def op_add(a, b, sub, mode):
    r = Res()
    ka, kb = kind(a), kind(b)
    sb = sgn(b) ^ (1 if sub else 0)
    r.inv = ka in "sd" or kb in "sd"
    if ka == "s":
        return r.default(fmax(sgn(a)))
    if kb == "s":
        return r.default(fmax(sb))
    q = flushed(a) + (-flushed(b) if sub else flushed(b))
    if q == 0:
        return r.default(zero_sum(sgn(a), sb, mode))
    return round_single(r, q, mode, True)


def op_mul(a, b, mode):
    r = Res()
    ka, kb = kind(a), kind(b)
    s = sgn(a) ^ sgn(b)
    r.inv = ka in "sd" or kb in "sd"
    if ka in "zd" or kb in "zd":
        return r.default(fzero(s))
    if ka == "s" or kb == "s":
        return r.default(fmax(s))
    return round_single(r, value(a) * value(b), mode, False)


def op_div(a, b, mode):
    r = Res()
    ka, kb = kind(a), kind(b)
    s = sgn(a) ^ sgn(b)
    r.inv = ka in "sd" or kb in "sd"
    if kb == "s":
        return r.default(fzero(s))
    if kb in "zd":
        if ka == "z":
            r.inv = True
        elif ka == "n" and kb == "z":
            r.dbz = True
        return r.default(fmax(s))
    if ka == "s":
        return r.default(fmax(s))
    if ka in "zd":
        return r.default(fzero(s))
    return round_single(r, value(a) / value(b), mode, False)


def op_madd(a, b, d, sub, neg, mode):
    """Table 5-3. The negated forms negate the rounded result."""
    r = Res()
    ka, kb, kd = kind(a), kind(b), kind(d)
    sp = sgn(a) ^ sgn(b)
    sd = sgn(d) ^ (1 if sub else 0)
    r.inv = any(k in "sd" for k in (ka, kb, kd))
    pzero = ka in "zd" or kb in "zd"
    if not pzero and (ka == "s" or kb == "s"):
        r.default(fmax(sp))
    elif kd == "s":
        r.default(fmax(sd))
    elif pzero:
        if kd in "zd":
            r.default(zero_sum(sp, sd, mode))
        else:
            r.default((d & 0x7FFFFFFF) | (sd << 31))
    else:
        addend = flushed(d)
        q = value(a) * value(b) + (-addend if sub else addend)
        if q == 0:
            r.default(zero_sum(sp, sd, mode))
        else:
            round_single(r, q, mode, True)
    if neg:
        r.val ^= 0x80000000
        r.trunc ^= 0x80000000
    return r


def op_sqrt(a, mode):
    r = Res()
    ka, s = kind(a), sgn(a)
    if ka == "z":
        return r.default(a)
    if ka == "d":
        r.inv = True
        return r.default(fzero(s))
    if s:
        r.inv = True
        return r.default(0x80000000)
    if ka == "s":
        r.inv = True
        return r.default(PMAX)
    v = value(a)
    # v * 4^k is an integer, and its root has well over the 26 bits a
    # rounding needs even for the smallest normal: at k = 80 the root of
    # 2^-125 had 18, and the first run of this file "found" forty wrong
    # square roots in an implementation that had them right.
    k = 240
    n = v * Fraction(4) ** k
    root = isqrt(n.numerator)
    return round_single(r, Fraction(root, 1 << k), mode, True,
                        sticky=root * root != n.numerator)


def order(x, zero_signed):
    m = x & 0x7FFFFFFF
    if m == 0 and not zero_signed:
        return 0
    return (-m - (1 if zero_signed else 0)) if sgn(x) else m


def op_minmax(a, b, want_max):
    r = Res()
    r.inv = kind(a) in "sd" or kind(b) in "sd"
    oa, ob = order(a, True), order(b, True)
    t = (b if oa < ob else a) if want_max else (a if oa < ob else b)
    if is_nan(a) and not is_nan(b):
        t = b
    elif is_nan(b) and not is_nan(a):
        t = a
    if kind(t) == "d":
        t &= 0x80000000
    elif kind(t) == "s":
        t = fmax(sgn(t))
    return r.default(t)


def cvt_from(b, signed, scale, mode):
    r = Res()
    v = (b - (1 << 32) if signed and b >> 31 else b)
    if v == 0:
        return r.default(0)
    return round_single(r, Fraction(v, 1 << scale), mode, False)


def cvt_to(b, signed, scale, mode):
    r = Res()
    kb, s = kind(b), sgn(b)
    hi = 0x7FFFFFFF if signed else 0xFFFFFFFF
    lo = 0x80000000 if signed else 0
    if kb == "z":
        return r.default(0)
    if kb == "d" or is_nan(b):
        r.inv = True
        return r.default(0)
    if kb == "s":
        r.inv = True
        return r.default(lo if s else hi)
    if not signed and s:
        r.inv = True
        return r.default(0)
    a = abs(value(b)) * (1 << scale)
    ip = a.numerator // a.denominator
    rem = a - ip
    g = rem >= Fraction(1, 2)
    x = (rem - (Fraction(1, 2) if g else 0)) != 0
    mag = ip + (1 if round_up(mode, bool(s), g, x, ip & 1) else 0)
    lim = (0x80000000 if signed else 0) if s else hi
    if mag > lim or ip > lim:
        r.inv = True
        return r.default(lo if s else hi)
    r.val = (-mag) & 0xFFFFFFFF if s else mag
    r.trunc = (-ip) & 0xFFFFFFFF if s else ip
    r.inx, r.fg, r.fx = g or x, g, x
    return r


def cvt_from_half(b):
    r = Res()
    s, e, f = (b >> 15) & 1, (b >> 10) & 31, b & 0x3FF
    if e == 0 and f == 0:
        return r.default(fzero(s))
    if e == 31:
        r.inv = True
        return r.default(fmax(s))
    if e == 0:
        r.inv = True
        return r.default(fzero(s))
    return r.default((s << 31) | ((e - 15 + 127) << 23) | (f << 13))


def cvt_to_half(b, mode):
    r = Res()
    kb, s = kind(b), sgn(b)
    hmax = (s << 15) | 0x7BFF
    if kb == "z":
        return r.default(s << 15)
    if kb == "s":
        r.inv = True
        return r.default(hmax)
    if kb == "d":
        r.inv = True
        return r.default(s << 15)
    a = abs(value(b))
    e = a.numerator.bit_length() - a.denominator.bit_length()
    if Fraction(2) ** e > a:
        e -= 1
    scaled = a / Fraction(2) ** (e - 10)          # 2^10 <= scaled < 2^11
    t = scaled.numerator // scaled.denominator
    rem = scaled - t
    g = rem >= Fraction(1, 2)
    x = (rem - (Fraction(1, 2) if g else 0)) != 0
    t0, e0 = t, e
    if round_up(mode, bool(s), g, x, t & 1):
        t += 1
        if t == 1 << 11:
            t >>= 1
            e += 1
    r.inx, r.fg, r.fx = g or x, g, x
    if e > 15:
        r.ovf = True
        return r.default(hmax)
    if e < -14:
        r.unf = True
        return r.default(s << 15)
    r.val = (s << 15) | ((e + 15) << 10) | (t & 0x3FF)
    r.trunc = hmax if e0 > 15 else (s << 15) | ((e0 + 15) << 10) | (t0 & 0x3FF)
    return r


COMPARES = {"efscmpgt": ">", "efscmplt": "<", "efscmpeq": "=",
            "efststgt": ">", "efststlt": "<", "efststeq": "="}


def model(mn, spefscr, a, b, d):
    """Returns (rD, SPEFSCR, interrupt, CR field)."""
    mode = spefscr & 3
    if mn in COMPARES:
        oa, ob = order(a, False), order(b, False)
        cl = {">": oa > ob, "<": oa < ob, "=": oa == ob}[COMPARES[mn]]
        if mn.startswith("efstst"):
            return d, spefscr, 0, 4 if cl else 0
        f = spefscr & ~STATUS & 0xFFFFFFFF
        if kind(a) in "sd" or kind(b) in "sd":
            f |= FINV | FINVS
            if f & FINVE:
                return d, f, 33, 0
        return d, f, 0, 4 if cl else 0
    r = {
        "efsadd": lambda: op_add(a, b, False, mode),
        "efssub": lambda: op_add(a, b, True, mode),
        "efsmul": lambda: op_mul(a, b, mode),
        "efsdiv": lambda: op_div(a, b, mode),
        "efsmadd": lambda: op_madd(a, b, d, False, False, mode),
        "efsmsub": lambda: op_madd(a, b, d, True, False, mode),
        "efsnmadd": lambda: op_madd(a, b, d, False, True, mode),
        "efsnmsub": lambda: op_madd(a, b, d, True, True, mode),
        "efssqrt": lambda: op_sqrt(a, mode),
        "efsmax": lambda: op_minmax(a, b, True),
        "efsmin": lambda: op_minmax(a, b, False),
        "efscfsi": lambda: cvt_from(b, True, 0, mode),
        "efscfui": lambda: cvt_from(b, False, 0, mode),
        "efscfsf": lambda: cvt_from(b, True, 31, mode),
        "efscfuf": lambda: cvt_from(b, False, 32, mode),
        "efsctsi": lambda: cvt_to(b, True, 0, mode),
        "efsctui": lambda: cvt_to(b, False, 0, mode),
        "efsctsiz": lambda: cvt_to(b, True, 0, RZ),
        "efsctuiz": lambda: cvt_to(b, False, 0, RZ),
        "efsctsf": lambda: cvt_to(b, True, 31, mode),
        "efsctuf": lambda: cvt_to(b, False, 32, mode),
        "efscfh": lambda: cvt_from_half(b & 0xFFFF),
        "efscth": lambda: cvt_to_half(b, mode),
    }.get(mn)
    if r is not None:
        r = r()
    else:
        r = Res()
        r.inv = kind(a) in "sd"
        r.default({"efsabs": a & 0x7FFFFFFF, "efsnabs": a | 0x80000000,
                   "efsneg": a ^ 0x80000000}[mn])
    f = spefscr & ~STATUS & 0xFFFFFFFF
    if r.inv:
        f |= FINV | FINVS
    if r.dbz:
        f |= FDBZ | FDBZS
    if r.unf:
        f |= FUNF | FUNFS
    if r.ovf:
        f |= FOVF | FOVFS
    err = r.inv or r.dbz or r.ovf or r.unf
    if not err:
        f |= (FG if r.fg else 0) | (FX if r.fx else 0)
    if ((r.inv and f & FINVE) or (r.dbz and f & FDBZE) or (r.unf and f & FUNFE)
            or (r.ovf and f & FOVFE)):
        return d, f, 33, 0
    inx = (r.inx and not r.inv and not r.dbz) or r.ovf or r.unf
    if inx:
        f |= FINXS
    if inx and f & FINXE:
        return r.trunc, f, 34, 0
    return r.val, f, 0, 0


# The conversions take their operand in rB; everything else in rA (and rB).
UNARY_A = ["efsabs", "efsnabs", "efsneg", "efssqrt"]
UNARY_B = ["efscfsi", "efscfui", "efscfsf", "efscfuf", "efsctsi", "efsctui",
           "efsctsiz", "efsctuiz", "efsctsf", "efsctuf", "efscfh", "efscth"]
BINARY = ["efsadd", "efssub", "efsmul", "efsdiv", "efsmax", "efsmin"] + sorted(COMPARES)
TERNARY = ["efsmadd", "efsmsub", "efsnmadd", "efsnmsub"]

# Every class of operand, both signs, and the values rounding turns on.
FP = [0x00000000, 0x80000000, 0x00000001, 0x80000001, 0x007FFFFF, 0x807FFFFF,
      0x00800000, 0x80800000, 0x00800001, 0x7F7FFFFF, 0xFF7FFFFF, 0x7F7FFFFE,
      0x7F800000, 0xFF800000, 0x7FC00000, 0xFFC00000, 0x7F800001, 0xFF800001,
      0x3F800000, 0xBF800000, 0x3F800001, 0x3F7FFFFF, 0x40000000, 0x3F000000,
      0x3FC00000, 0x40490FDB, 0x4B800000, 0x4B7FFFFF, 0x4F000000, 0xCF000000,
      0x4F800000, 0x4EFFFFFF, 0x5F000000, 0x33800000, 0x34000000, 0x00FFFFFF,
      0x7E800000, 0x01000000, 0x3EAAAAAB, 0x41200000]
INTS = [0, 1, 2, 0xFFFFFFFF, 0x80000000, 0x7FFFFFFF, 0x80000001, 0x00FFFFFF,
        0x01000000, 0x01000001, 0x01000003, 0xFFFFFF80, 0x40000000, 0x7FFFFF80,
        0x7FFFFFC0, 0xFFFFFF00, 0x3C00, 0x7C00, 0xFC00, 0x0001, 0x0400, 0x7BFF,
        0x8000, 0x03FF]


def rnd_fp(rng):
    if rng.random() < 0.45:
        return rng.choice(FP)
    # A normal number, with exponents clustered so sums and products
    # reach the edges of the range rather than the middle of it.
    e = rng.choice([rng.randrange(1, 255), rng.randrange(1, 6), rng.randrange(250, 255),
                    rng.randrange(120, 135)])
    return (rng.getrandbits(1) << 31) | (e << 23) | rng.getrandbits(23)


def enables(rng):
    """Mostly none, then each alone, then several: the defaults are the
    common case and each interrupt needs its own vectors."""
    x = rng.random()
    if x < 0.6:
        return 0
    if x < 0.9:
        return rng.choice([FINXE, FINVE, FDBZE, FUNFE, FOVFE])
    return rng.getrandbits(5) << 2


#
# Operands that are an edge only *together*, which no table of values
# and no random pair produces. Each is here because a deliberate defect
# in ppc_fpu.c survived the vectors without it.
#
#   0x00800001 * 0x3F7FFFFE   just under the smallest normal, and near
#       enough to round up into range: not an underflow, since that is
#       decided after rounding -- but its *truncation* is below the
#       range, which is what the round interrupt has to write.
#
DIRECTED = [
    ("efsmul", 0x00800001, 0x3F7FFFFE, 0),
    ("efsmul", 0x80800001, 0x3F7FFFFE, 0),
    ("efsdiv", 0x00FFFFFF, 0x40000000, 0),
    ("efsmadd", 0x00800001, 0x3F7FFFFE, 0x00000000),
    ("efsadd", 0x00800000, 0x807FFFFF, 0),
    ("efssub", 0x00800001, 0x00800000, 0),
]


def vectors(seed, count):
    rng = random.Random(seed)
    out = []
    for mn, a, b, d in DIRECTED:
        for mode in range(4):
            for en in (0, FINXE, FUNFE, FINXE | FUNFE):
                out.append((mn, mode | en, a, b, d))
    # Every pair of special operands, for the operations with tables.
    for mn in ["efsadd", "efssub", "efsmul", "efsdiv", "efsmax", "efsmin",
               "efscmpgt", "efscmplt", "efscmpeq"]:
        for a in FP:
            for b in FP:
                out.append((mn, rng.randrange(4), a, b, 0))
    for mn in UNARY_A:
        for a in FP:
            for m in range(4):
                out.append((mn, m, a, 0, 0x12345678))
    for mn in UNARY_B:
        for b in FP + INTS:
            for m in range(4):
                out.append((mn, m, 0, b, 0x12345678))
    for i in range(count):
        mn = rng.choice(BINARY * 3 + TERNARY * 4 + UNARY_A + UNARY_B)
        sp = rng.randrange(4) | enables(rng)
        if rng.random() < 0.3:
            sp |= rng.choice([FINXS, FINVS, FDBZS, FUNFS, FOVFS, FG | FX, FINV | FOVF])
        a, b, d = rnd_fp(rng), rnd_fp(rng), rnd_fp(rng)
        if mn in UNARY_B and rng.random() < 0.5:
            b = rng.choice(INTS) if rng.random() < 0.5 else rng.getrandbits(32)
        out.append((mn, sp, a, b, d))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tool", required=True)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--count", type=int, default=20000)
    ap.add_argument("-v", "--verbose", type=int, default=12)
    a = ap.parse_args()
    vs = vectors(a.seed, a.count)
    text = "".join("%s %x %x %x %x\n" % v for v in vs)
    got = subprocess.run([a.tool], input=text, stdout=subprocess.PIPE,
                         universal_newlines=True).stdout.splitlines()
    if len(got) != len(vs):
        print("the tool answered %d of %d vectors" % (len(got), len(vs)))
        sys.exit(1)
    bad = 0
    per = {}
    irq = {0: 0, 33: 0, 34: 0}
    for v, g in zip(vs, got):
        want = model(*v)
        irq[want[2]] += 1
        gf = g.split()
        have = (int(gf[0], 16), int(gf[1], 16), int(gf[2]), int(gf[3], 16))
        if have != want:
            bad += 1
            per[v[0]] = per.get(v[0], 0) + 1
            if bad <= a.verbose:
                print("  %-9s spefscr %08x a %08x b %08x d %08x\n"
                      "      got  %08x %08x irq %d cr %x\n"
                      "      want %08x %08x irq %d cr %x" % (v + have + want))
    print("efpu seed %d: %d vectors (%d default results, %d data interrupts, "
          "%d round interrupts): %d wrong%s" % (
              a.seed, len(vs), irq[0], irq[33], irq[34], bad,
              "" if not per else "  " + ", ".join(
                  "%s %d" % kv for kv in sorted(per.items()))))
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
