#pragma once
#include <stdint.h>
#include <stdbool.h>

#define VGA_WIDTH  80
#define VGA_HEIGHT 25

void text_mode_init(void);
void text_mode_clear(void);
void text_mode_write(const unsigned char *msg);

/* program the Terminus 8x16 glyphs into the VGA plane-2 character
 * generator so real text mode uses the same typeface as the framebuffer
 * console.  Returns false when no font table is available. */
bool vga_font_upload(void);
