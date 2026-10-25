#include "drivers/graphics/fb_font.h"
#include "drivers/graphics/fb_font_terminus.h"
#include <stdarg.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* Console typeface: Terminus 8x16 (SIL OFL 1.1)
 * the hand-drawn ASCII-only table is replaced by the real Terminus
 * bitmap font in classic CP437 ordering (0x20-0x7E ASCII, 0x80-0xFF CP437
 * graphics: box drawing, block elements, shades, accents).  The full table
 * lives in fb_font_terminus.h (generated from ter-u16n.bdf)
 * and is shared with the VGA text-mode
 * character generator (vga_font_upload), one typeface on every backend.
 * License: OFL 1.1 permits embedding without per-use credit, the license
 * text is at https://openfontlicense.org.                              */
#define FONT TERMINUS_8X16
#define FONT_N 256

/* The legacy embedded table is kept as a compiled-out reference for
 * regression bisection, remove entirely once this is confirmed good. */
#if 0
static const uint8_t LEGACY_FONT[95][16] = {
    /* 0x20 space */ {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    /* 0x21 !     */ {0,0,0x18,0x18,0x18,0x18,0x18,0x18,0,0x18,0,0,0,0,0,0},
    /* 0x22 "     */ {0,0,0x66,0x66,0x66,0,0,0,0,0,0,0,0,0,0,0},
    /* 0x23 #     */ {0,0,0x36,0x36,0x7F,0x36,0x36,0x7F,0x36,0x36,0,0,0,0,0,0},
    /* 0x24 $     */ {0,0x18,0x3E,0x6B,0x68,0x3C,0x0E,0x6B,0x6B,0x3E,0x18,0,0,0,0,0},
    /* 0x25 %     */ {0,0,0x63,0x33,0x18,0x0C,0x06,0x33,0x63,0,0,0,0,0,0,0},
    /* 0x26 &     */ {0,0,0x1C,0x36,0x36,0x1C,0x6E,0x33,0x33,0x6E,0,0,0,0,0,0},
    /* 0x27 '     */ {0,0,0x18,0x18,0x18,0,0,0,0,0,0,0,0,0,0,0},
    /* 0x28 (     */ {0,0,0x0C,0x18,0x30,0x30,0x30,0x30,0x18,0x0C,0,0,0,0,0,0},
    /* 0x29 )     */ {0,0,0x30,0x18,0x0C,0x0C,0x0C,0x0C,0x18,0x30,0,0,0,0,0,0},
    /* 0x2A *     */ {0,0,0,0x66,0x3C,0xFF,0x3C,0x66,0,0,0,0,0,0,0,0},
    /* 0x2B +     */ {0,0,0,0x18,0x18,0x7E,0x18,0x18,0,0,0,0,0,0,0,0},
    /* 0x2C ,     */ {0,0,0,0,0,0,0,0,0x18,0x18,0x30,0,0,0,0,0},
    /* 0x2D -     */ {0,0,0,0,0,0x7E,0,0,0,0,0,0,0,0,0,0},
    /* 0x2E .     */ {0,0,0,0,0,0,0,0,0,0x18,0,0,0,0,0,0},
    /* 0x2F /     */ {0,0,0x03,0x06,0x0C,0x18,0x30,0x60,0,0,0,0,0,0,0,0},
    /* 0x30 0     */ {0,0,0x3C,0x66,0x6E,0x76,0x66,0x66,0x3C,0,0,0,0,0,0,0},
    /* 0x31 1     */ {0,0,0x18,0x38,0x18,0x18,0x18,0x18,0x7E,0,0,0,0,0,0,0},
    /* 0x32 2     */ {0,0,0x3C,0x66,0x06,0x0C,0x18,0x30,0x7E,0,0,0,0,0,0,0},
    /* 0x33 3     */ {0,0,0x3C,0x66,0x06,0x1C,0x06,0x66,0x3C,0,0,0,0,0,0,0},
    /* 0x34 4     */ {0,0,0x0C,0x1C,0x3C,0x6C,0x7E,0x0C,0x0C,0,0,0,0,0,0,0},
    /* 0x35 5     */ {0,0,0x7E,0x60,0x7C,0x06,0x06,0x66,0x3C,0,0,0,0,0,0,0},
    /* 0x36 6     */ {0,0,0x1C,0x30,0x60,0x7C,0x66,0x66,0x3C,0,0,0,0,0,0,0},
    /* 0x37 7     */ {0,0,0x7E,0x06,0x0C,0x18,0x30,0x30,0x30,0,0,0,0,0,0,0},
    /* 0x38 8     */ {0,0,0x3C,0x66,0x66,0x3C,0x66,0x66,0x3C,0,0,0,0,0,0,0},
    /* 0x39 9     */ {0,0,0x3C,0x66,0x66,0x3E,0x06,0x0C,0x38,0,0,0,0,0,0,0},
    /* 0x3A :     */ {0,0,0,0,0x18,0,0,0,0x18,0,0,0,0,0,0,0},
    /* 0x3B,     */ {0,0,0,0,0x18,0,0,0,0x18,0x18,0x30,0,0,0,0,0},
    /* 0x3C <     */ {0,0,0x0E,0x1C,0x38,0x70,0x38,0x1C,0x0E,0,0,0,0,0,0,0},
    /* 0x3D =     */ {0,0,0,0,0x7E,0,0,0x7E,0,0,0,0,0,0,0,0},
    /* 0x3E >     */ {0,0,0x70,0x38,0x1C,0x0E,0x1C,0x38,0x70,0,0,0,0,0,0,0},
    /* 0x3F ?     */ {0,0,0x3C,0x66,0x06,0x0C,0x18,0,0x18,0,0,0,0,0,0,0},
    /* 0x40 @     */ {0,0,0x3C,0x66,0x6E,0x6A,0x6E,0x60,0x3C,0,0,0,0,0,0,0},
    /* 0x41 A     */ {0,0,0x18,0x3C,0x66,0x66,0x7E,0x66,0x66,0,0,0,0,0,0,0},
    /* 0x42 B     */ {0,0,0x7C,0x66,0x66,0x7C,0x66,0x66,0x7C,0,0,0,0,0,0,0},
    /* 0x43 C     */ {0,0,0x3C,0x66,0x60,0x60,0x60,0x66,0x3C,0,0,0,0,0,0,0},
    /* 0x44 D     */ {0,0,0x78,0x6C,0x66,0x66,0x66,0x6C,0x78,0,0,0,0,0,0,0},
    /* 0x45 E     */ {0,0,0x7E,0x60,0x60,0x7C,0x60,0x60,0x7E,0,0,0,0,0,0,0},
    /* 0x46 F     */ {0,0,0x7E,0x60,0x60,0x7C,0x60,0x60,0x60,0,0,0,0,0,0,0},
    /* 0x47 G     */ {0,0,0x3C,0x66,0x60,0x6E,0x66,0x66,0x3C,0,0,0,0,0,0,0},
    /* 0x48 H     */ {0,0,0x66,0x66,0x66,0x7E,0x66,0x66,0x66,0,0,0,0,0,0,0},
    /* 0x49 I     */ {0,0,0x3C,0x18,0x18,0x18,0x18,0x18,0x3C,0,0,0,0,0,0,0},
    /* 0x4A J     */ {0,0,0x1E,0x0C,0x0C,0x0C,0x6C,0x6C,0x38,0,0,0,0,0,0,0},
    /* 0x4B K     */ {0,0,0x66,0x6C,0x78,0x70,0x78,0x6C,0x66,0,0,0,0,0,0,0},
    /* 0x4C L     */ {0,0,0x60,0x60,0x60,0x60,0x60,0x60,0x7E,0,0,0,0,0,0,0},
    /* 0x4D M     */ {0,0,0x63,0x77,0x7F,0x6B,0x63,0x63,0x63,0,0,0,0,0,0,0},
    /* 0x4E N     */ {0,0,0x66,0x76,0x7E,0x6E,0x66,0x66,0x66,0,0,0,0,0,0,0},
    /* 0x4F O     */ {0,0,0x3C,0x66,0x66,0x66,0x66,0x66,0x3C,0,0,0,0,0,0,0},
    /* 0x50 P     */ {0,0,0x7C,0x66,0x66,0x7C,0x60,0x60,0x60,0,0,0,0,0,0,0},
    /* 0x51 Q     */ {0,0,0x3C,0x66,0x66,0x66,0x6E,0x3C,0x06,0,0,0,0,0,0,0},
    /* 0x52 R     */ {0,0,0x7C,0x66,0x66,0x7C,0x6C,0x66,0x66,0,0,0,0,0,0,0},
    /* 0x53 S     */ {0,0,0x3C,0x66,0x60,0x3C,0x06,0x66,0x3C,0,0,0,0,0,0,0},
    /* 0x54 T     */ {0,0,0x7E,0x18,0x18,0x18,0x18,0x18,0x18,0,0,0,0,0,0,0},
    /* 0x55 U     */ {0,0,0x66,0x66,0x66,0x66,0x66,0x66,0x3C,0,0,0,0,0,0,0},
    /* 0x56 V     */ {0,0,0x66,0x66,0x66,0x66,0x3C,0x3C,0x18,0,0,0,0,0,0,0},
    /* 0x57 W     */ {0,0,0x63,0x63,0x63,0x6B,0x7F,0x77,0x63,0,0,0,0,0,0,0},
    /* 0x58 X     */ {0,0,0x66,0x66,0x3C,0x18,0x3C,0x66,0x66,0,0,0,0,0,0,0},
    /* 0x59 Y     */ {0,0,0x66,0x66,0x3C,0x18,0x18,0x18,0x18,0,0,0,0,0,0,0},
    /* 0x5A Z     */ {0,0,0x7E,0x06,0x0C,0x18,0x30,0x60,0x7E,0,0,0,0,0,0,0},
    /* 0x5B [     */ {0,0,0x3C,0x30,0x30,0x30,0x30,0x30,0x3C,0,0,0,0,0,0,0},
    /* 0x5C \     */ {0,0,0x60,0x30,0x18,0x0C,0x06,0,0,0,0,0,0,0,0,0},
    /* 0x5D ]     */ {0,0,0x3C,0x0C,0x0C,0x0C,0x0C,0x0C,0x3C,0,0,0,0,0,0,0},
    /* 0x5E ^     */ {0,0x18,0x3C,0x66,0,0,0,0,0,0,0,0,0,0,0,0},
    /* 0x5F _     */ {0,0,0,0,0,0,0,0,0,0,0,0x7E,0,0,0,0},
    /* 0x60 `     */ {0,0,0x18,0x18,0x0C,0,0,0,0,0,0,0,0,0,0,0},
    /* 0x61 a     */ {0,0,0,0,0x3C,0x06,0x3E,0x66,0x3E,0,0,0,0,0,0,0},
    /* 0x62 b     */ {0,0,0x60,0x60,0x7C,0x66,0x66,0x66,0x7C,0,0,0,0,0,0,0},
    /* 0x63 c     */ {0,0,0,0,0x3C,0x66,0x60,0x66,0x3C,0,0,0,0,0,0,0},
    /* 0x64 d     */ {0,0,0x06,0x06,0x3E,0x66,0x66,0x66,0x3E,0,0,0,0,0,0,0},
    /* 0x65 e     */ {0,0,0,0,0x3C,0x66,0x7E,0x60,0x3C,0,0,0,0,0,0,0},
    /* 0x66 f     */ {0,0,0x1C,0x30,0x7C,0x30,0x30,0x30,0x30,0,0,0,0,0,0,0},
    /* 0x67 g     */ {0,0,0,0,0x3E,0x66,0x66,0x3E,0x06,0x3C,0,0,0,0,0,0},
    /* 0x68 h     */ {0,0,0x60,0x60,0x7C,0x66,0x66,0x66,0x66,0,0,0,0,0,0,0},
    /* 0x69 i     */ {0,0,0x18,0,0x38,0x18,0x18,0x18,0x3C,0,0,0,0,0,0,0},
    /* 0x6A j     */ {0,0,0x06,0,0x06,0x06,0x06,0x06,0x66,0x3C,0,0,0,0,0,0},
    /* 0x6B k     */ {0,0,0x60,0x60,0x66,0x6C,0x78,0x6C,0x66,0,0,0,0,0,0,0},
    /* 0x6C l     */ {0,0,0x38,0x18,0x18,0x18,0x18,0x18,0x3C,0,0,0,0,0,0,0},
    /* 0x6D m     */ {0,0,0,0,0x66,0x7F,0x7F,0x6B,0x63,0,0,0,0,0,0,0},
    /* 0x6E n     */ {0,0,0,0,0x7C,0x66,0x66,0x66,0x66,0,0,0,0,0,0,0},
    /* 0x6F o     */ {0,0,0,0,0x3C,0x66,0x66,0x66,0x3C,0,0,0,0,0,0,0},
    /* 0x70 p     */ {0,0,0,0,0x7C,0x66,0x66,0x7C,0x60,0x60,0,0,0,0,0,0},
    /* 0x71 q     */ {0,0,0,0,0x3E,0x66,0x66,0x3E,0x06,0x06,0,0,0,0,0,0},
    /* 0x72 r     */ {0,0,0,0,0x6C,0x76,0x60,0x60,0x60,0,0,0,0,0,0,0},
    /* 0x73 s     */ {0,0,0,0,0x3C,0x60,0x3C,0x06,0x7C,0,0,0,0,0,0,0},
    /* 0x74 t     */ {0,0,0x30,0x30,0x7C,0x30,0x30,0x30,0x1C,0,0,0,0,0,0,0},
    /* 0x75 u     */ {0,0,0,0,0x66,0x66,0x66,0x66,0x3E,0,0,0,0,0,0,0},
    /* 0x76 v     */ {0,0,0,0,0x66,0x66,0x3C,0x3C,0x18,0,0,0,0,0,0,0},
    /* 0x77 w     */ {0,0,0,0,0x63,0x6B,0x7F,0x3E,0x36,0,0,0,0,0,0,0},
    /* 0x78 x     */ {0,0,0,0,0x66,0x3C,0x18,0x3C,0x66,0,0,0,0,0,0,0},
    /* 0x79 y     */ {0,0,0,0,0x66,0x66,0x3E,0x06,0x3C,0,0,0,0,0,0,0},
    /* 0x7A z     */ {0,0,0,0,0x7E,0x0C,0x18,0x30,0x7E,0,0,0,0,0,0,0},
    /* 0x7B {     */ {0,0,0x0E,0x18,0x18,0x70,0x18,0x18,0x0E,0,0,0,0,0,0,0},
    /* 0x7C |     */ {0,0,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0,0,0,0,0,0,0},
    /* 0x7D }     */ {0,0,0x70,0x18,0x18,0x0E,0x18,0x18,0x70,0,0,0,0,0,0,0},
    /* 0x7E ~     */ {0,0,0x76,0xDC,0,0,0,0,0,0,0,0,0,0,0,0},
};
#endif /* 0 - legacy font */

#define FONT_W  8
#define FONT_H 16

/* CP437 box-drawing / block glyphs
 * The VGA text framebuffer and our graphical console share the same
 * "extended glyph" space: the classic CP437 codes.  Userland prints box
 * art in UTF-8 box-drawing characters, term.c decodes the UTF-8 and
 * hands us a CP437 code, which we render from this table.
 *
 * Geometry (must stay consistent, or box art breaks apart):
 *   - double vertical strokes  = 0x24 (columns 2 and 5)  -> matches the double vertical glyph
 *   - single vertical strokes  = 0x18 (columns 3 and 4)  -> matches the single vertical glyph
 *   - double horizontal lines  = 0xFF on rows 6 and 9    -> matches the double horizontal glyph
 *   - single horizontal line   = 0xFF on row 7           -> matches the single horizontal glyph
 *   - corners ("T" joints): the vertical runs from the horizontal lines
 *     to the cell edge (top corners descend to the bottom, bottom
 *     corners rise to the top), the rest of the cell is blank, this is
 *     the authentic CP437 look and keeps corners aligned with the double strokes.
 * The block art is built from full blocks, double corners, double verticals and double horizontals only, with the rules
 * above every corner connects flush to the neighbouring cells.
 *
 * Each glyph: 16 bytes, one byte per row, MSB = leftmost pixel.
 * Missing codes fall back to '?' (via draw_glyph's ASCII path). */
typedef struct { uint8_t code; uint8_t rows[16]; } box_glyph_t;

#define G16(r0,r1,r2,r3,r4,r5,r6,r7,r8,r9,r10,r11,r12,r13,r14,r15) \
    {r0,r1,r2,r3,r4,r5,r6,r7,r8,r9,r10,r11,r12,r13,r14,r15}
#define DOUBLE_VERT_FULL   G16(0x24,0x24,0x24,0x24,0x24,0x24,0x24,0x24,0x24,0x24,0x24,0x24,0x24,0x24,0x24,0x24)
#define SINGLE_VERT_FULL   G16(0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18)
/* double "T" pointing down (double corners): blank top half, lines, descender   */
#define DOUBLE_CORNER_TOP  G16(0x00,0x00,0x00,0x00,0x00,0x00,0xFF,0x24,0x24,0xFF,0x24,0x24,0x24,0x24,0x24,0x24)
/* double "T" pointing up (double corners): ascender, lines, blank bottom half   */
#define DOUBLE_CORNER_BOT  G16(0x24,0x24,0x24,0x24,0x24,0x24,0xFF,0x24,0x24,0xFF,0x00,0x00,0x00,0x00,0x00,0x00)
/* single "T" pointing down (single joints): blank top, rule, vertical below  */
#define SINGLE_CORNER_TOP  G16(0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18)
/* single "T" pointing up (single joints): vertical above, rule, blank bottom */
#define SINGLE_CORNER_BOT  G16(0x18,0x18,0x18,0x18,0x18,0x18,0x18,0xFF,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00)

static const box_glyph_t BOX_GLYPHS[] = {
    /* #  full block            */ {0xDB, G16(0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF)},
    /* ^  upper half block      */ {0xDF, G16(0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00)},
    /* _  lower half block      */ {0xDC, G16(0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF)},
    /* .  light shade           */ {0xB0, G16(0x44,0x11,0x44,0x11,0x44,0x11,0x44,0x11,0x44,0x11,0x44,0x11,0x44,0x11,0x44,0x11)},
    /* :  medium shade          */ {0xB1, G16(0xAA,0x55,0xAA,0x55,0xAA,0x55,0xAA,0x55,0xAA,0x55,0xAA,0x55,0xAA,0x55,0xAA,0x55)},
    /* #  dark shade            */ {0xB2, G16(0xEE,0xBB,0xEE,0xBB,0xEE,0xBB,0xEE,0xBB,0xEE,0xBB,0xEE,0xBB,0xEE,0xBB,0xEE,0xBB)},
    /* |  single vertical       */ {0xB3, SINGLE_VERT_FULL},
    /* +  vertical + rule       */ {0xB4, SINGLE_VERT_FULL},
    /* +  double cross T        */ {0xB9, {0x24,0x24,0x24,0x24,0x24,0x24,0xFF,0x24,0x24,0xFF,0x24,0x24,0x24,0x24,0x24,0x24}},
    /* |  double vertical       */ {0xBA, DOUBLE_VERT_FULL},
    /* +  double corner TR      */ {0xBB, DOUBLE_CORNER_TOP},
    /* +  double corner BR      */ {0xBC, DOUBLE_CORNER_BOT},
    /* +  single corner TR      */ {0xBF, SINGLE_CORNER_TOP},
    /* +  single corner BL      */ {0xC0, SINGLE_CORNER_BOT},
    /* +  single rule T-up      */ {0xC1, SINGLE_CORNER_BOT},
    /* +  single rule T-down    */ {0xC2, SINGLE_CORNER_TOP},
    /* +  vertical + rule       */ {0xC3, SINGLE_VERT_FULL},
    /* -  single horizontal     */ {0xC4, G16(0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00)},
    /* +  single corner TL      */ {0xDA, SINGLE_CORNER_TOP},
    /* +  double corner TL      */ {0xC9, DOUBLE_CORNER_TOP},
    /* +  double corner BL      */ {0xC8, DOUBLE_CORNER_BOT},
    /* +  single corner BR      */ {0xD9, SINGLE_CORNER_BOT},
    /* =  double horizontal     */ {0xCD, G16(0x00,0x00,0x00,0x00,0x00,0x00,0xFF,0x00,0x00,0xFF,0x00,0x00,0x00,0x00,0x00,0x00)},
    /* +  double cross T        */ {0xCC, {0x24,0x24,0x24,0x24,0x24,0x24,0xFF,0x24,0x24,0xFF,0x24,0x24,0x24,0x24,0x24,0x24}},
    /* +  double cross          */ {0xCE, {0x24,0x24,0x24,0x24,0x24,0x24,0xFF,0x24,0x24,0xFF,0x24,0x24,0x24,0x24,0x24,0x24}},
    /* +  single cross          */ {0xC5, SINGLE_VERT_FULL},
};

/* Unicode (U+2500..U+259F block/box) -> CP437 code.  0 = no glyph. */
uint8_t fb_cp437_for_unicode(uint32_t cp) {
    switch (cp) {
    case 0x2500: return 0xC4;  /* - */
    case 0x2502: return 0xB3;  /* | */
    case 0x250C: return 0xDA;  /* + */
    case 0x2510: return 0xBF;  /* + */
    case 0x2514: return 0xC0;  /* + */
    case 0x2518: return 0xD9;  /* + */
    case 0x251C: return 0xC3;  /* + */
    case 0x2524: return 0xB4;  /* + */
    case 0x252C: return 0xC2;  /* + */
    case 0x2534: return 0xC1;  /* + */
    case 0x253C: return 0xC5;  /* + */
    case 0x2550: return 0xCD;  /* = */
    case 0x2551: return 0xBA;  /* | */
    case 0x2554: return 0xC9;  /* + */
    case 0x2557: return 0xBB;  /* + */
    case 0x255A: return 0xC8;  /* + */
    case 0x255D: return 0xBC;  /* + */
    case 0x2560: return 0xCC;  /* + */
    case 0x2561: return 0xB9;  /* + */
    case 0x256C: return 0xCE;  /* + */
    case 0x2580: return 0xDF;  /* ^ */
    case 0x2584: return 0xDC;  /* _ */
    case 0x2588: return 0xDB;  /* # */
    case 0x2591: return 0xB0;  /* . */
    case 0x2592: return 0xB1;  /* : */
    case 0x2593: return 0xB2;  /* # */
    default:     return 0;
    }
}

/* Driver state */
static uint8_t  *fb_base  = NULL;
static uint32_t  fb_w     = 0;
static uint32_t  fb_h     = 0;
static uint32_t  fb_pitch = 0;
static uint8_t   fb_bpp   = 0;
static bool      fb_ready = false;

static uint32_t  fg_color = 0xFFFFFF;
static uint32_t  bg_color = 0x1E1E2E;

static int cur_col = 0;
static int cur_row = 0;

/* Init */
void fb_font_init(uint8_t *fb, uint32_t width, uint32_t height,
                  uint32_t pitch, uint8_t bpp) {
    fb_base  = fb;
    fb_w     = width;
    fb_h     = height;
    fb_pitch = pitch;
    fb_bpp   = bpp;
    fb_ready = (fb != NULL && bpp == 32);
    cur_col  = 0;
    cur_row  = 0;
}

bool fb_font_ready(void)   { return fb_ready; }
int  fb_font_cols(void)    { return fb_ready ? (int)(fb_w / FONT_W) : 0; }
int  fb_font_rows(void)    { return fb_ready ? (int)(fb_h / FONT_H) : 0; }

void fb_font_set_color(uint32_t fg, uint32_t bg) { fg_color = fg; bg_color = bg; }
void fb_font_set_cursor(int c, int r) { cur_col = c; cur_row = r; }
void fb_font_get_cursor(int *c, int *r) { if(c) *c = cur_col; if(r) *r = cur_row; }

/* Pixel write */
static inline void put_pixel(uint32_t x, uint32_t y, uint32_t color) {
    if (x >= fb_w || y >= fb_h) return;
    uint32_t *p = (uint32_t *)(fb_base + y * fb_pitch + x * 4);
    *p = color;
}

/* Clear */
void fb_font_clear(void) {
    if (!fb_ready) return;
    for (uint32_t y = 0; y < fb_h; y++)
        for (uint32_t x = 0; x < fb_w; x++)
            put_pixel(x, y, bg_color);
    cur_col = 0;
    cur_row = 0;
}

/* Draw one glyph at pixel (px, py) */
static void draw_glyph(int px, int py, char c, uint32_t fg, uint32_t bg) {
    /* Terminus: the table is indexed by the raw byte - ASCII 0x20-
     * 0x7E directly, high bytes hit their CP437 slots (box drawing etc.).
     * Control codes fall back to the blank slot like a real PC font. */
    uint8_t idx = (unsigned char)c;
    if (idx < 0x20 || idx == 0x7F) idx = 0x20;
    const uint8_t *glyph = TERMINUS_8X16[idx];
    for (int row = 0; row < FONT_H; row++) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < FONT_W; col++) {
            uint32_t color = (bits & (0x80 >> col)) ? fg : bg;
            put_pixel((uint32_t)(px + col), (uint32_t)(py + row), color);
        }
    }
}

