; The FXU on PE0: a vector kernel over the harness's RAM (r5 points at
; a 16-aligned 32 bytes), its loads and stores, the pairing forms, the
; comparisons, a mask, a shuffle, a conversion and TRFSRV.
    .section ".text", text
    ldv.qw    0[r5], wr1
    ldv.qw    16[r5], wr2
    addf.s4   wr1, wr2, wr3
    mulf.s4   wr3, wr2, wr4
    fmaf.s4   wr1, wr2, wr4
    addsubxf.s4 wr4, wr1, wr5
    addrf.s4  wr5, wr4, wr6
    cmpf.s4   4, wr1, wr2, wr7
    cmovf.w4  wr7, wr5, wr6, wr8
    shflv.w4  0xE4B, wr8, wr4, wr9
    roundf.sw4 wr9, wr10
    cvtf.ws4  wr10, wr11
    ldv.w     5, 8[r5], wr11
    ldv.dw    2, 8[r5], wr12
    ldvz.h4   24[r5], wr13
    stv.qw    wr9, 32[r5]
    stv.dw    1, wr11, 48[r5]
    stvz.h4   wr13, 56[r5]
    stv.w     3, wr10, 60[r5]
    trfsrv.w4 5, wr7
    bz        .done
    add       1, r20
.done:
    halt
