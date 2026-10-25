#pragma once
#include "fs/vfs.h"
#include "drivers/ata/ata.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/*
 * ext4 - read-only ext2/ext3/ext4 driver
 *
 * Supports:
 *   - ext2 / ext3 / ext4 superblock detection
 *   - 32-bit and 64-bit block addressing
 *   - Extent tree (ext4 flag EXT4_EXTENTS_FL)
 *   - Indirect block maps (ext2/ext3 and ext4 without extents)
 *   - Directory htree (linear scan fallback)
 *   - Regular files, directories, symlinks (read target)
 *   - Inline data (small files stored in inode)
 *
 * Does not support: writing, journaling, ACLs, encryption, compression.
 *
 * Usage:
 *   1. Call ext4_probe() to check if a partition looks like ext4.
 *   2. Call ext4_mount() to get a VFS root node.
 *   3. Mount the result with vfs_mount().
 */

/* Result codes */
#define EXT4_OK         0
#define EXT4_ERR_MAGIC -1
#define EXT4_ERR_IO    -2
#define EXT4_ERR_INVAL -3
#define EXT4_ERR_NOMEM -4

/*
 * Probe: returns EXT4_OK if the partition starting at `lba_start`
 * on bus/drive looks like a valid ext2/3/4 filesystem.
 */
int ext4_probe(int bus, int drive, uint64_t lba_start);

/*
 * Mount: initialise the filesystem and return a VFS root node.
 * lba_start is the first sector of the partition (from the MBR/GPT).
 * Returns NULL on error.
 */
vfs_node_t *ext4_mount(int bus, int drive, uint64_t lba_start);
