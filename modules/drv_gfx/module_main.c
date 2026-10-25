/*
 * module_main.c - drv_gfx module entry (lifecycle).
 * Registers this module's builtin PCI drivers with the driver core
 * (drv_bus) through the explicit range API, the .drivers section is
 * module-local in RMM, not a kernel-wide linker section.
 */
#include <rmm/module.h>
#include "drivers/core/driver.h"

extern const driver_t *const __rmm_mod_drivers_start[];
extern const driver_t *const __rmm_mod_drivers_end[];

int module_entry(void) {
    driver_register_range(__rmm_mod_drivers_start, __rmm_mod_drivers_end);
    return 0;
}
