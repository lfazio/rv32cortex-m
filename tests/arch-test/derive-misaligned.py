#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
#
# derive-misaligned.py - the DUT description of an RV32_MISALIGNED=ON build.
#
#   derive-misaligned.py SRC_DIR DST_DIR
#
# rv32cortex-m-rv32/ describes the core as the arch-test runner builds it,
# RV32_MISALIGNED=OFF: a misaligned load or store raises. The emulator's
# *default* is the other one -- picolibc's word-at-a-time strcmp reads lw
# straight off an odd pointer -- and that build was described by nothing,
# so the four Misalign suites (which require MISALIGNED_LDST true) could
# not be offered and the default build was never run against the suite.
#
# Derived rather than copied: the two descriptions differ in one property
# and agree in every other, and two hand-kept copies of an 18 KB Sail
# config would stop agreeing the first time one of them was edited. Every
# substitution asserts how many times it matched, so an edit to the base
# that moves a line this script relies on fails here rather than producing
# a description that silently still says "raises".
#
# What changes, and why each value:
#
#   MISALIGNED_LDST true      loads and stores proceed (rv_hart_load/store
#                             under RV_MISALIGNED_OK). Sail: load_store None,
#                             globally and in every region -- the bus passes
#                             a misaligned access to a device as it is, and
#                             never raises for one.
#   GRANULE_SIZE 0            a misaligned access is not promised atomic:
#                             mem_wide splits it into naturally aligned bus
#                             transactions, and per page under Sv32.
#   SPLIT_STRATEGY            sequential_bytes, the one UDB defines: every
#                             part is checked before any is performed, and a
#                             fault names the lowest part refused.
#   PRIORITY high (kept)      AMOs and LR/SC still raise misaligned, and
#                             check alignment before anything else
#                             (rv_hart_amo). UDB allows high only with a
#                             zero granule, which is the other reason it is
#                             zero.

import pathlib
import re
import sys

BASE = "rv32cortex-m-rv32"
NAME = "rv32cortex-m-rv32-misaligned"


def sub(text, old, new, count, what):
    n = text.count(old)
    if n != count:
        sys.exit(f"derive-misaligned: {what}: expected {count} match(es), "
                 f"found {n} -- the base description has moved")
    return text.replace(old, new)


def main():
    if len(sys.argv) != 3:
        sys.exit("usage: derive-misaligned.py SRC_DIR DST_DIR")
    src = pathlib.Path(sys.argv[1])
    dst = pathlib.Path(sys.argv[2])
    dst.mkdir(parents=True, exist_ok=True)

    note = ("# DERIVED by tests/arch-test/derive-misaligned.py from "
            f"{BASE}/ -- edit the base, not this.\n"
            "# Comments below that say misaligned accesses raise describe "
            "the base;\n# the values are this build's.\n")

    # test_config.yaml: its own name, and the UDB file under its new name.
    cfg = (src / "test_config.yaml").read_text()
    cfg = sub(cfg, f"name: {BASE}\n", f"name: {NAME}\n", 1, "test name")
    cfg = sub(cfg, f"udb_config: {BASE}.yaml\n", f"udb_config: {NAME}.yaml\n",
              1, "udb_config")
    (dst / "test_config.yaml").write_text(note + cfg)

    # UDB.
    udb = (src / f"{BASE}.yaml").read_text()
    udb = sub(udb, "\nname: rv32cortex_m_rv32\n",
              "\nname: rv32cortex_m_rv32_misaligned\n", 1, "UDB name")
    udb = sub(udb, "\n  MISALIGNED_LDST: false\n",
              "\n  MISALIGNED_LDST: true\n"
              "  MISALIGNED_MAX_ATOMICITY_GRANULE_SIZE: 0\n"
              "  MISALIGNED_SPLIT_STRATEGY: sequential_bytes\n",
              1, "MISALIGNED_LDST")
    udb = sub(udb, "\n  MISALIGNED_LDST_EXCEPTION_PRIORITY: high\n",
              "\n  MISALIGNED_LDST_EXCEPTION_PRIORITY: high\n", 1,
              "priority (kept high: AMOs check alignment first)")
    udb = sub(udb, "\n  MISALIGNED_AMO: false\n", "\n  MISALIGNED_AMO: false\n",
              1, "MISALIGNED_AMO (kept false)")
    (dst / f"{NAME}.yaml").write_text(note + udb)

    # Sail. Every scalar load/store entry, the global one and each region's,
    # and nothing else: lrsc and amo keep AlignmentException.
    sail = (src / "sail.json").read_text()
    raising = re.compile(
        r'"load_store":\s*\{\s*"Some":\s*"AlignmentException"\s*\}')
    regions = sail.count('"misaligned_exceptions":')
    sail, n = raising.subn('"load_store": { "None": null }', sail)
    if n != regions + 1:
        sys.exit(f"derive-misaligned: sail load_store: {n} entries for "
                 f"{regions} regions plus the global one")
    if '"lrsc": "AlignmentException"' not in sail:
        sys.exit("derive-misaligned: sail lrsc no longer AlignmentException")
    (dst / "sail.json").write_text(
        "".join("//" + line[1:] + "\n" for line in note.splitlines()) + sail)

    # The rest is the same core.
    for f in ("link.ld", "rvmodel_macros.h"):
        (dst / f).write_bytes((src / f).read_bytes())


if __name__ == "__main__":
    main()
