/*
 * A widening sum from e32, whose result would be 64 bits -- past ELEN
 * at Zve32x, so reserved: the translator must leave it to the helper's
 * illegal instruction, after a lowered one at e16 in the same block.
 */
    vsetivli zero, 7, e16, m1, ta, ma
    vwredsum.vs v1, v2, v3
    vsetivli zero, 3, e32, m1, ta, ma
    vwredsumu.vs v4, v5, v6              /* from e32: reserved */
    ecall
