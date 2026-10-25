#include "drivers/memory/vmm.h"
#include "console/display.h"
#include "exec/process.h"      /* PROC_USER_VA_*, PROC_STACK_BASE */
#include <stdint.h>
#include <rmm/resource.h>       /* per-module resource ledger (recovery) */
void serial_print_string(const char *);
#include <stddef.h>
#include <stdbool.h>

//  Physical memory manager  (bitmap allocator)

// We keep a static bitmap for up to 512 MiB of RAM (128 K pages).
// Each bit = 1 page (4 KiB).  Bit set = page is free.
#define PMM_MAX_PAGES   (512 * 1024 * 1024 / PAGE_SIZE)   // 131072

static uint8_t  pmm_bitmap[PMM_MAX_PAGES / 8];
static uint64_t pmm_total;   // total pages in system
static uint64_t pmm_used;    // currently allocated pages

// helpers
static inline void bitmap_set(uint64_t page) {
    pmm_bitmap[page / 8] |=  (uint8_t)(1u << (page % 8));
}
static inline void bitmap_clear(uint64_t page) {
    pmm_bitmap[page / 8] &= ~(uint8_t)(1u << (page % 8));
}
static inline bool bitmap_test(uint64_t page) {
    return (pmm_bitmap[page / 8] >> (page % 8)) & 1u;
}

void pmm_init(uint64_t mem_size_bytes) {
    pmm_total = mem_size_bytes / PAGE_SIZE;
    if (pmm_total > PMM_MAX_PAGES) pmm_total = PMM_MAX_PAGES;
    pmm_used  = pmm_total;   // start: everything marked used

    // Zero the bitmap (all bits 0 = all pages used/unavailable)
    for (size_t i = 0; i < sizeof(pmm_bitmap); i++)
        pmm_bitmap[i] = 0;
}

// Mark page as free (caller supplies physical address)
void pmm_free_page(uint64_t phys_addr) {
    uint64_t page = phys_addr / PAGE_SIZE;
    if (page >= pmm_total) return;
    if (!bitmap_test(page)) {
        bitmap_set(page);
        pmm_used--;
    }
}

// Mark page as in-use
void pmm_alloc_page(uint64_t phys_addr) {
    uint64_t page = phys_addr / PAGE_SIZE;
    if (page >= pmm_total) return;
    if (bitmap_test(page)) {
        bitmap_clear(page);
        pmm_used++;
    }
}

// Allocate one physical page, returns physical address, 0 on OOM
uint64_t pmm_alloc(void) {
    for (uint64_t i = 0; i < pmm_total; i++) {
        if (bitmap_test(i)) {
            bitmap_clear(i);
            pmm_used++;

            // Zero the page before handing it out
            uint8_t *p = (uint8_t *)(i * PAGE_SIZE);
            for (size_t b = 0; b < PAGE_SIZE; b++) p[b] = 0;

            uint64_t pa = i * PAGE_SIZE;
            /* RMM: attribute to the running module (per-CPU context)
             * so a recovery can inventory (or reclaim) what it held. */
            rmm_res_note_alloc(pa, 1);
            return pa;
        }
    }
    return 0; // OOM
}

/* allocate n consecutive physical pages.
 *
 * Callers that compute an end/base address from a single allocation
 * (process kernel stacks: kstack_top = base + PROC_KSTACK_SIZE) must use
 * this - n independent pmm_alloc() calls return scattered pages once the
 * bitmap fragments (freed page tables, image pages, reaped stacks), and
 * the computed "stack" then overlaps other tasks' live kernel stacks.
 * Observed login's kstack [0xa8d000,0xa91000) overlapping init's
 * live stack [0xa8e000,0xa92000) - the new task's syscalls and park
 * frames overwrote init's parked continuation, and the next kctx_resume
 * executed a corrupted frame (ring-0 #UD at RIP=0x7fff39fa,
 * RSP=0xa90e58 - exactly the overlap zone).
 *
 * The block is zeroed before it is handed out.  Returns base phys or 0. */
uint64_t pmm_alloc_contig(size_t n) {
    if (!n) return 0;
    size_t run = 0;
    for (uint64_t i = 0; i < pmm_total; i++) {
        if (bitmap_test(i)) {
            run++;
            if (run == n) {
                uint64_t base_pg = i - (n - 1);
                for (uint64_t j = base_pg; j <= i; j++) bitmap_clear(j);
                pmm_used += n;
                uint64_t base = base_pg * PAGE_SIZE;
                uint8_t *p = (uint8_t *)(uintptr_t)base;
                for (size_t b = 0; b < n * PAGE_SIZE; b++) p[b] = 0;
                /* RMM: attribute the run to the running module (ledger) */
                rmm_res_note_alloc(base, (uint32_t)n);
                return base;
            }
        } else {
            run = 0;
        }
    }
    return 0;   /* no contiguous run of n pages */
}

