#pragma once
#include "fs/vfs.h"
#include <stdint.h>
#include <stddef.h>

/*
 * ramfs - simple RAM-backed filesystem (rootfs / tmpfs alike)
 *
 * Stores file data as flat byte arrays in a static heap.
 * Fully writable: supports create, read, write, readdir, unlink.
 *
 * Compile-time limits:
 *   RAMFS_MAX_FILES - maximum file nodes
 *   RAMFS_MAX_DIRS - maximum directory nodes
 *   RAMFS_HEAP_SIZE - total bytes available for file data
 */

#define RAMFS_MAX_FILES  128
#define RAMFS_MAX_DIRS    64
#define RAMFS_HEAP_SIZE  (8 * 1024 * 1024)   /* 8 MiB for file data (initrd
                                              * binaries live here, dash
                                              * alone is ~250 KiB static) */

/* Initialise ramfs and return the root vfs_node ready to mount at '/'. */
vfs_node_t *ramfs_init(void);

/* Low-level helpers (used by kernel init to pre-populate the tree) */
vfs_node_t *ramfs_mkdir (vfs_node_t *parent, const char *name);
vfs_node_t *ramfs_mkfile(vfs_node_t *parent, const char *name,
                          const void *data, size_t size);

/* Map the ramfs file-data heap into the shared kernel half (boot time). */
bool ramfs_heap_init(void);
