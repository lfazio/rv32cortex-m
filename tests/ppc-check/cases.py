#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""cases.py - generated e200z7 instructions, three ways.

    tests/ppc-check/cases.py --emu build/x/emu-host [--mode b|v] [--seed N]
                             [--count N] [--out DIR] [--only CLASS,...]

Each case loads five registers, CR, XER, CTR and LR from a seeded
generator, executes one instruction (or the two or three a memory or
branch test needs) and prints all nine. The same image is run by the
interpreter and by the JIT, and the two outputs must be identical; and
for every case this file can work out for itself, both must match that
too.

Three opinions, then, of which two are this project's:

  the interpreter   ppc_interp.c, executing ppc_decode's output
  the translator    ppc_ir.c, lowering the same decode to IR
  the model         below: Python integers and nothing from the tree

There is no fourth. No e200 is attached to this project, and no other
emulator is consulted. So the model is deliberately a *different shape*
from the interpreter -- arithmetic on unbounded integers, one function
per mnemonic rather than per semantic class -- because a second copy of
the same code would only agree with the first. What it cannot tell
apart from a correct implementation is a shared misreading of the
manual, and that limit is the reason every statement about this
frontend says "not run against an e200".

A quarter of all operands come from a table of special values: a random
32-bit number is almost never where carry, overflow, a shift by 32 or a
sign boundary live. And some inputs are not on any table because they
are a *relation*: a divide's INT_MIN over -1, a subtract-from-immediate
whose register equals the immediate (the one input that tells a carry
computed as >= from one computed as >). Those are put in on purpose.
Both were found by breaking the translator and watching 6,000 random
cases agree with it.

