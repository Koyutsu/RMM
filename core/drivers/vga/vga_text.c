#include "drivers/vga/vga_text.h"
#include "drivers/graphics/fb_font.h"
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

static volatile uint16_t *const VIDEO_RAM = (volatile uint16_t *)0xB8000;

static inline void outb_vga(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}
static inline uint8_t inb_vga(uint16_t port) {
    uint8_t v;
    __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

/*
 *  programmable character generator: Terminus in real text mode
 *
 *  The VGA BIOS font is whatever the video BIOS carries, we want the same
 *  Terminus typeface the framebuffer console uses.  Classic plane-2 font
 *  load (VGADOC / Linux vgacon_do_font_op sequence):
 *
 *    1. sequencer synchronous reset
 *    2. map mask -> plane 2 only, odd/even addressing off (seq 0x04 = 0x06)
 *    3. copy 256 x cell-height bytes to 0xA0000 (char N at offset N*32)
 *    4. restore sequencer (odd/even ON, all planes)
 *    5. CRTC: max scanline = cell height - 1, cursor rows, underline
 *    6. attribute: "line graphics" bit off so codes 0xC0-0xDF render our
 *       real CP437 glyphs instead of the 9th-dot duplication trick
 */
#define VGA_FONT_CELL  16        /* bytes per glyph (8x16 font)  */
#define VGA_FONT_CHARS 256

bool vga_font_upload(void) {
    const uint8_t *font = fb_font_console_glyphs();
    if (!font) return false;

    volatile uint8_t *plane2 = (volatile uint8_t *)0xA0000;

    /* 1-2: sequencer into font-load mode */
    outb_vga(0x3C4, 0x00); outb_vga(0x3C5, 0x01);   /* synchronous reset */
    outb_vga(0x3C4, 0x02); outb_vga(0x3C5, 0x04);   /* map mask: plane 2 */
    outb_vga(0x3C4, 0x04); outb_vga(0x3C5, 0x06);   /* odd/even off, ext */
    outb_vga(0x3C4, 0x00); outb_vga(0x3C5, 0x03);   /* end reset         */

    /* 3: copy glyphs, char N lives at plane2[N * 32] */
    for (int ch = 0; ch < VGA_FONT_CHARS; ch++) {
        uint8_t *dst = (uint8_t *)(plane2 + ch * 32);
        const uint8_t *src = font + ch * VGA_FONT_CELL;
        for (int r = 0; r < VGA_FONT_CELL; r++)
            dst[r] = src[r];
    }

    /* 4: sequencer back to text mode */
    outb_vga(0x3C4, 0x02); outb_vga(0x3C5, 0x0F);   /* all planes        */
    outb_vga(0x3C4, 0x04); outb_vga(0x3C5, 0x02);   /* odd/even on, ext  */

    /* 5: CRTC - max scanline 15 (16-row cells), cursor 14->15,
     * underline location 15 */
    outb_vga(0x3D4, 0x09);
    uint8_t msr = inb_vga(0x3D5);
    outb_vga(0x3D5, (uint8_t)((msr & 0xE0) | (VGA_FONT_CELL - 1)));

    outb_vga(0x3D4, 0x0A);
    uint8_t cs = inb_vga(0x3D5);
    outb_vga(0x3D5, (uint8_t)((cs & 0x1F) | 0x0E));  /* start 14, enable */

    outb_vga(0x3D4, 0x0B);
    uint8_t ce = inb_vga(0x3D5);
    outb_vga(0x3D5, (uint8_t)((ce & 0x1F) | 0x0F));  /* end 15 */

    /* underline location = cell height - 1 (char height programmable) */
    outb_vga(0x3D4, 0x18); outb_vga(0x3D5, (uint8_t)(VGA_FONT_CELL - 1));

    /* 6: attribute mode control, clear "line graphics" (bit 2) */
    (void)inb_vga(0x3DA);            /* reset attribute index/data flip  */
    outb_vga(0x3C0, 0x10);           /* select attribute reg 0x10        */
    uint8_t amc = inb_vga(0x3C1);
    outb_vga(0x3C0, (uint8_t)((amc & ~0x04) | 0x00));
    (void)inb_vga(0x3DA);            /* re-arm flip-flop, leave data mode */

    return true;
}

static volatile struct {
    int col;
    int row;
    uint16_t buffer[VGA_HEIGHT][VGA_WIDTH];
} text_mode_state;

static void scroll(void) {
    for (int i = 1; i < VGA_HEIGHT; i++)
        for (int j = 0; j < VGA_WIDTH; j++)
            text_mode_state.buffer[i-1][j] = text_mode_state.buffer[i][j];
    for (int j = 0; j < VGA_WIDTH; j++)
        text_mode_state.buffer[VGA_HEIGHT-1][j] = 0x0F20;
    text_mode_state.row = VGA_HEIGHT - 1;
    text_mode_state.col = 0;
}

static void flush(void) {
    for (int i = 0; i < VGA_HEIGHT; i++)
        for (int j = 0; j < VGA_WIDTH; j++)
            VIDEO_RAM[i * VGA_WIDTH + j] = text_mode_state.buffer[i][j];
}

void text_mode_write(const unsigned char *msg) {
    while (*msg) {
        switch (*msg) {
        case '\b':
            if (text_mode_state.col > 0)
                text_mode_state.buffer[text_mode_state.row][--text_mode_state.col] = 0x0F20;
            break;
        case '\r':
            text_mode_state.col = 0;
            break;
        case '\n':
            text_mode_state.col = 0;
            text_mode_state.row++;
            break;
        default:
            if (text_mode_state.col >= VGA_WIDTH) {
                text_mode_state.col = 0;
                text_mode_state.row++;
            }
            if (text_mode_state.row >= VGA_HEIGHT)
                scroll();
            text_mode_state.buffer[text_mode_state.row][text_mode_state.col++] =
                (uint16_t)(*msg | (0x0F << 8));
            break;
        }
        if (text_mode_state.row >= VGA_HEIGHT)
            scroll();
        msg++;
    }
    flush();
}

void text_mode_clear(void) {
    for (int i = 0; i < VGA_HEIGHT; i++)
        for (int j = 0; j < VGA_WIDTH; j++) {
            /* was i * VGA_HEIGHT (wrong), must be i * VGA_WIDTH */
            VIDEO_RAM[i * VGA_WIDTH + j] = 0x0F20;
            text_mode_state.buffer[i][j]  = 0x0F20;
        }
}

void text_mode_init(void) {
    text_mode_state.col = 0;
    text_mode_state.row = 0;
    text_mode_clear();
}
