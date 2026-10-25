/*
 *  Local APIC (xAPIC mode)
 *
 *  Minimal enable path: map the MMIO page, switch the LAPIC on, register
 *  the spurious vector and provide EOI + the two IPIs the SMP trampoline
 *  boot needs (INIT / SIPI).  Interrupt sources (timer, error, LINT) are
 *  not routed yet, that is future work when IRQs move off the PIC.
 */
#include "cpu/lapic.h"
#include "cpu/cpu.h"
#include "cpu/percpu.h"
#include "cpu/idt.h"
#include "drivers/memory/vmm.h"
#include "console/print.h"

#include <stdint.h>
#include <stdbool.h>

#define LAPIC_DEFAULT_PHYS  0xFEE00000ull
#define LAPIC_MSR           0x1B

/* register offsets */
#define LAPIC_ID        0x020
#define LAPIC_VERSION   0x030
#define LAPIC_EOI       0x0B0
#define LAPIC_SPURIOUS  0x0F0
#define LAPIC_ICR_LOW   0x300
#define LAPIC_ICR_HIGH  0x310

#define SPURIOUS_VECTOR 0xFF

/* ICR delivery modes */
#define ICR_INIT        (5u << 8)
#define ICR_STARTUP     (6u << 8)
#define ICR_ASSERT      (1u << 14)
#define ICR_DEASSERT    (0u << 14)
#define ICR_ALL_EXCL    (3u << 18)

static volatile uint32_t *lapic = NULL;
static uint32_t            bsp_apic_id = 0;

static inline uint32_t lapic_read(uint32_t reg) {
    return lapic[reg / 4];
}
static inline void lapic_write(uint32_t reg, uint32_t value) {
    lapic[reg / 4] = value;
    __asm__ volatile("" ::: "memory");
}

/* naked IRET stub for the spurious interrupt */
static __attribute__((naked)) void spurious_stub(void) {
    __asm__ volatile("iretq");
}

void lapic_init(void) {
    if (!g_cpu.has_apic) {
        print_str("[lapic] not present (CPUID.1:EDX[9]=0)\n");
        return;
    }

    /* map the LAPIC MMIO page */
    if (!vmm_identity_map_range(LAPIC_DEFAULT_PHYS,
                                LAPIC_DEFAULT_PHYS + PAGE_SIZE,
                                PTE_PRESENT | PTE_WRITABLE)) {
        print_str("[lapic] MMIO map failed\n");
        return;
    }
    lapic = (volatile uint32_t *)(uintptr_t)LAPIC_DEFAULT_PHYS;

    /* global enable in IA32_APIC_BASE (keep xAPIC mode: do not set bit10) */
    uint64_t apic_msr = rdmsr(LAPIC_MSR);
    apic_msr |= (1ull << 11);
    apic_msr &= ~(1ull << 10);
    wrmsr(LAPIC_MSR, apic_msr);

    /* spurious vector: enable bit + IRET-able handler */
    idt_register_vector(SPURIOUS_VECTOR, spurious_stub);
    lapic_write(LAPIC_SPURIOUS, SPURIOUS_VECTOR | (1u << 8));

    bsp_apic_id = lapic_read(LAPIC_ID) >> 24;
    printf("[lapic] enabled, bsp apic id %u, version 0x%x\n",
           bsp_apic_id, lapic_read(LAPIC_VERSION) & 0xFF);
}

bool lapic_present(void) { return lapic != NULL; }

uint32_t lapic_id(void) {
    return lapic ? (lapic_read(LAPIC_ID) >> 24) : 0;
}

void lapic_eoi(void) {
    if (lapic) lapic_write(LAPIC_EOI, 0);
}

void lapic_send_init(void) {
    if (!lapic) return;
    lapic_write(LAPIC_ICR_HIGH, 0);                       /* shorthand dst */
    lapic_write(LAPIC_ICR_LOW,  ICR_INIT | ICR_DEASSERT | ICR_ALL_EXCL);
    lapic_write(LAPIC_ICR_HIGH, 0);
    lapic_write(LAPIC_ICR_LOW,  ICR_INIT | ICR_ASSERT   | ICR_ALL_EXCL);
}

void lapic_send_sipi(uint8_t vector) {
    if (!lapic) return;
    lapic_write(LAPIC_ICR_HIGH, 0);
    lapic_write(LAPIC_ICR_LOW,
                ICR_STARTUP | ICR_ASSERT | ICR_ALL_EXCL | (vector & 0xFF));
}
