#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/*
 * fb_font - software text renderer for linear framebuffers
 *
 * Draws an 8x16 bitmap font directly into the active framebuffer
 * (VESA / BGA / VMware).  Provides a drop-in parallel to the VGA
 * text driver so boot messages appear in graphics mode.
 *
 * Colour format: 0x00RRGGBB  (same as what the graphics drivers use)
 */

/* Initialise with the framebuffer's virtual address, dimensions and pitch.
 * Call after the graphics driver is up.  bpp must be 32 (others ignored). */
void fb_font_init(uint8_t *fb, uint32_t width, uint32_t height,
                  uint32_t pitch, uint8_t bpp);

/* True once fb_font_init() has been called successfully. */
bool fb_font_ready(void);

/* Set foreground / background colour (0x00RRGGBB). */
void fb_font_set_color(uint32_t fg, uint32_t bg);

/* Print a single character at current cursor position. */
void fb_font_putchar(char c);

/* Render a CP437 box/block glyph (0xB0-0xDF) at the current position.
 * Same cursor/scroll behaviour as fb_font_putchar. */
void fb_font_putglyph(uint8_t cp437_code);

/* Render a decoded Unicode codepoint (box/block elements map to CP437
 * glyphs, unmapped codepoints print '?'). */
void fb_font_putcodepoint(uint32_t cp);

/* Unicode box/block codepoint -> CP437 glyph code (0 = unmapped). */
uint8_t fb_cp437_for_unicode(uint32_t cp);

/* Erase from the cursor to end-of-line with the current background. */
void fb_font_erase_to_eol(void);

/* Print a NUL-terminated string. */
void fb_font_puts(const char *s);

/* UTF-8 aware puts: decodes box/block codepoints into CP437 glyphs. */
void fb_font_utf8_puts(const char *s);

/* printf-style print to framebuffer. */
void fb_font_printf(const char *fmt, ...);

/* Move text cursor to (col, row) in character cells. */
void fb_font_set_cursor(int col, int row);

/* Get current cursor position. */
void fb_font_get_cursor(int *col, int *row);

/* Clear the framebuffer to the current background colour. */
void fb_font_clear(void);

/* How many character columns / rows fit on screen. */
int fb_font_cols(void);
int fb_font_rows(void);

/* the 256x16 console glyph table (Terminus, CP437 ordering).
 * Shared with the VGA text-mode character generator (vga_font_upload). */
const uint8_t *fb_font_console_glyphs(void);

/* Predefined colours (0x00RRGGBB) */
#define FB_BLACK        0x000000u
#define FB_WHITE        0xFFFFFFu
#define FB_RED          0xCC0000u
#define FB_GREEN        0x00CC00u
#define FB_BRIGHT_GREEN 0x00FF88u
#define FB_YELLOW       0xFFCC00u
#define FB_CYAN         0x00CCCCu
#define FB_BLUE         0x0066FFu
#define FB_LIGHT_GRAY   0xAAAAAAu
#define FB_DARK_BG      0x1E1E2Eu
#define FB_DARKER_BG    0x161622u
#define FB_HIGHLIGHT    0x3E3E5Eu
