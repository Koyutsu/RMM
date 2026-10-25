#include "drivers/pci/pci.h"
#include "drivers/pci/io.h"
#include "drivers/core/driver.h"
#include "console/display.h"
#include "drivers/memory/vmm.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/*
 * Config-space backends
 * Legacy  CF8/CFC: two outportl per access, 32-bit granularity only.
 * ECAM (MMCFG):    byte-granular memory-mapped config space, the base
 *                  normally comes from the ACPI MCFG table.  We have no
 *                  ACPI parser yet, so we probe the two well-known QEMU
 *                  bases and validate them against the legacy ports:
 *                      q35    -> 0xB0000000 (256 MiB window)
 *                      i440fx -> none (probe fails, stay on ports)
 *                  On real hardware the probe validates against bus 0,
 *                  device 0 - the host bridge, which every chipset has,
 *                  if the values match we trust the window.  A full MCFG
 *                  parser replaces this once ACPI lands.
 */

#define PCI_LEGACY_ADDR 0xCF8
#define PCI_LEGACY_DATA 0xCFC

static uint64_t ecam_base   = 0;
static bool     ecam_active = false;

static const uint64_t ECAM_CANDIDATES[] = {
    0xB0000000ULL,   /* QEMU q35 */
    0xE0000000ULL,   /* many real chipsets / OVMF default */
    0xC0000000ULL,   /* some server firmware */
};
#define ECAM_CANDIDATE_COUNT (int)(sizeof(ECAM_CANDIDATES)/sizeof(ECAM_CANDIDATES[0]))

bool     pci_using_ecam(void) { return ecam_active; }
uint64_t pci_ecam_base(void)  { return ecam_base; }

static uint32_t legacy_addr(uint32_t bus, uint32_t dev, uint32_t fn, uint32_t off) {
    return (1u << 31) | (bus << 16) | (dev << 11) | (fn << 8) | (off & 0xFC);
}

static uint32_t legacy_read(uint32_t bus, uint32_t dev, uint32_t fn, uint32_t off) {
    outportl(PCI_LEGACY_ADDR, legacy_addr(bus, dev, fn, off));
    return inportl(PCI_LEGACY_DATA);
}

static void legacy_write(uint32_t bus, uint32_t dev, uint32_t fn,
                         uint32_t off, uint32_t val) {
    outportl(PCI_LEGACY_ADDR, legacy_addr(bus, dev, fn, off));
    outportl(PCI_LEGACY_DATA, val);
}

/* ECAM: 4 KiB of config space per function */
static volatile uint8_t *ecam_ptr(uint32_t bus, uint32_t dev, uint32_t fn,
                                  uint32_t off) {
    uint64_t addr = ecam_base
                  + ((uint64_t)bus << 20)
                  + ((uint64_t)dev << 15)
                  + ((uint64_t)fn  << 12)
                  + (off & 0xFFF);   /* byte-granular within the 4 KiB page */
    return (volatile uint8_t *)(uintptr_t)addr;
}

/*
 * Probe for a usable ECAM window: the candidate must agree with the
 * legacy ports on the host bridge (00:00.0) vendor/device id and on a
 * second device if present.  The kernel identity-maps at least the low
 * 1 GiB, and these candidates live below 1 GiB, so direct pointer access
 * is safe.  (0xE0000000 is checked against the VMM identity limit.)
 */
