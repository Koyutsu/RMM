/*
 * resources.c - per-module resource ledger (recovery cleanup).
 * See include/rmm/resource.h for the design rationale.
 *
 * Architecture-independent.  The ledger is a fixed per-module ring -
 * no dynamic allocation, safe to walk from the recovery path.
 */
#include <stdatomic.h>
#include <stdbool.h>
#include <rmm/rmm.h>
#include <rmm/module.h>
#include <rmm/fault.h>
#include <rmm/arch.h>
#include <rmm/resource.h>
#include <rmm/modules.generated.h>
#include "console/print.h"
#include "drivers/memory/vmm.h"     /* pmm_free (reclaim path) */

typedef struct {
    uint8_t          used;
    uint8_t          kind;        /* rmm_res_policy_t                  */
    uint8_t          reclaimed;
    uint8_t          _pad;
    uint64_t         phys;        /* base physical address             */
    uint32_t         npages;
} res_ent_t;

static res_ent_t  g_res[RMM_MODULE_COUNT * RMM_RES_LEDGER_CAP];
static struct spinlock g_res_lock = { 0, "rmm.res" };
static bool g_res_ready;

#define RES_IDX(mod, i) (&g_res[(mod)->id * RMM_RES_LEDGER_CAP + (i)])

void rmm_resources_init(void) { g_res_ready = true; }

static struct rmm_cpu_context *res_cpu(void) {
    if (!g_res_ready) return NULL;
    struct rmm_cpu_context *cpu = arch_cpu_ctx();
    if (!cpu || cpu->interrupt_depth != 0) return NULL;
    return cpu;
}

void rmm_res_track_pages(rmm_module_t *m, uint64_t phys, uint32_t npages,
                         rmm_res_policy_t policy) {
    if (!m || !phys || !npages) return;
    spin_lock(&g_res_lock);
    res_ent_t *slot = NULL, *ring = &g_res[m->id * RMM_RES_LEDGER_CAP];
    static uint32_t rr[RMM_MODULE_COUNT];
    for (uint32_t i = 0; i < RMM_RES_LEDGER_CAP; i++) {
        res_ent_t *e = &ring[i];
        if (e->used && e->phys == phys) {          /* merge re-allocation */
            e->npages = npages > e->npages ? npages : e->npages;
            e->kind   = (uint8_t)policy;
            spin_unlock(&g_res_lock);
            return;
        }
        if (!slot && !e->used) slot = e;
    }
    if (!slot) {                                   /* ring full: overwrite */
        slot = &ring[rr[m->id]++ % RMM_RES_LEDGER_CAP];
        printf("[rmm.res] %s ledger full - overwriting entry (leak grows)\n",
               m->name);
    }
    slot->used      = 1;
    slot->kind      = (uint8_t)policy;
    slot->reclaimed = 0;
    slot->phys      = phys;
    slot->npages    = npages;
    spin_unlock(&g_res_lock);
}

void rmm_res_release_pages(rmm_module_t *m, uint64_t phys) {
    if (!m || !phys) return;
    spin_lock(&g_res_lock);
    res_ent_t *ring = &g_res[m->id * RMM_RES_LEDGER_CAP];
    for (uint32_t i = 0; i < RMM_RES_LEDGER_CAP; i++) {
        res_ent_t *e = &ring[i];
        if (!e->used || phys < e->phys ||
            phys >= e->phys + (uint64_t)e->npages * PAGE_SIZE)
            continue;
        if (phys == e->phys) {                     /* head release        */
            e->phys += PAGE_SIZE;
            e->npages--;
        } else if (phys == e->phys + (uint64_t)(e->npages - 1) * PAGE_SIZE) {
            e->npages--;                           /* tail release        */
        } else {
            /* interior release: untrack the whole range (conservative -
             * the remainder simply becomes unreported, never freed)   */
            e->used = 0;
        }
        if (e->npages == 0) e->used = 0;
        spin_unlock(&g_res_lock);
        return;
    }
    spin_unlock(&g_res_lock);
}

void rmm_res_note_alloc(uint64_t phys, uint32_t npages) {
    struct rmm_cpu_context *cpu = res_cpu();
    if (!cpu || !cpu->current_module) return;
    rmm_res_track_pages(cpu->current_module, phys, npages, RMM_RES_REPORT);
}

void rmm_res_note_free(uint64_t phys) {
    struct rmm_cpu_context *cpu = res_cpu();
    if (!cpu || !cpu->current_module) return;
    rmm_res_release_pages(cpu->current_module, phys);
}

uint32_t rmm_resources_reclaim(rmm_module_t *m) {
    if (!m || !g_res_ready) return 0;
    uint32_t leaked = 0, reclaimed = 0;
    uint64_t leaked_bytes = 0;

    spin_lock(&g_res_lock);
    res_ent_t *ring = &g_res[m->id * RMM_RES_LEDGER_CAP];

    /* pass 1: apply reclaim policy (clear entries before freeing so a
     * nested pmm_free cannot re-untrack) */
    for (uint32_t i = 0; i < RMM_RES_LEDGER_CAP; i++) {
        res_ent_t *e = &ring[i];
        if (!e->used || e->kind != RMM_RES_RECLAIM) continue;
        uint64_t base = e->phys;
        uint32_t n    = e->npages;
        e->used = 0;
        e->reclaimed = 1;
        spin_unlock(&g_res_lock);
        for (uint32_t p = 0; p < n; p++) pmm_free(base + (uint64_t)p * PAGE_SIZE);
        reclaimed += n;
        spin_lock(&g_res_lock);
    }

    /* pass 2: inventory the rest */
    for (uint32_t i = 0; i < RMM_RES_LEDGER_CAP; i++) {
        res_ent_t *e = &ring[i];
        if (!e->used) continue;
        leaked       += e->npages;
        leaked_bytes += (uint64_t)e->npages * PAGE_SIZE;
        printf("  [rmm.res] %s held %u page(s) @ phys %p (%s) - leak\n",
               m->name, (unsigned)e->npages, (void *)(uintptr_t)e->phys,
               e->kind == RMM_RES_RECLAIM ? "reclaim pending" : "shared/unknown owner");
        e->used = 0;
    }
    spin_unlock(&g_res_lock);

    if (reclaimed || leaked)
        printf("  [rmm.res] %s recovery: %u page(s) reclaimed, %u page(s) "
               "(%u KiB) reported as leak\n",
               m->name, reclaimed, leaked, (unsigned)(leaked_bytes >> 10));
    return leaked;
}
