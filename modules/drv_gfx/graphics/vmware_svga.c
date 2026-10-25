#include "drivers/pci/io.h"
#include "drivers/pci/pci.h"
#include "drivers/core/driver.h"
#include "console/print.h"
#include "drivers/memory/vmm.h"
#include "drivers/graphics/vmware_svga.h"
#include <stdint.h>
#include <stdbool.h>

//  VMware SVGA-II register interface
#define SVGA_INDEX_PORT     0
#define SVGA_VALUE_PORT     4   // Note: the value port is at BASE+4, not BASE+1

#define SVGA_REG_ID         0
#define SVGA_REG_ENABLE     1
#define SVGA_REG_WIDTH      2
#define SVGA_REG_HEIGHT     3
#define SVGA_REG_MAX_WIDTH  4
#define SVGA_REG_MAX_HEIGHT 5
#define SVGA_REG_DEPTH      6
#define SVGA_REG_BPP        7
#define SVGA_REG_PSEUDOCOLOR 8
#define SVGA_REG_RED_MASK   9
#define SVGA_REG_GREEN_MASK 10
#define SVGA_REG_BLUE_MASK  11
#define SVGA_REG_BYTES_PER_LINE 12
#define SVGA_REG_FB_START   13
#define SVGA_REG_FB_OFFSET  14
#define SVGA_REG_VRAM_SIZE  15
#define SVGA_REG_FB_SIZE    16
#define SVGA_REG_CAPABILITIES 17

// SVGA-II magic/version IDs
#define SVGA_MAGIC         0x900000UL
#define SVGA_VERSION_2     2
#define SVGA_ID_2          (SVGA_MAGIC << 8 | SVGA_VERSION_2)

// VMware PCI vendor / device
#define VMWARE_VENDOR_ID   0x15AD
#define VMWARE_SVGA_DEVICE 0x0405

//  Driver state
static uint32_t  svga_io_base     = 0;
static uint32_t *svga_framebuffer = NULL;   // virtual address of FB
static uint32_t  svga_width       = 0;
static uint32_t  svga_height      = 0;
static uint32_t  svga_pitch       = 0;      // bytes per line
static bool      svga_enabled     = false;

//  Low-level register access
static void svga_write(uint32_t reg, uint32_t value) {
    outportl((uint16_t)(svga_io_base + SVGA_INDEX_PORT), reg);
    outportl((uint16_t)(svga_io_base + SVGA_VALUE_PORT), value);
}

static uint32_t svga_read(uint32_t reg) {
    outportl((uint16_t)(svga_io_base + SVGA_INDEX_PORT), reg);
    return inportl((uint16_t)(svga_io_base + SVGA_VALUE_PORT));
}

//  PCI lookup via the PCI core (device registry, sized BARs)
static bool vmware_find_io_base(uint32_t *out_io_base) {
    pci_device_t *d = pci_find_device(VMWARE_VENDOR_ID, VMWARE_SVGA_DEVICE);
    if (!d) return false;
    for (int i = 0; i < 6; i++) {
        if (d->bars[i].type == PCI_BAR_IO && d->bars[i].size) {
            *out_io_base = (uint32_t)d->bars[i].base;
            return true;
        }
    }
    return false;
}

//  Map the physical framebuffer into virtual address space
static bool svga_map_framebuffer(uint64_t phys_fb, uint32_t fb_size) {
    // We identity-map the framebuffer (phys == virt) using the VMM.
    // This is safe as long as the FB sits within the lower 4 GiB and
    // doesn't collide with kernel text/data (typically above 0xFD000000
    // for VMware).
    uint64_t phys_start = phys_fb & ~(uint64_t)(PAGE_SIZE - 1);
    uint64_t phys_end   = phys_fb + fb_size;

    if (!vmm_identity_map_range(phys_start, phys_end,
                                PTE_PRESENT | PTE_WRITABLE)) {
        print_str("[SVGA] ERROR: failed to map framebuffer pages\n");
        return false;
    }

    svga_framebuffer = (uint32_t *)(uintptr_t)phys_fb;
    return true;
}

