#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* Node types */
#define VFS_TYPE_FILE   1
#define VFS_TYPE_DIR    2
#define VFS_TYPE_DEV    3   /* character device (e.g. /dev/null, /dev/tty) */
#define VFS_TYPE_LINK   4   /* symbolic link (future) */

/* POSIX-style open flags
 * These values are the LINUX x86-64 ABI values (asm-generic/fcntl.h) because
 * userland (musl) passes them directly via SYS_OPEN / SYS_OPENAT.
 */
#define O_ACCMODE  0x0003
#define O_RDONLY   0x0000
#define O_WRONLY   0x0001
#define O_RDWR     0x0002
#define O_CREAT    0x0040   /* 0100     */
#define O_EXCL     0x0080   /* 0200     */
#define O_NOCTTY   0x0100   /* 0400     */
#define O_TRUNC    0x0200   /* 01000    */
#define O_APPEND   0x0400   /* 02000    */
#define O_NONBLOCK 0x0800   /* 04000    */
#define O_DIRECTORY 0x10000 /* 0200000  */
#define O_NOFOLLOW 0x20000  /* 0400000  */
#define O_CLOEXEC  0x80000  /* 02000000 */

/* errno-style error codes */
#define VFS_EPERM    1
#define VFS_ENOENT   2
#define VFS_EIO      5
#define VFS_EAGAIN   11
#define VFS_EBADF    9
#define VFS_ENOMEM  12
#define VFS_EACCES  13
#define VFS_EEXIST  17
#define VFS_ENOTDIR 20
#define VFS_EISDIR  21
#define VFS_EINVAL  22
#define VFS_ENODEV  19
#define VFS_ENOSPC  28
#define VFS_EXDEV   18
#define VFS_ENOTEMPTY 39

/* Limits */
#define VFS_NAME_MAX   64
#define VFS_PATH_MAX  256
#define VFS_MAX_NODES 2048
#define VFS_MAX_FDS    32   /* 0=stdin, 1=stdout, 2=stderr, 3+ user */
#define VFS_MAX_MOUNTS  8

typedef struct vfs_node vfs_node_t;

/* File operations vtable */
typedef struct {
    int         (*open)   (vfs_node_t *node, int flags);
    int         (*close)  (vfs_node_t *node);
    int         (*read)   (vfs_node_t *node, void *buf, size_t len, size_t off);
    int         (*write)  (vfs_node_t *node, const void *buf, size_t len, size_t off);
    /* readdir: idx 0..N-1, fills name[VFS_NAME_MAX] and *type_out.
     * Returns 0 on success, -1 when idx is past the last entry. */
    int         (*readdir)(vfs_node_t *dir, int idx,
                           char *name_out, uint8_t *type_out);
    vfs_node_t *(*finddir)(vfs_node_t *dir, const char *name);
    /* mkdir/creat: create child node inside dir */
    vfs_node_t *(*mknod)  (vfs_node_t *dir, const char *name, uint8_t type);
    /* unlink: remove child named 'name' from dir.  Returns 0 or -errno. */
    int         (*unlink) (vfs_node_t *dir, const char *name);
    /* persist mode/uid/gid changes to backing storage (ext2 inode write-
     * back).  mode==0 -> don't touch mode, uid/gid == (uint32_t)-1 -> don't
     * touch that field.  Returns 0 or -errno.  NULL = in-core only. */
    int         (*setattr)(vfs_node_t *node, uint32_t mode,
                           uint32_t uid, uint32_t gid);
    /* rename: move the on-disk directory entry of node (currently named
     * node->name inside node->parent) to NEWDIR/NEWNAME.  Filesystems
     * whose on-disk state is the truth (ext2) implement this, in-core
     * trees (ramfs) may leave it NULL - vfs_rename then only re-parents
     * the cached node.  Returns 0 or -errno. */
    int         (*frename)(vfs_node_t *node, vfs_node_t *newdir,
                           const char *newname);
} vfs_ops_t;

