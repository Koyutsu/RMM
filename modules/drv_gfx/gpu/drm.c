#include "drivers/gpu/drm.h"
#include "drivers/graphics/fbdev.h"
#include "drivers/graphics/vesa.h"
#include "drivers/graphics/bga.h"
#include "drivers/graphics/vmware_svga.h"
#include "drivers/graphics/virtio_gpu.h"
#include "drivers/graphics/modeset.h"
#include "drivers/memory/vmm.h"
#include "console/print.h"

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/*
 *  drm.c - KMS-lite core
 *  The ioctl surface speaks the x86-64 DRM/KMS wire ABI (the struct
 *  layouts and ioctl numbers direct-rendering programs use).  The
 *  cooperative kernel copies ioctl payloads directly through the user
 *  pointers - the same convention as the fb0/evdev ioctl
 *  implementations in exec/syscall_table.c.
 */

/* UAPI structures (x86-64 wire layouts) */

typedef struct {
    uint32_t fb_id;
    uint32_t width, height, pitch, bpp, depth, handle;
} drm_mode_fb_cmd_t;                     /* ADDFB / GETFB (7 wire fields) */

typedef struct {
    uint32_t fb_id, width, height, pixel_format, flags;
    uint32_t handles[4], pitches[4], offsets[4];
    uint64_t modifier[4];
} drm_mode_fb_cmd2_t;                    /* ADDFB2 */

typedef struct {
    uint32_t height, width, bpp, flags;
    uint32_t handle, pitch;
    uint64_t size;
} drm_mode_create_dumb_t;                /* 24 bytes */

typedef struct {
    uint32_t handle, pad;
    uint64_t offset;
} drm_mode_map_dumb_t;                   /* 16 bytes */

typedef struct {
    uint32_t handle;
} drm_mode_destroy_dumb_t;

typedef struct {
    uint32_t clock;
    uint16_t hdisplay, hsync_start, hsync_end, htotal, hskew;
    uint16_t vdisplay, vsync_start, vsync_end, vtotal, vscan;
    uint32_t vrefresh;
    uint32_t flags;
    uint32_t type;
    char     name[32];
} drm_mode_modeinfo_t;                   /* 68 bytes */

typedef struct {
    uint64_t fb_id_ptr, crtc_id_ptr, connector_id_ptr, encoder_id_ptr;
    uint32_t count_fbs, count_crtcs, count_connectors, count_encoders;
    uint32_t min_width, max_width, min_height, max_height;
} drm_mode_card_res_t;                   /* 64 bytes */

typedef struct {
    uint64_t set_connectors_ptr;
    uint32_t count_connectors, crtc_id, fb_id, x, y, gamma_size, mode_valid;
    drm_mode_modeinfo_t mode;
} drm_mode_crtc_t;                       /* 108 bytes */

typedef struct {
    uint32_t encoder_type, encoder_id, crtc_id, possible_crtcs, possible_clones;
} drm_mode_get_encoder_t;                /* 20 bytes */

typedef struct {
    uint64_t encoders_ptr, props_ptr, prop_values_ptr, modes_ptr;
    uint32_t count_modes, count_props, count_encoders;
    uint32_t encoder_id, connector_id;
    uint32_t connector_type, connector_type_id;
    uint32_t connection, mm_width, mm_height, subpixel, pad1;
} drm_mode_get_connector_t;              /* 80 bytes */

typedef struct {
    uint32_t crtc_id, fb_id, flags, reserved;
    uint64_t user_data;
} drm_mode_crtc_page_flip_t;

typedef struct {
    uint32_t fb_id, flags, color, num_clips;
    uint64_t clips_id;
} drm_mode_fb_dirty_cmd_t;

typedef struct {
    uint64_t capability, value;
} drm_get_cap_t;

typedef struct {
    int32_t  version_major, version_minor, version_patchlevel;
    uint64_t name_len;  char *name;
    uint64_t date_len;  char *date;
    uint64_t desc_len;  char *desc;
} drm_version_t;                         /* 64 bytes */

/* UAPI constants */
#define DRM_MODE_TYPE_DRIVER     16u
#define DRM_MODE_TYPE_PREFERRED   8u
#define DRM_MODE_CONNECTED        1u
#define DRM_MODE_CONNECTOR_VIRTUAL 11u
#define DRM_MODE_ENCODER_VIRTUAL   5u
#define DRM_MODE_SUBPIXEL_UNKNOWN  1u
#define DRM_MODE_PAGE_FLIP_EVENT   1u
#define DRM_CLIENT_CAP_STEREO_TILE 1u
#define DRM_CLIENT_CAP_UNIVERSAL_PLANES 2u
#define DRM_CLIENT_CAP_ASYNC_PAGE_FLIP 3u
#define DRM_CAP_DUMB_BUFFER        1u
#define DRM_CAP_VBLANK_HIGH_CRTC   2u
#define DRM_CAP_DUMB_PREFERRED_DEPTH 3u
#define DRM_FOURCC_XRGB8888 0x34325258u   /* 'XR24' */

#define _DRM_NR(req)   ((req) & 0xFF)

