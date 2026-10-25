#include "drivers/core/driver.h"
#include "drivers/pci/pci.h"
#include "drivers/pci/io.h"
#include "drivers/memory/vmm.h"
#include "console/print.h"

/*
 *  Driver core
 *
 *  - driver_list: priority-ordered registry (highest priority first)
 *  - device list: filled by the PCI bus walker, platform devices (VBE) are
 *    added by their owning code before bus_enumerate_pci() runs.
 */
#define MAX_DEVICES 48

static device_t  dev_pool[MAX_DEVICES];
static int       ndev = 0;
static driver_t *drv_head = NULL;
static int       ndrv = 0;

static char dev_name_pool[MAX_DEVICES][16];

int driver_count(void) { return ndrv; }
int device_count(void) { return ndev; }

static bool str_len_cmp(const char *a, const char *b);

/* bound driver names indexed by pci.c registry slot (for lspci) */
static char drv_name_tbl[PCI_MAX_DEVICES][20];

/* Matching */
static bool match_one(const driver_match_t *m, const device_t *d) {
    if (m->vendor != PCI_ANY_ID && m->vendor != d->pci_vendor)    return false;
    if (m->device != PCI_ANY_ID && m->device != d->pci_device)    return false;
    if (m->class_ != 0xFF && m->class_ != d->pci_class)           return false;
    return true;
}

static bool str_len_cmp(const char *a, const char *b) {
    while (*a && *b) { if (*a++ != *b++) return 1; }
    return *a || *b;
}

static bool driver_matches(const driver_t *drv, const device_t *d) {
    if (drv->bus != d->bus || !drv->probe) return false;
    if (!drv->match_table) {
        /* platform drivers match by device name */
        if (d->bus == BUS_PLATFORM && drv->name && d->name &&
            !str_len_cmp(drv->name, d->name)) return true;
        return false;
    }
    for (int i = 0; i < 16; i++) {
        /* table is terminated by an all-zero entry */
        if (!drv->match_table[i].vendor && !drv->match_table[i].device &&
            drv->match_table[i].class_ == 0) break;
        if (match_one(&drv->match_table[i], d)) return true;
    }
    return false;
}

/* Registration */
static void try_bind(device_t *d) {
    if (d->bound) return;
    driver_t *best = NULL;
    for (driver_t *p = drv_head; p; p = p->next) {
        if (p->bus == d->bus && p->probe && driver_matches(p, d)) { best = p; break; }
        /* list is priority-sorted, so the first match is the best match */
    }
    if (!best) return;
    if (best->probe(d) == 0) {
        d->drv   = best;
        d->bound = true;
        printf("[drv] %s (%x/%x class %x.%x) -> %s\n",
               d->name, d->pci_vendor, d->pci_device,
               d->pci_class, d->pci_subclass, best->name);
        /* record the binding for lspci (pci.c registry slot) */
        {
            pci_device_t *p = pci_find_slot(d->pci_bus, d->pci_dev, d->pci_func);
            if (p) {
                int idx = (int)(p - g_pci_devices);
                if (idx >= 0 && idx < PCI_MAX_DEVICES && best->name) {
                    char *dst = drv_name_tbl[idx];
                    int k = 0;
                    while (best->name[k] && k < 19) { dst[k] = best->name[k]; k++; }
                    dst[k] = '\0';
                }
            }
        }
    }
}

void driver_register(driver_t *drv) {
    if (!drv || !drv->name || !drv->probe) return;

    /* priority-sorted insert (stable: later registration wins ties) */
    driver_t **pp = &drv_head;
    while (*pp && (*pp)->priority >= drv->priority) pp = &(*pp)->next;
    drv->next = *pp;
    *pp = drv;
    ndrv++;

    /* match against devices that are already known (module hot-add) */
    for (int i = 0; i < ndev; i++) try_bind(&dev_pool[i]);
}

void driver_register_builtins(void) {
    for (const driver_t *const *p = __drivers_start; p < __drivers_end; p++) {
        if (*p) driver_register((driver_t *)*p);
    }
}

/* RMM: a module registers its own builtins at module_init
 * time through this range API, same register path, explicit bounds. */
void driver_register_range(const driver_t *const *start,
                           const driver_t *const *end) {
    if (!start || !end) return;
    for (const driver_t *const *p = start; p < end; p++)
        if (*p) driver_register((driver_t *)*p);
}

/* Device list */
static device_t *device_add(void) {
    if (ndev >= MAX_DEVICES) return NULL;
    device_t *d = &dev_pool[ndev++];
    d->name = dev_name_pool[ndev - 1];
    return d;
}

/* Platform pseudo-device (name-matched, no BARs). */
device_t *platform_device_add(const char *name, void *priv) {
    device_t *d = device_add();
    if (!d) return NULL;
    d->bus  = BUS_PLATFORM;
    d->name = name;
    d->priv = priv;
    return d;
}

