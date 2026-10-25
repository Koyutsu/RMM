#include "fs/vfs.h"
#include "fs/pty.h"

#include "console/display.h"
#include "exec/sched.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* Global errno */
int vfs_errno = 0;

/* Linux S_IF* type bits (local copies, vfs.h stays exec-agnostic) */
#define VFS_S_IFMT  0170000
#define VFS_S_IFREG 0100000
#define VFS_S_IFDIR 0040000
#define VFS_S_IFCHR 0020000
#define VFS_S_IFBLK 0060000
#define VFS_S_IFIFO 0010000

/* forward declarations - permission layer is defined below the fd API */
static uint32_t node_eff_mode(vfs_node_t *n);
bool vfs_check_perm(vfs_node_t *n, int bits);
bool vfs_check_exec(vfs_node_t *n);
static bool dir_writable(vfs_node_t *dir);

/* Node pool */
static vfs_node_t node_pool[VFS_MAX_NODES];
static bool       node_used[VFS_MAX_NODES];
static uint32_t   next_inode = 1;

/* File descriptor table (0=stdin,1=stdout,2=stderr reserved) */
static vfs_fd_t fd_table[VFS_MAX_FDS];

/* Inherited-view tracking (cooperative fork): the forked child's fd slots
 * are borrowed views, closing them must not touch pipe refcounts. */
static bool fd_inherited[VFS_MAX_FDS];
static void pipe_maybe_free(pipe_buf_t *pb);

/* Mount table */
static vfs_mount_t mount_table[VFS_MAX_MOUNTS];
static int         mount_count = 0;

/* Root node */
static vfs_node_t *vfs_root = NULL;

/* String helpers (no libc) */
static size_t vfs_strlen(const char *s) {
    size_t n = 0; while (s[n]) n++; return n;
}
static void vfs_strcpy(char *d, const char *s, size_t max) {
    size_t i = 0;
    while (s[i] && i < max - 1) { d[i] = s[i]; i++; }
    d[i] = '\0';
}
static int vfs_strcmp(const char *a, const char *b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}
static int vfs_strncmp(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i]) return (unsigned char)a[i] - (unsigned char)b[i];
        if (!a[i]) return 0;
    }
    return 0;
}
static void vfs_memzero(void *p, size_t n) {
    uint8_t *b = (uint8_t *)p;
    while (n--) *b++ = 0;
}

/* Default dir ops (used by in-core directories) */
static int dir_readdir(vfs_node_t *dir, int idx,
                        char *name_out, uint8_t *type_out) {
    if (idx < 0 || idx >= dir->child_count) return -1;
    vfs_strcpy(name_out, dir->children[idx]->name, VFS_NAME_MAX);
    *type_out = dir->children[idx]->type;
    return 0;
}
static vfs_node_t *dir_finddir(vfs_node_t *dir, const char *name) {
    for (int i = 0; i < dir->child_count; i++)
        if (!vfs_strcmp(dir->children[i]->name, name))
            return dir->children[i];
    return NULL;
}
static vfs_node_t *dir_mknod(vfs_node_t *parent, const char *name, uint8_t type);
static int dir_unlink(vfs_node_t *dir, const char *name);

static const vfs_ops_t default_dir_ops = {
    .readdir = dir_readdir,
    .finddir = dir_finddir,
    .mknod   = dir_mknod,
    .unlink  = dir_unlink,
};

/* Node allocator */
vfs_node_t *vfs_create_node(const char *name, uint8_t type,
                              const vfs_ops_t *ops, void *data) {
    for (int i = 0; i < VFS_MAX_NODES; i++) {
        if (!node_used[i]) {
            node_used[i] = true;
            vfs_node_t *n = &node_pool[i];
            vfs_memzero(n, sizeof(vfs_node_t));
            vfs_strcpy(n->name, name, VFS_NAME_MAX);
            n->type  = type;
            n->ops   = ops ? ops : (type == VFS_TYPE_DIR ? &default_dir_ops : NULL);
            n->data  = data;
            n->inode = next_inode++;
            /* Default Linux mode (type bits + permissions).  Note: pipe
             * nodes abuse mode as a read/write-end discriminator (0/1) -
             * never give them type bits. */
            if (type == VFS_TYPE_DIR)       n->mode = VFS_S_IFDIR | 0755;
            else if (type == VFS_TYPE_DEV)  n->mode = VFS_S_IFCHR | 0666;
            else if (type == VFS_TYPE_PIPE) n->mode = 0;
            else                            n->mode = VFS_S_IFREG | 0644;
            n->mtime = 0;
            return n;
        }
    }
    vfs_errno = VFS_ENOMEM;
    return NULL;
}

/* Free a node back to the pool (for ephemeral finddir nodes) */
void vfs_free_node(vfs_node_t *node) {
    if (!node) return;
    for (int i = 0; i < VFS_MAX_NODES; i++) {
        if (node_used[i] && &node_pool[i] == node) {
            node_used[i] = false;
            return;
        }
    }
}

/* Attach child to parent dir */
bool vfs_attach(vfs_node_t *parent, vfs_node_t *child) {
    if (!parent || !child) return false;
    if (parent->child_count >= 32) { vfs_errno = VFS_ENOSPC; return false; }
    child->parent = parent;
    parent->children[parent->child_count++] = child;
    return true;
}

/* Default mknod for in-core dirs */
static vfs_node_t *dir_mknod(vfs_node_t *parent, const char *name, uint8_t type) {
    /* Check for duplicate */
    for (int i = 0; i < parent->child_count; i++)
        if (!vfs_strcmp(parent->children[i]->name, name)) {
            vfs_errno = VFS_EEXIST;
            return NULL;
        }
    vfs_node_t *n = vfs_create_node(name, type, NULL, NULL);
    if (!n) return NULL;
    if (!vfs_attach(parent, n)) return NULL;
    return n;
}

/* Default unlink for in-core dirs */
static int dir_unlink(vfs_node_t *dir, const char *name) {
    for (int i = 0; i < dir->child_count; i++) {
        if (!vfs_strcmp(dir->children[i]->name, name)) {
            vfs_node_t *victim = dir->children[i];
            if (victim->type == VFS_TYPE_DIR && victim->child_count > 0)
                return -(int)VFS_ENOTEMPTY;
            /* Mark node as free */
            for (int j = 0; j < VFS_MAX_NODES; j++) {
                if (node_used[j] && &node_pool[j] == victim) {
                    node_used[j] = false;
                    break;
                }
            }
            /* Compact children array */
            for (int j = i; j < dir->child_count - 1; j++)
                dir->children[j] = dir->children[j + 1];
            dir->child_count--;
            return 0;
        }
    }
    return -(int)VFS_ENOENT;
}

/* Path resolution */
/* Mount-point crossing
 * vfs_mount() registers {path -> root}; resolve now honours those mounts
 * via longest-prefix matching, so /proc and /dev (registered by
 * pseudo_mount_all) resolve into their synthetic trees on every boot
 * path.  Components are walked inside the mount root with the same
 * finddir/children logic as the main walk.  Read-only: any create that
 * resolves into a mount root fails later at the ops level.            */
