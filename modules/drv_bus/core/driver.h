#pragma once
/*
 *  RMM driver framework
 *
 *  A small, Linux-style device/driver model.  Goals:
 *
 *   1. Drivers never poke hardware unless a device matched them.
 *   2. Buses (currently PCI) enumerate into a device list, drivers declare
 *      a match table, the core binds best-priority driver to each device.
 *   3. Built-in drivers register themselves through a linker section
 *      (.drivers) via BUILTIN_DRIVER() - exactly the same register API a
 *      future loadable driver module will use.  Nothing in the core has
 *      to know the driver set at compile time.
 *
 *  Future (planned): a driver module is an ELF relocatable
 *  object placed under /boot/drivers (e.g. mydrv.ko) and installed at
 *  runtime by root ("modload").  The module's init calls driver_register()
 *  with the very same struct driver, no kernel recompile needed.
 */
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* Buses */
typedef enum {
    BUS_NONE = 0,
    BUS_PCI,          /* real PCI function discovered by the bus core     */
    BUS_PLATFORM,     /* pseudo-device (e.g. multiboot-provided VBE LFB)  */
} bus_type_t;

#define PCI_ANY_ID  0xFFFFu

/* Match entry */
typedef struct driver_match {
    uint16_t vendor;        /* PCI_ANY_ID matches any */
    uint16_t device;
    uint8_t  class_;        /* PCI base class, 0xFF = any */
} driver_match_t;

struct device;
struct driver;

/* Device */
typedef struct device {
    const char   *name;                 /* filled by bus core ("0000:00:02.0") */
    bus_type_t    bus;

    /* PCI specifics (valid when bus == BUS_PCI) */
    uint8_t  pci_bus, pci_dev, pci_func;
    uint16_t pci_vendor, pci_device;
    uint8_t  pci_class, pci_subclass, pci_progif;
    uint32_t bar[6];                    /* physical base (flags masked off) */
    uint32_t bar_raw[6];                /* raw BAR incl. type bits (bit0=I/O) */
    uint64_t bar_size[6];
    uint8_t  irq_pin, irq_line;

    void     *priv;                     /* driver-private state             */
    struct driver *drv;                 /* bound driver                     */
    bool      bound;
} device_t;

/* Driver */
typedef struct driver {
    const char      *name;
    bus_type_t       bus;
    driver_match_t  *match_table;       /* NULL-terminated (vendor/device)  */
    int            (*probe)(device_t *dev);
    void           (*remove)(device_t *dev);
    int              priority;          /* higher wins when several match   */
    struct driver   *next;              /* core-managed list link           */
} driver_t;

/* Register a driver with the core.  Registers are ordered by priority and
 * immediately matched against any unbound devices already on the bus.
 * This is the entry point both for built-ins and future modules. */
void driver_register(driver_t *drv);

/* Walk the .drivers linker section and register every built-in driver. */
void driver_register_builtins(void);

/* RMM: register a module-local builtin range, each module's .drivers
 * section lives in its own image (see module-sdk/README.md). */
void driver_register_range(const driver_t *const *start,
                           const driver_t *const *end);

/* Enumerate the PCI bus: builds the device list, then binds and probes the
 * best matching driver for every function found. */
void bus_enumerate_pci(void);

/* Add a platform pseudo-device (e.g. the multiboot-provided VBE LFB).
 * Platform drivers match by `name`.  Call before bus_enumerate_pci(). */
device_t *platform_device_add(const char *name, void *priv);

/* Look up devices. */
device_t *device_find_pci(uint16_t vendor, uint16_t device);
device_t *device_find_class(uint8_t base_class);

/* Name of the driver bound to a PCI slot ("virtio-gpu"), or NULL.
 * Backed by the pci.c registry, used by lspci. */
const char *pci_bound_driver_name(uint32_t bus, uint32_t dev, uint32_t fn);

int driver_count(void);
int device_count(void);

/* Built-in registration macro
 * Usage (after defining the driver_t):
 *     BUILTIN_DRIVER(virtio_gpu_drv);
 * Places a pointer to the driver in the .drivers section which
 * driver_register_builtins() walks at boot.                                  */
#define BUILTIN_DRIVER(_drv_var)                                              \
    __attribute__((used, section(".drivers")))                                \
    static driver_t *const _drv_var##_entry = &_drv_var

/* Linker-provided section bounds (see linker.ld). */
extern const driver_t *const __drivers_start[];
extern const driver_t *const __drivers_end[];