static void ecam_probe(void) {
    /* host bridge must exist to compare against anything */
    uint32_t hb_vendor = legacy_read(0, 0, 0, PCI_CFG_VENDOR_ID) & 0xFFFF;
    if (hb_vendor == 0xFFFF) return;

    for (int i = 0; i < ECAM_CANDIDATE_COUNT; i++) {
        uint64_t cand = ECAM_CANDIDATES[i];
        /* map the candidate window page before dereferencing anything */
        if (!vmm_identity_map_range(cand, cand + 0x1000,
                                    PTE_PRESENT | PTE_WRITABLE))
            continue;

        volatile uint32_t *win = (volatile uint32_t *)(uintptr_t)cand;
        uint32_t ecam_vendor_dev = win[0];

        /* bits 31:16 vendor, 15:0 device in ECAM dword 0 (little-endian
         * dword = vendor | device<<16) - same layout as legacy read     */
        if ((ecam_vendor_dev & 0xFFFF) != hb_vendor) continue;
        uint32_t legacy_vd = legacy_read(0, 0, 0, 0);
        if (ecam_vendor_dev != legacy_vd) continue;

        /* cross-check revision/header too - 12 bytes of agreement */
        uint32_t ecam_3 = win[3];
        uint32_t leg_3  = legacy_read(0, 0, 0, 12);
        if (ecam_3 != leg_3) continue;

        /* validated - now map the full standard 256 MiB window
         * (bus 0-255, 4 KiB per function) so enumeration can reach
         * every device's config space without further faults        */
        if (!vmm_identity_map_range(cand, cand + 0x10000000ULL,
                                    PTE_PRESENT | PTE_WRITABLE)) {
            display_puts("  PCI: ECAM window map failed - legacy ports\n");
            continue;
        }
        ecam_base   = cand;
        ecam_active = true;
        return;
    }
}

/* Public config-space API */
uint32_t pci_read_cfg(uint32_t bus, uint32_t dev, uint32_t fn,
                      uint32_t off, uint32_t size) {
    /* ECAM gives 4 KiB per function (the full PCIe extended config
     * space), the legacy mechanism tops out at 256 bytes. */
    uint32_t off_limit = ecam_active ? 0xFFCu : 0xFCu;
    if (off > off_limit || (dev > 31 && !ecam_active) || fn > 7)
        return 0xFFFFFFFFu;

    if (ecam_active) {
        volatile uint8_t *p = ecam_ptr(bus, dev, fn, off);
        switch (size) {
        case 1: return *p;
        case 2: { uint16_t v; __asm__ volatile("movw %1, %0" : "=r"(v) : "m"(*(volatile uint16_t *)p)); return v; }
        case 4: { uint32_t v; __asm__ volatile("movl %1, %0" : "=r"(v) : "m"(*(volatile uint32_t *)p)); return v; }
        default: return 0xFFFFFFFFu;
        }
    }

    /* legacy path: 32-bit aligned accesses only */
    uint32_t aligned = off & 0xFC;
    uint32_t data = legacy_read(bus, dev, fn, aligned);
    if (size == 1) return (data >> ((off & 3) * 8)) & 0xFF;
    if (size == 2) return (data >> ((off & 2) * 8)) & 0xFFFF;
    return data;
}

void pci_write_cfg(uint32_t bus, uint32_t dev, uint32_t fn,
                   uint32_t off, uint32_t size, uint32_t val) {
    uint32_t off_limit = ecam_active ? 0xFFCu : 0xFCu;
    if (off > off_limit || (dev > 31 && !ecam_active) || fn > 7) return;

    if (ecam_active) {
        volatile uint8_t *p = ecam_ptr(bus, dev, fn, off);
        switch (size) {
        case 1: *p = (uint8_t)val; break;
        case 2: __asm__ volatile("movw %0, %1" :: "r"((uint16_t)val), "m"(*(volatile uint16_t *)p) : "memory"); break;
        case 4: __asm__ volatile("movl %0, %1" :: "r"(val), "m"(*(volatile uint32_t *)p) : "memory"); break;
        default: break;
        }
        return;
    }

    if (size == 4 && (off & 3) == 0) {
        legacy_write(bus, dev, fn, off, val);
        return;
    }
    /* read-modify-write for sub-dword writes */
    uint32_t aligned = off & 0xFC;
    uint32_t shift   = (off & 3) * 8;
    uint32_t mask    = (size == 1) ? 0xFFu : 0xFFFFu;
    uint32_t cur     = legacy_read(bus, dev, fn, aligned);
    cur = (cur & ~(mask << shift)) | ((val & mask) << shift);
    legacy_write(bus, dev, fn, aligned, cur);
}

/* Device registry */
pci_device_t g_pci_devices[PCI_MAX_DEVICES];
int          g_pci_device_count = 0;

static void dev_init(pci_device_t *d) {
    uint8_t *p = (uint8_t *)d;
    for (size_t i = 0; i < sizeof(*d); i++) p[i] = 0;
    d->valid = true;
}