//  Public API
void graphics_install_vmware(uint16_t w, uint16_t h) {
    // 1. Find the device through the PCI device registry
    if (!vmware_find_io_base(&svga_io_base)) {
        print_str("[SVGA] VMware SVGA-II device not found\n");
        return;
    }
    printf("[SVGA] IO base: 0x%x\n", svga_io_base);

    // 2. Negotiate SVGA-II protocol version
    svga_write(SVGA_REG_ID, SVGA_ID_2);
    if (svga_read(SVGA_REG_ID) != SVGA_ID_2) {
        print_str("[SVGA] SVGA-II version negotiation failed\n");
        return;
    }

    // 3. Disable while configuring
    svga_write(SVGA_REG_ENABLE, 0);

    // 4. Set desired resolution & colour depth
    svga_write(SVGA_REG_WIDTH,  w);
    svga_write(SVGA_REG_HEIGHT, h);
    svga_write(SVGA_REG_BPP,   32);

    // 5. Enable
    svga_write(SVGA_REG_ENABLE, 1);

    // 6. Read back actual values
    svga_width  = svga_read(SVGA_REG_WIDTH);
    svga_height = svga_read(SVGA_REG_HEIGHT);
    svga_pitch  = svga_read(SVGA_REG_BYTES_PER_LINE);

    uint32_t fb_phys = svga_read(SVGA_REG_FB_START);
    uint32_t fb_size = svga_read(SVGA_REG_FB_SIZE);

    printf("[SVGA] Resolution: %dx%d, pitch: %d\n",
           svga_width, svga_height, svga_pitch);
    printf("[SVGA] Framebuffer phys: 0x%x, size: %d KiB\n",
           fb_phys, fb_size / 1024);

    // 7. Map the framebuffer into virtual address space
    if (!svga_map_framebuffer(fb_phys, fb_size)) return;

    svga_enabled = true;
    print_str("[SVGA] Driver ready\n");
}

void vmware_set_resolution(uint16_t w, uint16_t h) {
    if (!svga_io_base) return;
    svga_write(SVGA_REG_ENABLE,  0);
    svga_write(SVGA_REG_WIDTH,   w);
    svga_write(SVGA_REG_HEIGHT,  h);
    svga_write(SVGA_REG_BPP,    32);
    svga_write(SVGA_REG_ENABLE,  1);

    svga_width  = svga_read(SVGA_REG_WIDTH);
    svga_height = svga_read(SVGA_REG_HEIGHT);
    svga_pitch  = svga_read(SVGA_REG_BYTES_PER_LINE);

    // Re-map in case the FB address changed
    uint32_t fb_phys = svga_read(SVGA_REG_FB_START);
    uint32_t fb_size = svga_read(SVGA_REG_FB_SIZE);
    svga_map_framebuffer(fb_phys, fb_size);
}

void vmware_clear_screen(uint32_t color) {
    if (!svga_enabled || !svga_framebuffer) return;

    uint32_t pixels_per_row = svga_pitch / 4;
    for (uint32_t y = 0; y < svga_height; y++) {
        for (uint32_t x = 0; x < svga_width; x++) {
            svga_framebuffer[y * pixels_per_row + x] = color;
        }
    }
}

void vmware_draw_pixel(int x, int y, uint32_t color) {
    if (!svga_enabled || !svga_framebuffer) return;
    if (x < 0 || y < 0) return;
    if ((uint32_t)x >= svga_width || (uint32_t)y >= svga_height) return;

    uint32_t pixels_per_row = svga_pitch / 4;
    svga_framebuffer[(uint32_t)y * pixels_per_row + (uint32_t)x] = color;
}

// Fill a rectangle
void vmware_fill_rect(int x, int y, int w, int h, uint32_t color) {
    if (!svga_enabled || !svga_framebuffer) return;
    for (int row = y; row < y + h; row++)
        for (int col = x; col < x + w; col++)
            vmware_draw_pixel(col, row, color);
}
uint8_t *vmware_fb_addr(void) { return (uint8_t *)svga_framebuffer; }

/*
 *  driver-core binding: probe + enable the SVGA-II device at
 *  bind time so the DRM layer can pick it as scanout backend (and
 *  lspci shows drv=vmware-svga) without waiting for the fallback chain.
 */
static int vmware_drv_probe(device_t *dev) {
    (void)dev;
    if (vmware_fb_addr()) return 0;             /* already enabled */
    graphics_install_vmware(1024, 768);
    return vmware_fb_addr() ? 0 : -1;
}

static driver_match_t vmware_matches[] = {
    { 0x15AD, 0x0405, 0xFF },
    { 0, 0, 0 },
};

driver_t vmware_drv = {
    .name        = "vmware-svga",
    .bus         = BUS_PCI,
    .match_table = vmware_matches,
    .probe       = vmware_drv_probe,
    .remove      = NULL,
    .priority    = 95,
};

BUILTIN_DRIVER(vmware_drv);