/* VFS node (inode-like) */
struct vfs_node {
    char            name[VFS_NAME_MAX];
    uint8_t         type;           /* VFS_TYPE_* */
    uint8_t         pinned;         /* 1 = mount-point root, never free on close */
    uint32_t        size;           /* bytes (files only) */
    uint32_t        inode;          /* unique id */
    uint32_t        mode;           /* Full linux mode incl. S_IF* + rwx bits */
    uint32_t        uid;            /* owner user id  (0 = root) */
    uint32_t        gid;            /* owner group id (0 = root) */
    uint32_t        mtime;          /* unix epoch seconds (0 = unknown) */
    void           *data;           /* fs-private payload */
    const vfs_ops_t *ops;
    vfs_node_t     *parent;         /* NULL for root */
    /* Directory children (for simple in-core dirs) */
    vfs_node_t     *children[32];
    int             child_count;
};

/* stat structure (mirrors struct stat subset) */
typedef struct {
    uint32_t st_ino;
    uint8_t  st_type;
    uint32_t st_size;
    uint32_t st_mode;   /* full linux mode (S_IF* | perms); 0 = derive */
    uint32_t st_uid;    /* owner uid (0 = root) */
    uint32_t st_gid;    /* owner gid (0 = root) */
    uint32_t st_mtime;  /* unix epoch seconds; 0 = use current time   */
    char     st_name[VFS_NAME_MAX];
} vfs_stat_t;

/* Open file descriptor */
typedef struct {
    bool        used;
    vfs_node_t *node;
    size_t      offset;
    int         flags;
} vfs_fd_t;

/* Mount table entry */
typedef struct {
    char        path[VFS_PATH_MAX];
    vfs_node_t *root;
    bool        used;
} vfs_mount_t;

/*
 *  Public API
 */

/* Must be called once before anything else */
void vfs_init(void);

/* Mount a filesystem root at path.  '/' must come first. */
bool vfs_mount(const char *path, vfs_node_t *root);

/* Resolve a path to its node (returns NULL if not found) */
vfs_node_t *vfs_resolve(const char *path);

/* Allocate and partially initialise a node from the pool */
vfs_node_t *vfs_create_node(const char *name, uint8_t type,
                             const vfs_ops_t *ops, void *data);

/* Free a node back to the pool (safe to call with NULL) */
void vfs_free_node(vfs_node_t *node);

/* Attach child to parent directory */
bool vfs_attach(vfs_node_t *parent, vfs_node_t *child);

/* POSIX fd-based API */
int  vfs_open   (const char *path, int flags);   /* returns fd or -errno */
int  vfs_close  (int fd);
int  vfs_read   (int fd, void *buf, size_t len);
int  vfs_write  (int fd, const void *buf, size_t len);
int  vfs_readdir(int fd, int idx, char *name_out, uint8_t *type_out);
int  vfs_fstat  (int fd, vfs_stat_t *st);
int  vfs_lseek  (int fd, size_t offset, int whence);
int  vfs_dup    (int fd);
int  vfs_dup2   (int fd, int newfd);
/* True when fd refers to the console pseudo-device (used slot, NULL node).
 * Works for any fd number: dash dups the tty to fd >= 10 via fcntl
 * F_DUPFD for job control, so tty classification by fd number alone
 * (0/1/2) breaks tcgetpgrp/tcgetattr on the dup'd fd.  Every other fd
 * kind (file, dir, pipe, dev, kobj) carries a real node pointer. */
bool vfs_fd_is_console(int fd);

/* Path-based helpers */
int  vfs_stat   (const char *path, vfs_stat_t *st);  /* 0 or -errno */
int  vfs_mkdir  (const char *path);                  /* 0 or -errno */
int  vfs_creat  (const char *path);                  /* 0 or -errno */
int  vfs_unlink (const char *path);                  /* 0 or -errno */
int  vfs_rename (const char *oldpath, const char *newpath); /* 0 or -errno */
int  vfs_chmod  (const char *path, uint32_t mode);   /* 0 or -errno */
int  vfs_fchmod (int fd, uint32_t mode);             /* 0 or -errno */