/* Scroll up one line */
static void scroll_up(void) {
    uint32_t line_bytes = (uint32_t)FONT_H * fb_pitch;
    uint32_t copy_bytes = (fb_h - (uint32_t)FONT_H) * fb_pitch;

    /* Move all pixel rows up by FONT_H rows */
    uint8_t *dst = fb_base;
    uint8_t *src = fb_base + line_bytes;
    for (uint32_t i = 0; i < copy_bytes; i++)
        dst[i] = src[i];

    /* Clear the last text row */
    uint8_t *last = fb_base + (fb_h - (uint32_t)FONT_H) * fb_pitch;
    for (uint32_t i = 0; i < (uint32_t)FONT_H * fb_pitch; i += 4) {
        last[i+0] = (uint8_t)(bg_color & 0xFF);
        last[i+1] = (uint8_t)((bg_color >> 8) & 0xFF);
        last[i+2] = (uint8_t)((bg_color >> 16) & 0xFF);
        last[i+3] = 0;
    }
}

/* putchar */
void fb_font_putchar(char c) {
    if (!fb_ready) return;
    int cols = fb_font_cols();
    int rows = fb_font_rows();

    if (c == '\n') {
        cur_col = 0;
        cur_row++;
    } else if (c == '\r') {
        cur_col = 0;
    } else if (c == '\b') {
        if (cur_col > 0) {
            cur_col--;
            draw_glyph(cur_col * FONT_W, cur_row * FONT_H, ' ', fg_color, bg_color);
        }
    } else if (c == '\t') {
        cur_col = (cur_col + 8) & ~7;
        if (cur_col >= cols) { cur_col = 0; cur_row++; }
    } else {
        draw_glyph(cur_col * FONT_W, cur_row * FONT_H, c, fg_color, bg_color);
        cur_col++;
        if (cur_col >= cols) { cur_col = 0; cur_row++; }
    }

    while (cur_row >= rows) {
        scroll_up();
        cur_row = rows - 1;
    }
}

