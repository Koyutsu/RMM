/*
 * loader.c - RMM module image loader.
 *
 * Permanently resident, core-only: the loader depends on the PMM, the
 * page-table layer and the in-image module archive, never on a
 * demand-loaded module.  Images are position-fixed at link time
 * (no runtime relocations) and mapped W^X with guard pages.
 */
#include <stdatomic.h>
#include <rmm/rmm.h>
#include <rmm/module.h>
#include <rmm/image.h>
#include <rmm/fault.h>
#include <rmm/arch.h>
#include <rmm/modules.generated.h>
#include "console/print.h"

/* archive bounds (archive.S: .incbin into .rmm.archive) */
extern const uint8_t __rmm_archive_start[];
extern const uint8_t __rmm_archive_end[];

void writeWarn(const char *);

struct rmm_arch_entry {
    char     name[16];      /* natural alignment: offset lands at +16 */
    uint64_t offset;      /* blob offset inside the archive          */
    uint64_t size;        /* header + image bytes                    */
    uint64_t img_start;   /* blob[0] corresponds to slot + img_start */
};

static const uint8_t *archive_base;
static uint64_t       archive_len;
static uint32_t       archive_count;

static bool strneq(const char *a, const char *b, uint32_t n);
void loader_unload_image(rmm_module_t *m);

void loader_init(void) {
    archive_base = __rmm_archive_start;
    archive_len  = (uint64_t)(uintptr_t)(__rmm_archive_end - __rmm_archive_start);
    if (archive_len < 4) {
        writeWarn("RMM: module archive missing");
        archive_count = 0;
        return;
    }
    archive_count = *(const uint32_t *)archive_base;
    printf("  [RMM archive: %u module image%s, %u KiB]\n", archive_count,
           archive_count == 1 ? "" : "s", (unsigned)(archive_len >> 10));
}

static const struct rmm_arch_entry *archive_find(const char *name) {
    const uint8_t *p = archive_base + 4;
    for (uint32_t i = 0; i < archive_count; i++, p += sizeof(struct rmm_arch_entry)) {
        const struct rmm_arch_entry *e = (const struct rmm_arch_entry *)p;
        if (strneq(e->name, name, sizeof(e->name))) return e;
    }
    return NULL;
}

static bool strneq(const char *a, const char *b, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        if (a[i] != b[i]) return false;
        if (!a[i]) return true;
    }
    return true;
}

/* The archive blob is [image header][flat image].  The header's segment
 * offsets are relative to the flat image ("into the flat binary" per
 * image.h - the generator computes them as <vma> - <slot base>), not
 * relative to the blob.  Every byte-identical copy in this file must go
 * through img[] - indexing blob[] directly silently shifts the whole
 * module by sizeof(header) bytes and leaves the veneer prefix unmapped
 * garbage (boot-breaking: entry points call veneers first). */
_Static_assert(sizeof(struct rmm_image_header) == 144,
               "rmm_image_header must match the rmm-gen image header layout");

/* validation: the loader never trusts the image just because it
 * came from the archive. */
