#include "console/display.h"
#include "console/print.h"
#include "console/term.h"
#include "drivers/graphics/fb_font.h"
#include "drivers/vga/vga_text.h"
#include <stdarg.h>
#include <stdint.h>
#include <stddef.h>

/*
 * All console bytes flow through term_feed() (console/term.c):
 *   - raw mirror to COM1 (headless testing renders native ANSI)
 *   - UTF-8 -> codepoint -> CP437 glyph mapping
 *   - ANSI SGR colours + cursor/erase sequences
 * display.c keeps only the backend selection logic.
 *
 * Backend rule: whenever a framebuffer exists we render on it - even in
 * TTY mode.  The VGA hardware stays in a graphics mode after GRUB
 * gfxterm / the fb probe, so 0xB8000 would not be visible.  VGA text is
 * the fallback for machines without any framebuffer.
 */

static boot_mode_t active_mode = BOOT_MODE_TTY;

bool display_use_fb(void) {
    return fb_font_ready();
}

static uint8_t rgb_to_vga_fg(uint32_t rgb) {
    uint8_t r = (rgb >> 16) & 0xFF;
    uint8_t g = (rgb >>  8) & 0xFF;
    uint8_t b =  rgb        & 0xFF;
    if (r > 200 && g > 200 && b > 200) return PRINT_COLOR_WHITE;
    if (r < 50  && g > 150 && b > 150) return PRINT_COLOR_CYAN;
    if (r < 50  && g > 150 && b < 50 ) return PRINT_COLOR_GREEN;
    if (r > 150 && g > 150 && b < 50 ) return PRINT_COLOR_YELLOW;
    if (r > 150 && g < 50  && b < 50 ) return PRINT_COLOR_RED;
    if (r < 50  && g < 50  && b > 150) return PRINT_COLOR_BLUE;
    return PRINT_COLOR_LIGHT_GRAY;
}

void display_init(boot_mode_t mode) {
    active_mode = mode;
    /* no framebuffer -> real VGA text mode.  Program the Terminus
     * glyphs into the character generator so text mode shows the same
     * typeface as the graphics console ("same font for VGA mode").
     * Failure (or QEMU quirk) leaves the BIOS font, purely cosmetic. */
    if (!fb_font_ready())
        vga_font_upload();
    term_reset();
    display_clear();
}

boot_mode_t display_mode(void) { return active_mode; }

void display_clear(void) {
    if (display_use_fb()) fb_font_clear();
    else print_clear();
}

void display_putchar(char c) {
    term_feed(&c, 1);
}

static size_t disp_strlen(const char *s) {
    size_t n = 0; while (s[n]) n++; return n;
}

void display_puts(const char *s) {
    term_feed(s, disp_strlen(s));
}

void display_newline(void) { display_putchar('\n'); }

void display_set_color(uint32_t fg_rgb, uint32_t bg_rgb) {
    if (display_use_fb()) {
        fb_font_set_color(fg_rgb, bg_rgb);
    } else {
        print_set_color(rgb_to_vga_fg(fg_rgb), PRINT_COLOR_BLACK);
    }
}

void display_set_col(int col) {
    if (display_use_fb()) {
        int row; fb_font_get_cursor(NULL, &row);
        fb_font_set_cursor(col, row);
    } else {
        set_col((size_t)col);
    }
}

/* Internal unsigned int -> string */

static void puts_uint_pad(unsigned long v, int width) {
    /* format into tmp, then pad with zeros */
    char tmp[24]; int i = 0;
    if (!v) { tmp[i++] = '0'; } else {
        unsigned long w = v;
        while (w) { int d=(int)(w%10); tmp[i++]=(char)('0'+d); w/=10; }
    }
    /* tmp is reversed, length = i */
    for (int p = i; p < width; p++) display_putchar('0');
    while (i-- > 0) display_putchar(tmp[i]);
}

static void puts_hex(unsigned long v) {
    char tmp[18]; int i = 0;
    if (!v) { tmp[i++] = '0'; } else {
        while (v) {
            int d = (int)(v & 0xF);
            tmp[i++] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
            v >>= 4;
        }
    }
    while (i-- > 0) display_putchar(tmp[i]);
}

void display_printf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') { display_putchar(*p); continue; }
        p++;
        /* optional zero-pad width: %02u etc */
        int zero_pad = 0;
        int width = 0;
        if (*p == '0') { zero_pad = 1; p++; }
        while (*p >= '0' && *p <= '9') { width = width*10 + (*p - '0'); p++; }
        /* length modifiers: consume 'l' / 'll' / 'z'  */
        int long_mod = 0;
        while (*p == 'l' || *p == 'z') { long_mod = 1; p++; }
        (void)zero_pad;
        switch (*p) {
        case 'd': {
            long v = long_mod ? va_arg(ap, long) : va_arg(ap, int);
            if (v < 0) { display_putchar('-'); puts_uint_pad((unsigned long)-v, width); }
            else puts_uint_pad((unsigned long)v, width);
            break;
        }
        case 'u': puts_uint_pad(long_mod ? va_arg(ap, unsigned long) : va_arg(ap, unsigned), width); break;
        case 'x': puts_hex(long_mod ? va_arg(ap, unsigned long) : va_arg(ap, unsigned));  break;
        case 'p': display_puts("0x"); puts_hex((unsigned long)(uintptr_t)va_arg(ap, void*)); break;
        case 's': display_puts(va_arg(ap, const char *)); break;
        case 'c': display_putchar((char)va_arg(ap, int)); break;
        case '%': display_putchar('%'); break;
        default:  display_putchar('%'); display_putchar(*p); break;
        }
    }
    va_end(ap);
}

/* Log levels */
void display_ok(const char *msg) {
    display_newline();
    display_set_color(FB_WHITE, FB_BLACK); display_puts("[");
    display_set_color(FB_BRIGHT_GREEN, FB_BLACK); display_puts("  OK  ");
    display_set_color(FB_WHITE, FB_BLACK); display_puts("] "); display_puts(msg);
}
void display_info(const char *msg) {
    display_newline();
    display_set_color(FB_WHITE, FB_BLACK); display_puts("[");
    display_set_color(FB_CYAN, FB_BLACK);  display_puts(" INFO ");
    display_set_color(FB_WHITE, FB_BLACK); display_puts("] "); display_puts(msg);
}
void display_warn(const char *msg) {
    display_newline();
    display_set_color(FB_WHITE, FB_BLACK);  display_puts("[");
    display_set_color(FB_YELLOW, FB_BLACK); display_puts(" WARN ");
    display_set_color(FB_WHITE, FB_BLACK);  display_puts("] "); display_puts(msg);
}
void display_error(const char *msg) {
    display_newline();
    display_set_color(FB_WHITE, FB_BLACK); display_puts("[");
    display_set_color(FB_RED, FB_BLACK);   display_puts(" ERR  ");
    display_set_color(FB_WHITE, FB_BLACK); display_puts("] "); display_puts(msg);
}

/* Shell line renderer */
/*
 * Prompt and line rendering belong to userland.  These are no-op stubs
 * kept so the display API stays stable.
 */
void display_prompt(void)                            { }
void display_buffer(const char *buf, uint8_t len)   { (void)buf; (void)len; }
void display_prompt_and_buf(const char *buf, uint8_t buflen) { (void)buf; (void)buflen; }
