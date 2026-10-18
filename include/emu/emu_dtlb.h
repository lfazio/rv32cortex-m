/* SPDX-License-Identifier: Apache-2.0 */
/*
 * emu_dtlb.h - a data TLB that answers in host addresses.
 *
 * **Every guest load and store below M-mode was a C call costing ~190
 * host instructions**, because the inlined RAM window is only legal
 * while nothing can make an access mean more than a move -- and paging
 * and PMP both can. Profiling sv32bench's paged build under the JIT:
 * rv_pmp_check 31.6% of host instructions, rv_mmu_translate 19.5%, the
 * load and store helpers themselves 36.5%, translated code 10%.
 *
 * This is the cache in front of all of it. An entry says: *at this
 * virtual page, for the privilege whose table this is, a load (and
 * perhaps a store) of any aligned width is a plain host access at
 * `va + addend`*. The frontend fills it only when that is true of the
 * whole page and will stay true until something the frontend already
 * sees invalidates it -- an SFENCE.VMA, a satp write, a PMP write. The
 * translation, the permission check and the region walk are then asked
 * once per page rather than once per access.
 *
 * The layout is fixed because two code generators read it. A tag is a
 * virtual page number, not an address, so the index and the compare
 * come from one shift: `vpn = va >> 12; e = &table[vpn & mask]; e->tag
 * == vpn`. EMU_DTLB_NONE has bits above any 20-bit page number set and
 * therefore never matches. Sixteen bytes on every host, so the index
 * scales by a shift on both.
 *
 * Alignment is the caller's to test: an entry covers its page, and an
 * aligned access of four bytes or fewer cannot leave one. Anything
 * misaligned goes to the slow path, as it does for the RAM window.
 */
#ifndef EMU_DTLB_H
#define EMU_DTLB_H

#include <stddef.h>
#include <stdint.h>

#define EMU_DTLB_PAGE_SHIFT 12u
#define EMU_DTLB_NONE 0xFFFFFFFFu

typedef struct emu_dtlb_entry {
    uint32_t read;    /* page number if loads are a host access    */
    uint32_t write;   /* page number if stores are, else NONE      */
    uintptr_t addend; /* host address = guest address + addend      */
#if UINTPTR_MAX == 0xFFFFFFFFu
    uint32_t pad; /* sixteen bytes, so the index is a shift */
#endif
} emu_dtlb_entry_t;

_Static_assert(sizeof(emu_dtlb_entry_t) == 16u,
               "both backends scale the index by a shift of 4");

#define EMU_DTLB_OFF_READ 0u
#define EMU_DTLB_OFF_WRITE 4u
#define EMU_DTLB_OFF_ADDEND 8u

_Static_assert(offsetof(emu_dtlb_entry_t, read) == EMU_DTLB_OFF_READ, "");
_Static_assert(offsetof(emu_dtlb_entry_t, write) == EMU_DTLB_OFF_WRITE, "");
_Static_assert(offsetof(emu_dtlb_entry_t, addend) == EMU_DTLB_OFF_ADDEND, "");

#endif /* EMU_DTLB_H */
