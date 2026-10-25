#pragma once
/*
 * percpu.h - per-CPU data via the GS base register
 *
 * Groundwork for SMP: every CPU owns a percpu_t describing itself and its
 * currently running task.  The BSP (and later each AP) points GS.base at
 * its own area with wrmsr(MSR_GS_BASE).  Kernel code accesses its own area
 * through GS-relative addressing, which stays correct no matter which CPU
 * is executing (and will compose with swapgs on the syscall path later).
 */
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <rmm/module.h>

struct process;                     /* exec/process.h */

/*
 * percpu_t - per-CPU area (GS-relative on x86_64, TPIDR_EL1 on arm64).
 *
 * Layout is ABI: the generated trampolines (build/generated/trampolines.S)
 * address the embedded rmm_cpu_context at hardcoded offsets that mirror
 * build/generated/offsets.txt.  Insert new fields only before `rmm`
 * after regenerating offsets, and keep the _Static_assert in arch.c true.
 */
typedef struct percpu {
    uint32_t          cpu_id;       /* logical index (0 = BSP)            */
    uint32_t          apic_id;      /* from LAPIC / CPUID                 */
    struct process   *current;      /* running task (NULL while idle)     */
    uint64_t          kernel_stack; /* top of this CPU's kernel stack     */
    uint64_t          irq_nesting;
    uint64_t          ticks;        /* per-cpu tick counter               */
    uint64_t          self;         /* points back at this struct         */
    uint64_t          syscall_ksp;  /* syscall-entry kernel stack top     */
    struct rmm_cpu_context rmm;     /* RMM module execution context  */
} percpu_t;

#define MSR_GS_BASE        0xC0000101   /* active GS base      */
#define MSR_KERNEL_GS_BASE 0xC0000102   /* user GS (swapgs)    */

#define SMP_MAX_CPUS 8

extern percpu_t g_percpu[SMP_MAX_CPUS];

/* Install `area` as this CPU's percpu area (GS-relative). */
void percpu_init_cpu(percpu_t *area, uint32_t cpu_id, uint32_t apic_id);

/* This CPU's area (GS-relative, no memory deref via linear address). */
percpu_t *percpu_this(void);

static inline void wrmsr(uint32_t msr, uint64_t value) {
    uint32_t lo = (uint32_t)value, hi = (uint32_t)(value >> 32);
    __asm__ volatile("wrmsr" : : "c"(msr), "a"(lo), "d"(hi));
}
static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}
