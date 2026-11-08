/*
 * A store that rewrites an instruction further on in its own block, for
 * CFG_SMC (fuzz.sh runs every seed under it too): code in RAM, r0 at the
 * program's start. The interpreter executes the new movs, r1 = 0; a
 * block translated before the store holds the old one, r1 = 5.
 */
    .syntax unified
    .thumb
    movs    r6, #0x21
    lsls    r6, r6, #8          /* 0x2100: movs r1, #0 */
    strh    r6, [r0, #8]
    nop
    movs    r1, #5              /* at +8: rewritten to movs r1, #0 */
    bkpt    #0
