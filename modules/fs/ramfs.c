#include "fs/ramfs.h"
#include "drivers/memory/vmm.h"
#include "fs/vfs.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* Static heap for file data */
/* the heap used to be a static BSS array, but this kernel
 * links at VA==phys 1 MiB, and an 8 MiB BSS object extends well past
 * 0x400000 where user images load.  A task's address space therefore
 * aliases kernel BSS with its own code/data: any file buffer above
 * 0x400000 read as garbage inside syscalls (observed: /etc/passwd
 * served login's own .text after its buffer crossed the line).
 *
 * The heap now lives in the kernel half at PML4 entry 511
 * (0xFFFFFF8000000000) - shared by reference across every address
 * space (USER_PML4_LIMIT = 4), so it is valid in bootstrap CR3 and in
 * every fork/exec'd task alike, and no user page operation can ever
 * flip, free or shadow it.                                            */
static uint8_t *rfs_heap_base = 0;
static size_t   rfs_heap_top = 0;
static uint64_t rfs_heap_vma  = 0;   /* PML4 entry chosen at boot */

bool ramfs_heap_init(void) {
    if (rfs_heap_base) return true;

    /* Find an absent PML4 entry in the kernel half (entries 300..500 are
     * never used by the bootstrap tables: 0 = identity 1 GiB, 511 =
     * higher-half kernel).  A fresh entry avoids vmm_map's huge-page
     * passthrough path entirely. */
    extern void *vmm_get_pml4(void);
    uint64_t *top = (uint64_t *)vmm_get_pml4();
    int entry = -1;
    for (int i = 300; i <= 500; i++) {
        if (!(top[i] & 0x1ULL)) { entry = i; break; }   /* PTE_PRESENT bit */
    }
    if (entry < 0) return false;
    /* Sign-extend: entries 256..511 are kernel-half and their VAs must
     * have bits 63:48 all set, or the address is non-canonical (#GP). */
    uint64_t vma = ((uint64_t)entry << 39)
                 | (entry >= 256 ? 0xFFFF000000000000ULL : 0ULL);

    uint64_t r = vmm_alloc_pages(vma,
                                 RAMFS_HEAP_SIZE / PAGE_SIZE,
                                 PTE_PRESENT | PTE_WRITABLE);
    if (r != vma) return false;
    rfs_heap_base = (uint8_t *)(uintptr_t)vma;
    rfs_heap_vma  = vma;
    return true;
}

static void *rfs_alloc(size_t n) {
    if (!rfs_heap_base) return NULL;
    if (rfs_heap_top + n > RAMFS_HEAP_SIZE) return NULL;
    void *p = rfs_heap_base + rfs_heap_top;
    rfs_heap_top += n;
    return p;
}

/* Private file metadata */
typedef struct {
    uint8_t *data;
    size_t   capacity;
    size_t   size;
} rfs_file_t;

static rfs_file_t file_pool[RAMFS_MAX_FILES];
static int        file_pool_top = 0;

static rfs_file_t *rfs_file_alloc(void) {
    if (file_pool_top >= RAMFS_MAX_FILES) return NULL;
    rfs_file_t *f = &file_pool[file_pool_top++];
    f->data = NULL; f->capacity = 0; f->size = 0;
    return f;
}

/* str helpers */
static void rfs_strcpy(char *d, const char *s, size_t max) {
    size_t i = 0;
    while (s[i] && i < max - 1) { d[i] = s[i]; i++; }
    d[i] = '\0';
}
static int rfs_strcmp(const char *a, const char *b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}
static void rfs_memcpy(void *d, const void *s, size_t n) {
    uint8_t *dd = (uint8_t *)d;
    const uint8_t *ss = (const uint8_t *)s;
    while (n--) *dd++ = *ss++;
}

/* ops forward decl */
static int         rfs_file_read   (vfs_node_t *, void *, size_t, size_t);
static int         rfs_file_write  (vfs_node_t *, const void *, size_t, size_t);
static int         rfs_dir_readdir (vfs_node_t *, int, char *, uint8_t *);
static vfs_node_t *rfs_dir_finddir (vfs_node_t *, const char *);
static vfs_node_t *rfs_dir_mknod   (vfs_node_t *, const char *, uint8_t);
static int         rfs_dir_unlink  (vfs_node_t *, const char *);

static const vfs_ops_t rfs_file_ops = {
    .read  = rfs_file_read,
    .write = rfs_file_write,
};

static const vfs_ops_t rfs_dir_ops = {
    .readdir = rfs_dir_readdir,
    .finddir = rfs_dir_finddir,
    .mknod   = rfs_dir_mknod,
    .unlink  = rfs_dir_unlink,
};

