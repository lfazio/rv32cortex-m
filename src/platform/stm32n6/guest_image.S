/* SPDX-License-Identifier: Apache-2.0 */
/*
 * guest_image.S - embed the guest binary in the host part's flash.
 *
 * .incbin places the raw bytes in .rodata with no size overhead: no ELF
 * headers, no generated C array (roughly 6x the image size in source, and
 * slow to compile), and no run-time parser. The emulator points a bus
 * region straight at these symbols, so the guest's .text and .rodata are
 * executed out of the host's flash and cost the guest no RAM.
 *
 * **One image, embedded whole.** It used to be two -- the same binary
 * split at __guest_ro_end and embedded consecutively -- because the guest
 * was linked entirely in RAM and the firmware needed to know how much of
 * it was read-only in order to serve that much from flash. The guest
 * links .text and .rodata into flash and .data into RAM now, and copies
 * .data itself at start-up, so there is no boundary for anything here to
 * carry.
 *
 * That split was worth removing rather than merely unnecessary. The
 * halves had to concatenate back to the unsplit binary exactly, an
 * invariant this file asserted in a comment and the build did not check;
 * it was false for two of the four guests, because the `. = ALIGN(4)`
 * that set the boundary sat *between* output sections and so moved the
 * location counter without padding .rodata. Three bytes went missing,
 * .data landed below where the guest read it, and the guest ran anyway,
 * on zeros.
 *
 * EMU_GUEST_IMAGE is set by the build to the path of a flat .bin.
 */

#if !defined(EMU_GUEST_IMAGE)
#error "EMU_GUEST_IMAGE must be defined (path to the guest .bin)"
#endif

/* .incbin needs a string literal, so the macro value is stringified. */
#define STR_(x) #x
#define STR(x)  STR_(x)

    .section .rodata.guest, "a"
    .balign 4

    .globl emu_guest_image
    .type emu_guest_image, %object
emu_guest_image:
    .incbin STR(EMU_GUEST_IMAGE)
    .balign 4
emu_guest_image_end:
    .size emu_guest_image, emu_guest_image_end - emu_guest_image

    .globl emu_guest_image_size
    .type emu_guest_image_size, %object
    .balign 4
emu_guest_image_size:
    .word emu_guest_image_end - emu_guest_image
    .size emu_guest_image_size, 4
