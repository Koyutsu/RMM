/*
 *  ACPI MADT parser
 *
 *  RSDP (EBDA + BIOS ROM area) -> RSDT/XSDT -> "APIC" table -> LAPIC/IOAPIC
 *  entries.  This is the topology source for smp.c (how many application
 *  processors to wake) and, later, for IOAPIC-based IRQ routing.
 */
#include "cpu/madt.h"
#include "drivers/memory/vmm.h"
#include "console/print.h"

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

madt_info_t g_madt;

/* raw ACPI headers */
typedef struct {
    char     signature[8];
    uint8_t  checksum;
    char     oemid[6];
    uint8_t  revision;
    uint32_t rsdt_addr;
    /* rev >= 2: */
    uint32_t length;
    uint64_t xsdt_addr;
    uint8_t  ext_checksum;
    uint8_t  reserved[3];
} rsdp_t;

typedef struct {
    char     signature[4];
    uint32_t length;
    uint8_t  revision;
    uint8_t  checksum;
    char     oemid[6];
    char     oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} sdt_hdr_t;                                 /* 36 bytes */

/* helpers */
static bool sig_eq(const char *a, const char *b, int n) {
    for (int i = 0; i < n; i++) if (a[i] != b[i]) return false;
    return true;
}

static uint8_t checksum8(const void *p, size_t len) {
    const uint8_t *b = p;
    uint8_t sum = 0;
    while (len--) sum += *b++;
    return sum;
}

static rsdp_t *find_rsdp(void) {
    /* 1. EBDA (segment at 0x40E) */
    volatile uint16_t *ebda_seg_p = (volatile uint16_t *)(uintptr_t)0x40E;
    uint16_t ebda_seg = *ebda_seg_p;
    uint32_t ebda     = (uint32_t)ebda_seg << 4;
    if (ebda && ebda < 0x100000) {
        for (uint32_t a = ebda; a < ebda + 1024; a += 16) {
            rsdp_t *r = (rsdp_t *)(uintptr_t)a;
            if (sig_eq(r->signature, "RSD PTR ", 8) &&
                checksum8(r, 20) == 0) return r;
        }
    }
    /* 2. BIOS ROM area */
    for (uint32_t a = 0xE0000; a < 0xFFFFF; a += 16) {
        rsdp_t *r = (rsdp_t *)(uintptr_t)a;
        if (sig_eq(r->signature, "RSD PTR ", 8) &&
            checksum8(r, 20) == 0) return r;
    }
    return NULL;
}

static void parse_madt(sdt_hdr_t *madt) {
    uint8_t *p   = (uint8_t *)madt + 44;      /* entries start after header */
    uint8_t *end = (uint8_t *)madt + madt->length;

    while (p + 2 <= end) {
        uint8_t type = p[0];
        uint8_t len  = p[1];
        if (len < 2) break;

        if (type == 0 && len >= 8) {          /* Processor Local APIC */
            uint8_t  apic_id = p[3];
            uint32_t flags   = *(uint32_t *)(p + 4);
            if ((flags & 1) && g_madt.lapic_count < MADT_MAX_LAPICS)
                g_madt.lapic_ids[g_madt.lapic_count++] = apic_id;
        } else if (type == 1 && len >= 12) {  /* IOAPIC */
            uint32_t addr = *(uint32_t *)(p + 4);
            uint32_t gsib = *(uint32_t *)(p + 8);
            if (g_madt.ioapic_count < MADT_MAX_IOAPICS) {
                g_madt.ioapics[g_madt.ioapic_count].addr = addr;
                g_madt.ioapics[g_madt.ioapic_count].gsib = gsib;
                g_madt.ioapic_count++;
            }
        }
        p += len;
    }
    g_madt.found = true;
}

static void map_range(uint64_t phys, uint64_t len) {
    vmm_identity_map_range(phys & ~(uint64_t)(PAGE_SIZE - 1),
                           phys + len + PAGE_SIZE,
                           PTE_PRESENT | PTE_WRITABLE);
}

static bool try_rsdt(uint32_t rsdt_phys) {
    map_range(rsdt_phys, 4096);
    sdt_hdr_t *rsdt = (sdt_hdr_t *)(uintptr_t)rsdt_phys;
    if (!sig_eq(rsdt->signature, "RSDT", 4)) return false;
    map_range(rsdt_phys, rsdt->length + 4096);
    uint32_t entries = (rsdt->length - 36) / 4;
    uint32_t *tab    = (uint32_t *)((uintptr_t)rsdt + 36);
    for (uint32_t i = 0; i < entries; i++) {
        if (!tab[i]) continue;
        map_range(tab[i], 4096);
        sdt_hdr_t *h = (sdt_hdr_t *)(uintptr_t)tab[i];
        if (sig_eq(h->signature, "APIC", 4)) {
            map_range(tab[i], h->length + 4096);
            parse_madt(h);
            return true;
        }
    }
    return false;
}

static bool try_xsdt(uint64_t xsdt_phys) {
    map_range(xsdt_phys, 4096);
    sdt_hdr_t *xsdt = (sdt_hdr_t *)(uintptr_t)xsdt_phys;
    if (!sig_eq(xsdt->signature, "XSDT", 4)) return false;
    map_range(xsdt_phys, xsdt->length + 4096);
    uint32_t entries = (xsdt->length - 36) / 8;
    uint64_t *tab    = (uint64_t *)((uintptr_t)xsdt + 36);
    for (uint32_t i = 0; i < entries; i++) {
        if (!tab[i] || tab[i] >= 0x100000000ull) continue;
        map_range(tab[i], 4096);
        sdt_hdr_t *h = (sdt_hdr_t *)(uintptr_t)tab[i];
        if (sig_eq(h->signature, "APIC", 4)) {
            map_range(tab[i], h->length + 4096);
            parse_madt(h);
            return true;
        }
    }
    return false;
}

void madt_parse(void) {
    rsdp_t *rsdp = find_rsdp();
    if (!rsdp) {
        print_str("[madt] no RSDP found (pre-ACPI machine?)\n");
        return;
    }
    printf("[madt] RSDP rev %u found\n", rsdp->revision);

    if (rsdp->revision >= 2 && rsdp->xsdt_addr &&
        checksum8(rsdp, sizeof(rsdp_t)) == 0) {
        if (try_xsdt(rsdp->xsdt_addr)) goto done;
    }
    if (rsdp->rsdt_addr) try_rsdt(rsdp->rsdt_addr);

done:
    if (g_madt.found) {
        printf("[madt] %u CPU(s), %u IOAPIC(s)\n",
               g_madt.lapic_count, g_madt.ioapic_count);
        for (uint32_t i = 0; i < g_madt.lapic_count; i++)
            printf("[madt]   cpu%u: apic_id %u\n", i, g_madt.lapic_ids[i]);
    } else {
        print_str("[madt] no MADT - assuming single CPU\n");
    }
}
