/*
 *  virtio-gpu 2D driver (virtio 1.0 / modern PCI)
 *
 *  QEMU's fastest emulated display: instead of racing the host on video
 *  memory, the guest owns a RAM-backed canvas and hands it to the host via
 *  virtqueue DMA (TRANSFER_TO_HOST_2D + RESOURCE_FLUSH).
 *
 *  Implemented subset (2D fixed function):
 *    GET_DISPLAY_INFO, RESOURCE_CREATE_2D, ATTACH_BACKING, SET_SCANOUT,
 *    TRANSFER_TO_HOST_2D, RESOURCE_FLUSH.
 *
 *  PCI: vendor 0x1AF4, device 0x1050 (transitional/modern virtio-gpu).
 *  Uses virtio "modern" PCI capabilities (common/isr/notify/device cfg).
 *  No interrupts: the control queue is polled via the used ring, which is
 *  perfectly fine for a boot/serial console workload.
 */
#include "drivers/graphics/virtio_gpu.h"
#include "drivers/core/driver.h"
#include "drivers/graphics/fbdev.h"
#include "console/print.h"
#include "drivers/pci/pci.h"
#include "drivers/pci/io.h"
#include "drivers/memory/vmm.h"
#include "console/print.h"

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* PCI IDs */
#define VIRTIO_PCI_VENDOR   0x1AF4
#define VIRTIO_GPU_DEV_MOD  0x1050      /* 0x1040 + 0x10 */

/* virtio modern PCI capability types */
#define VPCI_CAP_COMMON     1
#define VPCI_CAP_NOTIFY     2
#define VPCI_CAP_ISR        3
#define VPCI_CAP_DEVICE     4

/* device status bits */
#define VSTS_ACK            1u
#define VSTS_DRIVER         2u
#define VSTS_DRIVER_OK      4u
#define VSTS_FEATURES_OK    8u
#define VSTS_FAILED         128u

/* feature bit VIRTIO_F_VERSION_1 = 32 */
#define VIRTIO_F_VERSION_1  32

/* common config layout (offsets) */
#define CC_DEV_FEAT_SELECT   0x00
#define CC_DEV_FEAT          0x04
#define CC_DRV_FEAT_SELECT   0x0C
#define CC_DRV_FEAT          0x10
#define CC_MSIX_CONFIG       0x18
#define CC_NUM_QUEUES        0x1A
#define CC_STATUS            0x1C
#define CC_CFG_GENERATION    0x1D
#define CC_QUEUE_SELECT      0x1E
#define CC_QUEUE_SIZE        0x20
#define CC_QUEUE_MSIX        0x22
#define CC_QUEUE_ENABLE      0x24
#define CC_QUEUE_NOTIFY_OFF  0x26
#define CC_QUEUE_DESC        0x28
#define CC_QUEUE_AVAIL       0x30
#define CC_QUEUE_USED        0x38

#define NO_VECTOR            0xFFFFu

/* GPU protocol */
/* GPU protocol (command codes per the virtio-gpu 1.0 spec, the
 * file had several swapped values, but was never compiled, the
 * wrong codes would have made QEMU reject every command) */
#define VGPU_CMD_GET_DISPLAY_INFO    0x0100
#define VGPU_CMD_RESOURCE_CREATE_2D  0x0103
#define VGPU_CMD_RESOURCE_DESTROY_2D 0x0104
#define VGPU_CMD_RESOURCE_SET_SCANOUT 0x0105
#define VGPU_CMD_RESOURCE_FLUSH      0x0106
#define VGPU_CMD_TRANSFER_TO_HOST_2D 0x0107
#define VGPU_CMD_RESOURCE_ATTACH_BACKING  0x0109
#define VGPU_CMD_RESOURCE_DETACH_BACKING  0x010a
#define VGPU_RESP_OK_NODATA          0x1100
#define VGPU_RESP_OK_DISPLAY_INFO    0x1101
#define VGPU_RESP_ERR_UNSPEC         0x1200

#define VGPU_FORMAT_B8G8R8X8         1
#define VGPU_MAX_SCANOUTS            16

typedef struct {
    uint32_t type;
    uint32_t flags;
    uint64_t fence_id;
    uint32_t ctx_id;
    uint32_t pad;
} vgpu_hdr_t;               /* 24 bytes */

