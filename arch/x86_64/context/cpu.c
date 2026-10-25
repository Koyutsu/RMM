#include "cpu/cpu.h"
#include "console/print.h"

#include <stdint.h>
#include <stddef.h>

cpu_info_t g_cpu;

static inline void cpuid(uint32_t leaf, uint32_t subleaf,
                         uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d) {
    __asm__ volatile("cpuid"
                     : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
                     : "0"(leaf), "2"(subleaf));
}

static void read_vendor(void) {
    uint32_t a, b, c, d;
    cpuid(0, 0, &a, &b, &c, &d);
    *(uint32_t *)(g_cpu.vendor + 0)  = b;
    *(uint32_t *)(g_cpu.vendor + 4)  = d;
    *(uint32_t *)(g_cpu.vendor + 8)  = c;
    g_cpu.vendor[12] = '\0';
}

static void read_brand(void) {
    uint32_t *p = (uint32_t *)g_cpu.brand;
    for (uint32_t leaf = 0x80000002; leaf <= 0x80000004; leaf++)
        cpuid(leaf, 0, &p[(leaf - 0x80000002) * 4 + 0],
                        &p[(leaf - 0x80000002) * 4 + 1],
                        &p[(leaf - 0x80000002) * 4 + 2],
                        &p[(leaf - 0x80000002) * 4 + 3]);
    g_cpu.brand[48] = '\0';
}

