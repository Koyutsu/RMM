#pragma once
/*
 * cpu.h - CPU identification & feature detection
 *
 * Replaces the old drivers/cpu/detectCPU.c.  Everything the SMP and
 * driver layers need to know about the running CPU is probed once here.
 */
#include <stdint.h>
#include <stdbool.h>

typedef struct cpu_info {
    char     vendor[13];          /* NUL-terminated, e.g. GenuineIntel  */
    char     brand[49];           /* marketing name                     */
    uint8_t  family;
    uint8_t  model;
    uint8_t  stepping;
    uint32_t phys_addr_bits;
    uint32_t lin_addr_bits;

    /* feature flags we care about */
    bool     has_apic;            /* CPUID.1:EDX[9]                     */
    bool     has_x2apic;          /* CPUID.1:ECX[21]                    */
    bool     has_sse42;
    bool     has_xsave;
    bool     has_rdtscp;
    bool     has_invtsc;
    bool     has_pat;
    bool     has_pge;
    bool     has_smep;
    bool     has_smap;
    bool     has_nx;              /* CPUID.8x1:EDX[20]                  */
    bool     has_longmode;

    /* FPU/XSAVE + x86-64 ABI level (v1..v4)
     * x86_64_level is the GO/NO-GO level programs compiled with
     * -march=x86-64-v2/v3/v4 can rely on (ACPI-ish naming from the
     * distro x86-64 micro-architecture levels):
     *   v2  SSE3, SSSE3, SSE4.1/2, POPCNT, CMPXCHG16B
     *   v3  + AVX, AVX2, FMA, BMI1/2, F16C, LZCNT, MOVBE
     *   v4  + AVX512F/CD/BW/DQ/VL
     * has_avx / has_avx512f gate whether XCR0 gets the wide state bits
     * (see cpu_fpu_init) so YMM/ZMM context actually survives switches. */
    bool     has_osxsave;         /* XSAVE-capable (CPUID.1:ECX[26])    */
    bool     has_avx;             /* CPUID.1:ECX[28]                    */
    bool     has_avx2;            /* CPUID.7:EBX[5]                     */
    bool     has_avx512f;         /* CPUID.7:EBX[16]                    */
    bool     has_fma;             /* CPUID.1:ECX[12]                    */
    bool     has_movbe;           /* CPUID.1:ECX[22]                    */
    int      x86_64_level;        /* 1, 2, 3 or 4 (baseline = 1)        */
    uint64_t fpu_xcr0_mask;       /* what xsetbv(0,...) enabled           */
    char     cpu_flags[192];      /* /proc/cpuinfo-style flags string   */

    /* topology */
    uint32_t cores_per_package;    /* CPUID.4:*EAX[31:26]+1 or 0xB       */
    uint32_t initial_apic_id;      /* CPUID.1:EBX[31:24]                 */
} cpu_info_t;

extern cpu_info_t g_cpu;

void cpu_detect(void);                 /* probe everything, log a summary  */
const char *cpu_brand(void);           /* trimmed brand string             */
const char *cpu_vendor(void);

/* FPU/SSE/AVX bring-up + per-task state
 * cpu_fpu_init(): enable FPU+SSE unconditionally (CR0.EM/TS clear, MP
 * set, CR4.OSFXSR|OSXMMEXCPT) and, when the CPU has XSAVE+AVX, switch
 * on OSXSAVE and program XCR0 so YMM (and ZMM on v4 CPUs) context is
 * legal in user mode.  Call once during boot after cpu_detect().
 * fpu_save/fpu_restore snapshot the full extended state (xsave when
 * available, fxsave otherwise) into a 4096-byte 64-byte-aligned area -
 * the scheduler calls them around every switch so SSE/AVX registers
 * never leak across processes (required for the dynamic linker, which
 * runs before any user code could re-initialise them).             */
void cpu_fpu_init(void);
bool fpu_xsave_available(void);
uint64_t fpu_xcr0_mask(void);
void fpu_save(void *area);
void fpu_restore_init(void *area);   /* first-use: fninit + ldmxcsr   */
void fpu_restore(void *area);        /* xrstor/fxrstor a saved image  */
