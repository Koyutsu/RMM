#include "exec/syscall_table.h"
#include "exec/process.h"
#include "exec/sched.h"
#include "console/display.h"
#include "drivers/ps2/keyboard.h"
#include "drivers/serial/serial.h"
#include "drivers/timer/pit.h"
#include "drivers/pci/io.h"
#include "fs/vfs.h"
#include "fs/kobj.h"
#include "fs/pseudo.h"
#include "fs/pty.h"
#include "fs/locks.h"
#include "exec/timers.h"
#include "fs/pagecache.h"
#include "drivers/pci/pci.h"
#include "cpu/gdt.h"
#include "drivers/memory/vmm.h"
#include "drivers/graphics/fbdev.h"
#include "drivers/graphics/modeset.h"
#include "drivers/gpu/drm.h"
#include "drivers/input/evdev.h"
#include <stdint.h>
#include <stddef.h>

/*
 * Set to 1 to re-enable per-syscall tracing on the console.
 * Never enable in production builds: it prints on every syscall.
 */
#define SYSCALL_TRACE 0

/* Temporary fork/exec trail (serial console) for the boot debug session. */
#define FORK_DEBUG 0
#define PIPE_DEBUG 0

/* String helpers */
static size_t kstrlen(const char *s) {
    size_t n = 0; while (s[n]) n++; return n;
}
static void kmemcpy(void *d, const void *s, size_t n) {
    uint8_t *dd = d; const uint8_t *ss = s;
    while (n--) *dd++ = *ss++;
}
static void kmemset(void *d, int v, size_t n) {
    uint8_t *dd = d; while (n--) *dd++ = (uint8_t)v;
}

/* Current working directory */

const char *syscall_getcwd_ptr(void) { return g_current_proc ? g_current_proc->cwd : "/"; }

/* Path helpers */
static void path_join(char *out, size_t outsz,
                      const char *base, const char *rel) {
    if (rel[0] == '/') {
        size_t i = 0;
        while (rel[i] && i < outsz-1) { out[i] = rel[i]; i++; }
        out[i] = '\0'; return;
    }
    size_t b = 0;
    while (base[b] && b < outsz-1) { out[b] = base[b]; b++; }
    if (b > 1 && out[b-1] != '/') out[b++] = '/';
    size_t r = 0;
    while (rel[r] && b < outsz-1) out[b++] = rel[r++];
    out[b] = '\0';
}

static void path_normalize(char *path) {
    char *stack[64]; int top = 0;
    char tmp[VFS_PATH_MAX]; size_t ti = 0;
    if (path[0] != '/') return;
    const char *p = path;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        const char *start = p;
        while (*p && *p != '/') p++;
        size_t len = (size_t)(p - start);
        if (len == 1 && start[0] == '.') continue;
        if (len == 2 && start[0] == '.' && start[1] == '.') { if (top>0) top--; continue; }
        if (top < 64) stack[top++] = (char *)start;
    }
    if (top == 0) { path[0]='/'; path[1]='\0'; return; }
    for (int i = 0; i < top && ti < VFS_PATH_MAX-2; i++) {
        tmp[ti++] = '/';
        const char *c = stack[i];
        while (*c && *c != '/' && ti < VFS_PATH_MAX-1) tmp[ti++] = *c++;
    }
    tmp[ti] = '\0';
    size_t j = 0;
    while (tmp[j] && j < VFS_PATH_MAX-1) { path[j] = tmp[j]; j++; }
    path[j] = '\0';
}

/* Resolve path relative to cwd, writes result to buf[VFS_PATH_MAX]. */
static void resolve_path(char *buf, const char *path) {
    path_join(buf, VFS_PATH_MAX, g_current_proc ? g_current_proc->cwd : "/", path);
    path_normalize(buf);
}

/* Wall-clock (RTC-based, UTC) */
/* The CMOS RTC is read exactly once, afterwards wall time advances with
 * the PIT so calls are cheap and CMOS update-in-progress glitches can
 * never be observed.  Monotonic time stays pit_ticks()-based. */
extern uint64_t pit_ticks(void);
extern int64_t  rtc_epoch_secs(void);

static uint64_t wall_now_secs(void) {
    static int64_t  base_epoch = -1;
    static uint64_t base_ticks;
    if (base_epoch < 0) {
        base_epoch = rtc_epoch_secs();
        base_ticks = pit_ticks();
    }
    uint64_t now = pit_ticks();
    uint64_t delta = (now >= base_ticks) ? (now - base_ticks) / 1000 : 0;
    return (uint64_t)base_epoch + delta;
}

/* Fill Linux stat (144-byte x86-64 layout) from vfs_stat */
static void fill_linux_stat(linux_stat_t *ls, const vfs_stat_t *vs) {
    kmemset(ls, 0, sizeof(*ls));
    ls->st_ino   = vs->st_ino;
    ls->st_size  = vs->st_size;
    ls->st_nlink = 1;
    ls->st_dev   = 0;
    ls->st_rdev  = 0;
    ls->st_uid   = 0;
    ls->st_gid   = 0;
    ls->st_blksize = 4096;
    ls->st_blocks  = (vs->st_size + 511) / 512;
    /* Real mode bits when the fs provides them (ext2 i_mode, cpio mode);
     * otherwise derive from the node type. */
    if (vs->st_mode & S_IFMT)
        ls->st_mode = vs->st_mode;
    else if (vs->st_type == VFS_TYPE_DIR)
        ls->st_mode = S_IFDIR | 0755;
    else if (vs->st_type == VFS_TYPE_DEV)
        ls->st_mode = S_IFCHR | 0666;
    else if (vs->st_type == VFS_TYPE_PIPE)
        ls->st_mode = S_IFIFO | 0600;
    else
        ls->st_mode = S_IFREG | 0644;
    /* mtime from the filesystem (cpio/ext2) when known, atime is "now".
     * Fallback for both: RTC wall clock. */
    uint64_t now = wall_now_secs();
    uint64_t mt  = vs->st_mtime ? vs->st_mtime : now;
    ls->st_atim_sec = (int64_t)now;
    ls->st_mtim_sec = ls->st_ctim_sec = (int64_t)mt;
    ls->st_atim_nsec = 0;
    ls->st_mtim_nsec = ls->st_ctim_nsec = 0;
}

/* getdents64 */
static int64_t impl_getdents64(int fd, void *buf, size_t count) {
    uint8_t *out   = (uint8_t *)buf;
    size_t  filled = 0;
    char    name[VFS_NAME_MAX];
    uint8_t type;

    /* The directory index persists in the fd's file position: each
     * getdents64() call resumes where the previous one stopped, exactly
     * like Linux's readdir cursor.  Without this, every call restarts at
     * entry 0 and `ls` loops on the same dirent forever.               */
    int idx = (int)vfs_lseek(fd, 0, 1 /* SEEK_CUR */);

    while (vfs_readdir(fd, idx, name, &type) == 0) {
        size_t namelen = kstrlen(name);
        /* reclen = fixed header (19 bytes) + namelen + NUL, aligned to 8 */
        size_t reclen  = (19 + namelen + 1 + 7) & ~7u;
        if (filled + reclen > count) break;

        linux_dirent64_t *d = (linux_dirent64_t *)(out + filled);
        d->d_ino    = (uint64_t)(idx + 1);
        d->d_off    = (int64_t)(idx + 1);
        d->d_reclen = (uint16_t)reclen;
        d->d_type   = (type == VFS_TYPE_DIR) ? DT_DIR :
                      (type == VFS_TYPE_DEV) ? DT_CHR : DT_REG;
        kmemcpy(d->d_name, name, namelen + 1);

        filled += reclen;
        idx++;
    }

    /* Persist the cursor for the next call. */
    vfs_lseek(fd, (size_t)idx, 0 /* SEEK_SET */);
    return (int64_t)filled;
}

/* mmap (anonymous only) */
/*
 * Minimal mmap.  Only MAP_ANONYMOUS|MAP_PRIVATE is supported.
 * File-backed mmap requires a page-cache which does not exist yet.
 *
 * VA allocation uses g_current_proc->mmap_bump - a per-process bump pointer
 * that is reset to PROC_MMAP_BASE on execve and saved/restored across fork,
 * so processes never trample each other's anonymous mappings.
 */
/* pick a genuinely free VA region instead of blindly bumping.
 * Threads of one address space each carry a copy of mmap_bump (no shared
 * kernel state) - the scan makes a stale copy harmless and keeps
 * concurrent mmaps from colliding.  Syscalls are atomic on this kernel
 * (preemption happens only from user mode), so check-then-map is safe. */
static uint64_t mmap_pick_region(uint64_t pages) {
    uint64_t want = pages * PAGE_SIZE;
    uint64_t base = g_current_proc->mmap_bump;
    for (;;) {
        bool free_region = true;
        for (uint64_t v = base; v < base + want; v += PAGE_SIZE) {
            if (vmm_virt_to_phys(v)) { free_region = false; break; }
        }
        if (free_region && base + want <= PROC_MMAP_LIMIT) return base;
        base += PAGE_SIZE;
        if (base + want > PROC_MMAP_LIMIT) return 0;   /* exhausted */
    }
}