static vfs_node_t *mount_walk(const char *path) {
    vfs_node_t *best_root  = NULL;
    size_t      best_len   = 0;

    for (int i = 0; i < mount_count; i++) {
        if (!mount_table[i].used) continue;
        const char *mp = mount_table[i].path;
        size_t len = vfs_strlen(mp);
        if (len <= 1) continue;                       /* "/" handled elsewhere */
        if (vfs_strncmp(path, mp, len) != 0) continue;
        if (path[len] != '\0' && path[len] != '/') continue;
        if (len > best_len) {
            best_len  = len;
            best_root = mount_table[i].root;
        }
    }
    if (!best_root) return NULL;

    /* Walk the remainder inside the mount root. */
    vfs_node_t *cur = best_root;
    const char *p   = path + best_len;
    while (*p && cur) {
        if (*p == '/') { p++; continue; }
        char comp[VFS_NAME_MAX];
        int  ci = 0;
        while (*p && *p != '/') {
            if (ci < (int)VFS_NAME_MAX - 1) comp[ci++] = *p;
            p++;
        }
        comp[ci] = '\0';
        if (ci == 0) continue;
        if (comp[0] == '.' && comp[1] == '\0') continue;
        if (comp[0] == '.' && comp[1] == '.' && comp[2] == '\0') {
            cur = cur->parent ? cur->parent : best_root;   /* capped at mount root */
            continue;
        }

        if (cur->ops && cur->ops->finddir) {
            cur = cur->ops->finddir(cur, comp);
        } else {
            vfs_node_t *found = NULL;
            for (int i = 0; i < cur->child_count; i++)
                if (!vfs_strcmp(cur->children[i]->name, comp))
                    { found = cur->children[i]; break; }
            cur = found;
        }
    }
    return cur;   /* may be NULL -> ENOENT by caller */
}

vfs_node_t *vfs_resolve(const char *path) {
    if (!path || path[0] != '/') { vfs_errno = VFS_EINVAL; return NULL; }
    if (path[1] == '\0') return vfs_root;

    /* Longest-prefix mount crossing first (e.g. /proc/cpuinfo). */
    if (mount_count > 0) {
        vfs_node_t *m = mount_walk(path);
        if (m) return m;
        /* Path lives under a mount but wasn't found there -> it must not
         * fall through to the root tree (which has a different "proc"). */
        for (int i = 0; i < mount_count; i++) {
            if (!mount_table[i].used) continue;
            size_t len = vfs_strlen(mount_table[i].path);
            if (len <= 1) continue;
            if (vfs_strncmp(path, mount_table[i].path, len) == 0 &&
                (path[len] == '\0' || path[len] == '/')) {
                vfs_errno = VFS_ENOENT;
                return NULL;
            }
        }
    }

    vfs_node_t *cur = vfs_root;
    const char *p   = path + 1;

    while (*p && cur) {
        /* Extract component */
        char comp[VFS_NAME_MAX];
        int  ci = 0;
        while (*p && *p != '/') {
            if (ci < (int)VFS_NAME_MAX - 1) comp[ci++] = *p;
            p++;
        }
        comp[ci] = '\0';
        if (*p == '/') p++;
        if (ci == 0) continue;

        /* ".." navigation */
        if (comp[0] == '.' && comp[1] == '.' && comp[2] == '\0') {
            if (cur->parent) cur = cur->parent;
            continue;
        }
        /* "." - stay */
        if (comp[0] == '.' && comp[1] == '\0') continue;

        /* search (x) permission on every directory we traverse, checked
         * Before descending, cur is non-NULL here (loop invariant) */
        if (cur->type == VFS_TYPE_DIR && !vfs_check_perm(cur, 1)) {
            vfs_errno = VFS_EACCES;
            return NULL;
        }

        if (cur->ops && cur->ops->finddir)
            cur = cur->ops->finddir(cur, comp);
        else {
            vfs_node_t *found = NULL;
            for (int i = 0; i < cur->child_count; i++)
                if (!vfs_strcmp(cur->children[i]->name, comp))
                    { found = cur->children[i]; break; }
            cur = found;
        }
    }
    if (!cur) vfs_errno = VFS_ENOENT;
    return cur;
}

/* Resolve parent dir of path, return basename in *base_out */
static vfs_node_t *resolve_parent(const char *path,
                                   const char **base_out) {
    /* Find last '/' */
    size_t len = vfs_strlen(path);
    int last = -1;
    for (int i = (int)len - 1; i >= 0; i--)
        if (path[i] == '/') { last = i; break; }

    if (last <= 0) {
        /* e.g. "/foo" -> parent is root */
        *base_out = path + (last < 0 ? 0 : last + 1);
        return vfs_root;
    }

    char parent_path[VFS_PATH_MAX];
    vfs_strcpy(parent_path, path, (size_t)last + 1);
    parent_path[last] = '\0';
    *base_out = path + last + 1;
    return vfs_resolve(parent_path);
}

/* Mount */
bool vfs_mount(const char *path, vfs_node_t *root) {
    if (!path || !root) return false;
    if (mount_count >= VFS_MAX_MOUNTS) return false;

    /* Any mount root must never be auto-freed while mounted, the mount
     * table keeps the only reference (parent == NULL).  A non-pinned root
     * was recycled by the node pool once /tmp files cycled through it and
     * every subsequent /tmp access then resolved into an unrelated node
     * (observed: opens returned EPERM because the "mount root" had become
     * a regular file). */
    root->pinned = 1;

    if (path[0] == '/' && path[1] == '\0') {
        vfs_root = root;
        root->parent = NULL;
    }

    vfs_strcpy(mount_table[mount_count].path, path, VFS_PATH_MAX);
    mount_table[mount_count].root = root;
    mount_table[mount_count].used = true;
    mount_count++;
    return true;
}

/* FD helpers */
static int alloc_fd(void) {
    for (int i = 3; i < VFS_MAX_FDS; i++)
        if (!fd_table[i].used) return i;
    return -(int)VFS_ENOMEM;
}

