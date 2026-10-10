#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""gen.py - write ARMv7E-M test cases as assembly, for the board diff.

Every case is one function: load r0-r12 and APSR from a state block, run
the instruction under test, store everything back. The same cases.S is
linked into an emulator guest and into native Nucleo-F746ZG firmware,
and the two outputs are compared -- so **the expected value of every case
is whatever a real Cortex-M7 does**, and nothing here computes one.

Instructions are written as assembly text, not as encodings: GNU as is
the encoder, which keeps this file from being a second, possibly wrong,
description of the encoding space. What this file does have to get
right is *which operands are legal*: an UNPREDICTABLE combination
(writeback with Rt == Rn, RdLo == RdHi, SP or PC where the encoding
forbids them) may legitimately differ between the board and the
emulator, and a mismatch there would be noise. Operands are r0-r12 only,
and each template applies the ARM ARM's constraints for its encoding.

    gen.py --seed N --count N --classes a,b,c --out DIR
"""

import argparse
import random
import sys

R = list(range(13))  # r0-r12
LOW = list(range(8))
CONDS = ["eq", "ne", "cs", "cc", "mi", "pl", "vs", "vc",
         "hi", "ls", "ge", "lt", "gt", "le"]
INV = {"eq": "ne", "ne": "eq", "cs": "cc", "cc": "cs", "mi": "pl",
       "pl": "mi", "vs": "vc", "vc": "vs", "hi": "ls", "ls": "hi",
       "ge": "lt", "lt": "ge", "gt": "le", "le": "gt"}

MEM_BASE = 8192  # diff_mem offset a base register points at


class Case:
    def __init__(self, lines, text=None, addr=0, mem=(0, 0), fp=False,
                 setup=None):
        self.setup = setup or []
        self.lines = lines
        self.text = text or "; ".join(lines)
        self.addr = addr
        self.mem = mem
        self.fp = fp
        # False when a register's address-ness depends on whether the
        # instruction ran -- an LDM loading over its own base. Put under
        # IT, a failed condition leaves an address where the harness was
        # told to expect a value, and the two memory maps then differ.
        self.can_cond = True
        # A raw case: its single line is an encoding, the harness resumes
        # after it if the core refuses it, and the fault is part of the
        # result. See rawgen.py.
        self.raw = False


def regs(rng, n, pool=R):
    return rng.sample(pool, n)


def r(n):
    return "r%d" % n


def mod_imm(rng):
    """A value ThumbExpandImm can produce, in each of its five shapes."""
    k = rng.randrange(5)
    b = rng.randrange(256)
    if k == 0:
        return b
    if k == 1:
        return b | (b << 16)
    if k == 2:
        return (b << 8) | (b << 24)
    if k == 3:
        return b * 0x01010101
    rot = rng.randrange(8, 32)
    v = 0x80 | rng.randrange(128)
    return ((v >> rot) | (v << (32 - rot))) & 0xFFFFFFFF


def shift(rng):
    t = rng.randrange(5)
    if t == 0:
        return ", lsl #%d" % rng.randrange(0, 32)
    if t == 1:
        return ", lsr #%d" % rng.randrange(1, 33)
    if t == 2:
        return ", asr #%d" % rng.randrange(1, 33)
    if t == 3:
        return ", ror #%d" % rng.randrange(1, 32)
    return ", rrx"


def set_small(rng, reg, hi=40):
    """Make a register a small known value -- a shift amount, an index."""
    choices = list(range(0, hi)) + [255, 256, 0x1FF, 0xFFFFFFE0 & 0xFFFF]
    return "movw %s, #%d" % (r(reg), rng.choice(choices))


# ---------------------------------------------------------------- classes

def c_dpimm(rng):
    """Data processing, modified immediate (T1/T2 32-bit)."""
    op = rng.choice(["and", "bic", "orr", "orn", "eor", "add", "adc",
                     "sbc", "sub", "rsb", "mov", "mvn", "tst", "teq",
                     "cmn", "cmp"])
    imm = mod_imm(rng)
    d, n = regs(rng, 2)
    s = rng.choice(["", "s"])
    if op in ("tst", "teq", "cmn", "cmp"):
        return Case(["%s%s %s, #0x%x" % (op, ".w" if op != "teq" else "",
                                         r(n), imm)])
    if op in ("mov", "mvn"):
        return Case(["%s%s.w %s, #0x%x" % (op, s, r(d), imm)])
    w = "" if op == "orn" else ".w"  # ORN has no narrow form to avoid
    return Case(["%s%s%s %s, %s, #0x%x" % (op, s, w, r(d), r(n), imm)])


def c_dpplain(rng):
    """Plain binary immediate: ADDW/SUBW, MOVW/MOVT, SSAT/USAT, bitfields."""
    k = rng.randrange(9)
    d, n = regs(rng, 2)
    if k == 0:
        return Case(["addw %s, %s, #%d" % (r(d), r(n), rng.randrange(4096))])
    if k == 1:
        return Case(["subw %s, %s, #%d" % (r(d), r(n), rng.randrange(4096))])
    if k == 2:
        return Case(["movw %s, #%d" % (r(d), rng.randrange(65536))])
    if k == 3:
        return Case(["movt %s, #%d" % (r(d), rng.randrange(65536))])
    if k == 4:
        sh = rng.choice(["", ", lsl #%d" % rng.randrange(1, 32),
                         ", asr #%d" % rng.randrange(1, 32)])
        if rng.randrange(2):
            return Case(["ssat %s, #%d, %s%s" % (r(d), rng.randrange(1, 33),
                                                 r(n), sh)])
        return Case(["usat %s, #%d, %s%s" % (r(d), rng.randrange(0, 32),
                                             r(n), sh)])
    if k == 5:
        if rng.randrange(2):
            return Case(["ssat16 %s, #%d, %s" % (r(d), rng.randrange(1, 17),
                                                 r(n))])
        return Case(["usat16 %s, #%d, %s" % (r(d), rng.randrange(0, 16),
                                             r(n))])
    lsb = rng.randrange(32)
    width = rng.randrange(1, 33 - lsb)
    if k == 6:
        return Case(["%s %s, %s, #%d, #%d" % (rng.choice(["sbfx", "ubfx"]),
                                              r(d), r(n), lsb, width)])
    if k == 7:
        return Case(["bfi %s, %s, #%d, #%d" % (r(d), r(n), lsb, width)])
    return Case(["bfc %s, #%d, #%d" % (r(d), lsb, width)])


def c_dpreg(rng):
    """Data processing, shifted register (T2/T3 32-bit)."""
    op = rng.choice(["and", "bic", "orr", "orn", "eor", "add", "adc",
                     "sbc", "sub", "rsb", "mov", "mvn", "tst", "teq",
                     "cmn", "cmp", "pkhbt", "pkhtb"])
    d, n, m = regs(rng, 3)
    s = rng.choice(["", "s"])
    sh = shift(rng)
    if op in ("tst", "teq", "cmn", "cmp"):
        q = "" if op == "teq" else ".w"
        return Case(["%s%s %s, %s%s" % (op, q, r(n), r(m), sh)])
    if op == "mov":
        # MOV with a shift *is* the shift instruction; as wants it spelled
        # that way once a width is forced.
        kind, _, amt = sh[2:].partition(" ")
        if kind == "rrx":
            return Case(["rrx%s.w %s, %s" % (s, r(d), r(m))])
        return Case(["%s%s.w %s, %s, %s" % (kind, s, r(d), r(m), amt)])
    if op == "mvn":
        return Case(["mvn%s.w %s, %s%s" % (s, r(d), r(m), sh)])
    if op == "pkhbt":
        return Case(["pkhbt %s, %s, %s, lsl #%d" % (r(d), r(n), r(m),
                                                    rng.randrange(32))])
    if op == "pkhtb":
        return Case(["pkhtb %s, %s, %s, asr #%d" % (r(d), r(n), r(m),
                                                    rng.randrange(1, 33))])
    q = "" if op == "orn" else ".w"
    return Case(["%s%s%s %s, %s, %s%s" % (op, s, q, r(d), r(n), r(m), sh)])


def c_shreg(rng):
    """LSL/LSR/ASR/ROR by register, wide -- the amount is Rm's low byte."""
    op = rng.choice(["lsl", "lsr", "asr", "ror"])
    d, n, m = regs(rng, 3)
    s = rng.choice(["", "s"])
    setup = [set_small(rng, m, 70)] if rng.randrange(3) else []
    return Case(["%s%s.w %s, %s, %s" % (op, s, r(d), r(n), r(m))],
                setup=setup)


