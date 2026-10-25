#include "drivers/graphics/fb_font.h"
#include "drivers/memory/memmap.h"
#include "drivers/memory/vmm.h"
#include "console/display.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* Multiboot2 structures */
typedef struct __attribute__((packed)) {
    uint32_t type;
    uint32_t size;
} mb2_tag_t;

typedef struct __attribute__((packed)) {
    uint32_t type;          /* 6 */
    uint32_t size;
    uint32_t entry_size;
    uint32_t entry_version;
    /* entries follow */
} mb2_mmap_tag_t;

typedef struct __attribute__((packed)) {
    uint64_t base_addr;
    uint64_t length;
    uint32_t type;
    uint32_t reserved;
} mb2_mmap_entry_t;

/* Parse */
bool memmap_parse(uint64_t mb2_phys, mem_map_t *out) {
    if (!mb2_phys || !out) return false;

    out->count               = 0;
    out->total_usable_bytes  = 0;
    out->total_bytes         = 0;

    uint8_t  *base  = (uint8_t *)(uintptr_t)mb2_phys;
    uint32_t  total = *(uint32_t *)base;
    uint8_t  *ptr   = base + 8;
    uint8_t  *end   = base + total;

    bool found = false;

    while (ptr < end) {
        mb2_tag_t *tag = (mb2_tag_t *)ptr;
        if (tag->type == 0) break;   /* End tag */

        if (tag->type == 6) {        /* memory map tag */
            found = true;
            mb2_mmap_tag_t *mtag = (mb2_mmap_tag_t *)tag;
            uint32_t esize = mtag->entry_size;
            uint8_t *eptr  = (uint8_t *)(mtag + 1);
            uint8_t *eend  = ptr + tag->size;

            while (eptr < eend && out->count < MAX_REGIONS) {
                mb2_mmap_entry_t *e = (mb2_mmap_entry_t *)eptr;

                out->regions[out->count].base   = e->base_addr;
                out->regions[out->count].length = e->length;
                out->regions[out->count].type   = e->type;
                out->count++;

                uint64_t top = e->base_addr + e->length;
                if (top > out->total_bytes) out->total_bytes = top;

                if (e->type == MB2_MEM_AVAILABLE)
                    out->total_usable_bytes += e->length;

                eptr += esize;
            }
        }

        ptr += (tag->size + 7) & ~7u;
    }

    return found;
}