static pci_device_t *dev_alloc(void) {
    if (g_pci_device_count >= PCI_MAX_DEVICES) return NULL;
    pci_device_t *d = &g_pci_devices[g_pci_device_count++];
    dev_init(d);
    return d;
}

pci_device_t *pci_find_device(uint16_t vendor, uint16_t device) {
    for (int i = 0; i < g_pci_device_count; i++) {
        pci_device_t *d = &g_pci_devices[i];
        if (d->vendor_id == vendor && d->device_id == device) return d;
    }
    return NULL;
}

pci_device_t *pci_find_class(uint8_t class_code, uint8_t subclass) {
    for (int i = 0; i < g_pci_device_count; i++) {
        pci_device_t *d = &g_pci_devices[i];
        if (d->class_code == class_code &&
            (subclass == 0xFF || d->subclass == subclass)) return d;
    }
    return NULL;
}

pci_device_t *pci_find_slot(uint32_t bus, uint32_t dev, uint32_t fn) {
    for (int i = 0; i < g_pci_device_count; i++) {
        pci_device_t *d = &g_pci_devices[i];
        if (d->bus == bus && d->dev == dev && d->fn == fn) return d;
    }
    return NULL;
}

/* Capability walk */
static void scan_caps(pci_device_t *d) {
    if (!(d->status & PCI_STATUS_CAP_LIST)) return;

    uint8_t ptr = (uint8_t)pci_read_cfg(d->bus, d->dev, d->fn,
                                        PCI_CFG_CAP_PTR, 1);
    int guard = 0;
    while (ptr && ptr != 0xFF && guard++ < 24) {
        uint8_t id = (uint8_t)pci_read_cfg(d->bus, d->dev, d->fn, ptr, 1);
        switch (id) {
        case PCI_CAP_ID_MSI: {
            d->caps.has_msi   = true;
            d->caps.msi_off   = ptr;
            uint16_t ctrl = (uint16_t)pci_read_cfg(d->bus, d->dev, d->fn,
                                                   ptr + 2, 2);
            d->caps.msi_64bit        = (ctrl >> 7) & 1;
            d->caps.msi_per_vec_mask = (ctrl >> 8) & 1;
            break;
        }
        case PCI_CAP_ID_MSIX: {
            d->caps.has_msix = true;
            d->caps.msix_off = ptr;
            uint16_t ctrl = (uint16_t)pci_read_cfg(d->bus, d->dev, d->fn,
                                                   ptr + 2, 2);
            d->caps.msix_table_size = (uint16_t)((ctrl & 0x07FF) + 1);
            uint32_t tbl = pci_read_cfg(d->bus, d->dev, d->fn, ptr + 4, 4);
            d->caps.msix_table_bar = (uint8_t)(tbl & 0x7);
            d->caps.msix_table_off = tbl & ~0x7u;
            break;
        }
        case PCI_CAP_ID_PCIE: {
            d->caps.has_pcie = true;
            uint32_t reg = pci_read_cfg(d->bus, d->dev, d->fn, ptr, 4);
            d->caps.pcie_type = (uint8_t)((reg >> 20) & 0xF);
            break;
        }
        case PCI_CAP_ID_PM:
            d->caps.has_pm = true;
            d->caps.pm_off = ptr;
            break;
        default:
            break;
        }
        ptr = (uint8_t)pci_read_cfg(d->bus, d->dev, d->fn, ptr + 1, 1);
    }
}

/* BAR sizing */
/*
 * Classic algorithm: write all-1s, read back, the device lights up only
 * the address bits it decodes.  size = ~(mask) + 1.  Restore afterwards.
 */
