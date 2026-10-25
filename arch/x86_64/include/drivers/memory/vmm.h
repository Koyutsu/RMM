#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

//  Physical Memory Manager  (PMM)

#define PAGE_SIZE       4096
#define PAGES_PER_BYTE  8

// Call once at boot.  Pass the highest usable physical address
// (e.g. from multiboot memory map, or just assume 16 MiB for now).
void pmm_init(uint64_t mem_size_bytes);

// Mark a physical page as free / used.
void pmm_free_page (uint64_t phys_addr);
void pmm_alloc_page(uint64_t phys_addr);   // mark as used (for reserved regions)

// Allocate / release one 4 KiB physical page.
// pmm_alloc() returns 0 on failure.
uint64_t pmm_alloc(void);
// allocate n consecutive pages (zeroed).  Mandatory for anything
// that derives a range (kstack_top = base + size) from one allocation.
uint64_t pmm_alloc_contig(size_t n);
void     pmm_free (uint64_t phys_addr);

// Statistics
uint64_t pmm_used_pages(void);
uint64_t pmm_total_pages(void);

//  Virtual Memory Manager  (VMM)

// x86-64 page-table entry flags
#define PTE_PRESENT   (1ULL << 0)
#define PTE_WRITABLE  (1ULL << 1)
#define PTE_USER      (1ULL << 2)
#define PTE_HUGE      (1ULL << 7)    // 2 MiB page in PD entry
#define PTE_NX        (1ULL << 63)   // No-execute (needs EFER.NXE)

// Canonical "null" virtual address sentinel
#define VMM_ALLOC_FAILED  0ULL

// Initialise the VMM using the already-running PML4 set up by the
// assembly bootstrap.  After this call, vmm_map / vmm_unmap work.
void vmm_init(void);

// Map one physical page to one virtual page with the given flags.
// Creates intermediate tables as needed.
// Returns false on OOM.
bool vmm_map(uint64_t virt, uint64_t phys, uint64_t flags);

// Unmap a virtual page (does not free the physical page).
void vmm_unmap(uint64_t virt);

// Translate a virtual address to physical (returns 0 if not mapped).
uint64_t vmm_virt_to_phys(uint64_t virt);

// Debug: peek at raw PTE value for a virtual address (0 if not reachable).
uint64_t vmm_peek_pte(uint64_t virt);

// Higher-level helpers

// Allocate `n` contiguous virtual pages starting at `virt_base`,
// each backed by a fresh physical page.  Flags applied to every entry.
// Returns virt_base on success, VMM_ALLOC_FAILED on error.
uint64_t vmm_alloc_pages(uint64_t virt_base, size_t n, uint64_t flags);

// Free `n` pages starting at `virt_base`: unmaps and releases physical pages.
void vmm_free_pages(uint64_t virt_base, size_t n);

// Identity-map a physical range (e.g. MMIO, frame-buffer).
// Rounds down/up to page boundaries automatically.
bool vmm_identity_map_range(uint64_t phys_start, uint64_t phys_end, uint64_t flags);

// Return the current PML4 physical address (cr3 value).
uint64_t vmm_get_cr3(void);
void *vmm_get_pml4(void);    /* bootstrap PML4 - for boot-time heap placement */
uint64_t pmm_alloc_any(void);

// Per-process address spaces
// Activate a different PML4 (physical address); returns the previous CR3.
// Passing 0 is a no-op.  A plain CR3 load fully flushes the TLB.
uint64_t vmm_switch_root(uint64_t pml4_phys);

// Build a child address space: deep-copies the user-half table tree of the
// Current root, shares the kernel half, then (while the child root is
// active) maps copies[] = {va, parent_phys, child_phys} triples so the
// child's leaf PTEs reference its private page copies.
// Returns the child's PML4 physical address, or 0 on OOM.
uint64_t vmm_fork_address_space(uint64_t *copies, size_t ncopy /* triples */);

// Release the table pages of a child address space (PML4 + user-half
// PDPT/PD/PT pages).  Leaf data pages are not freed here, the process
// teardown path frees them via its own VA walk.
void vmm_free_tree(uint64_t pml4_phys);

// Copy-on-write fork + fault handling

/* Fork, phase 1 (run in the parent's address space, before the table
 * copy): flip every writable user leaf PTE to read-only + COW and take
 * one refcount per frame.  vas[]/physes[] receive the flipped pairs
 * (informational); max caps the arrays; *out_n gets the real count.
 * Returns false only if paging is not initialised.                     */
bool vmm_cow_flip_parent(uint64_t *vas, uint64_t *physes, size_t max,
                         size_t *out_n);

/* Page-fault triage - call from the exception handler for vector 14
 * Before the fatal paths, in both user and kernel mode.  Returns 1
 * when the fault was fixed (COW break or stack growth) and the
 * faulting instruction may retry.                                       */
int  vmm_handle_fault(uint64_t cr2, uint64_t error);

/* Exit-time release of one VA in the current address space: decrements
 * the COW refcount (freeing the frame on the last share) or frees a
 * private frame, memfd-owned frames are never freed here.               */
void vmm_cow_release_page(uint64_t va);

/* Undo all COW flips in the current (parent) address space, child
 * address-space allocation failed, restore W, drop refcounts.          */
void vmm_cow_rollback_parent(void);

/* mprotect(2) core on the current address space: PROT_NONE unmaps the
 * pages (guard pages), PROT_EXEC toggles NX.  W is preserved, see the
 * implementation note.  Returns 0 or -EINVAL/-ENOMEM.                   */
int  vmm_mprotect(uint64_t va, size_t len, int prot);