void pmm_free(uint64_t phys_addr) {
    /* RMM: drop the caller module's ledger entry for this page (no-op
     * when the caller is core or the page was never tracked). */
    rmm_res_note_free(phys_addr);
    pmm_free_page(phys_addr);
}

uint64_t pmm_used_pages(void)  { return pmm_used;  }
uint64_t pmm_total_pages(void) { return pmm_total; }


//  Virtual memory manager  (4-level paging, x86-64)
//
//  Virtual address layout used by the bootstrap:
//    bits 63:48 - sign extension (all 0 for low half)
//    bits 47:39 - PML4 index  (9 bits)
//    bits 38:30 - PDPT index  (9 bits)
//    bits 29:21 - PD   index  (9 bits)
//    bits 20:12 - PT   index  (9 bits)
//    bits 11:0 - page offset (12 bits)
//
//  The bootstrap sets up 2 MiB huge pages in the PD.  The VMM
//  works at 4 KiB granularity in the PT level.

#define PML4_IDX(v)  (((v) >> 39) & 0x1FF)
#define PDPT_IDX(v)  (((v) >> 30) & 0x1FF)
#define PD_IDX(v)    (((v) >> 21) & 0x1FF)
#define PT_IDX(v)    (((v) >> 12) & 0x1FF)
#define PTE_ADDR(e)  ((e) & 0x000FFFFFFFFFF000ULL)

typedef uint64_t pte_t;

// Pointer to the active PML4 (physical == virtual because the bootstrap
// identity-maps the first 1 GiB with huge pages).
static pte_t *pml4 = NULL;

static inline uint64_t read_cr3(void) {
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    return cr3;
}

static inline void flush_tlb(uint64_t virt) {
    __asm__ volatile("invlpg (%0)" :: "r"(virt) : "memory");
}

// Retrieve (or allocate) a child table from a parent entry.
// Returns a pointer to the child table (virtual address).
static pte_t *get_or_create_table(pte_t *parent, size_t idx, uint64_t flags) {
    if (parent[idx] & PTE_PRESENT) {
        return (pte_t *) PTE_ADDR(parent[idx]);
    }

    // Allocate a new page for the child table
    uint64_t phys = pmm_alloc();
    if (!phys) return NULL;

    parent[idx] = phys | PTE_PRESENT | PTE_WRITABLE | (flags & PTE_USER);
    return (pte_t *) phys;   // identity mapped
}

void vmm_init(void) {
    pml4 = (pte_t *) read_cr3();

    /* Enable EFER.NXE (bit 11) so PTE bit 63 is honoured as NX rather
     * than rejected as a reserved bit.  vmm_mprotect relies on this, the
     * bootloader only sets LME.  Idempotent, and a no-op for pages that
     * never set bit 63. */
    {
        uint32_t lo, hi;
        __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0xC0000080u));
        if (!(lo & (1u << 11))) {
            lo |= (1u << 11);
            __asm__ volatile("wrmsr" :: "a"(lo), "d"(hi), "c"(0xC0000080u));
        }
    }

    // The bootstrap identity-maps the first 1 GiB with 2 MiB huge pages,
    // but it does not set PTE_USER on the PML4[0] or PDPT[0] entries.
    // User-mode programs need U/S=1 at every level of the page walk,
    // so we patch both entries here.
    pml4[0] |= PTE_USER;
    pte_t *pdpt = (pte_t *) PTE_ADDR(pml4[0]);
    pdpt[0] |= PTE_USER;

    // Individual PD entries (the 2 MiB huge pages) are still supervisor-only.
    // When the first user page is mapped inside one of them, vmm_map will
    // split that 2 MiB range into 4 KiB PTEs and set PTE_USER at the PTE level.
}