typedef struct {            /* virtio_gpu_rect */
    uint32_t x, y, width, height;
} vgpu_rect_t;

typedef struct {            /* resource_create_2d body */
    uint32_t resource_id;
    uint32_t width;
    uint32_t height;
    uint32_t format;
} vgpu_create2d_t;

typedef struct {            /* mem_entry */
    uint64_t addr;
    uint32_t length;
    uint32_t pad;
} vgpu_mem_entry_t;

typedef struct {            /* attach_backing body: 2 x mem_entry max here */
    uint32_t resource_id;
    uint32_t nr_entries;
    vgpu_mem_entry_t entries[1];
} vgpu_attach_t;

typedef struct {            /* set_scanout body */
    vgpu_rect_t r;
    uint32_t scanout_id;
    uint32_t resource_id;
} vgpu_scanout_t;

typedef struct {            /* resource_flush / transfer_to_host_2d body */
    vgpu_rect_t r;
    uint32_t resource_id;
    uint32_t pad;
    uint64_t offset;        /* used by transfer_to_host_2d only */
} vgpu_flush_t;

typedef struct {            /* resp_display_info */
    vgpu_hdr_t hdr;
    struct {
        vgpu_rect_t r;
        uint32_t enabled;
        uint32_t flags;
    } pmode[VGPU_MAX_SCANOUTS];
} vgpu_display_info_t;

/* split virtqueue */
typedef struct {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} vq_desc_t;

#define VQ_DESC_F_NEXT   1u
#define VQ_DESC_F_WRITE  2u

typedef struct {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[64];
    uint16_t used_event;
} vq_avail_t;

typedef struct {
    uint32_t idx;
    uint32_t len;
} vq_used_elem_t;

typedef struct {
    uint16_t flags;
    uint16_t idx;
    vq_used_elem_t ring[64];
    uint16_t avail_event;
} vq_used_t;

/* driver state */
typedef struct {
    /* PCI capability windows */
    uint8_t   *common;      uint32_t common_len;
    uint8_t   *isr;         uint32_t isr_len;
    uint8_t   *notify;      uint32_t notify_len;
    uint8_t   *devcfg;      uint32_t devcfg_len;
    uint32_t   notify_mult;

    /* control queue */
    uint16_t   qsize;
    vq_desc_t *desc;
    vq_avail_t *avail;
    vq_used_t *used;
    uint16_t   next_free;
    uint16_t   last_used;

    /* canvas */
    uint64_t   canvas_phys;
    uint8_t   *canvas_virt;
    uint32_t   width, height;
    uint32_t   canvas_bytes;

    fbdev_t    fbdev;
    bool       ready;
} vgpu_state_t;

static vgpu_state_t vgpu;

/* forward decls: vq_submit_ok is used before its definition below */
static bool vq_submit_wait(vgpu_hdr_t *req, uint32_t req_len,
                           void *body, uint32_t body_len,
                           void *resp, uint32_t resp_len);
static bool vgpu_cmd_ok(vgpu_hdr_t *hdr, void *body, uint32_t blen);

/* low-level helpers */
static inline uint32_t rd32(uint8_t *base, uint32_t off) {
    uint32_t v; __asm__ volatile("movl (%1), %0" : "=r"(v) : "r"(base + off) : "memory");
    return v;
}
static inline void wr32(uint8_t *base, uint32_t off, uint32_t v) {
    __asm__ volatile("movl %0, (%1)" :: "r"(v), "r"(base + off) : "memory");
}
static inline uint16_t rd16(uint8_t *base, uint32_t off) {
    uint16_t v; __asm__ volatile("movw (%1), %0" : "=r"(v) : "r"(base + off) : "memory");
    return v;
}
static inline uint8_t rd8(uint8_t *base, uint32_t off) {
    uint8_t v; __asm__ volatile("movb (%1), %0" : "=q"(v) : "r"(base + off) : "memory");
    return v;
}
static inline void wr16(uint8_t *base, uint32_t off, uint16_t v) {
    __asm__ volatile("movw %0, (%1)" :: "r"(v), "r"(base + off) : "memory");
}
static inline void wr8(uint8_t *base, uint32_t off, uint8_t v) {
    __asm__ volatile("movb %0, (%1)" :: "r"(v), "r"(base + off) : "memory");
}
static inline void mfence(void) { __asm__ volatile("mfence" ::: "memory"); }

