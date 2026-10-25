#include "fs/ext4.h"
#include "fs/vfs.h"
#include "drivers/ata/ata.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/*
 *  On-disk structures  (all little-endian)
 */

#define EXT4_SUPER_MAGIC   0xEF53
#define EXT4_ROOT_INO      2

/* s_feature_incompat flags we care about */
#define EXT4_FEAT_INCOMPAT_EXTENTS   0x0040
#define EXT4_FEAT_INCOMPAT_64BIT     0x0080
#define EXT4_FEAT_INCOMPAT_INLINE    0x8000   /* inline data */

/* inode flags */
#define EXT4_EXTENTS_FL   0x00080000
#define EXT4_INLINE_FL    0x10000000

/* file types in dir entries */
#define EXT4_FT_REG_FILE  1
#define EXT4_FT_DIR       2
#define EXT4_FT_SYMLINK   7

typedef struct __attribute__((packed)) {
    uint32_t s_inodes_count;
    uint32_t s_blocks_count_lo;
    uint32_t s_r_blocks_count_lo;
    uint32_t s_free_blocks_count_lo;
    uint32_t s_free_inodes_count;
    uint32_t s_first_data_block;
    uint32_t s_log_block_size;    /* block_size = 1024 << s_log_block_size */
    uint32_t s_log_cluster_size;
    uint32_t s_blocks_per_group;
    uint32_t s_clusters_per_group;
    uint32_t s_inodes_per_group;
    uint32_t s_mtime;
    uint32_t s_wtime;
    uint16_t s_mnt_count;
    uint16_t s_max_mnt_count;
    uint16_t s_magic;
    uint16_t s_state;
    uint16_t s_errors;
    uint16_t s_minor_rev_level;
    uint32_t s_lastcheck;
    uint32_t s_checkinterval;
    uint32_t s_creator_os;
    uint32_t s_rev_level;
    uint16_t s_def_resuid;
    uint16_t s_def_resgid;
    /* EXT2_DYNAMIC_REV fields */
    uint32_t s_first_ino;
    uint16_t s_inode_size;
    uint16_t s_block_group_nr;
    uint32_t s_feature_compat;
    uint32_t s_feature_incompat;
    uint32_t s_feature_ro_compat;
    uint8_t  s_uuid[16];
    char     s_volume_name[16];
    char     s_last_mounted[64];
    uint32_t s_algo_bitmap;
    /* padding to 1024 bytes */
    uint8_t  _pad[820];
} ext4_super_t;

typedef struct __attribute__((packed)) {
    uint32_t bg_block_bitmap_lo;
    uint32_t bg_inode_bitmap_lo;
    uint32_t bg_inode_table_lo;
    uint16_t bg_free_blocks_count_lo;
    uint16_t bg_free_inodes_count_lo;
    uint16_t bg_used_dirs_count_lo;
    uint16_t bg_flags;
    uint32_t bg_exclude_bitmap_lo;
    uint16_t bg_block_bitmap_csum_lo;
    uint16_t bg_inode_bitmap_csum_lo;
    uint16_t bg_itable_unused_lo;
    uint16_t bg_checksum;
    /* 64-bit extension (present when s_desc_size >= 64) */
    uint32_t bg_block_bitmap_hi;
    uint32_t bg_inode_bitmap_hi;
    uint32_t bg_inode_table_hi;
    uint16_t bg_free_blocks_count_hi;
    uint16_t bg_free_inodes_count_hi;
    uint16_t bg_used_dirs_count_hi;
    uint16_t bg_itable_unused_hi;
    uint32_t bg_exclude_bitmap_hi;
    uint16_t bg_block_bitmap_csum_hi;
    uint16_t bg_inode_bitmap_csum_hi;
    uint32_t bg_reserved;
} ext4_bgd_t;

typedef struct __attribute__((packed)) {
    uint16_t i_mode;
    uint16_t i_uid;
    uint32_t i_size_lo;
    uint32_t i_atime;
    uint32_t i_ctime;
    uint32_t i_mtime;
    uint32_t i_dtime;
    uint16_t i_gid;
    uint16_t i_links_count;
    uint32_t i_blocks_lo;
    uint32_t i_flags;
    uint32_t i_osd1;
    uint32_t i_block[15];     /* extent tree root OR block map */
    uint32_t i_generation;
    uint32_t i_file_acl_lo;
    uint32_t i_size_high;
    uint32_t i_obso_faddr;
    uint8_t  i_osd2[12];
    uint16_t i_extra_isize;
    uint16_t i_checksum_hi;
    uint32_t i_ctime_extra;
    uint32_t i_mtime_extra;
    uint32_t i_atime_extra;
    uint32_t i_crtime;
    uint32_t i_crtime_extra;
    uint32_t i_version_hi;
    uint32_t i_projid;
} ext4_inode_t;

/* Extent header */
typedef struct __attribute__((packed)) {
    uint16_t eh_magic;     /* 0xF30A */
    uint16_t eh_entries;
    uint16_t eh_max;
    uint16_t eh_depth;
    uint32_t eh_generation;
} ext4_extent_header_t;

/* Extent leaf node */
typedef struct __attribute__((packed)) {
    uint32_t ee_block;     /* first logical block */
    uint16_t ee_len;       /* number of blocks (bit 15 = unwritten) */
    uint16_t ee_start_hi;
    uint32_t ee_start_lo;
} ext4_extent_t;

/* Extent internal node */
typedef struct __attribute__((packed)) {
    uint32_t ei_block;
    uint32_t ei_leaf_lo;
    uint16_t ei_leaf_hi;
    uint16_t ei_unused;
} ext4_extent_idx_t;

/* Directory entry (linear / htree) */
typedef struct __attribute__((packed)) {
    uint32_t inode;
    uint16_t rec_len;
    uint8_t  name_len;
    uint8_t  file_type;
    char     name[255];
} ext4_dirent_t;

/*
 *  Filesystem state
 */

#define EXT4_MAX_MOUNTS  4   /* support up to 4 mounted ext4 partitions */
#define EXT4_BLOCK_BUF   4096

typedef struct {
    bool     active;
    int      bus;
    int      drive;
    uint64_t part_lba;       /* partition start LBA */
    uint32_t block_size;     /* bytes per block */
    uint32_t inodes_per_group;
    uint32_t inode_size;
    uint32_t blocks_per_group;
    uint32_t first_data_block;
    uint64_t block_count;
    uint32_t bg_count;
    bool     feat_64bit;
    bool     feat_extents;
    uint16_t desc_size;      /* group descriptor size */
    /* block group descriptor table starts at block 1 (or 2 for 1k blocks) */
    uint64_t bgdt_block;
} ext4_fs_t;

