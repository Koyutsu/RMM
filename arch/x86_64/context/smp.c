/*
 *  SMP bring-up
 *
 *  Sequence (classic Intel multi-processor boot protocol):
 *    1. copy the self-contained trampoline to physical 0x7000
 *    2. stash the BSP cr3 for the APs to reuse
 *    3. INIT IPI (all excluding self) + 10 ms
 *    4. two SIPIs with vector 0x07 (start at 0x7000), 200 us apart
 *    5. wait until every MADT-listed AP increments the arrived counter
 *
 *  APs end up parked in a cli/hlt loop with their LAPIC id recorded -
 *  the scheduler/IPI work will pick them up from there later.
 */
#include "cpu/smp.h"
#include "cpu/cpu.h"
#include "cpu/lapic.h"
#include "cpu/madt.h"
#include "cpu/percpu.h"
#include "drivers/timer/pit.h"
#include "console/print.h"

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define TRAMP_BASE       0x7000u
#define TRAMP_AP_ARRIVED 0x7F00u
#define TRAMP_AP_READY   0x7F04u
#define TRAMP_BSP_CR3    0x7F08u
#define SIPI_VECTOR      0x07

static uint32_t ap_online = 0;

extern const uint8_t smp_trampoline_bin[];
extern const uint8_t smp_trampoline_bin_end[];

uint32_t smp_cpu_count(void) {
    return g_madt.found && g_madt.lapic_count ? g_madt.lapic_count : 1;
}

uint32_t smp_ap_online(void) { return ap_online; }

static void pit_wait_ms(uint64_t ms) {
    uint64_t start = pit_ticks();
    while (pit_ticks() - start < ms) __asm__ volatile("pause");
}

static void pit_wait_us(uint64_t us) {
    /* tick = 1 ms, busy-wait sub-tick precision isn't needed here */
    (void)us;
    uint64_t spin = 20000;                 /* ~200 us at TCG speeds */
    while (spin--) __asm__ volatile("pause");
}

void smp_init(void) {
    uint32_t ncpu = smp_cpu_count();
    if (ncpu <= 1 || !lapic_present() || !g_cpu.has_apic) {
        print_str("[smp] single-processor machine, AP bring-up skipped\n");
        return;
    }

    /* trampoline page is below 1 MiB (reserved by the PMM) */
    size_t blob_size = (size_t)(smp_trampoline_bin_end - smp_trampoline_bin);
    if (blob_size > 0xFE0) {
        print_str("[smp] trampoline too large!\n");
        return;
    }
    uint8_t *dst = (uint8_t *)(uintptr_t)TRAMP_BASE;
    for (size_t i = 0; i < blob_size; i++) dst[i] = smp_trampoline_bin[i];

    uint32_t *arrived = (uint32_t *)(uintptr_t)TRAMP_AP_ARRIVED;
    uint32_t *ready   = (uint32_t *)(uintptr_t)TRAMP_AP_READY;
    uint32_t *cr3out  = (uint32_t *)(uintptr_t)TRAMP_BSP_CR3;
    *arrived = 0;
    *ready   = 0;

    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    *cr3out = (uint32_t)cr3;

    printf("[smp] waking %u application processor(s), trampoline @ 0x%x\n",
           ncpu - 1, TRAMP_BASE);

    /* INIT-SIPI sequence */
    lapic_send_init();
    pit_wait_ms(10);
    lapic_send_sipi(SIPI_VECTOR);
    pit_wait_us(200);
    lapic_send_sipi(SIPI_VECTOR);

    /* wait up to 300 ms for everyone to arrive */
    uint64_t start = pit_ticks();
    while (*ready < ncpu - 1) {
        if (pit_ticks() - start > 300) break;
        __asm__ volatile("pause");
    }
    ap_online = *arrived;

    uint32_t *ids = (uint32_t *)(uintptr_t)0x7E00;
    for (uint32_t i = 0; i < ap_online && i < 8; i++)
        printf("[smp] ap%u online (apic id %u), parked\n", i, ids[i]);

    if (ap_online == ncpu - 1)
        printf("[smp] all %u CPU(s) online (BSP + %u AP(s))\n", ncpu, ap_online);
    else
        printf("[smp] %u/%u AP(s) online (timeout or not present)\n",
               ap_online, ncpu - 1);
}