static void cc_write32(uint32_t off, uint32_t v) { wr32(vgpu.common, off, v); }
static void cc_write16(uint32_t off, uint16_t v) { wr16(vgpu.common, off, v); }
static void cc_write8 (uint32_t off, uint8_t  v) { wr8 (vgpu.common, off, v); }
static uint32_t cc_read32(uint32_t off) { return rd32(vgpu.common, off); }
static uint16_t cc_read16(uint32_t off) { return rd16(vgpu.common, off); }
static uint8_t  cc_read8 (uint32_t off) { return rd8 (vgpu.common, off); }
/* queue addresses are 64-bit fields stored as two 32-bit halves */
static void cc_write64(uint32_t off, uint64_t v) {
    wr32(vgpu.common, off,     (uint32_t)(v & 0xFFFFFFFFu));
    wr32(vgpu.common, off + 4, (uint32_t)(v >> 32));
}

/* PCI capability parsing (modern virtio) */
static bool parse_caps(pci_device_t *dev) {
    /* walk capability list at config offset 0x34 (cap pointer) using the
     * PCI core registry device */
    uint8_t cap_off = (uint8_t)pci_read_cfg(dev->bus, dev->dev, dev->fn, 0x34, 1);
    int guard = 0;
    while (cap_off && guard++ < 24) {
        uint8_t vndr = (uint8_t)pci_read_cfg(dev->bus, dev->dev, dev->fn, cap_off + 0, 1);
        uint8_t next = (uint8_t)pci_read_cfg(dev->bus, dev->dev, dev->fn, cap_off + 1, 1);
        if (vndr == 0x09) {                     /* PCI_CAP_ID_VNDR (virtio) */
            uint8_t  cfg_type = (uint8_t)pci_read_cfg(dev->bus, dev->dev, dev->fn, cap_off + 3, 1);
            uint8_t  bar      = (uint8_t)pci_read_cfg(dev->bus, dev->dev, dev->fn, cap_off + 4, 1);
            uint32_t offset   = pci_read_cfg(dev->bus, dev->dev, dev->fn, cap_off + 8, 4);
            uint32_t length   = pci_read_cfg(dev->bus, dev->dev, dev->fn, cap_off + 12, 4);
            if (bar >= 6 || dev->bars[bar].type == PCI_BAR_NONE ||
                dev->bars[bar].type == PCI_BAR_IO) {
                cap_off = next; continue;
            }
            uint64_t bar_base = dev->bars[bar].base;
            uint64_t map_lo   = bar_base & ~(uint64_t)(PAGE_SIZE - 1);
            uint64_t map_hi   = bar_base + offset + length + PAGE_SIZE;
            if (!vmm_identity_map_range(map_lo, map_hi,
                                        PTE_PRESENT | PTE_WRITABLE)) {
                print_str("[vgpu] BAR map failed\n");
                return false;
            }
            uint8_t *p = (uint8_t *)(uintptr_t)(bar_base + offset);
            switch (cfg_type) {
            case VPCI_CAP_COMMON: vgpu.common = p; vgpu.common_len = length; break;
            case VPCI_CAP_ISR:    vgpu.isr    = p; vgpu.isr_len    = length; break;
            case VPCI_CAP_NOTIFY:
                vgpu.notify = p; vgpu.notify_len = length;
                vgpu.notify_mult = pci_read_cfg(dev->bus, dev->dev, dev->fn,
                                                cap_off + 16, 4);
                break;
            case VPCI_CAP_DEVICE: vgpu.devcfg = p; vgpu.devcfg_len = length; break;
            }
        }
        cap_off = next;
    }
    return vgpu.common && vgpu.isr && vgpu.notify && vgpu.devcfg;
}

/* virtqueue submission */
static int vq_alloc_desc(void) {
    for (int i = 0; i < (int)vgpu.qsize; i++) {
        uint16_t idx = (uint16_t)((vgpu.next_free + i) % vgpu.qsize);
        if (!(vgpu.desc[idx].flags & VQ_DESC_F_NEXT)) return idx;
    }
    return -1;
}

