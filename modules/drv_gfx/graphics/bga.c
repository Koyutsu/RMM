#include "drivers/graphics/bga.h"
#include "drivers/memory/vmm.h"
#include "drivers/pci/io.h"
#include "drivers/pci/pci.h"
#include "drivers/core/driver.h"
#include "console/print.h"

#include <stdint.h>
#include <stdbool.h>

//  VBE Dispi interface (Bochs / QEMU)
//  Index port: 0x01CE   Data port: 0x01CF
#define VBE_DISPI_IOPORT_INDEX  0x01CE
#define VBE_DISPI_IOPORT_DATA   0x01CF

#define VBE_DISPI_INDEX_ID          0
#define VBE_DISPI_INDEX_XRES        1
#define VBE_DISPI_INDEX_YRES        2
#define VBE_DISPI_INDEX_BPP         3
#define VBE_DISPI_INDEX_ENABLE      4
#define VBE_DISPI_INDEX_BANK        5
#define VBE_DISPI_INDEX_VIRT_WIDTH  6
#define VBE_DISPI_INDEX_VIRT_HEIGHT 7
#define VBE_DISPI_INDEX_X_OFFSET    8
#define VBE_DISPI_INDEX_Y_OFFSET    9

#define VBE_DISPI_DISABLED          0x00
#define VBE_DISPI_ENABLED           0x01
#define VBE_DISPI_LFB_ENABLED       0x40   // linear framebuffer (no banking)
#define VBE_DISPI_NOCLEARMEM        0x80

// Known valid BGA version IDs (0xB0C0 ... 0xB0C5)
#define VBE_DISPI_ID_MIN            0xB0C0
#define VBE_DISPI_ID_MAX            0xB0C5


// PCI IDs for the Bochs / QEMU BGA device
#define BGA_VENDOR_ID   0x1234
#define BGA_DEVICE_ID   0x1111

// Physical base address of the linear framebuffer.
// On QEMU/Bochs the LFB starts at 0xE0000000 unless relocated by PCI.
#define BGA_DEFAULT_LFB_PHYS  0xE0000000UL

//  Driver state
static uint32_t  bga_fb_phys  = BGA_DEFAULT_LFB_PHYS;
static uint32_t  bga_vram_sz  = 0;      /* BAR0 size (0=unknown)  */
static uint32_t *bga_fb_virt  = NULL;
static uint32_t  bga_w        = 0;
static uint32_t  bga_h        = 0;
static uint32_t  bga_bpp      = 0;
static uint32_t  bga_pitch_bytes  = 0;    // bytes per line
static bool      bga_ready    = false;

//  Low-level VBE register helpers
static void vbe_write(uint16_t index, uint16_t value) {
    outportw(VBE_DISPI_IOPORT_INDEX, index);
    outportw(VBE_DISPI_IOPORT_DATA,  value);
}

static uint16_t vbe_read(uint16_t index) {
    outportw(VBE_DISPI_IOPORT_INDEX, index);
    return inportw(VBE_DISPI_IOPORT_DATA);
}

/*
 *  PCI lookup via the PCI core: BAR0 = linear framebuffer (MEM)
 *  also records the BAR size, that is the VRAM size used by the
 *  DRM backend to plan page-flip slots.
 */
static uint32_t bga_find_fb_bar(void) {
    pci_device_t *d = pci_find_device(BGA_VENDOR_ID, BGA_DEVICE_ID);
    if (!d) return 0;
    for (int i = 0; i < 6; i++) {
        if ((d->bars[i].type == PCI_BAR_MEM32 || d->bars[i].type == PCI_BAR_MEM64)
            && d->bars[i].size) {
            bga_vram_sz = (uint32_t)d->bars[i].size;
            return (uint32_t)d->bars[i].base;
        }
    }
    return 0;
}

//  Map the framebuffer
static bool bga_map_fb(uint32_t phys, uint32_t size) {
    uint64_t ps = (uint64_t)phys & ~(uint64_t)(PAGE_SIZE - 1);
    uint64_t pe = (uint64_t)phys + size;
    if (!vmm_identity_map_range(ps, pe, PTE_PRESENT | PTE_WRITABLE)) {
        print_str("[BGA] ERROR: failed to map framebuffer\n");
        return false;
    }
    bga_fb_virt = (uint32_t *)(uintptr_t)phys;
    return true;
}

