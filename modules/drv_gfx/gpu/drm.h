#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/*
 *  drm - KMS-lite: the DRM/KMS wire ABI over RMM GPU drivers
 *
 *  Exposes /dev/dri/card0 (+ renderD128) speaking the byte-exact libdrm
 *  ioctl ABI subset that a dumb-buffer KMS client needs:
 *
 *    Version, GET_CAP, SET_CLIENT_CAP,
 *    MODE_GETRESOURCES / GETCRTC / SETCRTC / GETENCODER / GETCONNECTOR,
 *    MODE_ADDFB / ADDFB2 / RMFB / GETFB / DIRTYFB,
 *    MODE_PAGE_FLIP (no vblank events, completes immediately),
 *    MODE_CREATE_DUMB / MAP_DUMB / DESTROY_DUMB.
 *
 *  Scanout backends are provided by the GPU drivers:
 *
 *    virtio-gpu   native per-resource scanout: each dumb buffer is a
 *                 virtio 2D resource with guest-RAM backing, flips do
 *                 SET_SCANOUT + TRANSFER_TO_HOST_2D + RESOURCE_FLUSH.
 *    BGA/std-vga  TRUE page flipping in video RAM: the console scanout
 *                 is slot 0, client dumb buffers live in VRAM slots
 *                 above it and flips just re-program dispi Y_OFFSET.
 *    vmware/vesa  blit on flip (client RAM buffer -> scanout); the boot
 *                 console picture is not preserved across flips, the
 *                 kernel clears the scanout on restore.
 *
 *  GEM handles are global, dumb-buffer storage is described by a
 *  physical base + page count, which is all the user mmap path needs.
 */

#define DRM_MAX_CARDS   2
#define DRM_MAX_GEMS    32
#define DRM_MAX_FBS     16

/* per-card scanout backend */
typedef struct drm_gem drm_gem_t;

typedef struct drm_backend {
    const char *name;               /* "virtio-gpu", "bga", "vmware-svga"... */
    int         priority;           /* higher wins card0                   */

    /* allocate a dumb buffer.  *phys_out = physical base of the backing
     * store (VRAM or contiguous guest RAM); *is_vram = backend owns the
     * memory (core must not pmm_free it on DESTROY_DUMB). */
    int       (*dumb_create)(struct drm_backend *b, uint32_t w, uint32_t h,
                             uint32_t bpp, uint32_t *pitch, uint64_t *size,
                             uint64_t *phys_out, bool *is_vram,
                             void **priv_out);
    void      (*dumb_destroy)(struct drm_backend *b, drm_gem_t *priv);

    /* show `gem` on the scanout (gem == NULL -> restore boot console) */
    int       (*flip)(struct drm_backend *b, drm_gem_t *gem,
                      uint32_t w, uint32_t h, uint32_t pitch);

    /* re-push the current scanout after userland painted (DIRTYFB) */
    int       (*dirty)(struct drm_backend *b, drm_gem_t *gem,
                       uint32_t w, uint32_t h, uint32_t pitch);
} drm_backend_t;

/* lifecycle */
void drm_init(void);                 /* probe backends, build card0         */
int  drm_card_count(void);

/* syscall-layer entry points */
long drm_ioctl(int card, uint64_t req, uint64_t arg);
/* map `npages` of the GEM object identified by mmap offset `off` */
int  drm_mmap_shared(int card, uint64_t off, uint64_t base, size_t npages);
/* true when phys belongs to a GEM object (munmap must not pmm_free)     */
bool drm_owns_phys(uint64_t phys);