/* Kick a chain and wait for the response (single response descriptor).
 * Returns true only when the device answered with an OK status. */
static bool vq_submit_ok(vgpu_hdr_t *req, uint32_t req_len,
                         void *body, uint32_t body_len,
                         void *resp, uint32_t resp_len) {
    if (!vq_submit_wait(req, req_len, body, body_len, resp, resp_len))
        return false;
    vgpu_hdr_t *h = (vgpu_hdr_t *)resp;
    return (h->type & 0x1100u) == 0x1100u;   /* any OK_* response */
}

static bool vq_submit_wait(vgpu_hdr_t *req, uint32_t req_len,
                           void *body, uint32_t body_len,
                           void *resp, uint32_t resp_len) {
    int d0 = vq_alloc_desc();
    if (d0 < 0) return false;
    uint16_t d1 = (uint16_t)((d0 + 1) % vgpu.qsize);
    uint16_t d2 = (uint16_t)((d0 + 2) % vgpu.qsize);
    bool has_body = (body && body_len);

    vgpu.desc[d0] = (vq_desc_t){ .addr = (uint64_t)(uintptr_t)req,
                                 .len = req_len, .flags = VQ_DESC_F_NEXT,
                                 .next = has_body ? d1 : d2 };
    if (has_body) {
        vgpu.desc[d1] = (vq_desc_t){ .addr = (uint64_t)(uintptr_t)body,
                                     .len = body_len, .flags = VQ_DESC_F_NEXT,
                                     .next = d2 };
    }
    vgpu.desc[d2] = (vq_desc_t){ .addr = (uint64_t)(uintptr_t)resp,
                                 .len = resp_len, .flags = VQ_DESC_F_WRITE,
                                 .next = 0 };

    vgpu.next_free = (uint16_t)((d2 + 1) % vgpu.qsize);
    vgpu.avail->ring[vgpu.avail->idx % vgpu.qsize] = (uint16_t)d0;
    __asm__ volatile("" ::: "memory");
    vgpu.avail->idx++;
    __asm__ volatile("" ::: "memory");

    /* notify: notify_base + queue_notify_off * notify_off_multiplier */
    uint32_t qoff = cc_read16(CC_QUEUE_NOTIFY_OFF);
    volatile uint16_t *kick =
        (volatile uint16_t *)(vgpu.notify + qoff * vgpu.notify_mult);
    *kick = 0;      /* ctrlq = queue 0 */
    __asm__ volatile("" ::: "memory");

    /* poll the used ring (volatile: the device updates it asynchronously) */
    volatile uint16_t *used_idx = (volatile uint16_t *)&vgpu.used->idx;
    uint64_t spin = 0;
    while (*used_idx == vgpu.last_used) {
        if (++spin > 200ull * 1000 * 1000) return false;   /* ~seconds */
        __asm__ volatile("pause");
    }
    vgpu.last_used = *used_idx;
    return true;
}

/* fbdev flush (RAM canvas -> host) */
static bool vgpu_do_flush(vgpu_rect_t r, uint64_t offset) {
    static vgpu_hdr_t  tf_hdr, fl_hdr;
    static vgpu_flush_t tf, fl;
    static vgpu_hdr_t  resp_tf, resp_fl;

    tf_hdr = (vgpu_hdr_t){ .type = VGPU_CMD_TRANSFER_TO_HOST_2D };
    tf = (vgpu_flush_t){ .r = r, .resource_id = 1, .offset = offset };
    if (!vq_submit_ok(&tf_hdr, sizeof tf_hdr, &tf, sizeof tf,
                      &resp_tf, sizeof resp_tf)) return false;

    fl_hdr = (vgpu_hdr_t){ .type = VGPU_CMD_RESOURCE_FLUSH };
    fl = (vgpu_flush_t){ .r = r, .resource_id = 1 };
    if (!vq_submit_ok(&fl_hdr, sizeof fl_hdr, &fl, sizeof fl,
                      &resp_fl, sizeof resp_fl)) return false;
    return true;
}