/* Public fd API */
int vfs_open(const char *path, int flags) {
    vfs_node_t *node = vfs_resolve(path);

    if (!node) {
        /* O_CREAT: create file */
        if (!(flags & O_CREAT)) return -(int)VFS_ENOENT;
        const char *base;
        vfs_node_t *parent = resolve_parent(path, &base);
        if (!parent) return -(int)VFS_ENOENT;
        if (!parent->ops || !parent->ops->mknod) return -(int)VFS_EPERM;
        if (!dir_writable(parent)) return -(int)VFS_EACCES;
        node = parent->ops->mknod(parent, base, VFS_TYPE_FILE);
        if (!node) return -(int)vfs_errno;
        /* new file owned by the creator (group: creator's egid, no
         * setgid-directory inheritance yet, we have no sticky/setgid
         * directory semantics) */
        node->uid = vfs_cred_euid();
        node->gid = vfs_cred_egid();
    }

    if ((flags & O_DIRECTORY) && node->type != VFS_TYPE_DIR)
        return -(int)VFS_ENOTDIR;

    /* Access-mode permission check on the resolved target.  Devices and
     * pipes skip this (console fds must always work, pipe rights are
     * encoded in the fd itself). */
    if (node->type == VFS_TYPE_FILE || node->type == VFS_TYPE_DIR) {
        int acc = flags & O_ACCMODE;
        int need = (acc == O_RDONLY) ? 4 : (acc == O_WRONLY) ? 2 : (4 | 2);
        if (!vfs_check_perm(node, need)) return -(int)VFS_EACCES;
    }

    int fd = alloc_fd();
    if (fd < 0) return -(int)VFS_ENOMEM;

    if (node->ops && node->ops->open) {
        int r = node->ops->open(node, flags);
        if (r < 0) return r;
    }

    /* O_TRUNC */
    if ((flags & O_TRUNC) && node->type == VFS_TYPE_FILE && node->ops && node->ops->write)
        node->ops->write(node, NULL, 0, 0);   /* backend handles truncation */

    /* /dev/tty and /dev/console are the controlling terminal.
     * Opening them installs the console pseudo-fd (used slot, NULL node)
     * - exactly what init/login/dash inherited, so termios/tcgetpgrp/
     * job control work on fds obtained by opening the device paths.
     * Without this, dash's job-control probe (open("/dev/tty")) got a
     * raw dev node and reported "can't access tty, job control off". */
    {
        extern bool pseudo_node_is_console_tty(vfs_node_t *);
        if (pseudo_node_is_console_tty(node)) {
            fd_table[fd].used   = true;
            fd_table[fd].node   = NULL;
            fd_table[fd].offset = 0;
            fd_table[fd].flags  = flags;
            return fd;
        }
    }

    /* /dev/ptmx is a CLONING device, opening it allocates a pty
     * pair and the fd refers to the fresh master end (Linux semantics). */
    {
        extern bool pseudo_node_is_ptmx(vfs_node_t *);
        extern int  pty_open_ptmx(vfs_node_t *, vfs_fd_t *, int);
        if (pseudo_node_is_ptmx(node)) {
            vfs_fd_t slot;
            int r = pty_open_ptmx(node, &slot, flags);
            if (r < 0) { fd_table[fd].used = false; return r; }
            fd_table[fd]     = slot;
            fd_inherited[fd] = false;
            return fd;
        }
    }

    fd_table[fd].used   = true;
    fd_table[fd].node   = node;
    fd_table[fd].offset = (flags & O_APPEND) ? node->size : 0;
    fd_table[fd].flags  = flags;
    return fd;
}

int vfs_close(int fd) {
    if (fd < 0 || fd >= VFS_MAX_FDS || !fd_table[fd].used) return -(int)VFS_EBADF;
    vfs_node_t *node = fd_table[fd].node;
    bool inh = fd_inherited[fd];
    fd_inherited[fd] = false;
    fd_table[fd].used = false;

    /* flocks + OFD record locks die with the fd slot, POSIX
     * record locks die with any close of the file by this process. */
    {
        extern void locks_release_fd(int fd);
        locks_release_fd(fd);
    }

    if (node && node->type == VFS_TYPE_PIPE) {
        /* Pipe refcounting lives here (not in the ops close hook) so we
         * can honour inherited-view semantics: a forked child closing an
         * inherited slot does not touch the counts, the parent's slots
         * are restored right after the child finishes. */
        pipe_buf_t *pb = (pipe_buf_t *)node->data;
        if (pb && !inh) {
            if (node->mode & 1) {
                if (pb->write_open > 0) pb->write_open--;
            } else {
                if (pb->read_open > 0) pb->read_open--;
            }
            if (pb->write_open == 0 && pb->read_open == 0) {
                /* both logical ends gone: detach + release */
                vfs_node_t *rn = (vfs_node_t *)pb->rn;
                vfs_node_t *wn = (vfs_node_t *)pb->wn;
                if (rn) rn->data = NULL;
                if (wn) wn->data = NULL;
                pipe_maybe_free(pb);
                if (rn) vfs_free_node(rn);
                if (wn) vfs_free_node(wn);
            }
        }
        /* pipe nodes are pool nodes with parent==NULL, they are freed
         * exclusively in the both-ends-gone branch above, never here */
        return 0;
    }

    /* pty ends release their reference here (same inherited-view
     * semantics as pipes).  The pty's nodes are pinned and are freed only
     * by pty_on_close when both ends hit zero. */
    if (node && (pty_node_is_master(node) || pty_node_is_slave(node))) {
        pty_on_close(node, inh);
        return 0;
    }

    if (node && node->ops && node->ops->close)
        node->ops->close(node);
    fd_table[fd].used = false;
    /* Free ephemeral nodes created by finddir (they have no parent in the
     * permanent tree and must be freed or the node pool drains quickly).
     * Never free pinned nodes, those are mount-point roots (parent==NULL
     * by design) and must survive for the lifetime of the filesystem. */
    if (node && node->parent == NULL && !node->pinned)
        vfs_free_node(node);
    return 0;
}

int vfs_read(int fd, void *buf, size_t len) {
    if (fd < 0 || fd >= VFS_MAX_FDS || !fd_table[fd].used) return -(int)VFS_EBADF;
    vfs_fd_t   *f    = &fd_table[fd];
    vfs_node_t *node = f->node;
    if (!node) return -(int)VFS_EBADF;
    if (node->type == VFS_TYPE_DIR) return -(int)VFS_EISDIR;
    if (!node->ops || !node->ops->read) return -(int)VFS_EINVAL;
    int r = node->ops->read(node, buf, len, f->offset);
    if (r > 0) f->offset += (size_t)r;
    return r;
}

int vfs_write(int fd, const void *buf, size_t len) {
    if (fd < 0 || fd >= VFS_MAX_FDS || !fd_table[fd].used) return -(int)VFS_EBADF;
    vfs_fd_t   *f    = &fd_table[fd];
    vfs_node_t *node = f->node;
    if (!node) return -(int)VFS_EBADF;
    if (node->type == VFS_TYPE_DIR) return -(int)VFS_EISDIR;
    if (!node->ops || !node->ops->write) return -(int)VFS_EINVAL;
    /* O_APPEND: every write lands at end-of-file (POSIX atomicity is
     * irrelevant in the cooperative kernel). */
    if (f->flags & O_APPEND) f->offset = node->size;
    int r = node->ops->write(node, buf, len, f->offset);
    if (r > 0) f->offset += (size_t)r;
    return r;
}

int vfs_readdir(int fd, int idx, char *name_out, uint8_t *type_out) {
    if (fd < 0 || fd >= VFS_MAX_FDS || !fd_table[fd].used) return -(int)VFS_EBADF;
    vfs_node_t *node = fd_table[fd].node;
    if (!node) return -(int)VFS_EBADF;
    if (node->type != VFS_TYPE_DIR) return -(int)VFS_ENOTDIR;
    if (node->ops && node->ops->readdir)
        return node->ops->readdir(node, idx, name_out, type_out);
    /* Fallback: iterate children array */
    if (idx < 0 || idx >= node->child_count) return -1;
    vfs_strcpy(name_out, node->children[idx]->name, VFS_NAME_MAX);
    *type_out = node->children[idx]->type;
    return 0;
}

/* Node -> stat fill (mode + ownership + mtime) */
static void stat_fill(vfs_stat_t *st, vfs_node_t *n) {
    st->st_ino  = n->inode;
    st->st_type = n->type;
    st->st_size = n->size;
    vfs_strcpy(st->st_name, n->name, VFS_NAME_MAX);
    /* Use the node's full Linux mode when it carries type bits (ramfs
     * defaults, ext2 i_mode).  Pipe nodes store 0/1 in mode (R/W end) and
     * must always derive. */
    if (n->mode && (n->mode & VFS_S_IFMT)) {
        st->st_mode = n->mode;
    } else if (n->type == VFS_TYPE_DIR) {
        st->st_mode = VFS_S_IFDIR | 0755;
    } else if (n->type == VFS_TYPE_DEV) {
        st->st_mode = VFS_S_IFCHR | 0666;
    } else if (n->type == VFS_TYPE_PIPE) {
        st->st_mode = VFS_S_IFIFO | 0600;
    } else {
        st->st_mode = VFS_S_IFREG | 0644;
    }
    st->st_mtime = n->mtime;
    st->st_uid   = n->uid;
    st->st_gid   = n->gid;
}

