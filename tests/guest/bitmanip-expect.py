#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Expectations for isatest's test_bitmanip, from the specification.

Written from the Zba/Zbb/Zbc/Zbs chapter's definitions, with Python's
unbounded integers -- no 32-bit shifts, no builtins shared with the C,
so that it can be wrong only independently of the emulator.

Prints the check() lines test_bitmanip is made of, section by section;
the comments between sections in isatest.c are written by hand. Run it
and diff against the file after changing a case here.
"""
M = 0xFFFFFFFF
def s32(v): v &= M; return v - (1 << 32) if v >> 31 else v
def bits(v): return [(v >> i) & 1 for i in range(32)]

def clmul64(a, b):
    r = 0
    for i in range(32):
        if (b >> i) & 1: r ^= a << i
    return r

F2 = {
 'sh1add': lambda a,b: (a*2 + b) & M,
 'sh2add': lambda a,b: (a*4 + b) & M,
 'sh3add': lambda a,b: (a*8 + b) & M,
 'andn':   lambda a,b: a & (b ^ M),
 'orn':    lambda a,b: a | (b ^ M),
 'xnor':   lambda a,b: (a ^ b) ^ M,
 'min':    lambda a,b: a if s32(a) < s32(b) else b,
 'max':    lambda a,b: a if s32(a) > s32(b) else b,
 'minu':   lambda a,b: min(a,b),
 'maxu':   lambda a,b: max(a,b),
 'rol':    lambda a,b: ((a << (b % 32)) | (a >> (32 - b % 32))) & M if b % 32 else a,
 'ror':    lambda a,b: ((a >> (b % 32)) | (a << (32 - b % 32))) & M if b % 32 else a,
 'clmul':  lambda a,b: clmul64(a,b) & M,
 'clmulh': lambda a,b: (clmul64(a,b) >> 32) & M,
 'clmulr': lambda a,b: (clmul64(a,b) >> 31) & M,
 'bset':   lambda a,b: a | (1 << (b % 32)),
 'bclr':   lambda a,b: a & ~(1 << (b % 32)) & M,
 'binv':   lambda a,b: a ^ (1 << (b % 32)),
 'bext':   lambda a,b: (a >> (b % 32)) & 1,
}
F1 = {
 'clz':    lambda a: next((i for i in range(32) if (a >> (31 - i)) & 1), 32),
 'ctz':    lambda a: next((i for i in range(32) if (a >> i) & 1), 32),
 'cpop':   lambda a: sum(bits(a)),
 'sext.b': lambda a: s32(((a & 0xFF) ^ 0x80) - 0x80) & M,
 'sext.h': lambda a: s32(((a & 0xFFFF) ^ 0x8000) - 0x8000) & M,
 'zext.h': lambda a: a % 65536,
 'orc.b':  lambda a: sum((0xFF << (8*i)) for i in range(4) if (a >> (8*i)) % 256),
 'rev8':   lambda a: int.from_bytes(a.to_bytes(4,'little'),'big'),
}
FI = {'rori': 'ror', 'bseti': 'bset', 'bclri': 'bclr', 'binvi': 'binv', 'bexti': 'bext'}

out = []
def h(v): return '0x%08Xu' % v
def emit(s): out.append(s)

def two(mn, cases):
    for a,b in cases:
        emit('    check("%s %08x,%08x", B2("%s", %s, %s), %s);' % (mn,a,b,mn,h(a),h(b),h(F2[mn](a,b))))
def one(mn, cases):
    for a in cases:
        emit('    check("%s %08x", B1("%s", %s), %s);' % (mn,a,mn,h(a),h(F1[mn](a))))
def imm(mn, cases):
    for a,n in cases:
        emit('    check("%s %08x,%d", BI("%s", %s, %d), %s);' % (mn,a,n,mn,h(a),n,h(F2[FI[mn]](a,n))))
def regs(name, seq, a, b, want):
    emit('    check("%s",\n          B_REGS("%s", %s, %s), %s);' % (name, seq.replace('\n','\\n'), h(a), h(b), h(want)))

sections = {}
def section(k):
    global out
    out = sections.setdefault(k, [])

section('zba')
two('sh1add', [(1,1),(0x80000001,0x7FFFFFFF),(0xFFFFFFFF,0xFFFFFFFF)])
two('sh2add', [(1,1),(0x40000001,3),(0xFFFFFFFF,0xFFFFFFFF)])
two('sh3add', [(1,1),(0x20000001,0xFFFFFFF8),(0xFFFFFFFF,0xFFFFFFFF)])
a,b = 0x00001000, 0x00000234
regs('sh2add rd=rs1', 'sh2add t0, t0, t1\nmv t2, t0', a, b, F2['sh2add'](a,b))
regs('sh2add rd=rs2', 'sh2add t1, t0, t1\nmv t2, t1', a, b, F2['sh2add'](a,b))
regs('sh2add rs1=rs2', 'sh2add t2, t1, t1', a, b, F2['sh2add'](b,b))
regs('sh3add rd=rs1=rs2', 'sh3add t1, t1, t1\nmv t2, t1', a, b, F2['sh3add'](b,b))
regs('sh1add rd=x0', 'sh1add x0, t0, t1\nadd t2, x0, x0', a, b, 0)
regs('sh1add rs1=x0', 'sh1add t2, x0, t1', a, b, b)
regs('sh1add rs2=x0', 'sh1add t2, t0, x0', a, b, (a*2)&M)

section('logic')
A,B = 0xFF00FF00, 0x0F0F0F0F
for mn in ('andn','orn','xnor'):
    two(mn, [(A,B),(B,A),(0,0),(M,0),(0,M)])
regs('andn rd=rs2', 'andn t1, t0, t1\nmv t2, t1', A, B, F2['andn'](A,B))
regs('andn rd=rs1', 'andn t0, t0, t1\nmv t2, t0', A, B, F2['andn'](A,B))
regs('andn rs1=rs2', 'andn t2, t1, t1', A, B, 0)
regs('orn rd=rs2', 'orn t1, t0, t1\nmv t2, t1', A, B, F2['orn'](A,B))
regs('orn rs1=rs2', 'orn t2, t1, t1', A, B, M)
regs('xnor rd=rs2', 'xnor t1, t0, t1\nmv t2, t1', A, B, F2['xnor'](A,B))
regs('xnor rs1=rs2', 'xnor t2, t1, t1', A, B, M)
regs('orn rd=x0', 'orn x0, t0, t1\nadd t2, x0, x0', A, B, 0)

section('count')
one('clz', [0, 1, 0x80000000, 0x00010000, 0x7FFFFFFF, M])
one('ctz', [0, 1, 0x80000000, 0x00010000, 0xFFFFFFFE, M])
one('cpop', [0, M, 0x80000001, 0x12345678, 0x00010000])
regs('clz rd=rs1', 'clz t0, t0\nmv t2, t0', 0x00010000, 0, F1['clz'](0x00010000))
regs('clz rd=rs1 zero', 'clz t0, t0\nmv t2, t0', 0, 0, 32)
regs('ctz rd=rs1 zero', 'ctz t0, t0\nmv t2, t0', 0, 0, 32)
regs('cpop rd=rs1', 'cpop t0, t0\nmv t2, t0', 0x12345678, 0, F1['cpop'](0x12345678))
regs('clz rs1=x0', 'clz t2, x0', 5, 5, 32)
regs('cpop rd=x0', 'cpop x0, t0\nadd t2, x0, x0', M, 0, 0)

section('extend')
one('sext.b', [0x0000007F, 0x00000080, 0x12345680, 0xFFFFFF7F, 0])
one('sext.h', [0x00007FFF, 0x00008000, 0x12348000, 0xFFFF7FFF, 0x00000080])
one('zext.h', [M, 0x12348000, 0xFFFF0000, 0x0000FFFF])
regs('sext.b rd=rs1', 'sext.b t0, t0\nmv t2, t0', 0x12345680, 0, F1['sext.b'](0x12345680))
regs('zext.h rd=rs1', 'zext.h t0, t0\nmv t2, t0', M, 0, 0xFFFF)

section('minmax')
pairs = [(0x80000000,0x7FFFFFFF),(0x7FFFFFFF,0x80000000),(M,1),(1,M),(5,5),(0,M)]
for mn in ('min','max','minu','maxu'):
    two(mn, pairs)
a,b = 0x80000000, 0x7FFFFFFF
regs('min rd=rs1', 'min t0, t0, t1\nmv t2, t0', a, b, F2['min'](a,b))
regs('maxu rd=rs2', 'maxu t1, t0, t1\nmv t2, t1', a, b, F2['maxu'](a,b))
regs('max rs1=rs2', 'max t2, t1, t1', a, b, b)
regs('min rd=x0', 'min x0, t0, t1\nadd t2, x0, x0', a, b, 0)
regs('minu rs1=x0', 'minu t2, x0, t1', a, b, 0)
regs('maxu rs2=x0', 'maxu t2, t0, x0', a, b, a)

section('rotate')
X = 0x12345678
for mn in ('rol','ror'):
    two(mn, [(0x80000001,1),(X,0),(X,32),(X,36),(X,31),(X,0xFFFFFFE4),(M,7)])
imm('rori', [(X,0),(X,4),(X,31),(0x80000001,1),(X,16)])
regs('rol rd=rs1', 'rol t0, t0, t1\nmv t2, t0', X, 4, F2['rol'](X,4))
regs('ror rd=rs2', 'ror t1, t0, t1\nmv t2, t1', X, 4, F2['ror'](X,4))
regs('rol rs1=rs2', 'rol t2, t1, t1', 0, 0x00000105, F2['rol'](0x105,0x105))
regs('rori rd=rs1', 'rori t0, t0, 12\nmv t2, t0', X, 0, F2['ror'](X,12))
one('orc.b', [0, 0x00010000, 0x80000001, 0x01010101, 0x00FF0100, M])
one('rev8', [X, 0x000000FF, 0xFF000000, 0x00FF0000])
regs('orc.b rd=rs1', 'orc.b t0, t0\nmv t2, t0', 0x00010000, 0, 0x00FF0000)
regs('rev8 rd=rs1', 'rev8 t0, t0\nmv t2, t0', X, 0, F1['rev8'](X))

section('clmul')
cl = [(M,M),(0x80000000,2),(3,3),(X,0),(X,1),(1,X),(X,0x9ABCDEF0),(0x80000000,0x80000000),(M,0x80000000)]
for mn in ('clmul','clmulh','clmulr'):
    two(mn, cl)
a,b = X, 0x9ABCDEF0
regs('clmul rd=rs1', 'clmul t0, t0, t1\nmv t2, t0', a, b, F2['clmul'](a,b))
regs('clmulh rd=rs2', 'clmulh t1, t0, t1\nmv t2, t1', a, b, F2['clmulh'](a,b))
regs('clmulr rs1=rs2', 'clmulr t2, t1, t1', a, b, F2['clmulr'](b,b))
regs('clmul rd=x0', 'clmul x0, t0, t1\nadd t2, x0, x0', a, b, 0)

section('bits')
two('bset', [(0,31),(0,32),(0,0xFFFFFFE4),(M,5),(0x80000000,0)])
two('bclr', [(M,31),(M,32),(M,0xFFFFFFE4),(0,3),(1,0)])
two('binv', [(0,31),(M,32),(M,31),(0,0xFFFFFFE4),(0x80000000,31)])
two('bext', [(0x80000000,31),(0x80000000,63),(0x7FFFFFFF,31),(1,32),(0xFFFFFFFE,0),(0x00010000,16),(0x00010000,15)])
imm('bseti', [(0,31),(0,0),(M,7),(0,16)])
imm('bclri', [(M,31),(M,0),(0,7),(M,16)])
imm('binvi', [(0x80000000,31),(0,0),(M,31),(0,16)])
imm('bexti', [(0x80000000,31),(0x80000000,30),(1,0),(0xFFFFFFFE,0),(0x00010000,16),(0x00010000,17)])
regs('bset rd=rs2', 'bset t1, t0, t1\nmv t2, t1', 0x100, 5, 0x120)
regs('bclr rd=rs1', 'bclr t0, t0, t1\nmv t2, t0', M, 5, M ^ 0x20)
regs('bext rd=rs2', 'bext t1, t0, t1\nmv t2, t1', 0x20, 5, 1)
regs('binv rs1=rs2', 'binv t2, t1, t1', 0, 5, 5 ^ 0x20)
regs('bset rs1=rs2', 'bset t2, t1, t1', 0, 0x24, 0x24 | 0x10)
regs('bext rs1=rs2', 'bext t2, t1, t1', 0, 0x24, (0x24 >> 4) & 1)
regs('bseti rd=x0', 'bseti x0, t0, 3\nadd t2, x0, x0', 0, 0, 0)
regs('bexti rd=rs1', 'bexti t0, t0, 31\nmv t2, t0', 0x80000000, 0, 1)
regs('bset rs1=x0', 'bset t2, x0, t1', 7, 33, 2)

section('traffic')
a,b = 0x00000F0F, 0x00000033
# The value a called operation reads was written by the instruction
# before it, and the one it writes is read by the instruction after.
regs('helper reads prior write', 'addi t0, t0, 1\nclmul t2, t0, t1', a, b, F2['clmul'](a+1,b))
regs('helper result read back', 'li t2, 7\nclmul t2, t0, t1\naddi t2, t2, 1', a, b, (F2['clmul'](a,b)+1)&M)
regs('helper overwrites operand', 'min t0, t0, t1\nadd t2, t0, t0', a, b, 2*min(a,b))
regs('native after helper', 'cpop t0, t0\nsh2add t2, t0, t1', a, b, F2['sh2add'](F1['cpop'](a),b))
regs('helper after native', 'sh2add t0, t0, t1\ncpop t2, t0', a, b, F1['cpop'](F2['sh2add'](a,b)))
regs('two helpers', 'rol t0, t0, t1\norc.b t2, t0', a, b, F1['orc.b'](F2['rol'](a,b)))

import sys
# spot values worked by hand, so the model is itself checked
assert F2['clmul'](M,M) == 0x55555555 and F2['clmulh'](M,M) == 0x55555555 and F2['clmulr'](M,M) == 0xAAAAAAAA
assert F2['clmul'](3,3) == 5 and F2['clmulh'](0x80000000,2) == 1 and F2['clmulr'](0x80000000,2) == 2
assert F2['rol'](X,36) == 0x23456781 and F2['ror'](X,36) == 0x81234567 and F2['ror'](X,31) == 0x2468ACF0
assert F1['cpop'](X) == 13 and F1['orc.b'](0x00FF0100) == 0x00FFFF00 and F2['xnor'](A,B) == 0x0FF00FF0
assert F2['andn'](A,B) == 0xF000F000 and F2['orn'](A,B) == 0xFFF0FFF0 and F1['sext.h'](0x12348000) == 0xFFFF8000
for k, v in sections.items():
    print('    /* --- %s --- */' % k)
    print('\n'.join(v))
print('/* %d checks */' % sum(len(v) for v in sections.values()), file=sys.stderr)
