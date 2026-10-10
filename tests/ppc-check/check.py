#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""check.py - the e200z7 decoder and disassembler, against binutils.

    tests/ppc-check/check.py [--dis build/x/ppc-dis] [--sets vle16,vle32,booke,random]
    tests/ppc-check/check.py --elf build/x/guest/ppc-coremark.elf ...

Two questions, asked of the same encodings:

  round trip   every encoding the decoder names is disassembled, the text
               is assembled again, and the result must *decode to the
               same fields*. Fields rather than bytes, because some
               instructions have more than one encoding -- an SCI8
               immediate can often be placed by more than one scale --
               and the assembler is entitled to pick its own.

  validity     whether the decoder calls an encoding an instruction must
               agree with objdump, except where the e200z759n3 manual
               says otherwise, and the exceptions are listed below with
               the reason for each. An encoding objdump names and the
               decoder does not is an instruction this core would refuse;
               the reverse is one it would run that binutils has never
               heard of. Both are reported.

With --elf the encodings are a compiled program's: every word of its
executable sections, which must all be instructions this core has. That
is the question the sweeps cannot ask -- not "is this encoding decoded
correctly" but "does the decoder have everything a compiler emits".

The 16-bit VLE space is checked exhaustively. The 32-bit spaces are swept
by primary opcode with the operand fields held at a few patterns, plus
random words. binutils is the encoder of record here because nothing in
this tree describes VLE otherwise -- see ppc_decode.c.
"""

import argparse
import collections
import os
import random
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))

AS = "powerpc64-linux-gnu-as"
OBJDUMP = "powerpc64-linux-gnu-objdump"
if subprocess.run(["sh", "-c", "command -v powerpc-linux-gnu-as"],
                  stdout=subprocess.DEVNULL).returncode == 0:
    AS, OBJDUMP = "powerpc-linux-gnu-as", "powerpc-linux-gnu-objdump"

# Assembler option sets, tried in order for each line until one accepts it.
#
# The X-forms at opcode 31 are the same bytes in either kind of page, so
# -many -- which knows the enhanced reservations and wait -- is a valid
# last resort for a VLE line too.
#
AS_OPTS = {
    "v": [["-mvle"], ["-mvle", "-me500"], ["-me500mc"], ["-many"]],
    "b": [["-me500mc"], ["-many"], ["-me500", "-mefs2"]],
}
#
# objdump's -M options select a processor, not a union: "e500" with
# "vle" drops isel, icbt, dcba and wrtee, which the plain "vle" table
# has. Each word is asked of every set and the first name wins.
#
DUMP_OPTS = {"v": ["-Mvle,e500,efs2,spe", "-Mvle,e500mc"],
             "b": ["-Mbooke,e500,efs2,spe", "-Me500mc"]}

#
# Instructions the e200z759n3 has from Freescale's EIS that objdump's
# tables for these options do not: the enhanced reservations (manual
# 3.13) and wait (3.12).
#
E200_EIS = {"lbarx", "lharx", "stbcx.", "sthcx.", "wait"}

# X-form integer loads and stores: the core ignores bit 31 (3.16.4).
XFORM_LDST = {"lwzx", "lwzux", "lbzx", "lbzux", "lhzx", "lhzux", "lhax",
              "lhaux", "stwx", "stwux", "stbx", "stbux", "sthx", "sthux",
              "lwbrx", "lhbrx", "stwbrx", "sthbrx"}
UPDATE_FORMS = {"lwzux", "lbzux", "lhzux", "lhaux", "stwux", "stbux",
                "sthux", "lwzu", "lbzu", "lhzu", "lhau", "stwu", "stbu",
                "sthu"}

#
# Where objdump and the e200z759n3 disagree, and why the decoder follows
# the core. Keyed on objdump's mnemonic.
#
NOT_ON_THIS_CORE = {
    # 3.1: no string instructions, no FPU, no DCR moves on this core
    "lswi", "lswx", "stswi", "stswx", "mfdcr", "mtdcr", "mfdcrx", "mtdcrx",
    "mfapidi",
    # EFPU2 is single precision; efd* is the double-precision unit
    "efscfd",
    # the MMU, which this model does not have; see docs/frontend/ppc.md
    "tlbre", "tlbwe", "tlbsx", "tlbivax", "tlbsync", "tlbia", "tlbie",
    "tlbld", "tlbli",
    # other cores'
    "eciwx", "ecowx", "mfrtcu", "mtdbatl", "mfdbatl",
    "dcbtls", "dcbtstls", "dcblc", "icbtls", "icblc", "dcbzl",
    "se_rfgi", "rfgi", "ehpriv", "msgsnd", "msgclr", "dcbtep", "dcbfep",
    "mfpmr", "mtpmr", "dni", "lbdx", "stbdx", "bblels", "bbelr",
    "evlddepx", "evstddepx", "mtsrin", "mfsrin",
    "stfiwx", "dnh", "dsn", "tlbilx",
    # e500mc's indexed-with-decoration forms
    "lhdx", "lwdx", "lddx", "sthdx", "stwdx", "stddx", "lfddx", "stfddx",
}


def not_on_this_core(name):
    # External PID loads and stores (e500mc): lwepx, dcbstep, icbiep ...
    return (prefix_match(name, NOT_ON_THIS_CORE) or name.endswith("epx")
            or name in ("dcbtstep", "dcbstep", "icbiep", "dcbzep"))


def reserved_here(name, w):
    """Reserved on this core where objdump decodes a field."""
    if name in RESERVED_HERE:
        return True
    # bclr/bcctr: Book E has no BH; bits 16:20 are reserved.
    return (w >> 26) == 19 and ((w >> 1) & 0x3FF) in (16, 528) and (w & 0xF800)


def classic_fp(w, mode):
    """Opcodes 48-55, 59 and 63, and the X-form FP loads and stores."""
    op = w >> 26
    if mode == "b" and op in (48, 49, 50, 51, 52, 53, 54, 55, 59, 63):
        return True
    return op == 31 and ((w >> 1) & 0x3FF) in (
        0x217, 0x237, 0x257, 0x277, 0x297, 0x2B7, 0x2D7, 0x2F7, 0x3D7)


def bo_has_z(bo):
    """Book E BO: 001zy, 011zy, 1z00y, 1z01y and 1z1zz carry z bits."""
    if bo & 0x10:
        return bool(bo & 0x08) or (bo & 0x04 and bo & 0x03)
    return bool(bo & 0x04) and bool(bo & 0x02)


def executed_invalid(mn, w, t):
    """The invalid forms 3.16 says this core executes; binutils rejects."""
    ops = re.findall(r"r(\d+)", t)
    if mn in UPDATE_FORMS and len(ops) >= 2 and (
            ops[1] == "0" or ops[0] == ops[1]):
        return True  # 3.16.1
    if mn in ("lmw", "e_lmw") and len(ops) >= 2 and (
            int(ops[1]) >= int(ops[0]) or ops[1] == "0"):
        return True  # 3.16.2
    if mn.startswith("bc") and (w >> 26) in (16, 19):
        bo = (w >> 21) & 31
        if bo_has_z(bo):
            return True  # 3.16.4: z bits ignored
        if mn.startswith("bcctr") and not (bo & 4):
            return True  # 3.16.3
    return False

#
# Fields the e200z759n3 manual makes reserved where objdump's masks do
# not look: 3.16.4 says a non-zero reserved field is an illegal
# instruction everywhere but bit 31 of the X-form loads and stores and
# the z bits of BO. Each entry is the field and the form it is in.
#
RESERVED_HERE = {
    "e_mcrf": "bits 9:10 and 14:20, as mcrf",
    "mcrf": "bits 9:10 and 14:20",
    "mbar": "MO other than 0, 1 or 2 is illegal on this core (3.5)",
    "efsabs": "rB", "efsnabs": "rB", "efsneg": "rB", "efssqrt": "rB",
    "efscmpgt": "bits 9:10", "efscmplt": "bits 9:10", "efscmpeq": "bits 9:10",
    "efststgt": "bits 9:10", "efststlt": "bits 9:10", "efststeq": "bits 9:10",
    "efscfui": "rA", "efscfsi": "rA, unless 4 (efscfh)", "efscfuf": "rA",
    "efscfsf": "rA", "efsctui": "rA", "efsctsi": "rA, unless 4 (efscth)",
    "efsctuf": "rA", "efsctsf": "rA", "efsctuiz": "rA", "efsctsiz": "rA",
    "e_cmph": "bits 9:10", "e_cmphl": "bits 9:10",
    "dcbf": "bits 6:10 (no L on this core)", "dcbst": "bits 6:10",
    "cmp": "L = 1 is the 64-bit compare", "cmpl": "L = 1 is the 64-bit compare",
    "cmpi": "L", "cmpli": "L", "cmpwi": "L", "cmplwi": "L",
    "sc": "bits 6:29 and 31: there is no LEV on this core",
    "mtmsr": "L, bit 15", "mfcr": "bit 11 is mfocrf, not on this core",
    "mtcrf": "bit 11 is mtocrf, not on this core",
}


def prefix_match(name, names):
    return any(name == n or name.startswith(n) for n in names)


def run(cmd, inp=None):
    r = subprocess.run(cmd, input=inp, stdout=subprocess.PIPE,
                       stderr=subprocess.PIPE, universal_newlines=True)
    return r


def dis(tool, items):
    """items: (word, len, mode, pc) -> [(text, fields)]"""
    inp = "".join("%08x %d %s %x\n" % it for it in items)
    out = run([tool], inp).stdout.splitlines()
    if len(out) != len(items):
        sys.exit("ppc-dis returned %d lines for %d" % (len(out), len(items)))
    return [tuple(l.split("\t", 1)) for l in out]


def relative(text, pc):
    m = re.search(r"0x([0-9a-f]{8})$", text)
    if m and re.match(r"(se_b|se_bl|se_bc|e_b|e_bl|e_bc|e_bcl|b|bl|bc|bcl)\s",
                      text):
        off = (int(m.group(1), 16) - pc) & 0xFFFFFFFF
        if off >= 0x80000000:
            off -= 1 << 32
        return text[:m.start()] + ("." + ("+%d" % off if off >= 0 else "%d" % off))
    return text


def assemble(lines, mode, tmp):
    """lines: [(pc, text, len, pc_dis)] -> {pc: bytes} for those that
    assembled. `pc_dis` is the address the text was disassembled at, which
    is what its branch targets are relative to."""
    got = {}
    todo = list(lines)
    for opts in AS_OPTS[mode]:
        if not todo:
            break
        src = os.path.join(tmp, "rt.s")
        obj = os.path.join(tmp, "rt.o")
        with open(src, "w") as f:
            f.write("\t.text\n")
            for pc, text, _, pd in todo:
                f.write("\t.org 0x%x\n\t%s\n" % (pc, relative(text, pd)))
        r = run([AS, "-a32", "-mbig", "-mregnames"] + opts + ["-o", obj, src])
        bad = set()
        for l in r.stderr.splitlines():
            m = re.match(r".*?:(\d+): Error", l)
            if m:
                k, is_insn = divmod(int(m.group(1)) - 2, 2)
                bad.add(k if is_insn else k - 1)
        if r.returncode != 0 and not bad:
            sys.exit("assembler failed:\n" + r.stderr[:2000])
        if bad:
            # Re-assemble without the rejected lines, so the rest land.
            keep = [x for i, x in enumerate(todo) if i not in bad]
            with open(src, "w") as f:
                f.write("\t.text\n")
                for pc, text, _, pd in keep:
                    f.write("\t.org 0x%x\n\t%s\n" % (pc, relative(text, pd)))
            r = run([AS, "-a32", "-mbig", "-mregnames"] + opts + ["-o", obj, src])
            if r.returncode != 0:
                sys.exit("assembler failed twice:\n" + r.stderr[:2000])
        else:
            keep = todo
        binf = os.path.join(tmp, "rt.bin")
        run(["powerpc64-linux-gnu-objcopy", "-O", "binary", "-j", ".text", obj,
             binf])
        data = open(binf, "rb").read()
        for pc, text, n, _ in keep:
            got[pc] = data[pc:pc + n]
        todo = [x for i, x in enumerate(todo) if i in bad]
    return got


def objdump_names(words, mode, tmp):
    """The mnemonic objdump gives each word, or None.

    Each word is followed by a filler instruction, and is found again by
    its *address* in objdump's listing: an encoding objdump does not know
    is printed as data that may swallow the filler, and counting lines
    instead of addresses desynchronises at the first one.
    """
    src = os.path.join(tmp, "od.s")
    obj = os.path.join(tmp, "od.o")
    addrs = []
    off = 4 if mode == "b" else 2
    with open(src, "w") as f:
        f.write("\t.text\n\t%s\n" % ("se_isync" if mode == "v" else "nop"))
        for w, n in words:
            addrs.append(off)
            if n == 2:
                f.write("\t.short 0x%04x, 0x4400\n" % (w >> 16))
                off += 4
            elif mode == "v":
                f.write("\t.short 0x%04x, 0x%04x, 0x4400\n" % (w >> 16, w & 0xFFFF))
                off += 6
            else:
                f.write("\t.long 0x%08x, 0x60000000\n" % w)
                off += 8
    asf = ["-mvle"] if mode == "v" else []
    run([AS, "-a32", "-mbig"] + asf + ["-o", obj, src])
    names = [None] * len(words)
    for opts in DUMP_OPTS[mode]:
        out = run([OBJDUMP, "-d", opts, obj]).stdout
        at = {}
        for l in out.splitlines():
            p = l.split("\t")
            m = re.match(r"\s*([0-9a-f]+):", p[0]) if len(p) >= 3 else None
            if m:
                at[int(m.group(1), 16)] = (p[1].replace(" ", ""), p[2].strip())
        for i, ((w, n), a) in enumerate(zip(words, addrs)):
            if names[i] is not None:
                continue
            b, t = at.get(a, ("", "."))
            want = ("%04x" % (w >> 16)) if n == 2 else ("%08x" % w)
            if b == want and not t.startswith("."):
                names[i] = t.split()[0]
    return names


def vle_len(w):
    return 4 if ((w >> 28) & 9) == 1 else 2


def gen(sets, seed):
    rng = random.Random(seed)
    out = collections.OrderedDict()
    if "vle16" in sets:
        out["vle16"] = [((h << 16), 2, "v") for h in range(65536)
                        if vle_len(h << 16) == 2]
    pats = [0x064, 0x3FF, 0x000, 0x0A5, 0x2C1]
    if "vle32" in sets:
        ws = []
        for op in (4, 6, 7, 12, 13, 14, 20, 21, 22, 23, 28, 29, 30, 31):
            for p in pats:
                for lo in range(0, 65536, 1 if op in (4, 6, 31) else 7):
                    ws.append(((op << 26) | (p << 16) | lo, 4, "v"))
        out["vle32"] = ws
    if "booke" in sets:
        ws = []
        for op in range(64):
            for p in pats:
                for lo in range(0, 65536, 1 if op in (4, 19, 31) else 13):
                    ws.append(((op << 26) | (p << 16) | lo, 4, "b"))
        out["booke"] = ws
    if "random" in sets:
        ws = []
        for _ in range(60000):
            w = rng.getrandbits(32)
            if vle_len(w) == 4:
                ws.append((w, 4, "v"))
            ws.append((w, 4, "b"))
        out["random"] = ws
    return out


SHF_EXECINSTR = 0x4
SHF_PPC_VLE = 0x10000000


def elf_words(path):
    """The distinct instruction words of a Book E ELF's code sections."""
    d = open(path, "rb").read()
    if d[:4] != b"\x7fELF" or d[4] != 1 or d[5] != 2:
        raise SystemExit("%s: not a big-endian ELF32" % path)
    shoff = int.from_bytes(d[32:36], "big")
    shentsize = int.from_bytes(d[46:48], "big")
    shnum = int.from_bytes(d[48:50], "big")
    seen = collections.OrderedDict()
    for i in range(shnum):
        sh = d[shoff + i * shentsize:shoff + (i + 1) * shentsize]
        flags = int.from_bytes(sh[8:12], "big")
        off = int.from_bytes(sh[16:20], "big")
        size = int.from_bytes(sh[20:24], "big")
        if not flags & SHF_EXECINSTR or int.from_bytes(sh[4:8], "big") != 1:
            continue
        if flags & SHF_PPC_VLE:
            raise SystemExit("%s: a VLE section; instruction boundaries "
                             "are not recoverable from the bytes" % path)
        for o in range(off, off + size - 3, 4):
            seen[int.from_bytes(d[o:o + 4], "big")] = True
    return [(w, 4, "b") for w in seen]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dis", default=os.path.join(ROOT, "build", "ppcf",
                                                  "ppc-dis"))
    ap.add_argument("--sets", default=None)
    ap.add_argument("--elf", nargs="*", default=[])
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("-v", "--verbose", type=int, default=6)
    a = ap.parse_args()
    failed = False
    if a.sets is None:
        a.sets = "" if a.elf else "vle16,vle32,booke,random"
    with tempfile.TemporaryDirectory() as tmp:
        for path in a.elf:
            items = elf_words(path)
            res = dis(a.dis, [(w, n, m, 0) for w, n, m in items])
            unknown = [w for (w, n, m), (t, f) in zip(items, res)
                       if t.startswith(".")]
            name = os.path.basename(path)
            if unknown:
                failed = True
                print("%s: %d of %d distinct words are not instructions "
                      "of this core:" % (name, len(unknown), len(items)))
                for w in unknown[:a.verbose]:
                    print("    %08x" % w)
            if not items:
                failed = True
                print("%s: no code found" % name)
            failed |= check(name, items, a.dis, tmp, a.verbose)
        for name, items in gen(a.sets.split(","), a.seed).items():
            for mode in ("v", "b"):
                sel = [it for it in items if it[2] == mode]
                if not sel:
                    continue
                failed |= check(name + "/" + mode, sel, a.dis, tmp, a.verbose)
    sys.exit(1 if failed else 0)