/* Unix permission enforcement
 * Classic owner/group/other rwx model.  The credentials come from the
 * current process via the vfs_cred_* hooks (implemented in
 * exec/process.c around g_current_proc), root (euid 0) bypasses.
 *
 * Nodes whose mode lacks type bits get the same derived defaults as
 * stat_fill so that legacy filesystems (old cpio entries, ramfs nodes
 * created before a mode was assigned) keep working unchanged.       */
static uint32_t node_eff_mode(vfs_node_t *n) {
    if (n->mode && (n->mode & VFS_S_IFMT)) return n->mode;
    if (n->type == VFS_TYPE_DIR)  return VFS_S_IFDIR | 0755;
    if (n->type == VFS_TYPE_DEV)  return VFS_S_IFCHR | 0666;
    if (n->type == VFS_TYPE_PIPE) return VFS_S_IFIFO | 0600;
    return VFS_S_IFREG | 0644;
}

bool vfs_check_perm(vfs_node_t *n, int bits) {
    if (!n) return false;
    /* Pipes are anonymous kernel objects owned by their creators, do
     * not model ownership on them (the fd rights already encode R/W). */
    if (n->type == VFS_TYPE_PIPE) return true;
    if (vfs_cred_is_root()) return true;

    uint32_t mode = node_eff_mode(n);
    uint32_t shift;
    if (n->uid == vfs_cred_euid())                    shift = 6;  /* owner */
    else if (n->gid == vfs_cred_egid() ||
             vfs_cred_in_group(n->gid))               shift = 3;  /* group */
    else                                              shift = 0;  /* other */
    return ((mode >> shift) & bits) == (uint32_t)bits;
}

bool vfs_check_exec(vfs_node_t *n) {
    if (!n) return false;
    if (n->type == VFS_TYPE_DIR)  return vfs_check_perm(n, 1);
    if (n->type == VFS_TYPE_PIPE) return false;
    if (vfs_cred_is_root()) {
        /* root needs at least one x bit (Linux semantics) */
        return (node_eff_mode(n) & 0111) != 0;
    }
    return vfs_check_perm(n, 1);
}

/* parent-directory access for create/delete/rename (need w + x) */
static bool dir_writable(vfs_node_t *dir) {
    if (!dir || dir->type != VFS_TYPE_DIR) return false;
    return vfs_check_perm(dir, 2 | 1);
}

int vfs_fstat(int fd, vfs_stat_t *st) {
    if (fd < 0 || fd >= VFS_MAX_FDS || !fd_table[fd].used) return -(int)VFS_EBADF;
    vfs_node_t *n = fd_table[fd].node;
    if (!n) return -(int)VFS_EBADF;
    stat_fill(st, n);
    return 0;
}

/* Console fd classification
 * The console is a pseudo-fd: a used fd slot with a NULL node pointer.
 * Slots 0/1/2 are reserved for it at vfs_init(), but dup()/dup2() and
 * fcntl(F_DUPFD) copy the slot verbatim, so console fds can live at any
 * number - dash dups the tty to fd >= 10 (savefd) and drives job control
 * (tcgetpgrp/tcsetattr) through that dup.  Classifying by fd number alone
 * made every interactive dash print "can't access tty, job control
 * turned off".  All other fd kinds carry a real node pointer. */
bool vfs_fd_is_console(int fd) {
    if (fd < 0 || fd >= VFS_MAX_FDS || !fd_table[fd].used) return false;
    return fd_table[fd].node == NULL;
}

/* Path-based helpers */
int vfs_stat(const char *path, vfs_stat_t *st) {
    vfs_node_t *n = vfs_resolve(path);
    if (!n) return -(int)VFS_ENOENT;
    stat_fill(st, n);
    return 0;
}

/* chmod family (real permission storage) */
static int node_chmod(vfs_node_t *n, uint32_t mode) {
    if (!n) return -(int)VFS_ENOENT;
    if (n->type == VFS_TYPE_PIPE) return -(int)VFS_EINVAL;
    /* chmod: file owner or root (Linux CAP_FOWNER simplified) */
    if (!vfs_cred_is_root() && n->uid != vfs_cred_euid())
        return -(int)VFS_EPERM;
    if (!(n->mode & VFS_S_IFMT)) {
        /* node has no type bits yet, derive them first */
        if (n->type == VFS_TYPE_DIR)      n->mode = VFS_S_IFDIR | 0755;
        else if (n->type == VFS_TYPE_DEV) n->mode = VFS_S_IFCHR | 0666;
        else                              n->mode = VFS_S_IFREG | 0644;
    }
    n->mode = (n->mode & ~07777u) | (mode & 07777u);
    if (n->ops && n->ops->setattr)
        return n->ops->setattr(n, n->mode, (uint32_t)-1, (uint32_t)-1);
    return 0;
}

int vfs_chmod(const char *path, uint32_t mode) {
    vfs_node_t *n = vfs_resolve(path);
    if (!n) return -(int)VFS_ENOENT;
    return node_chmod(n, mode);
}

int vfs_fchmod(int fd, uint32_t mode) {
    if (fd < 0 || fd >= VFS_MAX_FDS || !fd_table[fd].used) return -(int)VFS_EBADF;
    return node_chmod(fd_table[fd].node, mode);
}

/* chown family (real ownership storage) */
static int node_chown(vfs_node_t *n, uint32_t uid, uint32_t gid) {
    if (!n) return -(int)VFS_ENOENT;
    if (n->type == VFS_TYPE_PIPE) return -(int)VFS_EINVAL;
    /* chown is privileged (Linux lets owners give files to their own
     * groups, we keep it root-only, useradd/login run as root). */
    if (!vfs_cred_is_root()) return -(int)VFS_EPERM;
    if (uid != (uint32_t)-1) n->uid = uid;
    if (gid != (uint32_t)-1) n->gid = gid;
    if (n->ops && n->ops->setattr)
        return n->ops->setattr(n, 0, uid, gid);
    return 0;
}

int vfs_chown(const char *path, uint32_t uid, uint32_t gid) {
    vfs_node_t *n = vfs_resolve(path);
    if (!n) return -(int)VFS_ENOENT;
    return node_chown(n, uid, gid);
}

int vfs_fchown(int fd, uint32_t uid, uint32_t gid) {
    if (fd < 0 || fd >= VFS_MAX_FDS || !fd_table[fd].used) return -(int)VFS_EBADF;
    return node_chown(fd_table[fd].node, uid, gid);
}

int vfs_mkdir(const char *path) {
    const char *base;
    vfs_node_t *parent = resolve_parent(path, &base);
    if (!parent) return -(int)VFS_ENOENT;
    if (!parent->ops || !parent->ops->mknod) return -(int)VFS_EPERM;
    if (!dir_writable(parent)) return -(int)VFS_EACCES;
    vfs_node_t *n = parent->ops->mknod(parent, base, VFS_TYPE_DIR);
    if (!n) return -(int)vfs_errno;
    n->uid = vfs_cred_euid();
    n->gid = vfs_cred_egid();
    if (n->ops && n->ops->setattr)
        n->ops->setattr(n, n->mode, n->uid, n->gid);
    return 0;
}