def c_shedge(rng):
    """
    Shifts by a register, at the counts where the rule changes.

    The count is Rm's low byte, so it runs to 255, and nearly all of that
    range is uninteresting: what a shift by 7 does, a shift by 9 does.
    The behaviour turns at 0 (nothing happens, and the carry is left
    alone), at 32 (the result is gone but the carry is still the last
    bit out), at 33 (the carry is gone too, except for ASR) and at 256
    (which is 0 again). c_shreg draws from 0..69, so it lands on exactly
    32 once in seventy, and only matters there when the bit that falls
    out is set and something then reads the carry -- a translator with
    `<` where `<=` belonged agreed with the board on six thousand cases.

    So: counts from that list, operands with both end bits in play, the
    flag-setting forms, and an ADC afterwards that makes the carry a
    register value instead of leaving it to the flags field alone.
    """
    op = rng.choice(["lsl", "lsr", "asr", "ror"])
    cnt = rng.choice([0, 1, 31, 32, 33, 63, 64, 255, 0x100, 0x120, 0x11F,
                      0x101, rng.randrange(256)])
    val = rng.choice([0x80000000, 0x00000001, 0x80000001, 0xFFFFFFFF,
                      0x7FFFFFFE, 0x40000002, rng.getrandbits(32)])
    if rng.randrange(2):
        d, m, x = rng.sample(LOW, 3)
        ins = "%ss %s, %s" % (op, r(d), r(m))          # 16-bit: rdn, rm
        setup = ["ldr %s, =0x%08x" % (r(d), val), "ldr %s, =0x%08x" % (r(m), cnt)]
    else:
        d, n, m, x = regs(rng, 4)
        ins = "%ss.w %s, %s, %s" % (op, r(d), r(n), r(m))
        setup = ["ldr %s, =0x%08x" % (r(n), val), "ldr %s, =0x%08x" % (r(m), cnt)]
    c = Case([ins, "adc.w %s, %s, #0" % (r(x), r(x))], setup=setup)
    c.can_cond = False
    return c


def c_qflag(rng):
    """
    The instructions that set Q, on operands that make them.

    Q is sticky and says a result saturated or an accumulate overflowed.
    A case starts from a random APSR, so half the time Q is already set
    and nothing an instruction does to it can show; and the operands
    that overflow a 32-bit accumulate, or saturate, are a sliver of the
    random ones. So Q is cleared first and the operands are built at the
    edge: an accumulator within a product's reach of INT_MAX or INT_MIN,
    and saturating inputs either side of their limits.
    """
    d, n, m, a = regs(rng, 4)
    edge = rng.choice([0x7FFFFFFF, 0x80000000, 0x7FFF0000, 0x80010000,
                       0x7FFFFFFE, 0x80000001, 0x40000000, 0xC0000000,
                       rng.getrandbits(32)])
    h = lambda: rng.choice([0x7FFF, 0x8000, 0xFFFF, 0x0001, 0x4000,
                            rng.getrandbits(16)])
    v1 = (h() << 16) | h()
    v2 = (h() << 16) | h()
    k = rng.randrange(6)
    if k == 0:
        op = rng.choice(["smlabb", "smlabt", "smlatb", "smlatt", "smlawb",
                         "smlawt", "smlad", "smladx", "smlsd", "smlsdx"])
        ins = "%s %s, %s, %s, %s" % (op, r(d), r(n), r(m), r(a))
        setup = ["ldr %s, =0x%08x" % (r(n), v1), "ldr %s, =0x%08x" % (r(m), v2),
                 "ldr %s, =0x%08x" % (r(a), edge)]
    elif k == 1:
        op = rng.choice(["smuad", "smuadx"])
        ins = "%s %s, %s, %s" % (op, r(d), r(n), r(m))
        setup = ["ldr %s, =0x%08x" % (r(n), rng.choice([0x80008000, v1])),
                 "ldr %s, =0x%08x" % (r(m), rng.choice([0x80008000, v2]))]
    elif k == 2:
        op = rng.choice(["qadd", "qsub", "qdadd", "qdsub"])
        ins = "%s %s, %s, %s" % (op, r(d), r(m), r(n))
        setup = ["ldr %s, =0x%08x" % (r(n), edge),
                 "ldr %s, =0x%08x" % (r(m), rng.choice(
                     [1, 0xFFFFFFFF, 0x40000000, 0xC0000000, 0x7FFFFFFF,
                      0x80000000, rng.getrandbits(32)]))]
    elif k == 3:
        bits = rng.randrange(1, 33)
        sh = rng.choice(["", ", lsl #%d" % rng.randrange(1, 32),
                         ", asr #%d" % rng.randrange(1, 32)])
        ins = "ssat %s, #%d, %s%s" % (r(d), bits, r(n), sh)
        lim = (1 << (bits - 1)) & 0xFFFFFFFF
        setup = ["ldr %s, =0x%08x" % (r(n), rng.choice(
            [lim, (lim - 1) & 0xFFFFFFFF, (0 - lim) & 0xFFFFFFFF,
             (0 - lim - 1) & 0xFFFFFFFF, edge]))]
    elif k == 4:
        bits = rng.randrange(0, 32)
        ins = "usat %s, #%d, %s" % (r(d), bits, r(n))
        lim = (1 << bits) & 0xFFFFFFFF
        setup = ["ldr %s, =0x%08x" % (r(n), rng.choice(
            [lim, (lim - 1) & 0xFFFFFFFF, 0xFFFFFFFF, 0, 0x80000000, edge]))]
    else:
        bits = rng.randrange(1, 17)
        op = rng.choice(["ssat16", "usat16"])
        if op == "usat16":
            bits -= 1
        ins = "%s %s, #%d, %s" % (op, r(d), bits, r(n))
        setup = ["ldr %s, =0x%08x" % (r(n), v1)]
    # Q is cleared through r12 *before* the operands are loaded, so an
    # operand that drew r12 simply overwrites the scratch.
    c = Case([ins], setup=["movs r12, #0", "msr APSR_nzcvq, r12"] + setup)
    c.can_cond = False
    return c


