#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# check-thumb2-vfp.sh - Assemble the VFP encodings src/backend/thumb2/
# emits and diff them against the real assembler.
#
# The Thumb-2 backend cannot be exercised by any host suite, and this
# project has twice shipped an encoding that assembled as a *different
# instruction* rather than failing: a 16-bit CMP given r8, and a shift
# amount split imm3:imm2 the wrong way round. Neither faulted.
#
# Every VFP encoding splits its register number across two non-adjacent
# fields, which is the same trap again -- so the numbers below are the
# ones the emitters produce, checked against what `as` produces for the
# mnemonic they claim to be. It needs no board and no guest.
set -uo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
as=${ARM_AS:-arm-none-eabi-as}
od=${ARM_OBJDUMP:-arm-none-eabi-objdump}
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT

# The cases exercise both halves of every split field: S2 has its low bit
# in D/M rather than in the V field, and r6 exercises the core register.
cases=(
  "vmov s0, r0"       "vmov r0, s0"
  "vadd.f32 s0, s0, s1"  "vsub.f32 s0, s0, s1"
  "vmul.f32 s0, s0, s1"  "vdiv.f32 s0, s0, s1"
  "vsqrt.f32 s0, s0"  "vmrs r0, fpscr"  "vmsr fpscr, r0"
  "vmov s2, r6"       "vadd.f32 s2, s0, s1"
)
{ echo ".syntax unified"; echo ".thumb"; echo ".fpu fpv5-sp-d16"
  printf '%s\n' "${cases[@]}"; } > "$tmp/v.s"

if ! "$as" -mcpu=cortex-m7 -mthumb "$tmp/v.s" -o "$tmp/v.o" 2>"$tmp/err"; then
    echo "error: $as failed"; cat "$tmp/err"; exit 1
fi
"$od" -d "$tmp/v.o" | awk '/^ +[0-9a-f]+:/{print $2, $3}' > "$tmp/want"

python3 - "$tmp/want" "${cases[@]}" <<'PY'
import sys
want = [l.split() for l in open(sys.argv[1])]
cases = sys.argv[2:]
VD = lambda n: (n >> 1) & 0xF
D  = lambda n: n & 1
def vmov_core(sn, rt, to_core):
    return (0xEE00 | (0x10 if to_core else 0) | VD(sn),
            (rt << 12) | 0x0A10 | (D(sn) << 7))
def vfp3(hi, sub, sd, sn, sm):
    return (hi | (D(sn) << 7) | VD(sn),
            (VD(sd) << 12) | 0x0A00 | (D(sd) << 6) | (0x40 if sub else 0)
            | (D(sm) << 5) | VD(sm))
def vsqrt(sd, sm):
    return (0xEEB1 | (D(sd) << 6),
            (VD(sd) << 12) | 0x0AC0 | (D(sm) << 5) | VD(sm))
mine = [vmov_core(0,0,False), vmov_core(0,0,True),
        vfp3(0xEE30,False,0,0,1), vfp3(0xEE30,True,0,0,1),
        vfp3(0xEE20,False,0,0,1), vfp3(0xEE80,False,0,0,1),
        vsqrt(0,0), (0xEEF1,0x0A10), (0xEEE1,0x0A10),
        vmov_core(2,6,False), vfp3(0xEE30,False,2,0,1)]
bad = 0
for name, got, w in zip(cases, mine, want):
    ref = (int(w[0],16), int(w[1],16))
    ok = got == ref
    bad += not ok
    print("%-24s %04x %04x  %s" % (name, got[0], got[1],
                                   "ok" if ok else "MISMATCH want %04x %04x" % ref))
print("\n%d of %d differ" % (bad, len(mine)))
sys.exit(1 if bad else 0)
PY