int vfs_creat(const char *path) {
    const char *base;
    vfs_node_t *parent = resolve_parent(path, &base);
    if (!parent) return -(int)VFS_ENOENT;
    if (!parent->ops || !parent->ops->mknod) return -(int)VFS_EPERM;
    if (!dir_writable(parent)) return -(int)VFS_EACCES;
    vfs_node_t *n = parent->ops->mknod(parent, base, VFS_TYPE_FILE);
    if (!n) return -(int)vfs_errno;
    n->uid = vfs_cred_euid();
    n->gid = vfs_cred_egid();
    return 0;
}

int vfs_unlink(const char *path) {
    const char *base;
    vfs_node_t *parent = resolve_parent(path, &base);
    if (!parent) return -(int)VFS_ENOENT;
    if (!parent->ops || !parent->ops->unlink) return -(int)VFS_EPERM;
    if (!dir_writable(parent)) return -(int)VFS_EACCES;
    return parent->ops->unlink(parent, base);
}

/* vfs_rename
 *
 * Move/rename a node.  Both paths must be in the same in-core directory tree
 * (we don't support cross-filesystem rename).
 *
 * Steps:
 *   1. Resolve old node and its parent directory.
 *   2. Resolve new parent directory and new basename.
 *   3. If newpath already exists as a file, unlink it first.
 *   4. Re-parent the node: remove from old parent's children[], rename the
 *      node's name field, attach to new parent.
 *
 * Returns 0 on success, -errno on failure.
 */
int vfs_rename(const char *oldpath, const char *newpath) {
    if (!oldpath || !newpath) return -(int)VFS_EINVAL;

    /* Resolve old node */
    vfs_node_t *node = vfs_resolve(oldpath);
    if (!node) return -(int)VFS_ENOENT;

    /* Find old parent */
    const char *old_base;
    vfs_node_t *old_parent = resolve_parent(oldpath, &old_base);
    if (!old_parent) return -(int)VFS_ENOENT;

    /* Find new parent and new name */
    const char *new_base;
    vfs_node_t *new_parent = resolve_parent(newpath, &new_base);
    if (!new_parent) return -(int)VFS_ENOENT;

    /* rename needs write+search on both parent directories (POSIX) */
    if (!dir_writable(old_parent)) return -(int)VFS_EACCES;
    if (!dir_writable(new_parent)) return -(int)VFS_EACCES;

    /* If destination exists and is a regular file, unlink it first */
    vfs_node_t *existing = vfs_resolve(newpath);
    if (existing) {
        if (existing->type == VFS_TYPE_DIR) return -(int)VFS_EISDIR;
        if (!new_parent->ops || !new_parent->ops->unlink)
            return -(int)VFS_EPERM;
        int r = new_parent->ops->unlink(new_parent, new_base);
        if (r < 0) return r;
    }

    /* On-disk rename for disk-backed filesystems (ext2)
     * Must run before the cached node is renamed: the filesystem needs
     * node->name (the old disk name) and node->parent (the old dir) to
     * move the directory entry.  Without it the file kept its old name
     * on disk and the new name resolved to ENOENT after a remount.    */
    if (node->ops && node->ops->frename) {
        int r = node->ops->frename(node, new_parent, new_base);
        if (r < 0) return r;
    }

    /* Remove node from old parent's children array */
    for (int i = 0; i < old_parent->child_count; i++) {
        if (old_parent->children[i] == node) {
            for (int j = i; j < old_parent->child_count - 1; j++)
                old_parent->children[j] = old_parent->children[j + 1];
            old_parent->child_count--;
            break;
        }
    }

    /* Rename the node itself */
    vfs_strcpy(node->name, new_base, VFS_NAME_MAX);
    node->parent = new_parent;

    /* Attach to new parent */
    if (!vfs_attach(new_parent, node)) {
        /* Roll back: re-attach to old parent */
        vfs_strcpy(node->name, old_base, VFS_NAME_MAX);
        node->parent = old_parent;
        vfs_attach(old_parent, node);
        return -(int)VFS_ENOSPC;
    }

    return 0;
}

/* Linux poll event bits (mirror of exec/syscall_table.h values) */
#define POLLIN_LOCAL   0x0001
#define POLLOUT_LOCAL  0x0004
#define POLLERR_LOCAL  0x0008
#define POLLHUP_LOCAL  0x0010
#define POLLNVAL_LOCAL 0x0020

/* Kernel-object fd installation */
int vfs_install_fd(vfs_node_t *node, int flags) {
    if (!node) return -(int)VFS_EINVAL;
    int fd = alloc_fd();
    if (fd < 0) return -(int)VFS_ENOMEM;
    fd_table[fd].used   = true;
    fd_table[fd].node   = node;
    fd_table[fd].offset = 0;
    fd_table[fd].flags  = flags;
    fd_inherited[fd]    = false;
    return fd;
}

/* Readiness probe for poll/epoll */
extern int kobj_node_poll(vfs_node_t *node, int events);
extern bool kb_has_char(void);
extern bool serial_has_input(void);
/* evdev readiness (fs/pseudo.h classification + drivers/input) */
extern bool pseudo_node_is_evdev(vfs_node_t *n, int *idx);
extern bool evdev_has_event(int idx);

int vfs_poll_ready(int fd, int events) {
    if (fd < 0 || fd >= VFS_MAX_FDS) return 0;
    if (!fd_table[fd].used) return POLLNVAL_LOCAL;
    vfs_node_t *n = fd_table[fd].node;
    if (!n) {
        /* console pseudo-fd: readable when the keyboard/serial has a
         * byte pending, always writable                            */
        int ev = 0;
        if (events & POLLIN_LOCAL) {
            if (kb_has_char() || serial_has_input()) ev |= POLLIN_LOCAL;
        }
        if (events & POLLOUT_LOCAL) ev |= POLLOUT_LOCAL;
        return ev;
    }
    if (n->type == VFS_TYPE_PIPE) {
        pipe_buf_t *pb = (pipe_buf_t *)n->data;
        if (!pb) return 0;
        int ev = 0;
        bool wr_end = (n->mode & 1) != 0;
        if (!wr_end) {
            if (pb->len > 0) ev |= POLLIN_LOCAL;
            if (pb->write_open == 0) ev |= POLLHUP_LOCAL;
        } else {
            if (pb->len < PIPE_BUF_SIZE) ev |= POLLOUT_LOCAL;
            if (pb->read_open == 0) ev |= POLLERR_LOCAL;
        }
        return ev & events;
    }
    if (n->type == VFS_TYPE_KOBJ)
        return kobj_node_poll(n, events);
    /* pty ends report real readiness (buffer state + hangup) */
    if (pty_node_is_master(n) || pty_node_is_slave(n))
        return pty_poll_node(n, events);
    if (n->type == VFS_TYPE_DEV) {
        /* /dev/input/eventN is readable when an event batch
         * is queued - real readiness for compositors (libinput).     */
        int ev_idx = -1;
        if (pseudo_node_is_evdev(n, &ev_idx)) {
            int ev = 0;
            if (events & POLLIN_LOCAL) {
                if (evdev_has_event(ev_idx)) ev |= POLLIN_LOCAL;
            }
            if (events & POLLOUT_LOCAL) ev |= POLLOUT_LOCAL;
            return ev;
        }
    }
    /* files/dirs/devices: always ready */
    {
        int ev = 0;
        if (events & POLLIN_LOCAL)  ev |= POLLIN_LOCAL;
        if (events & POLLOUT_LOCAL) ev |= POLLOUT_LOCAL;
        return ev;
    }
}