def c_alu16(rng):
    """The 16-bit forms. Outside an IT block they all set flags."""
    k = rng.randrange(12)
    d, n, m = regs(rng, 3, LOW)
    if k == 0:
        op = rng.choice(["lsls", "lsrs", "asrs"])
        lo = 0 if op == "lsls" else 1
        return Case(["%s.n %s, %s, #%d" % (op, r(d), r(m),
                                           rng.randrange(lo, 32 if lo == 0 else 33))])
    if k == 1:
        return Case(["%s.n %s, %s, %s" % (rng.choice(["adds", "subs"]),
                                          r(d), r(n), r(m))])
    if k == 2:
        return Case(["%s.n %s, %s, #%d" % (rng.choice(["adds", "subs"]),
                                           r(d), r(n), rng.randrange(8))])
    if k == 3:
        op = rng.choice(["movs", "cmp", "adds", "subs"])
        if op in ("movs", "cmp"):
            return Case(["%s.n %s, #%d" % (op, r(d), rng.randrange(256))])
        return Case(["%s.n %s, #%d" % (op, r(d), rng.randrange(256))])
    if k == 4:
        op = rng.choice(["ands", "eors", "lsls", "lsrs", "asrs", "adcs",
                         "sbcs", "rors", "orrs", "bics", "mvns"])
        setup = [set_small(rng, m, 40)] if op in ("lsls", "lsrs", "asrs",
                                                  "rors") and rng.randrange(2) else []
        return Case(["%s.n %s, %s" % (op, r(d), r(m))], setup=setup)
    if k == 5:
        op = rng.choice(["tst", "cmp", "cmn"])
        return Case(["%s.n %s, %s" % (op, r(d), r(m))])
    if k == 6:
        return Case(["negs.n %s, %s" % (r(d), r(m))])
    if k == 7:
        return Case(["muls.n %s, %s, %s" % (r(d), r(m), r(d))])
    if k == 8:
        a, b = rng.sample(R, 2)
        op = rng.choice(["add", "mov", "cmp"])
        if op == "add":
            return Case(["add.n %s, %s" % (r(a), r(b))])
        if op == "mov":
            return Case(["mov.n %s, %s" % (r(a), r(b))])
        return Case(["cmp.n %s, %s" % (r(a), r(b))]) if (a > 7 or b > 7) \
            else Case(["cmp.n %s, %s" % (r(a), r(b))])
    if k == 9:
        op = rng.choice(["sxtb", "sxth", "uxtb", "uxth", "rev", "rev16",
                         "revsh"])
        return Case(["%s.n %s, %s" % (op, r(d), r(m))])
    if k == 10:
        # The no-flags forms, which only exist inside an IT block.
        cond = rng.choice(CONDS)
        op = rng.choice(["add", "sub", "and", "orr", "eor", "lsl", "mov"])
        if op in ("add", "sub"):
            ins = "%s%s.n %s, %s, %s" % (op, cond, r(d), r(n), r(m))
        elif op == "lsl":
            # Not #0: that encoding is MOVS T2, UNPREDICTABLE in a block.
            ins = "lsl%s.n %s, %s, #%d" % (cond, r(d), r(m),
                                           rng.randrange(1, 32))
        elif op == "mov":
            ins = "mov%s.n %s, #%d" % (cond, r(d), rng.randrange(256))
        else:
            ins = "%s%s.n %s, %s" % (op, cond, r(d), r(m))
        return Case(["it %s" % cond, ins])
    # A whole IT block: four instructions, mixed then/else.
    cond = rng.choice(CONDS)
    pat = "".join(rng.choice("te") for _ in range(rng.randrange(0, 4)))
    lines = ["it%s %s" % (pat, cond)]
    for i in range(len(pat) + 1):
        c = cond if i == 0 or pat[i - 1] == "t" else INV[cond]
        a, b, x = regs(rng, 3, LOW)
        lines.append(rng.choice([
            "add%s.n %s, %s, %s" % (c, r(a), r(b), r(x)),
            "mov%s.n %s, #%d" % (c, r(a), rng.randrange(256)),
            "adds%s.w %s, %s, #%d" % (c, r(a), r(b), rng.randrange(256)),
            "eor%s.n %s, %s" % (c, r(a), r(b)),
        ]))
    return Case(lines)


def c_mul(rng):
    """Multiply, multiply-accumulate and the DSP multiplies."""
    d, n, m, a = regs(rng, 4)
    op = rng.choice(["mul", "mla", "mls", "smlabb", "smlabt", "smlatb",
                     "smlatt", "smlad", "smladx", "smlawb", "smlawt",
                     "smlsd", "smlsdx", "smmla", "smmlar", "smmls",
                     "smmlsr", "smmul", "smmulr", "smuad", "smuadx",
                     "smulbb", "smulbt", "smultb", "smultt", "smulwb",
                     "smulwt", "smusd", "smusdx", "usad8", "usada8"])
    if op == "mul":
        return Case(["mul.w %s, %s, %s" % (r(d), r(n), r(m))])
    three = ("smmul", "smmulr", "smuad", "smuadx", "smulbb", "smulbt",
             "smultb", "smultt", "smulwb", "smulwt", "smusd", "smusdx",
             "usad8")
    if op in three:
        return Case(["%s %s, %s, %s" % (op, r(d), r(n), r(m))])
    return Case(["%s %s, %s, %s, %s" % (op, r(d), r(n), r(m), r(a))])


def c_mull(rng):
    """Long multiplies and divide. RdLo and RdHi must differ."""
    lo, hi, n, m = regs(rng, 4)
    op = rng.choice(["smull", "umull", "smlal", "umlal", "umaal",
                     "smlalbb", "smlalbt", "smlaltb", "smlaltt", "smlald",
                     "smlaldx", "smlsld", "smlsldx", "sdiv", "udiv"])
    if op in ("sdiv", "udiv"):
        setup = []
        k = rng.randrange(4)
        if k == 0:
            setup = ["mov.w %s, #0" % r(m)]  # no S: the flags are under test
        elif k == 1:
            setup = ["mov.w %s, #-1" % r(m)]
        return Case(["%s %s, %s, %s" % (op, r(lo), r(n), r(m))], setup=setup)
    return Case(["%s %s, %s, %s, %s" % (op, r(lo), r(hi), r(n), r(m))])


def c_dsp(rng):
    """Parallel add/subtract, saturating arithmetic, and the misc group."""
    d, n, m = regs(rng, 3)
    k = rng.randrange(4)
    if k == 0:
        pre = rng.choice(["s", "q", "sh", "u", "uq", "uh"])
        op = rng.choice(["add16", "asx", "sax", "sub16", "add8", "sub8"])
        return Case(["%s%s %s, %s, %s" % (pre, op, r(d), r(n), r(m))])
    if k == 1:
        op = rng.choice(["qadd", "qdadd", "qsub", "qdsub"])
        return Case(["%s %s, %s, %s" % (op, r(d), r(m), r(n))])
    if k == 2:
        op = rng.choice(["rev.w", "rev16.w", "rbit", "revsh.w", "clz"])
        return Case(["%s %s, %s" % (op, r(d), r(m))])
    return Case(["sel %s, %s, %s" % (r(d), r(n), r(m))])