/* object model */
struct drm_gem {
    bool     used;
    uint32_t handle;
    uint32_t width, height, pitch, bpp;
    uint64_t size;
    bool     vram;             /* lives in device memory (never pmm_free) */
    uint64_t phys;             /* physical base (vram or contiguous RAM)  */
    void    *priv;             /* backend cookie                          */
    int      card;
};

typedef struct {
    bool     used;
    uint32_t id;
    int      card;
    uint32_t gem_handle;
    uint32_t width, height, pitch, bpp;
} drm_fb_obj_t;

typedef struct {
    bool               active;
    drm_backend_t     *be;
    uint32_t           w, h, bpp, pitch;     /* current mode               */
    uint32_t           crtc_id, encoder_id, connector_id;
    drm_gem_t          gems[DRM_MAX_GEMS];
    drm_fb_obj_t       fbs[DRM_MAX_FBS];
    uint32_t           next_handle;
    uint32_t           cur_fb;               /* scanout fb id (0=console)  */
    drm_gem_t         *cur_gem;
} drm_card_t;

static drm_card_t cards[DRM_MAX_CARDS];
static int        ncards = 0;
static drm_fb_obj_t fb_global[DRM_MAX_FBS];   /* id -> fb, cross-card lookup */
static uint32_t   next_fb_id = 1;

/* small helpers */
static void u32arr_put(uint64_t uptr, uint32_t n, const uint32_t *vals) {
    uint32_t *u = (uint32_t *)(uintptr_t)uptr;
    if (!u) return;
    for (uint32_t i = 0; i < n; i++) u[i] = vals[i];
}

static void mode_fill(drm_mode_modeinfo_t *m, uint32_t w, uint32_t h) {
    uint32_t clock = 0;
    if      (w == 640  && h == 480)  clock = 25175;
    else if (w == 800  && h == 600)  clock = 40000;
    else if (w == 1024 && h == 768)  clock = 65000;
    else if (w == 1280 && h == 720)  clock = 74250;
    else if (w == 1280 && h == 1024) clock = 108000;
    else if (w == 1920 && h == 1080) clock = 148500;
    else clock = (w / 8 + 24) * h / 1000;    /* rough estimate */

    m->clock = clock;
    m->hdisplay    = (uint16_t)w;  m->hsync_start = (uint16_t)(w + 24);
    m->hsync_end   = (uint16_t)(w + 24 + 136);
    m->htotal      = (uint16_t)(w + 24 + 136 + 160);
    m->hskew       = 0;
    m->vdisplay    = (uint16_t)h;  m->vsync_start = (uint16_t)(h + 3);
    m->vsync_end   = (uint16_t)(h + 3 + 6);
    m->vtotal      = (uint16_t)(h + 3 + 6 + 29);
    m->vscan       = 0;
    m->vrefresh    = 60;
    m->flags       = 0;
    m->type        = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
    for (int i = 0; i < 31 && i < 32; i++) m->name[i] = 0;
    /* "1024x768" */
    char *n = m->name;
    char tmp[12]; int t = 0;
    uint32_t v = w; if (!v) tmp[t++] = '0';
    while (v) { tmp[t++] = (char)('0' + v % 10); v /= 10; }
    while (t) *n++ = tmp[--t];
    *n++ = 'x';
    t = 0; v = h; if (!v) tmp[t++] = '0';
    while (v) { tmp[t++] = (char)('0' + v % 10); v /= 10; }
    while (t && n < m->name + 31) *n++ = tmp[--t];
    *n = 0;
}

static drm_card_t *card_by_crtc(uint32_t id) {
    for (int i = 0; i < ncards; i++)
        if (cards[i].active && cards[i].crtc_id == id) return &cards[i];
    return NULL;
}
static drm_card_t *card_by_connector(uint32_t id) {
    for (int i = 0; i < ncards; i++)
        if (cards[i].active && cards[i].connector_id == id) return &cards[i];
    return NULL;
}
static drm_card_t *card_by_encoder(uint32_t id) {
    for (int i = 0; i < ncards; i++)
        if (cards[i].active && cards[i].encoder_id == id) return &cards[i];
    return NULL;
}
static drm_gem_t *gem_lookup(uint32_t handle) {
    for (int c = 0; c < ncards; c++)
        for (int i = 0; i < DRM_MAX_GEMS; i++)
            if (cards[c].gems[i].used && cards[c].gems[i].handle == handle)
                return &cards[c].gems[i];
    return NULL;
}
static drm_fb_obj_t *fb_lookup(uint32_t id) {
    for (int i = 0; i < DRM_MAX_FBS; i++)
        if (fb_global[i].used && fb_global[i].id == id) return &fb_global[i];
    return NULL;
}

/*
 *  Backend: virtio-gpu (native per-resource scanout)
 */
typedef struct { uint32_t rid; } vbe_priv_t;

static int virtio_be_dumb_create(drm_backend_t *b, uint32_t w, uint32_t h,
                                 uint32_t bpp, uint32_t *pitch, uint64_t *size,
                                 uint64_t *phys_out, bool *is_vram,
                                 void **priv_out) {
    (void)b; (void)bpp;
    uint64_t pa; uint32_t rid;
    if (!virtio_gpu_dumb_create(w, h, &pa, &rid)) return -12;   /* -ENOMEM */
    static vbe_priv_t privs[DRM_MAX_GEMS];
    static int npriv = 0;
    vbe_priv_t *p = &privs[npriv++ % DRM_MAX_GEMS];
    p->rid = rid;
    *pitch = w * 4;
    *size  = (uint64_t)w * h * 4;
    *phys_out = pa;
    *is_vram  = true;      /* backend-owned: DESTROY frees via the device */
    *priv_out = (drm_gem_t *)p;
    return 0;
}

