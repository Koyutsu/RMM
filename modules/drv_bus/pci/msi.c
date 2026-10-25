#include "drivers/pci/msi.h"
#include "drivers/pci/io.h"
#include "console/print.h"
#include "drivers/memory/vmm.h"
#include <stdint.h>
#include <stdbool.h>

/* errno mirrors (kept local to avoid pulling the whole syscall header) */
#define ENOSYS_KERNEL 38
#define EINVAL_KERNEL 22
#define ENODEV_KERNEL 19
#define EFAULT_KERNEL 14

/*
 * Local APIC (minimal, boot CPU only)
 * Default physical base 0xFEE00000 (IA-32 spec).  Present iff CPUID.01h
 * EDX bit 9.  Enabled via MSR 0x1B bit 11.  Registers we touch:
 *   0x020  LAPIC ID
 *   0x0F0  Spurious Interrupt Vector (bit 8 = APIC enabled)
 *   0x0B0  End Of Interrupt
 */

#define MSR_APIC_BASE       0x1B
#define LAPIC_DEFAULT_BASE  0xFEE00000ULL

#define LAPIC_ID_OFF        0x020
#define LAPIC_EOI_OFF       0x0B0
#define LAPIC_SPURIOUS_OFF  0x0F0
#define LAPIC_LVT_TIMER_OFF 0x320

static uint64_t apic_mmio_base = 0;
static bool     apic_ready     = false;

static uint32_t rdmsr32(uint32_t msr, uint32_t *hi) {
    uint32_t lo;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(*hi) : "c"(msr));
    return lo;
}

static uint32_t lapic_read(uint32_t off) {
    volatile uint32_t *r = (volatile uint32_t *)(uintptr_t)(apic_mmio_base + off);
    return *r;
}

static void lapic_write(uint32_t off, uint32_t val) {
    volatile uint32_t *r = (volatile uint32_t *)(uintptr_t)(apic_mmio_base + off);
    *r = val;
}

bool lapic_init(void) {
    if (apic_ready) return true;

    /* CPUID leaf 1: EDX bit 9 = APIC present */
    uint32_t eax, ebx, ecx, edx;
    __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                     : "a"(1));
    if (!(edx & (1u << 9))) return false;

    /* APIC base MSR: bits 35:12 base, bit 11 global enable, bit 10 x2APIC */
    uint32_t lo, hi;
    rdmsr32(MSR_APIC_BASE, &hi);
    uint64_t base = (((uint64_t)hi & 0xFu) << 32) | (lo & 0xFFFFF000ULL);
    if (hi & (1u << 10)) {
        /* x2APIC mode already active (firmware), stay away from MMIO
         * until the x2APIC MSR access path exists, report not-ready.   */
        print_str("  LAPIC: x2APIC mode detected (deferred)\n");
        return false;
    }
    if (!base) base = LAPIC_DEFAULT_BASE;

    apic_mmio_base = base;

    /* the LAPIC MMIO page lives far outside RAM, map it explicitly
     * before any register access (identity map, writable)            */
    if (!vmm_identity_map_range(base, base + 0x1000,
                                PTE_PRESENT | PTE_WRITABLE)) {
        print_str("  LAPIC: failed to map MMIO page\n");
        return false;
    }

    /* global enable */
    uint32_t new_lo = (lo & 0xFFFFF000u) | (1u << 11);
    __asm__ volatile("wrmsr" :: "c"(MSR_APIC_BASE), "a"(new_lo), "d"(hi));

    /* enable spurious vector 0xFF (delivery-masked until IRQ refactor
     * routes LAPIC vectors into the IDT, the register is programmed so
     * MSI devices can start talking to a live LAPIC)                    */
    lapic_write(LAPIC_SPURIOUS_OFF, 0xFFu | (1u << 8));

    apic_ready = true;
    return true;
}

uint64_t lapic_base(void) { return apic_ready ? apic_mmio_base : 0; }

uint32_t lapic_id(void) {
    return apic_ready ? (lapic_read(LAPIC_ID_OFF) >> 24) : 0;
}

void lapic_eoi(void) {
    if (apic_ready) lapic_write(LAPIC_EOI_OFF, 0);
}