def c_ext(rng):
    """Sign and zero extension, with rotation and with accumulate."""
    d, n, m = regs(rng, 3)
    rot = rng.choice(["", ", ror #8", ", ror #16", ", ror #24"])
    op = rng.choice(["sxtb", "uxtb", "sxth", "uxth", "sxtb16", "uxtb16",
                     "sxtab", "uxtab", "sxtah", "uxtah", "sxtab16",
                     "uxtab16"])
    if op in ("sxtb", "uxtb", "sxth", "uxth"):
        return Case(["%s.w %s, %s%s" % (op, r(d), r(m), rot)])
    if op in ("sxtb16", "uxtb16"):
        return Case(["%s %s, %s%s" % (op, r(d), r(m), rot)])
    return Case(["%s %s, %s, %s%s" % (op, r(d), r(n), r(m), rot)])


def mem_case(lines, base, setup, lo, hi, skew=0, wb_base=True):
    """
    A case whose base register points into diff_mem, at MEM_BASE + skew.
    [lo, hi) is the byte range it may touch, relative to that address,
    and is what the harness hashes afterwards -- with eight bytes of
    margin either side, so an access that lands *next to* where it
    should is still seen.
    """
    at = MEM_BASE + skew
    s = ["ldr %s, =diff_mem+%d" % (r(base), at)] + setup
    c = Case(lines, setup=s, addr=(1 << base) if wb_base else 0,
             mem=(max(0, at + lo - 8), min(16384, at + hi + 8)))
    c.can_cond = wb_base
    return c


def c_ldst(rng):
    """Single loads and stores, every addressing mode."""
    size = rng.choice(["", "b", "h", "sb", "sh"])
    store = size in ("", "b", "h") and rng.randrange(2) == 0
    op = ("str" if store else "ldr") + size
    width = {"": 4, "b": 1, "h": 2, "sb": 1, "sh": 2}[size]
    b, t, m = regs(rng, 3)
    skew = rng.choice([0, 0, 0, 1, 2, 3]) if width > 1 else 0
    k = rng.randrange(5)
    if k == 0:  # T3: imm12, add
        imm = rng.randrange(4096)
        return mem_case(["%s.w %s, [%s, #%d]" % (op, r(t), r(b), imm)],
                        b, [], imm, imm + width, skew)
    if k == 1:  # T4: negative imm8
        imm = rng.randrange(1, 256)
        return mem_case(["%s %s, [%s, #-%d]" % (op, r(t), r(b), imm)],
                        b, [], -imm, -imm + width, skew)
    if k == 2:  # pre-index with writeback
        imm = rng.randrange(-255, 256)
        return mem_case(["%s %s, [%s, #%d]!" % (op, r(t), r(b), imm)],
                        b, [], min(imm, 0), max(imm, 0) + width, skew)
    if k == 3:  # post-index
        imm = rng.randrange(-255, 256)
        return mem_case(["%s %s, [%s], #%d" % (op, r(t), r(b), imm)],
                        b, [], min(imm, 0), max(imm, 0) + width, skew)
    # register offset, LSL #0-3
    sh = rng.randrange(4)
    idx = rng.randrange(0, 64)
    return mem_case(["%s.w %s, [%s, %s, lsl #%d]" % (op, r(t), r(b), r(m),
                                                     sh)],
                    b, ["movw %s, #%d" % (r(m), idx)], 0,
                    (idx << sh) + width, skew)


def c_ldst16(rng):
    """The 16-bit load/store forms: immediate and register offset."""
    b, t, m = regs(rng, 3, LOW)
    if rng.randrange(2):
        op = rng.choice(["str", "strh", "strb", "ldrsb", "ldr", "ldrh",
                         "ldrb", "ldrsh"])
        idx = rng.randrange(0, 64)
        return mem_case(["%s.n %s, [%s, %s]" % (op, r(t), r(b), r(m))],
                        b, ["movw %s, #%d" % (r(m), idx)], 0, idx + 4)
    op, scale = rng.choice([("str", 4), ("ldr", 4), ("strb", 1),
                            ("ldrb", 1), ("strh", 2), ("ldrh", 2)])
    imm = rng.randrange(32) * scale
    return mem_case(["%s.n %s, [%s, #%d]" % (op, r(t), r(b), imm)],
                    b, [], 0, imm + 4)


def c_lddual(rng):
    """LDRD/STRD. Word aligned, Rt != Rt2, and no writeback onto either."""
    b, t, t2 = regs(rng, 3)
    store = rng.randrange(2)
    op = "strd" if store else "ldrd"
    imm = rng.randrange(-255, 256) * 4
    k = rng.randrange(3)
    if k == 0:
        return mem_case(["%s %s, %s, [%s, #%d]" % (op, r(t), r(t2), r(b),
                                                    imm)],
                        b, [], min(imm, 0), max(imm, 0) + 8)
    if k == 1:
        return mem_case(["%s %s, %s, [%s, #%d]!" % (op, r(t), r(t2), r(b),
                                                     imm)],
                        b, [], min(imm, 0), max(imm, 0) + 8)
    return mem_case(["%s %s, %s, [%s], #%d" % (op, r(t), r(t2), r(b), imm)],
                    b, [], min(imm, 0), max(imm, 0) + 8)


def c_ldm(rng):
    """LDM/STM, both directions, with and without writeback."""
    b = rng.choice(R)
    others = [x for x in R if x != b]
    lst = sorted(rng.sample(others, rng.randrange(2, 7)))
    wb = rng.randrange(2)
    op = rng.choice(["ldmia.w", "ldmdb", "stmia.w", "stmdb"])
    if op.startswith("ldm") and not wb and rng.randrange(3) == 0:
        lst = sorted(set(lst) | {b})  # base in the list, no writeback: legal
    span = 4 * len(lst)
    lo = -span if "db" in op else 0
    regl = "{%s}" % ", ".join(r(x) for x in lst)
    return mem_case(["%s %s%s, %s" % (op, r(b), "!" if wb else "", regl)],
                    b, [], lo, lo + span, wb_base=(b not in lst))


def c_ldm16(rng):
    """16-bit LDMIA/STMIA: writeback unless the base is in an LDM list."""
    b = rng.choice(LOW)
    lst = sorted(rng.sample([x for x in LOW if x != b], rng.randrange(1, 5)))
    regl = "{%s}" % ", ".join(r(x) for x in lst)
    if rng.randrange(2):
        return mem_case(["stmia.n %s!, %s" % (r(b), regl)], b, [], 0,
                        4 * len(lst))
    return mem_case(["ldmia.n %s!, %s" % (r(b), regl)], b, [], 0,
                    4 * len(lst))


def c_excl(rng):
    """LDREX/STREX and the local monitor. Every case clears it first."""
    b, t, d = regs(rng, 3)
    sz = rng.choice(["", "b", "h"])
    imm = rng.randrange(0, 64) * 4 if sz == "" else 0
    addr = "[%s, #%d]" % (r(b), imm) if sz == "" else "[%s]" % r(b)
    k = rng.randrange(4)
    lines = []
    if k == 3:
        # STREX to a different address than the LDREX marked: whether it
        # succeeds is IMPLEMENTATION DEFINED, and this asks the board.
        other = "[%s, #%d]" % (r(b), imm + 8) if sz == "" else "[%s]" % r(b)
        if sz != "":
            return mem_case(["ldrex%s %s, [%s]" % (sz, r(t), r(b)),
                             "add %s, %s, #16" % (r(b), r(b)),
                             "strex%s %s, %s, [%s]" % (sz, r(d), r(t), r(b))],
                            b, ["clrex"], 0, 24)
        lines = ["ldrex %s, %s" % (r(t), addr),
                 "strex %s, %s, %s" % (r(d), r(t), other)]
        return mem_case(lines, b, ["clrex"], 0, imm + 12)
    if k == 0:
        lines = ["ldrex%s %s, %s" % (sz, r(t), addr),
                 "strex%s %s, %s, %s" % (sz, r(d), r(t), addr)]
    elif k == 1:
        lines = ["ldrex%s %s, %s" % (sz, r(t), addr), "clrex",
                 "strex%s %s, %s, %s" % (sz, r(d), r(t), addr)]
    else:
        lines = ["strex%s %s, %s, %s" % (sz, r(d), r(t), addr)]
    return mem_case(lines, b, ["clrex"], 0, imm + 4)