bool vmm_map(uint64_t virt, uint64_t phys, uint64_t flags) {
    pte_t *pdpt = get_or_create_table(pml4, PML4_IDX(virt), flags);
    if (!pdpt) return false;

    pte_t *pd = get_or_create_table(pdpt, PDPT_IDX(virt), flags);
    if (!pd) return false;

    // If the PD entry currently holds a huge (2 MiB) page and the
    // caller wants user-mode access, we must split it into 512 4 KiB
    // page-table entries so per-page PTE_USER can be set.
    if (pd[PD_IDX(virt)] & PTE_HUGE) {
        if (flags & PTE_USER) {
#ifdef DEBUG_VMM
            display_printf("vmm: split 2M at 0x%x\n", (unsigned)virt);
#endif
            uint64_t huge_phys  = PTE_ADDR(pd[PD_IDX(virt)]);
            uint64_t huge_flags = pd[PD_IDX(virt)] & 0xFFF;
            huge_flags &= ~PTE_HUGE;

            uint64_t pt_phys = pmm_alloc();
            if (!pt_phys) return false;
            pte_t *pt = (pte_t *)(uintptr_t)pt_phys;

            for (int i = 0; i < 512; i++) {
                uint64_t pg_phys = huge_phys + (uint64_t)i * PAGE_SIZE;
                pt[i] = pg_phys | huge_flags;
            }

            uint64_t pd_flags = (pd[PD_IDX(virt)] & 0xFFF);
            pd_flags |= (flags & PTE_USER);
            pd_flags &= ~PTE_HUGE;
            pd[PD_IDX(virt)] = pt_phys | pd_flags;
            /* Flush stale 2 MiB TLB entry that covered this range */
            flush_tlb(virt & ~(uint64_t)0x1FFFFF);
        } else {
            // Kernel mappings (MMIO, etc.) pass through unchanged.
            return true;
        }
    }

    pte_t *pt = get_or_create_table(pd, PD_IDX(virt), flags);
    if (!pt) return false;

    pt[PT_IDX(virt)] = (phys & 0x000FFFFFFFFFF000ULL) | flags | PTE_PRESENT;
    flush_tlb(virt);
    return true;
}

void vmm_unmap(uint64_t virt) {
    if (!pml4) return;

    pte_t *pdpt = (pte_t *) PTE_ADDR(pml4[PML4_IDX(virt)]);
    if (!(pml4[PML4_IDX(virt)] & PTE_PRESENT)) return;

    pte_t *pd = (pte_t *) PTE_ADDR(pdpt[PDPT_IDX(virt)]);
    if (!(pdpt[PDPT_IDX(virt)] & PTE_PRESENT)) return;

    if (pd[PD_IDX(virt)] & PTE_HUGE) return; // can't unmap huge pages here

    pte_t *pt = (pte_t *) PTE_ADDR(pd[PD_IDX(virt)]);
    if (!(pd[PD_IDX(virt)] & PTE_PRESENT)) return;

    pt[PT_IDX(virt)] = 0;
    flush_tlb(virt);
}

uint64_t vmm_virt_to_phys(uint64_t virt) {
    if (!pml4) return 0;

    if (!(pml4[PML4_IDX(virt)] & PTE_PRESENT)) return 0;
    pte_t *pdpt = (pte_t *) PTE_ADDR(pml4[PML4_IDX(virt)]);

    if (!(pdpt[PDPT_IDX(virt)] & PTE_PRESENT)) return 0;
    pte_t *pd = (pte_t *) PTE_ADDR(pdpt[PDPT_IDX(virt)]);

    if (!(pd[PD_IDX(virt)] & PTE_PRESENT)) return 0;
    if (pd[PD_IDX(virt)] & PTE_HUGE) {
        // 2 MiB page
        return PTE_ADDR(pd[PD_IDX(virt)]) + (virt & 0x1FFFFF);
    }

    pte_t *pt = (pte_t *) PTE_ADDR(pd[PD_IDX(virt)]);
    if (!(pt[PT_IDX(virt)] & PTE_PRESENT)) return 0;
    return PTE_ADDR(pt[PT_IDX(virt)]) + (virt & 0xFFF);
}

uint64_t vmm_alloc_pages(uint64_t virt_base, size_t n, uint64_t flags) {
    for (size_t i = 0; i < n; i++) {
        uint64_t phys = pmm_alloc();
        if (!phys) {
            // Roll back already mapped pages
            vmm_free_pages(virt_base, i);
            return VMM_ALLOC_FAILED;
        }
        if (!vmm_map(virt_base + i * PAGE_SIZE, phys, flags)) {
            pmm_free(phys);
            vmm_free_pages(virt_base, i);
            return VMM_ALLOC_FAILED;
        }
    }
    return virt_base;
}