/* MSI capability programming */
/*
 * MSI capability layout (offset relative to cap):
 *   +0  cap id/next
 *   +2  Message Control      (bit0 MSI-enable, bit6 per-vec mask, bit7 64-bit)
 *   +4  Message Address Low  (bits 31:20 base, 19:12 dest id, 3 RH, 2 RP)
 *   +8  Message Address High (64-bit only)
 *   +8/+12 Message Data      (bits 7:0 vector, 10:8 delivery, 14 level, 15 trig)
 */
#define MSI_MSG_ADDR_LOW   0x4
#define MSI_MSG_ADDR_HIGH  0x8
#define MSI_MSG_DATA_32    0x8
#define MSI_MSG_DATA_64    0xC

#define LAPIC_MSI_ADDR_LOW  0xFEE00000u   /* dest 0, no redirection */

int pci_enable_msi(pci_device_t *d, uint8_t vector) {
    if (!d || !d->caps.has_msi) return -(int)ENOSYS_KERNEL;
    uint32_t bus = d->bus, dev = d->dev, fn = d->fn;
    uint16_t off = d->caps.msi_off;

    /* message address: fixed delivery, physical mode, dest APIC 0 */
    pci_write_cfg(bus, dev, fn, off + MSI_MSG_ADDR_LOW, 4, LAPIC_MSI_ADDR_LOW);
    if (d->caps.msi_64bit)
        pci_write_cfg(bus, dev, fn, off + MSI_MSG_ADDR_HIGH, 4, 0);

    /* message data: vector, delivery mode 0 (fixed), trigger 0 (edge) */
    uint32_t data_off = d->caps.msi_64bit ? MSI_MSG_DATA_64 : MSI_MSG_DATA_32;
    pci_write_cfg(bus, dev, fn, off + data_off, 2, vector & 0xFF);

    /* enable MSI */
    uint16_t ctrl = (uint16_t)pci_read_cfg(bus, dev, fn, off + 2, 2);
    ctrl |= 0x0001;                       /* MSI enable */
    pci_write_cfg(bus, dev, fn, off + 2, 2, ctrl);

    /* stop the legacy line */
    pci_disable_intx(d);
    return 0;
}

void pci_disable_msi(pci_device_t *d) {
    if (!d || !d->caps.has_msi) return;
    uint16_t off = d->caps.msi_off;
    uint16_t ctrl = (uint16_t)pci_read_cfg(d->bus, d->dev, d->fn, off + 2, 2);
    ctrl &= ~0x0001u;
    pci_write_cfg(d->bus, d->dev, d->fn, off + 2, 2, ctrl);
}

/* MSI-X */
/*
 * MSI-X table entry: 16 bytes
 *   +0  Message Address (must not be all-1s = masked)
 *   +8  Message Address High
 *   +12 Message Data    (bit 0 = reserved-mask when vector masked)
 */
#define MSIX_ENTRY_SIZE     16
#define MSIX_MSG_ADDR_LOW   0
#define MSIX_MSG_DATA       12

int pci_enable_msix(pci_device_t *d, uint16_t index, uint8_t vector) {
    if (!d || !d->caps.has_msix) return -(int)ENOSYS_KERNEL;
    if (index >= d->caps.msix_table_size) return -(int)EINVAL_KERNEL;
    if (d->bars[d->caps.msix_table_bar].type == PCI_BAR_NONE)
        return -(int)ENODEV_KERNEL;

    uint64_t table_base = d->bars[d->caps.msix_table_bar].base
                        + d->caps.msix_table_off
                        + (uint64_t)index * MSIX_ENTRY_SIZE;
    if (table_base + MSIX_ENTRY_SIZE > 0x40000000ULL)
        return -(int)EFAULT_KERNEL;   /* outside identity map */

    volatile uint32_t *entry = (volatile uint32_t *)(uintptr_t)table_base;
    entry[MSIX_MSG_ADDR_LOW / 4] = LAPIC_MSI_ADDR_LOW;
    entry[1] = 0;                                  /* addr high */
    entry[MSIX_MSG_DATA / 4] = vector & 0xFF;

    /* function enable (bit 31 of Message Control) */
    uint16_t ctrl = (uint16_t)pci_read_cfg(d->bus, d->dev, d->fn,
                                           d->caps.msix_off + 2, 2);
    ctrl |= 0x8000;
    pci_write_cfg(d->bus, d->dev, d->fn, d->caps.msix_off + 2, 2, ctrl);

    pci_disable_intx(d);
    return 0;
}