static int64_t impl_mmap(uint64_t addr, uint64_t len, int prot,
                          int flags, int fd, uint64_t off) {
    (void)prot;
    if (len == 0) return -(int64_t)EINVAL;

    /* MAP_SHARED on a memfd: map the object's actual pages so writes
     * go straight through to the "file" - this is what wl_shm pools and
     * every other zero-copy shared-memory IPC path need.             */
    if (fd != -1 && !(flags & MAP_ANONYMOUS) && (flags & MAP_SHARED)) {
        vfs_node_t *fnode = vfs_node_from_fd(fd);
        if (!fnode) return -(int64_t)EBADF;

        /* /dev/dri/card0 + renderD128 - map a GEM dumb buffer
         * (offset cookie from DRM_IOCTL_MODE_MAP_DUMB) into user space.
         * Zero-copy: the pages are the buffer's real backing store. */
        {
            int drm_card = -1;
            if (fnode->type == VFS_TYPE_DEV && pseudo_node_is_drm(fnode, &drm_card)) {
                uint64_t pages = (len + PAGE_SIZE - 1) / PAGE_SIZE;
                uint64_t base;
                if (addr && (flags & MAP_FIXED)) {
                    base = addr & ~(uint64_t)(PAGE_SIZE - 1);
                } else {
                    if (!g_current_proc) return -(int64_t)ENOMEM;
                    base = mmap_pick_region(pages);
                    if (!base) return -(int64_t)ENOMEM;
                    g_current_proc->mmap_bump = base + pages * PAGE_SIZE;
                }
                int r = drm_mmap_shared(drm_card, off, base, (size_t)pages);
                if (r < 0) return r;
                return (int64_t)(uintptr_t)base;
            }
        }

        /* /dev/fb0 - map the primary framebuffer's actual memory
         * (video RAM / driver backing) so user writes hit the pixels.
         * Linux-compatible /dev/fb0 mmap semantics. */
        if (fnode->type == VFS_TYPE_DEV && pseudo_node_is_fb0(fnode)) {
            uint64_t pages = (len + PAGE_SIZE - 1) / PAGE_SIZE;
            uint64_t base;
            if (addr && (flags & MAP_FIXED)) {
                base = addr & ~(uint64_t)(PAGE_SIZE - 1);
            } else {
                if (!g_current_proc) return -(int64_t)ENOMEM;
                base = mmap_pick_region(pages);
                if (!base) return -(int64_t)ENOMEM;
                g_current_proc->mmap_bump = base + pages * PAGE_SIZE;
            }
            int r = fbdev_mmap_shared(base, (size_t)pages, off);
            if (r < 0) return r;
            return (int64_t)(uintptr_t)base;
        }

        if (kobj_node_kind(fnode) == KOBJ_MEMFD) {
            uint64_t pages = (len + PAGE_SIZE - 1) / PAGE_SIZE;
            uint64_t base;
            if (addr && (flags & MAP_FIXED)) {
                base = addr & ~(uint64_t)(PAGE_SIZE - 1);
            } else {
                if (!g_current_proc) return -(int64_t)ENOMEM;
                base = mmap_pick_region(pages);
                if (!base) return -(int64_t)ENOMEM;
                g_current_proc->mmap_bump = base + pages * PAGE_SIZE;
            }
            int r = kobj_memfd_mmap_shared(fnode, base, (size_t)pages, off);
            if (r < 0) return r;
            return (int64_t)(uintptr_t)base;
        }
        /* non-memfd MAP_SHARED falls through to the eager private copy */
    }

    vfs_node_t *fnode = NULL;
    if (fd != -1 && !(flags & MAP_ANONYMOUS)) {
        fnode = vfs_node_from_fd(fd);
        if (!fnode) return -(int64_t)EBADF;
        if (fnode->type != VFS_TYPE_FILE) return -(int64_t)ENODEV;
    } else if (fd != -1 && (flags & MAP_ANONYMOUS)) {
        /* Linux rejects anonymous mappings with a live fd - we tolerate */
    }

    uint64_t pages = (len + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t base;

    if (addr && (flags & MAP_FIXED)) {
        base = addr & ~(uint64_t)(PAGE_SIZE - 1);
    } else {
        if (!g_current_proc) return -(int64_t)ENOMEM;
        base = mmap_pick_region(pages);
        if (!base) return -(int64_t)ENOMEM;
        g_current_proc->mmap_bump = base + pages * PAGE_SIZE;
    }

    /* READ-ONLY file mappings go through the page cache
     * Every process that maps the same file region read-only shares
     * The same physical pages (mapped without PTE_WRITABLE).  This is
     * what makes dynamic linking actually save memory: one physical
     * copy of ld-musl/libc text across every dynamic process, instead
     * of one eager copy per process.  Writable mappings keep the
     * eager private copy below. */
    if (fnode && !(prot & PROT_WRITE) && !(flags & MAP_SHARED)) {
        for (uint64_t i = 0; i < pages; i++) {
            uint64_t page_idx = off / PAGE_SIZE + i;
            uint64_t phys = pagecache_get_or_load(fnode, (uint32_t)page_idx);
            if (!phys) return -(int64_t)ENOMEM;
            if (!vmm_map(base + i * PAGE_SIZE, phys, PTE_PRESENT | PTE_USER))
                return -(int64_t)ENOMEM;
        }
        return (int64_t)(uintptr_t)base;
    }

    for (uint64_t i = 0; i < pages; i++) {
        uint64_t phys = pmm_alloc();
        if (!phys) return -(int64_t)ENOMEM;
        /* zero the page via identity map */
        uint8_t *p = (uint8_t *)(uintptr_t)phys;
        for (size_t b = 0; b < PAGE_SIZE; b++) p[b] = 0;
        /* file content (eager read, no page cache yet) */
        if (fnode) {
            uint64_t foff = off + i * PAGE_SIZE;
            if (foff < fnode->size && fnode->ops && fnode->ops->read) {
                size_t chunk = PAGE_SIZE;
                if (foff + chunk > fnode->size)
                    chunk = (size_t)(fnode->size - foff);
                fnode->ops->read(fnode, p, chunk, (size_t)foff);
            }
        }
        if (!vmm_map(base + i * PAGE_SIZE, phys,
                     PTE_PRESENT | PTE_WRITABLE | PTE_USER))
            return -(int64_t)ENOMEM;
    }
    return (int64_t)(uintptr_t)base;
}

static int64_t impl_munmap(uint64_t addr, uint64_t len) {
    uint64_t pages = (len + PAGE_SIZE - 1) / PAGE_SIZE;
    for (uint64_t i = 0; i < pages; i++) {
        uint64_t va   = addr + i * PAGE_SIZE;
        uint64_t phys = vmm_virt_to_phys(va);
        /* pages owned by a memfd must not be freed here, they back the
         * shared "file" and may be mapped in other places.  The same
         * applies to framebuffer memory: device memory
         * and driver backing buffers are not PMM pages at all. */
        if (phys && !kobj_memfd_owns_phys(phys) &&
            !fbdev_owns_phys(phys) && !drm_owns_phys(phys) &&
            !pagecache_owns(phys))
            pmm_free(phys);
        vmm_unmap(va);
    }
    return 0;
}

/* Individual syscall implementations */

/* *at() path resolution
 * Builds the absolute path for an openat()-style (dirfd, path) pair by
 * reconstructing the dirfd vnode's path (vfs_node_path) and joining the
 * relative path.  Returns a pointer to a static buffer (the kernel is
 * cooperative/single-threaded), or NULL, then at_path_err holds the
 * appropriate errno (EBADF / ENOTDIR / ENOSYS). */
static int at_path_err;
static char at_path_buf[VFS_PATH_MAX];
static char at_path_buf2[VFS_PATH_MAX];   /* second result (rename needs
                                           * old+new simultaneously - a
                                           * single static buffer aliased
                                           * them and made oldpath ==
                                           * newpath, ENOENT) */
static const char *at_path_into(char *out, int dirfd, const char *path) {
    at_path_err = EINVAL;
    if (!path) return NULL;
    if (path[0] == '/' || dirfd == AT_FDCWD) {
        resolve_path(out, path);
        return out;
    }
    vfs_node_t *dir = vfs_node_from_fd(dirfd);
    if (!dir) { at_path_err = EBADF; return NULL; }
    if (dir->type != VFS_TYPE_DIR) { at_path_err = ENOTDIR; return NULL; }
    char dirbuf[VFS_PATH_MAX];
    if (vfs_node_path(dir, dirbuf, sizeof(dirbuf)) < 0) {
        at_path_err = ENOSYS;
        return NULL;
    }
    path_join(out, VFS_PATH_MAX, dirbuf, path);
    path_normalize(out);
    return out;
}

static const char *at_path(int dirfd, const char *path) {
    return at_path_into(at_path_buf, dirfd, path);
}

/* one-level procfs symlink following (/proc/self/exe, /cwd,
 * /root and /proc/self/fd/<n>).  Target receives the absolute target
 * path when PATH resolves to a pseudo link node. */
static bool resolve_pseudo_link(const char *path, char *target, size_t cap) {
    vfs_node_t *n = vfs_resolve(path);
    if (!n || n->type != VFS_TYPE_LINK) return false;
    int tl = pseudo_link_target(n, target, cap);
    return (tl > 0);
}

static int64_t impl_open(const char *path, int flags, int mode) {
    char buf[VFS_PATH_MAX];
    resolve_path(buf, path);
    {
        char tgt[VFS_PATH_MAX];
        if (resolve_pseudo_link(buf, tgt, sizeof(tgt)))
            resolve_path(buf, tgt);   /* open through the link */
    }
    vfs_stat_t pre;
    bool existed = (vfs_stat(buf, &pre) == 0);
    int fd = vfs_open(buf, flags);
    if (fd >= 0 && (flags & O_CREAT) && !existed) {
        /* Apply the creation mode with the PER-PROCESS umask (gettable/
         * settable via sys_umask), files created by `> file` land as
         * 0644, like Linux. */
        vfs_fchmod(fd, ((uint32_t)mode & 07777u) & ~vfs_cred_umask());
    }
    return fd;
}

static int64_t impl_openat(int dirfd, const char *path, int flags, int mode) {
    const char *abs = at_path(dirfd, path);
    if (!abs) return -(int64_t)at_path_err;
    return impl_open(abs, flags, mode);
}

static int64_t impl_stat(const char *path, linux_stat_t *ls) {
    char buf[VFS_PATH_MAX];
    resolve_path(buf, path);
    {
        char tgt[VFS_PATH_MAX];
        if (resolve_pseudo_link(buf, tgt, sizeof(tgt)))
            resolve_path(buf, tgt);   /* stat(2) follows the link */
    }
    vfs_stat_t vs;
    int r = vfs_stat(buf, &vs);
    if (r < 0) return -(int64_t)ENOENT;
    fill_linux_stat(ls, &vs);
    return 0;
}

static int64_t impl_lstat(const char *path, linux_stat_t *ls) {
    return impl_stat(path, ls);  /* no symlinks, lstat == stat */
}

static int64_t impl_fstat(int fd, linux_stat_t *ls) {
    vfs_stat_t vs;
    int r = vfs_fstat(fd, &vs);
    if (r < 0) return -(int64_t)EBADF;
    fill_linux_stat(ls, &vs);
    return 0;
}

static int64_t impl_fstatat(int dirfd, const char *path,
                              linux_stat_t *ls, int flags) {
    (void)flags;   /* AT_SYMLINK_NOFOLLOW: no symlinks exist yet */
    vfs_stat_t vs;
    int r;
    if (!path) {                 /* fstatat(fd, NULL, ...) -> stat the fd */
        r = vfs_fstat(dirfd, &vs);
        if (r < 0) return -(int64_t)EBADF;
        fill_linux_stat(ls, &vs);
        return 0;
    }
    const char *abs = at_path(dirfd, path);
    if (!abs) return -(int64_t)at_path_err;
    r = vfs_stat(abs, &vs);
    if (r < 0) return -(int64_t)ENOENT;
    fill_linux_stat(ls, &vs);
    return 0;
}

/* statx (musl 1.2.x fstatat calls this first, no ENOSYS fallback) */
static int64_t impl_statx(int dirfd, const char *path, int flags,
                          uint32_t mask, linux_statx_t *sx) {
    (void)flags;
    if (!sx) return -(int64_t)EFAULT;

    vfs_stat_t vs;
    int r;
    if (path) {
        const char *abs = at_path(dirfd, path);
        if (!abs) return -(int64_t)at_path_err;
        r = vfs_stat(abs, &vs);
    } else {
        r = vfs_fstat(dirfd, &vs);
    }
    if (r < 0) return -(int64_t)ENOENT;

    kmemset(sx, 0, sizeof(*sx));
    sx->stx_mask   = STATX_BASIC_STATS;
    sx->stx_blksize = 4096;
    sx->stx_nlink  = 1;
    sx->stx_uid    = 0;
    sx->stx_gid    = 0;
    sx->stx_ino    = vs.st_ino;
    sx->stx_size   = vs.st_size;
    sx->stx_blocks = (vs.st_size + 511) / 512;
    sx->stx_attributes_mask = 0;
    sx->stx_mode   = (uint16_t)(vs.st_mode ? vs.st_mode
                                 : (vs.st_type == VFS_TYPE_DIR ? S_IFDIR | 0755 :
                                    vs.st_type == VFS_TYPE_DEV ? S_IFCHR | 0666 :
                                    vs.st_type == VFS_TYPE_PIPE ? S_IFIFO | 0600 :
                                                                    S_IFREG | 0644));

    uint64_t now = wall_now_secs();
    uint64_t mt  = vs.st_mtime ? vs.st_mtime : now;
    sx->stx_atime.tv_sec = (int64_t)now;
    sx->stx_mtime.tv_sec = sx->stx_ctime.tv_sec = sx->stx_btime.tv_sec = (int64_t)mt;
    sx->stx_atime.tv_nsec = 0;
    sx->stx_mtime.tv_nsec = sx->stx_ctime.tv_nsec = sx->stx_btime.tv_nsec = 0;
    return 0;
}

static int64_t impl_access(const char *path, int mode) {
    (void)mode;
    char buf[VFS_PATH_MAX];
    resolve_path(buf, path);
    vfs_stat_t vs;
    return (vfs_stat(buf, &vs) == 0) ? 0 : -(int64_t)ENOENT;
}

static int64_t impl_chdir(const char *path) {
    char newpath[VFS_PATH_MAX];
    resolve_path(newpath, path);
    vfs_stat_t st;
    if (vfs_stat(newpath, &st) < 0) return -(int64_t)ENOENT;
    if (st.st_type != VFS_TYPE_DIR)  return -(int64_t)ENOTDIR;
    size_t i = 0;
    if (g_current_proc) {
        while (newpath[i] && i < VFS_PATH_MAX-1) { g_current_proc->cwd[i] = newpath[i]; i++; }
        g_current_proc->cwd[i] = '\0';
    }
    return 0;
}

static int64_t impl_getcwd(char *buf, size_t len) {
    size_t i = 0;
    const char *curr = g_current_proc ? g_current_proc->cwd : "/";
    while (curr[i] && i < len-1) { buf[i] = curr[i]; i++; }
    buf[i] = '\0';
    return (int64_t)(uintptr_t)buf;
}

static int64_t impl_mkdir(const char *path, int mode) {
    (void)mode;
    char buf[VFS_PATH_MAX];
    resolve_path(buf, path);
    return vfs_mkdir(buf);
}

static int64_t impl_rmdir(const char *path) {
    char buf[VFS_PATH_MAX];
    resolve_path(buf, path);
    return vfs_unlink(buf);
}

static int64_t impl_unlink(const char *path) {
    char buf[VFS_PATH_MAX];
    resolve_path(buf, path);
    return vfs_unlink(buf);
}

static int64_t impl_unlinkat(int dirfd, const char *path, int flags) {
    (void)flags;   /* AT_REMOVEDIR: unlink of an empty dir behaves the same */
    const char *abs = at_path(dirfd, path);
    if (!abs) return -(int64_t)at_path_err;
    return impl_unlink(abs);
}

static int64_t impl_readlink(const char *path, char *buf, size_t sz) {
    char full[VFS_PATH_MAX];
    resolve_path(full, path);
    vfs_node_t *n = vfs_resolve(full);
    if (!n) return -(int64_t)ENOENT;
    char tgt[VFS_PATH_MAX];
    int tl = pseudo_link_target(n, tgt, sizeof(tgt));
    if (tl <= 0) return -(int64_t)EINVAL;   /* not a link (or dead fd) */
    size_t n_copy = (size_t)tl;
    if (sz && n_copy > sz) n_copy = sz;     /* Linux truncates to bufsiz */
    for (size_t i = 0; i < n_copy; i++) buf[i] = tgt[i];
    return (int64_t)n_copy;
}

static int64_t impl_chmod(const char *path, uint32_t mode) {
    char buf[VFS_PATH_MAX];
    resolve_path(buf, path);
    return (int64_t)vfs_chmod(buf, mode);   /* real mode storage now */
}

static int64_t impl_symlink(const char *target, const char *linkpath) {
    (void)target; (void)linkpath;
    return -38; /* ENOSYS */
}

static int64_t impl_utimensat(int dirfd, const char *pathname, const void *times, int flags) {
    (void)dirfd; (void)times; (void)flags;
    if (!vfs_resolve(pathname)) return -(int64_t)ENOENT;
    return 0; /* Fake success to keep utilities like touch happy */
}

static int64_t impl_fchmodat(int dirfd, const char *path, uint32_t mode, int flags) {
    (void)flags;
    const char *abs = at_path(dirfd, path);
    if (!abs) return -(int64_t)at_path_err;
    return (int64_t)vfs_chmod(abs, mode);
}

static int64_t impl_fchownat(int dirfd, const char *path, uint32_t uid, uint32_t gid, int flags);

/* chown family - real ownership via vfs_chown/vfs_fchown */
static int64_t impl_chown(const char *path, uint32_t uid, uint32_t gid) {
    char buf[VFS_PATH_MAX];
    resolve_path(buf, path);
    return (int64_t)vfs_chown(buf, uid, gid);
}

static int64_t impl_fchown(int fd, uint32_t uid, uint32_t gid) {
    return (int64_t)vfs_fchown(fd, uid, gid);
}

static int64_t impl_fchownat(int dirfd, const char *path, uint32_t uid, uint32_t gid, int flags) {
    (void)flags;   /* AT_SYMLINK_NOFOLLOW: no symlinks exist */
    if (!path) return impl_fchown(dirfd, uid, gid);
    const char *abs = at_path(dirfd, path);
    if (!abs) return -(int64_t)at_path_err;
    return (int64_t)vfs_chown(abs, uid, gid);
}

static int64_t impl_mount(const char *source, const char *target, const char *fstype, uint64_t mountflags, const void *data) {
    (void)source; (void)target; (void)fstype; (void)mountflags; (void)data;
    /* Basic stub, OS initialization handles real vfs_mount internally */
    return -38; /* ENOSYS */
}

static int64_t impl_umount2(const char *target, int flags) {
    (void)target; (void)flags;
    return -38; /* ENOSYS */
}

static int64_t impl_mknod(const char *path, uint32_t mode, uint64_t dev) {
    (void)dev;
    char dir_path[VFS_PATH_MAX];
    char name[VFS_NAME_MAX];

    size_t len = 0;
    while (path[len]) len++;
    if (len >= VFS_PATH_MAX) return -36; /* ENAMETOOLONG */

    int last_slash = -1;
    for (int i = len - 1; i >= 0; i--) {
        if (path[i] == '/') {
            last_slash = i;
            break;
        }
    }

    if (last_slash == -1) {
        dir_path[0] = '.'; dir_path[1] = '\0';
        for (size_t i = 0; i <= len && i < VFS_NAME_MAX; i++) name[i] = path[i];
    } else {
        for (int i = 0; i < last_slash; i++) dir_path[i] = path[i];
        dir_path[last_slash] = '\0';
        if (last_slash == 0) { dir_path[0] = '/'; dir_path[1] = '\0'; }

        int nidx = 0;
        for (size_t i = last_slash + 1; i <= len && nidx < VFS_NAME_MAX; i++) {
            name[nidx++] = path[i];
        }
    }

    vfs_node_t *dir = vfs_resolve(dir_path);
    if (!dir) return -2; /* ENOENT */
    if (!dir->ops || !dir->ops->mknod) return -38; /* ENOSYS */

    uint8_t type = VFS_TYPE_FILE;
    if ((mode & 0170000) == 0040000) type = VFS_TYPE_DIR;
    else if ((mode & 0170000) == 0020000 || (mode & 0170000) == 0060000) type = VFS_TYPE_DEV;

    vfs_node_t *child = dir->ops->mknod(dir, name, type);
    if (!child) return -5; /* EIO */
    return 0;
}

static int64_t impl_lseek(int fd, int64_t off, int whence) {
    return (int64_t)vfs_lseek(fd, (size_t)off, whence);
}

/* positional I/O (pread64/pwrite64) - atomic w.r.t. the cooperative
 * kernel: we temporarily move the shared fd offset and restore it. */
static int64_t impl_pread64(int fd, void *buf, uint64_t n, uint64_t off) {
    if (off > 0x7fffffffULL) return -(int64_t)EINVAL;
    int64_t pos = vfs_lseek(fd, 0, 1);
    if (pos < 0) return pos;
    int64_t r = vfs_lseek(fd, (size_t)off, 0);
    if (r < 0) return r;
    r = vfs_read(fd, buf, (size_t)n);
    vfs_lseek(fd, (size_t)pos, 0);
    return r;
}

static int64_t impl_pwrite64(int fd, const void *buf, uint64_t n, uint64_t off) {
    if (off > 0x7fffffffULL) return -(int64_t)EINVAL;
    int64_t pos = vfs_lseek(fd, 0, 1);
    if (pos < 0) return pos;
    int64_t r = vfs_lseek(fd, (size_t)off, 0);
    if (r < 0) return r;
    r = vfs_write(fd, buf, (size_t)n);
    vfs_lseek(fd, (size_t)pos, 0);
    return r;
}

/* fchdir - change cwd to the directory referenced by an open fd */
static int64_t impl_fchdir(int fd) {
    vfs_node_t *n = vfs_node_from_fd(fd);
    if (!n) return -(int64_t)EBADF;
    if (n->type != VFS_TYPE_DIR) return -(int64_t)ENOTDIR;
    char buf[VFS_PATH_MAX];
    if (vfs_node_path(n, buf, sizeof(buf)) < 0) return -(int64_t)ENOSYS;
    if (g_current_proc) {
        size_t i = 0;
        while (buf[i] && i < VFS_PATH_MAX - 1) {
            g_current_proc->cwd[i] = buf[i]; i++;
        }
        g_current_proc->cwd[i] = '\0';
    }
    return 0;
}

static int64_t impl_fchmod(int fd, uint32_t mode) {
    return (int64_t)vfs_fchmod(fd, mode);
}

static int64_t impl_dup3(int fd, int newfd, int flags) {
    (void)flags;   /* O_CLOEXEC handling omitted, no exec-time difference */
    return vfs_dup2(fd, newfd);
}

static int64_t impl_dup(int fd) { return vfs_dup(fd); }
static int64_t impl_dup2(int fd, int newfd) { return vfs_dup2(fd, newfd); }

static int64_t impl_fcntl(int fd, int cmd, uint64_t arg) {
    switch (cmd) {
        case F_GETFD: return 0;
        case F_SETFD: (void)arg; return 0;
        case F_GETFL: {
            /* Return the real flags stored when this fd was opened.
             * vfs_fd_getflags() reads directly from the fd_table slot. */
            int fl = vfs_fd_getflags(fd);
            return fl < 0 ? -(int64_t)EBADF : (int64_t)fl;
        }
        case F_SETFL: {
            /* Linux only lets F_SETFL change the per-file status flags
             * (O_APPEND, O_NONBLOCK, ...) - keep the access mode. */
            int cur = vfs_fd_getflags(fd);
            if (cur < 0) return -(int64_t)EBADF;
            int nfl = (cur & O_ACCMODE) |
                      ((int)arg & ~(O_ACCMODE | O_CREAT | O_EXCL | O_NOCTTY |
                                    O_TRUNC | O_CLOEXEC | O_DIRECTORY));
            return vfs_fd_setflags(fd, nfl);
        }
        case F_DUPFD:        return vfs_dup(fd);
        case F_DUPFD_CLOEXEC: return vfs_dup(fd);
        /* POSIX + OFD record locks */
        case F_GETLK: case F_SETLK: case F_SETLKW:
        case F_OFD_GETLK: case F_OFD_SETLK: case F_OFD_SETLKW:
            return sys_fcntl_lock(fd, cmd, arg);
        default:      return -(int64_t)EINVAL;
    }
}

static int64_t impl_rename(const char *oldpath, const char *newpath) {
    return (int64_t)vfs_rename(oldpath, newpath);
}

static int64_t impl_renameat(int olddirfd, const char *oldpath,
                              int newdirfd, const char *newpath) {
    const char *olds = at_path_into(at_path_buf, olddirfd, oldpath);
    if (!olds) return -(int64_t)at_path_err;
    const char *news = at_path_into(at_path_buf2, newdirfd, newpath);
    if (!news) return -(int64_t)at_path_err;
    return (int64_t)vfs_rename(olds, news);
}

/* Phase-1 completeness: statfs family, sendfile,
 * copy_file_range, renameat2 - the probes coreutils/libs make before
 * falling back to slower paths. */

/* Linux struct statfs (x86-64) - musl's struct statfs is byte-identical
 * on this arch (all fields unsigned long / 64-bit, fsid = 2 x int32).  */
typedef struct {
    uint64_t f_type;
    uint64_t f_bsize;
    uint64_t f_blocks;
    uint64_t f_bfree;
    uint64_t f_bavail;
    uint64_t f_files;
    uint64_t f_ffree;
    int32_t  f_fsid[2];
    uint64_t f_namelen;
    uint64_t f_frsize;
    uint64_t f_flags;
    uint64_t f_spare[4];
} kstatfs_t;                    /* 120 bytes */

#define TMPFS_MAGIC        0x01021994
#define EXT2_SUPER_MAGIC   0xEF53

static int64_t fill_statfs(const char *path, kstatfs_t *sb) {
    if (!path || !sb) return -(int64_t)EFAULT;
    vfs_node_t *n = vfs_resolve(path);
    if (!n) return -(int64_t)ENOENT;
    /* Files are fine: fstatfs(2) reports the fs backing the fd's file,
     * not just directories. */
    if (n->type != VFS_TYPE_DIR && n->type != VFS_TYPE_FILE)
        return -(int64_t)ENOTDIR;

    kmemset(sb, 0, sizeof *sb);
    sb->f_type   = 1;           /* placeholder, set below          */
    sb->f_bsize  = 4096;
    sb->f_frsize = 4096;
    /* Storage accounting: file data lives in physical RAM on both boot
     * paths (initrd is unpacked into ramfs, the ext2 image is cached in
     * memory), so PMM totals are the numbers to report.                 */
    sb->f_blocks = pmm_total_pages();
    sb->f_bfree  = pmm_total_pages() - pmm_used_pages();
    sb->f_bavail = sb->f_bfree;
    int used, total;
    vfs_node_stats(&used, &total);
    sb->f_files  = (uint64_t)total;
    sb->f_ffree  = (uint64_t)(total - used);
    sb->f_fsid[0] = 0;
    sb->f_fsid[1] = 0;
    sb->f_namelen = VFS_NAME_MAX;
    sb->f_flags   = 0;
#if defined(BOOT_RAMFS) && BOOT_RAMFS
    sb->f_type = TMPFS_MAGIC;
#else
    sb->f_type = EXT2_SUPER_MAGIC;
#endif
    (void)n;
    return 0;
}

static int64_t impl_statfs(const char *path, kstatfs_t *sb) {
    if (!path) return -(int64_t)EFAULT;
    char rp[VFS_PATH_MAX];
    resolve_path(rp, path);
    return fill_statfs(rp, sb);
}

static int64_t impl_fstatfs(int fd, kstatfs_t *sb) {
    vfs_node_t *n = vfs_node_from_fd(fd);
    if (!n) return -(int64_t)EBADF;
    char pbuf[VFS_PATH_MAX];
    if (vfs_node_path(n, pbuf, sizeof pbuf) < 0) {
        /* anonymous node (pipe/kobj): report the generic numbers */
        if (!sb) return -(int64_t)EFAULT;
        kmemset(sb, 0, sizeof *sb);
        sb->f_type = TMPFS_MAGIC;
        sb->f_bsize = 4096;
        return 0;
    }
    return fill_statfs(pbuf, sb);
}

/* sendfile(out_fd, in_fd, offset*, count).
 * in_fd must be a regular file, out_fd may be any writable descriptor.
 * Explicit offset (offset != NULL) does not touch in_fd's position.     */
static int64_t impl_sendfile(int out_fd, int in_fd, int64_t *offset,
                             size_t count) {
    vfs_node_t *in = vfs_node_from_fd(in_fd);
    if (!in) return -(int64_t)EBADF;
    if (in->type != VFS_TYPE_FILE) return -(int64_t)EINVAL;

    static uint8_t chunk[4096];        /* cooperative kernel: safe */
    size_t total = 0;
    int64_t pos = 0;

    if (offset) {
        pos = *offset;
    } else {
        /* current file position of in_fd */
        int64_t cur = impl_lseek(in_fd, 0, 1 /* SEEK_CUR */);
        if (cur < 0) return cur;
        pos = cur;
    }

    while (total < count) {
        size_t want = count - total;
        if (want > sizeof chunk) want = sizeof chunk;

        int64_t got = (int64_t)in->ops->read(in, chunk, want, (size_t)pos);
        if (got < 0) { if (total == 0) return got; break; }
        if (got == 0) break;                       /* EOF */

        int64_t wr = vfs_write(out_fd, chunk, (size_t)got);
        if (wr < 0) { if (total == 0) return wr; break; }

        pos       += wr;
        total     += (size_t)wr;
        if ((size_t)wr < (size_t)got) break;       /* short write */
    }

    if (offset) *offset = pos;
    else        impl_lseek(in_fd, pos, 0 /* SEEK_SET */);
    return (int64_t)total;
}

/* copy_file_range(in_fd, off_in*, out_fd, off_out*, len, flags).
 * Explicit offsets are only valid for seekable (regular-file) fds.      */
static int64_t impl_copy_file_range(int in_fd, int64_t *off_in,
                                    int out_fd, int64_t *off_out,
                                    size_t len, unsigned flags) {
    (void)flags;
    vfs_node_t *in = vfs_node_from_fd(in_fd);
    vfs_node_t *out = vfs_node_from_fd(out_fd);
    if (!in || !out) return -(int64_t)EBADF;
    if ((off_in && in->type != VFS_TYPE_FILE) ||
        (off_out && out->type != VFS_TYPE_FILE))
        return -(int64_t)ESPIPE;

    static uint8_t chunk[4096];
    size_t total = 0;
    int64_t ipos, opos;

    if (off_in)  ipos  = *off_in;
    else { int64_t c = impl_lseek(in_fd, 0, 1);  if (c < 0) return c;  ipos  = c; }
    if (off_out) opos = *off_out;
    else { int64_t c = impl_lseek(out_fd, 0, 1); if (c < 0) return c; opos = c; }

    while (total < len) {
        size_t want = len - total;
        if (want > sizeof chunk) want = sizeof chunk;

        int64_t got = (int64_t)in->ops->read(in, chunk, want, (size_t)ipos);
        if (got <= 0) { if (got < 0 && total == 0) return got; break; }

        int64_t wr = out->ops->write(out, chunk, (size_t)got, (size_t)opos);
        if (wr <= 0) { if (wr < 0 && total == 0) return wr; break; }

        ipos += wr; opos += wr; total += (size_t)wr;
        if ((size_t)wr < (size_t)got) break;
    }

    if (off_in)  *off_in  = ipos;  else impl_lseek(in_fd,  ipos, 0);
    if (off_out) *off_out = opos;  else impl_lseek(out_fd, opos, 0);
    return (int64_t)total;
}

#define RENAME_NOREPLACE 1u
#define RENAME_EXCHANGE  2u
#define RENAME_WHITEOUT  4u

static int64_t impl_renameat2(int olddirfd, const char *oldpath,
                               int newdirfd, const char *newpath,
                               unsigned int flags) {
    if (flags & (RENAME_EXCHANGE | RENAME_WHITEOUT))
        return -(int64_t)EINVAL;       /* not supported (yet) */

    const char *olds = at_path_into(at_path_buf, olddirfd, oldpath);
    if (!olds) return -(int64_t)at_path_err;
    const char *news = at_path_into(at_path_buf2, newdirfd, newpath);
    if (!news) return -(int64_t)at_path_err;

    if (flags & RENAME_NOREPLACE) {
        vfs_node_t *tgt = vfs_resolve(news);
        if (tgt) return -(int64_t)EEXIST;
    }
    return (int64_t)vfs_rename(olds, news);
}

/* Forward declaration - fd_is_console is defined further below */
static bool fd_is_console(int fd);
/* Forward declarations - console TTY layer (defined with impl_read_console) */
static void tty_termios_get(uint8_t *p);
static void tty_termios_set(const uint8_t *p, bool flush_input);

static int64_t impl_ioctl(int fd, uint64_t req, uint64_t arg) {
    /* Linux's ioctl cmd is a 32-bit value, musl passes `int req`, so
     * _IOR/_IOW constants with the high bit set arrive SIGN-EXTENDED
     * (observed as 0xffffffff80045430 for TIOCGPTN, which made every
     * pty ioctl comparison fail with ENOTTY).  Truncate to 32 bits -
     * the evdev/DRM bit-extraction paths are unaffected. */
    req = (uint32_t)req;
    /*
     * Classify by fd first.  The console is a pseudo-fd (used slot, NULL
     * node) that can live at any fd number once dup'd - dash dups the tty
     * to fd >= 10 via fcntl(F_DUPFD) for job control.  Classifying by fd
     * number (0/1/2) made every interactive shell print "can't access
     * tty, job control turned off" because tcgetpgrp(dup) got ENOTTY.
     */
    bool is_tty = vfs_fd_is_console(fd);

    /* /dev/input/eventN - Linux evdev UAPI
     * Probe the node first: any 'E'-class request on an evdev device
     * routes into the evdev core (EVIOCGVERSION/ID/NAME/PHYS/UNIQ/PROP/
     * Key, EVIOCGBIT, EVIOCGRAB), every other node falls through. */
    {
        vfs_node_t *ev_n = vfs_node_from_fd(fd);
        int ev_idx = -1;
        if (ev_n && pseudo_node_is_evdev(ev_n, &ev_idx))
            return (int64_t)evdev_ioctl(ev_idx, req, arg);
    }

    /* /dev/dri/card0 - Linux DRM/KMS UAPI
     * All 'd'-class requests on the DRI nodes route into the KMS-lite
     * core (version, GET_CAP, MODE_* - dumb buffers + mode set). */
    {
        vfs_node_t *drm_n = vfs_node_from_fd(fd);
        int drm_card = -1;
        if (drm_n && pseudo_node_is_drm(drm_n, &drm_card))
            return drm_ioctl(drm_card, req, arg);
    }

    /* pty master / slave ends
     * TIOCGPTN/TIOCSPTLCK/TIOCGPTPEER on the master, termios/winsize/
     * pgrp on both ends, all served by the pty core. */
    {
        vfs_node_t *pty_n = vfs_node_from_fd(fd);
        if (pty_n && (pty_node_is_master(pty_n) || pty_node_is_slave(pty_n)))
            return pty_ioctl(pty_n, fd, req, arg);
    }

    switch (req) {
    /* Terminal attributes
     * A minimal but real termios: we store the flag words + c_cc so the
     * cooked read path (impl_read_console) can honour ECHO / ICANON /
     * VERASE.  struct termios (musl x86-64):
     *   u32 iflag @0, u32 oflag @4, u32 cflag @8, u32 lflag @12,
     *   u8  line  @16, u8 cc[32] @17, u32 ispeed @49, u32 ospeed @53 */
    case TCGETS: {
        if (!is_tty) return -(int64_t)ENOTTY;
        uint8_t *p = (uint8_t *)(uintptr_t)arg;
        if (!p) return -(int64_t)EFAULT;
        tty_termios_get(p);
        return 0;
    }

    case TCSETS:
    case TCSETSW:
    case TCSETSF:
    case TCGETA:
    case TCSETA:
    case TCSETAW:
    case TCSETAF: {
        if (!is_tty) return -(int64_t)ENOTTY;
        if (req == TCSETS || req == TCSETSW || req == TCSETSF) {
            const uint8_t *p = (const uint8_t *)(uintptr_t)arg;
            if (!p) return -(int64_t)EFAULT;
            bool flush_input = (req == TCSETSF);
            tty_termios_set(p, flush_input);
        }
        return 0;
    }

    /* Window size */
    case TIOCGWINSZ: {
        if (!is_tty) return -(int64_t)ENOTTY;
        /* struct winsize: ws_row, ws_col, ws_xpixel, ws_ypixel (all uint16) */
        typedef struct { uint16_t row; uint16_t col; uint16_t xp; uint16_t yp; } winsize_t;
        winsize_t *ws = (winsize_t *)(uintptr_t)arg;
        if (ws) { ws->row = 24; ws->col = 80; ws->xp = 0; ws->yp = 0; }
        return 0;
    }
    case TIOCSWINSZ:
        if (!is_tty) return -(int64_t)ENOTTY;
        return 0;

    /* Process group / foreground pgroup */
    case TIOCGPGRP: {
        if (!is_tty) return -(int64_t)ENOTTY;
        int *pgrp = (int *)(uintptr_t)arg;
        if (pgrp && g_current_proc) *pgrp = g_current_proc->fg_pgrp
                                          ? g_current_proc->fg_pgrp
                                          : g_current_proc->pgid
                                          ? g_current_proc->pgid
                                          : g_current_proc->pid;
        return 0;
    }
    case TIOCSPGRP: {
        if (!is_tty) return -(int64_t)ENOTTY;
        int *pgrp = (int *)(uintptr_t)arg;
        if (pgrp && g_current_proc) g_current_proc->fg_pgrp = *pgrp;
        return 0;
    }
    case TIOCGSID: {
        if (!is_tty) return -(int64_t)ENOTTY;
        int *sid = (int *)(uintptr_t)arg;
        if (sid && g_current_proc) *sid = g_current_proc->sid
                                        ? g_current_proc->sid
                                        : g_current_proc->pid;
        return 0;
    }
    case TIOCSCTTY:
        /* Make this TTY the controlling terminal of the session, accept */
        return 0;

    /* /dev/fb0 - Linux fbdev UAPI
     * Byte-exact linux/fb.h structs, the primary fbdev's real geometry is
     * reported so userland can mmap and draw like on Linux. */
    case FBIOGET_VSCREENINFO: {
        vfs_node_t *n = vfs_node_from_fd(fd);
        if (!n || !pseudo_node_is_fb0(n)) return -(int64_t)ENOTTY;
        fbdev_t *f = fbdev_primary();
        if (!f) return -(int64_t)ENODEV;
        fb_var_screeninfo_t *v = (fb_var_screeninfo_t *)(uintptr_t)arg;
        if (!v) return -(int64_t)EFAULT;
        kmemset(v, 0, sizeof(*v));
        v->xres = v->xres_virtual = f->width;
        v->yres = v->yres_virtual = f->height;
        v->bits_per_pixel = f->bpp;
        v->red.offset   = 16; v->red.length   = 8;
        v->green.offset =  8; v->green.length = 8;
        v->blue.offset  =  0; v->blue.length  = 8;
        v->transp.offset = 24; v->transp.length = 8;
        v->activate = FB_ACTIVATE_NOW;
        v->vmode    = FB_VMODE_NONINTERLACED;
        return 0;
    }
    case FBIOPUT_VSCREENINFO: {
        vfs_node_t *n = vfs_node_from_fd(fd);
        if (!n || !pseudo_node_is_fb0(n)) return -(int64_t)ENOTTY;
        fbdev_t *f = fbdev_primary();
        if (!f) return -(int64_t)ENODEV;
        fb_var_screeninfo_t *v = (fb_var_screeninfo_t *)(uintptr_t)arg;
        if (!v) return -(int64_t)EFAULT;
        /* real mode set through the modeset core.  The request
         * is validated against the standard mode table (and VRAM);
         * userland then reads back the actual geometry. */
        uint32_t req_bpp = v->bits_per_pixel ? v->bits_per_pixel : 32;
        int rc = modeset_apply(v->xres, v->yres, req_bpp);
        if (rc) return rc;
        kmemset(v, 0, sizeof(*v));
        v->xres = v->xres_virtual = f->width;
        v->yres = v->yres_virtual = f->height;
        v->bits_per_pixel = f->bpp;
        v->red.offset   = 16; v->red.length   = 8;
        v->green.offset =  8; v->green.length = 8;
        v->blue.offset  =  0; v->blue.length  = 8;
        v->transp.offset = 24; v->transp.length = 8;
        v->activate = FB_ACTIVATE_NOW;
        v->vmode    = FB_VMODE_NONINTERLACED;
        return 0;
    }
    case FBIOGET_FSCREENINFO: {
        vfs_node_t *n = vfs_node_from_fd(fd);
        if (!n || !pseudo_node_is_fb0(n)) return -(int64_t)ENOTTY;
        fbdev_t *f = fbdev_primary();
        if (!f) return -(int64_t)ENODEV;
        fb_fix_screeninfo_t *fi = (fb_fix_screeninfo_t *)(uintptr_t)arg;
        if (!fi) return -(int64_t)EFAULT;
        kmemset(fi, 0, sizeof(*fi));
        const char *id = f->name ? f->name : "rmm-fb";
        int i = 0;
        while (id[i] && i < 15) { fi->id[i] = id[i]; i++; }
        fi->smem_start  = vmm_virt_to_phys((uint64_t)(uintptr_t)f->fb);
        fi->smem_len    = f->pitch * f->height;
        fi->type        = FB_TYPE_PACKED_PIXELS;
        fi->visual      = FB_VISUAL_TRUECOLOR;
        fi->xpanstep    = 1;
        fi->ypanstep    = 1;
        fi->line_length = f->pitch;
        fi->accel       = FB_ACCEL_NONE;
        return 0;
    }
    case FBIOPAN_DISPLAY: {
        vfs_node_t *n = vfs_node_from_fd(fd);
        if (!n || !pseudo_node_is_fb0(n)) return -(int64_t)ENOTTY;
        /* RAM-backed drivers (virtio-gpu) must push the whole framebuffer */
        fbdev_flush_all();
        return 0;
    }

    default:
        return -(int64_t)ENOTTY;
    }
}

#include "kernel/version.h"

/* Nodename storage - default "(none)" like the Linux kernel before any
 * sethostname(); the OS init (not the kernel) picks the real name.
 * This is the Linux-alternative answer to the old OS-branded default. */
static char g_nodename[65] = "(none)";

static int64_t impl_uname(linux_utsname_t *u) {
    if (!u) return -(int64_t)EFAULT;
    kmemset(u, 0, sizeof(*u));
    kmemcpy(u->sysname,    RMM_KERNEL_NAME, sizeof(RMM_KERNEL_NAME));
    /* Neutral default nodename (Linux convention); sethostname(2)
     * overrides it.  The hostname is OS policy, the kernel does not
     * hardcode an OS-branded default. */
    for (int i = 0; i < 65 && g_nodename[i]; i++) u->nodename[i] = g_nodename[i];
    kmemcpy(u->release,    RMM_RELEASE,     sizeof(RMM_RELEASE));
    kmemcpy(u->version,    "#1 SMP",           7);
    kmemcpy(u->machine,    "x86_64",           7);
    kmemcpy(u->domainname, "(none)",           7);
    return 0;
}

/* SYS_SETHOSTNAME(170): userland (init) configures the nodename.
 * No capability system exists yet, so any caller may set it - same
 * trust model as the rest of the syscall layer. */
static int64_t impl_sethostname(const char *name, size_t len) {
    if (!name && len) return -(int64_t)EFAULT;
    if (len > 64) len = 64;                 /* Linux: ENAMETOOLONG > 64 */
    char tmp[65];
    size_t i = 0;
    if (name) {
        for (; i < len; i++) {
            char c = name[i];
            if (!c) break;
            tmp[i] = c;
        }
    }
    tmp[i] = '\0';
    for (size_t j = 0; j <= i; j++) g_nodename[j] = tmp[j];
    return 0;
}

static int64_t impl_clock_gettime(int clk, linux_timespec_t *ts) {
    if (!ts) return -(int64_t)EFAULT;
    uint64_t secs, nsecs;
    switch (clk) {
    case 0: /* CLOCK_REALTIME */
    case 5: /* CLOCK_REALTIME_COARSE */
        secs  = wall_now_secs();
        nsecs = (uint32_t)((pit_ticks() % 1000) * 1000000ULL);
        break;
    default: /* MONOTONIC(1), CPUTIME(2,3), RAW(4), BOOTTIME(7), ... */
        uint64_t ms = pit_ticks();
        secs  = ms / 1000;
        nsecs = (uint32_t)((ms % 1000) * 1000000ULL);
        break;
    }
    ts->tv_sec  = (int64_t)secs;
    ts->tv_nsec = (int64_t)nsecs;
    return 0;
}

static int64_t impl_gettimeofday(linux_timeval_t *tv, void *tz) {
    (void)tz;
    if (!tv) return 0;
    tv->tv_sec  = (int64_t)wall_now_secs();
    tv->tv_usec = (int64_t)((pit_ticks() % 1000) * 1000LL);
    return 0;
}

static int64_t impl_getgroups(int size, uint32_t *list) {
    if (!g_current_proc) { if (list && size > 0) { list[0] = 0; return 1; } return 0; }
    int n = g_current_proc->ngroups;
    if (size == 0) return n;             /* musl size-probe */
    if (size < n) return -(int64_t)EINVAL;
    if (!list) return -(int64_t)EFAULT;
    for (int i = 0; i < n; i++) list[i] = g_current_proc->groups[i];
    return n;
}

/*
 *  Credentials - get/set uid/gid/egid/suid, supplementary groups.
 *  Semantics follow Linux (man 2 setuid):
 *
 *  setuid(uid):
 *    - euid 0  -> set uid=euid=suid=uid  (permanent drop, like Linux)
 *    - else    -> only uid or suid may become the new euid, else EPERM
 *  setresuid(r,e,s): euid 0 -> anything, else each of r/e/s may be either
 *    the old value ("don't change") or uid/euid/suid, else EPERM.
 */
static int64_t impl_setuid(int uid) {
    if (!g_current_proc) return 0;
    if (g_current_proc->euid == 0) {
        g_current_proc->uid  = (uint32_t)uid;
        g_current_proc->euid = (uint32_t)uid;
        g_current_proc->suid = (uint32_t)uid;
        return 0;
    }
    if ((uint32_t)uid == g_current_proc->uid ||
        (uint32_t)uid == g_current_proc->suid) {
        g_current_proc->euid = (uint32_t)uid;
        return 0;
    }
    return -(int64_t)EPERM;
}

static int64_t impl_setgid(int gid) {
    if (!g_current_proc) return 0;
    if (g_current_proc->euid == 0) {
        g_current_proc->gid  = (uint32_t)gid;
        g_current_proc->egid = (uint32_t)gid;
        g_current_proc->sgid = (uint32_t)gid;
        return 0;
    }
    if ((uint32_t)gid == g_current_proc->gid ||
        (uint32_t)gid == g_current_proc->sgid) {
        g_current_proc->egid = (uint32_t)gid;
        return 0;
    }
    return -(int64_t)EPERM;
}

static int64_t impl_setreuid(int ruid, int euid) {
    process_t *p = g_current_proc;
    if (!p) return 0;
    if ((uint32_t)ruid != (uint32_t)-1 &&
        (uint32_t)ruid != p->uid && (uint32_t)ruid != p->euid &&
        (uint32_t)ruid != p->suid && p->euid != 0)
        return -(int64_t)EPERM;
    if ((uint32_t)euid != (uint32_t)-1 &&
        (uint32_t)euid != p->uid && (uint32_t)euid != p->suid &&
        p->euid != 0)
        return -(int64_t)EPERM;
    if ((uint32_t)ruid != (uint32_t)-1) p->uid = (uint32_t)ruid;
    if ((uint32_t)euid != (uint32_t)-1) {
        p->euid = (uint32_t)euid;
        /* unprivileged swap keeps the old euid as saved id */
        if (p->euid != p->uid && p->euid != p->suid)
            p->suid = p->euid;
    }
    return 0;
}

static int64_t impl_setregid(int rgid, int egid) {
    process_t *p = g_current_proc;
    if (!p) return 0;
    if ((uint32_t)rgid != (uint32_t)-1 &&
        (uint32_t)rgid != p->gid && (uint32_t)rgid != p->egid &&
        (uint32_t)rgid != p->sgid && p->euid != 0)
        return -(int64_t)EPERM;
    if ((uint32_t)egid != (uint32_t)-1 &&
        (uint32_t)egid != p->gid && (uint32_t)egid != p->sgid &&
        p->euid != 0)
        return -(int64_t)EPERM;
    if ((uint32_t)rgid != (uint32_t)-1) p->gid = (uint32_t)rgid;
    if ((uint32_t)egid != (uint32_t)-1) {
        p->egid = (uint32_t)egid;
        if (p->egid != p->gid && p->egid != p->sgid)
            p->sgid = p->egid;
    }
    return 0;
}

static int64_t impl_setresuid(int ruid, int euid, int suid) {
    process_t *p = g_current_proc;
    if (!p) return 0;
    if (p->euid != 0) {
        if ((uint32_t)ruid != (uint32_t)-1 &&
            (uint32_t)ruid != p->uid && (uint32_t)ruid != p->euid &&
            (uint32_t)ruid != p->suid)
            return -(int64_t)EPERM;
        if ((uint32_t)euid != (uint32_t)-1 &&
            (uint32_t)euid != p->uid && (uint32_t)euid != p->euid &&
            (uint32_t)euid != p->suid)
            return -(int64_t)EPERM;
        if ((uint32_t)suid != (uint32_t)-1 &&
            (uint32_t)suid != p->uid && (uint32_t)suid != p->euid &&
            (uint32_t)suid != p->suid)
            return -(int64_t)EPERM;
    }
    if ((uint32_t)ruid != (uint32_t)-1) p->uid  = (uint32_t)ruid;
    if ((uint32_t)euid != (uint32_t)-1) p->euid = (uint32_t)euid;
    if ((uint32_t)suid != (uint32_t)-1) p->suid = (uint32_t)suid;
    return 0;
}

static int64_t impl_setresgid(int rgid, int egid, int sgid) {
    process_t *p = g_current_proc;
    if (!p) return 0;
    if (p->euid != 0) {
        if ((uint32_t)rgid != (uint32_t)-1 &&
            (uint32_t)rgid != p->gid && (uint32_t)rgid != p->egid &&
            (uint32_t)rgid != p->sgid)
            return -(int64_t)EPERM;
        if ((uint32_t)egid != (uint32_t)-1 &&
            (uint32_t)egid != p->gid && (uint32_t)egid != p->egid &&
            (uint32_t)egid != p->sgid)
            return -(int64_t)EPERM;
        if ((uint32_t)sgid != (uint32_t)-1 &&
            (uint32_t)sgid != p->gid && (uint32_t)sgid != p->egid &&
            (uint32_t)sgid != p->sgid)
            return -(int64_t)EPERM;
    }
    if ((uint32_t)rgid != (uint32_t)-1) p->gid  = (uint32_t)rgid;
    if ((uint32_t)egid != (uint32_t)-1) p->egid = (uint32_t)egid;
    if ((uint32_t)sgid != (uint32_t)-1) p->sgid = (uint32_t)sgid;
    return 0;
}

static int64_t impl_getresuid(uint32_t *ruid, uint32_t *euid, uint32_t *suid) {
    if (!g_current_proc) return 0;
    if (ruid) *ruid = g_current_proc->uid;
    if (euid) *euid = g_current_proc->euid;
    if (suid) *suid = g_current_proc->suid;
    return 0;
}

static int64_t impl_getresgid(uint32_t *rgid, uint32_t *egid, uint32_t *sgid) {
    if (!g_current_proc) return 0;
    if (rgid) *rgid = g_current_proc->gid;
    if (egid) *egid = g_current_proc->egid;
    if (sgid) *sgid = g_current_proc->sgid;
    return 0;
}

static int64_t impl_setgroups(int size, const uint32_t *list) {
    if (!g_current_proc) return 0;
    if (!vfs_cred_is_root()) return -(int64_t)EPERM;
    if (size < 0 || size > PROC_NGROUPS_MAX) return -(int64_t)EINVAL;
    if (size > 0 && !list) return -(int64_t)EFAULT;
    for (int i = 0; i < size; i++)
        g_current_proc->groups[i] = list[i];
    g_current_proc->ngroups = size;
    return 0;
}

static int64_t impl_pipe(int *fds) {
    return (int64_t)vfs_pipe(fds);
}

/* mremap (copy-based, always MAYMOVE) */
static int64_t impl_mremap(uint64_t old_addr, uint64_t old_len,
                           uint64_t new_len, uint64_t flags,
                           uint64_t new_addr) {
    (void)flags; (void)new_addr;
    if (old_len == 0) return -(int64_t)EINVAL;
    if (new_len == 0) { impl_munmap(old_addr, old_len); return -(int64_t)EINVAL; }

    uint64_t old_pages = (old_len + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t new_pages = (new_len + PAGE_SIZE - 1) / PAGE_SIZE;

    /* allocate fresh region (free-region scan, see mmap_pick_region) */
    if (!g_current_proc) return -(int64_t)ENOMEM;
    uint64_t base = mmap_pick_region(new_pages);
    if (!base) return -(int64_t)ENOMEM;
    g_current_proc->mmap_bump = base + new_pages * PAGE_SIZE;

    for (uint64_t i = 0; i < new_pages; i++) {
        uint64_t phys = pmm_alloc();
        if (!phys) return -(int64_t)ENOMEM;
        uint8_t *p = (uint8_t *)(uintptr_t)phys;
        for (size_t b = 0; b < PAGE_SIZE; b++) p[b] = 0;
        if (!vmm_map(base + i * PAGE_SIZE, phys,
                     PTE_PRESENT | PTE_WRITABLE | PTE_USER))
            return -(int64_t)ENOMEM;
    }

    /* copy overlap (min of old/new), then release the old region */
    uint64_t copy_bytes = (old_len < new_len) ? old_len : new_len;
    uint8_t *dst = (uint8_t *)(uintptr_t)base;
    const uint8_t *src = (const uint8_t *)(uintptr_t)old_addr;
    for (uint64_t b = 0; b < copy_bytes; b++) dst[b] = src[b];

    impl_munmap(old_addr, old_pages * PAGE_SIZE);
    return (int64_t)(uintptr_t)base;
}

/* arch_prctl - TLS setup for musl (ARCH_SET_FS) */
static void wrmsr_fs(uint64_t val) {
    __asm__ volatile("wrmsr" :: "c"(0xC0000100u),   /* IA32_FS_BASE */
                                "a"((uint32_t)val),
                                "d"((uint32_t)(val >> 32)));
}
static uint64_t rdmsr_fs(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0xC0000100u));
    return ((uint64_t)hi << 32) | lo;
}

static int64_t impl_arch_prctl(int code, uint64_t addr) {
    process_t *p = g_current_proc;
    switch (code) {
    case ARCH_SET_FS:
        if (p) p->fs_base = addr;
        wrmsr_fs(addr);
        return 0;
    case ARCH_GET_FS:
        if (!addr) return -(int64_t)EFAULT;
        *(uint64_t *)(uintptr_t)addr = p ? p->fs_base : rdmsr_fs();
        return 0;
    case ARCH_SET_GS:
        /* GS base unused by musl on the main thread, accept & ignore */
        return 0;
    case ARCH_GET_GS:
        if (!addr) return -(int64_t)EFAULT;
        *(uint64_t *)(uintptr_t)addr = 0;
        return 0;
    default:
        return -(int64_t)EINVAL;
    }
}

/* futex: minimal private-futex support for musl locks
 * musl's internal locks (__lock/__unlock) degrade to busy-spins when
 * SYS_futex returns ENOSYS, combined with the BORE preemption that turned
 * into observable multi-second user-mode spins.  Real WAIT/WAKE (keyed by
 * (pid, uaddr) - musl only uses private futexes within one address space)
 * parks waiters as true sleeping tasks instead.
 * The waiter list is tiny: one entry per live task at most.              */
typedef struct { process_t *p; uint32_t *uaddr; } k_futex_waiter_t;
static k_futex_waiter_t k_futex_tab[PROC_MAX * 2];
static int              k_futex_n;

/* entries are keyed by (address space, uaddr) instead of
 * (pid, uaddr) - CLONE_VM threads share one address space under
 * different tids, and musl synchronises all of them through the same
 * futex words (thread list lock, mutexes, condvars, join states). */

void k_futex_detach_task(process_t *p) {
    for (int i = 0; i < k_futex_n; i++) {
        if (k_futex_tab[i].p == p) {
            k_futex_tab[i] = k_futex_tab[--k_futex_n];
            return;
        }
    }
}

static bool futex_tab_eq(int i, uint32_t *uaddr, uint64_t cr3_key) {
    return k_futex_tab[i].uaddr == uaddr &&
           k_futex_tab[i].p->cr3 == cr3_key;
}

/* Wake up to n waiters on (cr3_key, uaddr).  Used by FUTEX_WAKE and by
 * the thread-exit path (proc_clear_ctid_wake). */
int k_futex_wake_uaddr(uint64_t cr3_key, uint32_t *uaddr, int n) {
    int woken = 0;
    for (int i = 0; i < k_futex_n && woken < n; ) {
        if (futex_tab_eq(i, uaddr, cr3_key)) {
            process_t *w = k_futex_tab[i].p;
            k_futex_tab[i] = k_futex_tab[--k_futex_n];
            w->futex_woken = true;
            sched_wake(w);
            woken++;
        } else i++;
    }
    return woken;
}

static int64_t impl_futex(uint32_t *uaddr, int op, uint32_t val,
                          uint64_t timeout_ticks, uint32_t val2,
                          uint32_t *uaddr2) {
    if (!g_current_proc) return -(int64_t)EINVAL;
    int fop = op & 0x7F;                 /* strip private | CLOCK_REALTIME */
    uint64_t cr3_key = g_current_proc->cr3;

    /* FUTEX_WAIT_BITSET (musl's __timedwait uses plain wait, but map
     * bitset-with-ANY onto wait so late musl paths keep working). */
    if (fop == 9) fop = 0;

    if (fop == 0) {                      /* FUTEX_WAIT */
        uint32_t cur = *uaddr;           /* user VA readable: kernel half shares user map */
        if (cur != val) return -(int64_t)EAGAIN;
        if (k_futex_n >= (int)(sizeof(k_futex_tab)/sizeof(k_futex_tab[0])))
            return -(int64_t)EAGAIN;
        g_current_proc->futex_woken = false;
        k_futex_tab[k_futex_n].p     = g_current_proc;
        k_futex_tab[k_futex_n].uaddr = uaddr;
        k_futex_n++;
        sched_kernel_park(timeout_ticks);      /* 0 = event-only */
        bool woken = g_current_proc->futex_woken;
        k_futex_detach_task(g_current_proc);   /* timeout/spurious: drop entry */
        return woken ? 0 : -(int64_t)ETIMEDOUT;
    }
    if (fop == 1)                        /* FUTEX_WAKE */
        return k_futex_wake_uaddr(cr3_key, uaddr, (int)val);
    if (fop == 3 || fop == 4) {          /* FUTEX_REQUEUE / CMP_REQUEUE */
        if (fop == 4) {
            uint32_t cur = *uaddr;
            if (cur != val) return -(int64_t)EAGAIN;
        }
        int woken = k_futex_wake_uaddr(cr3_key, uaddr, (int)val2);
        /* move the remaining waiters from uaddr1 to uaddr2 */
        for (int i = 0; i < k_futex_n; i++) {
            if (futex_tab_eq(i, uaddr, cr3_key)) {
                k_futex_tab[i].uaddr = uaddr2;
            }
        }
        return woken;
    }
    /* PI / WAKE_OP / FD - musl never issues these for its basic locks,
     * report ENOSYS so callers take their documented fallbacks. */
    return -(int64_t)ENOSYS;
}

static int64_t impl_set_tid_address(int *tidptr) {
#if FORK_DEBUG
    display_printf("[fd] set_tid: pid=%d ptr=0x%lx\n",
                   g_current_proc ? g_current_proc->pid : -1,
                   (unsigned long)tidptr);
#endif
    /* Linux: set_tid_address sets clear_child_tid (kernel writes 0 there
     * and wakes the futex on this task's exit). */
    if (g_current_proc && tidptr)
        g_current_proc->clear_child_tid = (uint64_t)(uintptr_t)tidptr;
    return g_current_proc ? (int64_t)g_current_proc->pid : 1;
}

static int64_t impl_faccessat(int dirfd, const char *path, int mode, int flags) {
    (void)flags;
    const char *abs = at_path(dirfd, path);
    if (!abs) return -(int64_t)at_path_err;
    return impl_access(abs, mode);
}

/* faccessat2 - flags AT_SYMLINK_NOFOLLOW/AT_EACCESS are no-ops
 * on this VFS (no symlinks, euid is already what impl_access checks). */
static int64_t impl_faccessat2(int dirfd, const char *path, int mode, int flags) {
    (void)flags;
    const char *abs = at_path(dirfd, path);
    if (!abs) return -(int64_t)at_path_err;
    return impl_access(abs, mode);
}

static int64_t impl_readlinkat(int dirfd, const char *path, char *buf, size_t sz) {
    (void)dirfd; (void)path; (void)buf; (void)sz;
    return -(int64_t)EINVAL;  /* no symlinks */
}

static int64_t impl_mkdirat(int dirfd, const char *path, int mode) {
    (void)mode;
    const char *abs = at_path(dirfd, path);
    if (!abs) return -(int64_t)at_path_err;
    return impl_mkdir(abs, mode);
}

static int64_t impl_mknodat(int dirfd, const char *path, uint32_t mode, uint64_t dev) {
    const char *abs = at_path(dirfd, path);
    if (!abs) return -(int64_t)at_path_err;
    return impl_mknod(abs, mode, dev);
}

static bool fd_is_console(int fd);

static int64_t impl_writev(int fd, const void *iov_ptr, int iovcnt) {
    /* struct iovec { void *base, size_t len; } */
    typedef struct { uint64_t base; uint64_t len; } iovec_t;
    const iovec_t *iov = (const iovec_t *)iov_ptr;
    int64_t total = 0;
    bool con = (fd == 1 || fd == 2) && fd_is_console(fd);
    for (int i = 0; i < iovcnt; i++) {
        int64_t r;
        if (con) {
            /* console fds are kernel pseudo-fds (no VFS node), dprintf()
             * in init/dash reaches us via writev, so mirror write().      */
            const char *s = (const char *)(uintptr_t)iov[i].base;
            for (size_t j = 0; j < iov[i].len; j++) display_putchar(s[j]);
            r = (int64_t)iov[i].len;
        } else {
            r = vfs_write(fd, (const void *)(uintptr_t)iov[i].base,
                          (size_t)iov[i].len);
        }
        if (r < 0) return (total > 0) ? total : r;
        total += r;
    }
    return total;
}

static int64_t impl_readv(int fd, const void *iov_ptr, int iovcnt) {
    typedef struct { uint64_t base; uint64_t len; } iovec_t;
    const iovec_t *iov = (const iovec_t *)iov_ptr;
    int64_t total = 0;
    for (int i = 0; i < iovcnt; i++) {
        int64_t r = vfs_read(fd, (void *)(uintptr_t)iov[i].base,
                              (size_t)iov[i].len);
        if (r < 0) return (total > 0) ? total : r;
        total += r;
    }
    return total;
}

/* Helper: is fd connected to the framebuffer console?
 * Returns true only when the fd is the "raw" stdin/stdout/stderr that was
 * never redirected.  If dup2 has pointed fd 0/1/2 at a pipe node, we must
 * Not touch the display, let vfs_read/vfs_write handle it.
 */
static bool fd_is_console(int fd) {
    /* Delegates to the real check in vfs.c: console = used slot with a
     * NULL node pointer, at any fd number (see vfs_fd_is_console).  This
     * keeps the historical 0/1/2 call sites correct while dup'd console
     * fds (dash job control) now classify properly too.                  */
    return vfs_fd_is_console(fd);
}

static int64_t impl_write_console(int fd, const void *buf, size_t len) {
    if ((fd == 1 || fd == 2) && fd_is_console(fd)) {
        const char *s = (const char *)buf;
        for (size_t i = 0; i < len; i++) display_putchar(s[i]);
        return (int64_t)len;
    }
    return (int64_t)vfs_write(fd, buf, len);
}

/* Console input: PS/2 keyboard + COM1 serial (Linux console style)
 * Blocks with hlt until either source delivers a byte.  Serial input makes
 * the OS usable headless (`qemu -serial stdio`) and in automated tests.
 *
 * Note: the syscall dispatcher runs with IF=0 (SFMASK cleared it on the
 * `syscall` instruction), so a bare hlt here would sleep forever.  Enable
 * interrupts around the halt so the PIT/keyboard/serial can wake us, the
 * polling loop then sees whatever arrived.                            */
static int console_getchar(void) {
    for (;;) {
        int c = kb_getchar_nb();
        if (c) return c;
        c = serial_getchar_nb();
        if (c > 0) return c;
        /* (BORE): park the task instead of halting the only
         * kernel context - other tasks stay runnable while this one
         * waits for input.  Fallback keeps the old hlt when the
         * scheduler is not active (pre-boot). */
        if (sched_kernel_park(BORE_EVENT_POLL_MS) != 0)
            __asm__ volatile("sti; hlt; cli");
    }
}

/* Console TTY layer
 * A canonical ("cooked") line-discipline emulation, modelled after the
 * Linux N_TTY line discipline:
 *
 *   - The kernel keeps the termios state (flag words + c_cc) that
 *     TCGETS/TCSETS expose to userland, so login(1) can turn echo off
 *     for passwords and dash(1) sees sane attributes.
 *
 *   - In canonical mode a line is assembled, echoed (if ECHO), edited
 *     with backspace (VERASE / \b / DEL), inside the kernel, in a
 *     private line buffer.  The finished line is then served out of the
 *     buffer across as many read() calls as the caller makes.
 *
 *     This matters because dash(1) reads stdin one byte per read()
 *     (observed: read(0, buf, 1)); any per-call erase logic in a
 *     stateless reader could never work.  Serving the pre-assembled
 *     line byte-by-byte gives every consumer, dash's byte reads and
 *     musl fgets' line reads, the exact Linux-canonical behaviour.
 */
#define TTY_L_ISIG    0x00000001u
#define TTY_L_ICANON  0x00000002u
#define TTY_L_ECHO    0x00000008u

/* c_cc indices (Linux) */
#define CC_VINTR 0
#define CC_VQUIT 1
#define CC_VERASE 2
#define CC_VKILL 3
#define CC_VEOF 4
#define CC_VMIN 6

static uint32_t g_tty_iflag = 0x00000000u;
static uint32_t g_tty_oflag = 0x00000000u;
static uint32_t g_tty_cflag = 0x000004BFu;   /* CS8 | read CREAD-ish default */
static uint32_t g_tty_lflag = 0x0000003Bu;   /* ISIG|ICANON|ECHO|ECHOE|ECHOK */
static uint8_t  g_tty_cc[32] = {
    3,    /* VINTR  ^C */
    28,   /* VQUIT  ^\ */
    0x7f, /* VERASE DEL */
    21,   /* VKILL  ^U */
    4,    /* VEOF   ^D */
    0,    /* VTIME */
    1,    /* VMIN  */
    0, 17, 19, 26, 0, 0, 0, 0,   /* VSWTC VSTART VSTOP VSUSP VEOL ... */
};
/* canonical line buffer: assembled input waiting to be read */
static char   tty_line[512];
static size_t tty_len = 0;      /* bytes assembled               */
static size_t tty_pos = 0;      /* bytes already served out      */

static void tty_termios_get(uint8_t *p) {
    p[0]  = (uint8_t)(g_tty_iflag);  p[1]  = (uint8_t)(g_tty_iflag >> 8);
    p[2]  = (uint8_t)(g_tty_iflag >> 16); p[3] = (uint8_t)(g_tty_iflag >> 24);
    p[4]  = (uint8_t)(g_tty_oflag);  p[5]  = (uint8_t)(g_tty_oflag >> 8);
    p[6]  = (uint8_t)(g_tty_oflag >> 16); p[7] = (uint8_t)(g_tty_oflag >> 24);
    p[8]  = (uint8_t)(g_tty_cflag);  p[9]  = (uint8_t)(g_tty_cflag >> 8);
    p[10] = (uint8_t)(g_tty_cflag >> 16); p[11] = (uint8_t)(g_tty_cflag >> 24);
    p[12] = (uint8_t)(g_tty_lflag);  p[13] = (uint8_t)(g_tty_lflag >> 8);
    p[14] = (uint8_t)(g_tty_lflag >> 16); p[15] = (uint8_t)(g_tty_lflag >> 24);
    p[16] = 0;                                  /* c_line = N_TTY */
    for (int i = 0; i < 32; i++) p[17 + i] = g_tty_cc[i];
    p[49] = p[50] = p[51] = p[52] = 0;          /* c_ispeed */
    p[53] = p[54] = p[55] = p[56] = 0;          /* c_ospeed */
    /* tail padding stays zero */
    for (int i = 57; i < 60; i++) p[i] = 0;
}

static void tty_termios_set(const uint8_t *p, bool flush_input) {
    g_tty_iflag = (uint32_t)p[0] | ((uint32_t)p[1] << 8)
                | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    g_tty_oflag = (uint32_t)p[4] | ((uint32_t)p[5] << 8)
                | ((uint32_t)p[6] << 16) | ((uint32_t)p[7] << 24);
    g_tty_cflag = (uint32_t)p[8] | ((uint32_t)p[9] << 8)
                | ((uint32_t)p[10] << 16) | ((uint32_t)p[11] << 24);
    g_tty_lflag = (uint32_t)p[12] | ((uint32_t)p[13] << 8)
                | ((uint32_t)p[14] << 16) | ((uint32_t)p[15] << 24);
    for (int i = 0; i < 32; i++) g_tty_cc[i] = p[17 + i];
    if (flush_input) { tty_len = 0; tty_pos = 0; }   /* TCSAFLUSH */
}

static int64_t impl_read_console(int fd, void *buf, size_t len) {
    if (fd == 0 && fd_is_console(fd)) {
        char *s = (char *)buf;

        if (tty_pos >= tty_len) {
            /* assemble one line (canonical) or one byte (raw) */
            bool canonical = (g_tty_lflag & TTY_L_ICANON) != 0;
            tty_len = 0; tty_pos = 0;
            for (;;) {
                int c = console_getchar();

                /* ERASE: drop the last assembled char (kernel-side line
                 * editing - the consumer never sees the backspace).  */
                if (c == '\b' || c == 0x7f || c == (int)g_tty_cc[CC_VERASE]) {
                    if (tty_len > 0) {
                        tty_len--;
                        if (g_tty_lflag & TTY_L_ECHO) display_puts("\b \b");
                    }
                    continue;
                }
                if (c == '\n' || c == '\r') {
                    if (g_tty_lflag & TTY_L_ECHO) display_putchar('\n');
                    tty_line[tty_len++] = '\n';
                    break;
                }
                /* ^C (INTR): discard the line, echo, SIGINT like Linux */
                if ((g_tty_lflag & TTY_L_ISIG) && c == (int)g_tty_cc[CC_VINTR]) {
                    tty_len = 0; tty_pos = 0;
                    if (g_tty_lflag & TTY_L_ECHO) display_puts("^C\n");
                    if (g_current_proc &&
                        g_current_proc->sigactions[SIGINT].sa_handler != SIG_IGN)
                        g_current_proc->pending_sigs |= (1ull << SIGINT);
                    return -(int64_t)EINTR;
                }
                /* ^D (EOF): end-of-file at line start, flush otherwise */
                if (canonical && c == (int)g_tty_cc[CC_VEOF]) {
                    if (tty_len == 0) return 0;
                    break;                       /* serve partial line */
                }
                /* printable ASCII only, special keys arrive as 16-bit
                 * KEY_* codes and must never leak into the line.     */
                if (c >= 0x20 && c < 0x7f) {
                    if (tty_len < sizeof tty_line - 1) {
                        if (g_tty_lflag & TTY_L_ECHO) display_putchar(c);
                        tty_line[tty_len++] = (char)c;
                    }
                }
                if (!canonical) break;           /* raw: one byte/call */
                /* anything else (NUL, ctrl, KEY_* codes): skip */
            }
        }

        /* serve the assembled line out across reads */
        size_t n = tty_len - tty_pos;
        if (n > len) n = len;
        for (size_t k = 0; k < n; k++) s[k] = tty_line[tty_pos + k];
        tty_pos += n;
        if (tty_pos >= tty_len) { tty_len = 0; tty_pos = 0; }
        return (int64_t)n;
    }
    return (int64_t)vfs_read(fd, buf, len);
}

/* Wayland-ready IPC: sockets / epoll / eventfd / timerfd / signalfd /
 * memfd - the syscall glue (implementations live in fs/kobj.c and
 * fs/kobj_evt.c) */

/* struct sockaddr_un - LINUX layout (musl passes it verbatim) */
typedef struct {
    uint16_t sun_family;
    char     sun_path[108];
} ksa_un_t;

static int64_t impl_socket(int domain, int type, int protocol) {
    return kobj_socket(domain, type, protocol);
}

static int64_t impl_socketpair(int domain, int type, int protocol, int *fds) {
    if (!fds) return -(int64_t)EFAULT;
    int tmp[2];
    int64_t r = kobj_socketpair(domain, type, protocol, tmp);
    if (r < 0) return r;
    fds[0] = tmp[0];
    fds[1] = tmp[1];
    return 0;
}

static int64_t impl_bind(int fd, const void *addr, uint32_t addrlen) {
    return kobj_bind(fd, addr, addrlen);
}

static int64_t impl_listen(int fd, int backlog) {
    return kobj_listen(fd, backlog);
}

static int64_t impl_accept4(int fd, void *addr, uint32_t *addrlen, int flags) {
    return kobj_accept4(fd, addr, addrlen, flags);
}

static int64_t impl_connect(int fd, const void *addr, uint32_t addrlen) {
    return kobj_connect(fd, addr, addrlen);
}

static int64_t impl_shutdown(int fd, int how) {
    return kobj_shutdown(fd, how);
}

static int64_t impl_getsockname(int fd, void *addr, uint32_t *addrlen) {
    return kobj_getsockname(fd, addr, addrlen);
}

static int64_t impl_getpeername(int fd, void *addr, uint32_t *addrlen) {
    return kobj_getpeername(fd, addr, addrlen);
}

static int64_t impl_setsockopt(int fd, int level, int opt,
                               const void *v, uint32_t len) {
    return kobj_setsockopt(fd, level, opt, v, len);
}

static int64_t impl_getsockopt(int fd, int level, int opt,
                               void *v, uint32_t *len) {
    return kobj_getsockopt(fd, level, opt, v, len);
}

static int64_t impl_sendmsg(int fd, const ko_msghdr_t *mh, int flags) {
    return kobj_sendmsg(fd, mh, flags);
}

static int64_t impl_recvmsg(int fd, ko_msghdr_t *mh, int flags) {
    return kobj_recvmsg(fd, mh, flags);
}

/* sendto/recvfrom over connected unix sockets: address args ignored */
static int64_t impl_sendto(int fd, const void *buf, uint64_t len, int flags,
                           const void *addr, uint32_t addrlen) {
    (void)addr; (void)addrlen;
    ko_msghdr_t mh;
    __builtin_memset(&mh, 0, sizeof(mh));
    typedef struct { uint64_t base; uint64_t len; } kiovec_t;
    kiovec_t iov = { (uint64_t)(uintptr_t)buf, len };
    mh.msg_iov = (void *)(uintptr_t)&iov;
    mh.msg_iovlen = 1;
    return kobj_sendmsg(fd, &mh, flags);
}

static int64_t impl_recvfrom(int fd, void *buf, uint64_t len, int flags,
                             void *addr, uint32_t *addrlen) {
    (void)addr; (void)addrlen;
    ko_msghdr_t mh;
    __builtin_memset(&mh, 0, sizeof(mh));
    typedef struct { uint64_t base; uint64_t len; } kiovec_t;
    kiovec_t iov = { (uint64_t)(uintptr_t)buf, len };
    mh.msg_iov = (void *)(uintptr_t)&iov;
    mh.msg_iovlen = 1;
    return kobj_recvmsg(fd, &mh, flags);
}

static int64_t impl_epoll_create1(int flags) {
    return kobj_epoll_create1(flags);
}

static int64_t impl_epoll_ctl(int epfd, int op, int fd, void *ev) {
    return kobj_epoll_ctl(epfd, op, fd, (ko_epoll_event_t *)ev);
}

static int64_t impl_epoll_wait(int epfd, void *events, int maxevents,
                               int timeout) {
    return kobj_epoll_wait(epfd, (ko_epoll_event_t *)events, maxevents, timeout);
}

static int64_t impl_eventfd2(unsigned int initval, int flags) {
    return kobj_eventfd(initval, flags);
}

static int64_t impl_timerfd_create(int clockid, int flags) {
    return kobj_timerfd_create(clockid, flags);
}

static int64_t impl_timerfd_settime(int fd, int flags, const void *newt,
                                    void *oldt) {
    return kobj_timerfd_settime(fd, flags,
                                (const ko_itimerspec_t *)newt,
                                (ko_itimerspec_t *)oldt);
}

static int64_t impl_timerfd_gettime(int fd, void *curr) {
    return kobj_timerfd_gettime(fd, (ko_itimerspec_t *)curr);
}

static int64_t impl_signalfd4(int fd, const uint64_t *mask,
                              size_t sigsetsize, int flags) {
    return kobj_signalfd4(fd, mask, sigsetsize, flags);
}

static int64_t impl_memfd_create(const char *name, int flags) {
    return kobj_memfd_create(name, flags);
}

static int64_t impl_ftruncate(int fd, int64_t length) {
    return kobj_ftruncate(fd, length);
}

static int64_t impl_truncate(const char *path, int64_t length) {
    char buf[VFS_PATH_MAX];
    resolve_path(buf, path);
    int fd = vfs_open(buf, O_WRONLY);
    if (fd < 0) return fd;
    int64_t r = kobj_ftruncate(fd, length);
    vfs_close(fd);
    return r;
}

/* syscall round-out */

static int64_t impl_sched_setaffinity(int pid, size_t sz, const uint64_t *mask) {
    process_t *me = g_current_proc;
    if (!me) return -(int64_t)EINVAL;
    if (pid != 0 && pid != me->tgid) {
        bool ours = false;
        for (int i = 0; i < PROC_MAX; i++) {
            process_t *p = &g_proc_table[i];
            if (p->state != PROC_STATE_EMPTY && p->pid == pid &&
                p->tgid == me->tgid) { ours = true; break; }
        }
        if (!ours) return -(int64_t)ESRCH;
    }
    if (sz < 8) return -(int64_t)EINVAL;
    if (!mask) return -(int64_t)EFAULT;
    if (!*mask) return -(int64_t)EINVAL;
    return 0;   /* single CPU: any non-empty mask collapses to {cpu0} */
}

static int64_t impl_sched_getaffinity(int pid, size_t sz, uint64_t *mask) {
    process_t *me = g_current_proc;
    if (!me) return -(int64_t)EINVAL;
    if (pid != 0 && pid != me->tgid) {
        bool ours = false;
        for (int i = 0; i < PROC_MAX; i++) {
            process_t *p = &g_proc_table[i];
            if (p->state != PROC_STATE_EMPTY && p->pid == pid &&
                p->tgid == me->tgid) { ours = true; break; }
        }
        if (!ours) return -(int64_t)ESRCH;
    }
    if (!mask || sz == 0) return -(int64_t)EINVAL;
    /* Linux returns the number of bytes copied, our 1-CPU mask is one
     * byte, zero-padded to the 8-byte alignment userland expects */
    size_t n = (sz < 8) ? sz : 8;
    uint8_t *b = (uint8_t *)mask;
    for (size_t i = 0; i < n; i++) b[i] = (i == 0) ? 1 : 0;
    return (int64_t)n;
}

/* struct getcpu { unsigned cpu, node; } */
static int64_t impl_getcpu(unsigned *cpu, unsigned *node, void *tcache) {
    (void)tcache;
    if (cpu)  *cpu  = 0;
    if (node) *node = 0;
    return 0;
}

/* mincore - every mapped page is resident in this kernel (eager file
 * fill + anonymous pages), so mapped ranges report all-1s. */
static int64_t impl_mincore(void *addr, size_t len, uint8_t *vec) {
    if (!vec) return -(int64_t)EFAULT;
    if (((uint64_t)(uintptr_t)addr & 0xFFF) || len == 0) return -(int64_t)EINVAL;
    uint64_t start = (uint64_t)(uintptr_t)addr;
    size_t pages = (len + 0xFFF) / 0x1000;
    for (size_t i = 0; i < pages; i++) {
        uint64_t pte = vmm_peek_pte(start + i * 0x1000);
        if (!(pte & 1)) return -(int64_t)ENOMEM;
        vec[i] = 1;
    }
    return 0;
}

/* Deprecated RMM-private syscalls.  They are replaceable by
 * Linux-standard interfaces:
 *
 *   SYS_RMM_PCILIST (515)  -> /proc/bus/pci/devices   (Linux format)
 *   SYS_RMM_FBINFO  (516)  -> /dev/fb0 + FBIOGET_* ioctls, /proc/fb
 *   SYS_RMM_CPUINFO (517)  -> /proc/cpuinfo
 *   SYS_RMM_PUTS    (512)  -> write(1|2, ...)
 *   SYS_RMM_GETCHAR (513)  -> read(0, ...)
 *   SYS_RMM_UPTIME  (514)  -> /proc/uptime, sysconf(_SC_CLK_TCK)
 *
 * They stay only for existing userland (lspci, neofetch) and
 * will be retired once userland has migrated.  Do not use them in new
 * userland. */

/* RMM-private (deprecated, use /proc/bus/pci/devices): PCI table */
static int64_t impl_pci_list(char *buf, int bufsz) {
    if (!buf || bufsz <= 0) return -(int64_t)EFAULT;
    return pci_format_list(buf, bufsz);
}

extern uint32_t g_fb_width, g_fb_height, g_fb_bpp;

/* RMM-private (deprecated, use /dev/fb0 + FBIOGET_* or /proc/fb) */
static int64_t impl_fbinfo(uint32_t *out) {
    if (!out) return -(int64_t)EFAULT;
    out[0] = g_fb_width;
    out[1] = g_fb_height;
    out[2] = g_fb_bpp;
    return 0;
}

/* SYS_RMM_CPUINFO(517) - deprecated: /proc/cpuinfo carries the same
 * brand string (plus the standard fields) since the procfs-lite tree, */
static int64_t impl_cpuinfo(char *buf, int len) {
    if (!buf || len <= 0) return -(int64_t)EFAULT;
    extern const char *cpu_brand_string(void);
    const char *brand = cpu_brand_string();
    int i = 0;
    while (brand[i] && i < len - 1) { buf[i] = brand[i]; i++; }
    buf[i] = '\0';
    return i;
}

/*
 *  Central dispatcher  (called by both syscall stub and INT 0x80 stub)
 */
/*
 *  sched_yield / sysinfo / prctl
 */

/* struct sysinfo (Linux x86-64 layout) */
typedef struct __attribute__((packed)) {
    int64_t      uptime;
    uint64_t     loads[3];
    uint64_t     totalram;
    uint64_t     freeram;
    uint64_t     sharedram;
    uint64_t     bufferram;
    uint64_t     totalswap;
    uint64_t     freeswap;
    uint16_t     procs;
    uint16_t     pad;
    uint64_t     totalhigh;
    uint64_t     freehigh;
    uint32_t     mem_unit;
} linux_sysinfo_t;

static int64_t impl_sysinfo(linux_sysinfo_t *si) {
    if (!si) return -(int64_t)EINVAL;
    extern uint64_t pmm_total_pages(void);
    extern uint64_t pmm_used_pages(void);
    extern uint64_t pit_ticks(void);
    uint64_t total = pmm_total_pages() * 4096ULL;
    uint64_t free_ = (pmm_total_pages() - pmm_used_pages()) * 4096ULL;
    int procs = 0;
    extern process_t g_proc_table[];
    for (int i = 0; i < PROC_MAX; i++)
        if (g_proc_table[i].state != PROC_STATE_EMPTY) procs++;

    linux_sysinfo_t out;
    kmemset(&out, 0, sizeof(out));
    out.uptime    = (int64_t)(pit_ticks() / 1000);
    out.totalram  = total;
    out.freeram   = free_;
    out.procs     = (uint16_t)(procs ? procs : 1);
    out.mem_unit  = 1;
    kmemcpy(si, &out, sizeof(out));
    return 0;
}

/* prctl option subset, everything ported software actually calls.
 * Unknown options get Linux's default -EINVAL. */
#define KPR_SET_NAME          15
#define KPR_GET_NAME          16
#define KPR_GET_DUMPABLE       3
#define KPR_SET_DUMPABLE       4
#define KPR_GET_KEEPCAPS       8
#define KPR_SET_KEEPCAPS       9
#define KPR_GET_TIMERSLACK    30
#define KPR_SET_TIMERSLACK    29
#define KPR_SET_CHILD_SUBREAPER 36
#define KPR_GET_CHILD_SUBREAPER 37
#define KPR_SET_NO_NEW_PRIVS   38
#define KPR_GET_NO_NEW_PRIVS   39

static int64_t impl_prctl(int64_t op, uint64_t a2) {
    process_t *me = g_current_proc;
    if (!me) return -(int64_t)EINVAL;
    switch (op) {
    case KPR_SET_NAME: {
        char name[16];
        kmemset(name, 0, sizeof(name));
        for (int i = 0; i < 16; i++) {
            uint8_t ch = ((uint8_t *)(uintptr_t)a2)[i];   /* user VA read */
            name[i] = (char)ch;
            if (!ch) break;
        }
        for (int i = 0; i < 16; i++) me->comm[i] = name[i];
        me->comm[15] = '\0';
        return 0;
    }
    case KPR_GET_NAME:
        for (int i = 0; i < 16; i++)
            ((uint8_t *)(uintptr_t)a2)[i] = (uint8_t)me->comm[i];
        return 0;
    case KPR_GET_DUMPABLE:        return 1;
    case KPR_SET_DUMPABLE:        return 0;
    case KPR_GET_KEEPCAPS:        return 0;
    case KPR_SET_KEEPCAPS:        return 0;
    case KPR_GET_TIMERSLACK:      return 1;   /* 1 ns - we are not lazy */
    case KPR_SET_TIMERSLACK:      return 0;   /* accept, ignore          */
    case KPR_SET_CHILD_SUBREAPER: return 0;
    case KPR_GET_CHILD_SUBREAPER:
        *(uint32_t *)(uintptr_t)a2 = 0;
        return 0;
    case KPR_SET_NO_NEW_PRIVS:    return 0;
    case KPR_GET_NO_NEW_PRIVS:    return 0;
    default:                      return -(int64_t)EINVAL;
    }
}

int64_t syscall_dispatch_c(uint64_t nr,
                            uint64_t a1, uint64_t a2,
                            uint64_t a3, uint64_t a4,
                            uint64_t a5, uint64_t a6,
                            syscall_frame_t *frame) {
#if SYSCALL_TRACE
    display_printf("[trace] nr=%lu a1=0x%lx a2=0x%lx a3=0x%lx\n",
                    (unsigned long)nr, (unsigned long)a1,
                    (unsigned long)a2, (unsigned long)a3);
#endif
    int64_t ret = -(int64_t)ENOSYS;
#if FORK_DEBUG
    /* compact universal trace: "<pid>:nr " for every syscall */
    display_printf("%d:%lu ", g_current_proc ? g_current_proc->pid : -1,
                   (unsigned long)nr);
#endif
    switch ((int)nr) {
    /* Core I/O */
    case SYS_READ:    ret = impl_read_console((int)a1, (void *)a2, (size_t)a3); break;
    case SYS_WRITE:   ret = impl_write_console((int)a1, (const void *)a2, (size_t)a3); break;
    case SYS_OPEN:    ret = impl_open((const char *)a1, (int)a2, (int)a3); break;
    case SYS_CLOSE:   ret = (int64_t)vfs_close((int)a1); break;
    case SYS_STAT:    ret = impl_stat((const char *)a1, (linux_stat_t *)a2); break;
    case SYS_FSTAT:   ret = impl_fstat((int)a1, (linux_stat_t *)a2); break;
    case SYS_LSTAT:   ret = impl_lstat((const char *)a1, (linux_stat_t *)a2); break;
    case SYS_LSEEK:   ret = impl_lseek((int)a1, (int64_t)a2, (int)a3); break;
    case SYS_MMAP:    ret = impl_mmap(a1, a2, (int)a3, (int)a4, (int)a5, a6); break;
    case SYS_MUNMAP:  ret = impl_munmap(a1, a2); break;
    case SYS_MREMAP:  ret = impl_mremap(a1, a2, a3, a4, a5); break;
    case SYS_BRK:     ret = (int64_t)proc_brk(a1); break;
    case SYS_READV:   ret = impl_readv((int)a1, (const void *)a2, (int)a3); break;
    case SYS_WRITEV:  ret = impl_writev((int)a1, (const void *)a2, (int)a3); break;
    case SYS_ACCESS:  ret = impl_access((const char *)a1, (int)a2); break;
    case SYS_PIPE:    ret = impl_pipe((int *)a1); break;
    case SYS_DUP:     ret = impl_dup((int)a1); break;
    case SYS_DUP2:    ret = impl_dup2((int)a1, (int)a2); break;
    case SYS_DUP3:    ret = impl_dup3((int)a1, (int)a2, (int)a3); break;
    case SYS_FCNTL:   ret = impl_fcntl((int)a1, (int)a2, a3); break;
    case SYS_IOCTL:   ret = impl_ioctl((int)a1, a2, a3); break;
    case SYS_PREAD64:  ret = impl_pread64((int)a1, (void *)a2, a3, a4); break;
    case SYS_PWRITE64: ret = impl_pwrite64((int)a1, (const void *)a2, a3, a4); break;
    case SYS_FCHDIR:   ret = impl_fchdir((int)a1); break;
    case SYS_FCHMOD:   ret = impl_fchmod((int)a1, (uint32_t)a2); break;
    case SYS_FCHOWN:   ret = impl_fchown((int)a1, (uint32_t)a2, (uint32_t)a3); break;

    /* Memory misc */
    case SYS_MPROTECT: {
        /* r5: real enforcement - PROT_NONE guard pages + NX toggle.
         * W is preserved (no SMAP, kernel writes through user mappings).
         * See vmm.c vmm_mprotect() for the full design note. */
        ret = vmm_mprotect((uint64_t)a1, (size_t)a2, (int)a3);
        break;
    }
    case SYS_MSYNC:    ret = 0; break;
    case SYS_MADVISE:  ret = 0; break;   /* mallocng calls this on free */

    /* Directory */
    case SYS_GETDENTS64: ret = impl_getdents64((int)a1, (void *)a2, (size_t)a3); break;
    case SYS_GETDENTS:   ret = impl_getdents64((int)a1, (void *)a2, (size_t)a3); break;

    /* tee(2): Linux only tees between pipes and returns EINVAL otherwise.
     * Our console (and any non-pipe fd) therefore reports EINVAL, which is
     * exactly what dash expects to fall back to plain read().  We do not
     * implement pipe->pipe tee yet (would need non-destructive pipe peek). */
    case SYS_TEE:        ret = -(int64_t)EINVAL; break;

    /* Filesystem */
    case SYS_GETCWD:     ret = impl_getcwd((char *)a1, (size_t)a2); break;
    case SYS_CHDIR:      ret = impl_chdir((const char *)a1); break;
    case SYS_MKDIR:      ret = impl_mkdir((const char *)a1, (int)a2); break;
    case SYS_MKDIRAT:    ret = impl_mkdirat((int)a1, (const char *)a2, (int)a3); break;
    case SYS_MKNODAT:    ret = impl_mknodat((int)a1, (const char *)a2, (uint32_t)a3, a4); break;
    case SYS_RMDIR:      ret = impl_rmdir((const char *)a1); break;
    case SYS_UNLINK:     ret = impl_unlink((const char *)a1); break;
    case SYS_READLINK:   ret = impl_readlink((const char *)a1, (char *)a2, (size_t)a3); break;
    case SYS_CHMOD:      ret = impl_chmod((const char *)a1, (uint32_t)a2); break;
    case SYS_FCHMODAT:   ret = impl_fchmodat((int)a1, (const char *)a2, (uint32_t)a3, (int)a4); break;
    case SYS_CHOWN:      ret = impl_chown((const char *)a1, (uint32_t)a2, (uint32_t)a3); break;
    case SYS_LCHOWN:     ret = impl_chown((const char *)a1, (uint32_t)a2, (uint32_t)a3); break; /* no symlinks */
    case SYS_FCHOWNAT:   ret = impl_fchownat((int)a1, (const char *)a2, (uint32_t)a3, (uint32_t)a4, (int)a5); break;
    case SYS_SYMLINK:    ret = impl_symlink((const char *)a1, (const char *)a2); break;
    case SYS_MKNOD:      ret = impl_mknod((const char *)a1, (uint32_t)a2, a3); break;
    case SYS_UTIMENSAT:  ret = impl_utimensat((int)a1, (const char *)a2, (const void *)a3, (int)a4); break;
    case SYS_MOUNT:      ret = impl_mount((const char *)a1, (const char *)a2, (const char *)a3, a4, (const void *)a5); break;
    case SYS_UMOUNT2:    ret = impl_umount2((const char *)a1, (int)a2); break;
    case SYS_CREAT:      ret = impl_open((const char *)a1, O_CREAT|O_WRONLY|O_TRUNC, (int)a2); break;
    case SYS_OPENAT:     ret = impl_openat((int)a1, (const char *)a2, (int)a3, (int)a4); break;
    case SYS_FSTATAT:    ret = impl_fstatat((int)a1, (const char *)a2, (linux_stat_t *)a3, (int)a4); break;
    case SYS_STATX:      ret = impl_statx((int)a1, (const char *)a2, (int)a3, (uint32_t)a4, (linux_statx_t *)a5); break;
    case SYS_UNLINKAT:   ret = impl_unlinkat((int)a1, (const char *)a2, (int)a3); break;
    case SYS_FACCESSAT:  ret = impl_faccessat((int)a1, (const char *)a2, (int)a3, (int)a4); break;
    case SYS_READLINKAT: ret = impl_readlinkat((int)a1, (const char *)a2, (char *)a3, (size_t)a4); break;
    case SYS_RENAME:     ret = impl_rename((const char *)a1, (const char *)a2); break;
    case SYS_RENAMEAT:   ret = impl_renameat((int)a1, (const char *)a2, (int)a3, (const char *)a4); break;
    case SYS_RENAMEAT2:  ret = impl_renameat2((int)a1, (const char *)a2, (int)a3, (const char *)a4, (unsigned int)a5); break;

    /* Phase-1 completeness */
    case SYS_STATFS:         ret = impl_statfs((const char *)a1, (kstatfs_t *)a2); break;
    case SYS_FSTATFS:        ret = impl_fstatfs((int)a1, (kstatfs_t *)a2); break;
    case SYS_SENDFILE:       ret = impl_sendfile((int)a1, (int)a2, (int64_t *)a3, (size_t)a4); break;
    case SYS_COPY_FILE_RANGE: ret = impl_copy_file_range((int)a1, (int64_t *)a2, (int)a3, (int64_t *)a4, (size_t)a5, (unsigned)a6); break;
    case SYS_UMASK: {
        uint32_t old = g_current_proc ? g_current_proc->umask : 0022;
        if (g_current_proc) g_current_proc->umask = (uint32_t)a1 & 0777;
        ret = old; break;
    }

    /* Process */
    case SYS_FORK:
#if FORK_DEBUG
        display_printf("[fd] fork: pid=%d rcx=0x%lx rax=0x%lx rsp=0x%lx\n",
                       g_current_proc ? g_current_proc->pid : -1,
                       (unsigned long)frame->rcx, (unsigned long)frame->rax,
                       (unsigned long)frame->user_rsp);
#endif
        ret = (int64_t)proc_fork(frame); break;
    case SYS_CLONE: {
        /*
         * clone(flags, stack, parent_tid, child_tid, tls) - x86-64 ABI.
         * CLONE_VM|CLONE_THREAD creates a real thread sharing the
         * address space and (CLONE_FILES) the fd table, fork-like flag
         * sets fall through to proc_fork (musl fork = clone(SIGCHLD,0)).
         */
        ret = (int64_t)proc_clone(frame);
        break;
    }
    case SYS_VFORK:     ret = (int64_t)proc_fork(frame); break;
    case SYS_EXECVE:
#if FORK_DEBUG
        {
            int da = 0, de = 0;
            if (a2) while (((char *const *)(uintptr_t)a2)[da]) da++;
            if (a3) while (((char *const *)(uintptr_t)a3)[de]) de++;
            display_printf("[fd] execve: pid=%d path=%s argc=%d a2=0x%lx a2[0]=0x%lx a2[1]=0x%lx rsp=0x%lx\n",
                           g_current_proc ? g_current_proc->pid : -1,
                           (const char *)a1, da,
                           (unsigned long)a2,
                           da > 0 ? (unsigned long)((uint64_t *)(uintptr_t)a2)[0] : 0ul,
                           da > 1 ? (unsigned long)((uint64_t *)(uintptr_t)a2)[1] : 0ul,
                           frame ? (unsigned long)frame->user_rsp : 0ul);
        }
#endif
        ret = (int64_t)proc_execve((const char *)a1,
                                     (char *const *)a2,
                                     (char *const *)a3, frame);
#if FORK_DEBUG
        display_printf("[fd] execve ret=%ld new_rip=0x%lx rsp=0x%lx\n",
                       (long)ret, (unsigned long)frame->rcx,
                       (unsigned long)frame->user_rsp);
#endif
        break;
    case SYS_EXIT:
    case SYS_EXIT_GROUP:
#if FORK_DEBUG
        display_printf("[fd] exit: pid=%d code=%ld rcx=0x%lx\n",
                       g_current_proc ? g_current_proc->pid : -1,
                       (long)a1, (unsigned long)frame->rcx);
#endif
        /* proc_exit routes CLONE_THREAD tasks to proc_thread_exit
         * (auto-reap, no zombie) and leaders to group teardown first. */
        if (g_current_proc) proc_exit(g_current_proc, (int)a1);
        ret = 0; break;
    case SYS_WAIT4:
#if FORK_DEBUG
        display_printf("[fd] wait4: pid=%d wp=%ld rcx=0x%lx\n",
                       g_current_proc ? g_current_proc->pid : -1,
                       (long)a1, (unsigned long)frame->rcx);
#endif
        ret = (int64_t)proc_waitpid((int)a1, (int *)a2, (int)a4, frame);
#if FORK_DEBUG
        display_printf("[fd] wait4 ret=%ld rip=0x%lx\n", (long)ret,
                       (unsigned long)frame->rcx);
#endif
        break;
    case SYS_WAITID:
        ret = -(int64_t)EINVAL; break;  /* not needed by dash */
    /* getpid returns the thread group id, gettid the kernel tid */
    case SYS_GETPID:    ret = g_current_proc
                            ? (g_current_proc->tgid ? g_current_proc->tgid
                                                    : g_current_proc->pid)
                            : 1; break;
    case SYS_GETPPID:   ret = g_current_proc ? g_current_proc->parent_pid : 0; break;
    case SYS_GETTID:    ret = g_current_proc ? g_current_proc->pid : 1; break;
    case SYS_GETUID:    ret = g_current_proc ? g_current_proc->uid  : 0; break;
    case SYS_GETGID:    ret = g_current_proc ? g_current_proc->gid  : 0; break;
    case SYS_GETEUID:   ret = g_current_proc ? g_current_proc->euid : 0; break;
    case SYS_GETEGID:   ret = g_current_proc ? g_current_proc->egid : 0; break;
    case SYS_SETUID:    ret = impl_setuid((int)a1); break;
    case SYS_SETGID:    ret = impl_setgid((int)a1); break;
    case SYS_SETREUID:  ret = impl_setreuid((int)a1, (int)a2); break;
    case SYS_SETREGID:  ret = impl_setregid((int)a1, (int)a2); break;
    case SYS_SETRESUID: ret = impl_setresuid((int)a1, (int)a2, (int)a3); break;
    case SYS_GETRESUID: ret = impl_getresuid((uint32_t *)a1, (uint32_t *)a2, (uint32_t *)a3); break;
    case SYS_SETRESGID: ret = impl_setresgid((int)a1, (int)a2, (int)a3); break;
    case SYS_GETRESGID: ret = impl_getresgid((uint32_t *)a1, (uint32_t *)a2, (uint32_t *)a3); break;
    case SYS_SETGROUPS: ret = impl_setgroups((int)a1, (const uint32_t *)a2); break;
    case SYS_GETGROUPS: ret = impl_getgroups((int)a1, (uint32_t *)a2); break;
    case SYS_GETPGRP:   ret = g_current_proc
                            ? (g_current_proc->pgid ? g_current_proc->pgid : g_current_proc->pid)
                            : 1; break;
    case SYS_GETPGID: {
        /* getpgid(pid) - musl's getpgrp() calls __syscall(SYS_getpgid, 0),
         * Not SYS_getpgrp!  Missing here, dash's job-control init looped
         * forever on killpg(SIGTTIN) (ignored by default) because
         * tcgetpgrp() == pid never equalled getpgrp() == -1.             */
        int qpid = (int)a1;
        if (!g_current_proc) { ret = -(int64_t)ESRCH; break; }
        if (qpid == 0 || qpid == g_current_proc->pid) {
            ret = g_current_proc->pgid ? g_current_proc->pgid
                                       : g_current_proc->pid;
            break;
        }
        ret = -(int64_t)ESRCH;
        for (int i = 0; i < PROC_MAX; i++) {
            process_t *p = &g_proc_table[i];
            if (p->state == PROC_STATE_EMPTY || p->pid != qpid) continue;
            ret = p->pgid ? p->pgid : p->pid;
            break;
        }
        break;
    }
    case SYS_SETPGID: {
        int tpid  = (int)a1;
        int tpgid = (int)a2;
        if (!g_current_proc) { ret = -(int64_t)ESRCH; break; }
        /* Only handle self for now */
        if (tpid == 0 || tpid == g_current_proc->pid) {
            g_current_proc->pgid = (tpgid == 0) ? g_current_proc->pid : tpgid;
        }
        ret = 0; break;
    }
    case SYS_SETSID: {
        /* Create a new session: sid = pgid = pid, detach from old */
        if (!g_current_proc) { ret = -(int64_t)ESRCH; break; }
        g_current_proc->sid   = g_current_proc->pid;
        g_current_proc->pgid  = g_current_proc->pid;
        g_current_proc->fg_pgrp = 0;
        ret = (int64_t)g_current_proc->pid; break;
    }
    case SYS_GETSID: {
        if (!g_current_proc) { ret = -(int64_t)ESRCH; break; }
        ret = g_current_proc->sid ? g_current_proc->sid : g_current_proc->pid; break;
    }
    case SYS_KILL: {
        /*
         * kill(pid, sig) - delivers to any live process by pid.  The
         * cooperative model fires the signal at the target's next
         * syscall return (proc_deliver_signal in the dispatcher tail).
         * pid 0 -> current process group, pid -1 -> all live processes.
         */
        int kpid = (int)a1;
        int ksig = (int)a2;
        /* signal 64 is accepted by sigaction (sigset space) but
         * cannot sit in the pending bitmask (bit N = signal N in u64) */
        if (ksig < 0 || ksig > 63) { ret = -(int64_t)EINVAL; break; }
        bool any = false;
        for (int i = 0; i < PROC_MAX; i++) {
            process_t *p = &g_proc_table[i];
            if (p->state == PROC_STATE_EMPTY) continue;
            bool match =
                (kpid == p->pid) ||
                (kpid == 0  && g_current_proc && p->pgid == g_current_proc->pgid) ||
                (kpid == -1);
            if (!match) continue;
            if (ksig == 0) { any = true; if (kpid == p->pid) break; continue; }
            p->pending_sigs |= (1ull << ksig);
            /* (BORE): wake parked targets so the signal reaches
             * their syscall tail promptly (Ctrl+C on a sleeping task). */
            sched_wake(p);
            any = true;
            if (kpid == p->pid) break;
        }
        ret = any ? 0 : -(int64_t)ESRCH;
        break;
    }

    case SYS_TKILL: {                 /* tkill(tid, sig): tid==pid here */
        int tpid = (int)a1, tsig = (int)a2;
        if (tsig < 1 || tsig > 63) { ret = -(int64_t)EINVAL; break; }
        bool any = false;
        for (int i = 0; i < PROC_MAX; i++) {
            process_t *p = &g_proc_table[i];
            if (p->state == PROC_STATE_EMPTY || p->pid != tpid) continue;
            p->pending_sigs |= (1ull << tsig);
            any = true;
            break;
        }
        ret = any ? 0 : -(int64_t)ESRCH;
        break;
    }
    case SYS_TGKILL: {                /* tgkill(tgid, tid, sig): tid==pid */
        int tpid = (int)a2, tsig = (int)a3;
        if (tsig < 1 || tsig > 63) { ret = -(int64_t)EINVAL; break; }
        bool any = false;
        for (int i = 0; i < PROC_MAX; i++) {
            process_t *p = &g_proc_table[i];
            if (p->state == PROC_STATE_EMPTY || p->pid != tpid) continue;
            p->pending_sigs |= (1ull << tsig);
            any = true;
            break;
        }
        ret = any ? 0 : -(int64_t)ESRCH;
        break;
    }

    /* Signals */
    case SYS_RT_SIGACTION: {
        /*
         * rt_sigaction(sig, new_act, old_act, sigsetsize)
         *  a1=signum  a2=new linux_sigaction_t*  a3=old linux_sigaction_t*  a4=sigsetsize
         */
        int sig = (int)a1;
        const linux_sigaction_t *nact = (const linux_sigaction_t *)a2;
        linux_sigaction_t       *oact = (linux_sigaction_t *)a3;
        if (sig < 1 || sig >= NSIG) { ret = -(int64_t)EINVAL; break; }
        if (!g_current_proc) { ret = -(int64_t)ESRCH; break; }
        if (oact) *oact = g_current_proc->sigactions[sig];
        if (nact)  g_current_proc->sigactions[sig] = *nact;
        ret = 0;
        break;
    }
    case SYS_RT_SIGPROCMASK: {
        /*
         * rt_sigprocmask(how, new_set, old_set, sigsetsize)
         * how: 0=block 1=unblock 2=SETMASK.  The mask is honoured by the
         * cooperative delivery loop below (masked signals stay pending)
         * and consumed by signalfd readers.
         */
        int how = (int)a1;
        const uint64_t *newset = (const uint64_t *)a2;
        uint64_t *oldset = (uint64_t *)a3;
        size_t sigsetsize = (size_t)a4;
        if (sigsetsize < 8) { ret = -(int64_t)EINVAL; break; }
        if (!g_current_proc) { ret = -(int64_t)ESRCH; break; }
        /* userspace sigsets number signal N as bit N-1 (musl/glibc);
         * the kernel keeps pending_sigs/sig_mask as bit N - translate */
        if (oldset) *oldset = g_current_proc->sig_mask >> 1;
        if (newset) {
            /* full 64-bit mask (signals 1..63; user bit N-1 <->
             * kernel bit N) */
            uint64_t m = ((*newset) << 1) & 0xFFFFFFFFFFFFFFFEull;
            switch (how) {
            case 0: g_current_proc->sig_mask |=  m; break;
            case 1: g_current_proc->sig_mask &= ~m; break;
            case 2: g_current_proc->sig_mask  =  m; break;
            default: ret = -(int64_t)EINVAL; break;
            }
        }
        if (ret != -(int64_t)EINVAL) ret = 0;
        break;
    }
    case SYS_RT_SIGRETURN: {
        /*
         * Restore the full register context pushed by proc_deliver_signal.
         * After musl's handler returns, its restorer stub issues this syscall
         * with user_rsp pointing exactly at the 128-byte saved context.
         * ctx[6] (original rax) is returned so the entry stub re-patches the
         * saved-rax slot with the pre-signal syscall return value.
         */
        if (frame && g_current_proc) {
            uint64_t *ctx = (uint64_t *)(uintptr_t)frame->user_rsp;
            frame->r9       = ctx[0];
            frame->r8       = ctx[1];
            frame->r10      = ctx[2];
            frame->rdx      = ctx[3];
            frame->rsi      = ctx[4];
            frame->rdi      = ctx[5];
            frame->r15      = ctx[7];
            frame->r14      = ctx[8];
            frame->r13      = ctx[9];
            frame->r12      = ctx[10];
            frame->rbx      = ctx[11];
            frame->rbp      = ctx[12];
            frame->r11      = ctx[13];   /* user rflags */
            frame->rcx      = ctx[14];   /* user rip    */
            frame->user_rsp = ctx[15];
            ret = (int64_t)ctx[6];       /* original syscall rax */
        } else {
            ret = 0;
        }
        break;
    }
    case SYS_SIGALTSTACK:     ret = 0; break;

    /* System info */
    case SYS_UNAME:          ret = impl_uname((linux_utsname_t *)a1); break;
    case SYS_SETHOSTNAME:    ret = impl_sethostname((const char *)(uintptr_t)a1,
                                                    (size_t)a2); break;
    case SYS_ARCH_PRCTL:     ret = impl_arch_prctl((int)a1, a2); break;
    case SYS_SET_TID_ADDRESS: ret = impl_set_tid_address((int *)a1); break;
    case SYS_CLOCK_GETTIME:  ret = impl_clock_gettime((int)a1, (linux_timespec_t *)a2); break;
    case SYS_CLOCK_GETRES:  {
        linux_timespec_t *ts = (linux_timespec_t *)a2;
        if (ts) { ts->tv_sec = 0; ts->tv_nsec = 1000000; }
        ret = 0; break;
    }
    case SYS_GETTIMEOFDAY:  ret = impl_gettimeofday((linux_timeval_t *)a1, (void *)a2); break;
    case SYS_TIME: {
        linux_timespec_t ts;
        impl_clock_gettime(0, &ts);
        if (a1) *(uint64_t *)a1 = ts.tv_sec;
        ret = ts.tv_sec; break;
    }
    case SYS_TIMES: {
        /* struct tms: tms_utime, tms_stime, tms_cutime, tms_cstime */
        /* fake with 0 for now */
        if (a1) kmemset((void *)a1, 0, 32);
        extern uint64_t pit_ticks(void);
        ret = (int64_t)pit_ticks(); break;
    }
    case SYS_SYSCONF: {
        /* return fake sysconf values */
        /* _SC_CLK_TCK is 2, value typically 100 on linux */
        /* _SC_PAGESIZE is 30, value 4096 */
        int name = (int)a1;
        if (name == 2) ret = 1000;
        else if (name == 30) ret = 4096;
        else ret = -(int64_t)EINVAL;
        break;
    }

    /* poll / select */
    case SYS_POLL: {
        /*
         * poll(fds, nfds, timeout), real readiness via vfs_poll_ready():
         * pipes report buffer state, sockets/eventfds/timerfds report
         * their object state, the console reports pending input.  When
         * nothing is ready and timeout != 0 we idle with sti/hlt/cli and
         * reprobe (the cooperative analogue of blocking in the poll
         * wait queue).  Timeout is in milliseconds; -1 blocks forever.
         */
        linux_pollfd_t *fds  = (linux_pollfd_t *)a1;
        int             nfds = (int)a2;
        int             timeout = (int)a3;
        extern uint64_t pit_ticks(void);

        uint64_t deadline = (timeout > 0) ? (pit_ticks() + (uint64_t)timeout) : 0;

        for (;;) {
            int ready = 0;
            for (int i = 0; i < nfds; i++) {
                fds[i].revents = 0;
                if (fds[i].fd < 0) continue;
                int16_t rev = (int16_t)vfs_poll_ready(fds[i].fd, fds[i].events);
                fds[i].revents = rev;
                if (rev) ready++;
            }
            if (ready > 0) { ret = (int64_t)ready; break; }
            if (timeout == 0) { ret = 0; break; }
            if (timeout > 0 && pit_ticks() >= deadline) { ret = 0; break; }
            /* (BORE): switchable park (was: sti/hlt/cli of the
             * only kernel context) so poll no longer blocks other tasks */
            if (sched_kernel_park(BORE_EVENT_POLL_MS) != 0)
                __asm__ volatile("sti; hlt; cli");
        }
        break;
    }
    case SYS_SELECT: {
        /* select(nfds, readfds, writefds, exceptfds, timeout)
         * Just report that readfds[0] (stdin) is always ready. */
        int     nfds    = (int)a1;
        uint8_t *rfds   = (uint8_t *)a2;
        uint8_t *wfds   = (uint8_t *)a3;
        (void)nfds;
        if (rfds) rfds[0] |= 1;   /* fd 0 (stdin) readable */
        if (wfds) wfds[0] |= 6;   /* fd 1,2 (stdout/stderr) writable */
        ret = 1; break;
    }

    /* alarm / itimer */
    case SYS_ALARM: {
        /*
         * alarm(seconds) - returns seconds remaining from previous alarm.
         * We convert to PIT milliseconds and store in process.
         */
        extern uint64_t pit_ticks(void);
        uint32_t secs = (uint32_t)a1;
        uint64_t remaining = 0;
        if (g_current_proc) {
            uint64_t now = pit_ticks();
            if (g_current_proc->alarm_deadline_ms > now)
                remaining = (g_current_proc->alarm_deadline_ms - now + 999) / 1000;
            g_current_proc->alarm_deadline_ms = secs ? (now + (uint64_t)secs * 1000) : 0;
        }
        ret = (int64_t)remaining; break;
    }
    case SYS_SETITIMER: {
        /*
         * setitimer(which, new_value, old_value)
         * We only support ITIMER_REAL (which==0).
         * Stores the deadline in the process, old_value set to zero.
         */
        int which = (int)a1;
        const linux_itimerval_t *nval = (const linux_itimerval_t *)a2;
        linux_itimerval_t       *oval = (linux_itimerval_t *)a3;
        if (oval) { oval->it_interval.tv_sec = 0; oval->it_interval.tv_usec = 0;
                    oval->it_value.tv_sec    = 0; oval->it_value.tv_usec    = 0; }
        if (which == 0 && nval && g_current_proc) {
            extern uint64_t pit_ticks(void);
            uint64_t ms = (uint64_t)nval->it_value.tv_sec * 1000
                        + (uint64_t)nval->it_value.tv_usec / 1000;
            g_current_proc->alarm_deadline_ms = ms ? (pit_ticks() + ms) : 0;
        }
        ret = 0; break;
    }
    case SYS_GETITIMER: {
        linux_itimerval_t *oval = (linux_itimerval_t *)a2;
        if (oval && g_current_proc) {
            extern uint64_t pit_ticks(void);
            uint64_t now = pit_ticks();
            uint64_t rem_ms = (g_current_proc->alarm_deadline_ms > now)
                            ? (g_current_proc->alarm_deadline_ms - now) : 0;
            oval->it_value.tv_sec    = (int64_t)(rem_ms / 1000);
            oval->it_value.tv_usec   = (int64_t)((rem_ms % 1000) * 1000);
            oval->it_interval.tv_sec = 0;
            oval->it_interval.tv_usec = 0;
        }
        ret = 0; break;
    }
    case SYS_NANOSLEEP: {
        /*
         * nanosleep(req, rem) - (BORE): the task is parked with a
         * wake deadline and the scheduler runs other tasks in the
         * meantime (was: a full-halt sti/hlt/cli busy-wait that froze
         * the whole system for the sleep duration).
         */
        const linux_timespec_t *req = (const linux_timespec_t *)a1;
        linux_timespec_t       *rem = (linux_timespec_t *)a2;
        if (req) {
            uint64_t ms = (uint64_t)req->tv_sec * 1000
                        + (uint64_t)(req->tv_nsec / 1000000);
            uint64_t end = pit_ticks() + ms;
            while (pit_ticks() < end) {
                uint64_t left = end - pit_ticks();
                if (sched_kernel_park(left > 20 ? 20 : left) != 0)
                    __asm__ volatile("sti; hlt; cli");
            }
        }
        if (rem) { rem->tv_sec = 0; rem->tv_nsec = 0; }
        ret = 0; break;
    }

    /* getrandom (PIT-tick entropy, musl stack guard etc.) */
    case SYS_GETRANDOM: {
        uint8_t *out = (uint8_t *)(uintptr_t)a1;
        size_t   cnt = (size_t)a2;
        extern uint64_t pit_ticks(void);
        uint64_t state = pit_ticks() * 2654435761ULL + 0x9E3779B97F4A7C15ULL;
        for (size_t i = 0; i < cnt; i++) {
            if ((i & 7) == 0) {
                state ^= state << 13; state ^= state >> 7; state ^= state << 17;
                state += pit_ticks();
            }
            out[i] = (uint8_t)(state >> ((i & 7) * 8));
        }
        ret = (int64_t)cnt; break;
    }

    /* resource limits / rusage: stubs so dash/musl degrade cleanly */
    case SYS_GETRUSAGE: {
        if (a1) kmemset((void *)a1, 0, 144);
        ret = 0; break;
    }
    case SYS_GETRLIMIT:
    case SYS_SETRLIMIT:
    case SYS_PRLIMIT64:
        ret = -(int64_t)ENOSYS; break;

    /* futex: musl lock support */
    case SYS_SCHED_YIELD: {
        sched_yield_from_syscall(frame);   /* switches away, or returns */
        ret = 0; break;
    }
    case SYS_SYSINFO:  ret = impl_sysinfo((linux_sysinfo_t *)(uintptr_t)a1); break;
    case SYS_PRCTL:    ret = impl_prctl((int64_t)a1, a2); break;
    case SYS_FUTEX: {
        uint32_t *uaddr = (uint32_t *)(uintptr_t)a1;
        int op = (int)a2;
        uint32_t val = (uint32_t)a3;
        const linux_timespec_t *fts = (const linux_timespec_t *)a4;
        uint64_t tmo_ms = 0;
        if (fts && ((op & 0x7F) == 0 || (op & 0x7F) == 9))
            tmo_ms = (uint64_t)fts->tv_sec * 1000 + (uint64_t)(fts->tv_nsec / 1000000);
        /* a5 = val2 (timespec-as-ulong for REQUEUE), a6 = uaddr2 */
        ret = impl_futex(uaddr, op, val, tmo_ms, (uint32_t)a5,
                         (uint32_t *)(uintptr_t)a6);
        break;
    }
    case SYS_SET_ROBUST_LIST:  ret = 0; break;   /* musl TLS bookkeeping */
    case SYS_GET_ROBUST_LIST:  ret = -(int64_t)ENOSYS; break;

    /* pipe2 */
    case SYS_PIPE2:  ret = impl_pipe((int *)a1); break;  /* flags ignored */

    /* AF_UNIX sockets (Wayland transport) */
    case SYS_SOCKET:      ret = impl_socket((int)a1, (int)a2, (int)a3); break;
    case SYS_SOCKETPAIR:  ret = impl_socketpair((int)a1, (int)a2, (int)a3, (int *)a4); break;
    case SYS_BIND:        ret = impl_bind((int)a1, (const void *)a2, (uint32_t)a3); break;
    case SYS_LISTEN:      ret = impl_listen((int)a1, (int)a2); break;
    case SYS_ACCEPT:      ret = impl_accept4((int)a1, (void *)a2, (uint32_t *)a3, 0); break;
    case SYS_ACCEPT4:     ret = impl_accept4((int)a1, (void *)a2, (uint32_t *)a3, (int)a4); break;
    case SYS_CONNECT:     ret = impl_connect((int)a1, (const void *)a2, (uint32_t)a3); break;
    case SYS_SHUTDOWN:    ret = impl_shutdown((int)a1, (int)a2); break;
    case SYS_GETSOCKNAME: ret = impl_getsockname((int)a1, (void *)a2, (uint32_t *)a3); break;
    case SYS_GETPEERNAME: ret = impl_getpeername((int)a1, (void *)a2, (uint32_t *)a3); break;
    case SYS_SETSOCKOPT:  ret = impl_setsockopt((int)a1, (int)a2, (int)a3, (const void *)a4, (uint32_t)a5); break;
    case SYS_GETSOCKOPT:  ret = impl_getsockopt((int)a1, (int)a2, (int)a3, (void *)a4, (uint32_t *)a5); break;
    case SYS_SENDMSG:     ret = impl_sendmsg((int)a1, (const ko_msghdr_t *)a2, (int)a3); break;
    case SYS_RECVMSG:     ret = impl_recvmsg((int)a1, (ko_msghdr_t *)a2, (int)a3); break;
    case SYS_SENDTO:      ret = impl_sendto((int)a1, (const void *)a2, a3, (int)a4, (const void *)a5, (uint32_t)a6); break;
    case SYS_RECVFROM:    ret = impl_recvfrom((int)a1, (void *)a2, a3, (int)a4, (void *)a5, (uint32_t *)a6); break;

    /* epoll / eventfd / timerfd / signalfd (event loop family) */
    case SYS_EPOLL_CREATE1:   ret = impl_epoll_create1((int)a1); break;
    case SYS_EPOLL_CTL:       ret = impl_epoll_ctl((int)a1, (int)a2, (int)a3, (void *)a4); break;
    case SYS_EPOLL_WAIT:      ret = impl_epoll_wait((int)a1, (void *)a2, (int)a3, (int)a4); break;
    case SYS_EPOLL_PWAIT:     ret = impl_epoll_wait((int)a1, (void *)a2, (int)a3, (int)a4); break;  /* sigmask ignored */
    case SYS_EVENTFD2:        ret = impl_eventfd2((unsigned)a1, (int)a2); break;
    case SYS_TIMERFD_CREATE:  ret = impl_timerfd_create((int)a1, (int)a2); break;
    case SYS_TIMERFD_SETTIME: ret = impl_timerfd_settime((int)a1, (int)a2, (const void *)a3, (void *)a4); break;
    case SYS_TIMERFD_GETTIME: ret = impl_timerfd_gettime((int)a1, (void *)a2); break;
    case SYS_SIGNALFD4:       ret = impl_signalfd4((int)a1, (const uint64_t *)a2, (size_t)a3, (int)a4); break;

    /* memfd / truncate (shared-memory wl_shm pools) */
    case SYS_MEMFD_CREATE: ret = impl_memfd_create((const char *)a1, (int)a2); break;
    case SYS_FTRUNCATE:    ret = impl_ftruncate((int)a1, (int64_t)a2); break;
    case SYS_TRUNCATE:     ret = impl_truncate((const char *)a1, (int64_t)a2); break;

    /* RMM-private */
    /* round-out */
    case SYS_RT_SIGTIMEDWAIT:
        ret = sys_rt_sigtimedwait((const uint64_t *)a1, (void *)a2,
                                  (const linux_timespec_t *)a3, (size_t)a4);
        break;
    case SYS_MINCORE:           ret = impl_mincore((void *)a1, (size_t)a2, (uint8_t *)a3); break;
    case SYS_FLOCK:             ret = sys_flock((int)a1, (int)a2); break;
    case SYS_SCHED_SETAFFINITY: ret = impl_sched_setaffinity((int)a1, (size_t)a2, (const uint64_t *)a3); break;
    case SYS_SCHED_GETAFFINITY: ret = impl_sched_getaffinity((int)a1, (size_t)a2, (uint64_t *)a3); break;
    case SYS_TIMER_CREATE:      ret = sys_timer_create((int32_t)a1, (const ksigevent_t *)a2, (int32_t *)a3); break;
    case SYS_TIMER_SETTIME:     ret = sys_timer_settime((int32_t)a1, (int32_t)a2, (const kitimerspec_t *)a3, (kitimerspec_t *)a4); break;
    case SYS_TIMER_GETTIME:     ret = sys_timer_gettime((int32_t)a1, (kitimerspec_t *)a2); break;
    case SYS_TIMER_GETOVERRUN:  ret = sys_timer_getoverrun((int32_t)a1); break;
    case SYS_TIMER_DELETE:      ret = sys_timer_delete((int32_t)a1); break;
    case SYS_GETCPU:            ret = impl_getcpu((unsigned *)a1, (unsigned *)a2, (void *)a3); break;
    case SYS_FACCESSAT2:        ret = impl_faccessat2((int)a1, (const char *)a2, (int)a3, (int)a4); break;

    case SYS_RMM_PCILIST: ret = impl_pci_list((char *)a1, (int)a2); break;
    case SYS_RMM_FBINFO:  ret = impl_fbinfo((uint32_t *)a1); break;
    case SYS_RMM_CPUINFO: ret = impl_cpuinfo((char *)a1, (int)a2); break;
    case SYS_RMM_PUTS:    display_puts((const char *)a1); ret = 0; break;
    case SYS_RMM_GETCHAR: ret = (int64_t)kb_getchar(); break;
    case SYS_RMM_UPTIME:  ret = (int64_t)pit_ticks(); break;

    default: break;
    }
#if SYSCALL_TRACE
    display_printf("[trace] -> ret=0x%lx\n", (unsigned long)ret);
#endif

    /* \u2500\u2500 Cooperative signal delivery \u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500
     * Every syscall return is the only preemption point.  Check for:
     *   1. Expired alarm timer  -> raise SIGALRM
     *   2. Pending signals      -> deliver one per syscall return
     * proc_deliver_signal may redirect frame->rcx (user RIP) to a handler.
     * \u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500 */
    /* Store the syscall's return value into the frame before signal
     * delivery: proc_deliver_signal snapshots frame->rax as ctx[6]
     * ("original syscall return") and rt_sigreturn restores it.  With the
     * store after delivery, SIGCHLD arriving on dash's vfork() return
     * restored the stale pre-syscall rax (the syscall number 58) over the
     * child's pid -- dash's job tracking then never matched and every
     * forked command's exit status read 255. */
    frame->rax = (uint64_t)ret;

    if (g_current_proc) {
        /* 1. alarm/setitimer expiry */
        if (g_current_proc->alarm_deadline_ms) {
            extern uint64_t pit_ticks(void);
            if (pit_ticks() >= g_current_proc->alarm_deadline_ms) {
                g_current_proc->alarm_deadline_ms = 0;
                g_current_proc->pending_sigs |= (1ull << SIGALRM);
            }
        }
        /* 2. Deliver one pending signal (lowest numbered wins) that is
         * not blocked by the sigprocmask mask                        */
        uint64_t psig = g_current_proc->pending_sigs & ~g_current_proc->sig_mask;
        if (psig) {
            for (int s = 1; s < 64; s++) {
                if (psig & (1ull << s)) {
                    g_current_proc->pending_sigs &= ~(1ull << s);
                    proc_deliver_signal(g_current_proc, s, frame);
                    /* ret stays as original syscall return value,
                     * frame->rcx may now point to user handler */
                    break;
                }
            }
        }
    }

    return ret;
}

/*
 * INT 0x80 stub, backward-compat path for libcmd.
 *
 * Pushes registers in the same order as syscall_entry.S (the syscall
 * instruction path) so that syscall_frame_t is identical regardless of
 * entry method.
 *
 * On INT 0x80 entry from ring 3, the CPU pushes a 5-value iretq frame
 * (SS, RSP, RFLAGS, CS, RIP) onto the kernel stack.  This stub pushes
 * the 16-register syscall frame on top, reads user RIP / RFLAGS / RSP
 * from the original iretq frame, then returns via SYSRETQ.
 *
 * After popping all 16 registers, RCX holds user RIP (from the iretq
 * frame), R11 holds user RFLAGS, and RSP holds user RSP, exactly what
 * SYSRETQ needs.  The original iretq frame is left dangling on the
 * kernel stack, this is harmless because the stack is reset on every
 * context switch.
 *
 * syscall_dispatch_c(nr, a1, a2, a3, a4, a5, a6, frame)
 *   rdi=nr  rsi=a1  rdx=a2  rcx=a3  r8=a4  r9=a5  stack: [a6, frame*]
 */
__attribute__((naked)) void syscall_stub(void) {
    __asm__ volatile (
        /* Push the first 13 registers (all from current register values)
         * On INT 0x80 the CPU does not clobber rcx/r11, so r9,r8,r10...,rbp
         * all still hold their original user values.  We push them now.
         * After these 13 pushes (104 bytes) the original iretq frame is at
         * [RSP+104] - we will read the remaining 3 values from there.      */
        "push  %%r9\n\t"                /* off   0: a6                     */
        "push  %%r8\n\t"                /* off   8: a5                     */
        "push  %%r10\n\t"               /* off  16: a4  (orig r10)         */
        "push  %%rdx\n\t"               /* off  24: a3                     */
        "push  %%rsi\n\t"               /* off  32: a2                     */
        "push  %%rdi\n\t"               /* off  40: a1                     */
        "push  %%rax\n\t"               /* off  48: nr                     */
        "push  %%r15\n\t"               /* off  56                         */
        "push  %%r14\n\t"               /* off  64                         */
        "push  %%r13\n\t"               /* off  72                         */
        "push  %%r12\n\t"               /* off  80                         */
        "push  %%rbx\n\t"               /* off  88                         */
        "push  %%rbp\n\t"               /* off  96                         */

        /* Read user RFLAGS from iretq frame and push as r11
         * After 13 pushes, RSP points 104 bytes below the iretq frame.
         * The iretq frame layout (from CPU): [RIP][CS][RFLAGS][RSP][SS]
         * RFLAGS is at iretq_base + 16 = RSP + 104 + 16 = RSP + 120.      */
        "mov   120(%%rsp), %%r11\n\t"
        "push  %%r11\n\t"               /* off 104: user RFLAGS            */

        /* Read user RIP from iretq frame and push as rcx
         * After 14 pushes: iretq base = RSP + 112.  RIP at base+0.        */
        "mov   112(%%rsp), %%rcx\n\t"
        "push  %%rcx\n\t"               /* off 112: user RIP               */

        /* Read user RSP from iretq frame and push
         * After 15 pushes: iretq base = RSP + 120.  RSP at base+24.       */
        "mov   144(%%rsp), %%r10\n\t"
        "push  %%r10\n\t"               /* off 120: user RSP               */

        /* Load dispatcher args (same offsets as syscall_entry.S) */
        "mov   48(%%rsp), %%rdi\n\t"    /* nr                                */
        "mov   40(%%rsp), %%rsi\n\t"    /* a1                                */
        "mov   32(%%rsp), %%rdx\n\t"    /* a2                                */
        "mov   24(%%rsp), %%rcx\n\t"    /* a3                                */
        "mov   16(%%rsp), %%r8\n\t"     /* a4                                */
        "mov    8(%%rsp), %%r9\n\t"     /* a5                                */

        /* Push stack args: a6 (7th), then frame* (8th) */
        "mov   %%rsp, %%r10\n\t"        /* r10 = frame ptr                  */
        "mov    0(%%rsp), %%r11\n\t"    /* r11 = a6                         */
        "sub   $8, %%rsp\n\t"           /* alignment pad                    */
        "push  %%r10\n\t"               /* 8th arg: frame*                  */
        "push  %%r11\n\t"               /* 7th arg: a6                      */
        "call  syscall_dispatch_c\n\t"
        "add   $24, %%rsp\n\t"          /* pop a6, frame*, pad             */
        "mov   %%rax, 48(%%rsp)\n\t"    /* patch saved-rax slot -> retval   */

        /* Pop all 16 registers and return via SYSRETQ */
        "pop   %%r9\n\t"
        "pop   %%r8\n\t"
        "pop   %%r10\n\t"               /* restores a4 (orig r10)           */
        "pop   %%rdx\n\t"
        "pop   %%rsi\n\t"
        "pop   %%rdi\n\t"
        "pop   %%rax\n\t"               /* return value                     */
        "pop   %%r15\n\t"
        "pop   %%r14\n\t"
        "pop   %%r13\n\t"
        "pop   %%r12\n\t"
        "pop   %%rbx\n\t"
        "pop   %%rbp\n\t"
        "pop   %%r11\n\t"               /* user RFLAGS -> SYSRETQ reads r11  */
        "pop   %%rcx\n\t"                /* user RIP    -> SYSRETQ reads rcx */
        "pop   %%rsp\n\t"               /* restore user stack               */
        "sysretq\n\t"
        ::: "memory"
    );
}

