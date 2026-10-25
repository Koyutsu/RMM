#include "drivers/cpu/detectCPU.h"
#include "console/display.h"
#include <stdint.h>

static inline void cpuid(uint32_t reg, uint32_t *eax, uint32_t *ebx,
                          uint32_t *ecx, uint32_t *edx) {
    __asm__ volatile("cpuid"
        : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
        : "0"(reg));
}

/* Cached CPU brand string (CPUID 0x80000002..0x80000004)
 * Read once, stripped of leading spaces, NUL-terminated.  Exposed to
 * userland through SYS_RMM_CPUINFO(517) so neofetch can show the
 * real processor model instead of the architecture name. */
static char g_cpu_brand[49];
static bool g_cpu_brand_done = false;

/* decimal writer, returns chars written (buf needs >= 10 bytes) */
static int u32_to_dec(char *buf, uint32_t v) {
    char tmp[10];
    int n = 0;
    do { tmp[n++] = (char)('0' + (v % 10)); v /= 10; } while (v);
    for (int i = 0; i < n; i++) buf[i] = tmp[n - 1 - i];
    return n;
}

const char *cpu_brand_string(void) {
    if (!g_cpu_brand_done) {
        uint32_t max_ext, ignore;
        cpuid(0x80000000, &max_ext, &ignore, &ignore, &ignore);

        if (max_ext >= 0x80000004) {
            uint32_t *out = (uint32_t *)g_cpu_brand;
            cpuid(0x80000002, &out[0], &out[1], &out[2], &out[3]);
            cpuid(0x80000003, &out[4], &out[5], &out[6], &out[7]);
            cpuid(0x80000004, &out[8], &out[9], &out[10], &out[11]);
            g_cpu_brand[48] = '\0';
        } else {
            g_cpu_brand[0] = '\0';
        }

        /* strip leading spaces (brands are commonly space-padded) */
        const char *p = g_cpu_brand;
        while (*p == ' ') p++;
        if (p != g_cpu_brand) {
            int i = 0;
            while (p[i]) { g_cpu_brand[i] = p[i]; i++; }
            g_cpu_brand[i] = '\0';
        }

        /* Some CPUs (QEMU's default "qemu64" model, old pre-brand Intel)
         * return an all-zero brand string.  Synthesise a descriptive
         * "vendor Family X Model Y Stepping Z" line from CPUID leaf 1 so
         * neofetch never falls back to a bare architecture name. */
        if (g_cpu_brand[0] == '\0') {
            uint32_t v[3], fam_eax;
            cpuid(0, &ignore, &v[0], &v[2], &v[1]);   /* EBX/EDX/ECX vendor */
            char vendor[13];
            char *vp = vendor;
            const uint32_t *vr = v;
            for (int w = 0; w < 3; w++) {
                vp[0] = (char)(vr[w] & 0xFF);
                vp[1] = (char)((vr[w] >> 8) & 0xFF);
                vp[2] = (char)((vr[w] >> 16) & 0xFF);
                vp[3] = (char)((vr[w] >> 24) & 0xFF);
                vp += 4;
            }
            vendor[12] = '\0';

            cpuid(1, &fam_eax, &ignore, &ignore, &ignore);
            uint32_t family  = (fam_eax >> 8) & 0xF;
            uint32_t extfam  = (fam_eax >> 20) & 0xFF;
            if (family == 0xF) family += extfam;
            uint32_t model   = (fam_eax >> 4) & 0xF;
            uint32_t extmdl  = (fam_eax >> 16) & 0xF;
            if (family >= 6) model |= (extmdl << 4);
            uint32_t stepping = fam_eax & 0xF;

            char *o = g_cpu_brand;
            const char *s = vendor;
            while (*s && (int)(o - g_cpu_brand) < 44) *o++ = *s++;
            s = " Family ";
            while (*s) *o++ = *s++;
            o += u32_to_dec(o, family);
            s = " Model ";
            while (*s) *o++ = *s++;
            o += u32_to_dec(o, model);
            s = " Stepping ";
            while (*s) *o++ = *s++;
            o += u32_to_dec(o, stepping);
            *o = '\0';
        }
        g_cpu_brand_done = true;
    }
    return g_cpu_brand;
}

void CpuName(void) {
    display_puts(cpu_brand_string());
}