static ext4_fs_t g_ext4[EXT4_MAX_MOUNTS];

/* Helpers */
static void e4_memzero(void *p, size_t n) {
    uint8_t *b = (uint8_t *)p; while (n--) *b++ = 0;
}
static int e4_streq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (*a == *b);
}
static void e4_memcpy(void *d, const void *s, size_t n) {
    uint8_t *dd=(uint8_t*)d; const uint8_t *ss=(const uint8_t*)s;
    while(n--) *dd++=*ss++;
}

static void e4_strcpy(char *d, const char *s, size_t n) {
    size_t i=0; while(s[i]&&i<n-1){d[i]=s[i];i++;} d[i]='\0';
}

/* Convert block number -> LBA */
static uint64_t block_to_lba(ext4_fs_t *fs, uint64_t blk) {
    uint64_t bytes_per_lba = ATA_SECTOR_SIZE;
    uint64_t sectors_per_block = fs->block_size / bytes_per_lba;
    return fs->part_lba + blk * sectors_per_block;
}

/* Read a full filesystem block into buf (must be >= block_size bytes) */
static int read_block(ext4_fs_t *fs, uint64_t blk, void *buf) {
    uint64_t lba = block_to_lba(fs, blk);
    size_t   cnt = fs->block_size / ATA_SECTOR_SIZE;
    return ata_read(fs->bus, fs->drive, lba, cnt, buf);
}

/* Block group descriptor */
static int read_bgd(ext4_fs_t *fs, uint32_t group, ext4_bgd_t *out) {
    uint64_t offset  = (uint64_t)group * fs->desc_size;
    uint64_t bgd_lba = block_to_lba(fs, fs->bgdt_block)
                       + offset / ATA_SECTOR_SIZE;
    /* Read 2 sectors so BGDs that straddle a sector boundary are captured */
    uint8_t  sec[ATA_SECTOR_SIZE * 2];
    if (ata_read(fs->bus, fs->drive, bgd_lba, 2, sec) != ATA_OK)
        return EXT4_ERR_IO;
    e4_memcpy(out, sec + (offset % ATA_SECTOR_SIZE), sizeof(ext4_bgd_t));
    return EXT4_OK;
}

/* Inode reader */
static int read_inode(ext4_fs_t *fs, uint32_t ino, ext4_inode_t *out) {
    if (ino == 0) return EXT4_ERR_INVAL;
    uint32_t group  = (ino - 1) / fs->inodes_per_group;
    uint32_t idx    = (ino - 1) % fs->inodes_per_group;

    ext4_bgd_t bgd;
    if (read_bgd(fs, group, &bgd) != EXT4_OK) return EXT4_ERR_IO;

    uint64_t itable_block = (uint64_t)bgd.bg_inode_table_lo;
    if (fs->feat_64bit)
        itable_block |= ((uint64_t)bgd.bg_inode_table_hi << 32);

    uint64_t inode_offset = (uint64_t)idx * fs->inode_size;
    uint64_t lba = block_to_lba(fs, itable_block)
                   + inode_offset / ATA_SECTOR_SIZE;

    /* Read 2 sectors so we can safely handle inodes that straddle a
       sector boundary (e.g. inode_size=256, offset=384 within sector). */
    uint8_t sec[ATA_SECTOR_SIZE * 2];
    if (ata_read(fs->bus, fs->drive, lba, 2, sec) != ATA_OK)
        return EXT4_ERR_IO;

    size_t copy_sz = fs->inode_size < sizeof(ext4_inode_t)
                   ? fs->inode_size : sizeof(ext4_inode_t);
    e4_memzero(out, sizeof(ext4_inode_t));
    e4_memcpy(out, sec + (inode_offset % ATA_SECTOR_SIZE), copy_sz);
    return EXT4_OK;
}

/* Extent tree: map logical block -> physical block */
#define EXTENT_MAGIC 0xF30A

static int extent_lookup(ext4_fs_t *fs, const ext4_inode_t *ino,
                          uint32_t logical, uint64_t *phys_out) {
    /* The 60-byte i_block[] holds the root of the extent tree */
    const uint8_t *root_data = (const uint8_t *)ino->i_block;
    const ext4_extent_header_t *hdr = (const ext4_extent_header_t *)root_data;

    if (hdr->eh_magic != EXTENT_MAGIC) return EXT4_ERR_INVAL;

    /* Walk the tree depth-first (depth 0 = leaf) */
    uint8_t  blk_buf[EXT4_BLOCK_BUF];
    const uint8_t *data = root_data;

    for (;;) {
        hdr = (const ext4_extent_header_t *)data;
        if (hdr->eh_magic != EXTENT_MAGIC) return EXT4_ERR_INVAL;

        if (hdr->eh_depth == 0) {
            /* Leaf: scan extents */
            const ext4_extent_t *ext =
                (const ext4_extent_t *)(data + sizeof(ext4_extent_header_t));
            for (int i = 0; i < hdr->eh_entries; i++) {
                uint32_t start = ext[i].ee_block;
                uint16_t len   = ext[i].ee_len & 0x7FFF; /* mask unwritten bit */
                if (logical >= start && logical < start + len) {
                    uint64_t phys = (uint64_t)ext[i].ee_start_lo
                                  | ((uint64_t)ext[i].ee_start_hi << 32);
                    *phys_out = phys + (logical - start);
                    return EXT4_OK;
                }
            }
            return EXT4_ERR_INVAL; /* sparse / hole */
        } else {
            /* Internal node: find the right child */
            const ext4_extent_idx_t *idx =
                (const ext4_extent_idx_t *)(data + sizeof(ext4_extent_header_t));
            int best = -1;
            for (int i = 0; i < hdr->eh_entries; i++) {
                if (idx[i].ei_block <= logical) best = i;
                else break;
            }
            if (best < 0) return EXT4_ERR_INVAL;

            uint64_t child_blk = (uint64_t)idx[best].ei_leaf_lo
                               | ((uint64_t)idx[best].ei_leaf_hi << 32);
            if (read_block(fs, child_blk, blk_buf) != ATA_OK)
                return EXT4_ERR_IO;
            data = blk_buf;
        }
    }
}

