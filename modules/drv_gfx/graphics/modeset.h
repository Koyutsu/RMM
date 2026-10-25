#pragma once
/*
 *  modeset - display mode table + runtime resolution changes
 *
 *  One place owns "which graphics mode is the device in and how do I
 *  change it".  A standard mode table (32 bpp) is offered to both the
 *  boot path (program a known-good mode instead of inheriting whatever
 *  the firmware picked) and userland (FBIOPUT_VSCREENINFO on /dev/fb0,
 *  the DRM connector mode list).
 *
 *  Backends that can reprogram their scanout:
 *      BGA/dispi (QEMU std-vga, Bochs), full mode set support
 *      VESA-only / VMware, mode table reported,
 *                                         changes rejected with -ENOSYS
 */
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define MODESET_MAX_MODES 12

typedef struct {
    uint32_t w, h;
} modeset_mode_t;

/* Number of entries offered for this machine (VRAM-clamped for BGA). */
int      modeset_mode_count(void);

/* Table access: entry i, valid for i < modeset_mode_count(). */
modeset_mode_t modeset_mode(int i);

/* The boot default - a normal 16:9 mode every target can drive. */
modeset_mode_t modeset_default(void);

/* True when the machine has a scanout that can be reprogrammed. */
bool     modeset_can_change(void);

/* Apply a mode.  Returns 0 and repaints the console on success,
 * -EINVAL for a mode outside the table/VRAM, -ENOSYS when no
 * programmable scanout exists.  bpp must be 32. */
int      modeset_apply(uint32_t w, uint32_t h, uint32_t bpp);

/* Find a table entry (0 = exact match, -1 = not in table). */
int      modeset_find(uint32_t w, uint32_t h);