void vmm_free_pages(uint64_t virt_base, size_t n) {
    for (size_t i = 0; i < n; i++) {
        uint64_t virt = virt_base + i * PAGE_SIZE;
        uint64_t phys = vmm_virt_to_phys(virt);
        if (phys) pmm_free(phys);
        vmm_unmap(virt);
    }
}

bool vmm_identity_map_range(uint64_t phys_start, uint64_t phys_end, uint64_t flags) {
    phys_start &= ~(uint64_t)(PAGE_SIZE - 1);          // round down
    phys_end    = (phys_end + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1); // round up

    for (uint64_t addr = phys_start; addr < phys_end; addr += PAGE_SIZE) {
        if (!vmm_map(addr, addr, flags)) return false;
    }
    return true;
}

void *vmm_get_pml4(void) { return (void *)pml4; }

uint64_t vmm_get_cr3(void) {
    return read_cr3();
}

/* Debug: peek at PTE for a virtual address (0 if not present). */
uint64_t vmm_peek_pte(uint64_t virt) {
    if (!pml4) return 0;
    if (!(pml4[PML4_IDX(virt)] & PTE_PRESENT)) return 0;
    pte_t *pdpt = (pte_t *) PTE_ADDR(pml4[PML4_IDX(virt)]);
    if (!(pdpt[PDPT_IDX(virt)] & PTE_PRESENT)) return 0;
    pte_t *pd = (pte_t *) PTE_ADDR(pdpt[PDPT_IDX(virt)]);
    if (!(pd[PD_IDX(virt)] & PTE_PRESENT)) return 0;
    if (pd[PD_IDX(virt)] & PTE_HUGE) return pd[PD_IDX(virt)];
    pte_t *pt = (pte_t *) PTE_ADDR(pd[PD_IDX(virt)]);
    return pt[PT_IDX(virt)];
}

/* Find and allocate any free page. Returns physical address, or 0 on failure. */
uint64_t pmm_alloc_any(void) {
    for (uint64_t i = 1; i < pmm_total; i++) {
        if (bitmap_test(i)) {
            bitmap_clear(i);
            pmm_used++;
            return i * PAGE_SIZE;
        }
    }
    return 0;
}

/*
 *  Per-process address spaces (fork/exec support)
 *
 *  The bootstrap maps the first 1 GiB identity, all kernel tables live
 *  below 4 GiB in PML4 entries 0-3, while true user VAs live in entries
 *  0-1 (0-1 TiB).  A child address space therefore:
 *    - shares every PML4 entry >= USER_PML4_LIMIT (kernel half), and
 *    - deep-copies the whole table tree for entries < USER_PML4_LIMIT
 *      (user half, including the kernel's low identity region), so the
 *      child can rewrite its own leaf PTEs without touching the parent.
 */

#define USER_PML4_LIMIT 4   /* PML4 entries 0..3 cover the first 4 TiB */

/* COW forward declarations (definitions in the COW section below) */
#define PTE_COW  (1ULL << 9)    /* software: frame is COW-shared (bit 9)  */
static void cow_ref_inc(uint64_t phys);
static void vmm_cow_count_child(void);

static void *vmm_alloc_table(void) {
    uint64_t phys = pmm_alloc();
    return phys ? (pte_t *)(uintptr_t)phys : NULL;
}

uint64_t vmm_switch_root(uint64_t pml4_phys) {
    uint64_t old = read_cr3();
    if (pml4_phys && PTE_ADDR(pml4_phys) != PTE_ADDR(old)) {
        pml4 = (pte_t *)(uintptr_t)PTE_ADDR(pml4_phys);
        /* Plain CR3 load flushes the whole TLB (no PCIDs in this kernel). */
        __asm__ volatile("mov %0, %%cr3" :: "r"(pml4_phys) : "memory");
    }
    return old;
}

static pte_t *copy_user_tree(pte_t *src_table, int level) {
    /* level 1 = PDPT, 2 = PD, 3 = PT (leaf data pages are not copied -
     * the caller rewrites leaf PTEs from the fork copy list)          */
    pte_t *dst = vmm_alloc_table();
    if (!dst) return NULL;
    for (int i = 0; i < 512; i++) dst[i] = src_table[i];
    if (level < 3) {
        for (int i = 0; i < 512; i++) {
            if (!(dst[i] & PTE_PRESENT)) continue;
            if (dst[i] & PTE_HUGE) continue;         /* 2M identity leaf */
            pte_t *child = copy_user_tree(
                (pte_t *)(uintptr_t)PTE_ADDR(dst[i]), level + 1);
            if (!child) return NULL;
            dst[i] = ((uint64_t)(uintptr_t)child & 0x000FFFFFFFFFF000ULL)
                     | (dst[i] & 0xFFF);
        }
    }
    return dst;
}