static device_t *device_find_slot(uint8_t b, uint8_t dv, uint8_t fn) {
    (void)b; (void)dv; (void)fn;
    return NULL;
}
/* (reserved for future bus re-scan / module hotplug support) */

/*
 *  single-source PCI enumeration
 *
 *  The old flat hardware scan (bus 0-255 x dev 0-31, own BAR
 *  sizing, no bridge recursion) duplicated drivers/pci/pci.c and could
 *  not see devices behind PCIe bridges.  bus_enumerate_pci() now builds
 *  its device list from the pci.c registry (g_pci_devices[]) - one
 *  hardware walk, one BAR sizing pass, bridge-aware, and only adds the
 *  driver-model bookkeeping (match/probe/bind) on top.  Endpoints get
 *  memory + I/O + bus-master decode enabled before probe, as PCI
 *  requires for MMIO/DMA-capable endpoints.
 */

const char *pci_bound_driver_name(uint32_t bus, uint32_t dev, uint32_t fn) {
    pci_device_t *p = pci_find_slot(bus, dev, fn);
    if (!p) return NULL;
    int idx = (int)(p - g_pci_devices);
    if (idx < 0 || idx >= PCI_MAX_DEVICES) return NULL;
    return drv_name_tbl[idx][0] ? drv_name_tbl[idx] : NULL;
}

static bool dev_register_pci(const pci_device_t *p) {
    device_t *d = device_add();
    if (!d) return false;

    d->bus         = BUS_PCI;
    d->pci_bus     = (uint8_t)p->bus;
    d->pci_dev     = (uint8_t)p->dev;
    d->pci_func    = (uint8_t)p->fn;
    d->pci_vendor  = p->vendor_id;
    d->pci_device  = p->device_id;
    d->pci_class   = p->class_code;
    d->pci_subclass= p->subclass;
    d->pci_progif  = p->prog_if;
    d->irq_pin     = p->irq_pin;
    d->irq_line    = p->irq_line;

    for (int i = 0; i < 6; i++) {
        switch (p->bars[i].type) {
        case PCI_BAR_IO:
            d->bar_raw[i] = (uint32_t)p->bars[i].base | 0x1u;
            d->bar[i]     = (uint32_t)p->bars[i].base;
            d->bar_size[i]= p->bars[i].size;
            break;
        case PCI_BAR_MEM32:
            d->bar_raw[i] = (uint32_t)p->bars[i].base |
                            (p->bars[i].prefetchable ? 0x8u : 0x0u);
            d->bar[i]     = (uint32_t)p->bars[i].base;
            d->bar_size[i]= p->bars[i].size;
            break;
        case PCI_BAR_MEM64:
            d->bar_raw[i] = (uint32_t)p->bars[i].base | 0x4u |
                            (p->bars[i].prefetchable ? 0x8u : 0x0u);
            d->bar[i]     = (uint32_t)p->bars[i].base;
            d->bar_size[i]= p->bars[i].size;
            break;
        default:
            break;
        }
    }

    /* "0000:00:02.0" bus-slot name (device_t.name points into the
     * shared name pool that device_add() assigned) */
    {
        char *n = dev_name_pool[ndev - 1];
        const char hex[] = "0123456789abcdef";
        n[0]='0'; n[1]='0'; n[2]='0'; n[3]='0'; n[4]=':';
        n[5]=hex[(p->bus>>4)&0xF]; n[6]=hex[p->bus&0xF]; n[7]=':';
        n[8]=hex[(p->dev>>4)&0xF]; n[9]=hex[p->dev&0xF]; n[10]='.';
        n[11]='0'+(p->fn & 7); n[12]='\0';
    }
    return true;
}

void bus_enumerate_pci(void) {
    /* devices may already have been imported (module hot-add path) */
    if (ndev == 0) {
        for (int i = 0; i < g_pci_device_count; i++)
            dev_register_pci(&g_pci_devices[i]);
    }

    /* Endpoints need memory + I/O decode and bus mastering before a
     * driver touches MMIO/DMA (PCI command register bits 0/1/2).
     * Bridges and unbound-class devices are left alone. */
    for (int i = 0; i < g_pci_device_count; i++) {
        pci_device_t *p = &g_pci_devices[i];
        if (p->is_bridge || p->header_type == 0x01) continue;
        pci_enable_io(p);
        pci_enable_mem(p);
        pci_enable_busmaster(p);
    }

    /* bind + probe everything */
    for (int i = 0; i < ndev; i++) try_bind(&dev_pool[i]);
}

/* Lookups */
device_t *device_find_pci(uint16_t vendor, uint16_t device) {
    for (int i = 0; i < ndev; i++)
        if (dev_pool[i].bus == BUS_PCI &&
            dev_pool[i].pci_vendor == vendor && dev_pool[i].pci_device == device)
            return &dev_pool[i];
    return NULL;
}

device_t *device_find_class(uint8_t base_class) {
    for (int i = 0; i < ndev; i++)
        if (dev_pool[i].bus == BUS_PCI && dev_pool[i].pci_class == base_class)
            return &dev_pool[i];
    return NULL;
}
