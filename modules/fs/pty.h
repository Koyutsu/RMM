#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "fs/vfs.h"

/*
 *  pty - BSD-style pseudo-terminal pairs
 *
 *  /dev/ptmx - opening allocates a fresh pty pair, the fd refers to the
 *               Master end (per-pty node).  TIOCGPTN returns the pts index,
 *               TIOCSPTLCK sets the lock bit, TIOCGPTPEER opens the slave.
 *  /dev/pts/N - the slave end, materialised by the devpts finddir hook.
 *
 *  Data paths:
 *    master write -> line discipline (canonical editing + echo) -> slave read
 *    slave write  -> output ring                                -> master read
 *
 *  Both ends park in sched_kernel_park() loops when empty/full (the same
 *  blocking model as the console read path and pipes) and report real
 *  readiness to poll/epoll through vfs_poll_ready().
 *
 *  Refcounts mirror pipes: one reference per open end, fork takes its own
 *  reference (vfs_pipe_refs_bump handles pty nodes too); when both ends
 *  drop to zero the pty and its two nodes are released.
 */

#define PTY_MAX        8      /* simultaneous pty pairs                  */
#define PTY_BUF_SIZE   4096   /* bytes per direction ring                */
#define PTY_LINE_MAX   512    /* canonical line-assembly buffer          */

/* True when node is a pty master / slave end (ops-table identity). */
bool pty_node_is_master(vfs_node_t *n);
bool pty_node_is_slave(vfs_node_t *n);

/* True when node is the /dev/ptmx cloning device (pseudo.c dev node). */
bool pseudo_node_is_ptmx(vfs_node_t *n);

/* forward type so the API below can name a pty object */
typedef struct pty pty_t;

/* vfs_open hook for /dev/ptmx: allocate a pty pair and fill *slot with the
 * master-end descriptor.  Returns 0, or -errno (ENOMEM/ENODEV). */
int pty_open_ptmx(vfs_node_t *node, vfs_fd_t *slot, int flags);

/* Fill *out with a slave-end descriptor for PT (used by TIOCGPTPEER and
 * by the devpts open path).  Returns 0 or -errno. */
int pty_open_slave_fd(pty_t *pt, int flags, vfs_fd_t *out);

/* pty number behind a master/slave node (-1 when invalid). */
int pty_node_index(vfs_node_t *n);

/* /proc/self/fd readlink helper: render "/dev/ptmx" or "/dev/pts/N" for a
 * pty end.  Returns false when N is not a live pty node. */
bool pty_node_paths(vfs_node_t *n, char *out, size_t outsz);

/* devpts directory hooks (wired into pseudo.c's /dev/pts node). */
vfs_node_t *pty_pts_find(vfs_node_t *dir, const char *name);
int pty_pts_readdir(vfs_node_t *dir, int idx, char *name_out, uint8_t *type_out);

/* ioctl router - called from impl_ioctl for master/slave nodes.
 * FD is the requesting descriptor (needed by TIOCGPTPEER fd install). */
int64_t pty_ioctl(vfs_node_t *n, int fd, uint64_t req, uint64_t arg);

/* poll readiness (vfs_poll_ready branch): subset of events ready now. */
int pty_poll_node(vfs_node_t *n, int events);

/* vfs_close hook - drop this descriptor's reference on a pty end.
 * Inherited has the pipe fork-view semantics (child's borrowed slots do
 * not release). */
void pty_on_close(vfs_node_t *n, bool inherited);

/* fork hook - take one reference per pty end in table (mirrors
 * vfs_pipe_refs_bump, called from the fork paths alongside it). */
void pty_refs_bump(const vfs_fd_t table[VFS_MAX_FDS]);

/* dup hook - a freshly copied slot must own its own reference, because
 * closing either slot releases.  Called from vfs_dup/vfs_dup2. */
void pty_ref_bump_node(vfs_node_t *n);