void cpu_detect(void) {
    uint32_t a, b, c, d;

    read_vendor();

    cpuid(1, 0, &a, &b, &c, &d);
    g_cpu.stepping         = a & 0xF;
    g_cpu.model            = ((a >> 4) & 0xF) | ((a >> 16 & 0xF) << 4);
    g_cpu.family           = ((a >> 8) & 0xF) | ((a >> 20 & 0xFF) << 4);
    g_cpu.initial_apic_id  = b >> 24;
    g_cpu.has_apic         = (d >> 9)  & 1;
    g_cpu.has_pat          = (d >> 16) & 1;
    g_cpu.has_pge          = (d >> 13) & 1;
    g_cpu.has_x2apic       = (c >> 21) & 1;
    g_cpu.has_sse42        = (c >> 20) & 1;
    g_cpu.has_xsave        = (c >> 26) & 1;
    g_cpu.has_rdtscp       = (c >> 27) & 1;
    g_cpu.has_smep         = (c >> 7)  & 1;
    g_cpu.has_smap         = (c >> 23) & 1;
    g_cpu.has_osxsave      = g_cpu.has_xsave;
    g_cpu.has_avx          = ((c >> 27) & 1) && ((c >> 28) & 1);
    g_cpu.has_fma          = (c >> 12) & 1;
    g_cpu.has_movbe        = (c >> 22) & 1;
    uint32_t f_sse3   = (c >> 0)  & 1;
    uint32_t f_ssse3  = (c >> 9)  & 1;
    uint32_t f_sse41  = (c >> 19) & 1;
    uint32_t f_popcnt = (c >> 23) & 1;
    uint32_t f_cx16   = (c >> 13) & 1;
    uint32_t f_f16c   = (c >> 29) & 1;

    cpuid(0x80000001, 0, &a, &b, &c, &d);
    g_cpu.has_nx        = (d >> 20) & 1;
    g_cpu.has_longmode  = (d >> 29) & 1;
    g_cpu.has_invtsc    = (d >> 8)  & 1;
    uint32_t f_lzcnt    = (d >> 5)  & 1;

    /* CPUID leaf 7 (structured features) - AVX2/AVX-512/BMI gates */
    uint32_t f_avx2 = 0, f_bmi1 = 0, f_bmi2 = 0;
    uint32_t f_avx512f = 0, f_avx512dq = 0, f_avx512cd = 0,
             f_avx512bw = 0, f_avx512vl = 0;
    {
        uint32_t max_leaf;
        cpuid(0, 0, &max_leaf, &b, &c, &d);
        if (max_leaf >= 7) {
            cpuid(7, 0, &a, &b, &c, &d);
            f_avx2      = (b >> 5)  & 1;
            f_bmi1      = (b >> 3)  & 1;
            f_bmi2      = (b >> 8)  & 1;
            f_avx512f   = (b >> 16) & 1;
            f_avx512dq  = (b >> 17) & 1;
            f_avx512cd  = (b >> 28) & 1;
            f_avx512bw  = (b >> 30) & 1;
            f_avx512vl  = (b >> 31) & 1;
        }
    }
    g_cpu.has_avx2    = f_avx2 != 0;
    g_cpu.has_avx512f = f_avx512f != 0;

    /* x86-64 micro-architecture level (distro x86-64-psABI v1..v4) */
    bool v2 = f_sse3 && f_ssse3 && f_sse41 && g_cpu.has_sse42 &&
              f_popcnt && f_cx16;
    bool v3 = v2 && g_cpu.has_avx && g_cpu.has_avx2 && g_cpu.has_fma &&
              f_bmi1 && f_bmi2 && f_f16c && f_lzcnt && g_cpu.has_movbe;
    bool v4 = v3 && f_avx512f && f_avx512cd && f_avx512bw &&
              f_avx512dq && f_avx512vl;
    g_cpu.x86_64_level = v4 ? 4 : (v3 ? 3 : (v2 ? 2 : 1));

    /* /proc/cpuinfo + hwprobe flags string (subset programs care about) */
    {
        char *s = g_cpu.cpu_flags;
        size_t cap = sizeof(g_cpu.cpu_flags), n = 0;
        #define CF_ADD(name) do { \
            if (n) s[n++] = ' '; \
            const char *f = name; \
            while (*f && n + 1 < cap) s[n++] = *f++; \
        } while (0)
        CF_ADD("fpu"); CF_ADD("sse2");
        if (f_sse3)   CF_ADD("sse3");
        if (f_ssse3)  CF_ADD("ssse3");
        if (f_sse41)  CF_ADD("sse4_1");
        if (g_cpu.has_sse42) CF_ADD("sse4_2");
        if (f_popcnt) CF_ADD("popcnt");
        if (f_cx16)   CF_ADD("cx16");
        if (g_cpu.has_avx)   CF_ADD("avx");
        if (f_f16c)   CF_ADD("f16c");
        if (g_cpu.has_fma)   CF_ADD("fma");
        if (g_cpu.has_movbe) CF_ADD("movbe");
        if (f_lzcnt)  CF_ADD("abm");
        if (f_bmi1)   CF_ADD("bmi1");
        if (f_bmi2)   CF_ADD("bmi2");
        if (f_avx2)   CF_ADD("avx2");
        if (g_cpu.has_avx512f) CF_ADD("avx512f");
        if (f_avx512cd) CF_ADD("avx512cd");
        if (f_avx512bw) CF_ADD("avx512bw");
        if (f_avx512dq) CF_ADD("avx512dq");
        if (f_avx512vl) CF_ADD("avx512vl");
        if (g_cpu.has_xsave) CF_ADD("xsave");
        if (g_cpu.has_osxsave && g_cpu.has_avx) CF_ADD("xsavec");
        s[n] = '\0';
    }

    /* address sizes */
    cpuid(0x80000008, 0, &a, &b, &c, &d);
    g_cpu.phys_addr_bits = a & 0xFF;
    g_cpu.lin_addr_bits  = (a >> 8) & 0xFF;

    /* cores per package (leaf 4 preferred, 0xB fallback) */
    g_cpu.cores_per_package = 1;
    if (g_cpu.vendor[0] == 'G' /* GenuineIntel */ && g_cpu.family >= 6) {
        cpuid(4, 0, &a, &b, &c, &d);
        if ((a >> 26) != 0 && ((a >> 26) & 0x3F) != 0x3F)
            g_cpu.cores_per_package = ((a >> 26) & 0x3F) + 1;
    } else if (g_cpu.vendor[0] == 'A' /* AuthenticAMD */) {
        cpuid(0x80000008, 0, &a, &b, &c, &d);
        if ((c & 0xFF) > 0) g_cpu.cores_per_package = (c & 0xFF) + 1;
    }

    read_brand();

    /* trim leading spaces of the brand */
    const char *br = g_cpu.brand;
    while (*br == ' ') br++;
    print_str("[cpu] ");
    print_str(br);
    print_str("\n");
    printf("[cpu] vendor=%s family=%u model=%u stepping=%u apic_id=%u cores/pkg=%u\n",
           g_cpu.vendor, g_cpu.family, g_cpu.model, g_cpu.stepping,
           g_cpu.initial_apic_id, g_cpu.cores_per_package);
    printf("[cpu] apic=%u x2apic=%u nx=%u xsave=%u paddr=%u bits\n",
           g_cpu.has_apic, g_cpu.has_x2apic, g_cpu.has_nx, g_cpu.has_xsave,
           g_cpu.phys_addr_bits);
    printf("[cpu] x86-64-level=v%u avx=%u avx2=%u avx512f=%u\n",
           g_cpu.x86_64_level, (unsigned)g_cpu.has_avx,
           (unsigned)g_cpu.has_avx2, (unsigned)g_cpu.has_avx512f);
}