Mode b is classic Book E and mode v is VLE; the encodings differ and the
semantics are shared, so most classes exist in both and the VLE-only
forms (se_*, the SCI8 and I16 immediates) in v alone.
"""

import argparse
import os
import random
import re
import subprocess
import sys

M = 0xFFFFFFFF
SO, OV, CA = 0x80000000, 0x40000000, 0x20000000

AS = "powerpc64-linux-gnu-as"
LD = "powerpc64-linux-gnu-ld"
for d in os.environ.get("PATH", "").split(os.pathsep):
    if os.path.exists(os.path.join(d, "powerpc-linux-gnu-as")):
        AS, LD = "powerpc-linux-gnu-as", "powerpc-linux-gnu-ld"

SPECIAL = [0, 1, 2, 3, 0xFFFFFFFF, 0xFFFFFFFE, 0x80000000, 0x80000001,
           0x7FFFFFFF, 0xFFFF, 0x10000, 0x8000, 0x7FFF, 0xFFFF8000, 0xFF,
           0x80, 0x7F, 31, 32, 33, 63, 64, 0x55555555, 0xAAAAAAAA]

REGS = [3, 4, 5, 6, 7]  # all reachable by the 16-bit forms
BUF = 64                # bytes of scratch memory, at r11


def s32(x):
    x &= M
    return x - (1 << 32) if x & 0x80000000 else x


def sx(x, bits):
    x &= (1 << bits) - 1
    return x - (1 << bits) if x >> (bits - 1) else x


def rotl(x, n):
    n &= 31
    return ((x << n) | (x >> (32 - n))) & M if n else x & M


def mask(mb, me):
    a = M >> mb
    b = (M << (31 - me)) & M
    return (a & b) if mb <= me else (a | b)


class State:
    """What a case starts from and what it prints."""

    def __init__(self, rng, mem):
        self.r = {}
        for n in REGS:
            self.r[n] = rng.choice(SPECIAL) if rng.random() < 0.25 else rng.getrandbits(32)
        self.cr = rng.getrandbits(32)
        self.xer = rng.choice([0, SO, OV | SO, CA, CA | SO, CA | OV | SO])
        self.ctr = rng.choice([0, 1, 2, 0xFFFFFFFF, rng.getrandbits(32)])
        self.lr = rng.getrandbits(32) & ~3
        self.mem = mem
        # A field the model declines to predict is set to None.

    def so(self):
        return 1 if self.xer & SO else 0

    def ca(self):
        return 1 if self.xer & CA else 0

    def set_ca(self, c):
        self.xer = (self.xer & ~CA & M) | (CA if c else 0)

    def set_ov(self, o):
        self.xer = (self.xer | OV | SO) if o else (self.xer & ~OV & M)

    def crf(self, f):
        return (self.cr >> (4 * (7 - f))) & 15

    def set_crf(self, f, v):
        sh = 4 * (7 - f)
        self.cr = (self.cr & ~(15 << sh) & M) | ((v & 15) << sh)

    def crbit(self, i):
        return (self.cr >> (31 - i)) & 1

    def set_crbit(self, i, v):
        self.cr = (self.cr & ~(1 << (31 - i)) & M) | ((v & 1) << (31 - i))

    def compare(self, f, a, b):
        self.set_crf(f, (8 if a < b else 4 if a > b else 2) | self.so())

    def rc(self, res):
        self.compare(0, s32(res), 0)

    def line(self):
        f = [self.r[n] for n in REGS] + [self.cr, self.xer, self.ctr, self.lr]
        return [None if v is None else v & M for v in f]


# --------------------------------------------------------------------
# The harness, in either encoding
# --------------------------------------------------------------------

class Asm:
    def __init__(self, mode):
        self.v = mode == "v"
        self.out = []
        self.label = 0

    def emit(self, *lines):
        for l in lines:
            self.out.append("\t" + l)

    def new_label(self):
        self.label += 1
        return ".L%d" % self.label

    def li32(self, r, val):
        val &= M
        if self.v:
            self.emit("e_lis %d,%d" % (r, val >> 16), "e_or2i %d,%d" % (r, val & 0xFFFF))
        else:
            self.emit("lis %d,%d" % (r, sx(val >> 16, 16)),
                      "ori %d,%d,%d" % (r, r, val & 0xFFFF))

    def la(self, r, sym):
        if self.v:
            self.emit("e_lis %d,%s@h" % (r, sym), "e_or2i %d,%s@l" % (r, sym))
        else:
            self.emit("lis %d,%s@h" % (r, sym), "ori %d,%d,%s@l" % (r, r, sym))

    def li(self, r, val):
        self.emit(("e_li %d,%d" if self.v else "li %d,%d") % (r, val))

    def b(self, target):
        self.emit(("e_b %s" if self.v else "b %s") % target)

    def bl(self, target):
        self.emit(("e_bl %s" if self.v else "bl %s") % target)


def harness_head(a):
    v = a.v
    o = a.out
    o.append("\t.text\n\t.globl _start\n_start:")
    a.b("reset")
    # One slot per interrupt, sixteen bytes apart: which one, then report.
    for n in list(range(16)) + [32, 33, 34, 35]:
        o.append("\t.balign 16\nvec%d:" % n)
        a.li(3, n)
        a.b("trap")
    o.append("\t.balign 16\ntrap:")
    a.la(30, "0x10000000")
    for ch in "TRAP ":
        a.li(20, ord(ch))
        a.emit(("e_stb 20,0(30)" if v else "stb 20,0(30)"))
    a.emit("mr 21,3" if not v else "or 21,3,3")
    a.bl("puthex")
    a.emit("mfspr 21,26")
    a.bl("puthex")
    a.emit("mfspr 21,62")
    a.bl("puthex")
    a.li(20, 10)
    a.emit(("e_stb 20,0(30)" if v else "stb 20,0(30)"))
    a.li(3, 99)
    a.li(0, 93)
    a.emit("se_sc" if v else "sc")

    # r21 as eight hex digits and a space. No compare, no record form
    # and no CTR: the caller's CR, XER and CTR are what is being
    # printed, so this must not touch them.
    o.append("puthex:")
    for i in range(8):
        sh = 4 * (i + 1)
        if v:
            a.emit("e_rlwinm 20,21,%d,28,31" % (sh & 31), "e_add16i 19,20,6",
                   "e_srwi 19,19,4", "e_mulli 19,19,39", "add 20,20,19",
                   "e_add16i 20,20,48", "e_stb 20,0(30)")
        else:
            a.emit("rlwinm 20,21,%d,28,31" % (sh & 31), "addi 19,20,6",
                   "srwi 19,19,4", "mulli 19,19,39", "add 20,20,19",
                   "addi 20,20,48", "stb 20,0(30)")
    a.li(20, 32)
    a.emit("e_stb 20,0(30)" if v else "stb 20,0(30)")
    a.emit("se_blr" if v else "blr")

    # r3-r7, CR, XER, CTR and r10 (the case's LR), one line.
    o.append("dump:")
    a.emit("mfspr 25,8")
    for src in ["or 21,3,3", "or 21,4,4", "or 21,5,5", "or 21,6,6", "or 21,7,7",
                "mfcr 21", "mfspr 21,1", "mfspr 21,9", "or 21,10,10"]:
        a.emit(src)
        a.bl("puthex")
    a.li(20, 10)
    a.emit("e_stb 20,0(30)" if v else "stb 20,0(30)")
    a.emit("mtspr 8,25")
    a.emit("se_blr" if v else "blr")

    o.append("reset:")
    a.la(1, "stack_top")
    a.la(12, "_start")
    a.emit("mtspr 63,12")
    for n in range(16):
        a.li(12, 0)
        a.emit(("e_add16i 12,12,vec%d-_start" if v else "addi 12,12,vec%d-_start") % n,
               "mtspr %d,12" % (400 + n))
    for n in range(32, 36):
        a.li(12, 0)
        a.emit(("e_add16i 12,12,vec%d-_start" if v else "addi 12,12,vec%d-_start") % n,
               "mtspr %d,12" % (528 + n - 32))
    a.la(30, "0x10000000")
    a.la(11, "buf")


def harness_tail(a):
    a.li(3, 0)
    a.li(0, 93)
    a.emit("se_sc" if a.v else "sc")
    a.out.append("\t.balign 8\nbuf:\t.space %d\n\t.space 4096\nstack_top:" % (BUF + 16))


LDS = """ENTRY(_start)
SECTIONS { . = 0x80000000; .text : { *(.text) } /DISCARD/ : { *(.note*) *(.PPC*) } }
"""

# --------------------------------------------------------------------
# The cases. Each generator returns (assembly lines, model) or None;
# the model is a function of the state, or None where this file has no
# opinion and only the two backends are compared.
# --------------------------------------------------------------------


def R(rng):
    return rng.choice(REGS)


def pick(rng, xs):
    return xs[rng.randrange(len(xs))]


def dot(rc):
    return "." if rc else ""


def g_arith(rng, st, v):
    """The carrying adds and subtracts, with OE and Rc."""
    table = {
        # name: (complement a, b, carry in, writes CA, takes rB)
        "add": (0, "rb", 0, 0, 1), "addc": (0, "rb", 0, 1, 1),
        "adde": (0, "rb", "ca", 1, 1), "addme": (0, M, "ca", 1, 0),
        "addze": (0, 0, "ca", 1, 0), "subf": (1, "rb", 1, 0, 1),
        "subfc": (1, "rb", 1, 1, 1), "subfe": (1, "rb", "ca", 1, 1),
        "subfme": (1, M, "ca", 1, 0), "subfze": (1, 0, "ca", 1, 0),
        "neg": (1, 0, 1, 0, 0),
    }
    mn = pick(rng, sorted(table))
    inv, bk, ck, wca, has_b = table[mn]
    d, a, b = R(rng), R(rng), R(rng)
    oe, rc = rng.random() < 0.3, rng.random() < 0.4
    asm = "%s%s%s %d,%d" % (mn, "o" if oe else "", dot(rc), d, a)
    if has_b:
        asm += ",%d" % b

    def model(s):
        x = (~s.r[a] & M) if inv else s.r[a]
        y = s.r[b] if bk == "rb" else bk
        c = s.ca() if ck == "ca" else ck
        t = x + y + c
        res = t & M
        if wca:
            s.set_ca(t >> 32)
        if oe:
            s.set_ov(((x ^ res) & (y ^ res)) >> 31)
        s.r[d] = res
        if rc:
            s.rc(res)

    return [asm], model


def g_muldiv(rng, st, v):
    mn = pick(rng, ["mullw", "mulhw", "mulhwu", "divw", "divwu"])
    d, a, b = R(rng), R(rng), R(rng)
    oe = mn in ("mullw", "divw", "divwu") and rng.random() < 0.3
    rc = rng.random() < 0.4
    asm = "%s%s%s %d,%d,%d" % (mn, "o" if oe else "", dot(rc), d, a, b)
    # The two divisors with no quotient, which no random pair lands on.
    edge = rng.random()
    if edge < 0.12:
        st.r[b] = 0
    elif edge < 0.24 and a != b:
        st.r[a], st.r[b] = 0x80000000, 0xFFFFFFFF

    def model(s):
        x, y = s.r[a], s.r[b]
        ov = False
        if mn == "mullw":
            p = s32(x) * s32(y)
            res = p & M
            ov = p != s32(res)
        elif mn == "mulhw":
            res = ((s32(x) * s32(y)) >> 32) & M
        elif mn == "mulhwu":
            res = (x * y) >> 32
        elif mn == "divwu":
            ov = y == 0
            res = 0 if ov else x // y
        else:
            ov = y == 0 or (x == 0x80000000 and y == M)
            if ov:
                res = 0  # undefined by the architecture; this model's choice too
            else:
                q = abs(s32(x)) // abs(s32(y))
                res = (-q if (s32(x) < 0) != (s32(y) < 0) else q) & M
        if oe:
            s.set_ov(ov)
        s.r[d] = res
        if rc:
            s.rc(res)

    return [asm], model


def si16(rng):
    return pick(rng, [0, 1, -1, 32767, -32768, 255, -256, rng.randrange(-32768, 32768)])


def ui16(rng):
    return pick(rng, [0, 1, 0xFFFF, 0x8000, 0x7FFF, 0xFF, rng.randrange(65536)])


def sci8(rng):
    """An SCI8 immediate: a byte at one of four positions, the rest 0 or 1s."""
    f, scl, ui = rng.randrange(2), rng.randrange(4), rng.randrange(256)
    return ((M ^ (0xFF << (8 * scl))) if f else 0) | (ui << (8 * scl))


def g_imm(rng, st, v):
    d, a = R(rng), R(rng)
    if not v:
        mn = pick(rng, ["addi", "addis", "addic", "addic.", "subfic", "mulli"])
        k = si16(rng)
        asm = "%s %d,%d,%d" % (mn, d, a, k)
        kk = (k << 16) & M if mn == "addis" else k & M
    else:
        mn = pick(rng, ["e_add16i", "e_addi", "e_addi.", "e_addic", "e_addic.",
                        "e_subfic", "e_subfic.", "e_mulli", "e_add2i.", "e_add2is",
                        "e_mull2i", "se_addi", "se_subi", "se_subi."])
        if mn == "e_add16i":
            k = si16(rng)
            kk = k & M
            asm = "%s %d,%d,%d" % (mn, d, a, k)
        elif mn in ("e_add2i.", "e_add2is", "e_mull2i"):
            k = si16(rng)
            kk = (k << 16) & M if mn == "e_add2is" else k & M
            d = a
            asm = "%s %d,%d" % (mn, a, k)
        elif mn.startswith("se_"):
            k = rng.randrange(1, 33)
            kk = k if mn == "se_addi" else (-k) & M
            d = a
            asm = "%s %d,%d" % (mn, a, k)
        else:
            kk = sci8(rng)
            asm = "%s %d,%d,%d" % (mn, d, a, s32(kk))
    base = mn.rstrip(".")
    rc = mn.endswith(".")
    # The register equal to the immediate, and to its complement: where
    # a carry out of a subtract or an add changes.
    edge = rng.random()
    if edge < 0.15:
        st.r[a] = kk
    elif edge < 0.25:
        st.r[a] = (~kk) & M
    elif edge < 0.32:
        st.r[a] = (-kk) & M

    def model(s):
        x = s.r[a]
        if base in ("addi", "addis", "e_add16i", "e_addi", "e_add2i", "e_add2is",
                    "se_addi", "se_subi"):
            res = (x + kk) & M
        elif base in ("addic", "e_addic"):
            res = (x + kk) & M
            s.set_ca((x + kk) >> 32)
        elif base in ("subfic", "e_subfic"):
            t = (~x & M) + kk + 1
            res = t & M
            s.set_ca(t >> 32)
        else:
            res = (s32(x) * s32(kk)) & M
        s.r[d] = res
        if rc:
            s.rc(res)

    return [asm], model


def g_logic(rng, st, v):
    ops = {"and": lambda x, y: x & y, "andc": lambda x, y: x & ~y,
           "or": lambda x, y: x | y, "orc": lambda x, y: x | ~y,
           "xor": lambda x, y: x ^ y, "nand": lambda x, y: ~(x & y),
           "nor": lambda x, y: ~(x | y), "eqv": lambda x, y: ~(x ^ y)}
    mn = pick(rng, sorted(ops))
    a, s_, b = R(rng), R(rng), R(rng)
    rc = rng.random() < 0.4
    asm = "%s%s %d,%d,%d" % (mn, dot(rc), a, s_, b)

    def model(s):
        res = ops[mn](s.r[s_], s.r[b]) & M
        s.r[a] = res
        if rc:
            s.rc(res)

    return [asm], model


def g_logic_imm(rng, st, v):
    a, s_ = R(rng), R(rng)
    if not v:
        mn = pick(rng, ["andi.", "andis.", "ori", "oris", "xori", "xoris"])
        k = ui16(rng)
        kk = (k << 16) if mn.rstrip(".").endswith("s") else k
        asm = "%s %d,%d,%d" % (mn, a, s_, k)
    else:
        mn = pick(rng, ["e_andi", "e_andi.", "e_ori", "e_ori.", "e_xori", "e_xori.",
                        "e_or2i", "e_and2i.", "e_or2is", "e_and2is.", "se_andi",
                        "se_bclri", "se_bseti"])
        if mn in ("e_or2i", "e_and2i.", "e_or2is", "e_and2is."):
            k = ui16(rng)
            kk = (k << 16) if "is" in mn else k
            s_ = a
            asm = "%s %d,%d" % (mn, a, k)
        elif mn.startswith("se_"):
            k = rng.randrange(32)
            kk = k if mn == "se_andi" else (0x80000000 >> k)
            if mn == "se_bclri":
                kk = ~kk & M
            s_ = a
            asm = "%s %d,%d" % (mn, a, k)
        else:
            kk = sci8(rng)
            asm = "%s %d,%d,%d" % (mn, a, s_, kk)
    base = mn.rstrip(".")
    rc = mn.endswith(".")
    kind = ("and" if "and" in base or base == "se_bclri" else
            "xor" if "xor" in base else "or")

    def model(s):
        x = s.r[s_]
        res = (x & kk) if kind == "and" else (x ^ kk) if kind == "xor" else (x | kk)
        res &= M
        s.r[a] = res
        if rc:
            s.rc(res)

    return [asm], model


def g_unary(rng, st, v):
    a, s_ = R(rng), R(rng)
    if v and rng.random() < 0.5:
        mn = pick(rng, ["se_extsb", "se_extsh", "se_extzb", "se_extzh", "se_not",
                        "se_neg", "se_mr", "se_li", "se_bmaski", "se_bgeni", "e_li",
                        "e_lis"])
        rc = False
        if mn == "se_mr":
            asm = "%s %d,%d" % (mn, a, s_)
        elif mn == "se_li":
            k = rng.randrange(128)
            asm = "%s %d,%d" % (mn, a, k)
        elif mn in ("se_bmaski", "se_bgeni"):
            k = rng.randrange(32)
            asm = "%s %d,%d" % (mn, a, k)
        elif mn == "e_li":
            k = pick(rng, [0, -1, 524287, -524288, rng.randrange(-524288, 524288)])
            asm = "%s %d,%d" % (mn, a, k)
        elif mn == "e_lis":
            k = ui16(rng)
            asm = "%s %d,%d" % (mn, a, k)
        else:
            s_ = a
            asm = "%s %d" % (mn, a)
    else:
        mn = pick(rng, ["extsb", "extsh", "cntlzw"])
        rc = rng.random() < 0.4
        k = 0
        asm = "%s%s %d,%d" % (mn, dot(rc), a, s_)

    def model(s):
        x = s.r[s_]
        if mn in ("extsb", "se_extsb"):
            res = sx(x, 8)
        elif mn in ("extsh", "se_extsh"):
            res = sx(x, 16)
        elif mn == "se_extzb":
            res = x & 0xFF
        elif mn == "se_extzh":
            res = x & 0xFFFF
        elif mn == "cntlzw":
            res = 32 - x.bit_length()
        elif mn == "se_not":
            res = ~x
        elif mn == "se_neg":
            res = -x
        elif mn == "se_mr":
            res = x
        elif mn in ("se_li", "e_li"):
            res = k
        elif mn == "se_bmaski":
            res = M if k == 0 else (1 << k) - 1
        elif mn == "se_bgeni":
            res = 0x80000000 >> k
        else:
            res = k << 16
        res &= M
        s.r[a] = res
        if rc:
            s.rc(res)

    return [asm], model


def g_shift(rng, st, v):
    a, s_, b = R(rng), R(rng), R(rng)
    sh, mb, me = rng.randrange(32), rng.randrange(32), rng.randrange(32)
    rc = rng.random() < 0.4
    names = ["slw", "srw", "sraw", "srawi"]
    if v:
        names += ["e_rlwinm", "e_rlwimi", "e_rlw", "e_rlwi", "e_slwi", "e_srwi",
                  "se_slw", "se_srw", "se_sraw", "se_slwi", "se_srwi", "se_srawi"]
    else:
        names += ["rlwinm", "rlwimi", "rlwnm"]  # rlwnm has no VLE spelling but e_rlw
    mn = pick(rng, names)
    if mn in ("e_rlwinm", "e_rlwimi") or mn.startswith("se_"):
        rc = False
    if mn.startswith("se_"):
        s_ = a
    base = mn.replace("se_", "").replace("e_", "")
    if base in ("slw", "srw", "sraw"):
        asm = "%s%s %d,%d,%d" % (mn, dot(rc), a, s_, b) if not mn.startswith("se_") \
            else "%s %d,%d" % (mn, a, b)
    elif base in ("srawi", "slwi", "srwi", "rlwi"):
        asm = "%s%s %d,%d,%d" % (mn, dot(rc), a, s_, sh) if not mn.startswith("se_") \
            else "%s %d,%d" % (mn, a, sh)
    elif base == "rlw":
        asm = "%s%s %d,%d,%d" % (mn, dot(rc), a, s_, b)
    elif base == "rlwnm":
        asm = "%s%s %d,%d,%d,%d,%d" % (mn, dot(rc), a, s_, b, mb, me)
    else:
        asm = "%s%s %d,%d,%d,%d,%d" % (mn, dot(rc), a, s_, sh, mb, me)

    def model(s):
        x = s.r[s_]
        if base in ("slw", "srw"):
            n = s.r[b] & 63
            res = 0 if n >= 32 else ((x << n) & M if base == "slw" else x >> n)
        elif base in ("sraw", "srawi"):
            n = (s.r[b] & 63) if base == "sraw" else sh
            res = (s32(x) >> min(n, 31)) & M if n < 32 else (M if s32(x) < 0 else 0)
            lost = x & ((1 << min(n, 32)) - 1)
            s.set_ca(s32(x) < 0 and lost != 0)
        elif base == "slwi":
            res = (x << sh) & M
        elif base == "srwi":
            res = x >> sh
        elif base == "rlwi":
            res = rotl(x, sh)
        elif base == "rlw":
            res = rotl(x, s.r[b] & 31)
        elif base == "rlwnm":
            res = rotl(x, s.r[b] & 31) & mask(mb, me)
        elif base == "rlwinm":
            res = rotl(x, sh) & mask(mb, me)
        else:
            m_ = mask(mb, me)
            res = (rotl(x, sh) & m_) | (s.r[a] & ~m_ & M)
        s.r[a] = res & M
        if rc:
            s.rc(res)

    return [asm], model


def g_cmp(rng, st, v):
    a, b = R(rng), R(rng)
    f = rng.randrange(8)
    names = ["cmpw", "cmplw"]
    if v:
        names += ["se_cmp", "se_cmpl", "se_cmph", "se_cmphl", "se_cmpi", "se_cmpli",
                  "e_cmpi", "e_cmpli", "e_cmp16i", "e_cmpl16i", "e_cmph16i",
                  "e_cmphl16i", "e_cmph", "e_cmphl", "se_btsti"]
    else:
        names += ["cmpwi", "cmplwi"]
    mn = pick(rng, names)
    k = None
    if mn in ("cmpw", "cmplw", "e_cmph", "e_cmphl"):
        asm = "%s cr%d,%d,%d" % (mn, f, a, b)
    elif mn in ("cmpwi",):
        k = si16(rng)
        asm = "%s cr%d,%d,%d" % (mn, f, a, k)
    elif mn in ("cmplwi",):
        k = ui16(rng)
        asm = "%s cr%d,%d,%d" % (mn, f, a, k)
    elif mn in ("se_cmp", "se_cmpl", "se_cmph", "se_cmphl"):
        f = 0
        asm = "%s %d,%d" % (mn, a, b)
    elif mn == "se_cmpi":
        f, k = 0, rng.randrange(32)
        asm = "%s %d,%d" % (mn, a, k)
    elif mn == "se_cmpli":
        f, k = 0, rng.randrange(1, 33)
        asm = "%s %d,%d" % (mn, a, k)
    elif mn == "se_btsti":
        f, k = 0, rng.randrange(32)
        asm = "%s %d,%d" % (mn, a, k)
    elif mn in ("e_cmpi", "e_cmpli"):
        f = rng.randrange(4)
        k = sci8(rng)
        asm = "%s cr%d,%d,%d" % (mn, f, a, s32(k) if mn == "e_cmpi" else k)
    elif mn in ("e_cmp16i", "e_cmph16i"):
        f, k = 0, si16(rng)
        asm = "%s %d,%d" % (mn, a, k)
    else:
        f, k = 0, ui16(rng)
        asm = "%s %d,%d" % (mn, a, k)
    # Equal operands, and one either side: EQ is one value in 2^32.
    if rng.random() < 0.3:
        eq = (st.r[b] if k is None else k) & M
        st.r[a] = (eq + pick(rng, [0, 0, 1, -1])) & M

    def model(s):
        x = s.r[a]
        y = s.r[b] if k is None else k
        if mn == "se_btsti":
            s.set_crf(0, (4 if x & (0x80000000 >> k) else 2) | s.so())
        elif mn in ("cmpw", "cmpwi", "se_cmp", "se_cmpi", "e_cmpi", "e_cmp16i"):
            s.compare(f, s32(x), s32(y))
        elif mn in ("cmplw", "cmplwi", "se_cmpl", "se_cmpli", "e_cmpli", "e_cmpl16i"):
            s.compare(f, x, y & M)
        elif mn in ("se_cmph", "e_cmph"):
            s.compare(f, sx(x, 16), sx(y, 16))
        elif mn == "e_cmph16i":
            s.compare(f, sx(x, 16), s32(y))
        elif mn in ("se_cmphl", "e_cmphl"):
            s.compare(f, x & 0xFFFF, y & 0xFFFF)
        else:  # e_cmphl16i
            s.compare(f, x & 0xFFFF, y & M)

    return [asm], model


def g_cr(rng, st, v):
    ops = {"crand": lambda x, y: x & y, "crandc": lambda x, y: x & (y ^ 1),
           "creqv": lambda x, y: (x ^ y) ^ 1, "crnand": lambda x, y: (x & y) ^ 1,
           "crnor": lambda x, y: (x | y) ^ 1, "cror": lambda x, y: x | y,
           "crorc": lambda x, y: x | (y ^ 1), "crxor": lambda x, y: x ^ y}
    kind = pick(rng, ["logic", "logic", "mcrf", "mcrxr", "mfcr", "mtcrf", "isel"])
    d, a, b = R(rng), R(rng), R(rng)
    if kind == "logic":
        mn = pick(rng, sorted(ops))
        bt, ba, bb = rng.randrange(32), rng.randrange(32), rng.randrange(32)
        if rng.random() < 0.2:
            bb = ba
        asm = "%s%s %d,%d,%d" % ("e_" if v else "", mn, bt, ba, bb)

        def model(s):
            s.set_crbit(bt, ops[mn](s.crbit(ba), s.crbit(bb)))
    elif kind == "mcrf":
        fd, fs = rng.randrange(8), rng.randrange(8)
        asm = "%smcrf cr%d,cr%d" % ("e_" if v else "", fd, fs)

        def model(s):
            s.set_crf(fd, s.crf(fs))
    elif kind == "mcrxr":
        fd = rng.randrange(8)
        asm = "mcrxr cr%d" % fd

        def model(s):
            s.set_crf(fd, s.xer >> 28)
            s.xer &= 0x0FFFFFFF
    elif kind == "mfcr":
        asm = "mfcr %d" % d

        def model(s):
            s.r[d] = s.cr
    elif kind == "mtcrf":
        fxm = pick(rng, [0xFF, 0x80, 0x01, rng.randrange(256)])
        asm = "mtcrf %d,%d" % (fxm, d)

        def model(s):
            m_ = 0
            for i in range(8):
                if fxm & (0x80 >> i):
                    m_ |= 15 << (4 * (7 - i))
            s.cr = (s.cr & ~m_ & M) | (s.r[d] & m_)
    else:
        bc = rng.randrange(32)
        asm = "isel %d,%d,%d,%d" % (d, a, b, bc)

        def model(s):
            s.r[d] = s.r[a] if s.crbit(bc) else s.r[b]
    return [asm], model


def g_spr(rng, st, v):
    d = R(rng)
    mn = pick(rng, ["mfxer", "mtxer", "mfctr", "mtctr", "mflr", "mtlr"] +
              (["se_mflr", "se_mtlr", "se_mfctr", "se_mtctr"] if v else []))
    asm = "%s %d" % (mn, d)
    base = mn.replace("se_", "")

    def model(s):
        if base == "mfxer":
            s.r[d] = s.xer
        elif base == "mtxer":
            s.xer = s.r[d] & 0xE000007F
        elif base == "mfctr":
            s.r[d] = s.ctr
        elif base == "mtctr":
            s.ctr = s.r[d]
        elif base == "mflr":
            s.r[d] = s.lr
        else:
            s.lr = s.r[d]

    return [asm], model


def bo_taken(s, bo, bi):
    """Book E's BO, and the CTR decrement it may ask for."""
    ok = True
    if not bo & 4:
        s.ctr = (s.ctr - 1) & M
        ok = (s.ctr == 0) if bo & 2 else (s.ctr != 0)
    if not bo & 16:
        ok = ok and (s.crbit(bi) == ((bo >> 3) & 1))
    return ok