/*
 *  FPU/SSE/AVX bring-up, x86-64 micro-architecture level,
 *  per-task extended-state save
 *
 *  cpu_fpu_init() runs once at boot (after this module's lazy brand
 *  probe has CPUID available anyway):
 *    - CR0.EM/TS clear, MP set, CR4.OSFXSR|OSXMMEXCPT - SSE2 baseline
 *      is always usable.
 *    - With XSAVE+AVX: CR4.OSXSAVE on and XCR0 = x87|SSE|YMM
 *      (+ opmask|ZMM_Hi256|Hi16_ZMM on AVX-512F CPUs) so VEX/EVEX
 *      instructions work in user mode.
 *  fpu_save()/fpu_restore() snapshot the full extended state around
 *  every context switch (xsave/xrstor when enabled, fxsave/fxrstor as
 *  the universal fallback).  The kernel is compiled -mno-sse, so no
 *  kernel code path can clobber the saved image between IRQ entry and
 *  the save.
 */

static bool     g_fpu_xsave;      /* xsave/xrstor available & enabled   */
static uint64_t g_fpu_xcr0;       /* programmed XCR0 value              */
static int      g_x8664_level = 1;/* micro-architecture level v1..v4    */
static char     g_cpu_flags[192]; /* /proc/cpuinfo-style flags string   */
static bool     g_feat_done = false;

bool     fpu_xsave_available(void) { return g_fpu_xsave; }
uint64_t fpu_xcr0_mask(void)       { return g_fpu_xcr0;   }
int      cpu_x86_64_level(void)    { return g_x8664_level;}
const char *cpu_flags_string(void) { return g_cpu_flags;  }

static inline void cpuid7(uint32_t sub, uint32_t *eax, uint32_t *ebx,
                          uint32_t *ecx, uint32_t *edx) {
    __asm__ volatile("cpuid"
        : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
        : "0"(7), "2"(sub));
}

static void detect_features(void) {
    uint32_t a, b, c, d;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "0"(1));
    bool has_xsave = (c >> 26) & 1;
    bool osxsave_cap = (c >> 27) & 1;   /* XGETBV/XSETBV + CR4.OSXSAVE */
    bool has_avx     = (c >> 28) & 1;
    bool fma         = (c >> 12) & 1;
    bool movbe       = (c >> 22) & 1;
    bool sse3        = (c >> 0)  & 1;
    bool ssse3       = (c >> 9)  & 1;
    bool sse41       = (c >> 19) & 1;
    bool sse42       = (c >> 20) & 1;
    bool popcnt      = (c >> 23) & 1;
    bool cx16        = (c >> 13) & 1;
    bool f16c        = (c >> 29) & 1;

    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                     : "0"(0x80000001));
    bool lzcnt = (d >> 5) & 1;

    bool avx2 = 0, bmi1 = 0, bmi2 = 0;
    bool avx512f = 0, avx512dq = 0, avx512cd = 0, avx512bw = 0, avx512vl = 0;
    {
        uint32_t max_leaf, ig;
        __asm__ volatile("cpuid" : "=a"(max_leaf), "=b"(ig), "=c"(ig), "=d"(ig)
                         : "0"(0));
        if (max_leaf >= 7) {
            cpuid7(0, &a, &b, &c, &d);
            avx2      = (b >> 5)  & 1;
            bmi1      = (b >> 3)  & 1;
            bmi2      = (b >> 8)  & 1;
            avx512f   = (b >> 16) & 1;
            avx512dq  = (b >> 17) & 1;
            avx512cd  = (b >> 28) & 1;
            avx512bw  = (b >> 30) & 1;
            avx512vl  = (b >> 31) & 1;
        }
    }

    bool v2 = sse3 && ssse3 && sse41 && sse42 && popcnt && cx16;
    bool v3 = v2 && has_avx && avx2 && fma && bmi1 && bmi2 && f16c &&
              lzcnt && movbe;
    bool v4 = v3 && avx512f && avx512cd && avx512bw && avx512dq && avx512vl;
    g_x8664_level = v4 ? 4 : (v3 ? 3 : (v2 ? 2 : 1));

    /* /proc/cpuinfo + hwprobe flag list (the bits ported software and
     * -march=x86-64-vN builds actually branch on) */
    size_t n = 0;
    #define CF_PUT(s_) do {                                       \
        const char *f_ = s_;                                      \
        if (n) g_cpu_flags[n++] = ' ';                            \
        while (*f_ && n + 1 < sizeof(g_cpu_flags))                \
            g_cpu_flags[n++] = *f_++;                             \
    } while (0)
    CF_PUT("fpu"); CF_PUT("sse2");
    if (sse3)  CF_PUT("sse3");
    if (ssse3) CF_PUT("ssse3");
    if (sse41) CF_PUT("sse4_1");
    if (sse42) CF_PUT("sse4_2");
    if (popcnt) CF_PUT("popcnt");
    if (cx16)  CF_PUT("cx16");
    if (has_avx) CF_PUT("avx");
    if (f16c)  CF_PUT("f16c");
    if (fma)   CF_PUT("fma");
    if (movbe) CF_PUT("movbe");
    if (lzcnt) CF_PUT("abm");
    if (bmi1)  CF_PUT("bmi1");
    if (bmi2)  CF_PUT("bmi2");
    if (avx2)  CF_PUT("avx2");
    if (avx512f)  CF_PUT("avx512f");
    if (avx512cd) CF_PUT("avx512cd");
    if (avx512bw) CF_PUT("avx512bw");
    if (avx512dq) CF_PUT("avx512dq");
    if (avx512vl) CF_PUT("avx512vl");
    if (has_xsave && has_avx) CF_PUT("xsave");
    #undef CF_PUT
    g_cpu_flags[n] = '\0';

    /* XCR0 program: x87|SSE always, YMM when AVX, the AVX-512 triple
     * when AVX-512F.  Requires CPUID.1:ECX[27] (XSETBV usable). */
    g_fpu_xsave = false;
    g_fpu_xcr0  = 0x3ULL;
    if (has_xsave && osxsave_cap && has_avx) {
        g_fpu_xsave = true;
        g_fpu_xcr0  = 0x7ULL;
        if (avx512f) g_fpu_xcr0 |= 0xE0ULL;
    }
}