const char *cpu_brand(void)  { return g_cpu.brand; }
const char *cpu_vendor(void) { return g_cpu.vendor; }

/*
 *  FPU/SSE/AVX bring-up + per-task extended-state save
 *
 *  Earlier the kernel enabled the FPU once at boot (fninit+ldmxcsr)
 *  and never saved extended state across context switches.  That survived
 *  static single-purpose processes, but the dynamic linker (ld-musl)
 *  self-relocates with SSE before any user code runs, and AVX upper
 *  halves (YMM/ZMM) silently leak across preemptions, so the kernel now makes
 *  extended state a first-class part of the context.
 *
 *  XCR0 program:  x87|SSE always, YMM when AVX (CPUID.1:ECX[27] OSXSAVE
 *  capability + ECX[28]), opmask|ZMM_Hi256|Hi16_ZMM when AVX-512F.
 *  The same mask is used for xsave/xrstor so the on-disk image layout is
 *  always the compact XSAVE area format.                         */

static bool     g_fpu_xsave;      /* use xsave/xrstor (vs fxsave)      */
static uint64_t g_fpu_xcr0;       /* active XCR0 value                 */

bool     fpu_xsave_available(void) { return g_fpu_xsave; }
uint64_t fpu_xcr0_mask(void)       { return g_fpu_xcr0;   }

static inline uint64_t rdcr0(void) {
    uint64_t v; __asm__ volatile("mov %%cr0, %0" : "=r"(v)); return v;
}
static inline void wrcr0(uint64_t v) { __asm__ volatile("mov %0, %%cr0" :: "r"(v)); }
static inline uint64_t rdcr4(void) {
    uint64_t v; __asm__ volatile("mov %%cr4, %0" : "=r"(v)); return v;
}
static inline void wrcr4(uint64_t v) { __asm__ volatile("mov %0, %%cr4" :: "r"(v)); }

/* CR4 write that keeps the "maintain CR4.TSD/DE across wr" contract of
 * older CPUs happy: some 486-era semantics fault a plain write that
 * leaves reserved bits, read-modify-write only the bits we own. */
#define CR4_OSFXSR      (1ULL << 9)
#define CR4_OSXMMEXCPT  (1ULL << 10)
#define CR4_OSXSAVE     (1ULL << 18)

static inline void xsetbv(uint32_t reg, uint64_t val) {
    uint32_t lo = (uint32_t)val, hi = (uint32_t)(val >> 32);
    __asm__ volatile("xsetbv" :: "a"(lo), "d"(hi), "c"(reg));
}

