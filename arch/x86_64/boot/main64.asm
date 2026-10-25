global long_mode_start
extern kernel_main

section .text
bits 64

long_mode_start:
    ; Clear segment registers
    mov ax, 0
    mov ss, ax
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    ; rbx still holds the multiboot2 info physical address that GRUB put
    ; there before jumping through the GDT far-jmp in main.asm.
    ; Pass it as the first argument (System V AMD64 ABI: rdi).
    mov rdi, rbx

    call kernel_main
    hlt