/* File ops */
static int rfs_file_read(vfs_node_t *node, void *buf, size_t len, size_t off) {
    rfs_file_t *f = (rfs_file_t *)node->data;
    if (!f || off >= f->size) return 0;
    size_t avail = f->size - off;
    if (len > avail) len = avail;
    rfs_memcpy(buf, f->data + off, len);
    return (int)len;
}

static int rfs_file_write(vfs_node_t *node, const void *buf, size_t len, size_t off) {
    rfs_file_t *f = (rfs_file_t *)node->data;
    if (!f) return -1;

    /* NULL buf + len=0 = truncate */
    if (!buf && len == 0) {
        f->size     = 0;
        node->size  = 0;
        return 0;
    }

    size_t end = off + len;
    if (end > f->capacity) {
        /* Grow: allocate new chunk (simple bump allocator, no reuse) */
        size_t newcap = end + 256;   /* some slack */
        uint8_t *nd = (uint8_t *)rfs_alloc(newcap);
        if (!nd) return -1;
        if (f->data) rfs_memcpy(nd, f->data, f->size);
        f->data     = nd;
        f->capacity = newcap;
    }

    rfs_memcpy(f->data + off, buf, len);
    if (end > f->size) { f->size = end; node->size = (uint32_t)end; }
    return (int)len;
}

/* Dir ops */
static int rfs_dir_readdir(vfs_node_t *node, int idx,
                             char *name_out, uint8_t *type_out) {
    if (idx < 0 || idx >= node->child_count) return -1;
    rfs_strcpy(name_out, node->children[idx]->name, VFS_NAME_MAX);
    *type_out = node->children[idx]->type;
    return 0;
}

static vfs_node_t *rfs_dir_finddir(vfs_node_t *node, const char *name) {
    for (int i = 0; i < node->child_count; i++)
        if (!rfs_strcmp(node->children[i]->name, name))
            return node->children[i];
    return NULL;
}

static vfs_node_t *rfs_dir_mknod(vfs_node_t *parent,
                                   const char *name, uint8_t type) {
    /* Refuse duplicates */
    if (rfs_dir_finddir(parent, name)) {
        vfs_errno = VFS_EEXIST;
        return NULL;
    }
    if (parent->child_count >= 32) {
        vfs_errno = VFS_ENOSPC;
        return NULL;
    }

    vfs_node_t *n;
    if (type == VFS_TYPE_FILE) {
        rfs_file_t *f = rfs_file_alloc();
        if (!f) { vfs_errno = VFS_ENOMEM; return NULL; }
        n = vfs_create_node(name, VFS_TYPE_FILE, &rfs_file_ops, f);
    } else {
        n = vfs_create_node(name, VFS_TYPE_DIR, &rfs_dir_ops, NULL);
    }
    if (!n) return NULL;

    n->parent = parent;
    parent->children[parent->child_count++] = n;
    return n;
}

static int rfs_dir_unlink(vfs_node_t *dir, const char *name) {
    for (int i = 0; i < dir->child_count; i++) {
        if (!rfs_strcmp(dir->children[i]->name, name)) {
            vfs_node_t *v = dir->children[i];
            if (v->type == VFS_TYPE_DIR && v->child_count > 0)
                return -(int)VFS_ENOTEMPTY;
            /* Compact */
            for (int j = i; j < dir->child_count - 1; j++)
                dir->children[j] = dir->children[j + 1];
            dir->child_count--;
            return 0;
        }
    }
    return -(int)VFS_ENOENT;
}

/* Public API */
vfs_node_t *ramfs_init(void) {
    rfs_heap_top    = 0;
    file_pool_top   = 0;
    vfs_node_t *root = vfs_create_node("/", VFS_TYPE_DIR, &rfs_dir_ops, NULL);
    return root;
}

vfs_node_t *ramfs_mkdir(vfs_node_t *parent, const char *name) {
    return rfs_dir_mknod(parent, name, VFS_TYPE_DIR);
}

vfs_node_t *ramfs_mkfile(vfs_node_t *parent, const char *name,
                          const void *data, size_t size) {
    vfs_node_t *n = rfs_dir_mknod(parent, name, VFS_TYPE_FILE);
    if (!n) return NULL;
    if (data && size > 0) {
        rfs_file_t *f = (rfs_file_t *)n->data;
        uint8_t *buf = (uint8_t *)rfs_alloc(size);
        if (!buf) return NULL;
        rfs_memcpy(buf, data, size);
        f->data = buf; f->capacity = size; f->size = size;
        n->size = (uint32_t)size;
    }
    return n;
}

size_t ramfs_dbg_heap_top(void) { return rfs_heap_top; }