/* puts */
void fb_font_puts(const char *s) {
    while (*s) fb_font_putchar(*s++);
}

/* CP437 extended glyphs */
/* Render one CP437 box/block glyph (0xB0-0xDF range) at the cursor and
 * advance exactly like fb_font_putchar.
 * ASCII codes (< 0x80) are forwarded to fb_font_putchar, the art/logo
 * loops feed every byte through here, so spaces and other printable
 * characters must render as themselves (they used to fall through to the
 * '?' fallback, littering the output with question marks). */
void fb_font_putglyph(uint8_t cp437_code) {
    if (!fb_ready) return;
    if (cp437_code < 0x80) {          /* ASCII: normal font, not box art */
        fb_font_putchar((char)cp437_code);
        return;
    }

    int cols = fb_font_cols();
    int rows = fb_font_rows();

    /* the real Terminus glyph is authoritative (authentic CP437
     * strokes), the hand-drawn table only covers a subset and stays as a
     * fallback for codes the BDF conversion could ever miss. */
    const uint8_t *glyph = TERMINUS_8X16[cp437_code];
    if (!glyph) {
        for (unsigned i = 0; i < sizeof(BOX_GLYPHS)/sizeof(BOX_GLYPHS[0]); i++) {
            if (BOX_GLYPHS[i].code == cp437_code) { glyph = BOX_GLYPHS[i].rows; break; }
        }
    }

    int px = cur_col * FONT_W, py = cur_row * FONT_H;
    if (glyph) {
        for (int row = 0; row < FONT_H; row++)
            for (int col = 0; col < FONT_W; col++)
                put_pixel((uint32_t)(px+col), (uint32_t)(py+row),
                          (glyph[row] & (0x80 >> col)) ? fg_color : bg_color);
    } else {
        draw_glyph(px, py, '?', fg_color, bg_color);
    }
    cur_col++;
    if (cur_col >= cols) { cur_col = 0; cur_row++; }
    while (cur_row >= rows) { scroll_up(); cur_row = rows - 1; }
}