uint64_t vmm_fork_address_space(uint64_t *copies, size_t ncopy) {
    /* copies[] = { va, parent_phys, child_phys } - rewrite the child's
     * leaf PTEs to point at the fresh child_phys pages.                */
    pte_t *new_pml4 = vmm_alloc_table();
    if (!new_pml4) return 0;
    for (int i = 0; i < 512; i++) new_pml4[i] = pml4[i];

    for (int i = 0; i < USER_PML4_LIMIT; i++) {
        if (!(new_pml4[i] & PTE_PRESENT)) continue;
        if (new_pml4[i] & PTE_HUGE) continue;
        pte_t *copy = copy_user_tree(
            (pte_t *)(uintptr_t)PTE_ADDR(new_pml4[i]), 1);
        if (!copy) return 0;
        new_pml4[i] = ((uint64_t)(uintptr_t)copy & 0x000FFFFFFFFFF000ULL)
                      | (new_pml4[i] & 0xFFF);
    }

    /* Install as the active root so vmm_map writes into the child tree. */
    uint64_t old_cr3 = vmm_switch_root((uint64_t)(uintptr_t)new_pml4);

    /* COW accounting: the child inherits every read-only+COW
     * leaf from the copied tables, the flip pass in the parent only
     * counted the parent's claim.  Take one refcount per inherited COW
     * frame here, after all table copies succeeded (so an OOM above
     * never leaves over-counted frames), while the child root is still
     * active.  Fork-of-fork relies on this: without it, three spaces
     * share a frame whose count says 1, and the first exit frees a live
     * frame.                                                             */
    vmm_cow_count_child();

    for (size_t c = 0; c < ncopy; c++) {
        vmm_map(copies[c * 3 + 0] /* va */,
                copies[c * 3 + 2] /* child_phys */,
                PTE_PRESENT | PTE_WRITABLE | PTE_USER);
    }

    vmm_switch_root(old_cr3);
    return (uint64_t)(uintptr_t)new_pml4;
}

/*
 *  Copy-on-write fork support
 *
 *  Eager fork copied every user page, on a 512 MiB box with 8 process
 *  slots that both wasted memory and made fork() O(resident pages).
 *
 *  COW model
 *
 *   - proc_fork walks the user half of the current root and flips every
 *     writable user leaf PTE to read-only + PTE_COW (software bit 9).
 *     It returns the list of flipped {va, phys} pairs.
 *   - vmm_fork_address_space_cow() deep-copies the table tree as usual -
 *     the child's leaf PTEs come over verbatim (read-only, same phys),
 *     so no page data is copied at all.
 *   - Any later write fault (user or kernel mode) on a PTE_COW page is
 *     resolved by vmm_handle_fault(): refcount > 1 -> allocate a fresh
 *     page, copy, point the faulting address space at it, refcount == 1
 *     -> just restore writability.  Refcounts live in cow_tab below and
 *     count address spaces sharing the frame.
 *   - Teardown (proc_free_user_pages) decrements the refcount of COW
 *     frames instead of freeing them, the last space to leave frees.
 *   - memfd MAP_SHARED frames (kobj_memfd_owns_phys) are never flipped
 *     and never freed by the process walk, the memfd object owns them.
 */

#define PTE_COW  (1ULL << 9)    /* software: frame is COW-shared (bit 9,
                                   architecturally "available")           */

/* Open-addressing refcount table keyed by physical frame address.  User
 * footprint is bounded by FORK_MAX_PAGES x PROC_MAX ~ 128 KiB frames in
 * the worst case, so 8192 slots is comfortably oversubscribed x4.      */
#define COW_TAB_SIZE 8192
typedef struct {
    uint64_t phys;      /* frame address, 0 = empty slot                    */
    uint32_t cnt;       /* address spaces currently sharing the frame       */
} cow_ent_t;
static cow_ent_t cow_tab[COW_TAB_SIZE];

static size_t cow_hash(uint64_t phys) {
    return (size_t)((phys >> 12) * 2654435761u) & (COW_TAB_SIZE - 1);
}

