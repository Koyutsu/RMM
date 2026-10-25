#pragma once
#include <stdint.h>
#include <stddef.h>

/*
 * term - terminal emulation for the RMM console
 *
 * Sits between the kernel/userland byte streams and the two console
 * backends (VGA text 0xB8000 in TTY mode, fb_font in graphics mode).
 *
 *   - decodes UTF-8 into codepoints (so box-drawing characters
 *     render as real glyphs instead of raw bytes / mojibake)
 *   - parses ANSI CSI sequences: SGR colours (ESC[...m), cursor moves,
 *     erase line/screen, so neofetch & friends get real colours
 *   - maps box/block codepoints to CP437 glyph codes (VGA hardware font
 *     on TTY, fb_font BOX_GLYPHS on the framebuffer)
 *
 * Raw bytes are mirrored to COM1 before parsing, so an external serial
 * terminal sees the original UTF-8 + ANSI stream and renders it natively.
 */

/* Feed raw output bytes through the terminal state machine. */
void term_feed(const char *s, size_t n);

/* Reset terminal state (mode switch, clear). */
void term_reset(void);