/* fd snapshot / install (SCM_RIGHTS fd passing) */
int vfs_fd_snapshot(int fd, vfs_fd_t *out) {
    if (fd < 0 || fd >= VFS_MAX_FDS || !fd_table[fd].used || !out)
        return -(int)VFS_EBADF;
    *out = fd_table[fd];
    return 0;
}

int vfs_install_snapshot(const vfs_fd_t *snap) {
    if (!snap || !snap->node) return -(int)VFS_EBADF;
    int fd = alloc_fd();
    if (fd < 0) return -(int)VFS_ENOMEM;
    fd_table[fd] = *snap;
    fd_table[fd].used = true;
    fd_inherited[fd]  = false;
    return fd;
}

/* Per-process fd table API
 *
 * The live fd_table is a process-local snapshot.  On fork/exec/exit the kernel
 * saves or restores it via these helpers so each process has its own fd space.
 */

void vfs_fd_save(vfs_fd_t dst[VFS_MAX_FDS]) {
    for (int i = 0; i < VFS_MAX_FDS; i++) dst[i] = fd_table[i];
}

/* peek the live fd table (not a process's snapshot), used by
 * /proc/self/fd/<n> so freshly-opened fds are visible immediately. */
vfs_node_t *vfs_fd_peek_node(int fd) {
    if (fd < 0 || fd >= VFS_MAX_FDS || !fd_table[fd].used) return NULL;
    return fd_table[fd].node;   /* NULL for console pseudo-fds */
}

void vfs_fd_restore(const vfs_fd_t src[VFS_MAX_FDS]) {
    for (int i = 0; i < VFS_MAX_FDS; i++) {
        fd_table[i] = src[i];
        fd_inherited[i] = false;   /* owner slots again */
    }
}

/* Clone src -> dst for fork().
 * Child inherits all open fds (same node pointer, same offset).
 * The two copies are independent, closing one does not affect the other.
 * Pipe ref-counts are not incremented here, the caller must bump them if
 * needed.  For our cooperative model (child runs then parent resumes) this
 * is safe because the parent's copy is parked in saved_fd_table and not
 * touched while the child runs.
 */
void vfs_fd_clone(vfs_fd_t dst[VFS_MAX_FDS], const vfs_fd_t src[VFS_MAX_FDS]) {
    for (int i = 0; i < VFS_MAX_FDS; i++) {
        dst[i] = src[i];
        /* the child's slots are borrowed views: closing them must not
         * disturb pipe refcounts (see fd_inherited notes above) */
        fd_inherited[i] = src[i].used;
    }
}

/* Close all fds that have O_CLOEXEC set.  Called by execve. */
void vfs_fd_close_cloexec(void) {
    for (int i = 3; i < VFS_MAX_FDS; i++) {
        if (fd_table[i].used && (fd_table[i].flags & O_CLOEXEC))
            vfs_close(i);
    }
}

/* Close every fd - including 0/1/2 if they were redirected to real file
 * objects (dup2'd pipes etc.).  Slots whose node is NULL (the console
 * pseudo-fds) are simply marked unused, the console needs no refcount.
 * Called on process teardown.  (previously started at fd 3, so
 * a dup2'd pipe on stdout kept its buffer pinned forever.) */
void vfs_fd_close_all(void) {
    for (int i = 0; i < VFS_MAX_FDS; i++) {
        if (fd_table[i].used) vfs_close(i);
    }
}

/* pipe reference bumps for fork
 * Pipe refcounts count logical ends (read_open/write_open start at 1).
 * In the old cooperative model a forked child's inherited slots were
 * "borrowed views" (fd_inherited) and never touched the counts.  With
 * the BORE enqueue model parent and child truly run concurrently and
 * Both hold the pipe end, so every pipe end inherited through a fork
 * must take its own reference (each user of the shared end owns one).
 * Threads (CLONE_FILES) share one table and must not bump.                     */
void vfs_pipe_refs_bump(const vfs_fd_t table[VFS_MAX_FDS]) {
    for (int i = 0; i < VFS_MAX_FDS; i++) {
        vfs_node_t *n = table[i].node;
        if (!table[i].used || !n || n->type != VFS_TYPE_PIPE) continue;
        pipe_buf_t *pb = (pipe_buf_t *)n->data;
        if (!pb) continue;
        if (n->mode & 1) {
            if (pb->write_open < 0xFFFF) pb->write_open++;
        } else {
            if (pb->read_open < 0xFFFF) pb->read_open++;
        }
    }
    /* pty ends take their own fork reference exactly like pipes */
    {
        extern void pty_refs_bump(const vfs_fd_t table[VFS_MAX_FDS]);
        pty_refs_bump(table);
    }
}

/* Return the open flags stored in the fd slot, or -EBADF. */
int vfs_fd_getflags(int fd) {
    if (fd < 0 || fd >= VFS_MAX_FDS || !fd_table[fd].used)
        return -(int)VFS_EBADF;
    return fd_table[fd].flags;
}

/* Refcounted fd tables
 * Static pool: the kernel has no heap allocator.  Sized for one table per
 * task plus clone/fork transients (PROC_MAX lives in exec/process.h, which
 * cannot be included here, keep the two in sync).                          */
#define VFS_FDT_POOL_MAX 32
static vfs_fdt_t fdt_pool[VFS_FDT_POOL_MAX];
static bool      fdt_pool_used[VFS_FDT_POOL_MAX];

vfs_fdt_t *vfs_fdt_new(void) {
    for (int i = 0; i < VFS_FDT_POOL_MAX; i++) {
        if (!fdt_pool_used[i]) {
            fdt_pool_used[i] = true;
            for (int f = 0; f < VFS_MAX_FDS; f++)
                fdt_pool[i].t[f].used = false;
            fdt_pool[i].refcnt = 1;
            return &fdt_pool[i];
        }
    }
    return NULL;
}

vfs_fdt_t *vfs_fdt_clone(vfs_fdt_t *src) {
    vfs_fdt_t *dst = vfs_fdt_new();
    if (!dst || !src) return dst;
    for (int f = 0; f < VFS_MAX_FDS; f++) dst->t[f] = src->t[f];
    return dst;
}

vfs_fdt_t *vfs_fdt_share(vfs_fdt_t *t) {
    if (t) t->refcnt++;
    return t;
}

void vfs_fdt_put(vfs_fdt_t *t) {
    if (!t) return;
    if (--t->refcnt > 0) return;
    for (int i = 0; i < VFS_FDT_POOL_MAX; i++) {
        if (&fdt_pool[i] == t) { fdt_pool_used[i] = false; return; }
    }
}

/* Replace the open flags stored in the fd slot (F_SETFL). */
int vfs_fd_setflags(int fd, int flags) {
    if (fd < 0 || fd >= VFS_MAX_FDS || !fd_table[fd].used)
        return -(int)VFS_EBADF;
    fd_table[fd].flags = flags;
    return 0;
}