/* Indirect block map (ext2/ext3 style) */
static int indirect_lookup(ext4_fs_t *fs, const ext4_inode_t *ino,
                             uint32_t logical, uint64_t *phys_out) {
    uint32_t ptrs_per_block = fs->block_size / 4;

    if (logical < 12) {
        /* Direct */
        *phys_out = ino->i_block[logical];
        return (*phys_out) ? EXT4_OK : EXT4_ERR_INVAL;
    }
    logical -= 12;

    uint8_t buf[EXT4_BLOCK_BUF];

    if (logical < ptrs_per_block) {
        /* Single indirect */
        if (!ino->i_block[12]) return EXT4_ERR_INVAL;
        if (read_block(fs, ino->i_block[12], buf) != ATA_OK) return EXT4_ERR_IO;
        *phys_out = ((uint32_t *)buf)[logical];
        return (*phys_out) ? EXT4_OK : EXT4_ERR_INVAL;
    }
    logical -= ptrs_per_block;

    if (logical < ptrs_per_block * ptrs_per_block) {
        /* Double indirect */
        if (!ino->i_block[13]) return EXT4_ERR_INVAL;
        if (read_block(fs, ino->i_block[13], buf) != ATA_OK) return EXT4_ERR_IO;
        uint32_t l1 = ((uint32_t *)buf)[logical / ptrs_per_block];
        if (!l1) return EXT4_ERR_INVAL;
        if (read_block(fs, l1, buf) != ATA_OK) return EXT4_ERR_IO;
        *phys_out = ((uint32_t *)buf)[logical % ptrs_per_block];
        return (*phys_out) ? EXT4_OK : EXT4_ERR_INVAL;
    }

    /* Triple indirect - uncommon, skip for now */
    return EXT4_ERR_INVAL;
}

/* Map logical block -> physical, respecting extent vs indirect */
static int block_map(ext4_fs_t *fs, const ext4_inode_t *ino,
                      uint32_t logical, uint64_t *phys_out) {
    if (ino->i_flags & EXT4_EXTENTS_FL)
        return extent_lookup(fs, ino, logical, phys_out);
    return indirect_lookup(fs, ino, logical, phys_out);
}

/* File read */
/* Read up to len bytes at file offset into buf, given the inode number.
 * Returns bytes read or negative error. */
static int ext4_inode_read(ext4_fs_t *fs, uint32_t ino_num,
                             void *buf, size_t len, size_t off) {
    ext4_inode_t ino;
    if (read_inode(fs, ino_num, &ino) != EXT4_OK) return EXT4_ERR_IO;

    uint64_t file_size = (uint64_t)ino.i_size_lo
                        | ((uint64_t)ino.i_size_high << 32);

    if (off >= file_size) return 0;
    size_t avail = (size_t)(file_size - off);
    if (len > avail) len = avail;

    uint8_t *out = (uint8_t *)buf;
    size_t   done = 0;
    uint8_t  blk_buf[EXT4_BLOCK_BUF];

    while (done < len) {
        uint32_t logical   = (uint32_t)((off + done) / fs->block_size);
        size_t   blk_off   = (off + done) % fs->block_size;
        size_t   to_copy   = fs->block_size - blk_off;
        if (to_copy > len - done) to_copy = len - done;

        /* Inline data: stored in i_block[] directly (max 60 bytes) */
        if (ino.i_flags & EXT4_INLINE_FL) {
            const uint8_t *inline_data = (const uint8_t *)ino.i_block;
            if (off + done + to_copy > 60) to_copy = 60 - (off + done);
            e4_memcpy(out + done, inline_data + blk_off, to_copy);
        } else {
            uint64_t phys;
            if (block_map(fs, &ino, logical, &phys) != EXT4_OK) break;
            if (read_block(fs, phys, blk_buf) != ATA_OK) break;
            e4_memcpy(out + done, blk_buf + blk_off, to_copy);
        }
        done += to_copy;
    }
    return (int)done;
}

/* Directory lookup */
static uint32_t ext4_dir_lookup(ext4_fs_t *fs, uint32_t dir_ino,
                                  const char *name) {
    ext4_inode_t ino;
    if (read_inode(fs, dir_ino, &ino) != EXT4_OK) return 0;

    uint64_t dir_size = (uint64_t)ino.i_size_lo
                       | ((uint64_t)ino.i_size_high << 32);
    uint8_t  blk_buf[EXT4_BLOCK_BUF];
    size_t   name_len = 0;
    while (name[name_len]) name_len++;

    uint64_t pos = 0;
    while (pos < dir_size) {
        uint32_t logical = (uint32_t)(pos / fs->block_size);
        uint64_t phys;
        if (block_map(fs, &ino, logical, &phys) != EXT4_OK) break;
        if (read_block(fs, phys, blk_buf) != ATA_OK) break;

        size_t block_pos = 0;
        while (block_pos < fs->block_size) {
            ext4_dirent_t *de = (ext4_dirent_t *)(blk_buf + block_pos);
            if (de->rec_len == 0) break;
            if (de->inode && de->name_len == name_len) {
                bool match = true;
                for (size_t i = 0; i < name_len; i++)
                    if (de->name[i] != name[i]) { match = false; break; }
                if (match) return de->inode;
            }
            block_pos += de->rec_len;
        }
        pos += fs->block_size;
    }
    return 0;
}

/* VFS node / private data */
typedef struct {
    ext4_fs_t *fs;
    uint32_t   ino_num;
} e4_nodedata_t;

/* nodedata pool - one slot per possible open vfs_node.
 * We use a simple used[] bitmap so slots can be reused when
 * vfs_free_node() frees a node that e4_vfs_finddir() created. */
#define E4_NODEPOOL_SIZE 2048
static e4_nodedata_t e4_ndpool[E4_NODEPOOL_SIZE];
static bool          e4_ndpool_used[E4_NODEPOOL_SIZE];

static e4_nodedata_t *e4_nd_alloc(int fs_idx) {
    (void)fs_idx;
    for (int i = 0; i < E4_NODEPOOL_SIZE; i++) {
        if (!e4_ndpool_used[i]) {
            e4_ndpool_used[i] = true;
            return &e4_ndpool[i];
        }
    }
    return NULL; /* pool exhausted */
}

static void e4_nd_free(e4_nodedata_t *nd) {
    if (!nd) return;
    for (int i = 0; i < E4_NODEPOOL_SIZE; i++) {
        if (&e4_ndpool[i] == nd) {
            e4_ndpool_used[i] = false;
            return;
        }
    }
}

/* Forward declarations */
static int         e4_vfs_read   (vfs_node_t *, void *, size_t, size_t);
static int         e4_vfs_write  (vfs_node_t *, const void *, size_t, size_t);
static int         e4_vfs_readdir(vfs_node_t *, int, char *, uint8_t *);
static vfs_node_t *e4_vfs_finddir(vfs_node_t *, const char *);
static vfs_node_t *e4_vfs_mknod  (vfs_node_t *, const char *, uint8_t);
static int         e4_vfs_unlink (vfs_node_t *, const char *);
static int         e4_vfs_close  (vfs_node_t *);
static int         e4_vfs_setattr(vfs_node_t *node, uint32_t mode,
                                  uint32_t uid, uint32_t gid);
