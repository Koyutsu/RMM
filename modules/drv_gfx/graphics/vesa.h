#pragma once

#include <stdint.h>
#include <stdbool.h>

//  VESA / Multiboot2 linear framebuffer driver
//
//  How it works
//  Rather than calling real-mode BIOS INT 10h from a 64-bit kernel (which
//  requires a complicated real-mode trampoline), we let GRUB set the video
//  mode for us.  Add a framebuffer tag to your multiboot2 header:
//
//      ; in header.asm, inside the multiboot2 header:
//      framebuffer_tag_start:
//          dw  5           ; type = framebuffer request
//          dw  1           ; flags (optional)
//          dd  framebuffer_tag_end - framebuffer_tag_start
//          dd  1024        ; preferred width  (0 = no preference)
//          dd  768         ; preferred height
//          dd  32          ; preferred bpp
//      framebuffer_tag_end:
//
//  GRUB will then pass framebuffer information inside the multiboot2
//  info structure.  Call vesa_init() early in kernel_main, passing the
//  physical address of the multiboot2 info block that was left in %rbx
//  by the bootloader.
//
//  If GRUB couldn't honour the request the framebuffer type will be
//  EGA/VGA text (type 2) - vesa_init() returns false in that case and
//  you can fall back to the VGA text driver.

// Multiboot2 tag types we care about
#define MB2_TAG_END         0
#define MB2_TAG_FRAMEBUFFER 8

// Framebuffer types
#define MB2_FB_TYPE_INDEXED  0
#define MB2_FB_TYPE_RGB      1
#define MB2_FB_TYPE_EGA_TEXT 2

// Multiboot2 structures (packed, so offsets match the Multiboot2 spec)

typedef struct __attribute__((packed)) {
    uint32_t type;
    uint32_t size;
} mb2_tag_t;

typedef struct __attribute__((packed)) {
    uint32_t type;          // 8
    uint32_t size;
    uint64_t framebuffer_addr;
    uint32_t framebuffer_pitch;
    uint32_t framebuffer_width;
    uint32_t framebuffer_height;
    uint8_t  framebuffer_bpp;
    uint8_t  framebuffer_type;
    uint16_t reserved;
    // colour info follows (we ignore it for RGB mode)
} mb2_tag_framebuffer_t;

// Public API

// Parse the multiboot2 info block at `mb2_info_phys` and set up the
// linear framebuffer.  Must be called after vmm_init().
// Returns true if a usable RGB framebuffer was found and mapped.
bool vesa_init(uint64_t mb2_info_phys);

// Fill the entire screen with a 32-bit packed colour (0x00RRGGBB for
// 24/32-bpp RGB modes).
void vesa_clear(uint32_t color);

// Draw a single pixel.  Out-of-range writes are silently ignored.
void vesa_draw_pixel(int x, int y, uint32_t color);

// Fill an axis-aligned rectangle.
void vesa_fill_rect(int x, int y, int w, int h, uint32_t color);

// Accessors
uint32_t vesa_width(void);
uint32_t vesa_height(void);
uint32_t vesa_pitch(void);   // bytes per scanline
bool     vesa_available(void);

/* Returns the virtual address of the framebuffer, or NULL if not ready. */
uint8_t *vesa_fb_addr(void);
