#pragma once
/*
 * madt.h - ACPI RSDP/RSDT/XSDT/MADT parsing
 */
#include <stdint.h>
#include <stdbool.h>

#define MADT_MAX_LAPICS 16
#define MADT_MAX_IOAPICS 4

typedef struct madt_info {
    uint32_t lapic_count;                     /* enabled LAPICs          */
    uint8_t  lapic_ids[MADT_MAX_LAPICS];
    uint32_t ioapic_count;
    struct {
        uint32_t addr;
        uint32_t gsib;
    } ioapics[MADT_MAX_IOAPICS];
    bool     found;
} madt_info_t;

extern madt_info_t g_madt;

/* Scan for the RSDP and walk to the MADT.  Safe to call after vmm_init(). */
void madt_parse(void);