static int validate_image(const struct rmm_image_header *h, uint64_t blob_len,
                          struct rmm_module_descriptor *m, uint64_t img_start) {
    uint64_t img_len = blob_len - sizeof(struct rmm_image_header);
    if (blob_len < sizeof(struct rmm_image_header))  return -11;
    if (h->magic != RMM_MAGIC)                    return -1;
    if (h->version != RMM_IMAGE_VERSION)          return -2;
#if defined(__x86_64__)
    if (h->arch != RMM_ARCH_X86_64)               return -3;
#endif
    if (h->module_id != m->id)                    return -4;
    if (h->abi_version != RMM_ABI_VERSION)        return -5;

    /* segment bounds (offsets are flat-image-relative); empty segments are
     * legal (a module may have no .rodata/.data at all) */
    if (h->code_offset + h->code_size > img_len) return -6;
    if (h->rodata_size && h->rodata_offset + h->rodata_size > img_len) return -7;
    if (h->data_size && h->data_offset + h->data_size > img_len) return -8;

    /* W^X sanity: no segment claims both write and exec, the
     * mapping below enforces this by construction. */

    /* slot fit: highest segment + its guard must stay in-slot */
    uint64_t seg_top = img_start + h->data_offset + h->data_size;
    if (seg_top < img_start + h->code_offset + h->code_size)
        seg_top = img_start + h->code_offset + h->code_size;
    if (seg_top < img_start + h->rodata_offset + h->rodata_size)
        seg_top = img_start + h->rodata_offset + h->rodata_size;
    if (seg_top + RMM_GUARD_BYTES > m->size)      return -9;

    /* bss fits too (layout) */
    uint64_t bss_top = img_start + h->data_offset + h->data_size +
                       RMM_GUARD_BYTES + h->bss_size;
    if (bss_top + RMM_GUARD_BYTES > m->size)      return -10;
    return 0;
}

static bool map_range(uint64_t va_start, uint64_t va_end, uint64_t flags) {
    va_start &= ~(uint64_t)(RMM_GUARD_BYTES - 1);
    va_end    = (va_end + RMM_GUARD_BYTES - 1) & ~(uint64_t)(RMM_GUARD_BYTES - 1);
    for (uint64_t va = va_start; va < va_end; va += RMM_GUARD_BYTES) {
        uint64_t phys = arch_page_alloc();
        if (!phys) return false;
        if (!arch_map(va, phys, flags)) {
            arch_page_free(phys);
            return false;
        }
    }
    return true;
}

/* : load image, construct page tables, populate, protect,
 * flush - then READY (caller's job). */
