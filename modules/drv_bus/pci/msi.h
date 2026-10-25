#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "drivers/pci/pci.h"

/*
 * msi - Message Signaled Interrupts + minimal Local APIC enablement
 *
 * Intel HD Graphics (and every modern PCIe device) delivers interrupts
 * as memory writes to the LAPIC's address space instead of legacy INTx
 * lines.  Two halves:
 *
 *   1. LAPIC - enable the CPU's Local APIC (MSR 0x1B), program a spurious
 *               vector, remember the EOI register.  Vectors stay routed
 *               through the 8259 PIC for the built-in drivers (PIT, PS/2,
 *               serial); LAPIC delivery becomes the primary path in the
 *               upcoming IRQ refactor.
 *   2. MSI/X - program a device's MSI/MSI-X capability: message address
 *               0xFEE00000 | destination, data = vector.  Once enabled,
 *               the device stops asserting INTx# (we also set the
 *               INTX_DISABLE command bit).
 *
 * Single-CPU assumption for destination APIC id 0, physical
 * delivery mode, fixed trigger.  SMP will extend this per-CPU.
 */

/* Enable the Local APIC on the boot CPU.  Returns true when the LAPIC
 * is present and was enabled (CPUID-flagged).  Safe to call once. */
bool lapic_init(void);

/* LAPIC physical base (0xFEE00000 after standard enable; 0 if absent). */
uint64_t lapic_base(void);

/* Read LAPIC identification (x2APIC-safe read path). */
uint32_t lapic_id(void);

/* Signal end-of-interrupt for LAPIC-delivered vectors (MSI).  The PIC
 * handlers keep using pic_eoi(); call lapic_eoi() only for MSI vectors. */
void lapic_eoi(void);

/* Enable MSI on a device for the given vector (0-255).  Returns 0 on
 * success, -errno style negative on failure (ENOSYS: no MSI cap). */
int pci_enable_msi(pci_device_t *d, uint8_t vector);

/* Enable MSI-X and program entry `index` with the given vector.
 * Requires the MSI-X table BAR to be identity-mapped (it is for the
 * low-4-GiB BARs the kernel maps).  Returns 0 or negative errno. */
int pci_enable_msix(pci_device_t *d, uint16_t index, uint8_t vector);

/* Disable both mechanisms (restore INTx). */
void pci_disable_msi(pci_device_t *d);
