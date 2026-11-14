#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
sm4-affine.py - SM4's S-box as AES's between two affine maps, for a host
that has AESENCLAST but no S-box of SM4's (x86-64's lowering of
EMU_IR_SM4SB, src/backend/x86_64/ir_lower.c).

    scripts/sm4-affine.py            # print emu_sm4_affine, its four tables
    scripts/sm4-affine.py --check    # compare it with emu_crypto.c's
    scripts/sm4-affine.py --search   # find SM4's form from the table again

Derived from the definitions, and nothing taken from anyone's code:

  - SM4's S-box is S(x) = A(I(A x + C)) + C, I the inverse in GF(2^8)
    modulo x^8+x^7+x^6+x^5+x^4+x^2+1, A the circulant map summing
    left rotations by 0, 1, 3, 6 and 7 (0xCB; 0xA7 in right rotations),
    C 0xD3. Searched for, not assumed (--search): of the 255 circulant
    maps and 256 constants, this is the only one that reproduces the
    S-box table in src/emu/emu_crypto.c, which is the published table
    (and Spike's, BSD).
  - T, an isomorphism from that field to AES's (modulo 0x11B), maps x^i
    to beta^i for a root beta of SM4's polynomial in AES's field; then
    I(z) = T^-1(inv(T z)), and inv(y) = L^-1(AESSbox(y) + 0x63), L the
    AES affine map's linear part.
  - So S(x) = M2 (AESSbox(M1 x + c1)) + c2, with M1 = T A, c1 = T C,
    M2 = A T^-1 L^-1, c2 = M2 0x63 + C -- checked on all 256 inputs.

Each affine map is applied as two 16-entry lookups, a byte's low and
high nibbles, XORed: what PSHUFB does without a memory access, so the
lowering stays in time independent of the data (Zkt).
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CRYPTO = os.path.join(ROOT, "src", "emu", "emu_crypto.c")


def gmul(a, b, poly):
    r = 0
    while b:
        if b & 1:
            r ^= a
        b >>= 1
        a <<= 1
        if a & 0x100:
            a ^= poly
    return r


def inverse_table(poly):
    t = [0] * 256
    for x in range(1, 256):
        for y in range(1, 256):
            if gmul(x, y, poly) == 1:
                t[x] = y
                break
    return t


def rotl(b, k):
    return ((b << k) | (b >> (8 - k))) & 0xFF


def circulant(pattern):
    """The linear map summing left rotations by the pattern's bits."""
    return [_circ(b, pattern) for b in range(256)]


def _circ(b, pattern):
    v = 0
    for k in range(8):
        if pattern >> k & 1:
            v ^= rotl(b, k)
    return v


def compose(f, g):
    return [f[g[x]] for x in range(256)]


def invert(f):
    t = [0] * 256
    for x in range(256):
        t[f[x]] = x
    assert sorted(t) == list(range(256)), "not a bijection"
    return t


def sm4_table():
    src = open(CRYPTO).read()
    i = src.index("k_sm4_sbox[256] = {")
    j = src.index("};", i)
    t = [int(x, 16) for x in re.findall(r"0x([0-9a-f]{2})", src[i:j])]
    assert len(t) == 256, "the SM4 table in emu_crypto.c"
    return t


def aes_sbox():
    inv = inverse_table(0x11B)
    lin = circulant(0x1F)  # b + rotl 1..4: AES's affine map, less 0x63
    return [lin[inv[x]] ^ 0x63 for x in range(256)], lin


def derive():
    sm4 = sm4_table()
    invf = inverse_table(0x1F5)
    a = circulant(0xCB)
    c = 0xD3
    assert all(a[invf[a[x] ^ c]] ^ c == sm4[x] for x in range(256)), \
        "SM4's algebraic form does not give the table"

    # A root of SM4's polynomial in AES's field, and T from its powers.
    beta = next(b for b in range(2, 256)
                if _poly_at(b) == 0)
    powers = [1]
    for _ in range(7):
        powers.append(gmul(powers[-1], beta, 0x11B))
    t = [0] * 256
    for x in range(256):
        v = 0
        for i in range(8):
            if x >> i & 1:
                v ^= powers[i]
        t[x] = v
    tinv = invert(t)
    inv = inverse_table(0x11B)
    assert all(tinv[inv[t[z]]] == invf[z] for z in range(256)), \
        "T is not an isomorphism"

    aes, lin = aes_sbox()
    linv = invert(lin)
    m1 = compose(t, a)
    c1 = t[c]
    m2 = compose(a, compose(tinv, linv))
    c2 = m2[0x63] ^ c
    pre = [m1[x] ^ c1 for x in range(256)]
    post = [m2[y] ^ c2 for y in range(256)]
    assert all(post[aes[pre[x]]] == sm4[x] for x in range(256)), \
        "the composition is not SM4's S-box"
    return m1, c1, m2, c2


def _poly_at(b):
    # x^8 + x^7 + x^6 + x^5 + x^4 + x^2 + 1 at b, in AES's field.
    p = [1]
    for _ in range(8):
        p.append(gmul(p[-1], b, 0x11B))
    return p[8] ^ p[7] ^ p[6] ^ p[5] ^ p[4] ^ p[2] ^ p[0]


def search():
    """Every circulant map (left rotations, as a pattern of bits) and
    constant C with S(x) = A(I(A x + C)) + C equal to the table."""
    sm4 = sm4_table()
    invf = inverse_table(0x1F5)
    found = []
    for pattern in range(1, 256):
        a = circulant(pattern)
        for c in range(256):
            if a[invf[c]] ^ c == sm4[0] and \
                    all(a[invf[a[x] ^ c]] ^ c == sm4[x] for x in range(256)):
                found.append((pattern, c))
    return found


def table():
    """The four maps' nibble tables, one after another, as emu_crypto.c's
    emu_sm4_affine holds them: pre low, pre high, post low, post high."""
    m1, c1, m2, c2 = derive()
    return ([m1[n] ^ c1 for n in range(16)] + [m1[n << 4] for n in range(16)] +
            [m2[n] ^ c2 for n in range(16)] + [m2[n << 4] for n in range(16)])


NAMES = ("pre, low nibble", "pre, high nibble", "post, low nibble",
         "post, high nibble")


def as_c(v):
    out = ["_Alignas(16) const uint8_t emu_sm4_affine[64] = {"]
    for k in range(4):
        row = ", ".join("0x%02x" % x for x in v[16 * k:16 * k + 16])
        out.append("    /* %s */" % NAMES[k])
        out.append("    " + row[:47] + "\n    " + row[48:] + ",")
    out.append("};")
    return "\n".join(out)


def main():
    if "--search" in sys.argv[1:]:
        for pattern, c in search():
            print("sm4-affine: circulant 0x%02x, constant 0x%02x" % (pattern, c))
        return 0
    v = table()
    if "--check" in sys.argv[1:]:
        src = open(CRYPTO).read()
        i = src.find("emu_sm4_affine[64] = {")
        got = []
        if i >= 0:
            j = src.index("};", i)
            got = [int(x, 16) for x in re.findall(r"0x([0-9a-f]{2})", src[i:j])]
        ok = got == v
        print("sm4-affine: %s" % ("emu_sm4_affine matches" if ok else
                                  "emu_sm4_affine in emu_crypto.c is not what the "
                                  "definitions give:\n" + as_c(v)))
        return 0 if ok else 1
    print(as_c(v))
    return 0


if __name__ == "__main__":
    sys.exit(main())