/* Find the slot for phys.  If create is false, return NULL when absent. */
static cow_ent_t *cow_lookup(uint64_t phys, bool create) {
    if (!phys) return NULL;
    size_t i = cow_hash(phys);
    for (size_t probe = 0; probe < COW_TAB_SIZE; probe++) {
        size_t s = (i + probe) & (COW_TAB_SIZE - 1);
        if (cow_tab[s].phys == phys) return &cow_tab[s];
        if (!cow_tab[s].phys) {
            if (!create) return NULL;
            cow_tab[s].phys = phys;
            cow_tab[s].cnt  = 0;
            return &cow_tab[s];
        }
    }
    return NULL;    /* table full - should never happen */
}

static void cow_ref_inc(uint64_t phys) {
    cow_ent_t *e = cow_lookup(phys, true);
    if (e) e->cnt++;
}

/* Returns the remaining share count after decrement (0 = last user). */
static uint32_t cow_ref_dec(uint64_t phys) {
    cow_ent_t *e = cow_lookup(phys, false);
    if (!e) return 0;
    if (e->cnt > 0) e->cnt--;
    uint32_t left = e->cnt;
    if (left == 0) e->phys = 0;         /* retire slot */
    return left;
}

static uint32_t cow_ref_get(uint64_t phys) {
    cow_ent_t *e = cow_lookup(phys, false);
    return e ? e->cnt : 0;
}

/* vmm_fork_address_space phase 2: while the child root is active,
 * take one refcount for every inherited read-only+COW leaf.  See the
 * note in vmm_fork_address_space, this is what keeps fork-of-fork
 * counts correct.                                                      */
static void vmm_cow_count_child(void) {
    if (!pml4) return;
    for (int i = 0; i < USER_PML4_LIMIT; i++) {
        if (!(pml4[i] & PTE_PRESENT) || (pml4[i] & PTE_HUGE)) continue;
        pte_t *pdpt = (pte_t *)(uintptr_t)PTE_ADDR(pml4[i]);
        for (int j = 0; j < 512; j++) {
            if (!(pdpt[j] & PTE_PRESENT) || (pdpt[j] & PTE_HUGE)) continue;
            pte_t *pd = (pte_t *)(uintptr_t)PTE_ADDR(pdpt[j]);
            for (int k = 0; k < 512; k++) {
                if (!(pd[k] & PTE_PRESENT) || (pd[k] & PTE_HUGE)) continue;
                pte_t *pt = (pte_t *)(uintptr_t)PTE_ADDR(pd[k]);
                for (int l = 0; l < 512; l++) {
                    if (!(pt[l] & PTE_PRESENT) || !(pt[l] & PTE_USER))
                        continue;
                    if (pt[l] & PTE_COW) cow_ref_inc(PTE_ADDR(pt[l]));
                }
            }
        }
    }
}

/* Leaf-PTE pointer in the current root (NULL if any level is absent). */
static pte_t *cow_walk_leaf(uint64_t virt) {
    if (!pml4) return NULL;
    if (!(pml4[PML4_IDX(virt)] & PTE_PRESENT)) return NULL;
    pte_t *pdpt = (pte_t *)(uintptr_t)PTE_ADDR(pml4[PML4_IDX(virt)]);
    if (!(pdpt[PDPT_IDX(virt)] & PTE_PRESENT)) return NULL;
    pte_t *pd = (pte_t *)(uintptr_t)PTE_ADDR(pdpt[PDPT_IDX(virt)]);
    if (!(pd[PD_IDX(virt)] & PTE_PRESENT) || (pd[PD_IDX(virt)] & PTE_HUGE))
        return NULL;
    pte_t *pt = (pte_t *)(uintptr_t)PTE_ADDR(pd[PD_IDX(virt)]);
    return &pt[PT_IDX(virt)];
}

/* Copy 4 KiB phys->phys through the low identity map. */
static void cow_copy_page(uint64_t dst, uint64_t src) {
    uint64_t *d = (uint64_t *)(uintptr_t)dst;
    uint64_t *s = (uint64_t *)(uintptr_t)src;
    for (size_t i = 0; i < PAGE_SIZE / 8; i++) d[i] = s[i];
}

