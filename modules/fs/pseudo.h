#pragma once
#include "fs/vfs.h"

/*
 * pseudo.h - procfs-lite + devtmpfs
 *
 * pseudo_mount_all() builds two synthetic trees and registers them as
 * mount points:
 *
 *   /proc - dynamic process/kernel information files:
 *     cpuinfo, meminfo, uptime, version, mounts,
 *     self/{cmdline, status, stat}   (content generated per read,
 *                                     reflecting the calling process)
 *
 *   /dev - character devices: null, zero, console, tty, random, urandom,
 *          fb0 + the /dev/shm POSIX shared-memory namespace
 *
 * The trees are mostly READ-ONLY (file content is regenerated on every
 * read() from live kernel state); /dev/shm is the exception, it supports
 * lazy file creation and unlink with POSIX shm semantics.
 * Mount registration is done via vfs_mount(); vfs_resolve() gained
 * longest-prefix mount crossing so /proc and /dev work on both the
 * initrd (ramfs) and the ext2 boot paths.
 */

/* Build and register both trees.  Call once after the root filesystem
 * is mounted (both boot paths).  Safe to call only during boot. */
void pseudo_mount_all(void);

/* Node classification
 * pseudo_node_is_console_tty: node is /dev/tty or /dev/console - vfs_open
 *   installs the console pseudo-fd for it so opening the device path
 *   behaves exactly like the inherited console fds (dash job control).
 * pseudo_node_is_fb0: node is /dev/fb0 (FB ioctls + mmap dispatch).
 * pseudo_node_is_evdev: node is /dev/input/eventN. */
bool pseudo_node_is_console_tty(vfs_node_t *n);
bool pseudo_node_is_fb0(vfs_node_t *n);
bool pseudo_node_is_evdev(vfs_node_t *n, int *idx);
/* pseudo_node_is_drm: node is /dev/dri/card0 or renderD128. */
bool pseudo_node_is_drm(vfs_node_t *n, int *card);

/* procfs dynamic symlinks (/proc/self/exe, /proc/self/fd/*)
 * pseudo_link_target(node, BUF, CAP) fills BUF with the symlink's
 * Current target (for /proc/self/* these depend on the calling process
 * and are regenerated per call).  Returns the target length, 0 when
 * Node is not a pseudo symlink, or negative -VFS errno. */
int pseudo_link_target(vfs_node_t *node, char *buf, size_t cap);