/* Return the vnode backing an open fd (NULL when invalid). */
vfs_node_t *vfs_node_from_fd(int fd) {
    if (fd < 0 || fd >= VFS_MAX_FDS || !fd_table[fd].used)
        return NULL;
    return fd_table[fd].node;
}

/* vfs_node_path: reconstruct an absolute path for a node
 * Walks the parent chain up to the mount root, then prefixes the mount
 * path from the mount table.  Returns 0 on success, -1 when the node's
 * path cannot be derived (detached/ephemeral node, unknown mount). */
int vfs_node_path(vfs_node_t *node, char *out, size_t outsz) {
    if (!node || !out || outsz < 2) return -1;

    /* Mount root itself? */
    if (!node->parent) {
        for (int i = 0; i < mount_count; i++) {
            if (mount_table[i].used && mount_table[i].root == node) {
                vfs_strcpy(out, mount_table[i].path, outsz);
                return 0;
            }
        }
        return -1;
    }

    /* Walk up, stacking name components reversed into tmp:
     * tmp ends up as "eman_last/eman_prev/.../" (each name char-reversed). */
    char tmp[VFS_PATH_MAX];
    size_t ti = 0;
    vfs_node_t *cur = node;
    while (cur && cur->parent) {
        size_t len = vfs_strlen(cur->name);
        if (ti + len + 1 >= sizeof(tmp)) return -1;
        for (size_t j = len; j > 0; j--) tmp[ti++] = cur->name[j - 1];
        tmp[ti++] = '/';
        cur = cur->parent;
    }
    if (!cur) return -1;

    /* cur is the mount root, find its mount path */
    char base[VFS_PATH_MAX];
    bool found = false;
    for (int i = 0; i < mount_count; i++) {
        if (mount_table[i].used && mount_table[i].root == cur) {
            vfs_strcpy(base, mount_table[i].path, VFS_PATH_MAX);
            found = true;
            break;
        }
    }
    if (!found) return -1;

    size_t blen = vfs_strlen(base);
    if (blen > 1 && base[blen - 1] == '/') base[--blen] = '\0';
    bool is_root = (blen == 1 && base[0] == '/');

    /* Do not strip tmp's trailing slash: reading tmp backwards yields
     * "/comp1/comp2...", i.e. every component already comes out with its
     * leading '/'.  For a root mount the result is therefore "/tmp" etc.;
     * for a non-root mount we prefix the mount path (no double slash
     * because base has been de-slashed above). */
    size_t oi = 0;
    if (!is_root && blen) {
        for (size_t j = 0; j < blen && oi < outsz - 1; j++) out[oi++] = base[j];
    }
    for (size_t j = ti; j > 0 && oi < outsz - 1; j--) out[oi++] = tmp[j - 1];
    if (oi == 0) out[oi++] = '/';
    out[oi] = '\0';
    return 0;
}

bool vfs_write_file(const char *path, const void *buf, size_t len) {
    int fd = vfs_open(path, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) return false;
    int r = vfs_write(fd, buf, len);
    vfs_close(fd);
    return r == (int)len;
}

/* Init */
void vfs_init(void) {
    for (int i = 0; i < VFS_MAX_NODES; i++) node_used[i] = false;
    for (int i = 0; i < VFS_MAX_FDS;   i++) fd_table[i].used = false;
    for (int i = 0; i < VFS_MAX_MOUNTS;i++) mount_table[i].used = false;
    mount_count = 0;
    next_inode = 1;
    vfs_root = NULL;

    /* Reserve stdin(0), stdout(1), stderr(2) */
    fd_table[0].used = true;
    fd_table[1].used = true;
    fd_table[2].used = true;
}

/* Debug print */
/* Node-pool accounting for statfs (f_files/f_ffree). */
void vfs_node_stats(int *used_out, int *total_out) {
    if (used_out) {
        int used = 0;
        for (int i = 0; i < VFS_MAX_NODES; i++) if (node_used[i]) used++;
        *used_out = used;
    }
    if (total_out) *total_out = VFS_MAX_NODES;
}

void vfs_print_tree(vfs_node_t *node, int depth) {
    if (!node) return;
    for (int i = 0; i < depth * 2; i++) display_putchar(' ');
    display_puts(node->name);
    if (node->type == VFS_TYPE_DIR) {
        display_putchar('/');
    } else {
        display_printf("  (%u B)", node->size);
    }
    display_putchar('\n');
    for (int i = 0; i < node->child_count; i++)
        vfs_print_tree(node->children[i], depth + 1);
}

/* vfs_lseek */
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

int vfs_lseek(int fd, size_t offset, int whence) {
    if (fd < 0 || fd >= VFS_MAX_FDS || !fd_table[fd].used) return -(int)VFS_EBADF;
    vfs_fd_t *f = &fd_table[fd];
    if (!f->node) return -(int)VFS_EBADF;
    size_t new_off;
    switch (whence) {
        case SEEK_SET: new_off = offset; break;
        case SEEK_CUR: new_off = f->offset + offset; break;
        case SEEK_END: new_off = f->node->size + offset; break;
        default: return -(int)VFS_EINVAL;
    }
    f->offset = new_off;
    return (int)new_off;
}

/* vfs_dup / vfs_dup2 */
int vfs_dup(int fd) {
    if (fd < 0 || fd >= VFS_MAX_FDS || !fd_table[fd].used) return -(int)VFS_EBADF;
    int newfd = alloc_fd();
    if (newfd < 0) return -(int)VFS_ENOMEM;
    fd_table[newfd] = fd_table[fd];
    fd_inherited[newfd] = fd_inherited[fd];
    /* dup'd pty ends take their own reference (mirror of the
     * pipe dup bump below, every slot owns one close-release) */
    {
        vfs_node_t *n = fd_table[newfd].node;
        if (n && (pty_node_is_master(n) || pty_node_is_slave(n)))
            pty_ref_bump_node(n);
    }
    return newfd;
}

int vfs_dup2(int fd, int newfd) {
    if (fd < 0 || fd >= VFS_MAX_FDS || !fd_table[fd].used) return -(int)VFS_EBADF;
    if (newfd < 0 || newfd >= VFS_MAX_FDS) return -(int)VFS_EBADF;
    if (newfd == fd) return newfd;
    if (fd_table[newfd].used) vfs_close(newfd);
    fd_table[newfd] = fd_table[fd];
    fd_inherited[newfd] = fd_inherited[fd];
    /* a dup'd pipe end takes its own logical reference, matching
     * the per-slot close decrements (Linux dup bumps the file ref).     */
    {
        vfs_node_t *n = fd_table[newfd].node;
        if (n && n->type == VFS_TYPE_PIPE) {
            pipe_buf_t *pb = (pipe_buf_t *)n->data;
            if (pb) {
                if (n->mode & 1) {
                    if (pb->write_open < 0xFFFF) pb->write_open++;
                } else {
                    if (pb->read_open < 0xFFFF) pb->read_open++;
                }
            }
        }
        /* same rule for pty ends */
        if (n && (pty_node_is_master(n) || pty_node_is_slave(n)))
            pty_ref_bump_node(n);
    }
    return newfd;
}

/*
 *  Anonymous pipe  (vfs_pipe)
 *
 *  Design
 *
 *  Each pipe() call gets one pipe_buf_t from a small static pool.
 *  Two vfs_node_t entries (read-end, write-end) share the same data ptr.
 *  The ops vtable checks the node type tag to distinguish R vs W.
 *
 *  Close semantics
 *
 *  pipe_close() decrements the appropriate ref-count.  When write_open
 *  reaches 0, subsequent read()s drain the buffer and then return 0 (EOF).
 *  When read_open reaches 0 and write_open > 0, writes return -EPIPE.
 *  When both reach 0, the pipe_buf_t slot is returned to the pool.
 */