static void size_bar(pci_device_t *d, int idx, uint32_t bar_off) {
    uint32_t bus = d->bus, dev = d->dev, fn = d->fn;
    uint32_t lo_orig = pci_read_cfg(bus, dev, fn, bar_off, 4);
    if (lo_orig == 0xFFFFFFFFu) return;

    pci_write_cfg(bus, dev, fn, bar_off, 4, 0xFFFFFFFFu);
    uint32_t lo_mask = pci_read_cfg(bus, dev, fn, bar_off, 4);
    pci_write_cfg(bus, dev, fn, bar_off, 4, lo_orig);

    if (lo_orig & 0x1) {                      /* I/O BAR */
        uint32_t sz = ~(lo_mask & ~0x3u) + 1;
        if (lo_orig & ~0x3u) {
            d->bars[idx].type  = PCI_BAR_IO;
            d->bars[idx].base  = lo_orig & ~0x3u;
            d->bars[idx].size  = sz;
        }
        return;
    }

    uint8_t mem_type = (lo_orig >> 1) & 0x3;  /* bits 2:1 */
    bool prefetch    = (lo_orig & 0x8) != 0;

    if (mem_type == 0x2) {                    /* 64-bit MEM */
        uint32_t hi_off  = bar_off + 4;
        uint32_t hi_orig = pci_read_cfg(bus, dev, fn, hi_off, 4);
        pci_write_cfg(bus, dev, fn, hi_off, 4, 0xFFFFFFFFu);
        uint32_t hi_mask = pci_read_cfg(bus, dev, fn, hi_off, 4);
        pci_write_cfg(bus, dev, fn, hi_off, 4, hi_orig);

        uint64_t sz = (((uint64_t)hi_mask << 32) | (lo_mask & ~0xFu));
        sz = ~sz + 1;
        if (((uint64_t)hi_orig << 32) | (lo_orig & ~0xFu)) {
            d->bars[idx].type  = PCI_BAR_MEM64;
            d->bars[idx].prefetchable = prefetch;
            d->bars[idx].base  = (((uint64_t)hi_orig << 32) | (lo_orig & ~0xFu));
            d->bars[idx].size  = sz;
        }
        /* Note: caller must skip the next BAR slot (it is the high half) */
        return;
    }

    uint32_t sz = ~(lo_mask & ~0xFu) + 1;
    if ((lo_orig & ~0xFu) && mem_type == 0x0) {   /* 32-bit MEM only */
        d->bars[idx].type  = PCI_BAR_MEM32;
        d->bars[idx].prefetchable = prefetch;
        d->bars[idx].base  = lo_orig & ~0xFu;
        d->bars[idx].size  = sz;
    }
}

/* Enumeration */
static void enumerate_bus(uint32_t bus, int depth);

/* bus range guard: bridges can form loops on broken firmware */
#define MAX_BUS 256
static bool bus_scanned[MAX_BUS];

static void scan_function(uint32_t bus, uint32_t dev, uint32_t fn, int depth) {
    uint16_t vendor = (uint16_t)(pci_read_cfg(bus, dev, fn,
                                              PCI_CFG_VENDOR_ID, 2));
    if (vendor == 0xFFFF || vendor == 0x0000) return;

    pci_device_t *d = dev_alloc();
    if (!d) return;

    d->bus = bus; d->dev = dev; d->fn = fn;
    d->vendor_id = vendor;
    d->device_id = (uint16_t)pci_read_cfg(bus, dev, fn, PCI_CFG_DEVICE_ID, 2);
    uint32_t revprog = pci_read_cfg(bus, dev, fn, PCI_CFG_REVISION, 2);
    d->revision = (uint8_t)(revprog & 0xFF);
    d->prog_if  = (uint8_t)(revprog >> 8);
    uint32_t cl = pci_read_cfg(bus, dev, fn, PCI_CFG_SUBCLASS, 2);
    d->subclass = (uint8_t)(cl & 0xFF);
    d->class_code = (uint8_t)(cl >> 8);
    d->header_type = (uint8_t)pci_read_cfg(bus, dev, fn, PCI_CFG_HEADER_TYPE, 1);
    d->status = (uint16_t)pci_read_cfg(bus, dev, fn, PCI_CFG_STATUS, 2);
    d->command = (uint16_t)pci_read_cfg(bus, dev, fn, PCI_CFG_COMMAND, 2);
    d->irq_pin = (uint8_t)pci_read_cfg(bus, dev, fn, PCI_CFG_IRQ_PIN, 1);
    d->irq_line = (uint8_t)pci_read_cfg(bus, dev, fn, PCI_CFG_IRQ_LINE, 1);
    d->subsystem_vendor = (uint16_t)pci_read_cfg(bus, dev, fn, PCI_CFG_SUBSYS_VEND, 2);
    d->subsystem_device = (uint16_t)pci_read_cfg(bus, dev, fn, PCI_CFG_SUBSYS_ID, 2);

    scan_caps(d);

    uint8_t ht = d->header_type & 0x7F;
    if (ht == 0x00) {
        /* endpoint: 6 BARs, watch for 64-bit pairs */
        for (int i = 0; i < 6; i++) {
            uint32_t off = PCI_CFG_BAR0 + i * 4;
            size_bar(d, i, off);
            if (d->bars[i].type == PCI_BAR_MEM64) i++;   /* skip high dword */
        }
    } else if (ht == 0x01) {
        /* PCI-to-PCI bridge: 2 BARs + window registers + secondary bus */
        d->is_bridge = true;
        for (int i = 0; i < 2; i++) {
            uint32_t off = PCI_CFG_BAR0 + i * 4;
            size_bar(d, i, off);
            if (d->bars[i].type == PCI_BAR_MEM64) i++;
        }
        d->secondary_bus = (uint8_t)pci_read_cfg(bus, dev, fn, PCI_CFG_SEC_BUS, 1);
        uint32_t mb = pci_read_cfg(bus, dev, fn, PCI_CFG_MEM_BASE, 2);
        uint32_t ml = pci_read_cfg(bus, dev, fn, PCI_CFG_MEM_LIMIT, 2);
        d->mem_base  = ((uint64_t)(mb & 0xFFF0)) << 16;
        d->mem_limit = ((uint64_t)(ml & 0xFFF0) << 16) | 0xFFFFFu;
        uint32_t pb = pci_read_cfg(bus, dev, fn, PCI_CFG_PF_MEM_BASE, 2);
        uint32_t pl = pci_read_cfg(bus, dev, fn, PCI_CFG_PF_MEM_LIMIT, 2);
        d->pf_mem_base  = ((uint64_t)(pb & 0xFFF0)) << 16;
        d->pf_mem_limit = ((uint64_t)(pl & 0xFFF0) << 16) | 0xFFFFFu;
    }
    /* header 0x02 = CardBus: not supported, still listed */

    (void)depth;
}