static int         e4_vfs_frename (vfs_node_t *node, vfs_node_t *newdir,
                                   const char *newname);

/* persist mode/uid/gid into the on-disk ext2 inode */
static int write_inode(ext4_fs_t *fs, uint32_t ino, const ext4_inode_t *in);

static int e4_vfs_setattr(vfs_node_t *node, uint32_t mode,
                          uint32_t uid, uint32_t gid) {
    e4_nodedata_t *nd = (e4_nodedata_t *)node->data;
    if (!nd) return -VFS_ENOENT;
    ext4_inode_t ino;
    if (read_inode(nd->fs, nd->ino_num, &ino) != EXT4_OK) return -VFS_EIO;
    if (mode) {
        /* keep the type bits already stored in i_mode */
        ino.i_mode = (uint16_t)((ino.i_mode & 0xF000u) | (mode & 0x0FFFu));
        node->mode = ino.i_mode;
    }
    if (uid != (uint32_t)-1) { ino.i_uid = (uint16_t)uid; node->uid = uid; }
    if (gid != (uint32_t)-1) { ino.i_gid = (uint16_t)gid; node->gid = gid; }
    return (write_inode(nd->fs, nd->ino_num, &ino) == EXT4_OK)
            ? 0 : -VFS_EIO;
}

static const vfs_ops_t e4_file_ops = { .read    = e4_vfs_read,
                                        .write   = e4_vfs_write,
                                        .close   = e4_vfs_close,
                                        .setattr = e4_vfs_setattr,
                                        .frename = e4_vfs_frename };
static const vfs_ops_t e4_dir_ops  = { .readdir = e4_vfs_readdir,
                                        .finddir = e4_vfs_finddir,
                                        .mknod   = e4_vfs_mknod,
                                        .unlink  = e4_vfs_unlink,
                                        .close   = e4_vfs_close,
                                        .setattr = e4_vfs_setattr,
                                        .frename = e4_vfs_frename };

static vfs_node_t *e4_make_node(int fs_idx, const char *name,
                                  uint32_t ino_num) {
    ext4_fs_t   *fs  = &g_ext4[fs_idx];
    ext4_inode_t ino;
    if (read_inode(fs, ino_num, &ino) != EXT4_OK) return NULL;

    uint8_t  vtype;
    uint16_t mode = ino.i_mode & 0xF000;
    if      (mode == 0x8000) vtype = VFS_TYPE_FILE;
    else if (mode == 0x4000) vtype = VFS_TYPE_DIR;
    else if (mode == 0xA000) vtype = VFS_TYPE_FILE; /* symlink treated as file */
    else                     vtype = VFS_TYPE_FILE;

    const vfs_ops_t *ops = (vtype == VFS_TYPE_DIR) ? &e4_dir_ops : &e4_file_ops;

    e4_nodedata_t *nd = e4_nd_alloc(fs_idx);
    if (!nd) return NULL;
    nd->fs      = fs;
    nd->ino_num = ino_num;

    vfs_node_t *n = vfs_create_node(name, vtype, ops, nd);
    if (!n) return NULL;

    n->size  = (uint32_t)ino.i_size_lo;
    n->inode = ino_num;
    /* Preserve the on-disk linux mode (type bits + rwx) and mtime so
     * stat()/ls -l report real permissions and timestamps. */
    n->mode  = ino.i_mode;
    n->mtime = ino.i_mtime;
    /* real ownership from the ext2 inode (i_uid @ offset 2, i_gid @ 24
     * in the ext2 rev-1 layout, both u16) */
    n->uid   = ino.i_uid;
    n->gid   = ino.i_gid;
    return n;
}

/* VFS ops implementations */
static int e4_vfs_close(vfs_node_t *node) {
    /* Never free the nodedata of a pinned node, pinned nodes are
     * mount-point roots that must stay alive for the filesystem lifetime.
     * Freeing their nodedata would corrupt the fs/ino_num pointers the
     * very next time the pool slot gets reused by a finddir call.
     *
     * Same for dcache nodes: a child attached to its parent directory by
     * e4_vfs_finddir() stays cached after the fd closes.  Freeing its
     * nodedata here would leave the cache entry pointing at a recycled
     * slot (another file's fs/ino), silent data confusion.  Attached
     * nodes keep their nodedata for the pool lifetime.                  */
    if (node && node->data && !node->pinned && node->parent == NULL)
        e4_nd_free((e4_nodedata_t *)node->data);
    return 0;
}

static int e4_vfs_read(vfs_node_t *node, void *buf, size_t len, size_t off) {
    e4_nodedata_t *nd = (e4_nodedata_t *)node->data;
    return ext4_inode_read(nd->fs, nd->ino_num, buf, len, off);
}

static int e4_vfs_readdir(vfs_node_t *dir, int idx,
                            char *name_out, uint8_t *type_out) {
    e4_nodedata_t *nd  = (e4_nodedata_t *)dir->data;
    ext4_fs_t     *fs  = nd->fs;
    ext4_inode_t   ino;
    if (read_inode(fs, nd->ino_num, &ino) != EXT4_OK) return -1;

    uint64_t dir_size = (uint64_t)ino.i_size_lo
                       | ((uint64_t)ino.i_size_high << 32);
    uint8_t  blk_buf[EXT4_BLOCK_BUF];

    int      count = 0;
    uint64_t pos   = 0;

    while (pos < dir_size) {
        uint32_t logical = (uint32_t)(pos / fs->block_size);
        uint64_t phys;
        if (block_map(fs, &ino, logical, &phys) != EXT4_OK) break;
        if (read_block(fs, phys, blk_buf) != ATA_OK) break;

        size_t bp = 0;
        while (bp < fs->block_size) {
            ext4_dirent_t *de = (ext4_dirent_t *)(blk_buf + bp);
            if (de->rec_len == 0) break;
            if (de->inode && de->name_len > 0 &&
                !(de->name_len == 1 && de->name[0] == '.') &&
                !(de->name_len == 2 && de->name[0] == '.' && de->name[1] == '.')) {
                if (count == idx) {
                    e4_strcpy(name_out, de->name, de->name_len + 1 < VFS_NAME_MAX
                              ? de->name_len + 1 : VFS_NAME_MAX);
                    name_out[de->name_len] = '\0';
                    *type_out = (de->file_type == EXT4_FT_DIR)
                                ? VFS_TYPE_DIR : VFS_TYPE_FILE;
                    return 0;
                }
                count++;
            }
            bp += de->rec_len;
        }
        pos += fs->block_size;
    }
    return -1; /* EOF */
}