/* chown family (real ownership storage)
 * (uint32_t)-1 for uid/gid means "leave unchanged" (chown semantics).  */
int  vfs_chown  (const char *path, uint32_t uid, uint32_t gid); /* 0 or -errno */
int  vfs_fchown (int fd, uint32_t uid, uint32_t gid);           /* 0 or -errno */

/* Unix permission enforcement
 * Credential hooks (vfs_cred_euid/_egid/_is_root/_in_group/_umask) are
 * implemented in exec/process.c so the VFS layer stays independent of
 * the process table.  vfs_check_perm(node, 4|2|1) tests r/w/x against
 * owner/group/other mode bits with root bypass, vfs_check_exec is the
 * execve flavour (any x bit suffices for root).  Returns true=granted. */
uint32_t vfs_cred_euid(void);
uint32_t vfs_cred_egid(void);
bool     vfs_cred_is_root(void);
bool     vfs_cred_in_group(uint32_t gid);
uint32_t vfs_cred_umask(void);
bool vfs_check_perm(vfs_node_t *node, int bits);
bool vfs_check_exec(vfs_node_t *node);   /* execve flavour */

/* fd/node helpers for the *at() syscall family
 * vfs_node_from_fd() returns the vnode backing an open fd (NULL + -EBADF
 * semantics via vfs_errno when invalid).
 * vfs_node_path() reconstructs an absolute path for a node by walking the
 * parent chain (mount roots are matched against the mount table).  Returns
 * 0 on success, -1 when the node has no derivable path.               */
vfs_node_t *vfs_node_from_fd(int fd);
int  vfs_node_path(vfs_node_t *node, char *out, size_t outsz);
int  vfs_fd_setflags(int fd, int flags);             /* 0 or -EBADF */

/* Per-process fd table management
 *
 * The global fd_table inside vfs.c is a logical view of the current process's
 * open files.  On fork/exec/exit the kernel must save, clone, or restore this
 * table so that each process has an independent fd namespace.
 *
 * vfs_fd_save(dst) - copy the live fd_table into dst[VFS_MAX_FDS]
 * vfs_fd_restore(src) - install src[VFS_MAX_FDS] as the live fd_table
 * vfs_fd_clone(dst,src) - deep-copy src into dst (for fork: child inherits
 *                          parent's open files but gets independent slots)
 * vfs_fd_close_cloexec() - close all fds with O_CLOEXEC set (called on execve)
 * vfs_fd_close_all() - close every fd > 2 (called on process exit cleanup)
 *
 * Caller is responsible for providing arrays of VFS_MAX_FDS vfs_fd_t each.
 */
void vfs_fd_save        (vfs_fd_t dst[VFS_MAX_FDS]);
/* live-table peek for /proc/self/fd - node behind fd in the
 * Current live table (NULL = console pseudo-fd / unused / bad fd). */
vfs_node_t *vfs_fd_peek_node(int fd);
void vfs_fd_restore     (const vfs_fd_t src[VFS_MAX_FDS]);
void vfs_fd_clone       (vfs_fd_t dst[VFS_MAX_FDS], const vfs_fd_t src[VFS_MAX_FDS]);
void vfs_fd_close_cloexec (void);
void vfs_fd_close_all   (void);

/* Refcounted fd tables
 * A CLONE_FILES clone (musl pthread_create) must share one fd namespace:
 * a file opened by one thread is immediately visible to its siblings.
 * The old inline per-task array cannot express that, so tasks now point at
 * a vfs_fdt_t from a small static pool:
 *
 *   vfs_fdt_new() - fresh empty table (refcnt 1)
 *   vfs_fdt_clone() - deep copy of an existing table (fork semantics)
 *   vfs_fdt_share() - take a reference (CLONE_FILES semantics)
 *   vfs_fdt_put() - drop a reference, freed back to the pool at zero
 *
 * The save/restore snapshot model is unchanged, tasks sharing a table
 * simply save/restore through the same memory. */
