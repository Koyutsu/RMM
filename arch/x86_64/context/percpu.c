#include "cpu/percpu.h"
#include "cpu/cpu.h"
#include "console/print.h"

#include <stdint.h>
#include <stddef.h>

percpu_t g_percpu[SMP_MAX_CPUS];

void percpu_init_cpu(percpu_t *area, uint32_t cpu_id, uint32_t apic_id) {
    for (size_t i = 0; i < sizeof(*area) / 8; i++)
        ((uint64_t *)area)[i] = 0;
    area->cpu_id  = cpu_id;
    area->apic_id = apic_id;
    area->self    = (uint64_t)(uintptr_t)area;

    /* point GS at the area, after this GS:0 == area->cpu_id */
    wrmsr(MSR_GS_BASE, (uint64_t)(uintptr_t)area);
}

percpu_t *percpu_this(void) {
    percpu_t *p;
    __asm__ volatile("movq %%gs:%c1, %0"
                     : "=r"(p)
                     : "i"(offsetof(percpu_t, self)));
    return p;
}