static vfs_node_t *e4_vfs_finddir(vfs_node_t *dir, const char *name) {
    e4_nodedata_t *nd = (e4_nodedata_t *)dir->data;

    /* Determine fs_idx */
    int fs_idx = -1;
    for (int i = 0; i < EXT4_MAX_MOUNTS; i++)
        if (&g_ext4[i] == nd->fs) { fs_idx = i; break; }
    if (fs_idx < 0) return NULL;

    /* Children cache (dcache)
     * Every PATH search used to allocate a fresh node + nodedata per
     * component and never free it (resolve has no close), draining both
     * pools until execs failed with ENOENT under load.  Cache resolved
     * children on the directory node and reuse them instead.           */
    for (int i = 0; i < dir->child_count; i++)
        if (e4_streq(dir->children[i]->name, name))
            return dir->children[i];

    uint32_t child_ino = ext4_dir_lookup(nd->fs, nd->ino_num, name);
    if (!child_ino) return NULL;
    vfs_node_t *child = e4_make_node(fs_idx, name, child_ino);
    if (child) {
        if (!vfs_attach(dir, child)) {
            /* children[] full - keep the node alive (fd-close would
             * otherwise free a node another lookup may still return) */
            child->pinned = 1;
        }
    }
    return child;
}

/*
 *  Write helpers  (ext2 only, no extents written, direct block map)
 */

/* Write a full filesystem block */
static int write_block(ext4_fs_t *fs, uint64_t blk, const void *buf) {
    uint64_t lba = block_to_lba(fs, blk);
    size_t   cnt = fs->block_size / ATA_SECTOR_SIZE;
    return ata_write(fs->bus, fs->drive, lba, cnt, buf);
}

/* Write inode back to disk */
static int write_inode(ext4_fs_t *fs, uint32_t ino, const ext4_inode_t *in) {
    if (ino == 0) return EXT4_ERR_INVAL;
    uint32_t group = (ino - 1) / fs->inodes_per_group;
    uint32_t idx   = (ino - 1) % fs->inodes_per_group;

    ext4_bgd_t bgd;
    if (read_bgd(fs, group, &bgd) != EXT4_OK) return EXT4_ERR_IO;

    uint64_t itable_block = (uint64_t)bgd.bg_inode_table_lo;
    if (fs->feat_64bit) itable_block |= ((uint64_t)bgd.bg_inode_table_hi << 32);

    uint64_t inode_offset = (uint64_t)idx * fs->inode_size;
    uint64_t lba = block_to_lba(fs, itable_block) + inode_offset / ATA_SECTOR_SIZE;

    uint8_t sec[ATA_SECTOR_SIZE * 2];
    if (ata_read(fs->bus, fs->drive, lba, 2, sec) != ATA_OK) return EXT4_ERR_IO;
    size_t copy_sz = fs->inode_size < sizeof(ext4_inode_t)
                   ? fs->inode_size : sizeof(ext4_inode_t);
    e4_memcpy(sec + (inode_offset % ATA_SECTOR_SIZE), in, copy_sz);
    if (ata_write(fs->bus, fs->drive, lba, 2, sec) != ATA_OK) return EXT4_ERR_IO;
    return EXT4_OK;
}

/* Write BGD back to disk */
static int write_bgd(ext4_fs_t *fs, uint32_t group, const ext4_bgd_t *bgd) {
    uint64_t offset  = (uint64_t)group * fs->desc_size;
    uint64_t bgd_lba = block_to_lba(fs, fs->bgdt_block) + offset / ATA_SECTOR_SIZE;
    uint8_t  sec[ATA_SECTOR_SIZE * 2];
    if (ata_read(fs->bus, fs->drive, bgd_lba, 2, sec) != ATA_OK) return EXT4_ERR_IO;
    e4_memcpy(sec + (offset % ATA_SECTOR_SIZE), bgd, sizeof(ext4_bgd_t));
    if (ata_write(fs->bus, fs->drive, bgd_lba, 2, sec) != ATA_OK) return EXT4_ERR_IO;
    return EXT4_OK;
}

/* Update superblock free counts on disk */
static int write_superblock(ext4_fs_t *fs, uint32_t free_inodes_delta,
                             uint32_t free_blocks_delta, int sign) {
    uint64_t sb_lba = fs->part_lba + 2; /* superblock always at byte 1024 */
    uint8_t sec[ATA_SECTOR_SIZE * 2];
    if (ata_read(fs->bus, fs->drive, sb_lba, 2, sec) != ATA_OK) return EXT4_ERR_IO;
    ext4_super_t *sb = (ext4_super_t *)sec;
    if (sign < 0) {
        sb->s_free_inodes_count -= free_inodes_delta;
        sb->s_free_blocks_count_lo -= free_blocks_delta;
    } else {
        sb->s_free_inodes_count += free_inodes_delta;
        sb->s_free_blocks_count_lo += free_blocks_delta;
    }
    if (ata_write(fs->bus, fs->drive, sb_lba, 2, sec) != ATA_OK) return EXT4_ERR_IO;
    return EXT4_OK;
}

/* Allocate a free block in group `grp`, returns block number or 0 on fail */
static uint64_t alloc_block_in_group(ext4_fs_t *fs, uint32_t grp) {
    ext4_bgd_t bgd;
    if (read_bgd(fs, grp, &bgd) != EXT4_OK) return 0;
    if (bgd.bg_free_blocks_count_lo == 0) return 0;

    /* Read block bitmap (1 block) */
    uint8_t bitmap[EXT4_BLOCK_BUF];
    uint64_t bm_blk = bgd.bg_block_bitmap_lo;
    if (read_block(fs, bm_blk, bitmap) != ATA_OK) return 0;

    uint32_t bits = fs->blocks_per_group;
    for (uint32_t i = 0; i < bits; i++) {
        if (!(bitmap[i / 8] & (1u << (i % 8)))) {
            /* Mark used */
            bitmap[i / 8] |= (1u << (i % 8));
            write_block(fs, bm_blk, bitmap);
            bgd.bg_free_blocks_count_lo--;
            write_bgd(fs, grp, &bgd);
            write_superblock(fs, 0, 1, -1);
            return (uint64_t)grp * fs->blocks_per_group
                   + fs->first_data_block + i;
        }
    }
    return 0;
}

static uint64_t alloc_block(ext4_fs_t *fs) {
    for (uint32_t g = 0; g < fs->bg_count; g++) {
        uint64_t blk = alloc_block_in_group(fs, g);
        if (blk) return blk;
    }
    return 0;
}

