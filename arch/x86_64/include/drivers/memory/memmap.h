#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/*
 * memmap - Multiboot2 memory map parser + PMM initialiser
 *
 * Walks the multiboot2 info block, finds the memory map tag,
 * and feeds every usable region into the PMM.
 *
 * Supports the full 64-bit address space: any region above 4 GiB
 * that multiboot2 reports as usable is also made available.
 *
 * After memmap_init() the PMM bitmap covers up to PMM_MAX_PAGES pages.
 * The caller should then mark any regions that are in use (kernel image,
 * multiboot2 info block itself, etc.) as allocated.
 */

/* Multiboot2 memory region types */
#define MB2_MEM_AVAILABLE        1
#define MB2_MEM_RESERVED         2
#define MB2_MEM_ACPI_RECLAIMABLE 3
#define MB2_MEM_NVS              4
#define MB2_MEM_BADRAM           5

/* One parsed memory region */
typedef struct {
    uint64_t base;
    uint64_t length;
    uint32_t type;
} mem_region_t;

#define MAX_REGIONS 128

typedef struct {
    mem_region_t regions[MAX_REGIONS];
    int          count;
    uint64_t     total_usable_bytes;
    uint64_t     total_bytes;          /* highest address seen */
} mem_map_t;

/*
 * Parse the multiboot2 info block and fill out a mem_map_t.
 * Returns true if the memory map tag was found.
 */
bool memmap_parse(uint64_t mb2_phys, mem_map_t *out);

/*
 * Initialise the PMM using the memory map.
 * - Marks all usable memory as free.
 * - Re-marks the first 2 MiB (BIOS + kernel) as reserved.
 * - Re-marks the multiboot2 info block pages as reserved.
 * - Re-marks the kernel image pages as reserved
 *   (kernel_start/end are physical addresses from the linker).
 *
 * Returns total usable RAM in bytes.
 */
uint64_t memmap_init_pmm(const mem_map_t *map,
                          uint64_t mb2_phys,
                          uint64_t kernel_phys_start,
                          uint64_t kernel_phys_end);

/* Print the memory map to the current display output */
void memmap_print(const mem_map_t *map);