def g_branch(rng, st, v, asm_):
    """A branch, observed by which of two moves it lets run."""
    t, e = asm_.new_label(), asm_.new_label()
    kinds = ["bc", "bclr", "bcctr"]
    kind = pick(rng, kinds)
    lk = rng.random() < 0.3
    # Every BO a branch may have, with its hint bit either way -- except
    # "always", whose low bits are not a hint and must be zero for the
    # assembler to take it.
    bo = pick(rng, [0, 2, 4, 8, 10, 12, 16, 18, 20])
    if kind == "bcctr":
        # Not the decrementing forms: 3.16.3 branches to the count as it
        # was, which here is an address this file does not know.
        bo = pick(rng, [4, 12, 20])
    if bo != 20:
        bo |= rng.randrange(2)
    bi = rng.randrange(32)
    lines = []
    if v and kind == "bc":
        which = pick(rng, ["e_bc", "se_bc"])
        if which == "e_bc":
            bo32, bi = rng.randrange(4), rng.randrange(16)
            bo = [4, 12, 16, 18][bo32]
            lines.append("e_bc%s %d,%d,%s" % ("l" if lk else "", bo32, bi, t))
        else:
            bo16, bi = rng.randrange(2), rng.randrange(4)
            bo = 12 if bo16 else 4
            lk = False
            lines.append("se_bc %d,%d,%s" % (bo16, bi, t))
    elif kind == "bc":
        lines.append("bc%s %d,%d,%s" % ("l" if lk else "", bo, bi, t))
    else:
        reg_spr = 8 if kind == "bclr" else 9
        if v:
            # VLE has only the unconditional forms of these.
            bo, bi = 20, 0
            lines += ["e_lis 12,%s@h" % t, "e_or2i 12,%s@l" % t,
                      "mtspr %d,12" % reg_spr,
                      "se_b%s%s" % ("lr" if kind == "bclr" else "ctr", "l" if lk else "")]
        else:
            lines += ["lis 12,%s@h" % t, "ori 12,12,%s@l" % t,
                      "mtspr %d,12" % reg_spr,
                      "%s%s %d,%d" % (kind, "l" if lk else "", bo, bi)]
    if v:
        lines += ["se_li 3,1", "se_b %s" % e, "%s:" % t, "se_li 3,2", "%s:" % e]
    else:
        lines += ["li 3,1", "b %s" % e, "%s:" % t, "li 3,2", "%s:" % e]

    def model(s):
        if kind == "bcctr":
            s.ctr = None  # holds the label's address, which this does not know
            taken = (bo & 16) or (s.crbit(bi) == ((bo >> 3) & 1))
        else:
            taken = bo_taken(s, bo, bi)
        s.r[3] = 2 if taken else 1
        if lk or kind == "bclr":
            s.lr = None  # an address

    return lines, model