/* Allocate a free inode, returns inode number or 0 on fail */
static uint32_t alloc_inode(ext4_fs_t *fs, bool is_dir) {
    for (uint32_t g = 0; g < fs->bg_count; g++) {
        ext4_bgd_t bgd;
        if (read_bgd(fs, g, &bgd) != EXT4_OK) continue;
        if (bgd.bg_free_inodes_count_lo == 0) continue;

        uint8_t bitmap[EXT4_BLOCK_BUF];
        uint64_t bm_blk = bgd.bg_inode_bitmap_lo;
        if (read_block(fs, bm_blk, bitmap) != ATA_OK) continue;

        for (uint32_t i = 0; i < fs->inodes_per_group; i++) {
            if (!(bitmap[i / 8] & (1u << (i % 8)))) {
                bitmap[i / 8] |= (1u << (i % 8));
                write_block(fs, bm_blk, bitmap);
                bgd.bg_free_inodes_count_lo--;
                if (is_dir) bgd.bg_used_dirs_count_lo++;
                write_bgd(fs, g, &bgd);
                write_superblock(fs, 1, 0, -1);
                return g * fs->inodes_per_group + i + 1;
            }
        }
    }
    return 0;
}

/* Free a block (clear its bitmap bit) */
static void free_block(ext4_fs_t *fs, uint64_t blk) {
    if (!blk) return;
    uint32_t base = (uint32_t)(blk - fs->first_data_block);
    uint32_t grp  = base / fs->blocks_per_group;
    uint32_t bit  = base % fs->blocks_per_group;
    ext4_bgd_t bgd;
    if (read_bgd(fs, grp, &bgd) != EXT4_OK) return;
    uint8_t bitmap[EXT4_BLOCK_BUF];
    if (read_block(fs, bgd.bg_block_bitmap_lo, bitmap) != ATA_OK) return;
    bitmap[bit / 8] &= ~(1u << (bit % 8));
    write_block(fs, bgd.bg_block_bitmap_lo, bitmap);
    bgd.bg_free_blocks_count_lo++;
    write_bgd(fs, grp, &bgd);
    write_superblock(fs, 0, 1, +1);
}

/* Free an inode (clear bitmap, zero inode) */
static void free_inode(ext4_fs_t *fs, uint32_t ino, bool is_dir) {
    if (!ino) return;
    uint32_t grp = (ino - 1) / fs->inodes_per_group;
    uint32_t bit = (ino - 1) % fs->inodes_per_group;
    ext4_bgd_t bgd;
    if (read_bgd(fs, grp, &bgd) != EXT4_OK) return;
    uint8_t bitmap[EXT4_BLOCK_BUF];
    if (read_block(fs, bgd.bg_inode_bitmap_lo, bitmap) != ATA_OK) return;
    bitmap[bit / 8] &= ~(1u << (bit % 8));
    write_block(fs, bgd.bg_inode_bitmap_lo, bitmap);
    bgd.bg_free_inodes_count_lo++;
    if (is_dir && bgd.bg_used_dirs_count_lo > 0) bgd.bg_used_dirs_count_lo--;
    write_bgd(fs, grp, &bgd);
    write_superblock(fs, 1, 0, +1);
    /* Zero out the on-disk inode */
    ext4_inode_t blank;
    e4_memzero(&blank, sizeof(blank));
    write_inode(fs, ino, &blank);
}

/* Add a directory entry to a directory inode.
 * Uses direct blocks only (max 12*block_size bytes per dir, plenty). */
static int dir_add_entry(ext4_fs_t *fs, uint32_t dir_ino,
                          uint32_t child_ino, uint8_t ftype,
                          const char *name) {
    uint8_t namelen = 0;
    while (name[namelen]) namelen++;

    ext4_inode_t dir_inode;
    if (read_inode(fs, dir_ino, &dir_inode) != EXT4_OK) return EXT4_ERR_IO;

    uint64_t dir_size = (uint64_t)dir_inode.i_size_lo;
    uint8_t  blk_buf[EXT4_BLOCK_BUF];

    /* Minimum record length (rounded to 4 bytes) */
    uint16_t need = (uint16_t)((8 + namelen + 3) & ~3u);

    /* Scan existing blocks for slack space in the last entry of each block */
    uint32_t nblocks = (uint32_t)((dir_size + fs->block_size - 1) / fs->block_size);
    for (uint32_t b = 0; b < nblocks && b < 12; b++) {
        uint64_t phys = dir_inode.i_block[b];
        if (!phys) continue;
        if (read_block(fs, phys, blk_buf) != ATA_OK) continue;

        size_t bp = 0;
        while (bp < fs->block_size) {
            ext4_dirent_t *de = (ext4_dirent_t *)(blk_buf + bp);
            if (de->rec_len == 0) break;
            uint16_t min_rec = (uint16_t)((8 + de->name_len + 3) & ~3u);
            uint16_t slack   = de->rec_len - min_rec;
            bool is_last = (bp + de->rec_len >= fs->block_size);
            if (is_last && slack >= need) {
                /* Shrink existing entry and append new one */
                uint16_t old_rec = de->rec_len;
                de->rec_len = min_rec;
                ext4_dirent_t *ne = (ext4_dirent_t *)(blk_buf + bp + min_rec);
                ne->inode     = child_ino;
                ne->rec_len   = old_rec - min_rec;
                ne->name_len  = namelen;
                ne->file_type = ftype;
                e4_memcpy(ne->name, name, namelen);
                write_block(fs, phys, blk_buf);
                return EXT4_OK;
            }
            bp += de->rec_len;
        }
    }

    /* No slack found, allocate a new block */
    if (nblocks >= 12) return EXT4_ERR_INVAL; /* out of direct blocks */
    uint64_t new_blk = alloc_block(fs);
    if (!new_blk) return EXT4_ERR_INVAL;
    e4_memzero(blk_buf, fs->block_size);

    ext4_dirent_t *ne = (ext4_dirent_t *)blk_buf;
    ne->inode     = child_ino;
    ne->rec_len   = (uint16_t)fs->block_size; /* spans whole block */
    ne->name_len  = namelen;
    ne->file_type = ftype;
    e4_memcpy(ne->name, name, namelen);
    write_block(fs, new_blk, blk_buf);

    dir_inode.i_block[nblocks] = (uint32_t)new_blk;
    dir_inode.i_size_lo += fs->block_size;
    dir_inode.i_blocks_lo += (uint32_t)(fs->block_size / ATA_SECTOR_SIZE);
    write_inode(fs, dir_ino, &dir_inode);
    return EXT4_OK;
}

