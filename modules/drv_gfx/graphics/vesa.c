#include "drivers/graphics/vesa.h"
#include "drivers/memory/vmm.h"
#include "console/print.h"

#include <stdint.h>
#include <stdbool.h>

//  Driver state
static uint8_t  *vesa_fb      = NULL;   // virtual address of framebuffer
static uint32_t  vesa_w       = 0;
static uint32_t  vesa_h       = 0;
static uint32_t  vesa_p       = 0;      // pitch (bytes per scanline)
static uint8_t   vesa_bpp     = 0;      // bits per pixel (24 or 32 expected)
static uint8_t   vesa_bypp    = 0;      // bytes per pixel
static bool      vesa_ready   = false;

//  Multiboot2 tag walker

// The multiboot2 info block layout:
//   [0..3]  total_size  (uint32)
//   [4..7]  reserved    (uint32)
//   [8..]   tags        (variable, each 8-byte aligned)

static mb2_tag_framebuffer_t *find_fb_tag(uint64_t mb2_phys) {
    // The whole info block is within the first 4 GiB (GRUB is a 32-bit
    // bootloader).  Our identity map covers low RAM so the cast is safe.
    uint8_t  *base  = (uint8_t *)(uintptr_t)mb2_phys;
    uint32_t  total = *(uint32_t *)base;

    uint8_t *ptr = base + 8;   // skip fixed header
    uint8_t *end = base + total;

    while (ptr < end) {
        mb2_tag_t *tag = (mb2_tag_t *)ptr;
        if (tag->type == MB2_TAG_END) break;
        if (tag->type == MB2_TAG_FRAMEBUFFER)
            return (mb2_tag_framebuffer_t *)tag;
        // Tags are padded to 8-byte alignment
        ptr += (tag->size + 7) & ~7u;
    }
    return NULL;
}

//  Public API
bool vesa_init(uint64_t mb2_info_phys) {
    if (!mb2_info_phys) {
        print_str("[VESA] No multiboot2 info pointer supplied\n");
        return false;
    }

    mb2_tag_framebuffer_t *tag = find_fb_tag(mb2_info_phys);
    if (!tag) {
        print_str("[VESA] No framebuffer tag in multiboot2 info\n");
        return false;
    }

    if (tag->framebuffer_type != MB2_FB_TYPE_RGB) {
        printf("[VESA] Framebuffer type %d is not RGB - ignoring\n",
               (int)tag->framebuffer_type);
        return false;
    }

    vesa_w    = tag->framebuffer_width;
    vesa_h    = tag->framebuffer_height;
    vesa_p    = tag->framebuffer_pitch;
    vesa_bpp  = tag->framebuffer_bpp;
    vesa_bypp = (vesa_bpp + 7) / 8;

    uint64_t fb_phys = tag->framebuffer_addr;
    uint64_t fb_size = (uint64_t)vesa_p * vesa_h;

    printf("[VESA] %dx%d @ %d bpp, pitch=%d\n",
           vesa_w, vesa_h, vesa_bpp, vesa_p);
    printf("[VESA] Framebuffer phys=0x%x, size=%d KiB\n",
           (uint32_t)fb_phys, (uint32_t)(fb_size / 1024));

    // Map the framebuffer into virtual address space
    uint64_t ps = fb_phys & ~(uint64_t)(PAGE_SIZE - 1);
    uint64_t pe = fb_phys + fb_size;
    if (!vmm_identity_map_range(ps, pe, PTE_PRESENT | PTE_WRITABLE)) {
        print_str("[VESA] ERROR: vmm_identity_map_range failed\n");
        return false;
    }

    vesa_fb    = (uint8_t *)(uintptr_t)fb_phys;
    vesa_ready = true;
    print_str("[VESA] Driver ready\n");
    return true;
}

// Write a colour at byte offset `off` with the correct pixel width.
static inline void write_pixel_at(uint8_t *dst, uint32_t color) {
    // We handle 32-bpp and 24-bpp, anything else is a no-op.
    if (vesa_bypp == 4) {
        *((uint32_t *)dst) = color;
    } else if (vesa_bypp == 3) {
        dst[0] = (uint8_t)( color        & 0xFF);   // B
        dst[1] = (uint8_t)((color >>  8) & 0xFF);   // G
        dst[2] = (uint8_t)((color >> 16) & 0xFF);   // R
    } else if (vesa_bypp == 2) {
        // Convert 0x00RRGGBB -> RGB565
        uint16_t r = ((color >> 16) & 0xFF) >> 3;
        uint16_t g = ((color >>  8) & 0xFF) >> 2;
        uint16_t b = ( color        & 0xFF) >> 3;
        *((uint16_t *)dst) = (uint16_t)((r << 11) | (g << 5) | b);
    }
}

void vesa_clear(uint32_t color) {
    if (!vesa_ready) return;
    for (uint32_t y = 0; y < vesa_h; y++) {
        uint8_t *row = vesa_fb + y * vesa_p;
        for (uint32_t x = 0; x < vesa_w; x++)
            write_pixel_at(row + x * vesa_bypp, color);
    }
}

void vesa_draw_pixel(int x, int y, uint32_t color) {
    if (!vesa_ready) return;
    if (x < 0 || y < 0) return;
    if ((uint32_t)x >= vesa_w || (uint32_t)y >= vesa_h) return;
    write_pixel_at(vesa_fb + (uint32_t)y * vesa_p + (uint32_t)x * vesa_bypp,
                   color);
}

void vesa_fill_rect(int x, int y, int w, int h, uint32_t color) {
    if (!vesa_ready) return;
    for (int row = y; row < y + h; row++)
        for (int col = x; col < x + w; col++)
            vesa_draw_pixel(col, row, color);
}

uint32_t vesa_width(void)    { return vesa_w;     }
uint32_t vesa_height(void)   { return vesa_h;     }
uint32_t vesa_pitch(void)    { return vesa_p;     }
bool     vesa_available(void){ return vesa_ready; }
uint8_t *vesa_fb_addr(void) { return vesa_fb; }