def c_literal(rng):
    """PC-relative loads, forward and backward, the four sizes."""
    t, t2 = regs(rng, 2)
    val = rng.getrandbits(32)
    val2 = rng.getrandbits(32)
    op = rng.choice(["ldr.w", "ldrb.w", "ldrh.w", "ldrsb.w", "ldrsh.w",
                     "ldrd", "ldr.n"])
    if op == "ldr.n":
        t = rng.choice(LOW)
    dst = "%s, %s" % (r(t), r(t2)) if op == "ldrd" else r(t)
    if rng.randrange(2) or op == "ldr.n":
        return Case(["%s %s, 2f" % (op, dst), "b 3f", ".align 2",
                     "2: .word 0x%08x, 0x%08x" % (val, val2), "3:"])
    return Case(["b 3f", ".align 2", "2: .word 0x%08x, 0x%08x" % (val, val2),
                 "3: %s %s, 2b" % (op, dst)])


def c_carry(rng):
    """
    ADC and SBC where the carry *in* is what produces the carry out.

    A three-input add carries out of either of its two additions, and the
    second -- adding the carry itself -- only carries when the first sum
    is exactly 0xFFFFFFFF. For ADC that needs a + b == 0xFFFFFFFF; for
    SBC, which is a + ~b + C, it needs a == b. Two random operands
    essentially never satisfy either, so six thousand random cases
    exercised one half of the rule and never the other: a translator
    that dropped the second carry agreed with the board on every one of
    them.

    So the operands are built to sit on that edge and one either side of
    it, and the carry in is set both ways.
    """
    d, n, m = regs(rng, 3)
    x = rng.choice([rng.getrandbits(32), 0, 1, 0x7FFFFFFF, 0x80000000,
                    0xFFFFFFFF, 0xFFFFFFFE])
    delta = rng.choice([0, 0, 1, 0xFFFFFFFF])
    op = rng.choice(["adcs", "sbcs", "adc", "sbc", "adcs.w", "sbcs.w"])
    y = (((~x) if op.startswith("adc") else x) + delta) & 0xFFFFFFFF
    # Carry in: `cmp r, r` sets C, `cmn r, r` on zero clears it.
    cin = rng.choice([["cmp %s, %s" % (r(d), r(d))],
                      ["movs %s, #0" % r(d), "cmn %s, %s" % (r(d), r(d))]])
    if op in ("adcs", "sbcs") and d < 8 and m < 8 and rng.randrange(2):
        # The 16-bit form, rdn and rm. The carry-in set-up uses rd as its
        # scratch, so rd is loaded after it: a literal load leaves the
        # flags alone.
        c = Case(["%s %s, %s" % (op, r(d), r(m))],
                 setup=cin + ["ldr %s, =0x%08x" % (r(d), x),
                              "ldr %s, =0x%08x" % (r(m), y)])
    else:
        c = Case(["%s%s %s, %s, %s" % (op.replace(".w", ""),
                                       ".w" if op.endswith(".w") else "",
                                       r(d), r(n), r(m))],
                 setup=cin + ["ldr %s, =0x%08x" % (r(n), x),
                              "ldr %s, =0x%08x" % (r(m), y)])
    c.can_cond = False
    return c


def c_adr(rng):
    """
    ADR, narrow and wide, forward and backward, at both alignments of the
    pc and -- wide only -- to targets at every byte offset.

    The result is an address in the image, which is not the same number
    on the board as in the emulator. So it is compared with the same
    label fetched as a literal, which the linker relocated and no
    pc-relative arithmetic touched: the difference is zero on both, or
    ADR is wrong.

    **The whole difficulty is Align(PC, 4)**, so the optional NOP in front
    matters more than anything else here: without it every ADR in a case
    would sit at the same alignment and half the rule would never run.
    """
    a = rng.choice(LOW)
    b = rng.choice([x for x in R if x != a])
    narrow = rng.randrange(3) == 0
    back = (not narrow) and rng.randrange(2) == 1
    k = 0 if narrow else rng.randrange(4)
    pad = ["nop"] if rng.randrange(2) else []
    data = [".align 2"]
    if k:
        data.append(".byte " + ", ".join(["0x%02x" % rng.randrange(256)] * k))
    data.append("2: .byte " + ", ".join("0x%02x" % rng.randrange(256)
                                         for _ in range(4 - k)))
    use = ["adr%s %s, %s" % ("" if narrow else ".w", r(a), "2b" if back else "2f"),
           "ldr.w %s, =%s" % (r(b), "2b" if back else "2f"),
           "subs %s, %s, %s" % (r(a), r(a), r(b)),
           "mov.w %s, #0" % r(b)]
    if back:
        return Case(["b 3f"] + data + ["3:"] + pad + use)
    return Case(pad + use + ["b 3f"] + data + ["3:"])


def c_branch(rng):
    """Conditional branches, both widths, CBZ/CBNZ, and TBB/TBH."""
    k = rng.randrange(4)
    a = rng.choice(R)
    if k == 0:
        cond = rng.choice(CONDS)
        w = rng.choice([".n", ".w"])
        return Case(["b%s%s 1f" % (cond, w), "movw %s, #0xBAD" % r(a), "1:"])
    if k == 1:
        lo = rng.choice(LOW)
        op = rng.choice(["cbz", "cbnz"])
        setup = ["movs %s, #0" % r(lo)] if rng.randrange(2) else []
        return Case(["%s %s, 1f" % (op, r(lo)), "movw %s, #0xBAD" % r(a),
                     "1:"], setup=setup)
    if k == 2:
        m = rng.choice(R)
        idx = rng.randrange(4)
        return Case(["movw %s, #%d" % (r(m), idx), "tbb [pc, %s]" % r(m),
                     "0: .byte (10f-0b)/2, (11f-0b)/2, (12f-0b)/2, (13f-0b)/2",
                     "10: movw r0, #0x10", "b 4f",
                     "11: movw r0, #0x11", "b 4f",
                     "12: movw r0, #0x12", "b 4f",
                     "13: movw r0, #0x13", "4:"])
    m = rng.choice(R)
    idx = rng.randrange(3)
    return Case(["movw %s, #%d" % (r(m), idx), "tbh [pc, %s, lsl #1]" % r(m),
                 "0: .hword (10f-0b)/2, (11f-0b)/2, (12f-0b)/2",
                 "10: movw r1, #0x20", "b 4f",
                 "11: movw r1, #0x21", "b 4f",
                 "12: movw r1, #0x22", "4:"])