//  Public API
bool bga_init(uint16_t width, uint16_t height, uint16_t bpp) {
    // 1. Probe for BGA via VBE ID register
    uint16_t id = vbe_read(VBE_DISPI_INDEX_ID);
    if (id < VBE_DISPI_ID_MIN || id > VBE_DISPI_ID_MAX) {
        printf("[BGA] VBE ID 0x%x not recognised - device absent\n", id);
        return false;
    }
    printf("[BGA] Found BGA device, VBE ID: 0x%x\n", id);

    // 2. Try to find the LFB physical address via the PCI BAR registry
    uint32_t pci_bar0 = bga_find_fb_bar();
    if (pci_bar0) {
        bga_fb_phys = pci_bar0;
        printf("[BGA] PCI BAR0 framebuffer: 0x%x\n", bga_fb_phys);
    } else {
        printf("[BGA] PCI BAR0 not found, using default LFB: 0x%x\n",
               bga_fb_phys);
    }

    return bga_set_resolution(width, height, bpp);
}

bool bga_set_resolution(uint16_t width, uint16_t height, uint16_t bpp) {
    // Disable first
    vbe_write(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_DISABLED);

    // Program dimensions
    vbe_write(VBE_DISPI_INDEX_XRES, width);
    vbe_write(VBE_DISPI_INDEX_YRES, height);
    vbe_write(VBE_DISPI_INDEX_BPP,  bpp);

    // Virtual framebuffer == physical size (no double-buffering for now)
    vbe_write(VBE_DISPI_INDEX_VIRT_WIDTH,  width);
    vbe_write(VBE_DISPI_INDEX_VIRT_HEIGHT, height);
    vbe_write(VBE_DISPI_INDEX_X_OFFSET, 0);
    vbe_write(VBE_DISPI_INDEX_Y_OFFSET, 0);

    // Enable with LFB mode, keep VRAM contents (caller repaints) -
    // a runtime mode change must not wipe the DRM flip slots.
    vbe_write(VBE_DISPI_INDEX_ENABLE,
              VBE_DISPI_ENABLED | VBE_DISPI_LFB_ENABLED |
              VBE_DISPI_NOCLEARMEM);

    bga_w     = width;
    bga_h     = height;
    bga_bpp   = bpp;
    bga_pitch_bytes = ((uint32_t)width * (bpp / 8) + 3) & ~3u; // 4-byte aligned

    // Calculate required FB size and (re)map it
    uint32_t fb_size = bga_pitch_bytes * height;
    if (!bga_map_fb(bga_fb_phys, fb_size)) {
        bga_ready = false;
        return false;
    }

    bga_ready = true;
    printf("[BGA] %dx%d @ %d bpp ready, pitch=%d, fb_virt=0x%p\n",
           bga_w, bga_h, bga_bpp, bga_pitch_bytes, (void *)(uintptr_t)bga_fb_virt);
    return true;
}

void bga_clear(uint32_t color) {
    if (!bga_ready) return;
    uint32_t pprow = bga_pitch_bytes / 4;
    for (uint32_t y = 0; y < bga_h; y++)
        for (uint32_t x = 0; x < bga_w; x++)
            bga_fb_virt[y * pprow + x] = color;
}

void bga_draw_pixel(int x, int y, uint32_t color) {
    if (!bga_ready) return;
    if (x < 0 || y < 0) return;
    if ((uint32_t)x >= bga_w || (uint32_t)y >= bga_h) return;
    bga_fb_virt[(uint32_t)y * (bga_pitch_bytes / 4) + (uint32_t)x] = color;
}

void bga_fill_rect(int x, int y, int w, int h, uint32_t color) {
    if (!bga_ready) return;
    for (int row = y; row < y + h; row++)
        for (int col = x; col < x + w; col++)
            bga_draw_pixel(col, row, color);
}

uint32_t bga_width(void)    { return bga_w; }
uint32_t bga_height(void)   { return bga_h; }
bool     bga_available(void){ return bga_ready; }
uint8_t *bga_fb_addr(void) { return (uint8_t *)bga_fb_virt; }

/*
 *  map the whole VRAM into the kernel identity region
 *
 *  bga_map_fb() only maps the visible surface.  The DRM slot plan and
 *  the mode-change blit write beyond it (page-flip slots above the
 *  console scanout), so the full BAR must be addressable.  Safe to
 *  call repeatedly (re-mapping an already-mapped range is a no-op).
 */
bool bga_map_vram_full(void) {
    uint32_t sz = bga_vram_size();
    if (!sz) sz = bga_pitch_bytes * bga_h;
    if (!sz) return false;
    return vmm_identity_map_range(bga_fb_phys, bga_fb_phys + sz,
                                  PTE_PRESENT | PTE_WRITABLE);
}