/* Align helpers */
static uint64_t page_align_down(uint64_t addr) {
    return addr & ~(uint64_t)(PAGE_SIZE - 1);
}
static uint64_t page_align_up(uint64_t addr) {
    return (addr + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
}

/* PMM init using real map */
uint64_t memmap_init_pmm(const mem_map_t *map,
                          uint64_t mb2_phys,
                          uint64_t kernel_phys_start,
                          uint64_t kernel_phys_end) {
    if (!map) return 0;

    /* 1. Tell the PMM how much RAM we have total */
    pmm_init(map->total_bytes);

    /* 2. Walk map: free every usable page */
    for (int i = 0; i < map->count; i++) {
        if (map->regions[i].type != MB2_MEM_AVAILABLE) continue;

        uint64_t base = page_align_up(map->regions[i].base);
        uint64_t top  = page_align_down(map->regions[i].base +
                                         map->regions[i].length);

        /* Skip the null page */
        if (base < PAGE_SIZE) base = PAGE_SIZE;

        for (uint64_t a = base; a < top; a += PAGE_SIZE)
            pmm_free_page(a);
    }

    /* 3. Re-mark reserved / critical areas as used */

    /* Low 1 MiB: BIOS, VGA hole, EBDA */
    for (uint64_t a = 0; a < 0x100000; a += PAGE_SIZE)
        pmm_alloc_page(a);

    /* Kernel image */
    uint64_t ks = page_align_down(kernel_phys_start);
    uint64_t ke = page_align_up(kernel_phys_end);
    for (uint64_t a = ks; a < ke; a += PAGE_SIZE)
        pmm_alloc_page(a);

    /* Multiboot2 info block */
    if (mb2_phys) {
        uint32_t mb2_size = *(uint32_t *)(uintptr_t)mb2_phys;
        uint64_t mb2s = page_align_down(mb2_phys);
        uint64_t mb2e = page_align_up(mb2_phys + mb2_size);
        for (uint64_t a = mb2s; a < mb2e; a += PAGE_SIZE)
            pmm_alloc_page(a);
    }

    /* reserve every Multiboot2 module (the initrd cpio).
     * The ramfs heap is now allocated from the PMM at boot before the
     * cpio is parsed, without this reservation the heap pages could
     * land on top of the module and destroy it (observed: initrd parse
     * produced zero entries).  The old code got away with it only because
     * nothing consumed PMM pages between boot and the parse. */
    if (mb2_phys) {
        uint32_t mb2_size = *(uint32_t *)(uintptr_t)mb2_phys;
        uint8_t *tag = (uint8_t *)(uintptr_t)mb2_phys + 8; /* skip size+reserved */
        uint8_t *end = (uint8_t *)(uintptr_t)(mb2_phys + mb2_size);
        while (tag + 8 <= end) {
            uint32_t type = *(uint32_t *)(uintptr_t)tag;
            uint32_t tsize = *(uint32_t *)(uintptr_t)(tag + 4);
            if (type == 0) break;                       /* end tag */
            if (type == 3 && tsize >= 16) {             /* module */
                /* Note: MB2 module fields are 32-bit! */
                uint64_t mstart = (uint64_t)*(uint32_t *)(uintptr_t)(tag + 8);
                uint64_t mend   = (uint64_t)*(uint32_t *)(uintptr_t)(tag + 12);
                uint64_t ms = page_align_down(mstart);
                uint64_t me = page_align_up(mend);
                if (me > ms)        /* guard wrap-around */
                    for (uint64_t a = ms; a < me; a += PAGE_SIZE)
                        pmm_alloc_page(a);
            }
            tag += (tsize + 7) & ~7u;                   /* 8-byte align */
        }
    }

    return map->total_usable_bytes;
}

/* Print */
static const char *region_type_str(uint32_t t) {
    switch (t) {
    case MB2_MEM_AVAILABLE:        return "Usable";
    case MB2_MEM_RESERVED:         return "Reserved";
    case MB2_MEM_ACPI_RECLAIMABLE: return "ACPI Reclaimable";
    case MB2_MEM_NVS:              return "ACPI NVS";
    case MB2_MEM_BADRAM:           return "Bad RAM";
    default:                       return "Unknown";
    }
}

static void print_hex64(uint64_t v) {
    /* Print a 64-bit hex value without printf */
    char buf[17];
    buf[16] = '\0';
    for (int i = 15; i >= 0; i--) {
        int nib = (int)(v & 0xF);
        buf[i] = (char)(nib < 10 ? '0' + nib : 'a' + nib - 10);
        v >>= 4;
    }
    display_puts("0x");
    /* Skip leading zeros but keep at least 4 digits */
    int start = 0;
    while (start < 12 && buf[start] == '0') start++;
    display_puts(buf + start);
}

static void print_size(uint64_t bytes) {
    if (bytes >= 1024ULL * 1024 * 1024) {
        display_printf("%u GiB", (unsigned)(bytes / (1024*1024*1024)));
    } else if (bytes >= 1024 * 1024) {
        display_printf("%u MiB", (unsigned)(bytes / (1024*1024)));
    } else if (bytes >= 1024) {
        display_printf("%u KiB", (unsigned)(bytes / 1024));
    } else {
        display_printf("%u B", (unsigned)bytes);
    }
}

void memmap_print(const mem_map_t *map) {
    if (!map) return;

    display_set_color(FB_CYAN, FB_DARK_BG);
    display_puts("  Memory Map:\n");
    display_set_color(FB_LIGHT_GRAY, FB_DARK_BG);
    display_puts("  Base               Length             Type\n");
    display_puts("  -------------------------------------------------\n");

    for (int i = 0; i < map->count; i++) {
        display_puts("  ");
        print_hex64(map->regions[i].base);
        display_puts("  ");
        print_hex64(map->regions[i].length);
        display_puts("  ");

        if (map->regions[i].type == MB2_MEM_AVAILABLE)
            display_set_color(FB_BRIGHT_GREEN, FB_DARK_BG);
        else
            display_set_color(FB_LIGHT_GRAY, FB_DARK_BG);

        display_puts(region_type_str(map->regions[i].type));
        display_set_color(FB_LIGHT_GRAY, FB_DARK_BG);
        display_putchar('\n');
    }

    display_puts("\n  Total usable: ");
    display_set_color(FB_BRIGHT_GREEN, FB_DARK_BG);
    print_size(map->total_usable_bytes);
    display_set_color(FB_WHITE, FB_DARK_BG);
    display_putchar('\n');
}