/* Remove a directory entry by name from a directory inode */
static int dir_remove_entry(ext4_fs_t *fs, uint32_t dir_ino, const char *name) {
    uint8_t namelen = 0;
    while (name[namelen]) namelen++;

    ext4_inode_t dir_inode;
    if (read_inode(fs, dir_ino, &dir_inode) != EXT4_OK) return EXT4_ERR_IO;

    uint64_t dir_size = (uint64_t)dir_inode.i_size_lo;
    uint8_t  blk_buf[EXT4_BLOCK_BUF];

    uint32_t nblocks = (uint32_t)((dir_size + fs->block_size - 1) / fs->block_size);
    for (uint32_t b = 0; b < nblocks && b < 12; b++) {
        uint64_t phys = dir_inode.i_block[b];
        if (!phys) continue;
        if (read_block(fs, phys, blk_buf) != ATA_OK) continue;

        size_t bp = 0;
        ext4_dirent_t *prev = NULL;
        while (bp < fs->block_size) {
            ext4_dirent_t *de = (ext4_dirent_t *)(blk_buf + bp);
            if (de->rec_len == 0) break;
            if (de->inode && de->name_len == namelen) {
                bool match = true;
                for (int i = 0; i < namelen; i++)
                    if (de->name[i] != name[i]) { match = false; break; }
                if (match) {
                    if (prev) {
                        /* Merge this entry into previous */
                        prev->rec_len += de->rec_len;
                    } else {
                        /* First entry: zero out inode to mark deleted */
                        de->inode = 0;
                    }
                    write_block(fs, phys, blk_buf);
                    return EXT4_OK;
                }
            }
            prev = de;
            bp += de->rec_len;
        }
    }
    return EXT4_ERR_INVAL; /* not found */
}

/* Write VFS ops */

/* Write to a file, supports only direct blocks (up to 12*block_size bytes).
 * Creates/extends blocks as needed. O_TRUNC is handled by vfs_open. */
static int e4_vfs_write(vfs_node_t *node, const void *buf, size_t len, size_t off) {
    if (!buf || len == 0) {
        /* Truncate: zero out all data blocks and reset size */
        e4_nodedata_t *nd = (e4_nodedata_t *)node->data;
        ext4_inode_t ino;
        if (read_inode(nd->fs, nd->ino_num, &ino) != EXT4_OK) return -1;
        for (int i = 0; i < 12; i++) {
            if (ino.i_block[i]) { free_block(nd->fs, ino.i_block[i]); ino.i_block[i] = 0; }
        }
        ino.i_size_lo = 0;
        ino.i_blocks_lo = 0;
        write_inode(nd->fs, nd->ino_num, &ino);
        node->size = 0;
        return 0;
    }

    e4_nodedata_t *nd = (e4_nodedata_t *)node->data;
    ext4_fs_t *fs = nd->fs;
    ext4_inode_t ino;
    if (read_inode(fs, nd->ino_num, &ino) != EXT4_OK) return -1;

    uint8_t blk_buf[EXT4_BLOCK_BUF];
    size_t written = 0;

    while (written < len) {
        uint32_t logical = (uint32_t)((off + written) / fs->block_size);
        size_t   boff    = (off + written) % fs->block_size;
        size_t   chunk   = fs->block_size - boff;
        if (chunk > len - written) chunk = len - written;

        if (logical >= 12) return (int)written; /* beyond direct blocks */

        /* Allocate block if needed */
        if (!ino.i_block[logical]) {
            uint64_t new_blk = alloc_block(fs);
            if (!new_blk) return (int)written;
            e4_memzero(blk_buf, fs->block_size);
            write_block(fs, new_blk, blk_buf);
            ino.i_block[logical] = (uint32_t)new_blk;
            ino.i_blocks_lo += (uint32_t)(fs->block_size / ATA_SECTOR_SIZE);
        }

        if (read_block(fs, ino.i_block[logical], blk_buf) != ATA_OK) break;
        e4_memcpy(blk_buf + boff, (const uint8_t *)buf + written, chunk);
        write_block(fs, ino.i_block[logical], blk_buf);
        written += chunk;
    }

    uint32_t new_size = (uint32_t)(off + written);
    if (new_size > ino.i_size_lo) {
        ino.i_size_lo = new_size;
        node->size    = new_size;
    }
    write_inode(fs, nd->ino_num, &ino);
    return (int)written;
}

/* Create a child file or directory inside a directory node */
static vfs_node_t *e4_vfs_mknod(vfs_node_t *dir, const char *name, uint8_t type) {
    e4_nodedata_t *nd = (e4_nodedata_t *)dir->data;
    ext4_fs_t *fs = nd->fs;
    int fs_idx = -1;
    for (int i = 0; i < EXT4_MAX_MOUNTS; i++)
        if (&g_ext4[i] == fs) { fs_idx = i; break; }
    if (fs_idx < 0) return NULL;

    bool is_dir = (type == VFS_TYPE_DIR);
    uint32_t new_ino = alloc_inode(fs, is_dir);
    if (!new_ino) return NULL;

    /* Build and write the new inode */
    ext4_inode_t new_inode;
    e4_memzero(&new_inode, sizeof(new_inode));
    new_inode.i_mode       = is_dir ? (uint16_t)0x41ED  /* 040755 */
                                    : (uint16_t)0x81A4; /* 0100644 */
    new_inode.i_links_count = 1;
    /* owned by the creating process's effective credentials */
    new_inode.i_uid = (uint16_t)vfs_cred_euid();
    new_inode.i_gid = (uint16_t)vfs_cred_egid();

    if (is_dir) {
        /* Allocate one block for . and .. entries */
        uint64_t blk = alloc_block(fs);
        if (!blk) { free_inode(fs, new_ino, true); return NULL; }
        uint8_t blk_buf[EXT4_BLOCK_BUF];
        e4_memzero(blk_buf, fs->block_size);

        /* "." entry */
        ext4_dirent_t *dot = (ext4_dirent_t *)blk_buf;
        dot->inode     = new_ino;
        dot->rec_len   = 12;
        dot->name_len  = 1;
        dot->file_type = EXT4_FT_DIR;
        dot->name[0]   = '.';

        /* ".." entry */
        ext4_dirent_t *dotdot = (ext4_dirent_t *)(blk_buf + 12);
        dotdot->inode     = nd->ino_num;
        dotdot->rec_len   = (uint16_t)(fs->block_size - 12);
        dotdot->name_len  = 2;
        dotdot->file_type = EXT4_FT_DIR;
        dotdot->name[0]   = '.'; dotdot->name[1] = '.';

        write_block(fs, blk, blk_buf);
        new_inode.i_block[0]  = (uint32_t)blk;
        new_inode.i_size_lo   = fs->block_size;
        new_inode.i_blocks_lo = (uint32_t)(fs->block_size / ATA_SECTOR_SIZE);
    }

    write_inode(fs, new_ino, &new_inode);

    /* Add entry in parent directory */
    uint8_t ftype = is_dir ? EXT4_FT_DIR : EXT4_FT_REG_FILE;
    if (dir_add_entry(fs, nd->ino_num, new_ino, ftype, name) != EXT4_OK) {
        free_inode(fs, new_ino, is_dir);
        return NULL;
    }

    /* Return a VFS node for the new child, cache it on the parent so
     * the next finddir hits the dcache instead of re-reading the disk. */
    vfs_node_t *child = e4_make_node(fs_idx, name, new_ino);
    if (child) {
        if (!vfs_attach(dir, child)) child->pinned = 1;
    }
    return child;
}

