section .multiboot_header
header_start:
    ; Multiboot2 magic
    dd 0xe85250d6
    ; architecture: 0 = i386 protected mode (used for x86-64 too)
    dd 0
    ; header length
    dd header_end - header_start
    ; checksum
    dd 0x100000000 - (0xe85250d6 + 0 + (header_end - header_start))

    ; Framebuffer request tag (type 5)
    ; GRUB will set a linear framebuffer before jumping to the kernel.
    ; The multiboot2 info block passed in %rbx will then contain a
    ; framebuffer tag (type 8) with the physical address, dimensions and
    ; pixel format.  vesa_init() reads that tag.
    ; flags=1 means "optional" - boot continues even if GRUB can't comply.
    align 8
fb_tag_start:
    dw 5                            ; tag type: framebuffer request
    dw 1                            ; flags: optional
    dd fb_tag_end - fb_tag_start    ; tag size
    dd 1024                         ; preferred width  (0 = no preference)
    dd 768                          ; preferred height
    dd 32                           ; preferred bpp
fb_tag_end:

    ; End tag (required)
    align 8
    dw 0
    dw 0
    dd 8
header_end:
