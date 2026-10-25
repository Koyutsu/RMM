#include "fs/pseudo.h"
#include "fs/vfs.h"
#include "fs/kobj.h"
#include "fs/pty.h"
#include "exec/process.h"
#include "exec/sched.h"
#include "drivers/memory/vmm.h"
#include "drivers/timer/pit.h"
#include "drivers/timer/rtc.h"
#include "drivers/cpu/detectCPU.h"
#include "drivers/ps2/keyboard.h"
#include "drivers/serial/serial.h"
#include "drivers/input/evdev.h"
#include "drivers/graphics/fbdev.h"
#include "drivers/gpu/drm.h"
#include "drivers/pci/pci.h"
#include "kernel/version.h"
#include "console/display.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/*
 *  procfs-lite + devtmpfs, synthetic VFS trees
 *
 *  Why: a large share of portable software (and musl's own process
 *  helpers) reads /proc, and everything expects /dev/null and /dev/zero
 *  to exist.  Both trees are small in-core directories whose file nodes
 *  regenerate their content at read() time from live kernel state, so
 *  the "self" entries always describe the process that is reading (the
 *  kernel is cooperative, only one task ever reads at a time).
 */

/* pmm_* come from drivers/memory/vmm.h, rtc_epoch_secs from
 * drivers/timer/rtc.h, cpu_brand_string from detectCPU.h - all
 * included above, no local externs (they caused a type clash with
 * the real header declarations). */

/* dynamic file model */

typedef void (*pseudo_gen_t)(char *buf, size_t cap, size_t *out_len);

typedef struct {
    pseudo_gen_t gen;
    /* regenerated content cache (filled per read call) */
    char    cache[1024];
    size_t  cache_len;
} dyn_file_t;

static int dyn_read(vfs_node_t *node, void *buf, size_t len, size_t off) {
    dyn_file_t *df = (dyn_file_t *)node->data;
    if (!df || !df->gen) return -(int)VFS_EIO;

    df->cache_len = 0;
    df->gen(df->cache, sizeof df->cache, &df->cache_len);

    if (off >= df->cache_len) return 0;                 /* EOF */
    size_t n = df->cache_len - off;
    if (n > len) n = len;
    for (size_t i = 0; i < n; i++)
        ((char *)buf)[i] = df->cache[off + i];
    return (int)n;
}

static int dev_read(vfs_node_t *node, void *buf, size_t len, size_t off);

static const vfs_ops_t dyn_file_ops = {
    .read = dyn_read,
};

/* helpers */

static size_t ps_strl(char *dst, size_t cap, size_t *pos, const char *src) {
    while (*src && *pos + 1 < cap) dst[(*pos)++] = *src++;
    return *pos;
}

static size_t ps_u64(char *dst, size_t cap, size_t *pos, uint64_t v) {
    char tmp[24];
    int  i = 0;
    if (!v) tmp[i++] = '0';
    while (v) { tmp[i++] = (char)('0' + (v % 10)); v /= 10; }
    while (i && *pos + 1 < cap) dst[(*pos)++] = tmp[--i];
    return *pos;
}

static const char ps_hexdig[] = "0123456789abcdef";

static void ps_hexbyte(char *dst, size_t cap, size_t *pos, uint8_t v) {
    if (*pos + 2 < cap) {
        dst[(*pos)++] = ps_hexdig[(v >> 4) & 0xF];
        dst[(*pos)++] = ps_hexdig[v & 0xF];
    }
}

static void ps_hex16(char *dst, size_t cap, size_t *pos, uint16_t v) {
    ps_hexbyte(dst, cap, pos, (uint8_t)(v >> 8));
    ps_hexbyte(dst, cap, pos, (uint8_t)(v & 0xFF));
}

/* %016llx - fixed 16-digit hex (Linux /proc/bus/pci/devices column) */
static void ps_hex64_16(char *dst, size_t cap, size_t *pos, uint64_t v) {
    for (int s = 60; s >= 0; s -= 4)
        if (*pos + 1 < cap)
            dst[(*pos)++] = ps_hexdig[(v >> s) & 0xF];
}

/*
 *  /proc content generators
 */

static void gen_cpuinfo(char *buf, size_t cap, size_t *out_len) {
    size_t p = 0;
    ps_strl(buf, cap, &p, "processor       : 0\n");
    ps_strl(buf, cap, &p, "vendor_id       : GenuineRMM\n");
    {
        const char *brand = cpu_brand_string();
        ps_strl(buf, cap, &p, "model name      : ");
        ps_strl(buf, cap, &p, brand ? brand : "x86_64");
        ps_strl(buf, cap, &p, "\n");
    }
    ps_strl(buf, cap, &p, "architecture    : x86_64\n");
    /* x86-64 micro-architecture level + flag list */
    {
        char lv[2];
        lv[0] = (char)('0' + cpu_x86_64_level());
        lv[1] = '\0';
        ps_strl(buf, cap, &p, "x86-64 level    : v");
        ps_strl(buf, cap, &p, lv);
        ps_strl(buf, cap, &p, "\nflags           : ");
        ps_strl(buf, cap, &p, cpu_flags_string());
        ps_strl(buf, cap, &p, "\n");
    }
    ps_strl(buf, cap, &p, "bogomips        : 1000.00\n");
    ps_strl(buf, cap, &p, "\n");
    *out_len = p;
}

static void gen_meminfo(char *buf, size_t cap, size_t *out_len) {
    size_t p = 0;
    uint64_t total_kb = (pmm_total_pages() * 4096ULL) / 1024;
    uint64_t free_kb  = (pmm_total_pages() - pmm_used_pages()) * 4096ULL / 1024;
    ps_strl(buf, cap, &p, "MemTotal:     ");
    ps_u64(buf, cap, &p, total_kb);
    ps_strl(buf, cap, &p, " kB\nMemFree:      ");
    ps_u64(buf, cap, &p, free_kb);
    ps_strl(buf, cap, &p, " kB\nMemAvailable: ");
    ps_u64(buf, cap, &p, free_kb);
    ps_strl(buf, cap, &p, " kB\n");
    *out_len = p;
}

static void gen_uptime(char *buf, size_t cap, size_t *out_len) {
    size_t p = 0;
    uint64_t ms = pit_ticks();
    uint64_t secs = ms / 1000;
    uint64_t frac = (ms % 1000) / 100;      /* 1 decimal */
    ps_u64(buf, cap, &p, secs);
    ps_strl(buf, cap, &p, ".");
    ps_u64(buf, cap, &p, frac);
    ps_strl(buf, cap, &p, " 0.00\n");
    *out_len = p;
}

static void gen_version(char *buf, size_t cap, size_t *out_len) {
    size_t p = 0;
    ps_strl(buf, cap, &p, "RMM version " RMM_RELEASE
            " (build@rmm) (gcc) #1 SMP\n");
    *out_len = p;
}

static void gen_mounts(char *buf, size_t cap, size_t *out_len) {
    size_t p = 0;
#if defined(BOOT_RAMFS) && BOOT_RAMFS
    ps_strl(buf, cap, &p, "ramfs / ramfs rw 0 0\n");
#else
    ps_strl(buf, cap, &p, "/dev/root / ext2 rw 0 0\n");
#endif
    ps_strl(buf, cap, &p, "proc /proc proc rw 0 0\n");
    ps_strl(buf, cap, &p, "devtmpfs /dev devtmpfs rw 0 0\n");
    ps_strl(buf, cap, &p, "tmpfs /dev/shm tmpfs rw,nosuid,nodev 0 0\n");
    *out_len = p;
}

/* /proc/self/status - the fields ported software actually greps for. */
static void gen_self_status(char *buf, size_t cap, size_t *out_len) {
    size_t p = 0;
    process_t *me = g_current_proc;
    int pid   = me ? me->pid  : 1;
    int ppid  = me ? me->parent_pid : 0;

    {   /* real comm (prctl PR_SET_NAME / exec basename) */
        process_t *me2 = g_current_proc;
        ps_strl(buf, cap, &p, "Name:   ");
        ps_strl(buf, cap, &p,
                (me2 && me2->comm[0]) ? me2->comm : "rmm");
        ps_strl(buf, cap, &p, "\n");
    }
    ps_strl(buf, cap, &p, "State:  R (running)\n");
    ps_strl(buf, cap, &p, "Tgid:   ");
    ps_u64(buf, cap, &p, (uint64_t)pid);
    ps_strl(buf, cap, &p, "\nPid:    ");
    ps_u64(buf, cap, &p, (uint64_t)pid);
    ps_strl(buf, cap, &p, "\nPPid:   ");
    ps_u64(buf, cap, &p, (uint64_t)ppid);
    ps_strl(buf, cap, &p, "\nUid:    ");
    ps_u64(buf, cap, &p, me ? me->uid : 0);
    ps_strl(buf, cap, &p, " ");
    ps_u64(buf, cap, &p, me ? me->euid : 0);
    ps_strl(buf, cap, &p, " 0 0\nGid:    ");
    ps_u64(buf, cap, &p, me ? me->gid : 0);
    ps_strl(buf, cap, &p, " ");
    ps_u64(buf, cap, &p, me ? me->egid : 0);
    ps_strl(buf, cap, &p, " 0 0\n");
    *out_len = p;
}

/* /proc/self/cmdline - NUL-separated argv of the calling process. */
/* /proc/self/maps - synthesized from the loader metadata:
 * image range (+ interpreter range for dynamic images), heap, the
 * anonymous mmap window and the stack.  Perms are modelled (text
 * r-xp, data rw-p), purpose is introspection, not exactness. */
static void maps_line(char *buf, size_t cap, size_t *p,
                      uint64_t lo, uint64_t hi, const char *perms,
                      const char *name) {
    /* fixed-width hex (16 digits like Linux) */
    static const char hx[] = "0123456789abcdef";
    char tmp[19];
    for (int k = 0; k < 16; k++) tmp[15 - k] = hx[(lo >> (4 * k)) & 0xF];
    tmp[16] = 0;
    ps_strl(buf, cap, p, tmp);
    ps_strl(buf, cap, p, "-");
    for (int k = 0; k < 16; k++) tmp[15 - k] = hx[(hi >> (4 * k)) & 0xF];
    tmp[16] = 0;
    ps_strl(buf, cap, p, tmp);
    ps_strl(buf, cap, p, " ");
    ps_strl(buf, cap, p, perms);
    ps_strl(buf, cap, p, " 00000000 00:00 0");
    if (name && name[0]) {
        ps_strl(buf, cap, p, "    ");
        ps_strl(buf, cap, p, name);
    }
    ps_strl(buf, cap, p, "\n");
}

static void gen_self_maps(char *buf, size_t cap, size_t *out_len) {
    size_t p = 0;
    process_t *me = g_current_proc;
    if (!me) { *out_len = 0; return; }

    if (me->image.load_end > (uint64_t)(uintptr_t)me->image.load_base)
        maps_line(buf, cap, &p,
                  (uint64_t)(uintptr_t)me->image.load_base,
                  me->image.load_end,
                  "r-xp", me->exec_path);

    if (me->image.interp_path[0]) {
        uint64_t ilo = me->image.load_bias;
        /* interpreter was mapped at ELF_INTERP_BASE with its own bias */
        maps_line(buf, cap, &p, 0x30000000ULL, 0x30000000ULL + 0x40000ULL,
                  "r-xp", me->image.interp_path);
        (void)ilo;
    }

    if (me->heap_brk > me->heap_start)
        maps_line(buf, cap, &p, me->heap_start, me->heap_brk,
                  "rw-p", "[heap]");

    if (me->mmap_bump > PROC_MMAP_BASE)
        maps_line(buf, cap, &p, PROC_MMAP_BASE, me->mmap_bump,
                  "rw-p", "[anon]");

    maps_line(buf, cap, &p,
              PROC_STACK_BASE, PROC_STACK_TOP, "rw-p", "[stack]");

    *out_len = p;
}

static void gen_loadavg(char *buf, size_t cap, size_t *out_len) {
    size_t p = 0;
    unsigned run = sched_runnable_count();
    unsigned live = 0;
    for (int i = 0; i < PROC_MAX; i++)
        if (g_proc_table[i].state != PROC_STATE_EMPTY &&
            g_proc_table[i].state != PROC_STATE_ZOMBIE) live++;
    /* fixed-light load figures (single-CPU BORE): decayed constants */
    ps_strl(buf, cap, &p, "0.");
    ps_u64(buf, cap, &p, (uint64_t)(10 + (run % 3) * 5));
    ps_strl(buf, cap, &p, " 0.05 0.01 ");
    ps_u64(buf, cap, &p, run ? 1 : 0);
    ps_strl(buf, cap, &p, "/");
    ps_u64(buf, cap, &p, live);
    ps_strl(buf, cap, &p, " ");
    ps_u64(buf, cap, &p, live);
    ps_strl(buf, cap, &p, "\n");
    *out_len = p;
}

static void gen_self_cmdline(char *buf, size_t cap, size_t *out_len) {
    size_t p = 0;
    process_t *me = g_current_proc;
    if (me && me->argv) {
        for (int i = 0; i < me->argc; i++) {
            if (me->argv[i]) ps_strl(buf, cap, &p, me->argv[i]);
            if (i + 1 < me->argc && p + 1 < cap) buf[p++] = '\0';
        }
        if (p + 1 < cap) buf[p++] = '\0';
    } else {
        ps_strl(buf, cap, &p, "/init\0");
    }
    *out_len = p;
}

/* /proc/self/stat - minimal but parseable (ps/top style). */
static void gen_self_stat(char *buf, size_t cap, size_t *out_len) {
    size_t p = 0;
    process_t *me = g_current_proc;
    int pid = me ? me->pid : 1;
    ps_u64(buf, cap, &p, (uint64_t)pid);
    ps_strl(buf, cap, &p, " (");
    {
        process_t *me3 = g_current_proc;
        ps_strl(buf, cap, &p,
                (me3 && me3->comm[0]) ? me3->comm : "rmm");
    }
    ps_strl(buf, cap, &p, ") R ");
    ps_u64(buf, cap, &p, (uint64_t)(me ? me->parent_pid : 0));
    ps_strl(buf, cap, &p, " 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0\n");
    *out_len = p;
}

/*
 *  /dev character devices
 */

typedef enum {
    DEV_NULL = 1, DEV_ZERO, DEV_CONSOLE, DEV_RANDOM, DEV_FB0,
    DEV_EVENT0, DEV_EVENT1,
    DEV_DRM_CARD, DEV_DRM_RENDER,     /* /dev/dri/card0, renderD128 */
    DEV_PTMX,                         /* /dev/ptmx cloning device   */
} dev_kind_t;

static int dev_read(vfs_node_t *node, void *buf, size_t len, size_t off) {
    dev_kind_t kind = (dev_kind_t)(uintptr_t)node->data;
    (void)off;
    switch (kind) {
    case DEV_NULL:
        return 0;                                      /* permanent EOF */
    case DEV_ZERO: {
        for (size_t i = 0; i < len; i++) ((uint8_t *)buf)[i] = 0;
        return (int)len;
    }
    case DEV_CONSOLE: {
        /* raw (non-canonical) byte, console input parity with the
         * pseudo-tty read path (serial + keyboard).  (BORE):
         * switchable park between probes. */
        int c;
        for (;;) {
            c = kb_getchar_nb();
            if (c) break;
            c = serial_getchar_nb();
            if (c > 0) break;
            if (sched_kernel_park(BORE_EVENT_POLL_MS) != 0)
                __asm__ volatile("sti; hlt; cli");
        }
        if (c < 0) return 0;
        *(uint8_t *)buf = (uint8_t)c;
        return 1;
    }
    case DEV_RANDOM: {
        /* xorshift64* - not cryptographic, but fine for ids/temp names.
         * Seeded from RTC + PIT jitter. */
        static uint64_t rs;
        if (!rs) rs = rtc_epoch_secs() * 0x9E3779B97F4A7C15ULL
                      ^ pit_ticks() ^ 0xD1B54A32D192ED03ULL;
        uint8_t *out = (uint8_t *)buf;
        for (size_t i = 0; i < len; i++) {
            if (i % 8 == 0) {
                rs ^= rs >> 12; rs ^= rs << 25; rs ^= rs >> 27;
                uint64_t r = rs * 2685821657736338717ULL;
                for (int b = 0; b < 8 && (i + (size_t)b) < len; b++)
                    out[i + (size_t)b] = (uint8_t)(r >> (8 * b));
                i += 7;
            }
        }
        return (int)len;
    }
    case DEV_FB0: {
        /* read() on /dev/fb0 copies raw pixel bytes starting at
         * the file offset (same layout read()/write() uses on Linux) */
        fbdev_t *f = fbdev_primary();
        if (!f || !f->fb) return -(int)VFS_ENODEV;
        uint64_t fb_bytes = (uint64_t)f->pitch * f->height;
        if (off >= fb_bytes) return 0;
        size_t can = (size_t)(fb_bytes - off);
        if (len > can) len = can;
        const uint8_t *src = (const uint8_t *)(uintptr_t)f->fb + off;
        uint8_t *dst = (uint8_t *)buf;
        for (size_t i = 0; i < len; i++) dst[i] = src[i];
        return (int)len;
    }
    case DEV_EVENT0:                                      /* evdev */
    case DEV_EVENT1:
        /* struct input_event stream, offsets are ignored (char device) */
        return evdev_read((kind == DEV_EVENT0) ? 0 : 1, buf, len);
    case DEV_DRM_CARD:                                    /* KMS   */
    case DEV_DRM_RENDER:
        /* card0 read() is the vblank/event channel on Linux, no event
         * source exists yet, so report "would block" like O_NONBLOCK */
        return -(int)VFS_EAGAIN;
    case DEV_PTMX:                                        /* ptmx  */
        /* reads go through the per-pty master node allocated by the
         * open() hook in vfs.c - the ptmx node itself is never an fd */
        return -(int)VFS_EINVAL;
    default:
        return -(int)VFS_EIO;
    }
}

static int dev_write(vfs_node_t *node, const void *buf, size_t len,
                     size_t off) {
    dev_kind_t kind = (dev_kind_t)(uintptr_t)node->data;
    (void)buf; (void)off;
    switch (kind) {
    case DEV_NULL:
    case DEV_ZERO:
        return (int)len;                               /* black hole */
    case DEV_CONSOLE: {
        /* route through the display + serial like the console write path */
        const char *s = (const char *)buf;
        extern void serial_print_byte(const char character);
        for (size_t i = 0; i < len; i++) {
            display_putchar(s[i]);
            serial_print_byte(s[i]);
        }
        return (int)len;
    }
    case DEV_RANDOM:
        return (int)len;                               /* sink */
    case DEV_EVENT0:                                  /* evdev:   */
    case DEV_EVENT1:
        return -(int)VFS_EINVAL;     /* no writes to input devices      */
    case DEV_DRM_CARD:                                /* DRM is  */
    case DEV_DRM_RENDER:                              /* ioctl-only      */
        return -(int)VFS_EINVAL;
    case DEV_PTMX:
        return -(int)VFS_EINVAL;
    case DEV_FB0: {
        /* write() on /dev/fb0 copies raw pixel bytes into the
         * framebuffer at the file offset and flushes RAM-backed drivers. */
        fbdev_t *f = fbdev_primary();
        if (!f || !f->fb) return -(int)VFS_ENODEV;
        uint64_t fb_bytes = (uint64_t)f->pitch * f->height;
        if (off >= fb_bytes) return -(int)VFS_ENOSPC;
        size_t can = (size_t)(fb_bytes - off);
        if (len > can) len = can;
        uint8_t *dst = (uint8_t *)(uintptr_t)f->fb + off;
        const uint8_t *src = (const uint8_t *)buf;
        for (size_t i = 0; i < len; i++) dst[i] = src[i];
        if (f->flush_rows) f->flush_rows(f, off / f->pitch,
                                         (off + len - 1) / f->pitch);
        return (int)len;
    }
    default:
        return -(int)VFS_EIO;
    }
}

static const vfs_ops_t dev_ops = {
    .read  = dev_read,
    .write = dev_write,
};

/*
 *  /dev/shm - POSIX shared-memory namespace
 *
 *  On Linux /dev/shm is a tmpfs where shm_open("/name") is just
 *  open("/dev/shm/name", flags, mode) and MAP_SHARED gives every process
 *  The same physical pages.  musl implements shm_open exactly that way, so
 *  a directory with lazy-creating lookup is enough to make the whole
 *  shm_open/mmap/ftruncate/unlink family work unmodified:
 *
 *    - first resolve of /dev/shm/<name> creates a memfd-backed kobj node
 *      (page-list backing, real MAP_SHARED via kobj_memfd_mmap_shared) and
 *      attaches it - later opens resolve to the same node -> same pages,
 *    - the directory entry holds one fd_ref so the object survives until
 *      unlink (open adds refs, close removes them);
 *    - readdir iterates the attached children like any in-core dir.
 */

extern vfs_node_t *kobj_shm_new_file(const char *name);   /* kobj_evt.c */
extern void        kobj_shm_drop_dir_ref(vfs_node_t *n);  /* kobj_evt.c */

static bool shm_streq(const char *a, const char *b) {
    while (*a && *b) { if (*a != *b) return false; a++; b++; }
    return *a == *b;
}

static size_t shm_strlen(const char *s) {
    size_t n = 0; while (s[n]) n++; return n;
}

static vfs_node_t *shm_finddir(vfs_node_t *dir, const char *name) {
    /* lookup only - plain open() of a missing name must give ENOENT,
     * exactly like tmpfs.  Creation happens in mknod (the O_CREAT path). */
    for (int i = 0; i < dir->child_count; i++)
        if (shm_streq(dir->children[i]->name, name))
            return dir->children[i];
    return NULL;
}

static vfs_node_t *shm_mknod(vfs_node_t *dir, const char *name, uint8_t type) {
    if (type != VFS_TYPE_FILE) return NULL;
    if (shm_strlen(name) >= VFS_NAME_MAX) return NULL;
    /* O_CREAT on an existing entry opens the existing object (POSIX). */
    for (int i = 0; i < dir->child_count; i++)
        if (shm_streq(dir->children[i]->name, name))
            return dir->children[i];
    vfs_node_t *n = kobj_shm_new_file(name);
    if (!n) return NULL;
    if (!vfs_attach(dir, n)) { kobj_shm_drop_dir_ref(n); return NULL; }
    return n;
}

/* unlink: detach + drop the directory's reference (pages free once the
 * last fd / mapping is gone, exactly like POSIX shm_unlink). */
static int shm_unlink(vfs_node_t *dir, const char *name) {
    for (int i = 0; i < dir->child_count; i++) {
        if (shm_streq(dir->children[i]->name, name)) {
            vfs_node_t *victim = dir->children[i];
            for (int j = i; j < dir->child_count - 1; j++)
                dir->children[j] = dir->children[j + 1];
            dir->child_count--;
            kobj_shm_drop_dir_ref(victim);
            return 0;
        }
    }
    return -(int)VFS_ENOENT;
}

static const vfs_ops_t shm_dir_ops = {
    .finddir = shm_finddir,
    .mknod   = shm_mknod,
    .unlink  = shm_unlink,
};

/*
 *  dynamic symlink nodes: /proc/self/exe, /proc/self/cwd,
 *  /proc/self/root and /proc/self/fd/<n>
 *
 *  These behave like Linux procfs symlinks: readlink(2) fills the
 *  caller's buffer with the current target (regenerated per call -
 *  "self" always describes the calling process), and open(2) resolves
 *  one level through to the target (so `open("/proc/self/fd/1", ...)`
 *  and `cat $(realpath /proc/self/exe)` style usage work).
 *
 *  The kernel's real filesystems do not implement symlinks yet, these
 *  nodes live only inside the pseudo trees and are recognised by their
 *  ops pointer (dyn_link_ops), exactly like the /dev and /dev/shm
 *  specialisations above.
 */

typedef struct {
    void (*gen)(char *buf, size_t cap, size_t *out_len);  /* exe/cwd/root */
    int fd;                       /* >= 0: an /proc/self/fd/<fd> link   */
} dyn_link_t;

static uint32_t pseudo_link_ino = 9000;   /* own inode space for links */

int pseudo_node_kind_hint(vfs_node_t *n);   /* fwd: defined below */

static const vfs_ops_t dyn_link_ops = { };    /* marker, never called  */

static vfs_node_t *make_dyn_link(const char *name,
                                 void (*gen)(char *, size_t, size_t *),
                                 int fd) {
    static dyn_link_t link_pool[40];
    static int link_used = 0;
    if (link_used >= 40) return NULL;
    dyn_link_t *dl = &link_pool[link_used++];
    dl->gen = gen;
    dl->fd  = fd;

    vfs_node_t *n = vfs_create_node(name, VFS_TYPE_LINK, &dyn_link_ops, dl);
    if (!n) return NULL;
    n->mode  = 0777 | 0120000;    /* lrwxrwxrwx */
    n->inode = pseudo_link_ino++;
    n->size  = 0;
    n->pinned = 1;
    return n;
}

/* target generators */

static void gen_exe_target(char *buf, size_t cap, size_t *out_len) {
    size_t p = 0;
    process_t *me = g_current_proc;
    const char *path = (me && me->exec_path[0]) ? me->exec_path : "/init";
    ps_strl(buf, cap, &p, path);
    *out_len = p;
}

static void gen_cwd_target(char *buf, size_t cap, size_t *out_len) {
    size_t p = 0;
    process_t *me = g_current_proc;
    ps_strl(buf, cap, &p, (me && me->cwd[0]) ? me->cwd : "/");
    *out_len = p;
}

static void gen_root_target(char *buf, size_t cap, size_t *out_len) {
    size_t p = 0;
    ps_strl(buf, cap, &p, "/");
    *out_len = p;
}

/* /proc/self/fd - per-fd symlink children (dynamic dir) */

static vfs_node_t *fd_dir_finddir(vfs_node_t *dir, const char *name) {
    (void)dir;
    process_t *me = g_current_proc;
    if (!me || !me->fdt) return NULL;
    int fd = 0;
    for (const char *p = name; *p; p++) {
        if (*p < '0' || *p > '9') return NULL;
        fd = fd * 10 + (*p - '0');
        if (fd > 999) return NULL;
    }
    if (fd >= VFS_MAX_FDS) return NULL;
    if (!vfs_fd_peek_node(fd) && fd > 2) return NULL;
    if (fd <= 2 && !me->fdt->t[fd].used) return NULL;

    /* fd links are looked up repeatedly (musl __stdio_exit, shell
     * redirections, lsof-style tools).  A tiny per-(pid, fd) cache
     * keeps make_dyn_link's 40-slot pool from exhausting. */
    static struct { int pid; int fd; vfs_node_t *node; } cache[8];
    static int cache_round = 0;
    for (int i = 0; i < 8; i++)
        if (cache[i].pid == me->pid && cache[i].fd == fd &&
            cache[i].node)
            return cache[i].node;
    vfs_node_t *ln = make_dyn_link(name, NULL, fd);
    if (ln) {
        cache[cache_round] = (typeof(cache[0])){ me->pid, fd, ln };
        cache_round = (cache_round + 1) % 8;
    }
    return ln;
}

static int fd_dir_readdir(vfs_node_t *dir, int idx,
                          char *name_out, uint8_t *type_out) {
    (void)dir;
    process_t *me = g_current_proc;
    if (!me || !me->fdt) return -1;
    int seen = 0;
    for (int fd = 0; fd < VFS_MAX_FDS; fd++) {
        if (!me->fdt->t[fd].used) continue;
        if (idx == seen) {
            /* decimal fd number as the dirent name */
            char tmp[8];
            int n = 0;
            int v = fd;
            do { tmp[n++] = (char)('0' + v % 10); v /= 10; } while (v);
            for (int k = 0; k < n; k++) name_out[k] = tmp[n - 1 - k];
            name_out[n] = '\0';
            *type_out = VFS_TYPE_LINK;
            return 0;
        }
        seen++;
    }
    return -1;
}

static const vfs_ops_t fd_dir_ops = {
    .finddir = fd_dir_finddir,
    .readdir = fd_dir_readdir,
};

/* readlink resolution entry point (called from the syscall layer)
 * Returns the target length, or 0 when node is not a pseudo symlink.  */
/* tiny local strlen */
static size_t strlen_fallback(const char *s) {
    size_t n = 0; while (s[n]) n++; return n;
}

int pseudo_link_target(vfs_node_t *n, char *buf, size_t cap) {
    if (!n || n->type != VFS_TYPE_LINK || n->ops != &dyn_link_ops)
        return 0;
    dyn_link_t *dl = (dyn_link_t *)n->data;
    if (!dl) return 0;

    size_t len = 0;
    if (dl->gen) {
        dl->gen(buf, cap, &len);
        return (int)len;
    }
    if (dl->fd >= 0) {
        /* /proc/self/fd/<n>: reconstruct the open file's path.  For
         * anonymous objects there is no path, so mirror Linux's
         * synthetic names: pipe:[ino], socket:[ino], anon_inode:*     */
        process_t *me = g_current_proc;
        if (!me || !me->fdt || dl->fd >= VFS_MAX_FDS) return -(int)VFS_ENOENT;
        /* peek the live table, the process may have opened the fd
         * after its last scheduler switch saved the snapshot */
        vfs_node_t *fn = vfs_fd_peek_node(dl->fd);
        if (!fn) {
            if (dl->fd <= 2) {              /* console pseudo-fd         */
                size_t q = 0;
                ps_strl(buf, cap, &q, "/dev/tty");
                return (int)q;
            }
            return -(int)VFS_ENOENT;        /* dead / closed fd          */
        }

        if (!pseudo_node_is_console_tty(fn)) {
            /* pty ends have no resolvable tree path (their nodes
             * are parentless) - render the canonical devfs names. */
            {
                extern bool pty_node_paths(vfs_node_t *, char *, size_t);
                char pbuf[VFS_PATH_MAX];
                if (pty_node_paths(fn, pbuf, sizeof pbuf)) {
                    size_t q = 0;
                    ps_strl(buf, cap, &q, pbuf);
                    return (int)q;
                }
            }
            int vnp = vfs_node_path(fn, buf, cap);
            if (vnp == 0 && buf[0]) return (int)strlen_fallback(buf);
        }

        /* synthetic fallbacks */
        size_t p = 0;
        int hint = pseudo_node_kind_hint(fn);
        switch (hint) {
        case 1: ps_strl(buf, cap, &p, "pipe:["); break;
        case 2: ps_strl(buf, cap, &p, "socket:["); break;
        default: ps_strl(buf, cap, &p, "anon_inode:["); break;
        }
        ps_u64(buf, cap, &p, (uint64_t)fn->inode);
        ps_strl(buf, cap, &p, "]");
        (void)p;
        return (int)p;
    }
    return 0;
}

/* classify anonymous (pathless) nodes for /proc/self/fd fallback names */
int pseudo_node_kind_hint(vfs_node_t *n) {
    if (!n) return 0;
    if (n->type == VFS_TYPE_PIPE) return 1;               /* pipe:[ino]  */
    if (n->type == VFS_TYPE_KOBJ) return 2;               /* socket:[ino]*/
    return 0;                                             /* anon inode  */
}

/*
 *  Node classification helpers (used by vfs.c and the syscall layer)
 */

/* True when node is the /dev/tty or /dev/console character device.
 * Opening either must install the console pseudo-fd (used slot, NULL node)
 * so /dev/tty behaves like the real controlling terminal, dash's job
 * control opens /dev/tty and without this mapping it reports
 * "can't access tty, job control turned off". */
bool pseudo_node_is_console_tty(vfs_node_t *n) {
    return n && n->type == VFS_TYPE_DEV &&
           n->ops == &dev_ops &&
           (dev_kind_t)(uintptr_t)n->data == DEV_CONSOLE;
}

/* True when node is /dev/fb0 (framebuffer device). */
bool pseudo_node_is_fb0(vfs_node_t *n) {
    return n && n->type == VFS_TYPE_DEV &&
           n->ops == &dev_ops &&
           (dev_kind_t)(uintptr_t)n->data == DEV_FB0;
}

/* True when node is /dev/dri/card0 or renderD128, stores 0 in *CARD.
 * (Linux DRM/KMS UAPI boundary) */
bool pseudo_node_is_drm(vfs_node_t *n, int *card) {
    if (!n || n->type != VFS_TYPE_DEV || n->ops != &dev_ops) return false;
    dev_kind_t k = (dev_kind_t)(uintptr_t)n->data;
    if (k != DEV_DRM_CARD && k != DEV_DRM_RENDER) return false;
    if (card) *card = 0;
    return true;
}

/* True when node is /dev/ptmx (the pty cloning device). */
bool pseudo_node_is_ptmx(vfs_node_t *n) {
    return n && n->type == VFS_TYPE_DEV &&
           n->ops == &dev_ops &&
           (dev_kind_t)(uintptr_t)n->data == DEV_PTMX;
}

/* True when node is a /dev/input/eventN device, stores N in *IDX.
 * (Linux evdev UAPI boundary) */
bool pseudo_node_is_evdev(vfs_node_t *n, int *idx) {
    if (!n || n->type != VFS_TYPE_DEV || n->ops != &dev_ops) return false;
    dev_kind_t k = (dev_kind_t)(uintptr_t)n->data;
    if (k != DEV_EVENT0 && k != DEV_EVENT1) return false;
    if (idx) *idx = (k == DEV_EVENT0) ? 0 : 1;
    return true;
}

/*
 *  Tree construction
 */

static uint32_t pseudo_next_ino = 4000;

static vfs_node_t *make_dir(const char *name) {
    vfs_node_t *n = vfs_create_node(name, VFS_TYPE_DIR, NULL, NULL);
    if (!n) return NULL;
    n->mode  = 0555 | 0040000;              /* dr-xr-xr-x */
    n->inode = pseudo_next_ino++;
    n->pinned = 1;                          /* never auto-freed */
    return n;
}

static vfs_node_t *make_dyn_file(const char *name, pseudo_gen_t gen,
                                 uint32_t mode) {
    static dyn_file_t dyn_pool[16];
    static int dyn_used = 0;
    if (dyn_used >= 16) return NULL;
    dyn_file_t *df = &dyn_pool[dyn_used++];
    df->gen = gen;
    df->cache_len = 0;

    vfs_node_t *n = vfs_create_node(name, VFS_TYPE_FILE, &dyn_file_ops, df);
    if (!n) return NULL;
    n->mode  = mode | 0100000;
    n->inode = pseudo_next_ino++;
    n->size  = 0;
    n->pinned = 1;
    return n;
}

/* big-cache variant for tables that exceed 1 KiB (PCI device list) */
typedef struct {
    pseudo_gen_t gen;
    char    cache[4096];
    size_t  cache_len;
} dyn_file_big_t;

static int dyn_read_big(vfs_node_t *node, void *buf, size_t len, size_t off) {
    dyn_file_big_t *df = (dyn_file_big_t *)node->data;
    if (!df || !df->gen) return -(int)VFS_EIO;
    df->cache_len = 0;
    df->gen(df->cache, sizeof df->cache, &df->cache_len);
    if (off >= df->cache_len) return 0;
    size_t n = df->cache_len - off;
    if (n > len) n = len;
    for (size_t i = 0; i < n; i++)
        ((char *)buf)[i] = df->cache[off + i];
    return (int)n;
}

static const vfs_ops_t dyn_file_big_ops = {
    .read = dyn_read_big,
};

static vfs_node_t *make_dyn_file_big(const char *name, pseudo_gen_t gen,
                                     uint32_t mode) {
    static dyn_file_big_t big_pool[2];
    static int big_used = 0;
    if (big_used >= 2) return NULL;
    dyn_file_big_t *df = &big_pool[big_used++];
    df->gen = gen;
    df->cache_len = 0;

    vfs_node_t *n = vfs_create_node(name, VFS_TYPE_FILE, &dyn_file_big_ops, df);
    if (!n) return NULL;
    n->mode  = mode | 0100000;
    n->inode = pseudo_next_ino++;
    n->size  = 0;
    n->pinned = 1;
    return n;
}

static vfs_node_t *make_dev(const char *name, dev_kind_t kind,
                            uint32_t perms) {
    vfs_node_t *n = vfs_create_node(name, VFS_TYPE_DEV, &dev_ops,
                                    (void *)(uintptr_t)kind);
    if (!n) return NULL;
    n->mode  = perms | 0020000;             /* S_IFCHR */
    n->inode = pseudo_next_ino++;
    n->uid = 0; n->gid = 0;
    n->pinned = 1;
    return n;
}

/* Linux-standard tables:
 * /proc/bus/pci/devices and /proc/fb replace the RMM-private
 * SYS_RMM_PCILIST / SYS_RMM_FBINFO data path with the interfaces
 * Linux software expects.  The private syscalls stay as deprecated
 * aliases (see core/exec/syscall_table.c) but new tooling should read
 * these files instead. */
static void gen_pci_devices(char *buf, size_t cap, size_t *out_len) {
    /* Linux format per line:
     *   bus+devfn (%02x%02x)  vendor+device (%04x%04x)  irq (%02x)
     *   six resource slots as %016llx (BAR physical base, 0 = absent) */
    size_t p = 0;
    for (int i = 0; i < g_pci_device_count && i < PCI_MAX_DEVICES; i++) {
        const pci_device_t *d = &g_pci_devices[i];
        if (!d->valid) continue;
        size_t start = p;
        ps_strl(buf, cap, &p, "");
        /* bus, devfn */
        ps_hexbyte(buf, cap, &p, (uint8_t)d->bus);
        ps_hexbyte(buf, cap, &p, (uint8_t)((d->dev << 3) | (d->fn & 7)));
        ps_strl(buf, cap, &p, "\t");
        ps_hex16(buf, cap, &p, d->vendor_id);
        ps_hex16(buf, cap, &p, d->device_id);
        ps_strl(buf, cap, &p, "\t");
        ps_hexbyte(buf, cap, &p, d->irq_line);
        for (int b = 0; b < 6; b++) {
            ps_strl(buf, cap, &p, "\t");
            ps_hex64_16(buf, cap, &p, d->bars[b].size ? d->bars[b].base : 0);
        }
        ps_strl(buf, cap, &p, "\n");
        if (p + 128 >= cap) { p = start; break; }   /* cache full */
    }
    buf[p] = '\0';
    *out_len = p;
}

static void gen_fb(char *buf, size_t cap, size_t *out_len) {
    /* Linux /proc/fb: "<index> <driver>\n" per framebuffer */
    size_t p = 0;
    fbdev_t *f = fbdev_primary();
    if (f && f->name) {
        ps_strl(buf, cap, &p, "0 ");
        ps_strl(buf, cap, &p, f->name);
        ps_strl(buf, cap, &p, "\n");
    }
    buf[p] = '\0';
    *out_len = p;
}

void pseudo_mount_all(void) {
    /* /proc */
    vfs_node_t *proc = make_dir("proc");
    if (proc) {
        vfs_attach(proc, make_dyn_file("cpuinfo", gen_cpuinfo, 0444));
        vfs_attach(proc, make_dyn_file("meminfo", gen_meminfo, 0444));
        vfs_attach(proc, make_dyn_file("uptime",  gen_uptime,  0444));
        vfs_attach(proc, make_dyn_file("version", gen_version, 0444));
        vfs_attach(proc, make_dyn_file("mounts",  gen_mounts,  0444));
        /* Linux-standard kernel tables:
         * /proc/bus/pci/devices + /proc/fb - the Linux alternatives to
         * the deprecated RMM-private PCILIST/FBINFO syscalls. */
        vfs_node_t *bus = make_dir("bus");
        if (bus) {
            vfs_node_t *pcid = make_dir("pci");
            if (pcid) {
                vfs_attach(pcid, make_dyn_file_big("devices",
                                                   gen_pci_devices, 0444));
                vfs_attach(bus, pcid);
            }
            vfs_attach(proc, bus);
        }
        vfs_attach(proc, make_dyn_file("fb", gen_fb, 0444));
        /* loadavg + per-process maps */
        vfs_attach(proc, make_dyn_file("loadavg", gen_loadavg, 0444));

        vfs_node_t *self = make_dir("self");
        if (self) {
            vfs_attach(self, make_dyn_file("cmdline", gen_self_cmdline, 0444));
            vfs_attach(self, make_dyn_file("status",  gen_self_status,  0444));
            vfs_attach(self, make_dyn_file("stat",    gen_self_stat,    0444));
            /* memory-map introspection */
            vfs_attach(self, make_dyn_file("maps", gen_self_maps, 0444));
            /* exe / cwd / root symlinks + per-fd fd/ directory */
            vfs_attach(self, make_dyn_link("exe",  gen_exe_target,  -1));
            vfs_attach(self, make_dyn_link("cwd",  gen_cwd_target,  -1));
            vfs_attach(self, make_dyn_link("root", gen_root_target, -1));
            {
                vfs_node_t *fddir = vfs_create_node("fd", VFS_TYPE_DIR,
                                                    &fd_dir_ops, NULL);
                if (fddir) {
                    fddir->mode   = 0555 | 0040000;
                    fddir->inode  = pseudo_next_ino++;
                    fddir->pinned = 1;
                    vfs_attach(self, fddir);
                }
            }
            vfs_attach(proc, self);
        }
        vfs_mount("/proc", proc);
    }

    /* /dev */
    vfs_node_t *dev = make_dir("dev");
    if (dev) {
        vfs_attach(dev, make_dev("null",    DEV_NULL,    0666));
        vfs_attach(dev, make_dev("zero",    DEV_ZERO,    0666));
        vfs_attach(dev, make_dev("console", DEV_CONSOLE, 0620));
        vfs_attach(dev, make_dev("tty",     DEV_CONSOLE, 0666));
        vfs_attach(dev, make_dev("random",  DEV_RANDOM,  0666));
        vfs_attach(dev, make_dev("urandom", DEV_RANDOM,  0666));
        /* Linux-style framebuffer device, the primary fbdev's
         * memory is mmap-able MAP_SHARED, ioctls match linux/fb.h, and
         * read()/write() address raw pixels.  The fbdev itself may not
         * be registered yet (drivers probe later): /dev/fb0 resolves the
         * primary lazily on every use. */
        vfs_attach(dev, make_dev("fb0",     DEV_FB0,     0666));

        /* Linux DRM/KMS devices.  card0 exposes the KMS-lite
         * UAPI (dumb buffers + mode set) over whichever GPU backend won
         * the boot probe, renderD128 is the same device (render-node
         * restrictions don't apply, no GPU contexts exist yet).  The
         * card resolves lazily like /dev/fb0: the node exists even when
         * no display backend was found (ioctls then report -ENODEV). */
        vfs_node_t *dri = make_dir("dri");
        if (dri) {
            vfs_attach(dri, make_dev("card0",     DEV_DRM_CARD,  0666));
            vfs_attach(dri, make_dev("renderD128", DEV_DRM_RENDER, 0666));
            vfs_attach(dev, dri);
        }

        /* Linux evdev input devices.  event0 (PS/2 keyboard) is
         * always present, event1 (PS/2 mouse) only when its probe during
         * boot succeeded - exactly like a Linux box without a mouse. */
        vfs_node_t *input = make_dir("input");
        if (input) {
            vfs_attach(input, make_dev("event0", DEV_EVENT0, 0640));
            if (evdev_mouse_present())
                vfs_attach(input, make_dev("event1", DEV_EVENT1, 0640));
            vfs_attach(dev, input);
        }

        /* BSD pty cloning device + devpts directory.  open()
         * (/dev/ptmx) allocates a pair; /dev/pts/N resolves to the slave
         * end through pty_pts_find(). */
        vfs_attach(dev, make_dev("ptmx", DEV_PTMX, 0666));
        {
            extern const vfs_ops_t pty_pts_dir_ops;
            vfs_node_t *pts = vfs_create_node("pts", VFS_TYPE_DIR,
                                              &pty_pts_dir_ops, NULL);
            if (pts) {
                pts->mode   = 0755 | 0040000;   /* drwxr-xr-x (devpts) */
                pts->inode  = pseudo_next_ino++;
                pts->pinned = 1;
                vfs_attach(dev, pts);
            }
        }

        vfs_mount("/dev", dev);

        /* /dev/shm - POSIX shared memory namespace */
        vfs_node_t *shm = vfs_create_node("shm", VFS_TYPE_DIR,
                                          &shm_dir_ops, NULL);
        if (shm) {
            shm->mode   = 0777 | 0040000;    /* drwxrwxrwx (tmpfs-like) */
            shm->inode  = pseudo_next_ino++;
            shm->pinned = 1;
            vfs_attach(dev, shm);
        }
    }
}