static inline uint64_t rdcr0(void) {
    uint64_t v; __asm__ volatile("mov %%cr0, %0" : "=r"(v)); return v;
}
static inline void wrcr0(uint64_t v) {
    __asm__ volatile("mov %0, %%cr0" :: "r"(v) : "memory");
}
static inline uint64_t rdcr4(void) {
    uint64_t v; __asm__ volatile("mov %%cr4, %0" : "=r"(v)); return v;
}
static inline void wrcr4(uint64_t v) {
    __asm__ volatile("mov %0, %%cr4" :: "r"(v) : "memory");
}

#define KCR4_OSFXSR     (1ULL << 9)
#define KCR4_OSXMMEXCPT (1ULL << 10)
#define KCR4_OSXSAVE    (1ULL << 18)

static inline void kxsetbv(uint32_t reg, uint64_t val) {
    uint32_t lo = (uint32_t)val, hi = (uint32_t)(val >> 32);
    __asm__ volatile("xsetbv" :: "a"(lo), "d"(hi), "c"(reg));
}

void cpu_fpu_init(void) {
    if (!g_feat_done) { detect_features(); g_feat_done = true; }

    uint64_t cr0 = rdcr0();
    cr0 &= ~(1ULL << 2);          /* EM  = 0 - no #UD on x87         */
    cr0 &= ~(1ULL << 3);          /* TS  = 0 - state usable now      */
    cr0 |=  (1ULL << 1);          /* MP  = 1                         */
    wrcr0(cr0);

    uint64_t cr4 = rdcr4();
    cr4 |= KCR4_OSFXSR | KCR4_OSXMMEXCPT;
    if (g_fpu_xsave) cr4 |= KCR4_OSXSAVE;
    wrcr4(cr4);
    if (g_fpu_xsave) kxsetbv(0, g_fpu_xcr0);

    __asm__ volatile("fninit");
    uint32_t mxcsr = 0x1F80;
    __asm__ volatile("ldmxcsr %0" : : "m"(mxcsr));
}

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

void fpu_restore_init(void *area) {
    for (size_t i = 0; i < 4096; i++) ((uint8_t *)area)[i] = 0;
    if (g_fpu_xsave) {
        __asm__ volatile("fxsaveq (%0)" :: "r"(area) : "memory");
        uint64_t *hdr = (uint64_t *)((uintptr_t)area + 512);
        hdr[0] = g_fpu_xcr0;      /* XSTATE_BV: all enabled components  */
        hdr[1] = 0;               /* XCOMP_BV: non-compacted layout     */
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
