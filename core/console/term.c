#include "console/term.h"
#include "console/display.h"
#include "console/print.h"
#include "drivers/graphics/fb_font.h"
#include "drivers/serial/serial.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/*
 * State machine
 * Two independent mini-parsers share the byte stream:
 *
 *   1. UTF-8 decoder, assembles multi-byte sequences into codepoints.
 *   2. ANSI CSI parser - ESC [ ... <final>.  While inside a CSI sequence the
 *      UTF-8 decoder is bypassed (control bytes are plain ASCII).
 *
 * Raw bytes are mirrored to COM1 before parsing so an external serial
 * terminal sees the original UTF-8 + ANSI stream and renders it natively.
 * (This is now the only serial mirror, display.c no longer mirrors.)
 */

typedef enum { TERM_GROUND, TERM_ESC, TERM_CSI } term_state_t;

static term_state_t state      = TERM_GROUND;

/* UTF-8 assembly */
static uint32_t utf8_cp   = 0;
static int      utf8_need = 0;

/* CSI assembly */
static char     csi_buf[16];
static int      csi_len    = 0;
static bool     csi_private = false;   /* '?'-prefixed private sequences */

/* ANSI SGR colour state (16-colour model) */
static uint8_t  ansi_fg     = 7;   /* light gray  */
static uint8_t  ansi_bg     = 0;   /* black       */
static bool     ansi_bold   = false;
static bool     ansi_reverse = false;

/* Colour tables */
static const uint32_t ansi_rgb[16] = {
    0x000000, 0x0000AA, 0x00AA00, 0x00AAAA,   /* black blue green  cyan  */
    0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA,   /* red   magenta brown gray  */
    0x555555, 0x5555FF, 0x55FF55, 0x55FFFF,   /* +bright                  */
    0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF,
};

/* print.h enum == VGA IRGB order, so ANSI 0-15 maps straight over */
static const uint8_t ansi_vga[16] = {
    PRINT_COLOR_BLACK,        PRINT_COLOR_BLUE,
    PRINT_COLOR_GREEN,        PRINT_COLOR_CYAN,
    PRINT_COLOR_RED,          PRINT_COLOR_MAGENTA,
    PRINT_COLOR_BROWN,        PRINT_COLOR_LIGHT_GRAY,
    PRINT_COLOR_DARK_GRAY,    PRINT_COLOR_LIGHT_BLUE,
    PRINT_COLOR_LIGHT_GREEN,  PRINT_COLOR_LIGHT_CYAN,
    PRINT_COLOR_LIGHT_RED,    PRINT_COLOR_PINK,
    PRINT_COLOR_YELLOW,       PRINT_COLOR_WHITE,
};

/* Push current SGR state into the active backend. */
static void apply_sgr(void) {
    uint8_t fg = ansi_fg, bg = ansi_bg;
    if (ansi_reverse) { uint8_t t = fg; fg = bg; bg = t; }
    if (display_use_fb()) {
        uint32_t f = ansi_rgb[fg & 15];
        if (ansi_bold && fg < 8) f = ansi_rgb[fg + 8];
        fb_font_set_color(f, ansi_rgb[bg & 15]);
    } else {
        uint8_t f = ansi_vga[fg & 15];
        if (ansi_bold && fg < 8) f = ansi_vga[fg + 8];
        print_set_color(f, ansi_vga[bg & 15]);
    }
}

/* Reset to the classic defaults (white on black) and re-apply. */
void term_reset_colors(void) {
    ansi_fg = 7; ansi_bg = 0; ansi_bold = false; ansi_reverse = false;
    apply_sgr();
}

/* Emit one visible codepoint to the active backend */
static void term_emit_cp(uint32_t cp) {
    if (cp == '\n' || cp == '\r' || cp == '\b' || cp == '\t') {
        if (display_use_fb()) fb_font_putchar((char)cp);
        else print_char((char)cp);
        return;
    }
    if (cp < 0x80) {
        if (cp < 0x20) return;                    /* other ctrl: swallow  */
        if (display_use_fb()) fb_font_putchar((char)cp);
        else print_char((char)cp);
        return;
    }
    if (display_use_fb()) {
        fb_font_putcodepoint(cp);                 /* box/block -> glyphs   */
    } else {
        uint8_t g = fb_cp437_for_unicode(cp);     /* VGA font is CP437    */
        print_char(g ? (char)g : '?');
    }
}

/* CSI parameter helpers */
static int csi_first_param(int def) {
    int v = 0; bool any = false;
    for (int i = 0; i < csi_len; i++) {
        if (csi_buf[i] >= '0' && csi_buf[i] <= '9') { v = v*10 + (csi_buf[i]-'0'); any = true; }
        else break;
    }
    return any ? v : def;
}

/* Parse "p1;p2" (either may be empty -> def). */
static void csi_two_params(int def, int *p1, int *p2) {
    int vals[2] = { def, def };
    bool any[2] = { false, false };
    int which = 0, v = 0;
    for (int i = 0; i <= csi_len; i++) {
        char ch = (i < csi_len) ? csi_buf[i] : ';';   /* sentinel end */
        if (ch >= '0' && ch <= '9') { v = v*10 + (ch-'0'); any[which] = true; }
        else {
            if (which < 2 && any[which]) vals[which] = v;
            v = 0; which++;
            if (i == csi_len || which >= 2) break;
        }
    }
    *p1 = vals[0]; *p2 = vals[1];
}

/* SGR handler (ESC [ ... m) */
static void do_sgr(void) {
    int  vals[8]; int n = 0;
    int  cur = 0; bool any = false;
    for (int i = 0; i <= csi_len && n < 8; i++) {
        if (i < csi_len && csi_buf[i] >= '0' && csi_buf[i] <= '9') {
            cur = cur * 10 + (csi_buf[i] - '0'); any = true;
        } else if (i == csi_len || csi_buf[i] == ';') {
            vals[n++] = any ? cur : 0;   /* empty field == 0 (reset) */
            cur = 0; any = false;
        }
    }
    if (n == 0) { vals[0] = 0; n = 1; }
    for (int i = 0; i < n; i++) {
        int v = vals[i];
        if      (v == 0)  { ansi_fg = 7; ansi_bg = 0; ansi_bold = false; ansi_reverse = false; }
        else if (v == 1)  ansi_bold = true;
        else if (v == 2)  ansi_bold = false;
        else if (v == 7)  ansi_reverse = true;
        else if (v == 22) ansi_bold = false;
        else if (v == 27) ansi_reverse = false;
        else if (v >= 30 && v <= 37) ansi_fg = (uint8_t)(v - 30);
        else if (v == 39) ansi_fg = 7;
        else if (v >= 40 && v <= 47) ansi_bg = (uint8_t)(v - 40);
        else if (v == 49) ansi_bg = 0;
        else if (v >= 90 && v <= 97)  ansi_fg = (uint8_t)(v - 90 + 8);
        else if (v >= 100 && v <= 107) ansi_bg = (uint8_t)(v - 100 + 8);
    }
    apply_sgr();
}

/* Non-SGR CSI handlers */
static void do_csi_final(char f) {
    switch (f) {
    case 'm':                                   /* SGR                  */
        if (!csi_private) do_sgr();
        break;
    case 'K': {                                 /* erase in line        */
        int mode = csi_first_param(0);
        if (mode == 0) {
            if (display_use_fb()) fb_font_erase_to_eol();
            else print_erase_to_eol();
        }
        break;
    }
    case 'J': {                                 /* erase in display     */
        int mode = csi_first_param(0);
        if (mode == 2) {
            apply_sgr();                        /* bg first, then clear */
            if (display_use_fb()) fb_font_clear();
            else print_clear();
        }
        break;
    }
    case 'H':                                   /* cursor position 1-based */
    case 'f': {
        if (display_use_fb()) {
            int row, col; csi_two_params(1, &row, &col);
            if (row < 1) row = 1; if (col < 1) col = 1;
            fb_font_set_cursor(col - 1, row - 1);
        }
        break;
    }
    case 'A': case 'B': {                       /* cursor up / down     */
        int d = csi_first_param(1); if (d < 1) d = 1;
        if (display_use_fb()) {
            int c, r; fb_font_get_cursor(&c, &r);
            r += (f == 'A') ? -d : d; if (r < 0) r = 0;
            fb_font_set_cursor(c, r);
        }
        break;
    }
    case 'C': case 'D': {                       /* cursor fwd / back    */
        int d = csi_first_param(1); if (d < 1) d = 1;
        if (display_use_fb()) {
            int c, r; fb_font_get_cursor(&c, &r);
            c += (f == 'C') ? d : -d; if (c < 0) c = 0;
            fb_font_set_cursor(c, r);
        }
        break;
    }
    default: break;                             /* ignored              */
    }
    state = TERM_GROUND;
}

/* Byte pump */
void term_feed(const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        uint8_t b = (uint8_t)s[i];

        serial_print_byte((char)b);             /* raw mirror, pre-parse */

        /* escape / CSI parsing */
        if (state == TERM_GROUND && b == 0x1B) { state = TERM_ESC; continue; }
        if (state == TERM_ESC) {
            if (b == '[') { state = TERM_CSI; csi_len = 0; csi_private = false; continue; }
            state = TERM_GROUND;                /* other ESC: unsupported */
            continue;
        }
        if (state == TERM_CSI) {
            if (b == '?' && csi_len == 0) { csi_private = true; continue; }
            if (b >= '0' && b <= '9') { if (csi_len < (int)sizeof(csi_buf)-1) csi_buf[csi_len++] = (char)b; continue; }
            if (b == ';' || b == ':') { if (csi_len < (int)sizeof(csi_buf)-1) csi_buf[csi_len++] = ';'; continue; }
            if (b >= '@' && b <= '~') { do_csi_final((char)b); }   /* resets state */
            else state = TERM_GROUND;           /* malformed - abort     */
            continue;
        }

        /* UTF-8 assembly */
        if (utf8_need > 0) {
            if ((b & 0xC0) == 0x80) {
                utf8_cp = (utf8_cp << 6) | (b & 0x3F);
                if (--utf8_need == 0) term_emit_cp(utf8_cp);
            } else {
                utf8_need = 0;                  /* broken sequence      */
                term_emit_cp('?');
                i--;                            /* reprocess this byte  */
            }
            continue;
        }
        if (b < 0x80) { term_emit_cp(b); continue; }
        if ((b & 0xE0) == 0xC0) { utf8_cp = b & 0x1F; utf8_need = 1; continue; }
        if ((b & 0xF0) == 0xE0) { utf8_cp = b & 0x0F; utf8_need = 2; continue; }
        if ((b & 0xF8) == 0xF0) { utf8_cp = b & 0x07; utf8_need = 3; continue; }
        term_emit_cp('?');                      /* stray / invalid byte */
    }
}

void term_reset(void) {
    state = TERM_GROUND;
    utf8_cp = 0; utf8_need = 0;
    csi_len = 0;
    term_reset_colors();
}