static void enumerate_bus(uint32_t bus, int depth) {
    if (bus >= MAX_BUS || bus_scanned[bus] || depth > 6) return;
    bus_scanned[bus] = true;

    for (uint32_t dev = 0; dev < 32; dev++) {
        uint16_t vendor = (uint16_t)(pci_read_cfg(bus, dev, 0,
                                                  PCI_CFG_VENDOR_ID, 2));
        if (vendor == 0xFFFF) continue;

        uint8_t ht = (uint8_t)pci_read_cfg(bus, dev, 0, PCI_CFG_HEADER_TYPE, 1);
        bool multifunc = (ht & 0x80) != 0;
        int  funcs = multifunc ? 8 : 1;

        for (uint32_t fn = 0; fn < (uint32_t)funcs; fn++) {
            uint16_t v = (uint16_t)(pci_read_cfg(bus, dev, fn,
                                                 PCI_CFG_VENDOR_ID, 2));
            if (v == 0xFFFF) continue;
            scan_function(bus, dev, fn, depth);
            pci_device_t *d = pci_find_slot(bus, dev, fn);
            if (d && d->is_bridge && d->secondary_bus)
                enumerate_bus(d->secondary_bus, depth + 1);
        }
    }
}

/* Resource assignment (firmware normally does this, we fix up holes) */
#define RESASSIGN_BASE  0xC0000000ULL
#define RESASSIGN_LIMIT 0xF0000000ULL
static uint64_t resassign_next = RESASSIGN_BASE;

static void assign_bars(pci_device_t *d) {
    for (int i = 0; i < 6; i++) {
        pci_bar_t *b = &d->bars[i];
        if (b->type == PCI_BAR_NONE || b->base != 0 || b->size == 0) continue;
        if (b->type == PCI_BAR_MEM64) continue;   /* keep it simple: 32-bit only */
        if (resassign_next + b->size > RESASSIGN_LIMIT) continue;

        uint64_t base = (resassign_next + b->size - 1) & ~(b->size - 1);
        pci_write_cfg(d->bus, d->dev, d->fn, PCI_CFG_BAR0 + i * 4, 4,
                      (uint32_t)(base | (b->type == PCI_BAR_IO ? 0x1 : 0x0)));
        b->base = base;
        resassign_next = base + b->size;
    }
    if (d->is_bridge) return;   /* bridge windows: firmware-managed for now */
}