def check(label, items, tool, tmp, verbose):
    mode = items[0][2]
    rows = []
    # Each instruction at its own address, eight bytes apart.
    pcs = [8 * i for i in range(len(items))]
    res = dis(tool, [(w, n, m, pc) for (w, n, m), pc in zip(items, pcs)])
    named = [(i, t) for i, (t, f) in enumerate(res) if not t.startswith(".")]
    # Round trip, in chunks the assembler handles comfortably.
    rt_bad = collections.Counter()
    rt_ex = {}
    rejected = collections.Counter()
    rej_ex = {}
    for c in range(0, len(named), 40000):
        chunk = named[c:c + 40000]
        lines = [(8 * k, t, items[i][1], pcs[i]) for k, (i, t) in enumerate(chunk)]
        got = assemble(lines, mode, tmp)
        back = []
        for k, (i, t) in enumerate(chunk):
            b = got.get(8 * k)
            if b is None:
                mn = t.split()[0]
                if executed_invalid(mn, items[i][0], t):
                    continue
                rejected[mn] += 1
                rej_ex.setdefault(mn, "%08x  %s" % (items[i][0], t))
                continue
            w = int.from_bytes(b + bytes(4 - len(b)), "big") if len(b) == 4 or len(b) == 2 else 0
            if len(b) == 2:
                w = int.from_bytes(b, "big") << 16
            back.append((i, w, len(b), 8 * k))
        # At the original address, so a branch's target decodes alike.
        res2 = dis(tool, [(w, n, mode, pcs[i]) for i, w, n, pc in back])
        for (i, w, n, pc), (t2, f2) in zip(back, res2):
            if f2 != res[i][1] or n != items[i][1]:
                mn = res[i][0].split()[0]
                rt_bad[mn] += 1
                rt_ex.setdefault(mn, "%08x  %-28s -> %08x %s" % (
                    items[i][0], res[i][0], w, t2))
    # Validity against objdump.
    names = []
    for c in range(0, len(items), 100000):
        names += objdump_names([(w, n) for w, n, m in items[c:c + 100000]],
                               mode, tmp)
    only_od = collections.Counter()
    od_ex = {}
    only_us = collections.Counter()
    us_ex = {}
    for i, ((w, n, m), (t, f)) in enumerate(zip(items, res)):
        ours = not t.startswith(".") or f.startswith("spe")
        theirs = names[i]
        if theirs is not None and (not_on_this_core(theirs)
                                   or classic_fp(w, mode)):
            theirs = None
        if theirs is not None and theirs.startswith(("efd", "evfs", "ev", "brinc")):
            # SPE and the double-precision EFPU: the decoder marks SPE as
            # such and leaves efd* illegal; compare only "is it SPE".
            spe = theirs.startswith(("ev", "brinc"))
            if spe != f.startswith("spe") and not (theirs.startswith("efd") and not ours):
                only_od[theirs] += 1
                od_ex.setdefault(theirs, "%08x  objdump %s, ours %s" % (w, theirs, t))
            continue
        if theirs and not ours:
            if reserved_here(theirs, w):
                continue
            only_od[theirs] += 1
            od_ex.setdefault(theirs, "%08x  %s" % (w, theirs))
        elif ours and not theirs and not f.startswith("spe"):
            mn = t.split()[0]
            if mn in E200_EIS or mn in ("mfspr", "mtspr"):
                continue  # see E200_EIS; SPR numbers are checked at run time
            if mn in XFORM_LDST and (w & 1):
                continue  # 3.16.4
            if executed_invalid(mn, w, t):
                continue  # 3.16: invalid to binutils, executed here
            if mn in ("lwarx", "lbarx", "lharx") and (w & 1):
                continue  # 3.16.4
            only_us[mn] += 1
            us_ex.setdefault(mn, "%08x  %s" % (w, t))

    print("%-12s %7d encodings, %7d named: round trip %d wrong, %d rejected; "
          "objdump-only %d, ours-only %d" % (
              label, len(items), len(named), sum(rt_bad.values()),
              sum(rejected.values()), sum(only_od.values()),
              sum(only_us.values())))

    def show(title, cnt, ex):
        if not cnt:
            return
        print("  %s:" % title)
        for mn, k in cnt.most_common(40):
            print("    %-14s %6d   %s" % (mn, k, ex[mn]))

    show("round trip decodes differently", rt_bad, rt_ex)
    show("assembler rejected the text", rejected, rej_ex)
    show("objdump names, decoder refuses", only_od, od_ex)
    show("decoder names, objdump refuses", only_us, us_ex)
    return bool(rt_bad or rejected or only_od or only_us)


if __name__ == "__main__":
    main()