/* Render a decoded Unicode codepoint: box/block chars map to CP437
 * glyphs, anything >= 0x80 without a glyph falls back to '?'.  ASCII is
 * handled by the caller (fb_font_putchar) for efficiency. */
void fb_font_putcodepoint(uint32_t cp) {
    uint8_t g = fb_cp437_for_unicode(cp);
    if (g) fb_font_putglyph(g);
    else   fb_font_putchar('?');
}

/* Shared glyph table access
 * The VGA text-mode driver programs the same 256x16 bytes into the plane-2
 * character generator so real text mode shows Terminus too. */
const uint8_t *fb_font_console_glyphs(void) {
    return (const uint8_t *)TERMINUS_8X16;
}

/* Erase from the cursor to the end of the current line with the current
 * background colour (ANSI CSI K). */
void fb_font_erase_to_eol(void) {
    if (!fb_ready) return;
    int cols = fb_font_cols();
    for (int x = cur_col; x < cols; x++) {
        for (int row = 0; row < FONT_H; row++)
            for (int col = 0; col < FONT_W; col++)
                put_pixel((uint32_t)(x*FONT_W + col),
                          (uint32_t)(cur_row*FONT_H + row), bg_color);
    }
}

/* UTF-8 aware puts: ASCII goes through putchar, box/block codepoints are
 * decoded and rendered as CP437 glyphs.  This is how
 * kernel-side box art renders correctly in graphics mode. */
