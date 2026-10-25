#include "fs/pagecache.h"
#include "drivers/memory/vmm.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* Page-cache layout
 * Open addressing over a fixed table.  Key = (node pointer, page index);
 * entries store the physical page.  A free slot has node == NULL, a
 * Removed slot uses node == invalid (tombstone) so probes keep walking
 * but lookups fail.  LiveRAM model: nothing evicts, the tombstone case
 * only appears when pagecache_drop_node() runs on unlink.           */

typedef struct {
    vfs_node_t *node;
    uint32_t    page;
    uint64_t    phys;
} pc_slot_t;

#define PC_TOMBSTONE ((vfs_node_t *)(uintptr_t)1)
#define PC_SLOTS     8192

static pc_slot_t g_pc_table[PC_SLOTS];
static uint64_t  g_pc_used;

static inline size_t pc_hash(vfs_node_t *node, uint32_t page) {
    uintptr_t n = (uintptr_t)node;
    n ^= n >> 9;
    n *= 0x9E3779B97F4A7C15ULL;
    n ^= (uint32_t)page;
    n ^= n >> 7;
    return (size_t)(n & (PC_SLOTS - 1));
}

bool pagecache_lookup(vfs_node_t *node, uint32_t page, uint64_t *phys_out) {
    if (!node) return false;
    size_t i = pc_hash(node, page);
    for (size_t probe = 0; probe < PC_SLOTS; probe++, i = (i + 1) & (PC_SLOTS - 1)) {
        pc_slot_t *s = &g_pc_table[i];
        if (!s->node) return false;                       /* free: stop  */
        if (s->node == node && s->page == page) {
            if (phys_out) *phys_out = s->phys;
            return true;
        }
        /* tombstones and other nodes: keep probing */
    }
    return false;
}

uint64_t pagecache_get_or_load(vfs_node_t *node, uint32_t page) {
    if (!node || !node->ops || !node->ops->read) return 0;

    uint64_t hit;
    if (pagecache_lookup(node, page, &hit)) return hit;

    size_t i = pc_hash(node, page);
    pc_slot_t *free_slot = NULL;
    for (size_t probe = 0; probe < PC_SLOTS; probe++, i = (i + 1) & (PC_SLOTS - 1)) {
        pc_slot_t *s = &g_pc_table[i];
        if (s->node == PC_TOMBSTONE && !free_slot) { free_slot = s; continue; }
        if (!s->node) { free_slot = s; break; }
        if (s->node == node && s->page == page) return s->phys;  /* raced */
    }
    if (!free_slot) return 0;

    uint64_t phys = pmm_alloc();
    if (!phys) return 0;
    uint8_t *dst = (uint8_t *)(uintptr_t)phys;   /* direct phys map     */

    uint64_t foff = (uint64_t)page * 4096ULL;
    size_t   got  = 0;
    if (foff < node->size) {
        size_t want = 4096;
        if (foff + want > node->size) want = (size_t)(node->size - foff);
        int r = node->ops->read(node, dst, want, (size_t)foff);
        if (r < 0) { pmm_free(phys); return 0; }
        got = (size_t)r;
    }
    /* bytes past the file end stay zeroed (pmm_alloc zeroes), matches
     * the eager mapper's BSS-extension behaviour                      */
    (void)got;

    free_slot->node = node;
    free_slot->page = page;
    free_slot->phys = phys;
    g_pc_used++;
    return phys;
}

bool pagecache_owns(uint64_t phys) {
    if (!phys) return false;
    for (size_t i = 0; i < PC_SLOTS; i++) {
        pc_slot_t *s = &g_pc_table[i];
        if (s->node && s->node != PC_TOMBSTONE && s->phys == phys)
            return true;
    }
    return false;
}

void pagecache_drop_node(vfs_node_t *node) {
    if (!node) return;
    for (size_t i = 0; i < PC_SLOTS; i++) {
        pc_slot_t *s = &g_pc_table[i];
        if (s->node == node) {
            pmm_free(s->phys);
            s->node = PC_TOMBSTONE;
            s->page = 0;
            s->phys = 0;
            if (g_pc_used) g_pc_used--;
        }
    }
}

uint64_t pagecache_pages_used(void) { return g_pc_used; }