#define PIPE_POOL_SIZE  4
static pipe_buf_t pipe_pool[PIPE_POOL_SIZE];
static bool       pipe_pool_used[PIPE_POOL_SIZE];

static pipe_buf_t *pipe_alloc(void) {
    for (int i = 0; i < PIPE_POOL_SIZE; i++) {
        if (!pipe_pool_used[i]) {
            pipe_pool_used[i] = true;
            /* zero-initialise */
            pipe_buf_t *p = &pipe_pool[i];
            p->rpos = p->wpos = p->len = 0;
            p->write_open = 0;
            p->read_open  = 0;
            return p;
        }
    }
    return (void *)0;
}

static void pipe_maybe_free(pipe_buf_t *pb) {
    if (pb->write_open == 0 && pb->read_open == 0) {
        for (int i = 0; i < PIPE_POOL_SIZE; i++) {
            if (&pipe_pool[i] == pb) {
                pipe_pool_used[i] = false;
                return;
            }
        }
    }
}

/* pipe ops */

/* open: nothing to do - fds are pre-installed */
static int pipe_open(vfs_node_t *node, int flags) {
    (void)node; (void)flags; return 0;
}

/* Pipe refcount handling moved to vfs_close() (see fd_inherited notes).
 * The ops vtable still needs a close hook, it is a no-op now. */
static int pipe_close(vfs_node_t *node) {
    (void)node;
    return 0;
}

static int pipe_read(vfs_node_t *node, void *buf, size_t len, size_t off) {
    (void)off;
    pipe_buf_t *pb = (pipe_buf_t *)node->data;
    if (!pb) return 0;  /* both ends gone - EOF */

    /* park-poll instead of spinning.  The BORE
     * scheduler only preempts user mode - a kernel spin loop here would
     * starve the writing task forever (observed: systest exec-argv3
     * hung when the parent reached read() before the forked child ran).
     * sched_kernel_park(10) yields the CPU and re-checks every 10 ms,
     * the writer (child) runs as soon as we step off the CPU.          */
    while (pb->len == 0) {
        if (pb->write_open == 0) return 0;   /* EOF */
        sched_kernel_park(10);
    }

    size_t to_read = len;
    if (to_read > pb->len) to_read = pb->len;

    char *dst = (char *)buf;
    for (size_t i = 0; i < to_read; i++) {
        dst[i] = pb->buf[pb->rpos];
        pb->rpos = (pb->rpos + 1) % PIPE_BUF_SIZE;
    }
    pb->len -= to_read;
    return (int)to_read;
}

static int pipe_write(vfs_node_t *node, const void *buf, size_t len, size_t off) {
    (void)off;
    pipe_buf_t *pb = (pipe_buf_t *)node->data;
    if (!pb) return -(int)VFS_EIO;

    /* Note: in the cooperative fork model the child inherits a
     * CLONE of the fd table without bumping the pipe refcounts, so a
     * child closing its inherited read-end clone can transiently drive
     * read_open to 0 while the parent still holds the read end.  The old
     * hard EPIPE check here made such writes fail silently and lose data
     * (echo | cat through a dup2'd stdout).  Writes now land in the ring
     * regardless, EIO is returned only when the ring is full and nobody
     * is reading (a genuinely broken pipe). */
    const char *src = (const char *)buf;
    size_t written = 0;
    while (written < len) {
        if (pb->read_open == 0 && pb->len >= PIPE_BUF_SIZE)
            return -(int)VFS_EIO;
        if (pb->len < PIPE_BUF_SIZE) {
            pb->buf[pb->wpos] = src[written];
            pb->wpos = (pb->wpos + 1) % PIPE_BUF_SIZE;
            pb->len++;
            written++;
        } else {
            /* full ring - park-poll (see pipe_read) instead
             * of spinning, so the reader can drain.                    */
            sched_kernel_park(10);
        }
    }
    return (int)written;
}

static int pipe_readdir(vfs_node_t *n, int idx, char *name, uint8_t *type) {
    (void)n; (void)idx; (void)name; (void)type; return -1;
}
static vfs_node_t *pipe_finddir(vfs_node_t *n, const char *name) {
    (void)n; (void)name; return (void *)0;
}
static vfs_node_t *pipe_mknod(vfs_node_t *n, const char *name, uint8_t t) {
    (void)n; (void)name; (void)t; return (void *)0;
}
static int pipe_unlink(vfs_node_t *n, const char *name) {
    (void)n; (void)name; return -(int)VFS_EPERM;
}

static const vfs_ops_t pipe_ops = {
    .open    = pipe_open,
    .close   = pipe_close,
    .read    = pipe_read,
    .write   = pipe_write,
    .readdir = pipe_readdir,
    .finddir = pipe_finddir,
    .mknod   = pipe_mknod,
    .unlink  = pipe_unlink,
};

/* public vfs_pipe() */
int vfs_pipe(int fds[2]) {
    pipe_buf_t *pb = pipe_alloc();
    if (!pb) return -(int)VFS_ENOMEM;

    /* create read-end node (mode=0) */
    vfs_node_t *rnode = vfs_create_node("[pipe:r]", VFS_TYPE_PIPE, &pipe_ops, pb);
    if (!rnode) { pipe_pool_used[pb - pipe_pool] = false; return -(int)VFS_ENOMEM; }
    rnode->mode = 0;   /* read end */

    /* create write-end node (mode=1) */
    vfs_node_t *wnode = vfs_create_node("[pipe:w]", VFS_TYPE_PIPE, &pipe_ops, pb);
    if (!wnode) {
        vfs_free_node(rnode);
        pipe_pool_used[pb - pipe_pool] = false;
        return -(int)VFS_ENOMEM;
    }
    wnode->mode = 1;   /* write end */

    /* allocate two fds, the read-end slot must be
     * marked used before the second alloc_fd(), otherwise both ends
     * land on the same fd (rfd == wfd == 3) and the read end leaks. */
    int rfd = alloc_fd();
    if (rfd < 0) goto fail_rfd;
    fd_table[rfd].used = true;
    int wfd = alloc_fd();
    if (wfd < 0) { fd_table[rfd].used = false; goto fail_rfd; }

    fd_table[rfd].used   = true;
    fd_table[rfd].node   = rnode;
    fd_table[rfd].offset = 0;
    fd_table[rfd].flags  = O_RDONLY;

    fd_table[wfd].used   = true;
    fd_table[wfd].node   = wnode;
    fd_table[wfd].offset = 0;
    fd_table[wfd].flags  = O_WRONLY;

    pb->read_open  = 1;
    pb->write_open = 1;

    fds[0] = rfd;
    fds[1] = wfd;
#if PIPE_DEBUG
    display_printf("[pipe] created rfd=%d wfd=%d\n", rfd, wfd);
#endif
    return 0;

fail_wfd:
    /* rfd was allocated but wfd failed, release rfd slot */
    fd_table[rfd].used = false;
fail_rfd:
    vfs_free_node(rnode);
    vfs_free_node(wnode);
    pipe_pool_used[pb - pipe_pool] = false;
    return -(int)VFS_ENOMEM;
}