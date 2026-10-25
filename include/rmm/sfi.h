#pragma once
/*
 * sfi.h - Software Fault Isolation types and runtime.
 *
 * Status: modules are in the trusted class in this build, the
 * domain constants, confinement primitive and validator are in place so
 * the Stage-4 LLVM SFI pass slots in without ABI changes.
 */
#include <rmm/rmm.h>
#include <rmm/module.h>

/*
 * Domain-typed pointers.  The compiler/SFI pass uses these to
 * decide which pointers require confinement.  Do not blindly mask every
 * pointer - MMIO, stacks and core pointers are different domains.
 */
typedef void *       rmm_local_ptr;   /* confined to the module's slot      */
typedef void *       rmm_core_ptr;    /* points into the resident core      */
typedef volatile void *rmm_mmio_ptr;  /* device MMIO - never masked   */

/* Confinement primitive: safe = base | (ptr & (size-1)). */
static inline void *rmm_confine(const struct rmm_domain *d, void *p) {
    return (void *)(((uintptr_t)p & (uintptr_t)d->mask) | (uintptr_t)d->base);
}

/* True if va lies inside the domain's slot range. */
static inline bool rmm_domain_contains(const struct rmm_domain *d, uint64_t va) {
    return va >= d->base && va < d->base + d->size;
}

/* MMIO is not module RAM: mapped through the core with an
 * explicit device check, the returned pointer is tagged MMIO domain. */
void *rmm_mmio_map(uint64_t phys, uint64_t size);
void  rmm_mmio_unmap(void *va, uint64_t size);

/* DMA is outside compiler SFI: the core's DMA API controls
 * physical ownership. */
void *rmm_dma_alloc(uint64_t size, uint64_t *phys_out);
void  rmm_dma_free(void *va, uint64_t size);
