#pragma once
#include <stdint.h>
#include <stdbool.h>

/*
 * virtio-gpu 2D driver, probe entry point (bound via the driver core)
 * plus the DRM dumb-buffer backend API used by drivers/gpu/drm.c.
 */

struct device;                     /* device_t in drivers/core/driver.h */
int virtio_gpu_probe(struct device *dev);

/* DRM dumb-buffer backend (all no-op/false when not probed) */
bool     virtio_gpu_dumb_ready(void);
bool     virtio_gpu_dumb_create(uint32_t w, uint32_t h,
                                uint64_t *phys_out, uint32_t *res_out);
bool     virtio_gpu_dumb_destroy(uint32_t rid);
bool     virtio_gpu_dumb_scanout(uint32_t rid, uint32_t w, uint32_t h);
bool     virtio_gpu_dumb_flush(uint32_t rid, uint32_t w, uint32_t h);
bool     virtio_gpu_console_scanout(void);   /* scanout = console canvas */
uint64_t virtio_gpu_dumb_phys(uint32_t rid); /* backing store, 0 = unknown */
