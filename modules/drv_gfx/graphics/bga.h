#pragma once

#include <stdint.h>
#include <stdbool.h>

//  Bochs / QEMU BGA (VBE) graphics driver
//
//  Works with:
//    - QEMU  -vga std  (or -device VGA, -device bochs-display)
//    - Bochs  display_library: sdl  (with bx_options.vga.extension = vbe)
//    - VirtualBox SVGA (partial, resolution-set only)
//
//  The device exposes two interfaces:
//    1.  Legacy x86 VBE I/O ports  (0x01CE / 0x01CF) - always available.
//    2.  PCI BAR2 MMIO register window, used when available (preferred).
//
//  This driver uses the I/O-port path for maximum compatibility.

// Initialise BGA at the requested resolution.
// Must call vmm_init() first so the framebuffer can be mapped.
// Returns true on success.
bool bga_init(uint16_t width, uint16_t height, uint16_t bpp);

// Change resolution (disables + reconfigures + re-enables).
bool bga_set_resolution(uint16_t width, uint16_t height, uint16_t bpp);

// Fill the whole screen with a packed pixel value.
void bga_clear(uint32_t color);

// Draw a single pixel (bounds-checked, silently ignores out-of-range).
void bga_draw_pixel(int x, int y, uint32_t color);

// Fill an axis-aligned rectangle.
void bga_fill_rect(int x, int y, int w, int h, uint32_t color);

// Query current dimensions.
uint32_t bga_width(void);
uint32_t bga_height(void);

// True if the driver has been successfully initialised.
bool bga_available(void);

uint8_t *bga_fb_addr(void);

/* KMS backend hooks (used by drivers/gpu/drm.c) */
uint32_t bga_vram_size(void);      /* total VRAM bytes (BAR size)      */
bool bga_map_vram_full(void);      /* identity-map the whole BAR */
uint32_t bga_vram_phys(void);      /* physical base of VRAM            */
uint32_t bga_pitch(void);          /* current scanline pitch in bytes  */
/* Adopt the FIRMWARE-programmed dispi mode without touching the device
 * (used when the boot framebuffer came from the BIOS/GRUB path but the
 * hardware is a std-vga/bochs device).  Returns true when adopted.   */
bool bga_quiesce_adopt(void);

/* Enlarge the addressable virtual screen (rows) so Y_OFFSET can pan.
 * Zeroes the pan offsets, call once before handing out VRAM slots.  */
void bga_kms_set_virtual(uint32_t virt_height_rows);
/* Page flip: point the scanout at the given scanline.                */
void bga_kms_pan(uint32_t y_offset_rows);
