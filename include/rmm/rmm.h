#pragma once
/*
 * rmm.h - Recoverable Modular Monolithic Kernel (RMM) V2: basic types and build-wide constants.
 *
 * This header (and everything under include/rmm) is architecture-
 * independent: it must never contain an #ifdef __x86_64__ (or any other
 * arch conditional).  Architecture specifics live in arch/<arch>/ only.
 */
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define RMM_MAGIC          0x524D4D32u          /* "RMM2" */
#define RMM_ABI_VERSION    1ull

/* Supported module architectures (image header: uint16 arch) */
enum rmm_arch {
    RMM_ARCH_X86_64 = 1,
    RMM_ARCH_ARM64  = 2,
    RMM_ARCH_RISCV  = 3,
    RMM_ARCH_PPC64  = 4,
};

/* Module privilege classes */
enum rmm_class {
    RMM_CLASS_CORE    = 0,   /* unrestricted - part of the TCB          */
    RMM_CLASS_TRUSTED = 1,   /* limited additional operations           */
    RMM_CLASS_SFI     = 2,   /* compiler-isolated (Stage 4)             */
    RMM_CLASS_DRIVER  = 3,   /* SFI + explicit MMIO/DMA capabilities    */
};

/* Module ids: the generator assigns these from modules.map order.  The
 * generated header (modules.generated.h) defines RMM_MODID_<name>. */
typedef uint32_t rmm_module_id_t;

/* Cross-module error codes.  Negative so they flow through
 * the kernel's existing int-return conventions. */
#define RMM_ERR_MODULE_UNAVAILABLE  (-1000)  /* callee QUIESCING/FAULTED/DISABLED */
#define RMM_ERR_MODULE_FAULT        (-1001)  /* callee crashed                    */
#define RMM_ERR_MODULE_RELOADING    (-1002)  /* callee is being replaced          */
#define RMM_ERR_MODULE_NOT_RESIDENT (-1003)  /* not loaded + cannot fault-load */
#define RMM_ERR_MODULE_LOAD         (-1004)  /* load failure                */
#define RMM_ERR_MODULE_GENERATION   (-1005)  /* stale execution context     */

/* Convert an RMM module error into a plain -EIO style errno for userland
 * (the syscall layer never leaks RMM-internal codes). */
static inline int rmm_err_to_errno(int err) {
    if (err >= -1000 && err <= -1005) return -5;   /* -EIO */
    return err;
}

/* Minimal spinlock - the arch layer provides lock/unlock (arch.h) */
struct spinlock { uint32_t locked; const char *name; uint64_t _irqflags; };

/* Power-of-two helpers */
#define RMM_IS_POW2(x)  ((x) != 0 && (((x) & ((x) - 1)) == 0))
#define RMM_MASK_OF(size) ((size) - 1ull)

/* The one-page guard that separates module segments */
#define RMM_GUARD_BYTES 4096ull