typedef struct {
    vfs_fd_t t[VFS_MAX_FDS];
    int      refcnt;
} vfs_fdt_t;

void       vfs_pipe_refs_bump(const vfs_fd_t table[VFS_MAX_FDS]);

vfs_fdt_t *vfs_fdt_new   (void);
vfs_fdt_t *vfs_fdt_clone (vfs_fdt_t *src);   /* deep copy, refcnt = 1     */
vfs_fdt_t *vfs_fdt_share (vfs_fdt_t *t);     /* refcnt++, returns t       */
void       vfs_fdt_put   (vfs_fdt_t *t);     /* refcnt--; pool-free at 0  */

/* Return the open flags (O_RDONLY / O_WRONLY / O_RDWR / O_APPEND ...) that
 * were passed to vfs_open() for fd.  Returns -EBADF if fd is not open. */
int  vfs_fd_getflags    (int fd);

/* Write entire buf as file content (creates or overwrites) */
bool vfs_write_file(const char *path, const void *buf, size_t len);

/* Debug: print tree to console */
void vfs_print_tree(vfs_node_t *node, int depth);

/* Node-pool accounting for statfs: *used_out = allocated inodes,
 * *total_out = pool capacity.  Either pointer may be NULL. */
void vfs_node_stats(int *used_out, int *total_out);

/* Global last errno (set by vfs functions on error) */
extern int vfs_errno;

/* Anonymous pipe
 *
 * vfs_pipe(fds)  creates a kernel ring-buffer and installs two fds:
 *   fds[0]  read  end  (O_RDONLY)
 *   fds[1]  write end  (O_WRONLY)
 *
 * The two ends share one pipe_buf_t via the vfs_node data pointer.
 * write() appends, read() consumes.  When the write-end fd is closed the
 * read-end returns 0 (EOF) once the buffer is drained.
 *
 * Returns 0 on success, -errno on failure.
 */
#define PIPE_BUF_SIZE  4096   /* bytes in the shared ring buffer           */

typedef struct vfs_node *vfs_node_ptr;
typedef struct {
    char     buf[PIPE_BUF_SIZE];
    size_t   rpos;            /* next byte to read                         */
    size_t   wpos;            /* next byte to write                        */
    size_t   len;             /* bytes currently in buffer                 */
    int      write_open;      /* number of write-end fds still open        */
    int      read_open;       /* number of read-end fds still open         */
    void    *rn;              /* read-end node (for detach on free)        */
    void    *wn;              /* write-end node                            */
} pipe_buf_t;

/* VFS type tag for pipe nodes */
#define VFS_TYPE_PIPE  5

/* Kernel-object fd (unix sockets, epoll, eventfd, timerfd, signalfd,
 * memfd) - private ops table, node->data points to the object struct. */
#define VFS_TYPE_KOBJ  6

int vfs_pipe(int fds[2]);     /* returns 0 or -errno */

/* Kernel-object fd installation (kobj.c)
 * Installs a detached node (sockets / epoll / eventfd / ...) into the
 * current process's fd table and returns the fd, or -errno.          */
int  vfs_install_fd(vfs_node_t *node, int flags);

/* Readiness probe for poll/epoll: returns the subset of `events`
 * (POLLIN/POLLOUT/...) that is ready right now, 0 if not ready.
 * Handles pipes, kernel objects and the console fds. */
int  vfs_poll_ready(int fd, int events);

/* fd snapshot (SCM_RIGHTS fd passing)
 * vfs_fd_snapshot copies the live fd-table entry for `fd` into *out
 * (0 or -EBADF).  vfs_install_snapshot installs a snapshot as a new fd
 * in the current table (used when a fd arrives over a unix socket). */
int  vfs_fd_snapshot(int fd, vfs_fd_t *out);
int  vfs_install_snapshot(const vfs_fd_t *snap);