void fb_font_utf8_puts(const char *s) {
    if (!fb_ready) return;
    uint32_t cp = 0; int need = 0;
    for (; *s; s++) {
        uint8_t b = (uint8_t)*s;
        if (need > 0) {
            if ((b & 0xC0) == 0x80) {
                cp = (cp << 6) | (b & 0x3F);
                if (--need == 0) fb_font_putcodepoint(cp);
            } else { fb_font_putchar('?'); need = 0; }
            continue;
        }
        if (b < 0x80) { fb_font_putchar((char)b); continue; }
        if ((b & 0xE0) == 0xC0) { cp = b & 0x1F; need = 1; continue; }
        if ((b & 0xF0) == 0xE0) { cp = b & 0x0F; need = 2; continue; }
        if ((b & 0xF8) == 0xF0) { cp = b & 0x07; need = 3; continue; }
        fb_font_putchar('?');
    }
}

/* printf */
static void fb_itoa(char *buf, unsigned long val, int base) {
    int i = 0;
    if (val == 0) { buf[0] = '0'; buf[1] = '\0'; return; }
    while (val) {
        int d = (int)(val % (unsigned)base);
        buf[i++] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
        val /= (unsigned)base;
    }
    buf[i] = '\0';
    /* reverse */
    for (int a = 0, b = i-1; a < b; a++, b--) {
        char t = buf[a]; buf[a] = buf[b]; buf[b] = t;
    }
}

void fb_font_printf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char buf[32];
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') { fb_font_putchar(*p); continue; }
        p++;
        switch (*p) {
        case 'd': { int v = va_arg(ap, int);
                    if (v < 0) { fb_font_putchar('-'); v = -v; }
                    fb_itoa(buf, (unsigned long)v, 10); fb_font_puts(buf); break; }
        case 'u': fb_itoa(buf, va_arg(ap, unsigned), 10); fb_font_puts(buf); break;
        case 'x': fb_itoa(buf, va_arg(ap, unsigned), 16); fb_font_puts(buf); break;
        case 's': fb_font_puts(va_arg(ap, char *)); break;
        case 'c': fb_font_putchar((char)va_arg(ap, int)); break;
        case '%': fb_font_putchar('%'); break;
        default:  fb_font_putchar('%'); fb_font_putchar(*p); break;
        }
    }
    va_end(ap);
}
