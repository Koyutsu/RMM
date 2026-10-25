/*
 * main.c - RMM V2 boot sequence.
 *
 * Every subsystem that lives in a module demand-loads through page faults
 * on first use. The boot log shows the module manager mapping slots
 * lazily, so nothing pays RAM for code that never runs.
 */
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "console/print.h"
#include "console/display.h"
#include "version.h"
#include "cpu/gdt.h"
#include "cpu/idt.h"
#include "drivers/cpu/detectCPU.h"
#include "drivers/ps2/keyboard.h"
#include "drivers/input/evdev.h"
#include "drivers/ps2/mouse.h"
#include "drivers/memory/vmm.h"
#include "drivers/memory/memmap.h"
#include "drivers/graphics/vesa.h"
#include "drivers/graphics/bga.h"
#include "drivers/graphics/modeset.h"
#include "drivers/graphics/vmware_svga.h"
#include "drivers/graphics/fbdev.h"
#include "drivers/core/driver.h"
#include "drivers/gpu/drm.h"
#include "drivers/graphics/fb_font.h"
#include "drivers/timer/pit.h"
#include "drivers/timer/rtc.h"
#include "drivers/ata/ata.h"
#include "drivers/ata/mbr.h"
#include "fs/pseudo.h"
#include "drivers/pci/pci.h"
#include "drivers/pci/msi.h"
#include "fs/vfs.h"
#include "fs/ramfs.h"
#include "fs/initrd.h"
#include "fs/ext4.h"
#include "exec/syscall_table.h"
#include "exec/process.h"
#include <rmm/rmm.h>
#include <rmm/module.h>
#include <rmm/arch.h>
#include "cpu/percpu.h"

extern uint8_t kernel_phys_start[];
extern uint8_t kernel_phys_end[];

mem_map_t g_memmap;
bool      g_memmap_valid = false;

/* active framebuffer geometry (read by SYS_RMM_FBINFO / neofetch) */
uint32_t g_fb_width = 0, g_fb_height = 0, g_fb_bpp = 0;

/* RMM subsystems */
void rmm_modules_init(void);
void loader_init(void);
void rmm_modules_dump(void);

static void percpu_check(const char *tag) {
    percpu_t *pc = percpu_this();
    if (pc->self != (uint64_t)(uintptr_t)pc)
        printf("[canary] %s: self=%p expect=%p\n", tag,
               (void *)(uintptr_t)pc->self, (void *)pc);
}

static void pmm_report(const char *tag) {
    printf("  [%s: %u/%u pages used (%u KiB live)]\n", tag,
           (unsigned)arch_page_used(), (unsigned)arch_page_total(),
           (unsigned)(arch_page_used() * 4));
}

static void ps2_flush(void) {
    for (int i = 0; i < 32; i++) {
        if (!(inb(PS2_STATUS_PORT) & PS2_STATUS_OUTPUT_FULL)) break;
        inb(PS2_DATA_PORT);
    }
}

#ifndef BOOT_RAMFS
static bool mount_rootfs(void) {
    writeInfo("Probing ATA drives");
    ata_init();

    int buses[2]   = { ATA_PRIMARY,  ATA_PRIMARY };
    int drives[2]  = { ATA_MASTER,   ATA_SLAVE   };

    for (int i = 0; i < 2; i++) {
        int bus = buses[i], drv = drives[i];

        if (!ata_present(bus, drv)) {
            print_str(" [ATA "); printf("%d:%d", bus, drv);
            print_str(" not present]\n");
            continue;
        }

        uint64_t secs = ata_sectors(bus, drv);
        print_str(" [ATA "); printf("%d:%d", bus, drv);
        print_str(" present, "); printf("%u", (unsigned)(secs / 2048));
        print_str(" MiB]\n");

        part_entry_t parts[4];
        if (!mbr_read(bus, drv, parts)) {
            writeWarn("MBR read failed or bad signature");
            continue;
        }

        int pidx = mbr_find_linux(parts);
        if (pidx < 0) {
            writeWarn("No Linux partition found in MBR");
            continue;
        }

        int probe = ext4_probe(bus, drv, parts[pidx].lba_start);
        if (probe != EXT4_OK) {
            print_str(" [ext4_probe failed: "); printf("%d", probe);
            print_str("]\n");
            continue;
        }

        writeInfo("Mounting ext2/ext4 rootfs");
        vfs_node_t *root = ext4_mount(bus, drv, parts[pidx].lba_start);
        if (!root) {
            writeWarn("ext4_mount returned NULL");
            continue;
        }

        vfs_mount("/", root);

        vfs_node_t *tmp_root = ramfs_init();
        vfs_mount("/tmp", tmp_root);

        writeOK("ext2/ext4 rootfs mounted");
        return true;
    }

    writeWarn("No bootable disk found -- falling back to ramfs");
    return false;
}
#endif

