#pragma once

#include <stdint.h>

// Initialise the VMware SVGA-II driver and set initial resolution.
// Call vmm_init() (and optionally pmm_init()) before this.
void graphics_install_vmware(uint16_t w, uint16_t h);

// Change resolution at runtime.
void vmware_set_resolution(uint16_t w, uint16_t h);

// Fill entire screen with a 32-bit BGRX colour.
void vmware_clear_screen(uint32_t color);

// Draw a single pixel.  Out-of-bounds writes are silently ignored.
void vmware_draw_pixel(int x, int y, uint32_t color);

// Fill an axis-aligned rectangle.
void vmware_fill_rect(int x, int y, int w, int h, uint32_t color);

uint8_t *vmware_fb_addr(void);
