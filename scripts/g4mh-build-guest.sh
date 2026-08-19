#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
#
# g4mh-build-guest.sh - compile a G4MH guest with Renesas CC-RH.
#
#   scripts/g4mh-build-guest.sh tests/guest/g4mh/barrier3.c [out.bin]
#
# CC-RH is the only compiler that emits G4MH, and it is not something a
# checkout can assume: it needs the `ccrh` image described in
# docs/renesas/Dockerfile. That is why the resulting .bin is committed
# beside its source rather than built by CMake -- the same arrangement
# guest.bin already has, and the reason tests/guest/ppc/ can build from
# source while this cannot.
#
# **Rebuild and commit the .bin when the .c changes.** Nothing checks
# that they agree, which is a real hazard: a stale binary passes every
# test while the source says something else. `--check` re-builds and
# diffs against the committed file without replacing it, which is what
# CI would run if this had any.

set -eu

IMAGE=${CCRH_IMAGE:-ccrh:latest}
CCRH_BIN=/usr/local/Renesas/CC-RH/V2.08.00/bin

check_only=0
if [ "${1:-}" = "--check" ]; then
    check_only=1
    shift
fi

src=${1:?usage: g4mh-build-guest.sh [--check] <source.c> [out.bin]}
out=${2:-${src%.c}.bin}

if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
    echo "g4mh-build-guest: no such image: $IMAGE" >&2
    echo "build it from docs/renesas/Dockerfile, or set CCRH_IMAGE" >&2
    exit 2
fi

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cp "$src" "$work/in.c"

#
# The vector table, linked first.
#
# The emulator starts executing at the load address and a flat binary
# carries no entry point, so whatever the linker happens to place first
# is what runs. CC-RH placed a static helper there and `main` landed at
# +0x5c, so the program began in the middle of puts_ and ran to the
# instruction cap. Passing --entry to the runner would fix that run and
# break on the next edit, since the offset moves with the code; a stub in
# its own section, named first in -start, cannot move.
#
# It is a *table* rather than a branch because a trap only reports if
# something catches it. RBASE resets to the load address, so these
# offsets are the architecture's handler addresses -- and a guest without
# them sends, say, a reserved instruction to load+0x60, which in a flat
# image is ordinary code twenty bytes further on. Execution carries on
# with the faulting instruction skipped and nothing written anywhere,
# which is worse than a hang because the guest keeps producing plausible
# output. That has cost this project three separate multi-session hunts,
# on two different frontends, and it is the reason every G4MH guest now
# carries this whether it wants it or not.
#
# Self-contained on purpose: it calls nothing in the guest, because it is
# shared by every guest and cannot depend on a symbol any one of them
# happens to define. So it writes to the NS16550 directly, which is the
# same console the C code uses.
#
# 32 slots of 16 bytes is 512, which is exactly RBASE's alignment.
#
cat > "$work/entry.asm" <<'ASM'
	.section .text_entry, text
	.public _entry
	.extern _main

; --- the table -----------------------------------------------------
; Each slot records *which slot it is* in r19 before branching. That
; costs 4 bytes of a 16-byte entry and is what makes the report able
; to tell a mis-mapped exception from a correctly mapped one: without
; it every FE cause prints identically, so a vector sent to the wrong
; handler reads exactly like a vector sent to the right one.
	.offset  0x000
_entry:
	jr   _main                  ; reset
; Renesas' board package puts a SYNCI here, citing technical update
; TN-RH8-B0183B/E: without it the lockstep checker core reads an
; uninitialised register. A no-op here, kept so the table is the
; shape a real guest has.
	synci
	.offset  0x010
	mov  0x010, r19
	jr   _fe                    ; SYSERR
	.offset  0x020
	mov  0x020, r19
	jr   _fe                    ; (reserved)
	.offset  0x030
	mov  0x030, r19
	jr   _fe                    ; FETRAP
	.offset  0x040
	mov  0x040, r19
	jr   _ei                    ; TRAP 0-15
	.offset  0x050
	mov  0x050, r19
	jr   _ei                    ; TRAP 16-31
	.offset  0x060
	mov  0x060, r19
	jr   _fe                    ; RIE
	.offset  0x070
	mov  0x070, r19
	jr   _fe                    ; FPE / FXE
	.offset  0x080
	mov  0x080, r19
	jr   _fe                    ; UCPOP
	.offset  0x090
	mov  0x090, r19
	jr   _fe                    ; MIP / MDP
	.offset  0x0A0
	mov  0x0A0, r19
	jr   _fe                    ; PIE
	.offset  0x0B0
	mov  0x0B0, r19
	jr   _fe                    ; (reserved: debug)
	.offset  0x0C0
	mov  0x0C0, r19
	jr   _fe                    ; MAE
	.offset  0x0D0
	mov  0x0D0, r19
	jr   _fe                    ; (reserved)
	.offset  0x0E0
	mov  0x0E0, r19
	jr   _fe                    ; FENMI
	.offset  0x0F0
	mov  0x0F0, r19
	jr   _fe                    ; FEINT
	.offset  0x100
	mov  0x100, r19
	jr   _ei                    ; EIINT priority 0
	.offset  0x110
	mov  0x110, r19
	jr   _ei                    ; EIINT priority 1
	.offset  0x120
	mov  0x120, r19
	jr   _ei                    ; EIINT priority 2
	.offset  0x130
	mov  0x130, r19
	jr   _ei                    ; EIINT priority 3
	.offset  0x140
	mov  0x140, r19
	jr   _ei                    ; EIINT priority 4
	.offset  0x150
	mov  0x150, r19
	jr   _ei                    ; EIINT priority 5
	.offset  0x160
	mov  0x160, r19
	jr   _ei                    ; EIINT priority 6
	.offset  0x170
	mov  0x170, r19
	jr   _ei                    ; EIINT priority 7
	.offset  0x180
	mov  0x180, r19
	jr   _ei                    ; EIINT priority 8
	.offset  0x190
	mov  0x190, r19
	jr   _ei                    ; EIINT priority 9
	.offset  0x1A0
	mov  0x1A0, r19
	jr   _ei                    ; EIINT priority 10
	.offset  0x1B0
	mov  0x1B0, r19
	jr   _ei                    ; EIINT priority 11
	.offset  0x1C0
	mov  0x1C0, r19
	jr   _ei                    ; EIINT priority 12
	.offset  0x1D0
	mov  0x1D0, r19
	jr   _ei                    ; EIINT priority 13
	.offset  0x1E0
	mov  0x1E0, r19
	jr   _ei                    ; EIINT priority 14
	.offset  0x1F0
	mov  0x1F0, r19
	jr   _ei                    ; EIINT priority 15  (16+ share this)
	.offset  0x200