def g_mem(rng, st, v):
    """A store and a load of different shapes over the same bytes."""
    s_, d = R(rng), R(rng)
    off = rng.randrange(0, BUF - 8)
    k = rng.randrange(4)
    kind = pick(rng, ["w-b", "w-h", "w-ha", "h-b", "b-b", "w-w", "wbr-w", "w-wbr",
                      "hbr-h", "w-hbr", "wx", "upd", "updx"])
    lines = []
    e = "e_" if v else ""

    def bytes_of(val, n):
        return [(val >> (8 * (n - 1 - i))) & 0xFF for i in range(n)]

    def word_at(mem, at, n):
        v_ = 0
        for i in range(n):
            v_ = (v_ << 8) | mem[at + i]
        return v_

    if kind in ("w-b", "w-h", "w-ha", "w-w"):
        lines.append("%sstw %d,%d(11)" % (e, s_, off))
        ld, n, at = {"w-b": ("lbz", 1, off + k), "w-h": ("lhz", 2, off + k % 3),
                     "w-ha": ("lha", 2, off + k % 3), "w-w": ("lwz", 4, off)}[kind]
        lines.append("%s%s %d,%d(11)" % (e, ld, d, at))

        def model(s):
            s.mem[off:off + 4] = bytes(bytes_of(s.r[s_], 4))
            val = word_at(s.mem, at, n)
            s.r[d] = sx(val, 16) & M if ld == "lha" else val
    elif kind == "h-b":
        lines += ["%ssth %d,%d(11)" % (e, s_, off), "%slbz %d,%d(11)" % (e, d, off + k % 2)]

        def model(s):
            s.mem[off:off + 2] = bytes(bytes_of(s.r[s_] & 0xFFFF, 2))
            s.r[d] = s.mem[off + k % 2]
    elif kind == "b-b":
        lines += ["%sstb %d,%d(11)" % (e, s_, off), "%slhz %d,%d(11)" % (e, d, off)]

        def model(s):
            s.mem[off] = s.r[s_] & 0xFF
            s.r[d] = word_at(s.mem, off, 2)
    elif kind in ("wbr-w", "w-wbr", "hbr-h", "w-hbr"):
        # The byte-reversed forms are indexed only: the offset in r12.
        lines.append("%s 12,%d" % ("e_li" if v else "li", off))
        st_, ld = {"wbr-w": ("stwbrx", "lwzx"), "w-wbr": ("stwx", "lwbrx"),
                   "hbr-h": ("sthbrx", "lhzx"), "w-hbr": ("stwx", "lhbrx")}[kind]
        lines += ["%s %d,11,12" % (st_, s_), "%s %d,11,12" % (ld, d)]

        def model(s):
            x = s.r[s_]
            if st_ == "stwbrx":
                s.mem[off:off + 4] = bytes(reversed(bytes_of(x, 4)))
            elif st_ == "sthbrx":
                s.mem[off:off + 2] = bytes(reversed(bytes_of(x & 0xFFFF, 2)))
            else:
                s.mem[off:off + 4] = bytes(bytes_of(x, 4))
            if ld == "lwzx":
                s.r[d] = word_at(s.mem, off, 4)
            elif ld == "lhzx":
                s.r[d] = word_at(s.mem, off, 2)
            elif ld == "lwbrx":
                s.r[d] = int.from_bytes(bytes(s.mem[off:off + 4]), "little")
            else:
                s.r[d] = int.from_bytes(bytes(s.mem[off:off + 2]), "little")
    elif kind == "wx":
        st_, ld = pick(rng, [("stwx", "lwzx"), ("stbx", "lbzx"), ("sthx", "lhax"),
                             ("sthx", "lhzx")])
        lines += ["%s 12,%d" % ("e_li" if v else "li", off),
                  "%s %d,11,12" % (st_, s_), "%s %d,11,12" % (ld, d)]
        n = {"stwx": 4, "stbx": 1, "sthx": 2}[st_]

        def model(s):
            s.mem[off:off + n] = bytes(bytes_of(s.r[s_] & ((1 << (8 * n)) - 1), n))
            val = word_at(s.mem, off, n)
            s.r[d] = sx(val, 16) & M if ld == "lhax" else val
    elif kind == "upd":
        # The update forms: the base moves, which the subtract shows.
        st_, ld, n = pick(rng, [("stwu", "lwz", 4), ("stbu", "lbz", 1),
                                ("sthu", "lhz", 2)])
        o8 = rng.randrange(0, 60)
        d2 = pick(rng, [x for x in REGS if x != d])
        lines += ["or 12,11,11", "%s%s %d,%d(12)" % (e, st_, s_, o8),
                  "%s%s %d,0(12)" % (e, ld, d), "subf %d,11,12" % d2]

        def model(s):
            s.mem[o8:o8 + n] = bytes(bytes_of(s.r[s_] & ((1 << (8 * n)) - 1), n))
            s.r[d] = word_at(s.mem, o8, n)
            s.r[d2] = o8
    else:
        ld, n = pick(rng, [("lwzu", 4), ("lbzu", 1), ("lhzu", 2), ("lhau", 2)])
        o8 = rng.randrange(0, 60)
        d2 = pick(rng, [x for x in REGS if x != d])
        lines += ["or 12,11,11", "%s%s %d,%d(12)" % (e, ld, d, o8),
                  "subf %d,11,12" % d2]

        def model(s):
            val = word_at(s.mem, o8, n)
            s.r[d] = sx(val, 16) & M if ld == "lhau" else val
            s.r[d2] = o8
    return lines, model