/*
 *  quiescent device adoption for the DRM backend
 *
 *  When the boot framebuffer came from the GRUB/BIOS path (vesa), the
 *  std-vga device was never driven by bga_init() - but it is the same
 *  dispi hardware, already programmed by the firmware.  Adopt its live
 *  mode without touching enable (no disable/clear cycle), so the DRM
 *  card gains Y_OFFSET panning on top of the running console.
 */
bool bga_quiesce_adopt(void) {
    if (bga_ready) return true;             /* already driven normally */

    uint16_t id = vbe_read(VBE_DISPI_INDEX_ID);
    if (id < VBE_DISPI_ID_MIN || id > VBE_DISPI_ID_MAX) return false;

    uint16_t xres = vbe_read(VBE_DISPI_INDEX_XRES);
    uint16_t yres = vbe_read(VBE_DISPI_INDEX_YRES);
    uint16_t bpp  = vbe_read(VBE_DISPI_INDEX_BPP);
    uint16_t en   = vbe_read(VBE_DISPI_INDEX_ENABLE);
    if (!(en & VBE_DISPI_ENABLED) || !xres || !yres ||
        (bpp != 32 && bpp != 24)) return false;

    uint32_t pci_bar0 = bga_find_fb_bar();
    if (pci_bar0) bga_fb_phys = pci_bar0;

    bga_w = xres; bga_h = yres; bga_bpp = bpp;
    bga_pitch_bytes = ((uint32_t)xres * (bpp / 8) + 3) & ~3u;
    if (!bga_map_fb(bga_fb_phys, bga_pitch_bytes * yres)) return false;
    bga_ready = true;
    printf("[BGA] adopted live firmware mode %ux%u@%u (dispi 0x%x)\n",
           xres, yres, bpp, id);
    return true;
}

/*
 *  KMS support: VRAM geometry + true page-flip panning
 *
 *  The dispi interface can address a virtual screen taller than the
 *  visible mode and pan the scanout with Y_OFFSET (units = scanlines).
 *  The DRM BGA backend reserves VRAM slots above the console scanout
 *  (slot 0), maps user dumb buffers there, and flips by re-programming
 *  Y_OFFSET - zero-copy page flipping, exactly what Linux's bochs-drm
 *  does with its ttm/shmem backing stripped down.
 */

uint32_t bga_vram_size(void) {
    /* QEMU std-vga default: 16 MiB.  Fall back to a safe 8 MiB when the
     * BAR size was not observed (device without PCI info). */
    return bga_vram_sz ? bga_vram_sz : (8u << 20);
}

uint32_t bga_vram_phys(void) { return bga_fb_phys; }
uint32_t bga_pitch(void)     { return bga_pitch_bytes; }

/* Enlarge the virtual (addressable) screen so Y_OFFSET can pan.
 * Must be called before handing out VRAM slot addresses.  Does not
 * disturb the visible contents (no disable/clear). */
void bga_kms_set_virtual(uint32_t virt_height_rows) {
    if (!bga_ready) return;
    vbe_write(VBE_DISPI_INDEX_VIRT_WIDTH, (uint16_t)(bga_pitch_bytes / 4));
    vbe_write(VBE_DISPI_INDEX_VIRT_HEIGHT, (uint16_t)virt_height_rows);
    vbe_write(VBE_DISPI_INDEX_X_OFFSET, 0);
    vbe_write(VBE_DISPI_INDEX_Y_OFFSET, 0);
}

/* Pan the scanout to the given scanline (page flip). */
void bga_kms_pan(uint32_t y_offset_rows) {
    if (!bga_ready) return;
    vbe_write(VBE_DISPI_INDEX_Y_OFFSET, (uint16_t)y_offset_rows);
}

/*
 *  driver-core binding: adopt the std-vga device at bind time.
 *  When the boot framebuffer came from GRUB/BIOS, the dispi hardware is
 *  live but unmanaged, the driver core binds "bga" to 1234:1111 and the
 *  quiescent adoption brings it under kernel control (lspci drv=bga).
 */
static int bga_drv_probe(device_t *dev) {
    (void)dev;
    return bga_quiesce_adopt() ? 0 : -1;
}

static driver_match_t bga_matches[] = {
    { 0x1234, 0x1111, 0xFF },
    { 0, 0, 0 },
};

driver_t bga_drv = {
    .name        = "bga",
    .bus         = BUS_PCI,
    .match_table = bga_matches,
    .probe       = bga_drv_probe,
    .remove      = NULL,
    .priority    = 90,
};

BUILTIN_DRIVER(bga_drv);