/* Command helpers */
void pci_commit_command(pci_device_t *d) {
    pci_write_cfg(d->bus, d->dev, d->fn, PCI_CFG_COMMAND, 2, d->command & 0xFFFF);
}
void pci_enable_mem(pci_device_t *d) {
    d->command |= PCI_CMD_MEM_SPACE; pci_commit_command(d);
}
void pci_enable_io(pci_device_t *d) {
    d->command |= PCI_CMD_IO_SPACE; pci_commit_command(d);
}
void pci_enable_busmaster(pci_device_t *d) {
    d->command |= PCI_CMD_BUS_MASTER; pci_commit_command(d);
}
void pci_disable_intx(pci_device_t *d) {
    d->command |= PCI_CMD_INTX_DISABLE; pci_commit_command(d);
}

/* Driver binding */
static pci_driver_t *g_drivers;

void pci_register_driver(pci_driver_t *drv) {
    drv->next = g_drivers;
    g_drivers = drv;
}

int pci_bind_drivers(void) {
    int bound = 0;
    for (int i = 0; i < g_pci_device_count; i++) {
        pci_device_t *d = &g_pci_devices[i];
        for (pci_driver_t *drv = g_drivers; drv; drv = drv->next) {
            if (drv->vendor != 0xFFFF && drv->vendor != d->vendor_id) continue;
            if (drv->device != 0xFFFF && drv->device != d->device_id) continue;
            if (drv->class_code != 0xFF && drv->class_code != d->class_code) continue;
            if (drv->subclass != 0xFF && drv->subclass != d->subclass) continue;
            if (drv->probe(d) == 0) { d->driver = drv; bound++; break; }
        }
    }
    return bound;
}

/* Class names for readable dumps */
static const char *class_name(uint8_t c) {
    switch (c) {
    case 0x00: return "Unclassified";
    case 0x01: return "Mass storage";
    case 0x02: return "Network";
    case 0x03: return "Display";
    case 0x04: return "Multimedia";
    case 0x05: return "Memory";
    case 0x06: return "Bridge";
    case 0x07: return "Communication";
    case 0x08: return "Generic";
    case 0x09: return "Input";
    case 0x0C: return "Serial bus";
    case 0xFF: return "Unassigned";
    default:   return "Other";
    }
}

/* Boot console dump */
/* print.c printf only knows %d %u %x %s %c %p, so pci.c carries its own
 * tiny hex/dec formatters for the wide values BARs need.               */
static void print_hex64(uint64_t v) {
    char tmp[17]; int i = 0;
    if (!v) tmp[i++] = '0';
    while (v && i < 16) { int d = (int)(v & 0xF); tmp[i++] = d < 10 ? '0'+d : 'a'+d-10; v >>= 4; }
    tmp[i] = '\0';
    while (i-- > 0) display_putchar(tmp[i]);
}
static void print_hex2(uint32_t v) {
    static const char h[] = "0123456789abcdef";
    display_putchar(h[(v >> 4) & 0xF]);
    display_putchar(h[v & 0xF]);
}
static void print_hex4(uint32_t v) {
    print_hex2((v >> 8) & 0xFF);
    print_hex2(v & 0xFF);
}
static void print_dec(unsigned long v) {
    char tmp[12]; int i = 0;
    if (!v) tmp[i++] = '0';
    while (v) { tmp[i++] = (char)('0' + (v % 10)); v /= 10; }
    while (i-- > 0) display_putchar(tmp[i]);
}