def c_psr(rng):
    """MSR to the APSR's fields, then MRS it back."""
    s, d = regs(rng, 2)
    f = rng.choice(["APSR_nzcvq", "APSR_g", "APSR_nzcvqg"])
    return Case(["msr %s, %s" % (f, r(s)), "mrs %s, APSR" % r(d)])


def c_misc(rng):
    """Hints and preloads: they must decode and do nothing."""
    b = rng.choice(R)
    k = rng.randrange(4)
    if k == 0:
        return mem_case(["pld [%s, #%d]" % (r(b), rng.randrange(4096))],
                        b, [], 0, 0)
    if k == 1:
        return mem_case(["pli [%s, #-%d]" % (r(b), rng.randrange(1, 256))],
                        b, [], 0, 0)
    if k == 2:
        return Case([rng.choice(["nop.w", "nop", "yield", "sev"])])
    return Case([rng.choice(["dsb sy", "dmb sy", "isb sy"])])


# ---------------------------------------------------------------- FPU

def sreg(rng):
    return "s%d" % rng.randrange(32)


def fp_imm(rng):
    """A value VMOV.F32 can encode: +/-(16+efgh)/16 * 2^(-3..4)."""
    imm8 = rng.randrange(256)
    sign = -1.0 if imm8 & 0x80 else 1.0
    b = (imm8 >> 6) & 1
    exp = ((b ^ 1) << 2 | (imm8 >> 4) & 3) - 3 if True else 0
    exp = (((b ^ 1) << 2) | ((imm8 >> 4) & 3)) - 3
    mant = (16 + (imm8 & 15)) / 16.0
    return repr(sign * mant * (2.0 ** exp))


def fpcase(lines, setup=None, mem=None, base=None):
    c = Case(lines, setup=setup or [], fp=True)
    return c


def c_fparith(rng):
    d, n, m = sreg(rng), sreg(rng), sreg(rng)
    k = rng.randrange(5)
    if k == 0:
        op = rng.choice(["vadd", "vsub", "vmul", "vdiv", "vnmul"])
        return fpcase(["%s.f32 %s, %s, %s" % (op, d, n, m)])
    if k == 1:
        op = rng.choice(["vmla", "vmls", "vnmla", "vnmls", "vfma", "vfms",
                         "vfnma", "vfnms"])
        return fpcase(["%s.f32 %s, %s, %s" % (op, d, n, m)])
    if k == 2:
        op = rng.choice(["vsqrt", "vabs", "vneg", "vmov"])
        return fpcase(["%s.f32 %s, %s" % (op, d, m)])
    if k == 3:
        return fpcase(["vmov.f32 %s, #%s" % (d, fp_imm(rng))])
    # A short dependent chain: the second operation reads the first's
    # result, so a wrong rounding mode or flush shows up twice.
    op1 = rng.choice(["vadd", "vmul", "vsub"])
    op2 = rng.choice(["vdiv", "vfma", "vmla"])
    return fpcase(["%s.f32 %s, %s, %s" % (op1, d, n, m),
                   "%s.f32 %s, %s, %s" % (op2, m, d, n)])


def c_fpcmp(rng):
    d, m = sreg(rng), sreg(rng)
    op = rng.choice(["vcmp", "vcmpe"])
    if rng.randrange(3) == 0:
        ins = "%s.f32 %s, #0" % (op, d)
    else:
        ins = "%s.f32 %s, %s" % (op, d, m)
    return fpcase([ins, "vmrs APSR_nzcv, fpscr"])


def c_fpcvt(rng):
    d, m = sreg(rng), sreg(rng)
    k = rng.randrange(6)
    if k == 0:
        op = rng.choice(["vcvt.s32.f32", "vcvt.u32.f32", "vcvtr.s32.f32",
                         "vcvtr.u32.f32", "vcvt.f32.s32", "vcvt.f32.u32"])
        return fpcase(["%s %s, %s" % (op, d, m)])
    if k == 1:
        t = rng.choice(["s16", "u16", "s32", "u32"])
        size = 16 if t.endswith("16") else 32
        fb = rng.randrange(1, size + 1)
        if rng.randrange(2):
            return fpcase(["vcvt.%s.f32 %s, %s, #%d" % (t, d, d, fb)])
        return fpcase(["vcvt.f32.%s %s, %s, #%d" % (t, d, d, fb)])
    if k == 2:
        op = rng.choice(["vcvtb.f16.f32", "vcvtt.f16.f32", "vcvtb.f32.f16",
                         "vcvtt.f32.f16"])
        return fpcase(["%s %s, %s" % (op, d, m)])
    if k == 3:
        op = rng.choice(["vcvta", "vcvtn", "vcvtp", "vcvtm"])
        t = rng.choice(["s32", "u32"])
        return fpcase(["%s.%s.f32 %s, %s" % (op, t, d, m)])
    if k == 4:
        op = rng.choice(["vrinta", "vrintn", "vrintp", "vrintm", "vrintr",
                         "vrintz", "vrintx"])
        return fpcase(["%s.f32 %s, %s" % (op, d, m)])
    # AHP set first, then a conversion: the alternative half format.
    r0 = rng.choice(R)
    op = rng.choice(["vcvtb.f16.f32", "vcvtt.f32.f16"])
    return fpcase(["vmrs %s, fpscr" % r(r0), "orr %s, %s, #0x4000000" % (r(r0), r(r0)),
                   "vmsr fpscr, %s" % r(r0), "%s %s, %s" % (op, d, m)])


def c_fpnm(rng):
    d, n, m = sreg(rng), sreg(rng), sreg(rng)
    if rng.randrange(2):
        op = rng.choice(["vmaxnm", "vminnm"])
        return fpcase(["%s.f32 %s, %s, %s" % (op, d, n, m)])
    cc = rng.choice(["eq", "vs", "ge", "gt"])
    return fpcase(["vsel%s.f32 %s, %s, %s" % (cc, d, n, m)])


def c_fpmov(rng):
    a, b = regs(rng, 2)
    k = rng.randrange(7)
    sn = rng.randrange(31)
    dn = rng.randrange(16)
    if k == 0:
        return fpcase(["vmov s%d, %s" % (sn, r(a))])
    if k == 1:
        return fpcase(["vmov %s, s%d" % (r(a), sn)])
    if k == 2:
        return fpcase(["vmov s%d, s%d, %s, %s" % (sn, sn + 1, r(a), r(b))])
    if k == 3:
        return fpcase(["vmov %s, %s, s%d, s%d" % (r(a), r(b), sn, sn + 1)])
    if k == 4:
        if rng.randrange(2):
            return fpcase(["vmov d%d, %s, %s" % (dn, r(a), r(b))])
        return fpcase(["vmov %s, %s, d%d" % (r(a), r(b), dn)])
    if k == 5:
        # VMOV.32 Dd[x] <-> Rt. GNU as refuses the scalar form under
        # fpv5-sp-d16 although the ARM ARM allows it on single-precision
        # parts, so it goes in as an encoding and the board decides.
        x = rng.randrange(2)
        load = rng.randrange(2)
        w0 = 0xEE00 | (load << 4) | (x << 5) | (dn & 15)
        w1 = (a << 12) | 0x0B10 | (((dn >> 4) & 1) << 7)
        c = fpcase([".inst.w 0x%04x%04x" % (w0, w1)])
        c.text = "vmov.32 %s d%d[%d]%s" % ("r%d <-" % a if load else "->",
                                            dn, x, "" if load else ", r%d" % a)
        c.can_cond = False
        return c
    # FPSCR through a core register: which bits are writable.
    return fpcase(["vmsr fpscr, %s" % r(a), "vmrs %s, fpscr" % r(b)])