; --- handlers ------------------------------------------------------
; FE and EI differ only in which pair of registers holds the cause and
; the return address; everything after that is common.
_fe:
	stsr 14, r6, 0              ; FEIC
	stsr 2,  r7, 0              ; FEPC
	jr   _report
_ei:
	stsr 13, r6, 0              ; EIIC
	stsr 0,  r7, 0              ; EIPC

; Print "!TRAP <cause> @<pc> #<slot>" and stop. Halting is the point: carrying on
; is what made these invisible.
_report:
	mov  0x10000000, r20        ; NS16550 transmit holding register
	mov  0x21, r8               ; '!'
	st.b r8, 0x00000000[r20]
	mov  0x54, r8               ; 'T'
	st.b r8, 0x00000000[r20]
	mov  0x52, r8               ; 'R'
	st.b r8, 0x00000000[r20]
	mov  0x41, r8               ; 'A'
	st.b r8, 0x00000000[r20]
	mov  0x50, r8               ; 'P'
	st.b r8, 0x00000000[r20]
	mov  0x20, r8               ; ' '
	st.b r8, 0x00000000[r20]
	mov  r6, r18
	jarl _hex, r31
	mov  0x20, r8               ; ' '
	st.b r8, 0x00000000[r20]
	mov  0x40, r8               ; '@'
	st.b r8, 0x00000000[r20]
	mov  r7, r18
	jarl _hex, r31
	mov  0x20, r8               ; ' '
	st.b r8, 0x00000000[r20]
	mov  0x23, r8               ; '#'
	st.b r8, 0x00000000[r20]
	mov  r19, r18
	jarl _hex, r31
	mov  0x0A, r8               ; '\n'
	st.b r8, 0x00000000[r20]
	halt

; r18 in, eight hex digits out, r20 the port. Clobbers r8, r9, r10.
_hex:
	mov  28, r9
_hex_loop:
	shr  r9, r18, r10
	andi 0x000F, r10, r10
	cmp  10, r10
	bge  _hex_af
	addi 0x0030, r10, r10       ; '0'
	br   _hex_out
_hex_af:
	addi 0x0037, r10, r10       ; 'A' - 10
_hex_out:
	st.b r10, 0x00000000[r20]
	add  -4, r9
	cmp  0, r9
	bge  _hex_loop
	jmp  [r31]
ASM

#
# -Xcpu=g4mh picks the core; -Osize keeps the image small. The section
# order in -start is what puts the stub at 0x80000000, which is the host
# runner's default --load address.
#
docker run --rm -v "$work":/w -w /w "$IMAGE" sh -c \
    "PATH=$CCRH_BIN:\$PATH; \
     asrh -Xcommon=rh850 -Xcpu=g4mh entry.asm && \
     ccrh -Xcommon=rh850 -Xcpu=g4mh -Osize -c in.c -oin.obj && \
     rlink entry.obj in.obj -entry=_entry -form=binary -output=out.bin \
        -start=.text_entry,.text,.const,.data/80000000" \
    >"$work/log" 2>&1 || {
    echo "g4mh-build-guest: CC-RH failed:" >&2
    cat "$work/log" >&2
    exit 1
}

if [ ! -f "$work/out.bin" ]; then
    echo "g4mh-build-guest: no output produced; CC-RH said:" >&2
    cat "$work/log" >&2
    exit 1
fi

if [ "$check_only" -eq 1 ]; then
    if cmp -s "$work/out.bin" "$out"; then
        echo "g4mh-build-guest: $out is up to date"
    else
        echo "g4mh-build-guest: $out differs from a fresh build of $src" >&2
        exit 1
    fi
else
    cp "$work/out.bin" "$out"
    echo "g4mh-build-guest: wrote $out ($(wc -c <"$out") bytes)"
fi