bool vmm_cow_flip_parent(uint64_t *vas, uint64_t *physes, size_t max,
                         size_t *out_n) {
    *out_n = 0;
    if (!pml4) return false;

    for (uint64_t va = PROC_USER_VA_START; va < PROC_USER_VA_END;
         va += PAGE_SIZE)
    {
        pte_t *e = cow_walk_leaf(va);
        if (!e || !(*e & PTE_PRESENT) || !(*e & PTE_USER)) continue;

        uint64_t phys = PTE_ADDR(*e);
        if (!phys || phys == va) continue;         /* identity = kernel-owned */

        /* memfd MAP_SHARED frames stay writable and shared in both spaces. */
        extern bool kobj_memfd_owns_phys(uint64_t);
        if (kobj_memfd_owns_phys(phys)) continue;

        if (!(*e & PTE_WRITABLE)) continue;        /* already RO - plain share */

        /* Flip the parent's PTE to read-only + COW and count the frame. */
        *e &= ~PTE_WRITABLE;
        *e |=  PTE_COW;
        cow_ref_inc(phys);

        if (*out_n < max && vas && physes) {
            vas[*out_n]   = va;
            physes[*out_n] = phys;
            (*out_n)++;
        } else {
            /* out_n still counts every flipped page even when the caller
             * passes NULL arrays and only wants the count. */
            (*out_n)++;
        }
    }

    /* Full TLB flush: the parent continues to run on these mappings. */
    __asm__ volatile("mov %%cr3, %%rax\n\tmov %%rax, %%cr3" ::: "rax", "memory");
    return true;
}

/* Resolve a write fault on a COW frame.  Returns true if resolved.
 *
 * Ordering matters: allocate + copy before decrementing, so an OOM
 * failure leaves the refcount untouched (the fault will simply recur
 * and reach the fatal path - Linux's OOM-kill analogue).             */
static bool vmm_cow_break(uint64_t va) {
    pte_t *e = cow_walk_leaf(va);
    if (!e || !(*e & PTE_PRESENT) || !(*e & PTE_COW)) return false;

    uint64_t phys = PTE_ADDR(*e);
    uint32_t shares = cow_ref_get(phys);

    uint64_t np = 0;
    if (shares > 1) {
        /* Other spaces still share the frame, allocate a private copy. */
        np = pmm_alloc();
        if (!np) return false;                 /* OOM: fatal path will kill */
        cow_copy_page(np, phys);
    }

    uint32_t left = cow_ref_dec(phys);
    if (np) {
        (void)left;
        *e = ((*e) & ~0x000FFFFFFFFFF000ULL) | np;
    }
    /* else: we were the last sharer, keep the frame, it is ours now. */

    *e |=  PTE_WRITABLE;
    *e &= ~PTE_COW;
    flush_tlb(va);
    return true;
}

/* Demand-grow the user stack: map one fresh RW page inside the growth
 * window below PROC_STACK_BASE.  Returns true if a page was mapped.   */
#define STACK_GROW_MAX_PAGES 128    /* 512 KiB below the stack base */

static bool vmm_stack_grow(uint64_t va) {
    if (va < PROC_STACK_BASE - (uint64_t)STACK_GROW_MAX_PAGES * PAGE_SIZE)
        return false;
    if (va >= PROC_STACK_BASE) return false;

    uint64_t phys = pmm_alloc();
    if (!phys) return false;
    return vmm_map(va, phys, PTE_PRESENT | PTE_WRITABLE | PTE_USER);
}

/* Page-fault triage - called from exception_handler_c before the fatal
 * paths, for both user-mode and kernel-mode faults.  Returns true when
 * the fault was handled and the faulting access may simply retry:
 *   1. write fault on a present PTE_COW page      -> COW break
 *   2. fault inside the stack growth window       -> map a stack page
 * Everything else falls through to the existing kill/panic logic.      */
extern void serial_print_string(const char *);
extern void serial_print_hex64(unsigned long v);
int vmm_handle_fault(uint64_t cr2, uint64_t error) {
    uint64_t va = cr2 & ~0xFFFULL;

    if ((error & 1) && (error & 2)) {          /* present + write */
        pte_t *e = cow_walk_leaf(va);
        if (e && (*e & PTE_PRESENT) && (*e & PTE_COW) && (*e & PTE_USER))
            return vmm_cow_break(va) ? 1 : 0;
        serial_print_string("[cow] UNHANDLED\n");
        return 0;
    }

    if (!(error & 1)) {                        /* not present */
        /* Only accept faults that plausibly target the stack window,
         * everything else must reach the fatal path (NULL deref etc.). */
        if (va >= PROC_STACK_BASE - (uint64_t)STACK_GROW_MAX_PAGES * PAGE_SIZE
            && va < PROC_STACK_BASE)
            return vmm_stack_grow(va) ? 1 : 0;
        return 0;
    }

    return 0;
}