def c_fpldst(rng):
    b = rng.choice(R)
    k = rng.randrange(4)
    if k == 0:
        reg = "s%d" % rng.randrange(32) if rng.randrange(2) else "d%d" % rng.randrange(16)
        op = rng.choice(["vldr", "vstr"])
        off = rng.randrange(-255, 256) * 4
        c = mem_case(["%s %s, [%s, #%d]" % (op, reg, r(b), off)], b, [],
                     min(off, 0), max(off, 0) + 8)
    elif k == 1:
        first = rng.randrange(28)
        cnt = rng.randrange(1, 5)
        lst = ("{s%d}" % first) if cnt == 1 else "{s%d-s%d}" % (first, first + cnt - 1)
        op = rng.choice(["vldmia", "vstmia", "vldmdb", "vstmdb"])
        wb = "!" if (op.endswith("db") or rng.randrange(2)) else ""
        span = 4 * cnt
        lo = -span if op.endswith("db") else 0
        c = mem_case(["%s %s%s, %s" % (op, r(b), wb, lst)], b, [], lo, lo + span)
    elif k == 2:
        # VLDMIA/VSTMIA with D registers, as an encoding for the same
        # reason as the scalar VMOV: as refuses them for an SP-only FPU.
        first = rng.randrange(14)
        cnt = rng.randrange(1, 3)
        load = rng.randrange(2)
        w0 = 0xECA0 | (load << 4) | b          # P=0 U=1 W=1
        w1 = (first << 12) | 0x0B00 | (2 * cnt)
        c = mem_case([".inst.w 0x%04x%04x" % (w0, w1)], b, [], 0, 8 * cnt)
        c.text = "%s r%d!, {d%d-d%d}" % ("vldmia" if load else "vstmia", b,
                                          first, first + cnt - 1)
        c.can_cond = False
    else:
        val = rng.getrandbits(32)
        reg = "s%d" % rng.randrange(32)
        c = Case(["vldr %s, 2f" % reg, "b 3f", ".align 2",
                  "2: .word 0x%08x" % val, "3:"])
    c.fp = True
    return c


# The values an FPU's rules are about. Random operands reach a NaN pair
# with distinct payloads about once a run -- which is how a mutant that
# swapped NaN operand order passed 3000 cases -- so this class loads them
# on purpose.
FSPECIAL = [
    0x7FC00000, 0xFFC00000, 0x7FC12345, 0xFFE54321,   # quiet NaNs
    0x7F812345, 0xFF800001, 0x7FA00000,               # signalling NaNs
    0x7F800000, 0xFF800000, 0x00000000, 0x80000000,   # inf, zero
    0x00000001, 0x807FFFFF, 0x00400000, 0x80000003,   # denormals
    0x00800000, 0x80800001, 0x7F7FFFFF, 0xFF7FFFFF,   # normal extremes
    0x3F800000, 0xBF800000, 0x3F800001, 0x4B000000,   # 1, ulp, 2^23
    0x4B7FFFFF, 0x3F000000, 0x3FC00000, 0x40200000,   # halfway points
    0x4F000000, 0xCF000000, 0x4EFFFFFF, 0x47800000,   # int32 edges, 65536
    0x477FE000, 0x47FFE000, 0x387FC000, 0x33800000,   # half-precision edges
]

FPMODES = [0x00000000, 0x00400000, 0x00800000, 0x00C00000,  # RN RP RM RZ
           0x01000000, 0x02000000, 0x03000000, 0x04000000,  # FZ DN both AHP
           0x01C00000, 0x02800000, 0x07C00000]


def c_fpspecial(rng):
    d, n, m = rng.sample(range(32), 3)
    setup = []
    vals = []

    def load(sreg):
        v = rng.choice(FSPECIAL) if rng.randrange(5) else rng.getrandbits(32)
        vals.append("s%d=%08x" % (sreg, v))
        setup.extend(["ldr r12, =0x%08x" % v, "vmov s%d, r12" % sreg])

    for x in (d, n, m):
        load(x)
    mode = rng.choice(FPMODES)
    vals.append("fpscr=%08x" % mode)
    setup.extend(["ldr r12, =0x%08x" % mode, "vmsr fpscr, r12"])
    sd, sn, sm = "s%d" % d, "s%d" % n, "s%d" % m
    op = rng.choice([
        "vadd.f32 %s, %s, %s", "vsub.f32 %s, %s, %s", "vmul.f32 %s, %s, %s",
        "vdiv.f32 %s, %s, %s", "vnmul.f32 %s, %s, %s", "vmla.f32 %s, %s, %s",
        "vmls.f32 %s, %s, %s", "vnmla.f32 %s, %s, %s", "vnmls.f32 %s, %s, %s",
        "vfma.f32 %s, %s, %s", "vfms.f32 %s, %s, %s", "vfnma.f32 %s, %s, %s",
        "vfnms.f32 %s, %s, %s", "vmaxnm.f32 %s, %s, %s",
        "vminnm.f32 %s, %s, %s",
        "vsqrt.f32 %s, %s", "vcmp.f32 %s, %s", "vcmpe.f32 %s, %s",
        "vcvt.s32.f32 %s, %s", "vcvt.u32.f32 %s, %s", "vcvtr.s32.f32 %s, %s",
        "vcvtr.u32.f32 %s, %s", "vcvta.s32.f32 %s, %s",
        "vcvtn.u32.f32 %s, %s", "vcvtb.f16.f32 %s, %s",
        "vcvtt.f32.f16 %s, %s", "vrinta.f32 %s, %s", "vrintx.f32 %s, %s",
        "vrintz.f32 %s, %s", "vrintm.f32 %s, %s",
    ])
    nargs = op.count("%s")
    ins = op % ((sd, sn, sm) if nargs == 3 else (sd, sm))
    lines = [ins]
    if op.startswith("vcmp"):
        lines.append("vmrs APSR_nzcv, fpscr")
    c = Case(lines, setup=setup, fp=True)
    c.text = " ".join(vals) + "; " + "; ".join(lines)
    c.can_cond = False
    return c


