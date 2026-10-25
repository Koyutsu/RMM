#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/*
 * pci - PCI / PCI Express core
 *
 * What the old 36-line "scan" grew into:
 *   - config-space access via legacy CF8/CFC ports OR PCIe ECAM (MMCFG)
 *     when a working MMCONFIG window is detected (q35 and most real
 *     chipsets) - auto-probed at init
 *   - full recursive bus enumeration incl. PCI-to-PCI bridges (type-1
 *     headers) and multi-function devices
 *   - BAR decoding and sizing (I/O, MEM32, MEM64, prefetchable)
 *   - capability list walk (MSI, MSI-X, PCI Express, Power Management,
 *     vendor-specific)
 *   - built-in driver binding (register + match + probe,
 *     explicitly no loadable modules, root-installable drivers may come
 *     later, but the core stays built-in)
 *   - resource assignment for unassigned BARs (hotplug / quirky firmware)
 *
 * Intel HD Graphics groundwork (why this rewrite exists):
 *   The iGPU lives at 00:02.0 (vendor 0x8086, class 0x0300).  A working
 *   driver needs: (1) its MMIO BARs decoded and enabled, done here,
 *   (2) MSI interrupts, capability parsing + register programming in
 *   msi.c; (3) LAPIC, minimal enablement in msi.c.  Actual mode setting
 *   (GTT/PPGTT, ring buffers) lands in a later release.
 */

/* Config space access
 * byte-granular read/write with automatic ECAM/legacy dispatch.          */
uint32_t pci_read_cfg(uint32_t bus, uint32_t dev, uint32_t fn,
                      uint32_t off, uint32_t size);
void     pci_write_cfg(uint32_t bus, uint32_t dev, uint32_t fn,
                       uint32_t off, uint32_t size, uint32_t val);

/* True when an ECAM (MMCONFIG) window is in use. */
bool pci_using_ecam(void);
uint64_t pci_ecam_base(void);

/* BAR model */
typedef enum {
    PCI_BAR_NONE = 0,
    PCI_BAR_IO,
    PCI_BAR_MEM32,
    PCI_BAR_MEM64,
} pci_bar_type_t;

typedef struct {
    pci_bar_type_t type;
    bool           prefetchable;
    uint64_t       base;   /* physical base address (0 = unassigned) */
    uint64_t       size;   /* decoded size (0 = invalid)             */
} pci_bar_t;

/* Capabilities (subset) */
typedef struct {
    bool     has_msi;
    uint16_t msi_off;          /* capability offset in config space   */
    bool     msi_64bit;        /* 64-bit message address capable      */
    bool     msi_per_vec_mask; /* per-vector masking supported        */

    bool     has_msix;
    uint16_t msix_off;
    uint16_t msix_table_size;  /* number of table entries             */
    uint8_t  msix_table_bar;   /* BIR: which BAR holds the table      */
    uint32_t msix_table_off;   /* byte offset within that BAR         */

    bool     has_pcie;         /* PCI Express capability present      */
    uint8_t  pcie_type;        /* port type (cap register bits 17:20) */

    bool     has_pm;
    uint16_t pm_off;
} pci_caps_t;

/* Device */
struct pci_driver;
typedef struct pci_driver pci_driver_t;

typedef struct pci_device {
    uint32_t bus, dev, fn;
    uint16_t vendor_id, device_id;
    uint16_t subsystem_vendor, subsystem_device;
    uint8_t  class_code, subclass, prog_if, revision;
    uint8_t  header_type;
    uint8_t  irq_pin, irq_line;      /* INTx routing info (legacy)      */
    uint32_t command;                /* cached command register         */
    uint32_t status;                 /* cached status register          */

    pci_bar_t  bars[6];              /* type-0 endpoints                */
    /* bridge windows (type-1) */
    bool     is_bridge;
    uint8_t  secondary_bus;
    uint64_t mem_base, mem_limit;
    uint64_t pf_mem_base, pf_mem_limit;

    pci_caps_t caps;
    pci_driver_t *driver;            /* bound driver or NULL            */
    bool valid;
} pci_device_t;

#define PCI_MAX_DEVICES 48