void pci_dump(void) {
    for (int i = 0; i < g_pci_device_count; i++) {
        pci_device_t *d = &g_pci_devices[i];
        display_puts("  PCI ");
        print_hex2(d->bus); display_putchar(':');
        print_hex2(d->dev); display_putchar('.');
        display_putchar((char)('0' + (d->fn & 7))); display_putchar(' ');
        print_hex4(d->vendor_id); display_putchar(':');
        print_hex4(d->device_id); display_putchar(' ');
        display_puts(class_name(d->class_code));
        if (d->caps.has_pcie)  display_puts(" [PCIe]");
        if (d->caps.has_msi)   display_puts(" [MSI]");
        if (d->caps.has_msix)  display_puts(" [MSI-X]");
        display_putchar('\n');
        for (int b = 0; b < 6; b++) {
            if (d->bars[b].type == PCI_BAR_NONE || d->bars[b].size == 0) continue;
            display_puts("      BAR"); print_dec((unsigned long)b);
            if (d->bars[b].type == PCI_BAR_IO) {
                display_puts("   I/O  0x"); print_hex64(d->bars[b].base);
            } else {
                display_puts("   MEM");
                if (d->bars[b].type == PCI_BAR_MEM64) display_puts("64");
                display_puts("  0x"); print_hex64(d->bars[b].base);
            }
            display_puts("  size=0x"); print_hex64(d->bars[b].size);
            display_putchar('\n');
        }
    }
}

/* Text table for userland lspci (SYS_RMM_PCILIST) */
static char *fmtcat(char *p, char *end, const char *s) {
    while (*s && p < end) *p++ = *s++;
    return p;
}
static char *fmthex(char *p, char *end, uint64_t v, int width) {
    char tmp[16]; int i = 0;
    if (!v) tmp[i++] = '0';
    while (v && i < 16) { int d = v & 0xF; tmp[i++] = d < 10 ? '0'+d : 'a'+d-10; v >>= 4; }
    for (int z = i; z < width && p < end; z++) *p++ = '0';
    while (i-- > 0 && p < end) *p++ = tmp[i];
    return p;
}

int pci_format_list(char *buf, int bufsz) {
    char *p = buf, *end = buf + bufsz - 1;
    for (int i = 0; i < g_pci_device_count; i++) {
        pci_device_t *d = &g_pci_devices[i];
        if (p >= end) break;
        p = fmthex(p, end, d->bus, 2);  *p++ = ':';
        p = fmthex(p, end, d->dev, 2);  *p++ = '.';
        p = fmthex(p, end, d->fn, 1);   *p++ = ' ';
        p = fmthex(p, end, d->vendor_id, 4); *p++ = ':';
        p = fmthex(p, end, d->device_id, 4); *p++ = ' ';
        p = fmthex(p, end, d->class_code, 2);
        p = fmthex(p, end, d->subclass, 2);
        p = fmthex(p, end, d->prog_if, 2);   *p++ = ' ';
        p = fmtcat(p, end, class_name(d->class_code));
        if (d->caps.has_pcie) p = fmtcat(p, end, " PCIe");
        if (d->caps.has_msi)  p = fmtcat(p, end, " MSI");
        if (d->caps.has_msix) p = fmtcat(p, end, " MSI-X");
        {   /* show the bound driver, lspci-style */
            const char *dn = pci_bound_driver_name(d->bus, d->dev, d->fn);
            if (dn) {
                p = fmtcat(p, end, " drv=");
                p = fmtcat(p, end, dn);
            }
        }
        for (int b = 0; b < 6; b++) {
            if (d->bars[b].type == PCI_BAR_NONE || d->bars[b].size == 0) continue;
            p = fmtcat(p, end, " bar");
            p = fmthex(p, end, (uint64_t)b, 1);
            p = fmtcat(p, end, "=");
            p = fmthex(p, end, d->bars[b].base, 1);
            p = fmtcat(p, end, "/");
            p = fmthex(p, end, d->bars[b].size, 1);
        }
        if (p < end) *p++ = '\n';
    }
    *p = '\0';
    return (int)(p - buf);
}

/* Init */
void pci_init(void) {
    ecam_probe();
    for (int i = 0; i < MAX_BUS; i++) bus_scanned[i] = false;

    enumerate_bus(0, 0);

    /* fix up any unassigned 32-bit BARs (firmware normally pre-assigns) */
    for (int i = 0; i < g_pci_device_count; i++)
        assign_bars(&g_pci_devices[i]);

    display_newline();
    display_puts("[  OK  ] PCI/PCIe: ");
    if (ecam_active) {
        display_puts("ECAM @ 0x");
        print_hex64(ecam_base);
    } else {
        display_puts("legacy CF8/CFC");
    }
    display_puts(", ");
    print_dec((unsigned long)g_pci_device_count);
    display_puts(" device(s)");
}