void cpu_fpu_init(void) {
    /* CR0: TS clear (state available now), EM clear (no #UD trap), MP set
     * (FWAIT honours TS - moot with TS clear but keep the sane combo). */
    uint64_t cr0 = rdcr0();
    cr0 &= ~(1ULL << 2);          /* EM  = 0 */
    cr0 &= ~(1ULL << 3);          /* TS  = 0 */
    cr0 |=  (1ULL << 1);          /* MP  = 1 */
    wrcr0(cr0);

    /* CR4: OSFXSR (SSE instructions usable + FXSAVE saves XMM) and
     * OSXMMEXCPT (SSE FP exceptions trap properly instead of #GP). */
    uint64_t cr4 = rdcr4();
    cr4 |= CR4_OSFXSR | CR4_OSXMMEXCPT;

    /* XSAVE family: enable OSXSAVE and program XCR0 to advertise which
     * state components user mode may touch (AVX/AVX-512 need this -
     * without XCR0.YMM every VEX-encoded instruction #UDs). */
    g_fpu_xsave = false;
    g_fpu_xcr0  = 0x3ULL;         /* x87 + SSE minimum */
    if (g_cpu.has_xsave && g_cpu.has_avx) {
        cr4 |= CR4_OSXSAVE;
        uint64_t mask = 0x7ULL;   /* x87 | SSE | YMM */
        if (g_cpu.has_avx512f) mask |= 0xE0ULL;   /* opmask|ZMM_Hi256|Hi16 */
        g_fpu_xcr0 = mask;
    }
    wrcr4(cr4);
    if (g_fpu_xsave) xsetbv(0, g_fpu_xcr0);

    __asm__ volatile("fninit");
    uint32_t mxcsr = 0x1F80;
    __asm__ volatile("ldmxcsr %0" : : "m"(mxcsr));
}

/* xsave/xrstor need EDX:EAX = requested-feature bitmap, keep it in one
 * register pair the asm can load from a global. */
void fpu_save(void *area) {
    uint64_t mask = g_fpu_xcr0;
    if (g_fpu_xsave) {
        __asm__ volatile(
            "movq  %1, %%rdx\n\t"
            "movq  %%rdx, %%rax\n\t"
            "shrq  $32, %%rdx\n\t"
            "xsaveq (%0)\n\t"
            :: "r"(area), "r"(mask) : "rax", "rdx", "memory");
    } else {
        __asm__ volatile("fxsaveq (%0)" :: "r"(area) : "memory");
    }
}

/* Fresh task: initialise a neutral extended-state image in area so the
 * first xrstor loads defined values (fninit + clean mxcsr + zero XMM). */
void fpu_restore_init(void *area) {
    __builtin_memset(area, 0, 4096);
    if (g_fpu_xsave) {
        /* FXSAVE-format legacy region first, then mark all enabled
         * components present in the XSAVE header so xrstor restores
         * the whole enabled set. */
        __asm__ volatile("fxsaveq (%0)" :: "r"(area) : "memory");
        uint64_t *hdr = (uint64_t *)((uintptr_t)area + 512);
        hdr[0] = g_fpu_xcr0;      /* XSTATE_BV: all enabled bits valid   */
        hdr[1] = 0;               /* XCOMP_BV: compacted format off      */
    } else {
        __asm__ volatile("fninit");
        __asm__ volatile("fxsaveq (%0)" :: "r"(area) : "memory");
    }
}

void fpu_restore(void *area) {
    uint64_t mask = g_fpu_xcr0;
    if (g_fpu_xsave) {
        __asm__ volatile(
            "movq  %1, %%rdx\n\t"
            "movq  %%rdx, %%rax\n\t"
            "shrq  $32, %%rdx\n\t"
            "xrstorq (%0)\n\t"
            :: "r"(area), "r"(mask) : "rax", "rdx", "memory");
    } else {
        __asm__ volatile("fxrstorq (%0)" :: "r"(area) : "memory");
    }
}