static void virtio_be_dumb_destroy(drm_backend_t *b, drm_gem_t *priv) {
    (void)b;
    vbe_priv_t *p = (vbe_priv_t *)priv;
    if (p) virtio_gpu_dumb_destroy(p->rid);
}

static int virtio_be_flip(drm_backend_t *b, drm_gem_t *gem,
                          uint32_t w, uint32_t h, uint32_t pitch) {
    (void)b; (void)pitch;
    if (!gem) return virtio_gpu_console_scanout() ? 0 : -5;     /* -EIO */
    vbe_priv_t *p = (vbe_priv_t *)gem->priv;
    if (!virtio_gpu_dumb_scanout(p->rid, w, h)) return -5;
    if (!virtio_gpu_dumb_flush(p->rid, w, h))   return -5;
    return 0;
}

static int virtio_be_dirty(drm_backend_t *b, drm_gem_t *gem,
                           uint32_t w, uint32_t h, uint32_t pitch) {
    return virtio_be_flip(b, gem, w, h, pitch);
}

/*
 *  Backend: BGA / std-vga (true VRAM page-flip via dispi Y_OFFSET panning)
 *
 *  VRAM layout: slot 0 = boot console scanout (pitch*h bytes); slots
 *  1..n are DRM-owned.  A dumb buffer that exactly matches the mode
 *  lives directly in a free slot (zero-copy double buffering); any
 *  other dumb buffer comes from guest RAM and is blitted into a free
 *  scratch slot on flip.  The console keeps drawing into slot 0 and is
 *  fully restored by pan(0).
 */
#define BGA_MAX_SLOTS 4
typedef struct {
    uint32_t vram_phys, vram_size, pitch, mode_h;
    uint32_t slot_bytes;
    int      nslots;                    /* total incl. console slot 0       */
    bool     slot_used[BGA_MAX_SLOTS];
    uint64_t ram_scratch_phys;          /* guest-RAM scratch for RAM flips  */
    bool     scratch_in_use;
} bga_state_t;
static bga_state_t bga_be;

static int bga_alloc_slot(bga_state_t *s) {
    for (int i = 1; i < s->nslots; i++)
        if (!s->slot_used[i]) { s->slot_used[i] = true; return i; }
    return -1;
}
static void bga_free_slot(bga_state_t *s, int slot) {
    if (slot > 0 && slot < s->nslots) s->slot_used[slot] = false;
}

static int bga_be_dumb_create(drm_backend_t *b, uint32_t w, uint32_t h,
                              uint32_t bpp, uint32_t *pitch, uint64_t *size,
                              uint64_t *phys_out, bool *is_vram,
                              void **priv_out) {
    (void)b;
    bga_state_t *s = &bga_be;
    uint32_t p4 = ((w * (bpp / 8)) + 3) & ~3u;
    uint64_t bytes = (uint64_t)p4 * h;
    *pitch = p4;
    *size  = bytes;

    /* exact-mode buffers get a real VRAM slot (zero-copy page flips) */
    if (w == cards[0].w && h == cards[0].h && bpp == cards[0].bpp) {
        int slot = bga_alloc_slot(s);
        if (slot > 0) {
            *phys_out = s->vram_phys + (uint64_t)slot * s->slot_bytes;
            *is_vram  = true;
            *priv_out = (drm_gem_t *)(uintptr_t)(0x1000u | ((uint32_t)slot << 12));
            return 0;
        }
    }

    /* otherwise: contiguous guest RAM, blitted into a scratch slot */
    uint32_t pages = (uint32_t)((bytes + PAGE_SIZE - 1) / PAGE_SIZE);
    uint64_t pa = pmm_alloc_contig(pages);
    if (!pa) return -12;
    if (!vmm_identity_map_range(pa, pa + (uint64_t)pages * PAGE_SIZE,
                                PTE_PRESENT | PTE_WRITABLE)) {
        for (uint32_t i = 0; i < pages; i++) pmm_free(pa + (uint64_t)i * PAGE_SIZE);
        return -12;
    }
    *phys_out = pa;
    *is_vram  = false;
    *priv_out = NULL;              /* RAM cookie: flip() uses gem->phys */
    return 0;
}

static void bga_be_dumb_destroy(drm_backend_t *b, drm_gem_t *priv) {
    (void)b;
    bga_state_t *s = &bga_be;
    uintptr_t tag = (uintptr_t)priv;
    if (tag & 0x1000u) {                       /* VRAM slot buffer */
        bga_free_slot(s, (int)((tag >> 12) & 0xFFF));
    }
    /* RAM buffers are freed by the core (gem->phys, !vram) */
}

