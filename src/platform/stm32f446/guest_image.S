/* SPDX-License-Identifier: Apache-2.0 */
/*
 * guest_image.S - Embed the RISC-V guest binary into ARM flash.
 *
 * .incbin places the raw bytes directly in .rodata with no size overhead:
 * no ELF headers, no generated C array (which would cost roughly 6x the
 * image size in source and slow the build), and no runtime parser. The
 * emulator points a bus region straight at these symbols.
 *
 * RV32_GUEST_IMAGE is set by the build to the path of a flat .bin.
 */

#ifndef RV32_GUEST_IMAGE
#error "RV32_GUEST_IMAGE must be defined (path to the guest .bin)"
#endif

/* .incbin needs a string literal, so the macro value is stringified. */
#define STR_(x) #x
#define STR(x)  STR_(x)

    .section .rodata.guest, "a"
    .balign 4

    .globl rv_guest_image
    .type rv_guest_image, %object
rv_guest_image:
    .incbin STR(RV32_GUEST_IMAGE)
    .balign 4
rv_guest_image_end:
    .size rv_guest_image, rv_guest_image_end - rv_guest_image

    .globl rv_guest_image_size
    .type rv_guest_image_size, %object
    .balign 4
rv_guest_image_size:
    .word rv_guest_image_end - rv_guest_image
    .size rv_guest_image_size, 4
