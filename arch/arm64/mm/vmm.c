/*
 * arch/arm64/mm/vmm.c - AArch64 page-table model, skeleton.
 * Not buildable.  See ../README.md.
 *
 * Model (AArch64 VMSA, 48-bit VA, 4 KiB granule):
 *   L0 root (index [47:39]) -> L1 [38:30] -> L2 [29:21] -> L3 [20:12],
 *   descriptors: block (L0-L2) or table/page, attrs in bits [63:40]
 *   (AP, XN, nG, MAIR index).  Module slots get a dedicated L0 index -
 *   the arm64 analogue of x86_64's PML4[510] - with power-of-two slot
 *   sizes and base % size == 0.
 *
 * Required ops (from include/rmm/arch.h):
 *   arch_map_page / arch_unmap_page / arch_translate / arch_protect,
 *   arch_tlb_sync_range (DSB ISH + ISB, per-VA set/way fallback).
 *
 * W^X: MAIR attribute pairs per slot segment, XN on data, guard
 * pages unmapped between segments.
 */
#include <stdint.h>

int arch_map_page(uint64_t va, uint64_t pa, uint64_t flags) {
    (void)va; (void)pa; (void)flags;
    return -1;   /* Stub */
}
