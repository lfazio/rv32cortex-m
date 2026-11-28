#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
#
# gdbtrace-mcp.sh - GdbTrace's MCP server (tools/gdbtrace, the submodule)
# for an agent working in this tree: the paths its tools take -- an ELF,
# an OpenOCD configuration, an SVD -- are relative to this tree's root.
#
#   claude mcp add gdbtrace -e STLINK_SERIAL=<serial> -- scripts/gdbtrace-mcp.sh
#   scripts/gdbtrace-mcp.sh --check     # the environment it would run in
#
# Its virtualenv is made on first use, as scripts/gdbtrace-f746.sh makes
# it. STLINK_SERIAL picks an ST-LINK when several are attached (EMU_PROBE's
# third field); the F746's configuration is tools/gdbtrace/configs/
# nucleo-f746zg.cfg. Nothing is written to stdout but the protocol.
#
# Environment:
#   GDBTRACE   the gdbtrace checkout (default: tools/gdbtrace)

set -eu

ROOT=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
GT=${GDBTRACE:-$ROOT/tools/gdbtrace}
if [ ! -f "$GT/gdbtrace/mcp.py" ]; then
    echo "gdbtrace-mcp: no gdbtrace at $GT: git submodule update --init --recursive tools/gdbtrace, or set GDBTRACE" >&2
    exit 2
fi
PY=$GT/.venv/bin/python
if [ ! -x "$PY" ]; then
    echo "gdbtrace-mcp: making gdbtrace's virtualenv in $GT/.venv" >&2
    python3 -m venv "$GT/.venv" >&2 && "$PY" -m pip install -q -e "$GT[dev]" >&2 || {
        echo "gdbtrace-mcp: could not make $GT/.venv" >&2
        exit 1
    }
fi
exec "$PY" -m gdbtrace.mcp --root "$ROOT" "$@"