static int bga_be_flip(drm_backend_t *b, drm_gem_t *gem,
                       uint32_t w, uint32_t h, uint32_t pitch) {
    (void)b;
    bga_state_t *s = &bga_be;
    if (!gem) { bga_kms_pan(0); return 0; }    /* restore console */

    uintptr_t tag = (uintptr_t)gem->priv;
    if (tag & 0x1000u) {                       /* VRAM slot: pure pan */
        bga_kms_pan(((tag >> 12) & 0xFFF) * s->mode_h);
        return 0;
    }

    /* RAM buffer: blit into a free scratch slot, then pan to it */
    int slot = bga_alloc_slot(s);
    if (slot < 0) return -28;                  /* -ENOSPC: no VRAM scratch */
    uint64_t dst = s->vram_phys + (uint64_t)slot * s->slot_bytes;
    const uint8_t *src = (const uint8_t *)(uintptr_t)gem->phys;
    uint32_t rows = (h < cards[0].h) ? h : cards[0].h;
    uint32_t cp   = (pitch < s->pitch) ? pitch : s->pitch;
    for (uint32_t y = 0; y < rows; y++)
        for (uint32_t x = 0; x < cp; x++)
            ((volatile uint8_t *)dst)[y * s->pitch + x] = src[y * pitch + x];
    bga_kms_pan(slot * s->mode_h);
    return 0;
}

static int bga_be_dirty(drm_backend_t *b, drm_gem_t *gem,
                        uint32_t w, uint32_t h, uint32_t pitch) {
    (void)b; (void)gem; (void)w; (void)h; (void)pitch;
    return 0;                                   /* video RAM: always live */
}

/*
 *  Backend: generic blit (vmware-svga / vesa fbcon)
 */
static int blit_be_dumb_create(drm_backend_t *b, uint32_t w, uint32_t h,
                               uint32_t bpp, uint32_t *pitch, uint64_t *size,
                               uint64_t *phys_out, bool *is_vram,
                               void **priv_out) {
    (void)b; (void)priv_out;
    uint32_t p4 = ((w * (bpp / 8)) + 3) & ~3u;
    uint64_t bytes = (uint64_t)p4 * h;
    uint32_t pages = (uint32_t)((bytes + PAGE_SIZE - 1) / PAGE_SIZE);
    uint64_t pa = pmm_alloc_contig(pages);
    if (!pa) return -12;
    if (!vmm_identity_map_range(pa, pa + (uint64_t)pages * PAGE_SIZE,
                                PTE_PRESENT | PTE_WRITABLE)) {
        for (uint32_t i = 0; i < pages; i++) pmm_free(pa + (uint64_t)i * PAGE_SIZE);
        return -12;
    }
    *pitch = p4;
    *size  = bytes;
    *phys_out = pa;
    *is_vram  = false;
    return 0;
}

static void blit_be_dumb_destroy(drm_backend_t *b, drm_gem_t *priv) { (void)b; (void)priv; }

static int blit_be_flip(drm_backend_t *b, drm_gem_t *gem,
                        uint32_t w, uint32_t h, uint32_t pitch) {
    (void)b;
    fbdev_t *f = fbdev_primary();
    if (!f || !f->fb) return -19;               /* -ENODEV */
    if (!gem) return 0;                         /* console: nothing to do  */

    uint8_t *dst = f->fb;
    const uint8_t *src = (const uint8_t *)(uintptr_t)gem->phys;
    uint32_t rows = (h < f->height) ? h : f->height;
    uint32_t cols = (w < f->width) ? w : f->width;
    uint32_t sbpp = f->bpp / 8;
    for (uint32_t y = 0; y < rows; y++) {
        uint8_t *d = dst + (uint64_t)y * f->pitch;
        const uint8_t *s = src + (uint64_t)y * pitch;
        for (uint32_t x = 0; x < cols * sbpp; x++)
            d[x] = s[x];
    }
    if (f->flush_rows) f->flush_rows(f, 0, rows - 1);
    return 0;
}

static int blit_be_dirty(drm_backend_t *b, drm_gem_t *gem,
                         uint32_t w, uint32_t h, uint32_t pitch) {
    return blit_be_flip(b, gem, w, h, pitch);
}

/*
 *  Backend registration
 */
static drm_backend_t be_virtio = {
    .name = "virtio-gpu", .priority = 40,
    .dumb_create = virtio_be_dumb_create, .dumb_destroy = virtio_be_dumb_destroy,
    .flip = virtio_be_flip, .dirty = virtio_be_dirty,
};
static drm_backend_t be_bga = {
    .name = "bga", .priority = 30,
    .dumb_create = bga_be_dumb_create, .dumb_destroy = bga_be_dumb_destroy,
    .flip = bga_be_flip, .dirty = bga_be_dirty,
};
static drm_backend_t be_blit_vmware = {
    .name = "vmware-svga", .priority = 20,
    .dumb_create = blit_be_dumb_create, .dumb_destroy = blit_be_dumb_destroy,
    .flip = blit_be_flip, .dirty = blit_be_dirty,
};
static drm_backend_t be_blit_fbcon = {
    .name = "fbcon", .priority = 10,
    .dumb_create = blit_be_dumb_create, .dumb_destroy = blit_be_dumb_destroy,
    .flip = blit_be_flip, .dirty = blit_be_dirty,
};