extern pci_device_t g_pci_devices[PCI_MAX_DEVICES];
extern int          g_pci_device_count;

/* Enumeration
 * Recursively walks all buses, sizes every BAR, walks the capability
 * list and fills g_pci_devices[].  Also probes ECAM.  Call once.       */
void pci_init(void);

/* Find helpers (NULL when absent) */
pci_device_t *pci_find_device(uint16_t vendor, uint16_t device);
pci_device_t *pci_find_class(uint8_t class_code, uint8_t subclass);
pci_device_t *pci_find_slot(uint32_t bus, uint32_t dev, uint32_t fn);

/* Command register management */
void pci_enable_mem(pci_device_t *d);        /* memory space decode      */
void pci_enable_io(pci_device_t *d);         /* I/O space decode         */
void pci_enable_busmaster(pci_device_t *d);  /* bus mastering            */
void pci_disable_intx(pci_device_t *d);      /* mask legacy INTx#        */
void pci_commit_command(pci_device_t *d);    /* write cached cmd back    */

/* Built-in driver model */
typedef int (*pci_probe_fn)(pci_device_t *dev);

struct pci_driver {
    const char   *name;
    uint16_t      vendor;          /* 0xFFFF = match any vendor       */
    uint16_t      device;          /* 0xFFFF = match any device       */
    uint8_t       class_code;      /* 0xFF    = match any class       */
    uint8_t       subclass;        /* 0xFF    = match any subclass    */
    pci_probe_fn  probe;           /* return 0 on success             */
    pci_driver_t *next;
};

void pci_register_driver(pci_driver_t *drv);
int  pci_bind_drivers(void);   /* match+probe all devices, returns bound count */

/* Reporting */
void pci_dump(void);           /* boot-console summary                 */
int  pci_format_list(char *buf, int bufsz);  /* text table for lspci   */

/* Config space constants */
#define PCI_CFG_VENDOR_ID   0x00
#define PCI_CFG_DEVICE_ID   0x02
#define PCI_CFG_COMMAND     0x04
#define PCI_CFG_STATUS      0x06
#define PCI_CFG_REVISION    0x08
#define PCI_CFG_PROG_IF     0x09
#define PCI_CFG_SUBCLASS    0x0A
#define PCI_CFG_CLASS       0x0B
#define PCI_CFG_CACHE_LINE  0x0C
#define PCI_CFG_LATENCY     0x0D
#define PCI_CFG_HEADER_TYPE 0x0E
#define PCI_CFG_BIST        0x0F
#define PCI_CFG_BAR0        0x10
#define PCI_CFG_BAR1        0x14
#define PCI_CFG_BAR2        0x18
#define PCI_CFG_BAR3        0x1C
#define PCI_CFG_BAR4        0x20
#define PCI_CFG_BAR5        0x24
#define PCI_CFG_CAP_PTR     0x34
#define PCI_CFG_IRQ_LINE    0x3C
#define PCI_CFG_IRQ_PIN     0x3D
/* type-1 bridges */
#define PCI_CFG_PRI_BUS     0x18
#define PCI_CFG_SEC_BUS     0x19
#define PCI_CFG_SUB_BUS     0x1A
#define PCI_CFG_MEM_BASE    0x20
#define PCI_CFG_MEM_LIMIT   0x22
#define PCI_CFG_PF_MEM_BASE 0x24
#define PCI_CFG_PF_MEM_LIMIT 0x26
/* type-0 subsystem ids */
#define PCI_CFG_SUBSYS_VEND 0x2C
#define PCI_CFG_SUBSYS_ID   0x2E

/* command register bits */
#define PCI_CMD_IO_SPACE    0x0001
#define PCI_CMD_MEM_SPACE   0x0002
#define PCI_CMD_BUS_MASTER  0x0004
#define PCI_CMD_INTX_DISABLE 0x0400
/* status register bits */
#define PCI_STATUS_CAP_LIST 0x0010

/* capability ids */
#define PCI_CAP_ID_PM      0x01
#define PCI_CAP_ID_MSI     0x05
#define PCI_CAP_ID_VEND    0x09
#define PCI_CAP_ID_MSIX    0x11
#define PCI_CAP_ID_PCIE    0x10