static int vgpu_flush_rows(fbdev_t *f, uint32_t y0, uint32_t y1) {
    if (!vgpu.ready) return -1;
    if (y1 < y0) { uint32_t t = y0; y0 = y1; y1 = t; }
    if (y1 >= f->height) y1 = f->height - 1;
    if (!vgpu_do_flush((vgpu_rect_t){ .x = 0, .y = y0,
                                      .width = f->width, .height = y1 - y0 + 1 },
                       (uint64_t)y0 * f->pitch))
        return -1;
    return 0;
}

static int vgpu_flush_rect(fbdev_t *f, uint32_t x, uint32_t y,
                           uint32_t w, uint32_t h) {
    if (!vgpu.ready) return -1;
    if (x + w > f->width)  w = f->width  - x;
    if (y + h > f->height) h = f->height - y;
    if (!w || !h) return 0;
    if (!vgpu_do_flush((vgpu_rect_t){ .x = x, .y = y, .width = w, .height = h },
                       (uint64_t)y * f->pitch + (uint64_t)x * 4))
        return -1;
    return 0;
}

/* mode set = recreate canvas resource */
static bool vgpu_setup_canvas(uint32_t w, uint32_t h) {
    static vgpu_hdr_t    ci_hdr, ab_hdr, so_hdr;
    static vgpu_create2d_t ci;
    static vgpu_attach_t   ab;
    static vgpu_scanout_t  so;
    static vgpu_hdr_t      resp;

    ci_hdr = (vgpu_hdr_t){ .type = VGPU_CMD_RESOURCE_CREATE_2D };
    ci = (vgpu_create2d_t){ .resource_id = 1, .width = w, .height = h,
                            .format = VGPU_FORMAT_B8G8R8X8 };
    if (!vq_submit_ok(&ci_hdr, sizeof ci_hdr, &ci, sizeof ci,
                      &resp, sizeof resp)) return false;

    ab_hdr = (vgpu_hdr_t){ .type = VGPU_CMD_RESOURCE_ATTACH_BACKING };
    ab = (vgpu_attach_t){ .resource_id = 1, .nr_entries = 1,
                          .entries = { { .addr = vgpu.canvas_phys,
                                         .length = w * h * 4 } } };
    if (!vq_submit_ok(&ab_hdr, sizeof ab_hdr, &ab, sizeof ab,
                      &resp, sizeof resp)) return false;

    so_hdr = (vgpu_hdr_t){ .type = VGPU_CMD_RESOURCE_SET_SCANOUT };
    so = (vgpu_scanout_t){ .r = { .x = 0, .y = 0, .width = w, .height = h },
                           .scanout_id = 0, .resource_id = 1 };
    if (!vq_submit_ok(&so_hdr, sizeof so_hdr, &so, sizeof so,
                      &resp, sizeof resp)) return false;

    return true;
}