static bool card_setup(drm_backend_t *be) {
    fbdev_t *f = fbdev_primary();
    if (!f) return false;
    if (ncards >= DRM_MAX_CARDS) return false;
    drm_card_t *c = &cards[ncards];
    c->active      = true;
    c->be          = be;
    c->w           = f->width;
    c->h           = f->height;
    c->bpp         = f->bpp ? f->bpp : 32;
    c->pitch       = f->pitch;
    c->crtc_id     = 0x10u + (uint32_t)ncards;
    c->encoder_id  = 0x20u + (uint32_t)ncards;
    c->connector_id= 0x30u + (uint32_t)ncards;
    c->next_handle = 1;
    c->cur_fb      = 0;
    ncards++;
    return true;
}

void drm_init(void) {
    fbdev_t *f = fbdev_primary();
    if (!f) return;

    /* pick the best available scanout backend */
    drm_backend_t *be = NULL;

    if (virtio_gpu_dumb_ready())
        be = &be_virtio;

    if (!be && (bga_available() || bga_quiesce_adopt())) {
        bga_map_vram_full();                 /* slots live beyond the fb */
        bga_state_t *s = &bga_be;
        s->vram_phys  = bga_vram_phys();
        s->vram_size  = bga_vram_size();
        s->pitch      = bga_pitch();
        s->mode_h     = bga_height();
        s->slot_bytes = s->pitch * s->mode_h;
        s->nslots     = (int)(s->vram_size / s->slot_bytes);
        if (s->nslots > BGA_MAX_SLOTS) s->nslots = BGA_MAX_SLOTS;
        for (int i = 0; i < BGA_MAX_SLOTS; i++)
            s->slot_used[i] = (i == 0);        /* slot 0 = console */
        if (s->nslots >= 2 && s->slot_bytes) {
            /* tell the device it may address the extra slots */
            bga_kms_set_virtual((uint32_t)s->nslots * s->mode_h);
            be = &be_bga;
        }
    }

    if (!be && vmware_fb_addr())
        be = &be_blit_vmware;
    if (!be)
        be = &be_blit_fbcon;

    if (!card_setup(be)) return;

    print_str("[drm] card0: ");
    print_str(be->name);
    print_str(" scanout, dumb buffers, mode ");
    printf("%ux%u\n", cards[0].w, cards[0].h);

    /* QA log: the VGA-text print above never reaches the serial console */
    {
        extern void serial_print_byte(const char);
        const char *m = "[drm] card0 backend: ";
        for (int i = 0; m[i]; i++) serial_print_byte(m[i]);
        for (int i = 0; be->name[i]; i++) serial_print_byte(be->name[i]);
        m = "\n";
        for (int i = 0; m[i]; i++) serial_print_byte(m[i]);
    }
}

int drm_card_count(void) { return ncards; }

/*
 *  replan the BGA VRAM slot map after a runtime mode change
 *
 *  The slot geometry (pitch x mode_h) shifts with the new mode.  The
 *  console scanout returns to slot 0; VRAM-slot dumb buffers die with
 *  their slots (the layout they were carved from is gone), RAM dumb
 *  buffers survive - their flips re-blit into a fresh scratch slot at
 *  the new pitch.
 */
void drm_bga_replan(void) {
    if (ncards == 0 || cards[0].be != &be_bga) return;
    bga_map_vram_full();
    bga_state_t *s = &bga_be;
    s->pitch      = bga_pitch();
    s->mode_h     = bga_height();
    s->slot_bytes = s->pitch * s->mode_h;
    s->nslots     = (int)(s->vram_size / s->slot_bytes);
    if (s->nslots > BGA_MAX_SLOTS) s->nslots = BGA_MAX_SLOTS;
    for (int i = 0; i < BGA_MAX_SLOTS; i++)
        s->slot_used[i] = (i == 0);
    if (s->nslots >= 2 && s->slot_bytes)
        bga_kms_set_virtual((uint32_t)s->nslots * s->mode_h);

    drm_card_t *c = &cards[0];
    c->w = bga_width();
    c->h = bga_height();
    c->pitch = bga_pitch();
    c->cur_fb = 0;
    c->cur_gem = NULL;

    /* release slot-backed gems and their fb objects (slots are gone) */
    for (int i = 0; i < DRM_MAX_GEMS; i++) {
        drm_gem_t *g = &c->gems[i];
        if (!g->used || !g->vram ||
            !(g->priv && ((uintptr_t)g->priv & 0x1000u)))
            continue;
        g->used = false;
        for (int k = 0; k < DRM_MAX_FBS; k++) {
            if (fb_global[k].used && fb_global[k].card == 0 &&
                fb_global[k].gem_handle == g->handle)
                fb_global[k].used = false;
        }
    }
}

/*
 *  ioctl dispatch
 */
static long drm_gem_new(drm_card_t *c, uint32_t w, uint32_t h, uint32_t bpp,
                        drm_gem_t **out) {
    for (int i = 0; i < DRM_MAX_GEMS; i++) {
        drm_gem_t *g = &c->gems[i];
        if (g->used) continue;
        g->used   = true;
        g->handle = c->next_handle++;
        g->width  = w; g->height = h; g->bpp = bpp;
        g->card   = (int)(c - cards);
        g->vram   = false; g->phys = 0; g->priv = NULL;
        *out = g;
        return 0;
    }
    return -12;                                  /* -ENOMEM */
}

