#pragma once
/*
 * resource.h - per-module resource ledger (recovery cleanup).
 *
 * Problem this solves: a module recovery unwinds the call
 * and marks the slot FAULTED, but everything the module held outside its
 * slot is invisible to the kernel: PMM pages allocated through pmm_alloc()
 * while the module ran, lock waits parked inside the module, refcounts.
 * Without a ledger a successful recovery can leak memory the kernel never
 * sees, or park a process forever on state only the crashed module
 * would have changed.
 *
 * The ledger records what each module allocated from the core while it
 * executed (callers are identified through the per-CPU module context,
 * - no module code had to change).  On recovery:
 *
 *   - the generation is bumped (recovery epoch) so every blocking loop
 *     that captured it on entry can detect the crash and abort with
 *     -EIO instead of hanging (see modules/fs/locks.c),
 *   - the ledger is inventoried and reported: page ranges the module
 *     held outside its slot are counted and printed, so a recovery log
 *     states exactly what leaked.
 *
 * Reclaim policy: entries default to RMM_RES_REPORT because a crashed
 * module's pages may be shared with processes (e.g. memfd page lists in
 * the fs module), blindly freeing them corrupts the process that maps
 * them.  Code that knows a range is exclusively owned can tag it
 * RMM_RES_RECLAIM at allocation time and recovery frees it.
 * In-slot pages are not ledger business: the loader's unload path
 *  walks the segment ranges and frees them.
 */
#include <stdint.h>
#include <stddef.h>
#include <rmm/module.h>

#define RMM_RES_LEDGER_CAP 64u   /* entries per module, oldest overwritten */

typedef enum {
    RMM_RES_REPORT  = 0,   /* count + report on recovery (safe default) */
    RMM_RES_RECLAIM = 1,   /* exclusively owned - recovery frees it     */
} rmm_res_policy_t;

/* Explicit tagging (optional, the auto-tracking below uses report). */
void rmm_res_track_pages(rmm_module_t *m, uint64_t phys, uint32_t npages,
                         rmm_res_policy_t policy);

/* Drop tracking for one page inside a tracked range (partial release
 * shrinks the range, the remaining pages stay tracked). */
void rmm_res_release_pages(rmm_module_t *m, uint64_t phys);

/* Auto-tracking used by the PMM (arch/x86_64/mm/vmm.c).  No-ops unless
 * a module is currently executing on this CPU (context) and the
 * ledger is initialised, IRQ-context allocations are never attributed
 * (the suspended module did not make them). */
void rmm_res_note_alloc(uint64_t phys, uint32_t npages);
void rmm_res_note_free(uint64_t phys);

/* Recovery hook - called from rmm_module_fault().  Applies RMM_RES_RECLAIM
 * entries (free), inventories the rest, prints the summary and resets the
 * ledger.  Returns the number of reported (leaked) pages. */
uint32_t rmm_resources_reclaim(rmm_module_t *m);

/* Manager bootstrap: arm the ledger (earlier PMM calls are core boot
 * code and are intentionally not tracked). */
void rmm_resources_init(void);
