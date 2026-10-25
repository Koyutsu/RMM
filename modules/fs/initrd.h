#pragma once
#include "fs/vfs.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/*
 * initrd - newc CPIO archive loader
 *
 * Parses a newc-format CPIO archive (as built by `find | cpio -H newc`)
 * and populates a VFS tree from it.  Used to load /bin, /etc and other
 * real files from the initrd into the in-RAM VFS at boot.
 *
 * The archive is passed to the kernel as a GRUB multiboot2 module.
 */

/*
 * Walk a newc CPIO archive starting at `data` (physical address, already
 * mapped identity) of `size` bytes, and create every regular file and
 * directory found inside `root`.
 *
 * Returns the number of entries loaded (>= 0), or -1 on fatal error.
 */
int initrd_load(vfs_node_t *root, const void *data, size_t size);

/*
 * Find the multiboot2 module tag in the info block and call initrd_load().
 * Returns true if a module was found and loaded successfully.
 */
bool initrd_load_from_mb2(vfs_node_t *root, uint64_t mb2_info_phys);