/* probe */
int virtio_gpu_probe(device_t *dev) {
    if (vgpu.ready) return 0;              /* already bound */
    print_str("[vgpu] probing virtio-gpu\n");

    pci_device_t *pdev = pci_find_slot(dev->pci_bus, dev->pci_dev, dev->pci_func);
    if (!pdev) return -1;
    if (!parse_caps(pdev)) {
        print_str("[vgpu] modern PCI capabilities missing\n");
        return -1;
    }

    /* reset + bring up */
    cc_write8(CC_STATUS, 0);
    cc_write8(CC_STATUS, VSTS_ACK | VSTS_DRIVER);

    /* check VERSION_1 */
    cc_write32(CC_DEV_FEAT_SELECT, VIRTIO_F_VERSION_1 / 32);
    uint32_t hi = cc_read32(CC_DEV_FEAT);
    if (!(hi & (1u << (VIRTIO_F_VERSION_1 % 32)))) {
        print_str("[vgpu] device is not virtio 1.0\n");
        return -1;
    }
    cc_write32(CC_DRV_FEAT_SELECT, VIRTIO_F_VERSION_1 / 32);
    cc_write32(CC_DRV_FEAT, 1u << (VIRTIO_F_VERSION_1 % 32));
    cc_write8(CC_STATUS, cc_read8(CC_STATUS) | VSTS_FEATURES_OK);
    if (!(cc_read8(CC_STATUS) & VSTS_FEATURES_OK)) {
        print_str("[vgpu] FEATURES_OK rejected\n");
        return -1;
    }

    /* no MSI-X */
    cc_write16(CC_MSIX_CONFIG, NO_VECTOR);

    /* set up the control queue (queue 0) */
    uint16_t nqueues = cc_read16(CC_NUM_QUEUES);
    if (nqueues < 1) return -1;
    cc_write16(CC_QUEUE_SELECT, 0);
    uint16_t qsize = cc_read16(CC_QUEUE_SIZE);
    if (!qsize) return -1;
    if (qsize > 64) qsize = 64;
    vgpu.qsize = qsize;

    uint64_t dq = pmm_alloc_contig(
        (sizeof(vq_desc_t) * qsize + PAGE_SIZE - 1) / PAGE_SIZE + 1);
    uint64_t aq = pmm_alloc();
    uint64_t uq = pmm_alloc();
    if (!dq || !aq || !uq) return -1;

    vgpu.desc  = (vq_desc_t *)(uintptr_t)dq;
    vgpu.avail = (vq_avail_t *)(uintptr_t)aq;
    vgpu.used  = (vq_used_t *)(uintptr_t)uq;
    for (uint32_t i = 0; i < qsize; i++) vgpu.desc[i] = (vq_desc_t){0};
    vgpu.avail->flags = 0; vgpu.avail->idx = 0;
    vgpu.next_free = 0; vgpu.last_used = 0;

    cc_write16(CC_QUEUE_SIZE, qsize);
    cc_write16(CC_QUEUE_MSIX, NO_VECTOR);
    cc_write64(CC_QUEUE_DESC,  dq);
    cc_write64(CC_QUEUE_AVAIL, aq);
    cc_write64(CC_QUEUE_USED,  uq);
    cc_write16(CC_QUEUE_ENABLE, 1);

    cc_write8(CC_STATUS, cc_read8(CC_STATUS) | VSTS_DRIVER_OK);

    /* display info -> native resolution */
    static vgpu_hdr_t di_hdr;
    static vgpu_display_info_t di;
    di_hdr = (vgpu_hdr_t){ .type = VGPU_CMD_GET_DISPLAY_INFO };
    if (!vq_submit_ok(&di_hdr, sizeof di_hdr, NULL, 0, &di, sizeof di)) {
        print_str("[vgpu] GET_DISPLAY_INFO failed\n");
        return -1;
    }
    uint32_t w = 1024, h = 768;
    if (di.pmode[0].enabled && di.pmode[0].r.width) {
        w = di.pmode[0].r.width;
        h = di.pmode[0].r.height;
    }

    /* canvas: contiguous RAM backing store */
    uint32_t canvas_pages = (w * h * 4 + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t canvas = pmm_alloc_contig(canvas_pages);
    if (!canvas) {
        print_str("[vgpu] no contiguous canvas\n");
        return -1;
    }
    vgpu.canvas_phys  = canvas;
    vgpu.canvas_virt  = (uint8_t *)(uintptr_t)canvas;   /* identity map */
    vgpu.canvas_bytes = canvas_pages * PAGE_SIZE;
    if (!vmm_identity_map_range(canvas, canvas + vgpu.canvas_bytes,
                                PTE_PRESENT | PTE_WRITABLE)) {
        print_str("[vgpu] canvas map failed\n");
        return -1;
    }
    vgpu.width = w; vgpu.height = h;

    if (!vgpu_setup_canvas(w, h)) {
        print_str("[vgpu] canvas setup failed\n");
        return -1;
    }

    vgpu.fbdev = (fbdev_t){
        .name = "virtio-gpu", .fb = vgpu.canvas_virt,
        .width = w, .height = h, .pitch = w * 4, .bpp = 32,
        .priority = 110, .flush_rows = vgpu_flush_rows,
        .flush_rect = vgpu_flush_rect, .priv = &vgpu,
    };
    fbdev_register(&vgpu.fbdev);
    vgpu.ready = true;
    printf("[vgpu] %ux%u canvas @ 0x%x ready (DMA flush)\n",
           w, h, (unsigned)canvas);
    return 0;
}

/* registration */
static driver_match_t virtio_gpu_matches[] = {
    { VIRTIO_PCI_VENDOR, VIRTIO_GPU_DEV_MOD, 0xFF },
    { 0, 0, 0 },
};

driver_t virtio_gpu_drv = {
    .name        = "virtio-gpu",
    .bus         = BUS_PCI,
    .match_table = virtio_gpu_matches,
    .probe       = virtio_gpu_probe,
    .remove      = NULL,
    .priority    = 110,
};

BUILTIN_DRIVER(virtio_gpu_drv);

/*
 *  DRM dumb-buffer backend (consumed by drivers/gpu/drm.c)
 *
 *  Every dumb buffer is its own virtio 2D resource whose backing store
 *  is contiguous guest RAM.  mmap gives the user those guest pages
 *  directly (zero copy on the CPU side); SETCRTC/PAGE_FLIP program the
 *  scanout to the resource and push it with TRANSFER_TO_HOST_2D +
 *  RESOURCE_FLUSH - the same flow Linux's virtio-gpu dumb-buffer path
 *  uses.
 */
#define VGPU_DRM_RES_BASE  2          /* 1 = console canvas            */
#define VGPU_DRM_RES_MAX   62

static uint8_t vgpu_res_used[VGPU_DRM_RES_MAX + 1];
static struct { uint32_t rid; uint64_t phys; uint32_t pages; }
    vgpu_res_map[VGPU_DRM_RES_MAX + 1];

typedef struct { uint32_t resource_id; } vgpu_destroy_t;

bool virtio_gpu_dumb_ready(void) { return vgpu.ready; }

/* run one control-queue command with its own response slot */
static bool vgpu_cmd_ok(vgpu_hdr_t *hdr, void *body, uint32_t blen) {
    static vgpu_hdr_t r;
    return vq_submit_ok(hdr, sizeof *hdr, body, blen, &r, sizeof r);
}

static void vgpu_free_pages(uint64_t pa, uint32_t pages) {
    for (uint32_t i = 0; i < pages; i++) pmm_free(pa + (uint64_t)i * PAGE_SIZE);
}

bool virtio_gpu_dumb_create(uint32_t w, uint32_t h,
                            uint64_t *phys_out, uint32_t *res_out) {
    if (!vgpu.ready) return false;

    int rid = -1;
    for (int i = VGPU_DRM_RES_BASE; i <= VGPU_DRM_RES_MAX; i++)
        if (!vgpu_res_used[i]) { rid = i; break; }
    if (rid < 0) return false;

    uint64_t bytes = (uint64_t)w * h * 4;
    uint32_t pages = (uint32_t)((bytes + PAGE_SIZE - 1) / PAGE_SIZE);
    uint64_t pa = pmm_alloc_contig(pages);
    if (!pa) return false;
    if (!vmm_identity_map_range(pa, pa + (uint64_t)pages * PAGE_SIZE,
                                PTE_PRESENT | PTE_WRITABLE)) {
        vgpu_free_pages(pa, pages);
        return false;
    }

    {
        static vgpu_hdr_t      ci_hdr;
        static vgpu_create2d_t ci;
        ci_hdr = (vgpu_hdr_t){ .type = VGPU_CMD_RESOURCE_CREATE_2D };
        ci = (vgpu_create2d_t){ .resource_id = (uint32_t)rid,
                                .width = w, .height = h,
                                .format = VGPU_FORMAT_B8G8R8X8 };
        if (!vgpu_cmd_ok(&ci_hdr, &ci, sizeof ci)) goto fail;
    }
    {
        static vgpu_hdr_t ab_hdr;
        static uint8_t    ab_buf[sizeof(vgpu_attach_t) + sizeof(vgpu_mem_entry_t)];
        vgpu_attach_t    *ab = (vgpu_attach_t *)ab_buf;
        ab_hdr = (vgpu_hdr_t){ .type = VGPU_CMD_RESOURCE_ATTACH_BACKING };
        ab->resource_id = (uint32_t)rid;
        ab->nr_entries  = 1;
        ab->entries[0]  = (vgpu_mem_entry_t){ .addr = pa,
                                              .length = (uint32_t)bytes };
        if (!vgpu_cmd_ok(&ab_hdr, ab,
                         sizeof(vgpu_attach_t) + sizeof(vgpu_mem_entry_t)))
            goto fail;
    }

    vgpu_res_used[rid] = 1;
    vgpu_res_map[rid].rid   = (uint32_t)rid;
    vgpu_res_map[rid].phys  = pa;
    vgpu_res_map[rid].pages = pages;
    *phys_out = pa;
    *res_out  = (uint32_t)rid;
    return true;

fail:
    vgpu_free_pages(pa, pages);
    return false;
}

bool virtio_gpu_dumb_destroy(uint32_t rid) {
    if (!vgpu.ready || rid < VGPU_DRM_RES_BASE || rid > VGPU_DRM_RES_MAX ||
        !vgpu_res_used[rid]) return false;

    {
        static vgpu_hdr_t    dt_hdr;
        static vgpu_attach_t dt;
        dt_hdr = (vgpu_hdr_t){ .type = VGPU_CMD_RESOURCE_DETACH_BACKING };
        dt.resource_id = rid; dt.nr_entries = 0;
        vgpu_cmd_ok(&dt_hdr, &dt, sizeof dt);
    }
    {
        static vgpu_hdr_t     ds_hdr;
        static vgpu_destroy_t ds;
        ds_hdr = (vgpu_hdr_t){ .type = VGPU_CMD_RESOURCE_DESTROY_2D };
        ds.resource_id = rid;
        vgpu_cmd_ok(&ds_hdr, &ds, sizeof ds);
    }

    vgpu_free_pages(vgpu_res_map[rid].phys, vgpu_res_map[rid].pages);
    vgpu_res_used[rid]      = 0;
    vgpu_res_map[rid].rid   = 0;
    vgpu_res_map[rid].phys  = 0;
    vgpu_res_map[rid].pages = 0;
    return true;
}

bool virtio_gpu_dumb_scanout(uint32_t rid, uint32_t w, uint32_t h) {
    if (!vgpu.ready || rid < VGPU_DRM_RES_BASE || rid > VGPU_DRM_RES_MAX ||
        !vgpu_res_used[rid]) return false;
    static vgpu_hdr_t     so_hdr;
    static vgpu_scanout_t so;
    so_hdr = (vgpu_hdr_t){ .type = VGPU_CMD_RESOURCE_SET_SCANOUT };
    so = (vgpu_scanout_t){ .r = { .x = 0, .y = 0, .width = w, .height = h },
                           .scanout_id = 0, .resource_id = rid };
    return vgpu_cmd_ok(&so_hdr, &so, sizeof so);
}

bool virtio_gpu_dumb_flush(uint32_t rid, uint32_t w, uint32_t h) {
    if (!vgpu.ready || rid < VGPU_DRM_RES_BASE || rid > VGPU_DRM_RES_MAX ||
        !vgpu_res_used[rid]) return false;
    static vgpu_hdr_t   tf_hdr, fl_hdr;
    static vgpu_flush_t tf, fl;
    tf_hdr = (vgpu_hdr_t){ .type = VGPU_CMD_TRANSFER_TO_HOST_2D };
    tf = (vgpu_flush_t){ .r = { .x = 0, .y = 0, .width = w, .height = h },
                         .resource_id = rid, .offset = 0 };
    if (!vgpu_cmd_ok(&tf_hdr, &tf, sizeof tf)) return false;
    fl_hdr = (vgpu_hdr_t){ .type = VGPU_CMD_RESOURCE_FLUSH };
    fl = (vgpu_flush_t){ .r = { .x = 0, .y = 0, .width = w, .height = h },
                         .resource_id = rid };
    return vgpu_cmd_ok(&fl_hdr, &fl, sizeof fl);
}

/* restore the boot console (scanout = resource 1 canvas) */
bool virtio_gpu_console_scanout(void) {
    return virtio_gpu_dumb_scanout(1, vgpu.width, vgpu.height);
}

/* physical backing store of a live dumb resource (0 when unknown) */
uint64_t virtio_gpu_dumb_phys(uint32_t rid) {
    if (rid <= VGPU_DRM_RES_MAX && vgpu_res_map[rid].rid == rid)
        return vgpu_res_map[rid].phys;
    return 0;
}