void kernel_main(uint64_t mb2_info_phys) {
    print_clear();
    stack_guard_init();   /* paint stack-bottom magic before any deep call */
    writeInfo("Loading GDT");
    gdt_init();
    writeOK("GDT loaded");

    writeInfo("Loading IDT + PIC");
    idt_init();
    writeOK("IDT ready, interrupts enabled");

    /* GS base after the GDT: segment reloads in long mode clobber the
     * GS.base MSR with the descriptor base, per-CPU addressing
     * must be installed last before interrupts do their first tick. */
    arch_percpu_bsp_init();
    writeOK("per-CPU RMM context installed (GS base)");
    percpu_check("post-percpu");

    cpu_fpu_init();
    writeOK("FPU/SSE ready");

    /* RMM V2 core bring-up (resident minimum) */
    writeInfo("RMM core: module manager");
    rmm_modules_init();
    loader_init();
    writeOK("RMM core ready (slots armed, demand loading active)");

    writeInfo("Parsing memory map");
    g_memmap_valid = memmap_parse(mb2_info_phys, &g_memmap);
    if (g_memmap_valid) {
        uint64_t usable = memmap_init_pmm(&g_memmap, mb2_info_phys,
                              (uint64_t)(uintptr_t)kernel_phys_start,
                              (uint64_t)(uintptr_t)kernel_phys_end);
        writeOK("PMM ready");
        print_str(" (");
        printf("%u", (unsigned)(usable / (1024*1024)));
        print_str(" MiB)");
    } else {
        writeWarn("No memory map -- assuming 32 MiB");
        pmm_init(32ULL * 1024 * 1024);
        for (uint64_t a = 0;              a < 2ULL*1024*1024;  a += PAGE_SIZE) pmm_alloc_page(a);
        for (uint64_t a = 2ULL*1024*1024; a < 32ULL*1024*1024; a += PAGE_SIZE) pmm_free_page(a);
    }
    pmm_report("PMM");

    writeInfo("Initializing VMM");
    vmm_init();
    writeOK("VMM ready");

    writeInfo("Initializing PIT (1000 Hz)");
    pit_init(1000);
    writeOK("PIT ready");
    percpu_check("post-pit");

    /* First module demand-loads happen right here: evdev/kb/mouse live
     * in drv_input, the stub's page fault maps the slot on first call
     * (- no loader calls exist in the boot code). */
    writeInfo("Initializing evdev + keyboard");
    evdev_init();
    kb_init();
    if (mouse_init())
        writeOK("evdev ready (event0 kbd, event1 mouse)");
    else
        writeOK("evdev ready (event0 kbd)");
    percpu_check("post-evdev");
    pmm_report("PMM");

    writeInfo("Initializing VFS");
    vfs_init();

    {
        extern bool ramfs_heap_init(void);
        if (!ramfs_heap_init()) {
            writeInfo("ramfs heap map FAILED");
        }
    }

#ifdef BOOT_RAMFS
    vfs_node_t *rootfs = ramfs_init();
    vfs_mount("/", rootfs);
    (void)initrd_load_from_mb2(rootfs, mb2_info_phys);
    writeOK("VFS ready (initrd + ramfs)");
    percpu_check("post-vfs");
#else
    bool disk_ok = mount_rootfs();
    if (!disk_ok) {
        vfs_node_t *rootfs = ramfs_init();
        vfs_mount("/", rootfs);
        (void)initrd_load_from_mb2(rootfs, mb2_info_phys);
        writeOK("VFS ready (ramfs fallback)");
    }
#endif
    pmm_report("PMM");

    writeInfo("Mounting /proc + /dev");
    pseudo_mount_all();
    writeOK("procfs-lite + devtmpfs ready");

    writeInfo("Installing syscall table");
    syscall_init();
    writeOK("Syscall table + INT 0x80 handler ready");

    writeInfo("Enumerating PCI/PCIe");
    pci_init();
    pci_dump();
    writeOK("PCI enumeration complete");

    writeInfo("Enabling Local APIC");
    if (lapic_init()) {
        display_newline();
        display_puts("  [ LAPIC enabled @ 0x");
        display_printf("%p", (void *)(uintptr_t)lapic_base());
        display_puts(" - MSI-capable interrupts ready ]");
    } else {
        writeWarn("No LAPIC - MSI devices fall back to INTx");
    }

    /* Driver model: builtins register at each module's module_entry
     * ; binding happens after enumeration. */
    writeInfo("Binding PCI drivers");
    driver_register_builtins();
    bus_enumerate_pci();
    writeOK("Driver core ready");

    writeInfo("Probing graphics");
    uint8_t  *fb_addr = NULL;
    uint32_t  fb_w=0, fb_h=0, fb_pitch=0;
    const char *fb_drv = NULL;

    {
        modeset_mode_t def = modeset_default();
        if (bga_init((uint16_t)def.w, (uint16_t)def.h, 32)) {
            fb_w=bga_width(); fb_h=bga_height(); fb_pitch=bga_pitch();
            fb_addr=bga_fb_addr();
            fb_drv = "BGA";
            writeOK("BGA scanout, normal mode");
        } else if (vesa_init(mb2_info_phys)) {
            fb_w=vesa_width(); fb_h=vesa_height(); fb_pitch=vesa_pitch();
            fb_addr=vesa_fb_addr();
            fb_drv = "VESA/VBE";
            writeOK("VESA framebuffer (firmware mode kept)");
        } else {
            graphics_install_vmware(1024, 768);
            fb_addr=vmware_fb_addr(); fb_w=1024; fb_h=768; fb_pitch=1024*4;
            if (fb_addr) { fb_drv = "vmware-svga"; writeOK("VMware SVGA-II"); }
            else         writeWarn("No graphics -- TTY only");
        }
    }
    if (fb_addr) {
        g_fb_width = fb_w;
        g_fb_height = fb_h;
        g_fb_bpp = 32;

        {
            static fbdev_t primary_fb;
            primary_fb.name     = fb_drv;
            primary_fb.fb       = fb_addr;
            primary_fb.width    = fb_w;
            primary_fb.height   = fb_h;
            primary_fb.pitch    = fb_pitch;
            primary_fb.bpp      = 32;
            primary_fb.priority = 200;
            primary_fb.flush_rows = NULL;
            primary_fb.flush_rect = NULL;
            primary_fb.priv       = NULL;
            fbdev_register(&primary_fb);
        }
    }

    drm_init();

    boot_mode_t mode = BOOT_MODE_TTY;
    if (fb_addr) {
        fb_font_init(fb_addr, fb_w, fb_h, fb_pitch, 32);
        mode = BOOT_MODE_GRAPHICS;
        fb_font_set_color(FB_WHITE, FB_BLACK);
        fb_font_clear();
    } else {
        writeWarn("No framebuffer -- TTY mode");
    }
    ps2_flush();
    display_init(mode);

    display_info(RMM_KERNEL_NAME " " RMM_VERSION " (V2)");
    display_warn("Pre-alpha -- may be unstable");
    display_newline();
    display_set_color(FB_WHITE, FB_BLACK);
    display_puts("Licensed under GPL-3.0. Copyright (c) 2025-present Epiczhul.\n");
    display_newline();

    writeInfo("Starting init");
    display_newline();

    {
        extern int proc_create(process_t *, const char *, int, char **, char **);
        extern int proc_exec(process_t *);

        static char *init_argv[]  = { "/init", NULL };
        static char *init_envp[]  = {
            "PATH=/bin",
            "HOME=/root",
            "TERM=linux",
            "SHELL=/bin/dash",
            NULL,
        };

        process_t *init = proc_alloc();
        static const char *const init_paths[] = {
            "/init", "/bin/init", "/bin/sh", NULL
        };
        bool init_ok = false;
        for (int i = 0; init && !init_ok && init_paths[i]; i++) {
            if (proc_create(init, init_paths[i], 1, init_argv, init_envp)
                == ELF_OK)
                init_ok = true;
        }
        if (init_ok) {
            writeOK("init spawned (PID 1) - starting BORE scheduler");
            __asm__ volatile("sti");
            extern void sched_start(process_t *);
            sched_start(init);
        }
        writeWarn("No usable /init in initrd - system halted");
        display_puts("  HINT: boot with an initrd that contains /init\n");
        display_puts("        (passed as a multiboot2 module).\n");
    }

    while (1) {
        __asm__ volatile("hlt");
    }
}