FP_SPECIAL = [0x00000000, 0x80000000, 0x3F800000, 0xBF800000, 0x7F800000,
              0xFF800000, 0x7FC00000, 0x00000001, 0x007FFFFF, 0x00800000,
              0x7F7FFFFF, 0xFF7FFFFF, 0x3F000000, 0x40490FDB, 0x4B800000,
              0xCF000000, 0x4F000000, 0x33800000]


def g_efp(rng, st, v):
    """The embedded FP unit. No model: the interpreter is the reference,
    and what is being checked is that a block calling it from inside
    leaves the same state. Its own semantics are tests/unit's."""
    d, a, b = R(rng), R(rng), R(rng)
    for r in (a, b, d):
        if rng.random() < 0.5:
            st.r[r] = pick(rng, FP_SPECIAL)
    three = ["efsadd", "efssub", "efsmul", "efsdiv", "efsmadd", "efsmsub",
             "efsnmadd", "efsnmsub", "efsmax", "efsmin"]
    two = ["efsabs", "efsnabs", "efsneg", "efssqrt", "efscfui", "efscfsi",
           "efscfuf", "efscfsf", "efsctui", "efsctsi", "efsctuf", "efsctsf",
           "efsctuiz", "efsctsiz"]
    cmp_ = ["efscmpgt", "efscmplt", "efscmpeq", "efststgt", "efststlt", "efststeq"]
    mn = pick(rng, three + two + cmp_)
    if mn in three:
        asm = "%s %d,%d,%d" % (mn, d, a, b)
    elif mn in two:
        asm = "%s %d,%d" % (mn, d, a)
    else:
        asm = "%s cr%d,%d,%d" % (mn, rng.randrange(8), a, b)
    return [asm], None


