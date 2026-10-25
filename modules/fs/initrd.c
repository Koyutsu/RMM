#include "fs/initrd.h"
#include "fs/vfs.h"
#include "fs/ramfs.h"
#include "console/print.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* newc CPIO header (110 bytes, ASCII hex fields) */
typedef struct __attribute__((packed)) {
    char magic[6];      /* "070701" */
    char ino[8];
    char mode[8];
    char uid[8];
    char gid[8];
    char nlink[8];
    char mtime[8];
    char filesize[8];
    char devmajor[8];
    char devminor[8];
    char rdevmajor[8];
    char rdevminor[8];
    char namesize[8];
    char check[8];
} cpio_newc_hdr_t;

#define CPIO_HDR_SIZE  110
#define S_ISREG(m)   (((m) & 0170000) == 0100000)
#define S_ISDIR(m)   (((m) & 0170000) == 0040000)

/* Tiny helpers (no libc in kernel) */
static uint32_t hex8(const char *s) {
    uint32_t v = 0;
    for (int i = 0; i < 8; i++) {
        char c = s[i];
        uint32_t d;
        if (c >= '0' && c <= '9') d = (uint32_t)(c - '0');
        else if (c >= 'a' && c <= 'f') d = (uint32_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') d = (uint32_t)(c - 'A' + 10);
        else d = 0;
        v = (v << 4) | d;
    }
    return v;
}

static int k_strcmp(const char *a, const char *b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

static size_t k_strlen(const char *s) {
    size_t n = 0; while (s[n]) n++; return n;
}

/* Align up to 4-byte boundary */
static size_t align4(size_t x) { return (x + 3) & ~(size_t)3; }

/* Path helpers */

/* Given a full path like "bin/ls" or "./bin/ls", find or create
   the parent directory node under root and return the base name. */
static vfs_node_t *resolve_parent(vfs_node_t *root,
                                   const char *path,
                                   const char **basename_out)
{
    /* Skip leading "./" or "/" */
    while (*path == '.' && *(path+1) == '/') path += 2;
    while (*path == '/') path++;

    /* Find last '/' */
    const char *slash = NULL;
    for (const char *p = path; *p; p++) {
        if (*p == '/') slash = p;
    }

    if (!slash) {
        /* Top-level entry */
        *basename_out = path;
        return root;
    }

    /* Walk each component */
    vfs_node_t *cur = root;
    const char *seg = path;
    while (seg <= slash) {
        /* find end of this segment */
        const char *end = seg;
        while (*end && *end != '/') end++;
        if (end == seg) { seg++; continue; }

        /* copy segment name */
        char name[VFS_NAME_MAX];
        size_t len = (size_t)(end - seg);
        if (len >= VFS_NAME_MAX) len = VFS_NAME_MAX - 1;
        for (size_t i = 0; i < len; i++) name[i] = seg[i];
        name[len] = '\0';

        /* look up or create dir */
        vfs_node_t *child = (cur->ops && cur->ops->finddir)
                            ? cur->ops->finddir(cur, name) : NULL;
        if (!child) {
            child = ramfs_mkdir(cur, name);
        }
        if (!child) return NULL;
        cur = child;
        seg = end + 1;
    }
    *basename_out = slash + 1;
    return cur;
}

/* Main loader */
int initrd_load(vfs_node_t *root, const void *data, size_t size)
{
    if (!root || !data || size < CPIO_HDR_SIZE) return -1;

    const uint8_t *ptr = (const uint8_t *)data;
    const uint8_t *end = ptr + size;
    int count = 0;

    while (ptr + CPIO_HDR_SIZE <= end) {
        const cpio_newc_hdr_t *hdr = (const cpio_newc_hdr_t *)ptr;

        /* Validate magic */
        if (hdr->magic[0] != '0' || hdr->magic[1] != '7' ||
            hdr->magic[2] != '0' || hdr->magic[3] != '7' ||
            hdr->magic[4] != '0' || hdr->magic[5] != '1') {
            break;
        }

        uint32_t namesize = hex8(hdr->namesize);
        uint32_t filesize = hex8(hdr->filesize);
        uint32_t mode     = hex8(hdr->mode);

        /* Name immediately follows header */
        const char *name = (const char *)(ptr + CPIO_HDR_SIZE);

        /* Check for trailer (end of archive) */
        if (namesize >= 11 && k_strcmp(name, "TRAILER!!!") == 0) break;

        /* File data follows name (both padded to 4-byte boundary) */
        size_t name_padded = align4(CPIO_HDR_SIZE + namesize) - CPIO_HDR_SIZE;
        const uint8_t *file_data = ptr + CPIO_HDR_SIZE + name_padded;
        size_t next_offset = align4(CPIO_HDR_SIZE + name_padded + filesize);

        if (ptr + next_offset > end) break;

        /* Skip "." root entry */
        if (namesize == 2 && name[0] == '.' && name[1] == '\0') {
            ptr += next_offset;
            continue;
        }

        /* Skip leading "./" */
        const char *rname = name;
        if (rname[0] == '.' && rname[1] == '/') rname += 2;
        if (rname[0] == '/' ) rname++;

        if (k_strlen(rname) == 0) {
            ptr += next_offset;
            continue;
        }

        if (S_ISDIR(mode)) {
            /* Create directory (resolve_parent handles intermediates) */
            const char *basename = NULL;
            vfs_node_t *parent = resolve_parent(root, rname, &basename);
            if (parent && basename && k_strlen(basename) > 0) {
                vfs_node_t *existing = (parent->ops && parent->ops->finddir)
                                       ? parent->ops->finddir(parent, basename) : NULL;
                if (!existing) {
                    vfs_node_t *d = ramfs_mkdir(parent, basename);
                    if (d) {
                        /* Preserve the cpio entry's full linux mode + mtime
                         * so ls -l shows real permissions and dates. */
                        d->mode  = mode;
                        d->mtime = hex8(hdr->mtime);
                        /* Initrd content is built by the OS build and is
                         * root-owned by definition, force uid/gid 0 so a
                         * host uid leaking through cpio can never weaken
                         * /etc/shadow-style permission enforcement. */
                        d->uid   = 0;
                        d->gid   = 0;
                    }
                }
            }
            count++;
        } else if (S_ISREG(mode)) {
            /* Create regular file */
            const char *basename = NULL;
            vfs_node_t *parent = resolve_parent(root, rname, &basename);
            if (parent && basename && k_strlen(basename) > 0) {
                /* Remove existing file if present (idempotent) */
                vfs_node_t *existing = (parent->ops && parent->ops->finddir)
                                       ? parent->ops->finddir(parent, basename) : NULL;
                if (!existing) {
                    vfs_node_t *f = ramfs_mkfile(parent, basename,
                                                 file_data, filesize);
                    if (f) {
                        f->mode  = mode;
                        f->mtime = hex8(hdr->mtime);
                        f->uid   = 0;   /* see comment above: root-owned */
                        f->gid   = 0;
                    }
                    count++;
                }
            }
        }
        /* Skip symlinks, devices, etc. */

        ptr += next_offset;
    }

    return count;
}

/* Multiboot2 module tag finder */

/* Multiboot2 tag types */
#define MB2_TAG_END     0
#define MB2_TAG_MODULE  3

typedef struct __attribute__((packed)) {
    uint32_t type;
    uint32_t size;
} mb2_tag_hdr_t;

typedef struct __attribute__((packed)) {
    uint32_t type;   /* 3 */
    uint32_t size;
    uint32_t mod_start;
    uint32_t mod_end;
    char     string[0];
} mb2_tag_module_t;

bool initrd_load_from_mb2(vfs_node_t *root, uint64_t mb2_info_phys)
{
    if (!mb2_info_phys) return false;

    uint8_t  *base  = (uint8_t *)(uintptr_t)mb2_info_phys;
    uint32_t  total = *(uint32_t *)base;
    uint8_t  *ptr   = base + 8;
    uint8_t  *end   = base + total;

    while (ptr + sizeof(mb2_tag_hdr_t) <= end) {
        mb2_tag_hdr_t *tag = (mb2_tag_hdr_t *)ptr;
        if (tag->type == MB2_TAG_END) break;

        if (tag->type == MB2_TAG_MODULE) {
            mb2_tag_module_t *mod = (mb2_tag_module_t *)ptr;
            uint32_t mod_start = mod->mod_start;
            uint32_t mod_end   = mod->mod_end;
            size_t   mod_size  = mod_end - mod_start;

            writeInfo("initrd: loading CPIO from multiboot2 module");

            const void *cpio_data = (const void *)(uintptr_t)mod_start;
            int entries = initrd_load(root, cpio_data, mod_size);

            if (entries >= 0) {
                writeOK("initrd: loaded");
                return true;
            } else {
                writeWarn("initrd: CPIO parse failed");
                return false;
            }
        }

        ptr += (tag->size + 7) & ~7u;
    }

    writeWarn("initrd: no multiboot2 module found - falling back to built-in files");
    return false;
}