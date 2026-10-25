#include "drivers/graphics/fbdev.h"
#include "drivers/memory/vmm.h"
#include <stdint.h>
#include <stddef.h>

static fbdev_t *fb_list = NULL;
static fbdev_t *fb_best = NULL;
static int      fb_cnt  = 0;

void fbdev_register(fbdev_t *f) {
    if (!f || !f->fb || !f->width || !f->height) return;
    f->next  = fb_list;
    fb_list  = f;
    fb_cnt++;
    if (!fb_best || f->priority > fb_best->priority) fb_best = f;
}

fbdev_t *fbdev_primary(void) { return fb_best; }
int      fbdev_count(void)   { return fb_cnt;  }

/* /dev/fb0 support */

int fbdev_mmap_shared(uint64_t base, size_t npages, uint64_t off) {
    fbdev_t *f = fb_best;
    if (!f || !f->fb) return -22;                /* -EINVAL: no framebuffer */
    if (off & 0xFFFULL) return -22;              /* page-aligned offset     */

    /* Framebuffer geometry in bytes, user mappings beyond it are refused. */
    uint64_t fb_bytes = (uint64_t)f->pitch * f->height;
    if (off + npages * PAGE_SIZE > ((fb_bytes + 4095ULL) & ~4095ULL))
        return -22;

    /* The fb pointer is a kernel virtual address (identity-mapped video
     * RAM or a driver-allocated RAM buffer).  Translate each page through
     * the current (kernel-half shared) page tables and map the same
     * physical pages into the user region, true shared semantics. */
    for (size_t i = 0; i < npages; i++) {
        uint64_t va   = (uint64_t)(uintptr_t)f->fb + off + i * 4096ULL;
        uint64_t phys = vmm_virt_to_phys(va);
        if (!phys) return -12;                   /* -ENOMEM: unmapped fb    */
        if (!vmm_map(base + i * 4096ULL, phys,
                     PTE_PRESENT | PTE_WRITABLE | PTE_USER))
            return -12;
    }
    return 0;
}

void fbdev_flush_all(void) {
    fbdev_t *f = fb_best;
    if (!f) return;
    if (f->flush_rows) f->flush_rows(f, 0, f->height ? f->height - 1 : 0);
}

bool fbdev_owns_phys(uint64_t phys) {
    for (fbdev_t *f = fb_list; f; f = f->next) {
        if (!f->fb || !f->height) continue;
        uint64_t start = (uint64_t)(uintptr_t)f->fb;
        uint64_t end   = start + (uint64_t)f->pitch * f->height;
        if (phys >= start && phys < end) return true;
    }
    return false;
}