CLASSES = {
    "arith": g_arith, "muldiv": g_muldiv, "imm": g_imm, "logic": g_logic,
    "logic_imm": g_logic_imm, "unary": g_unary, "shift": g_shift, "cmp": g_cmp,
    "cr": g_cr, "spr": g_spr, "branch": g_branch, "mem": g_mem, "efp": g_efp,
}
WEIGHT = {"arith": 5, "shift": 5, "mem": 4, "branch": 4, "imm": 3, "cmp": 3,
          "cr": 3, "muldiv": 2, "logic": 2, "logic_imm": 2, "unary": 2, "spr": 1,
          "efp": 2}


def build(mode, seed, count, only):
    rng = random.Random(seed)
    a = Asm(mode)
    harness_head(a)
    names = [n for n in sorted(CLASSES) if not only or n in only]
    bag = [n for n in names for _ in range(WEIGHT[n])]
    mem = bytearray(BUF + 16)
    want = []
    info = []
    for i in range(count):
        cls = pick(rng, bag)
        st = State(rng, mem)
        if cls == "branch":
            lines, model = g_branch(rng, st, a.v, a)
        else:
            lines, model = CLASSES[cls](rng, st, a.v)
        # The state the generator settled on, after any special values
        # it substituted.
        for n in REGS:
            a.li32(n, st.r[n])
        a.li32(12, st.cr)
        a.emit("mtcrf 255,12")
        a.li32(12, st.xer)
        a.emit("mtspr 1,12")
        a.li32(12, st.ctr)
        a.emit("mtspr 9,12")
        a.li32(12, st.lr)
        a.emit("mtspr 8,12")
        for l in lines:
            if l.endswith(":"):
                a.out.append(l)
            else:
                a.emit(l)
        a.emit("mfspr 10,8")
        a.bl("dump")
        if model is not None:
            model(st)
            want.append(st.line())
        else:
            want.append(None)
        info.append("%s: %s" % (cls, "; ".join(lines)))
    harness_tail(a)
    return "\n".join(a.out) + "\n", want, info


