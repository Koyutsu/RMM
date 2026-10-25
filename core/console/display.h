#pragma once
#include <stdint.h>
#include <stdbool.h>

typedef enum {
    BOOT_MODE_GRAPHICS = 0,   /* framebuffer + bitmap font */
    BOOT_MODE_TTY      = 1,   /* VGA text mode (80x25)     */
} boot_mode_t;

/*
 * display - unified output layer
 *
 * After display_init() all output goes to whichever backend is active:
 *   BOOT_MODE_GRAPHICS  ->  fb_font  (bitmap text on framebuffer)
 *   BOOT_MODE_TTY       ->  VGA text buffer at 0xB8000
 *
 * The console/print.h functions continue to write to VGA text directly
 * for the early boot messages (before graphics is up).  display_* is
 * used after the mode is selected.
 */

void display_init(boot_mode_t mode);

/* Effective backend selection: TRUE when console output should go to the
 * framebuffer.  This is the case for BOOT_MODE_GRAPHICS and also for
 * BOOT_MODE_TTY whenever a framebuffer is available - GRUB gfxterm and
 * our own fb probe leave the VGA hardware in a graphics mode, so the
 * text buffer at 0xB8000 would be invisible.  VGA text is only used when
 * no framebuffer exists at all. */
bool display_use_fb(void);

/* These mirror the print.h API but route to the active backend. */
void display_clear(void);
void display_putchar(char c);
void display_puts(const char *s);
void display_set_color(uint32_t fg_rgb, uint32_t bg_rgb);
void display_set_col(int col);
void display_newline(void);
void display_printf(const char *fmt, ...);

/* Shortcuts for the four log-level prefixes */
void display_ok   (const char *msg);
void display_info (const char *msg);
void display_warn (const char *msg);
void display_error(const char *msg);

/* Prompt and input buffer rendering */
void display_prompt(void);
void display_buffer(const char *buf, uint8_t len);

boot_mode_t display_mode(void);

/* Render the full prompt + buffer on the current line in one call.
 * Erases one char past the end so backspace is visually correct. */
void display_prompt_and_buf(const char *buf, uint8_t buflen);