static long drm_create_dumb(drm_card_t *c, drm_mode_create_dumb_t *d) {
    if (!d->width || !d->height) return -22;     /* -EINVAL */
    uint32_t bpp = d->bpp ? d->bpp : 32;
    if (bpp != 32 && bpp != 24 && bpp != 16) return -22;

    drm_gem_t *g = NULL;
    long rc = drm_gem_new(c, d->width, d->height, bpp, &g);
    if (rc) return rc;

    uint32_t pitch = 0; uint64_t size = 0, phys = 0;
    bool is_vram = false;
    rc = c->be->dumb_create(c->be, d->width, d->height, bpp,
                            &pitch, &size, &phys, &is_vram, (void *)&g->priv);
    if (rc) { g->used = false; return rc; }

    g->pitch = pitch; g->size = size; g->phys = phys; g->vram = is_vram;
    d->handle = g->handle;
    d->pitch  = pitch;
    d->size   = size;
    return 0;
}

static long drm_addfb(drm_card_t *c, drm_mode_fb_cmd_t *f) {
    drm_gem_t *g = gem_lookup(f->handle);
    if (!g) return -2;                           /* -ENOENT */
    drm_fb_obj_t *fb = NULL;
    for (int i = 0; i < DRM_MAX_FBS; i++)
        if (!fb_global[i].used) { fb = &fb_global[i]; break; }
    if (!fb) return -12;
    fb->used   = true;
    fb->id     = next_fb_id++;
    fb->card   = (int)(c - cards);
    fb->gem_handle = f->handle;
    fb->width  = f->width  ? f->width  : g->width;
    fb->height = f->height ? f->height : g->height;
    fb->pitch  = f->pitch  ? f->pitch  : g->pitch;
    fb->bpp    = f->bpp    ? f->bpp    : g->bpp;
    f->fb_id   = fb->id;
    return 0;
}

static long drm_addfb2(drm_card_t *c, drm_mode_fb_cmd2_t *f) {
    drm_mode_fb_cmd_t o;
    o.handle = f->handles[0];
    o.width  = f->width;
    o.height = f->height;
    o.pitch  = f->pitches[0];
    o.bpp    = (f->pixel_format == DRM_FOURCC_XRGB8888) ? 32 : 24;
    o.depth  = (f->pixel_format == DRM_FOURCC_XRGB8888) ? 24 : 16;
    long rc = drm_addfb(c, &o);
    if (!rc) f->fb_id = o.fb_id;
    return rc;
}

static long drm_setcrtc(drm_card_t *c, drm_mode_crtc_t *cr, bool flip) {
    (void)flip;
    if (cr->crtc_id != c->crtc_id) {
        drm_card_t *cc = card_by_crtc(cr->crtc_id);
        if (!cc) return -22;
        c = cc;
    }
    if (cr->fb_id == 0) {                        /* restore boot console */
        c->cur_fb = 0; c->cur_gem = NULL;
        return c->be->flip(c->be, NULL, c->w, c->h, c->pitch);
    }
    drm_fb_obj_t *fb = fb_lookup(cr->fb_id);
    if (!fb || fb->card != (int)(c - cards)) return -22;
    drm_gem_t *g = gem_lookup(fb->gem_handle);
    if (!g) return -22;
    /* an fb whose size differs from the live mode requests a
     * mode change first (BGA backend only, the other scanouts render
     * into a fixed canvas and report -EINVAL instead). */
    if ((fb->width != c->w || fb->height != c->h)) {
        if (c->be != &be_bga) return -22;        /* -EINVAL */
        int rc = modeset_apply(fb->width, fb->height, 32);
        if (rc) return rc;
        /* replan ran inside modeset_apply, slot-backed gems died, so
         * re-resolve - a surviving fb must be RAM-backed now. */
        g = gem_lookup(fb->gem_handle);
        if (!g) return -2;                       /* -ENOENT */
    }
    long rc = c->be->flip(c->be, g, fb->width, fb->height, fb->pitch);
    if (!rc) { c->cur_fb = fb->id; c->cur_gem = g; }
    return rc;
}

