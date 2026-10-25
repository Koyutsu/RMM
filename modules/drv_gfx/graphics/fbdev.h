#pragma once
/*
 *  fbdev - framebuffer device layer
 *
 *  Graphics drivers (virtio-gpu, VMware SVGA-II, BGA, VBE/BIOS) all register
 *  a fbdev_t.  The framebuffer console binds to the *primary* fbdev, chosen
 *  by priority:
 *
 *      virtio-gpu  110   (QEMU fast path, DMA flush)
 *      VMware SVGA 100   (VMware fast path)
 *      BGA/dispi    90   (QEMU/Bochs std-vga, programmatic mode set)
 *      VBE (multiboot) 80 (GRUB-set mode, real hardware path)
 *
 *  DMI detection (dmi.c) can bias priorities: on VMware the SVGA driver is
 *  preferred, on QEMU the virtio-gpu driver is preferred.
 */
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct fbdev {
    const char *name;                   /* "virtio-gpu", "vmware-svga", ...  */
    uint8_t    *fb;                     /* linear framebuffer (mapped)     */
    uint32_t    width, height;          /* pixels                          */
    uint32_t    pitch;                  /* bytes per scanline              */
    uint8_t     bpp;                    /* bits per pixel (32 required)    */

    int         priority;               /* higher = preferred primary      */

    /* Optional: batched flush for RAM-backed framebuffers (virtio-gpu).
     * Drivers that render straight into video memory leave it NULL.
     *   flush_rows - rows [y0..y1], full width (scroll/clear)
     *   flush_rect - small dirty rect, e.g. one glyph cell (per-char) */
    int       (*flush_rows)(struct fbdev *, uint32_t y0, uint32_t y1);
    int       (*flush_rect)(struct fbdev *, uint32_t x, uint32_t y,
                            uint32_t w, uint32_t h);

    void       *priv;
    struct fbdev *next;
} fbdev_t;

/* Register a framebuffer device.  The highest-priority one becomes primary.
 * Registering a higher-priority fbdev later demotes the previous primary
 * automatically (the console simply switches on next use). */
void      fbdev_register(fbdev_t *f);
fbdev_t  *fbdev_primary(void);
int       fbdev_count(void);

/* /dev/fb0 support
 * Linux-style framebuffer device semantics on top of the registered fbdevs:
 *   - fbdev_mmap_shared(base, npages, off): map the primary framebuffer's
 *     Actual memory (video RAM or the driver's backing RAM) into user space
 *     MAP_SHARED - writes go straight to the pixels, exactly like mmap of
 *     /dev/fb0 on Linux.
 *   - fbdev_flush_all(): RAM-backed drivers (virtio-gpu) push their whole
 *     framebuffer to the host display after user writes / FBIOPAN_DISPLAY.
 *   - fbdev_owns_phys(phys): true when `phys` lies inside any registered
 *     framebuffer - impl_munmap must never pmm_free() video memory.       */
int  fbdev_mmap_shared(uint64_t base, size_t npages, uint64_t off);
void fbdev_flush_all(void);
bool fbdev_owns_phys(uint64_t phys);

/*
 *  Linux fbdev UAPI, byte-exact with linux/fb.h on x86-64 so
 *  portable userland (musl + SDL/wlroots-style code) works unmodified.
 *  Only the fields /dev/fb0 actually reports are meaningful here.
 */
#define FBIOGET_VSCREENINFO   0x4600
#define FBIOPUT_VSCREENINFO   0x4601
#define FBIOGET_FSCREENINFO   0x4602
#define FBIOPAN_DISPLAY       0x4605
#define FBIOGET_CON2FBMAP     0x460F

#define FB_TYPE_PACKED_PIXELS 0
#define FB_VISUAL_TRUECOLOR   2
#define FB_ACCEL_NONE         0
#define FB_VMODE_NONINTERLACED 0
#define FB_ACTIVATE_NOW       0

typedef struct {
    uint32_t offset, length, msb_right;
} fb_bitfield_t;

typedef struct {
    uint32_t xres, yres;            /* visible resolution               */
    uint32_t xres_virtual, yres_virtual;
    uint32_t xoffset, yoffset;
    uint32_t bits_per_pixel;
    uint32_t grayscale;             /* 0 = colour                       */
    fb_bitfield_t red, green, blue, transp;
    uint32_t nonstd;
    uint32_t activate;
    uint32_t height, width;         /* mm - 0 = unknown                 */
    uint32_t accel_flags;
    uint32_t pixclock, left_margin, right_margin;
    uint32_t upper_margin, lower_margin;
    uint32_t hsync_len, vsync_len, sync, vmode, rotate;
    uint32_t reserved[5];
} fb_var_screeninfo_t;              /* 160 bytes, layout == linux/fb.h  */

typedef struct {
    char     id[16];                /* e.g. "BGA", "virtio-gpu"         */
    uint64_t smem_start;            /* physical address of framebuffer  */
    uint32_t smem_len;
    uint32_t type;                  /* FB_TYPE_PACKED_PIXELS            */
    uint32_t type_aux;
    uint32_t visual;                /* FB_VISUAL_TRUECOLOR              */
    uint16_t xpanstep, ypanstep, ywrapstep;
    uint32_t line_length;           /* == pitch                         */
    uint64_t mmio_start;
    uint32_t mmio_len;
    uint32_t accel;                 /* FB_ACCEL_NONE                    */
    uint16_t capabilities;
    uint16_t reserved[2];
} fb_fix_screeninfo_t;              /* layout == linux/fb.h (x86-64)    */
