#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "fs/vfs.h"

/*
 * pagecache - shared physical pages for read-only file mappings
 *
 * Why: the whole point of dynamic linking is that N processes share one
 * copy of libc's code.  With the eager per-process copy in mmap(2),
 * every dynamic process re-copied libc text, dynamic linking made RAM
 * usage worse than static.  This cache fixes that: the first process to
 * map a page of a file read-only allocates + reads it, every other
 * process mapping the same (node, page) maps the same physical page
 * (mapped without PTE_WRITABLE, matching Linux's private-file-text
 * behaviour - text is never written, a write to it faults, which is
 * the same trap Linux raises for read-only text).
 *
 * Scope:
 *   - READ-ONLY mappings only ((prot & PROT_WRITE) == 0).  Writable
 *     file mappings keep the eager private copy.
 *   - No coherence yet: writes to the file through read/write syscalls
 *     after a page was cached are not observed by existing mappings.
 *     (libc.so and the interpreter never change, file-write
 *     invalidation is future work.)
 *   - No eviction: LiveRAM keeps pages for the boot session.
 *   - Indexed by (vfs_node_t*, page index) in open-addressing buckets.
 */

/* Returns the physical page for (node, page), allocating + reading it
 * from the file on first use.  Returns 0 on OOM / unreadable node. */
uint64_t pagecache_get_or_load(vfs_node_t *node, uint32_t page);

/* True + physical page when (node, page) is already cached. */
bool pagecache_lookup(vfs_node_t *node, uint32_t page, uint64_t *phys_out);

/* True when phys is a page owned by the cache (shared across
 * processes) - munmap must never pmm_free such pages. */
bool pagecache_owns(uint64_t phys);

/* Drop every cached page belonging to node (used when a file is
 * unlinked/removed so stale pages cannot be re-handed-out). */
void pagecache_drop_node(vfs_node_t *node);

/* Statistics for /proc/meminfo-style introspection. */
uint64_t pagecache_pages_used(void);