def c_fphalf(rng):
    """
    Half precision, both ways, in both formats, under every mode.

    The four conversions have more rules per instruction than anything
    else in the FPU -- two formats (IEEE and the alternative, which has no
    infinity or NaN and so saturates instead), four rounding directions,
    flush-to-zero and default-NaN -- and a uniformly random single is far
    outside a half's range almost every time, which exercises overflow
    and nothing else. So the operand's exponent is drawn from the band a
    half can hold, a little past each end, with a random sign and
    fraction; and the mode is any of the thirty-two.

    **Added after the alternative-format path was found rounding a
    negative number as though it were positive**: -108885.19 toward minus
    infinity. The existing classes had run eight thousand FP cases
    without the four conditions for that ever coinciding.
    """
    d, m = rng.sample(range(32), 2)
    to_half = rng.randrange(2) == 0
    if to_half:
        k = rng.randrange(8)
        if k == 0:
            v = rng.choice(FSPECIAL)
        else:
            # 2^-27 .. 2^18: a half holds 2^-24 .. 2^16 (2^17 alternative)
            e = 127 + rng.randrange(-27, 19)
            frac = rng.getrandbits(23)
            if k == 1:
                frac &= 0x7FE000   # exactly representable
            elif k == 2:
                frac = (frac & 0x7FE000) | 0x001000   # a tie
            v = (rng.getrandbits(1) << 31) | (e << 23) | frac
    else:
        v = rng.getrandbits(32)
        if rng.randrange(4) == 0:
            h = rng.choice([0x0000, 0x8000, 0x7C00, 0xFC00, 0x7E00, 0x7D00,
                            0xFE01, 0x0001, 0x83FF, 0x0400, 0x7BFF, 0xFFFF,
                            0x7FFF, 0x3C00])
            v = h | (h << 16)
    mode = rng.getrandbits(5) << 22
    old = rng.getrandbits(32)
    op = rng.choice(["vcvtb", "vcvtt"]) + (".f16.f32" if to_half else ".f32.f16")
    ins = "%s s%d, s%d" % (op, d, m)
    c = Case([ins], fp=True, setup=[
        "ldr r12, =0x%08x" % v, "vmov s%d, r12" % m,
        "ldr r12, =0x%08x" % old, "vmov s%d, r12" % d,
        "ldr r12, =0x%08x" % mode, "vmsr fpscr, r12"])
    c.text = "s%d=%08x s%d=%08x fpscr=%08x; %s" % (m, v, d, old, mode, ins)
    c.can_cond = False
    return c


def cond_wrap(rng, case):
    """
    Run a single-instruction case under IT: half the time its condition
    holds, half the time it does not, which is the only way to test that
    a 32-bit instruction is *suppressed* rather than executed.
    """
    if len(case.lines) != 1 or rng.randrange(4) != 0 or not case.can_cond:
        return case
    ins = case.lines[0]
    mnem, _, rest = ins.partition(" ")
    if mnem.startswith(("it", "b", "cb", "tb", "mov.n", "pld", "pli",
                        "nop", "yield", "sev", "dsb", "dmb", "isb",
                        "ldrex", "strex", "clrex", "msr", "mrs")):
        return case
    if mnem.endswith(".n"):
        return case  # the narrow encodings change meaning inside IT
    cond = rng.choice(CONDS)
    q = ""
    base = mnem
    if mnem.startswith("v"):
        # FP: the condition goes before the type suffix, vaddeq.f32 -- and
        # the FPv5 additions are unconditional by definition.
        if mnem.startswith(("vsel", "vmaxnm", "vminnm", "vrinta", "vrintn",
                            "vrintp", "vrintm", "vcvta", "vcvtn", "vcvtp",
                            "vcvtm")):
            return case
        base, dot, q = mnem.partition(".")
        q = dot + q
    elif mnem.endswith(".w"):
        base, q = mnem[:-2], ".w"
    case.lines = ["it %s" % cond, "%s%s%s %s" % (base, cond, q, rest)]
    case.text = "; ".join(case.lines)
    return case


CLASSES = {
    "dpimm": c_dpimm, "dpplain": c_dpplain, "dpreg": c_dpreg,
    "shreg": c_shreg, "shedge": c_shedge, "qflag": c_qflag,
    "alu16": c_alu16, "mul": c_mul, "mull": c_mull,
    "dsp": c_dsp, "ext": c_ext, "ldst": c_ldst, "ldst16": c_ldst16,
    "lddual": c_lddual, "ldm": c_ldm, "ldm16": c_ldm16, "excl": c_excl,
    "literal": c_literal, "adr": c_adr, "carry": c_carry, "branch": c_branch,
    "psr": c_psr,
    "misc": c_misc,
    "fparith": c_fparith, "fpcmp": c_fpcmp, "fpcvt": c_fpcvt, "fpnm": c_fpnm,
    "fpmov": c_fpmov, "fpldst": c_fpldst, "fpspecial": c_fpspecial,
    "fphalf": c_fphalf,
}

FP_CLASSES = "fparith,fpcmp,fpcvt,fpnm,fpmov,fpldst"


def emit(cases, seed, out):
    s = []
    s.append("/* Generated by tests/armv7m-diff/gen.py -- do not edit. */")
    s.append("    .syntax unified")
    s.append("    .thumb")
    s.append("    .fpu fpv5-sp-d16")
    s.append("    .text")
    for i, c in enumerate(cases):
        s.append("")
        s.append("@ case %d: %s" % (i, c.text))
        s.append("    .align 2")
        s.append("    .thumb_func")
        s.append("    .type case_%d, %%function" % i)
        s.append("case_%d:" % i)
        s.append("    push {r4-r11, lr}")
        s.append("    push {r0}")
        if c.fp:
            s.append("    add r1, r0, #%d" % 56)
            s.append("    vldm r1, {s0-s31}")
            s.append("    ldr r1, [r0, #184]")
            s.append("    vmsr fpscr, r1")
        s.append("    ldr r1, [r0, #52]")
        s.append("    msr APSR_nzcvqg, r1")
        s.append("    ldm r0, {r0-r12}")
        for line in c.setup:
            s.append("    " + line)
        if c.raw:
            s.append("probe_%d:" % i)
        for line in c.lines:
            s.append("    " + line)
        s.append("    push {r12}")
        s.append("    ldr r12, [sp, #4]")
        s.append("    stm r12, {r0-r11}")
        s.append("    pop {r0}")
        s.append("    str r0, [r12, #48]")
        s.append("    mrs r0, APSR")
        s.append("    str r0, [r12, #52]")
        if c.fp:
            s.append("    add r1, r12, #56")
            s.append("    vstm r1, {s0-s31}")
            s.append("    vmrs r1, fpscr")
            s.append("    str r1, [r12, #184]")
        s.append("    add sp, #4")
        s.append("    pop {r4-r11, pc}")
        s.append("    .ltorg")
    s.append("")
    s.append("    .section .rodata")
    s.append("    .align 2")
    s.append("    .global diff_cases")
    s.append("diff_cases:")
    for i in range(len(cases)):
        s.append("    .word case_%d" % i)
    s.append("    .global diff_meta")
    s.append("diff_meta:")
    for i, c in enumerate(cases):
        s.append("    .word 0x%x, %d, %d, %d, %s"
                 % (c.addr, c.mem[0], c.mem[1],
                    (1 if c.fp else 0) | (2 if c.raw else 0),
                    "probe_%d" % i if c.raw else "0"))
    s.append("    .global diff_ncases")
    s.append("diff_ncases: .word %d" % len(cases))
    s.append("    .global diff_seed")
    s.append("diff_seed: .word 0x%08x" % (seed & 0xFFFFFFFF))
    with open(out + "/cases.S", "w") as f:
        f.write("\n".join(s) + "\n")
    with open(out + "/cases.txt", "w") as f:
        for i, c in enumerate(cases):
            f.write("%d\t%s\n" % (i, c.text))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seed", type=lambda x: int(x, 0), default=1)
    ap.add_argument("--count", type=int, default=2000)
    ap.add_argument("--classes", default=",".join(CLASSES))
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    rng = random.Random(a.seed)
    names = [n for n in a.classes.split(",") if n]
    for n in names:
        if n not in CLASSES:
            sys.exit("unknown class %s; have %s" % (n, ", ".join(CLASSES)))
    cases = []
    for i in range(a.count):
        c = CLASSES[names[i % len(names)]](rng)
        cases.append(cond_wrap(rng, c))
    emit(cases, a.seed, a.out)


if __name__ == "__main__":
    main()