/* Unlink (remove) a child entry from a directory */
static int e4_vfs_unlink(vfs_node_t *dir, const char *name) {
    e4_nodedata_t *nd = (e4_nodedata_t *)dir->data;
    ext4_fs_t *fs = nd->fs;

    /* Look up the child to get its inode number */
    uint32_t child_ino = ext4_dir_lookup(fs, nd->ino_num, name);
    if (!child_ino) return -(int)VFS_ENOENT;

    ext4_inode_t child_inode;
    if (read_inode(fs, child_ino, &child_inode) != EXT4_OK) return -1;

    bool is_dir = ((child_inode.i_mode & 0xF000) == 0x4000);

    /* Decrement link count, free if zero */
    if (child_inode.i_links_count > 0) child_inode.i_links_count--;
    if (child_inode.i_links_count == 0) {
        /* Free all direct data blocks */
        for (int i = 0; i < 12; i++) {
            if (child_inode.i_block[i])
                free_block(fs, child_inode.i_block[i]);
        }
        free_inode(fs, child_ino, is_dir);
    } else {
        write_inode(fs, child_ino, &child_inode);
    }

    int r = dir_remove_entry(fs, nd->ino_num, name);

    /* dcache invalidation: drop the cached child (if any) so a later
     * lookup re-reads the disk instead of resurrecting a deleted file. */
    for (int i = 0; i < dir->child_count; i++) {
        if (e4_streq(dir->children[i]->name, name)) {
            vfs_node_t *victim = dir->children[i];
            for (int j = i; j < dir->child_count - 1; j++)
                dir->children[j] = dir->children[j + 1];
            dir->child_count--;
            if (victim->data)
                e4_nd_free((e4_nodedata_t *)victim->data);
            victim->data = NULL;
            vfs_free_node(victim);
            break;
        }
    }
    return r;
}

/* On-disk rename: move node's directory entry to NEWDIR/NEWNAME
 * vfs_rename() only re-parents the cached VFS node, for ext2 the disk
 * state is the truth, so the directory entries must move too.  Without
 * this a renamed file silently kept its old name on disk and later
 * lookups/uniinks of the new name failed with ENOENT (systest
 * rename-unlink).                                                    */
static int e4_vfs_frename(vfs_node_t *node, vfs_node_t *newdir,
                          const char *newname) {
    e4_nodedata_t *nd = (e4_nodedata_t *)node->data;
    if (!nd || !node->parent) return -(int)VFS_EINVAL;
    e4_nodedata_t *old_dir_nd = (e4_nodedata_t *)node->parent->data;
    e4_nodedata_t *new_dir_nd = (e4_nodedata_t *)newdir->data;
    if (!old_dir_nd || !new_dir_nd) return -(int)VFS_EINVAL;
    if (old_dir_nd->fs != new_dir_nd->fs) return -(int)VFS_EXDEV;

    uint8_t ftype = (node->type == VFS_TYPE_DIR) ? EXT4_FT_DIR
                                                 : EXT4_FT_REG_FILE;
    /* add first, then remove, recoverable order */
    if (dir_add_entry(new_dir_nd->fs, new_dir_nd->ino_num, nd->ino_num,
                      ftype, newname) != EXT4_OK)
        return -(int)VFS_EIO;
    dir_remove_entry(old_dir_nd->fs, old_dir_nd->ino_num, node->name);
    return 0;
}


int ext4_probe(int bus, int drive, uint64_t lba_start) {
    /* Superblock is at byte 1024 from partition start = LBA+2 (512-byte sectors) */
    uint64_t sb_lba = lba_start + 2;
    uint8_t  sec[ATA_SECTOR_SIZE * 2];
    if (ata_read(bus, drive, sb_lba, 2, sec) != ATA_OK) return EXT4_ERR_IO;
    ext4_super_t *sb = (ext4_super_t *)sec;
    if (sb->s_magic != EXT4_SUPER_MAGIC) return EXT4_ERR_MAGIC;
    return EXT4_OK;
}

vfs_node_t *ext4_mount(int bus, int drive, uint64_t lba_start) {
    /* Find a free slot */
    int idx = -1;
    for (int i = 0; i < EXT4_MAX_MOUNTS; i++)
        if (!g_ext4[i].active) { idx = i; break; }
    if (idx < 0) return NULL;

    /* Read superblock */
    uint8_t sec[ATA_SECTOR_SIZE * 2];
    if (ata_read(bus, drive, lba_start + 2, 2, sec) != ATA_OK) return NULL;
    ext4_super_t *sb = (ext4_super_t *)sec;
    if (sb->s_magic != EXT4_SUPER_MAGIC) return NULL;

    ext4_fs_t *fs = &g_ext4[idx];
    e4_memzero(fs, sizeof(ext4_fs_t));

    fs->active           = true;
    fs->bus              = bus;
    fs->drive            = drive;
    fs->part_lba         = lba_start;
    fs->block_size       = 1024u << sb->s_log_block_size;
    fs->inodes_per_group = sb->s_inodes_per_group;
    fs->inode_size       = sb->s_inode_size ? sb->s_inode_size : 128;
    fs->blocks_per_group = sb->s_blocks_per_group;
    fs->first_data_block = sb->s_first_data_block;
    fs->feat_64bit       = (sb->s_feature_incompat & EXT4_FEAT_INCOMPAT_64BIT) != 0;
    fs->feat_extents     = (sb->s_feature_incompat & EXT4_FEAT_INCOMPAT_EXTENTS) != 0;

    /* Group descriptor size */
    fs->desc_size = fs->feat_64bit ? 64 : 32;

    /* BGDT starts at block 1 (or block 2 for 1k-block filesystems) */
    fs->bgdt_block = (fs->block_size == 1024) ? 2 : 1;

    /* Total block groups */
    uint64_t block_count = (uint64_t)sb->s_blocks_count_lo;
    fs->block_count = block_count;
    fs->bg_count = (uint32_t)((block_count + fs->blocks_per_group - 1)
                               / fs->blocks_per_group);

    /* Create root VFS node from inode 2 */
    vfs_node_t *root = e4_make_node(idx, "/", EXT4_ROOT_INO);
    return root;
}