def run_emu(emu, elf, jit, cap):
    cmd = [emu] + (["--jit"] if jit else []) + ["--max-insn", str(cap), elf]
    r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                       universal_newlines=True)
    lines = [l for l in r.stdout.splitlines()
             if re.match(r"^([0-9a-f]{8} ){9}$", l)]
    return lines, r.stdout


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--emu", required=True)
    ap.add_argument("--mode", default="b,v")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--count", type=int, default=2000)
    ap.add_argument("--out", default="/tmp/ppc-cases")
    ap.add_argument("--only", default="")
    ap.add_argument("-v", "--verbose", type=int, default=8)
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    only = [x for x in a.only.split(",") if x]
    failed = False
    for mode in a.mode.split(","):
        src, want, info = build(mode, a.seed, a.count, only)
        base = os.path.join(a.out, "cases-%s-%d" % (mode, a.seed))
        open(base + ".s", "w").write(src)
        open(base + ".ld", "w").write(LDS)
        asf = ["-a32", "-mbig"] + (["-mvle"] if mode == "v" else ["-me500", "-many"])
        r = subprocess.run([AS] + asf + ["-o", base + ".o", base + ".s"],
                           stderr=subprocess.PIPE, universal_newlines=True)
        if r.returncode != 0:
            print("mode %s: the assembler rejected the cases:" % mode)
            for l in r.stderr.splitlines()[:a.verbose]:
                m = re.match(r".*:(\d+): (.*)", l)
                if m:
                    print("    %s    <- %s" % (
                        m.group(2), src.splitlines()[int(m.group(1)) - 1].strip()))
            failed = True
            continue
        subprocess.run([LD, "-m", "elf32ppc", "--build-id=none", "-T", base + ".ld",
                        "-o", base + ".elf", base + ".o"], check=True)
        cap = 2000 * a.count + 100000
        interp, raw_i = run_emu(a.emu, base + ".elf", False, cap)
        jit, raw_j = run_emu(a.emu, base + ".elf", True, cap)
        entries = re.search(r"blk entr\s+(\d+)", raw_j)
        bad_model = bad_jit = modelled = 0
        shown = 0
        for i in range(a.count):
            li = interp[i] if i < len(interp) else None
            lj = jit[i] if i < len(jit) else None
            if li is None or lj is None:
                break
            if li != lj:
                bad_jit += 1
                if shown < a.verbose:
                    shown += 1
                    print("  case %d  %s\n    interp %s\n    jit    %s" % (
                        i, info[i], li, lj))
            if want[i] is not None:
                modelled += 1
                got = [int(x, 16) for x in li.split()]
                if any(w is not None and w != g for w, g in zip(want[i], got)):
                    bad_model += 1
                    if shown < a.verbose:
                        shown += 1
                        print("  case %d  %s\n    interp %s\n    model  %s" % (
                            i, info[i], li, " ".join(
                                "--------" if w is None else "%08x" % w
                                for w in want[i])))
        short = len(interp) != a.count or len(jit) != a.count
        ok = not (bad_model or bad_jit or short or not entries or
                  int(entries.group(1)) == 0)
        print("mode %s seed %d: %d cases, %d modelled; interp vs model %d wrong, "
              "jit vs interp %d wrong; %s block entries%s" % (
                  mode, a.seed, a.count, modelled, bad_model, bad_jit,
                  entries.group(1) if entries else "no",
                  "" if not short else
                  "; RAN SHORT: interp %d lines, jit %d" % (len(interp), len(jit))))
        if short:
            for raw in (raw_i, raw_j):
                t = [l for l in raw.splitlines() if "TRAP" in l or "emu:" in l]
                for l in t[:4]:
                    print("    " + l)
        failed |= not ok
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