/*
 *  syscall instruction handler  (primary path for musl/glibc/newlib)
 *
 *  On entry: rip saved in rcx, rflags in r11, rsp still user stack.
 *  The CPU switched to kernel CS/SS but not rsp, we must do that manually
 *  using the TSS rsp0 value.
 *
 *  Convention: rax=nr, rdi/rsi/rdx/r10/r8/r9=args 1-6
 *  (r10 because syscall clobbers rcx with saved rip)
 */
/* syscall_entry is in src/exec/syscall_entry.S */
extern void     syscall_entry(void);
extern uint64_t syscall_user_rsp;   /* defined in .S */
extern uint64_t syscall_ksp;        /* defined in .S, set by syscall_msr_init */
extern uint64_t syscall_saved_r10;  /* defined in .S */

/* SYSCALL MSR setup */
/*
 * Three MSRs configure the syscall instruction:
 *
 * STAR  (0xC0000081):
 *   bits 47:32 = kernel CS selector  (SYSCALL loads this as CS, CS+8 as SS)
 *   bits 63:48 = user CS selector - 16  (SYSRET loads this+16 as CS, this+8 as SS)
 *
 *   Our GDT: 0x08=kernel code, 0x10=kernel data, 0x18=user code, 0x20=user data
 *   STAR[47:32] = 0x08  (kernel CS)
 *   STAR[63:48] = 0x18 - 16 = 0x08 ... wait, sysretq sets:
 *     CS = STAR[63:48] + 16
 *     SS = STAR[63:48] + 8
 *   So STAR[63:48] = GDT_USER_CODE - 16 = 0x18 - 0x10 = 0x08? No.
 *   Correct: sysretq uses STAR[63:48] as a base:
 *     CS = (STAR[63:48] | 3) + 16 - actually:
 *   The Intel/AMD manual says sysretq loads:
 *     CS from STAR[63:48]+16, with DPL forced to 3
 *     SS from STAR[63:48]+8,  with DPL forced to 3
 *   So we need: STAR[63:48]+16 = GDT_USER_CODE = 0x18  -> STAR[63:48] = 0x08
 *   And:        STAR[63:48]+8  = GDT_USER_DATA = 0x20? No, 0x08+8=0x10 (kernel data).
 *
 *   The real Linux layout is: null(0) | k_code(8) | k_data(16) | u_data(24) | u_code(32)
 *   where u_data comes before u_code so that STAR[63:48]=0x10:
 *     CS = 0x10+16 = 0x20 (ring-3), SS = 0x10+8 = 0x18 (ring-3 data)
 *
 *   Our GDT has: k_code(8), k_data(16), u_code(24), u_data(32).
 *   u_code and u_data are swapped vs Linux.  We need to swap them in the GDT
 *   so that STAR[63:48]+8 = user data and STAR[63:48]+16 = user code.
 *
 *   Required GDT order: null | k_code(8) | k_data(16) | u_data(24) | u_code(32)
 *   Then STAR[63:48] = 0x10:  CS=0x20 (u_code|3), SS=0x18 (u_data|3) OK
 *
 * LSTAR (0xC0000082): entry point for syscall (= &syscall_entry)
 * SFMASK(0xC0000084): rflags bits to clear on syscall entry (clear IF = 0x200)
 */

static void wrmsr(uint32_t msr, uint64_t val) {
    __asm__ volatile("wrmsr" :: "c"(msr),
                                "a"((uint32_t)val),
                                "d"((uint32_t)(val >> 32)));
}

static uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

#define MSR_EFER   0xC0000080
#define MSR_STAR   0xC0000081
#define MSR_LSTAR  0xC0000082
#define MSR_SFMASK 0xC0000084
#define EFER_SCE   (1ULL << 0)

void syscall_msr_init(void) {
    /* Enable SYSCALL/SYSRET in EFER */
    wrmsr(MSR_EFER, rdmsr(MSR_EFER) | EFER_SCE);

    /*
     * STAR[47:32] = GDT_KERNEL_CODE = 0x08
     *   syscall loads CS=0x08, SS=0x10
     * STAR[63:48] = GDT_KERNEL_DATA = 0x10
     *   sysretq loads CS = 0x10+16 = 0x20 = GDT_USER_CODE|3  OK
     *              SS  = 0x10+8  = 0x18 = GDT_USER_DATA|3  OK
     */
    wrmsr(MSR_STAR,  ((uint64_t)GDT_KERNEL_CODE << 32) |
                     ((uint64_t)GDT_KERNEL_DATA  << 48));
    wrmsr(MSR_LSTAR, (uint64_t)(uintptr_t)syscall_entry);
    wrmsr(MSR_SFMASK, 0x200ULL);  /* clear IF on entry */

    /* Provide a kernel stack for early syscalls (before any proc runs).
     * This gets updated to proc->kstack_top before each iretq to user. */
    static uint8_t early_kstack[4096 * 4] __attribute__((aligned(16)));
    syscall_ksp = (uint64_t)(uintptr_t)(early_kstack + sizeof(early_kstack));
}

/* Top-level init: INT 0x80 gate (legacy) + `syscall` MSRs */
void syscall_init(void) {
    extern void idt_register_syscall(void *stub);
    /* INT 0x80 is now the real ia32 ABI (Linux i386 register
     * convention + 32-bit syscall numbers).  The old 64-bit-args int
     * 0x80 stub (syscall_stub) had no remaining in-kernel users. */
    extern void ia32_syscall_entry(void);
    idt_register_syscall((void *)ia32_syscall_entry);
    syscall_msr_init();
}
/*
 *  IA-32 (i386) syscall compatibility layer
 *
 *  32-bit ELF programs enter via int 0x80 (ia32_syscall_entry in
 *  syscall_entry.S) and land here with the i386 register ABI:
 *    eax = number, ebx/ecx/edx/esi/edi/ebp = args 1..6.
 *  This dispatcher maps i386 syscall numbers onto the same native
 *  implementations the 64-bit table uses.  Upper halves are masked off
 *  (compat-mode writes zero-extend, but be defensive).
 *
 *  Scope the file/proc/ident/time surface a static i386
 *  program needs (this is what our 32bit-test and any early i386
 *  build exercises).  32-bit threads (futex), 32-bit signal frames,
 *  mmap2-with-huge-offsets and stat64-only consumers are documented
 *  as not supported yet.
 */

/* i386 struct stat64 (LSB layout) */
typedef struct __attribute__((packed)) {
    uint64_t st_dev;
    uint8_t  __pad0[4];
    uint32_t __st_ino;
    uint32_t st_mode;
    uint32_t st_nlink;
    uint32_t st_uid;
    uint32_t st_gid;
    uint64_t st_rdev;
    uint8_t  __pad3[4];
    int64_t  st_size;
    uint32_t st_blksize;
    uint64_t st_blocks;
    uint32_t st_atime; uint32_t st_atime_nsec;
    uint32_t st_mtime; uint32_t st_mtime_nsec;
    uint32_t st_ctime; uint32_t st_ctime_nsec;
    uint64_t st_ino;
} ia32_stat64_t;

/* i386 old_mmap argument struct */
typedef struct __attribute__((packed)) {
    uint32_t addr, len, prot, flags, fd, off;
} ia32_mmap_args_t;

static int64_t ia32_fill_stat64(const linux_stat_t *st, ia32_stat64_t *o) {
    kmemset(o, 0, sizeof(*o));
    o->st_dev     = st->st_dev;
    o->__st_ino   = (uint32_t)st->st_ino;
    o->st_mode    = st->st_mode;
    o->st_nlink   = (uint32_t)st->st_nlink;
    o->st_uid     = st->st_uid;
    o->st_gid     = st->st_gid;
    o->st_rdev    = st->st_rdev;
    o->st_size    = st->st_size;
    o->st_blksize = (uint32_t)st->st_blksize;
    o->st_blocks  = st->st_blocks;
    o->st_atime   = (uint32_t)st->st_atim_sec;
    o->st_atime_nsec = (uint32_t)st->st_atim_nsec;
    o->st_mtime   = (uint32_t)st->st_mtim_sec;
    o->st_mtime_nsec = (uint32_t)st->st_mtim_nsec;
    o->st_ctime   = (uint32_t)st->st_ctim_sec;
    o->st_ctime_nsec = (uint32_t)st->st_ctim_nsec;
    o->st_ino     = st->st_ino;
    return 0;
}

int64_t ia32_dispatch_c(uint64_t nr, uint64_t a1, uint64_t a2, uint64_t a3,
                        uint64_t a4, uint64_t a5, uint64_t a6,
                        syscall_frame_t *frame) {
    (void)frame;
    nr &= 0xFFFFFFFFULL;
    a1 &= 0xFFFFFFFFULL; a2 &= 0xFFFFFFFFULL; a3 &= 0xFFFFFFFFULL;
    a4 &= 0xFFFFFFFFULL; a5 &= 0xFFFFFFFFULL; a6 &= 0xFFFFFFFFULL;

    int64_t ret = -(int64_t)ENOSYS;
    switch (nr) {
    /* process lifecycle */
    case 1:   /* exit */
    case 252: /* exit_group */
        if (g_current_proc) proc_exit(g_current_proc, (int)a1);
        ret = 0; break;
    case 2:   /* fork */
    case 120: /* clone - fork-like only (CLONE_VM threads: deferred) */
        if (nr == 120 && ((a1 & 0x00000100ULL) || (a1 & 0x00010000ULL)))
            { ret = -(int64_t)ENOSYS; break; }   /* CLONE_VM / CLONE_FS  */
        ret = (int64_t)proc_fork(frame); break;
    case 11:  /* execve */
        ret = (int64_t)proc_execve((const char *)(uintptr_t)a1,
                                   (char *const *)(uintptr_t)a2,
                                   (char *const *)(uintptr_t)a3, frame);
        break;
    case 7:   /* waitpid */
    case 114: /* wait4 */
        ret = (int64_t)proc_waitpid((int)a1, (int *)(uintptr_t)a2,
                                    (int)a3, frame); break;
    case 20:  ret = g_current_proc ? g_current_proc->pid : 1; break;  /* getpid */
    case 224: ret = g_current_proc ? g_current_proc->pid : 1; break;  /* gettid */

    /* identity (single-user root) */
    case 24: case 199: case 200: case 201: case 202:
        ret = 0; break;               /* getuid/getgid/geteuid/getegid */

    /* basic file I/O */
    case 3:   ret = impl_read_console ((int)a1, (void *)(uintptr_t)a2, (size_t)a3); break;
    case 4:   ret = impl_write_console((int)a1, (const void *)(uintptr_t)a2, (size_t)a3); break;
    case 5:   ret = impl_open ((const char *)(uintptr_t)a1, (int)a2, (int)a3); break;
    case 6:   ret = (int64_t)vfs_close((int)a1); break;
    case 8:   ret = impl_open((const char *)(uintptr_t)a1,
                              O_CREAT | O_WRONLY | O_TRUNC, (int)a2); break;   /* creat */
    case 10:  ret = impl_unlink((const char *)(uintptr_t)a1); break;
    case 12:  ret = impl_chdir((const char *)(uintptr_t)a1); break;
    case 19:  ret = impl_lseek((int)a1, (int64_t)(int32_t)a2, (int)a3); break;
    case 33:  ret = impl_access((const char *)(uintptr_t)a1, (int)a2); break;
    case 41:  ret = impl_dup((int)a1); break;
    case 42:  ret = impl_pipe((int *)(uintptr_t)a1); break;
    case 45:  ret = (int64_t)proc_brk(a1); break;
    case 54:  ret = impl_ioctl((int)a1, a2, a3); break;
    case 78:  ret = impl_gettimeofday((linux_timeval_t *)(uintptr_t)a1,
                                      (void *)(uintptr_t)a2); break;
    case 85:  ret = impl_readlink((const char *)(uintptr_t)a1,
                                  (char *)(uintptr_t)a2, (size_t)a3); break;
    case 90:  { /* old_mmap(struct mmap_arg_struct *) */
        ia32_mmap_args_t m;
        kmemset(&m, 0, sizeof(m));
        kmemcpy(&m, (const void *)(uintptr_t)a1, sizeof(m));
        ret = impl_mmap(m.addr, m.len, (int)m.prot, (int)m.flags,
                        (int)m.fd, m.off);
        break;
    }
    case 91:  ret = impl_munmap(a1, a2); break;
    case 106: ret = impl_stat((const char *)(uintptr_t)a1,
                              (linux_stat_t *)(uintptr_t)a2); break;
    case 108: ret = impl_fstat((int)a1, (linux_stat_t *)(uintptr_t)a2); break;
    case 122: ret = impl_uname((linux_utsname_t *)(uintptr_t)a1); break;
    case 125: ret = 0; break;      /* mprotect: everything is RWX      */
    case 140: { /* _llseek(fd, hi, lo, *result, whence) */
        uint64_t off = ((uint64_t)a2 << 32) | a3;
        int64_t r = impl_lseek((int)a1, off, (int)a5);
        if (r >= 0) *(uint32_t *)(uintptr_t)a4 = (uint32_t)r;
        ret = r;
        break;
    }
    case 145: ret = impl_readv ((int)a1, (const void *)(uintptr_t)a2, (int)a3); break;
    case 146: ret = impl_writev((int)a1, (const void *)(uintptr_t)a2, (int)a3); break;
    case 183: ret = impl_getcwd((char *)(uintptr_t)a1, (size_t)a2); break;
    case 192: ret = impl_mmap(a1, a2, (int)a3, (int)a4, (int)a5,
                              a6 * 4096ULL); break;   /* mmap2: off in pages */
    case 195: { /* stat64 */
        linux_stat_t st64;
        ia32_stat64_t o;
        if (impl_stat((const char *)(uintptr_t)a1, &st64) != 0) { ret = -ENOENT; break; }
        ia32_fill_stat64(&st64, &o);
        kmemcpy((void *)(uintptr_t)a2, &o, sizeof(o));
        ret = 0; break;
    }
    case 197: { /* fstat64 */
        linux_stat_t st64;
        ia32_stat64_t o;
        if (impl_fstat((int)a1, &st64) != 0) { ret = -EBADF; break; }
        ia32_fill_stat64(&st64, &o);
        kmemcpy((void *)(uintptr_t)a2, &o, sizeof(o));
        ret = 0; break;
    }
    case 213: ret = 0; break;      /* setuid32 (root no-op)            */
    case 214: ret = 0; break;      /* setgid32 (root no-op)            */
    case 220: ret = impl_getdents64((int)a1, (void *)(uintptr_t)a2,
                                    (size_t)a3); break;
    case 265: ret = impl_clock_gettime((int)a1,
                                       (linux_timespec_t *)(uintptr_t)a2); break;
    case 355: { /* getrandom */
        uint8_t *out = (uint8_t *)(uintptr_t)a1;
        extern uint64_t pit_ticks(void);
        uint64_t state = pit_ticks() * 2654435761ULL + 0x9E3779B97F4A7C15ULL;
        for (uint32_t i = 0; i < a2; i++) {
            if ((i & 7) == 0) {
                state ^= state << 13; state ^= state >> 7; state ^= state << 17;
                state += pit_ticks();
            }
            out[i] = (uint8_t)(state >> ((i & 7) * 8));
        }
        ret = (int64_t)a2; break;
    }
    default:
        ret = -(int64_t)ENOSYS; break;
    }

    /* i386 convention: return value in eax, negative = -errno */
    return ret;
}

