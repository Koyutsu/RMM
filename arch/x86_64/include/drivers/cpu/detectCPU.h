#pragma once

#include "drivers/cpu/types.h"

void CpuName();

/* Cached CPUID brand string (stripped, NUL-terminated).  Never NULL,
 * safe to call from syscall context and from multiple boot stages. */
const char *cpu_brand_string(void);
/* FPU/SSE/AVX bring-up + x86-64 micro-architecture level
 * cpu_fpu_init(): one-time boot call, enables FPU+SSE (CR0/CR4) and,
 * on XSAVE+AVX CPUs, turns on OSXSAVE and programs XCR0 (YMM, and the
 * AVX-512 components on v4 CPUs) so VEX/EVEX user code does not #UD.
 * fpu_save/fpu_restore(_init): per-task extended-state snapshots used
 * by the scheduler around every switch (4096-byte 64-byte-aligned
 * areas, see process_t.fpu_area).
 * cpu_x86_64_level(): distro x86-64-psABI level of this CPU (1..4):
 *   v2 = SSE3/SSSE3/SSE4.1/SSE4.2/POPCNT/CMPXCHG16B
 *   v3 = + AVX/AVX2/FMA/BMI1/BMI2/F16C/LZCNT/MOVBE
 *   v4 = + AVX512F/CD/BW/DQ/VL
 * cpu_flags_string(): /proc/cpuinfo-style flag list. */
void cpu_fpu_init(void);
bool fpu_xsave_available(void);
uint64_t fpu_xcr0_mask(void);
int      cpu_x86_64_level(void);
const char *cpu_flags_string(void);
void fpu_save(void *area);
void fpu_restore_init(void *area);
void fpu_restore(void *area);