long drm_ioctl(int card, uint64_t req, uint64_t arg) {
    if (card < 0 || card >= ncards || !cards[card].active) return -19; /* -ENODEV */
    drm_card_t *c = &cards[card];
    uint32_t nr = _DRM_NR(req);
    void *a = (void *)(uintptr_t)arg;

    switch (nr) {
    case 0x00: {                                 /* Version */
        static const char d_name[] = "rmm-drm";
        static const char d_date[] = "20260917";
        static const char d_desc[] = "RMM KMS-lite (dumb buffers, bochs/virtio/vmware/vesa)";
        drm_version_t *v = a;
        if (!v) return -14;                      /* -EFAULT */
        v->version_major = 1; v->version_minor = 0; v->version_patchlevel = 0;
        if (v->name_len > sizeof d_name - 1 && v->name)
            { for (int i = 0; d_name[i]; i++) v->name[i] = d_name[i]; v->name[sizeof d_name - 1] = 0; }
        v->name_len = sizeof d_name - 1;
        if (v->date_len > sizeof d_date - 1 && v->date)
            { for (int i = 0; d_date[i]; i++) v->date[i] = d_date[i]; v->date[sizeof d_date - 1] = 0; }
        v->date_len = sizeof d_date - 1;
        if (v->desc_len > sizeof d_desc - 1 && v->desc)
            { for (int i = 0; d_desc[i]; i++) v->desc[i] = d_desc[i]; v->desc[sizeof d_desc - 1] = 0; }
        v->desc_len = sizeof d_desc - 1;
        return 0;
    }
    case 0x0C: {                                 /* GET_CAP */
        drm_get_cap_t *cp = a;
        if (!cp) return -14;
        switch (cp->capability) {
        case DRM_CAP_DUMB_BUFFER:          cp->value = 1; break;
        case DRM_CAP_VBLANK_HIGH_CRTC:     cp->value = 1; break;
        case DRM_CAP_DUMB_PREFERRED_DEPTH: cp->value = 24; break;
        default: cp->value = 0; break;
        }
        return 0;
    }
    case 0x0D:                                   /* SET_CLIENT_CAP */
        return 0;                                /* accept, ignore */

    case 0xA0: {                                 /* MODE_GETRESOURCES */
        drm_mode_card_res_t *r = a;
        if (!r) return -14;
        uint32_t fbs[DRM_MAX_FBS], crtcs[1], conns[1], encs[1];
        uint32_t nf = 0;
        for (int i = 0; i < DRM_MAX_FBS; i++)
            if (fb_global[i].used && fb_global[i].card == card)
                fbs[nf++] = fb_global[i].id;
        crtcs[0] = c->crtc_id; conns[0] = c->connector_id; encs[0] = c->encoder_id;
        u32arr_put(r->fb_id_ptr,       r->count_fbs       < nf ? r->count_fbs       : nf, fbs);
        u32arr_put(r->crtc_id_ptr,     r->count_crtcs     < 1  ? r->count_crtcs     : 1,  crtcs);
        u32arr_put(r->connector_id_ptr,r->count_connectors< 1  ? r->count_connectors: 1,  conns);
        u32arr_put(r->encoder_id_ptr,  r->count_encoders  < 1  ? r->count_encoders  : 1,  encs);
        r->count_fbs = nf; r->count_crtcs = 1;
        r->count_connectors = 1; r->count_encoders = 1;
        r->min_width = c->w; r->max_width = c->w;
        r->min_height = c->h; r->max_height = c->h;
        return 0;
    }
    case 0xA1: {                                 /* MODE_GETCRTC */
        drm_mode_crtc_t *cr = a;
        if (!cr) return -14;
        cr->count_connectors = 1;
        cr->crtc_id = c->crtc_id;
        cr->fb_id   = c->cur_fb;
        cr->x = 0; cr->y = 0;
        cr->gamma_size = 0;
        cr->mode_valid = 1;
        mode_fill(&cr->mode, c->w, c->h);
        return 0;
    }
    case 0xA2:                                   /* MODE_SETCRTC */
        return drm_setcrtc(c, (drm_mode_crtc_t *)a, false);
    case 0xA6: {                                 /* MODE_GETENCODER */
        drm_mode_get_encoder_t *e = a;
        if (!e) return -14;
        e->encoder_type    = DRM_MODE_ENCODER_VIRTUAL;
        e->encoder_id      = c->encoder_id;
        e->crtc_id         = c->crtc_id;
        e->possible_crtcs  = 1;
        e->possible_clones = 0;
        return 0;
    }
    case 0xA7: {                                 /* MODE_GETCONNECTOR */
        drm_mode_get_connector_t *co = a;
        if (!co) return -14;
        /* expose the standard mode table (live mode first) */
        uint32_t total = (uint32_t)modeset_mode_count();
        if (total == 0) total = 1;
        if (co->modes_ptr && co->count_modes) {
            drm_mode_modeinfo_t m = {0};
            mode_fill(&m, c->w, c->h);
            *(drm_mode_modeinfo_t *)(uintptr_t)co->modes_ptr = m;
            uint32_t emitted = 1;
            int ntab = modeset_mode_count();
            for (int i = 0; i < ntab && emitted < co->count_modes; i++) {
                modeset_mode_t mm = modeset_mode(i);
                if (mm.w == c->w && mm.h == c->h) continue;
                drm_mode_modeinfo_t mi = {0};
                mode_fill(&mi, mm.w, mm.h);
                ((drm_mode_modeinfo_t *)(uintptr_t)co->modes_ptr)[emitted++] = mi;
            }
        }
        uint32_t enc = c->encoder_id;
        u32arr_put(co->encoders_ptr, co->count_encoders < 1 ? co->count_encoders : 1, &enc);
        co->count_modes    = total;
        co->count_props    = 0;
        co->count_encoders = 1;
        co->encoder_id     = c->encoder_id;
        co->connector_id   = c->connector_id;
        co->connector_type = DRM_MODE_CONNECTOR_VIRTUAL;
        co->connector_type_id = 1;
        co->connection     = DRM_MODE_CONNECTED;
        co->mm_width       = 320;                /* ~96 dpi */
        co->mm_height      = 240;
        co->subpixel       = DRM_MODE_SUBPIXEL_UNKNOWN;
        return 0;
    }

    case 0xAD: {                                 /* MODE_GETFB */
        drm_mode_fb_cmd_t *f = a;
        if (!f) return -14;
        drm_fb_obj_t *fb = fb_lookup(f->fb_id ? f->fb_id : 0);
        /* GETFB carries the id in fb_id? libdrm uses handle field? - id
         * arrives in fb_id on newer headers, check both */
        if (!fb) fb = fb_lookup(f->handle);
        if (!fb) return -2;                      /* -ENOENT */
        drm_gem_t *g = gem_lookup(fb->gem_handle);
        f->fb_id  = fb->id;
        f->width  = fb->width; f->height = fb->height;
        f->pitch  = fb->pitch; f->bpp    = fb->bpp;
        f->depth  = fb->bpp == 32 ? 24 : fb->bpp;
        f->handle = g ? g->handle : 0;
        return 0;
    }
    case 0xAE:                                   /* MODE_ADDFB */
        return drm_addfb(c, (drm_mode_fb_cmd_t *)a);
    case 0xB8:                                   /* MODE_ADDFB2 */
        return drm_addfb2(c, (drm_mode_fb_cmd2_t *)a);
    case 0xAF: {                                 /* MODE_RMFB (arg = u32*) */
        uint32_t *id = a;
        if (!id) return -14;
        drm_fb_obj_t *fb = fb_lookup(*id);
        if (!fb || fb->card != card) return -22;
        if (c->cur_fb == *id) { c->cur_fb = 0; c->cur_gem = NULL; }
        fb->used = false;
        return 0;
    }
    case 0xB0: {                                 /* MODE_PAGE_FLIP */
        drm_mode_crtc_page_flip_t *pf = a;
        if (!pf) return -14;
        if (pf->flags & DRM_MODE_PAGE_FLIP_EVENT)
            return -22;                          /* no vblank event source yet */
        drm_mode_crtc_t cr;
        cr.crtc_id = pf->crtc_id;
        cr.fb_id   = pf->fb_id;
        return drm_setcrtc(c, &cr, true);
    }
    case 0xB1: {                                 /* MODE_DIRTYFB */
        drm_mode_fb_dirty_cmd_t *d = a;
        if (!d) return -14;
        drm_fb_obj_t *fb = fb_lookup(d->fb_id);
        if (!fb || fb->card != card) return -22;
        drm_gem_t *g = gem_lookup(fb->gem_handle);
        if (!g) return -22;
        return c->be->dirty(c->be, g, fb->width, fb->height, fb->pitch);
    }

    case 0xB2:                                   /* MODE_CREATE_DUMB */
        return drm_create_dumb(c, (drm_mode_create_dumb_t *)a);
    case 0xB3: {                                 /* MODE_MAP_DUMB */
        drm_mode_map_dumb_t *m = a;
        if (!m) return -14;
        drm_gem_t *g = gem_lookup(m->handle);
        if (!g) return -2;
        m->offset = (uint64_t)g->handle << 12;   /* fake offset cookie */
        return 0;
    }
    case 0xB4: {                                 /* MODE_DESTROY_DUMB */
        drm_mode_destroy_dumb_t *d = a;
        if (!d) return -14;
        drm_gem_t *g = gem_lookup(d->handle);
        if (!g || g->card != card) return -2;
        if (g->priv) c->be->dumb_destroy(c->be, g->priv);
        if (!g->vram && g->phys)
            for (uint64_t pa = g->phys; pa < g->phys + g->size; pa += PAGE_SIZE)
                pmm_free(pa);
        if (c->cur_gem == g) { c->cur_gem = NULL; c->cur_fb = 0; }
        g->used = false;
        return 0;
    }
    default:
        return -25;                              /* -ENOTTY */
    }
}

