/* SPDX-License-Identifier: Apache-2.0 */
/* corpus.S - the packed corpus (pack.py), found on the include path. */
    .section .rodata.ir_corpus, "a"
    .global ir_corpus
    .balign 4
ir_corpus:
    .incbin "ir-corpus.bin"
