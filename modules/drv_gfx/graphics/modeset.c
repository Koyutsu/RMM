#include "drivers/graphics/modeset.h"
#include "drivers/graphics/bga.h"
#include "drivers/graphics/fbdev.h"
#include "drivers/graphics/fb_font.h"
#include "console/print.h"
#include "exec/syscall_table.h"      /* errno values */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/*
 *  modeset - standard 32 bpp mode table + apply path
 *
 *  The table holds the classic VESA-family modes plus the common
 *  widescreen ones.  On a BGA/dispi scanout the list is clamped to what
 *  fits in VRAM (the DRM page-flip slots share that budget).  Applying
 *  a mode reprograms the device, updates the primary fbdev geometry and
 *  re-initialises the text console on the new surface.
 */

/* pitch for a mode, 4-byte aligned, 32 bpp */
static uint32_t modeset_pitch(uint32_t w) {
    return (w * 4 + 3) & ~3u;
}

static const modeset_mode_t table[] = {
    {  640,  480 },
    {  800,  600 },
    { 1024,  768 },
    { 1280,  720 },   /* boot default - normal 16:9 VM window size */
    { 1280,  800 },
    { 1280, 1024 },
    { 1440,  900 },
    { 1600,  900 },
    { 1680, 1050 },
    { 1920, 1080 },
    { 1920, 1200 },
    { 2048, 1152 },
};
#define TABLE_N ((int)(sizeof(table) / sizeof(table[0])))

/* VRAM budget: keep room for the console scanout + a flip scratch slot
 * (3x current mode) so the DRM page-flip path stays usable. */
static int modeset_vram_clamp(void) {
    if (!bga_available()) return TABLE_N;
    uint64_t vram = bga_vram_size();
    int n = 0;
    for (int i = 0; i < TABLE_N; i++) {
        uint64_t need = (uint64_t)modeset_pitch(table[i].w) * table[i].h * 3;
        if (need <= vram) n++;
    }
    return n ? n : 3;        /* smallest three always offered */
}

int modeset_mode_count(void) { return modeset_vram_clamp(); }

modeset_mode_t modeset_mode(int i) {
    if (i < 0) i = 0;
    if (i >= TABLE_N) i = TABLE_N - 1;
    return table[i];
}

modeset_mode_t modeset_default(void) {
    return (modeset_mode_t){ 1280, 720 };
}

bool modeset_can_change(void) {
    return bga_available();
}

int modeset_find(uint32_t w, uint32_t h) {
    int n = modeset_vram_clamp();
    for (int i = 0; i < n; i++)
        if (table[i].w == w && table[i].h == h) return i;
    return -1;
}

/* Notify the DRM layer that the BGA scanout geometry changed, the
 * VRAM slot plan (slot size, console mapping) is rebuilt there. */
extern void drm_bga_replan(void);

int modeset_apply(uint32_t w, uint32_t h, uint32_t bpp) {
    if (bpp != 32) return -(int)EINVAL;
    if (modeset_find(w, h) < 0) return -(int)EINVAL;
    if (!bga_available()) return -(int)ENOSYS;

    if (!bga_set_resolution((uint16_t)w, (uint16_t)h, 32))
        return -(int)EINVAL;
    bga_map_vram_full();     /* flip slots live beyond the visible fb */

    /* keep the fbdev primary + console in step with the new surface */
    fbdev_t *f = fbdev_primary();
    if (f) {
        f->fb    = bga_fb_addr();
        f->width = bga_width();
        f->height= bga_height();
        f->pitch = bga_pitch();
        f->bpp   = 32;
    }
    fb_font_init(bga_fb_addr(), bga_width(), bga_height(), bga_pitch(), 32);
    fb_font_set_color(0xFFFFFFFFu, 0xFF000000u);
    fb_font_clear();

    /* DRM BGA backend: rebuild the VRAM slot plan for the new pitch */
    drm_bga_replan();

    print_str("[modeset] ");
    printf("%ux%u@32 applied\n", w, h);
    return 0;
}