/*
 *  mmap / ownership
 */
int drm_mmap_shared(int card, uint64_t off, uint64_t base, size_t npages) {
    if (card < 0 || card >= ncards) return -19;
    uint32_t handle = (uint32_t)(off >> 12);
    if (off != ((uint64_t)handle << 12)) return -22;
    drm_gem_t *g = gem_lookup(handle);
    if (!g || !g->phys) return -22;

    uint64_t need = (uint64_t)npages * PAGE_SIZE;
    if (need > ((g->size + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1)))
        return -22;

    for (size_t i = 0; i < npages; i++)
        if (!vmm_map(base + i * PAGE_SIZE, g->phys + i * PAGE_SIZE,
                     PTE_PRESENT | PTE_WRITABLE | PTE_USER))
            return -12;
    return 0;
}

bool drm_owns_phys(uint64_t phys) {
    for (int c = 0; c < ncards; c++)
        for (int i = 0; i < DRM_MAX_GEMS; i++) {
            drm_gem_t *g = &cards[c].gems[i];
            if (!g->used || !g->phys) continue;
            if (phys >= g->phys && phys < g->phys + g->size) return true;
        }
    /* VRAM slots owned by the BGA backend (above the console scanout) */
    for (int c = 0; c < ncards; c++)
        if (cards[c].active && cards[c].be == &be_bga) {
            uint64_t base = bga_vram_phys();
            uint64_t end  = base + bga_vram_size();
            if (phys >= base && phys < end) return true;
        }
    return false;
}