/* Exit-time release: drop this space's reference to va's frame.
 * Frees the frame only when the caller is the last sharer.            */
void vmm_cow_release_page(uint64_t va) {
    pte_t *e = cow_walk_leaf(va);
    if (!e || !(*e & PTE_PRESENT)) return;

    uint64_t phys = PTE_ADDR(*e);
    extern bool kobj_memfd_owns_phys(uint64_t);

    if (phys && phys != va) {                  /* skip identity/kernel pages */
        if (kobj_memfd_owns_phys(phys)) {
            /* owned by a memfd object, never freed here */
        } else if (*e & PTE_COW) {
            if (cow_ref_dec(phys) == 0) pmm_free(phys);
        } else {
            pmm_free(phys);
        }
    }
    vmm_unmap(va);
}

/* Undo all COW flips in the current (parent) address space, used when
 * the child address-space allocation fails after phase 1.  Restores W,
 * clears COW, drops the per-frame refcount, flushes the TLB.           */
void vmm_cow_rollback_parent(void) {
    if (!pml4) return;
    for (uint64_t va = PROC_USER_VA_START; va < PROC_USER_VA_END;
         va += PAGE_SIZE)
    {
        pte_t *e = cow_walk_leaf(va);
        if (!e || !(*e & PTE_PRESENT)) continue;
        if (!(*e & PTE_COW)) continue;

        uint64_t phys = PTE_ADDR(*e);
        cow_ref_dec(phys);
        *e |=  PTE_WRITABLE;
        *e &= ~PTE_COW;
        flush_tlb(va);
    }
}

/* mprotect(2) core - enforced protection bits on present user pages.
 *
 * Scope note: this kernel accesses user memory
 * through the user page tables (no SMAP), so removing PTE_WRITABLE
 * would fault legitimate kernel writes (read() into a user buffer,
 * signal-frame setup).  mprotect therefore enforces:
 *   - PROT_NONE  -> page unmapped from the user walk (guard pages work)
 *   - PROT_EXEC  -> NX bit cleared
 *   - !PROT_EXEC -> NX bit set
 * W is left untouched (all user pages are RW today).  Full W^X returns
 * together with SMAP / kernel-alias access.                            */
int vmm_mprotect(uint64_t va, size_t len, int prot) {
    if (len == 0) return 0;
    if (prot & ~0x7) return -(int)VFS_EINVAL;
    if (va < PROC_USER_VA_START || va + len > PROC_USER_VA_END)
        return -(int)VFS_EINVAL;

    uint64_t start = va & ~0xFFFULL;
    uint64_t end   = (va + len + 0xFFF) & ~0xFFFULL;

    for (uint64_t cur = start; cur < end; cur += PAGE_SIZE) {
        pte_t *e = cow_walk_leaf(cur);
        if (!e || !(*e & PTE_PRESENT)) return -(int)VFS_ENOMEM;

        if (prot == 0 /* PROT_NONE */) {
            *e &= ~PTE_PRESENT;                /* guard page */
        } else {
            *e |=  PTE_PRESENT;                /* revive a PROT_NONE page */
            if (prot & 0x4) *e &= ~PTE_NX;     /* executable  */
            else            *e |=  PTE_NX;     /* data        */
        }
        flush_tlb(cur);
    }
    return 0;
}

void vmm_free_tree(uint64_t pml4_phys) {
    if (!pml4_phys) return;
    pte_t *root = (pte_t *)(uintptr_t)PTE_ADDR(pml4_phys);
    for (int i = 0; i < USER_PML4_LIMIT; i++) {
        if (!(root[i] & PTE_PRESENT) || (root[i] & PTE_HUGE)) continue;
        pte_t *pdpt = (pte_t *)(uintptr_t)PTE_ADDR(root[i]);
        for (int j = 0; j < 512; j++) {
            if (!(pdpt[j] & PTE_PRESENT) || (pdpt[j] & PTE_HUGE)) continue;
            pte_t *pd = (pte_t *)(uintptr_t)PTE_ADDR(pdpt[j]);
            for (int k = 0; k < 512; k++) {
                if (!(pd[k] & PTE_PRESENT) || (pd[k] & PTE_HUGE)) continue;
                pmm_free(PTE_ADDR(pd[k]));      /* PT page */
            }
            pmm_free(PTE_ADDR(pdpt[j]));        /* PD page */
        }
        pmm_free(PTE_ADDR(root[i]));            /* PDPT page */
    }
    pmm_free(PTE_ADDR(pml4_phys));              /* PML4 page */
}
