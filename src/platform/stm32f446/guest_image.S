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

#if !defined(RV32_GUEST_IMAGE_RO) || !defined(RV32_GUEST_IMAGE_RW)
#error "RV32_GUEST_IMAGE_RO and _RW must be defined (paths to the halves)"
#endif

/* .incbin needs a string literal, so the macro value is stringified. */
#define STR_(x) #x
#define STR(x)  STR_(x)

    .section .rodata.guest, "a"
    .balign 4

/*
 * The two halves, embedded consecutively, so rv_guest_image still names
 * the whole image and rv_guest_ro_size says where it splits.
 *
 * No .balign between them, deliberately. The guest link script aligns
 * __guest_ro_end to 4 so the read-only half is already a multiple of
 * four bytes; an alignment directive here would be a no-op in that case
 * and a silent corruption if it ever stopped being true, because it
 * would insert padding the guest's addresses do not account for.
 * Concatenated they are byte-identical to the unsplit .bin, which the
 * build checks.
 */
    .globl rv_guest_image
    .type rv_guest_image, %object
rv_guest_image:
    .incbin STR(RV32_GUEST_IMAGE_RO)
rv_guest_ro_end:
    .incbin STR(RV32_GUEST_IMAGE_RW)
    .balign 4
rv_guest_image_end:
    .size rv_guest_image, rv_guest_image_end - rv_guest_image

    .globl rv_guest_image_size
    .type rv_guest_image_size, %object
    .balign 4
rv_guest_image_size:
    .word rv_guest_image_end - rv_guest_image
    .size rv_guest_image_size, 4

/*
 * How much of it is read-only, and so can be executed straight out of
 * flash rather than copied into guest RAM. Zero-length .data is normal
 * -- two of the three guests here have one -- and then the whole image
 * is read-only.
 */
    .globl rv_guest_ro_size
    .type rv_guest_ro_size, %object
    .balign 4
rv_guest_ro_size:
    .word rv_guest_ro_end - rv_guest_image
    .size rv_guest_ro_size, 4
