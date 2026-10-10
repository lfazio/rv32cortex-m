#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""The answers alu.c checks itself against, worked out here and not there.

A test that derives its expectation from the implementation encodes the
bug as the expectation. So alu.c computes each value with compiled
PowerPC instructions, and this computes the same value with Python's
integers, which share nothing with the emulator. Run it to regenerate
alu_want.h after changing an input:

    python3 tests/guest/ppc/alu_want.py > tests/guest/ppc/alu_want.h

The inputs are repeated in alu.c as volatile globals; the two lists have
to agree, and a disagreement shows up as every check failing at once.
"""

M32 = 0xFFFFFFFF
M64 = 0xFFFFFFFFFFFFFFFF

VA = 0xDEADBEEF
VB = 7
VC = 0x80000000
VD = 0x12345678
SA = -1000003
SB = 17
QA = 0xFEDCBA98F6543210
QB = 0x0123456789ABCDEF
VN = 13
VM = 45


def s32(x):
    x &= M32
    return x - (1 << 32) if x & 0x80000000 else x


def s64(x):
    x &= M64
    return x - (1 << 64) if x & (1 << 63) else x


def tdiv(a, b):
    """C division: toward zero."""
    q = abs(a) // abs(b)
    return -q if (a < 0) != (b < 0) else q


def tmod(a, b):
    return a - tdiv(a, b) * b


def rotl(x, n):
    n &= 31
    return ((x << n) | (x >> (32 - n))) & M32 if n else x & M32


def clz(x):
    return 32 - x.bit_length()


def bswap32(x):
    return int.from_bytes(x.to_bytes(4, "big"), "little")


def fib(n):
    a, b = 0, 1
    for _ in range(n):
        a, b = b, (a + b) & M32
    return a


W = []


def want(name, v):
    W.append((name, v & M32))


def want64(name, v):
    want(name + "_HI", (v & M64) >> 32)
    want(name + "_LO", v & M32)


want64("ADD64", QA + QB)
want64("SUB64", QA - QB)
want64("SUB64R", QB - QA)
want64("MUL64", QA * QB)
want64("SHR64", QA >> VN)
want64("SHL64", QA << VN)
want64("SAR64", s64(QA) >> VN)
want64("SHR64W", QA >> VM)
want64("SHL64W", QA << VM)
want64("SAR64W", s64(QA) >> VM)
want("CMP64", (1 if QA > QB else 0) | (2 if s64(QA) > s64(QB) else 0)
     | (4 if QA == QB else 0) | (8 if s64(QA) < 0 else 0))
want64("NEG64", -QB)

want("UDIV", VA // VB)
want("UMOD", VA % VB)
want("SDIV", tdiv(SA, SB))
want("SMOD", tmod(SA, SB))
want("SDIV8", tdiv(SA, 8))
want("SMOD8", tmod(SA, 8))
want("SAR3", SA >> 3)
want("SDIVNEG", tdiv(SA, -SB))

want("ROTL", rotl(VA, VN))
want("ROTR", rotl(VA, 32 - VN))
want("EXTRACT", (VA >> 5) & 0x3FF)
want("INSERT", (VD & ~0x00FFF000 & M32) | ((VA << 12) & 0x00FFF000))
want("CLZ", clz(VD) + (clz(VB) << 8) + (clz(VC) << 16))
want("EXTSB", s32(VA & 0xFF if not VA & 0x80 else (VA & 0xFF) - 256))
want("EXTSH", s32(VA & 0xFFFF if not VA & 0x8000 else (VA & 0xFFFF) - 65536))
want("BSWAP", bswap32(VA))
want("BSWAP16", ((VD & 0xFF) << 8) | ((VD >> 8) & 0xFF))
want("UMAX", max(VA, VD))
want("SMAX", max(s32(VA), s32(VD)))
want("SMIN", min(s32(VA), s32(VD)))
want("MULHW", (SA * SB) >> 32)
want("MULHWU", (VA * VD) >> 32)
want("MULHWS", (s32(VA) * s32(VD)) >> 32)
want("MULLI", SA * 1000)
want("NEG", -SA)
want("ABS", abs(SA))
want("LOGIC", (~(VA & VD) & M32) ^ (VA & ~VD & M32) ^ ((VA | ~VD) & M32)
     ^ (~(VA ^ VD) & M32) ^ (~(VA | VD) & M32))
want("SETCC", (1 if VA == VD else 0) | (2 if VA != 0 else 0)
     | (4 if SA < SB else 0) | (8 if VA < VD else 0)
     | (16 if s32(VA) < s32(VD) else 0) | (32 if SA <= -1000003 else 0)
     | (64 if VC > 0x7FFFFFFF else 0) | (128 if s32(VC) >= 0 else 0))

# A 96-bit add in three words, the carries computed by comparison.
x = (VA << 64) | (VD << 32) | VC
y = (VC << 64) | (VA << 32) | VA
z = (x + y) & ((1 << 96) - 1)
want("ADD96_2", z >> 64)
want("ADD96_1", z >> 32)
want("ADD96_0", z)

# Memory: bytes, halfwords and words written by formula, then summed.
b = [(i * 37 + 11) & 0xFF for i in range(64)]
h = [(i * 2749 + 5) & 0xFFFF for i in range(32)]
w = [(i * 0x9E3779B1 + VA) & M32 for i in range(16)]
want("SUMB", sum(b))
want("SUMSB", sum(v - 256 if v & 0x80 else v for v in b))
want("SUMH", sum(h))
want("SUMSH", sum(v - 65536 if v & 0x8000 else v for v in h))
xw = 0
for v in w:
    xw = rotl(xw, 5) ^ v
want("XORW", xw)

# Byte order: a word stored and read back a byte at a time.
want("ENDIAN", 0x11 | (0x22 << 8) | (0x33 << 16) | (0x44 << 24))
# An unaligned word and halfword out of a byte buffer, big-endian.
buf = bytes((i * 29 + 3) & 0xFF for i in range(16))
want("UNALIGNED", int.from_bytes(buf[1:5], "big"))
want("UNALIGNEDH", int.from_bytes(buf[7:9], "big"))
# Byte-reversed load of the same bytes.
want("LWBRX", int.from_bytes(buf[4:8], "little"))

want("FIB", fib(20))
ops = [lambda a: a + 3, lambda a: a * 5, lambda a: a ^ 0x5A5A5A5A, lambda a: a >> 2]
acc = VD
for i in range(12):
    acc = ops[i & 3](acc) & M32
want("FNPTR", acc)


def sw(i):
    return {0: 11, 1: 23, 2: 5, 3: 77, 4: 42, 5: 9, 6: 100, 7: 1}.get(i, 3)


want("SWITCH", sum(sw(i) * (i + 1) for i in range(10)))
want("VARARGS", 1 + 2 + 3 + 4 + 5 + 6 + 7 + 8 + 9 + 10 + 11 + 12)
# Collatz steps from 27: a loop with a data-dependent branch.
n, steps = 27, 0
while n != 1:
    n = n // 2 if n % 2 == 0 else 3 * n + 1
    steps += 1
want("COLLATZ", steps)

print("/* SPDX-License-Identifier: Apache-2.0 */")
print("/* Generated by alu_want.py; do not edit. */")
print("#ifndef PPC_ALU_WANT_H")
print("#define PPC_ALU_WANT_H")
for name, v in W:
    print("#define WANT_%s 0x%08Xu" % (name, v))
print("#endif")