int loader_load_image(rmm_module_t *m) {
    if (!archive_count) return RMM_ERR_MODULE_LOAD;
    printf("[dbg] loader %s: searching archive\n", m->name);
    const struct rmm_arch_entry *e = archive_find(m->name);
    printf("[dbg] archive entry %s\n", e ? "found" : "MISSING");
    if (!e) {
        writeWarn("RMM: no image in archive for module");
        return RMM_ERR_MODULE_LOAD;
    }
    const uint8_t *blob = archive_base + e->offset;
    const struct rmm_image_header *h = (const struct rmm_image_header *)blob;

    int rc = validate_image(h, e->size, m, e->img_start);
    if (rc) {
        printf("  [RMM validate FAILED %s: %d]\n", m->name, rc);
        return RMM_ERR_MODULE_LOAD;
    }

    uint64_t base = m->base;
    uint64_t lo   = base + e->img_start;
    printf("[dbg] mapping %s at %p (%u bytes)\n", m->name,
           (void *)(uintptr_t)lo, (unsigned)e->size);

    /* code: RX - veneers are code and live below __rmm_code_start */
    uint64_t code_lo = lo;
    uint64_t code_hi = base + h->code_offset + h->code_size;
    /* rodata: R, data+bss: RW - one guard page between (script layout) */
    uint64_t ro_lo = base + h->rodata_offset + e->img_start;
    uint64_t ro_hi = base + h->rodata_offset + h->rodata_size + e->img_start;
    uint64_t da_lo = base + h->data_offset + e->img_start;
    uint64_t da_hi = base + h->data_offset + h->data_size + e->img_start;
    uint64_t bss_hi = da_hi + RMM_GUARD_BYTES + h->bss_size;

    /* : construct page tables, populate, then apply the
     * final W^X protections.  Segments are first mapped writable (code
     * pages are copied into), and only locked to RX/R after the bytes
     * are in place, never RX while being written. */
    if (!map_range(code_lo, code_hi, RMM_MAP_READ | RMM_MAP_WRITE) ||
        !map_range(ro_lo, ro_hi, RMM_MAP_READ | RMM_MAP_WRITE) ||
        !map_range(da_lo, bss_hi, RMM_MAP_READ | RMM_MAP_WRITE)) {
        loader_unload_image(m);
        return RMM_ERR_MODULE_LOAD;
    }

    /* copy segments out of the archive ().
     * blob[0..144) is the header, the flat image follows.  All source
     * indexing is against img = blob + sizeof(header).  The code copy
     * covers [0, code_offset + code_size) so the veneer prefix (slot+0
     * .. code_offset: .veneers, .drivers, inter-section padding) is
     * copied too - entry points call veneers before anything else and
     * the region must never contain PMM garbage. */
    const uint8_t *img = blob + sizeof(struct rmm_image_header);
    uint8_t *dst = (uint8_t *)(uintptr_t)(base + e->img_start);
    for (uint64_t i = 0; i < h->code_offset + h->code_size; i++)
        dst[i] = img[i];
    if (h->rodata_size) {
        dst = (uint8_t *)(uintptr_t)(base + h->rodata_offset + e->img_start);
        for (uint64_t i = 0; i < h->rodata_size; i++) dst[i] = img[h->rodata_offset + i];
    }
    if (h->data_size) {
        dst = (uint8_t *)(uintptr_t)(base + h->data_offset + e->img_start);
        for (uint64_t i = 0; i < h->data_size; i++) dst[i] = img[h->data_offset + i];
    }
    /* bss: pages were freshly allocated (zeroed by the PMM), step 6 */

    /* lock in the W^X policy now that the image is complete */
    for (uint64_t va = code_lo; va < code_hi; va += 4096)
        arch_protect(va, RMM_MAP_READ | RMM_MAP_EXEC);
    for (uint64_t va = ro_lo; va < ro_hi; va += 4096)
        arch_protect(va, RMM_MAP_READ);
    for (uint64_t va = da_lo; va < bss_hi; va += 4096)
        arch_protect(va, RMM_MAP_READ | RMM_MAP_WRITE);

    m->seg_code_start = code_lo;  m->seg_code_end = code_hi;
    m->seg_ro_start   = ro_lo;    m->seg_ro_end   = ro_hi;
    m->seg_data_start = da_lo;    m->seg_data_end = da_hi;
    m->seg_bss_start  = da_hi;    m->seg_bss_end  = bss_hi;
    m->code_size = h->code_size;
    m->data_size = h->data_size + h->bss_size;
    m->image_bytes = e->size;

    printf("[dbg] %s mapped+copied\n", m->name);
    /* mappings changed - sync every CPU's TLB */
    arch_tlb_sync_range(code_lo, bss_hi);
    /* instruction-side synchronization for the new code */
    arch_sync_code_range(code_lo, code_hi);
    return 0;
}

void loader_unload_image(rmm_module_t *m) {
    struct { uint64_t s, e; } ranges[] = {
        { m->seg_code_start, m->seg_code_end },
        { m->seg_ro_start,   m->seg_ro_end   },
        { m->seg_data_start, m->seg_bss_end  },
    };
    for (unsigned r = 0; r < 3; r++) {
        if (!(ranges[r].e > ranges[r].s)) continue;
        uint64_t s = ranges[r].s & ~(uint64_t)(RMM_GUARD_BYTES - 1);
        uint64_t e = (ranges[r].e + RMM_GUARD_BYTES - 1) &
                     ~(uint64_t)(RMM_GUARD_BYTES - 1);
        for (uint64_t va = s; va < e; va += RMM_GUARD_BYTES) {
            uint64_t phys = arch_translate(va);
            if (phys) arch_page_free(phys);
            arch_unmap(va);
        }
        arch_tlb_sync_range(s, e);
    }
    m->seg_code_start = m->seg_code_end = 0;
    m->seg_ro_start   = m->seg_ro_end   = 0;
    m->seg_data_start = m->seg_data_end = 0;
    m->seg_bss_start  = m->seg_bss_end  = 0;
    m->code_size = m->data_size = 0;
    m->image_bytes = 0;
}

uint64_t rmm_module_image_bytes(const char *name) {
    const struct rmm_arch_entry *e = archive_find(name);
    return e ? e->size : 0;
}
