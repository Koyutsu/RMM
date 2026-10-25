; SMP application-processor trampoline
; Assembled as a flat BINARY at a fixed org of 0x7000 and copied to
; physical 0x7000 by smp.c.  APs start here in real MODE (CS:IP =
; 0x0700:0000 after SIPI vector 0x07).
;
; Self-contained: embeds its own GDT (separate 32-bit and 64-bit code
; descriptors - a combined L=1|D=1 descriptor is invalid and QEMU would
; canonicalize it to D=0, executing the protected-mode part with 16-bit
; operands).  Data area uses fixed absolute addresses so no fixups are
; needed after the copy.  Reuses the BSP page tables (identity low RAM).
;
; Fixed data layout (referenced by absolute address from both sides):
; 0x7E00  ap_ids[16]        : LAPIC IDs written by each arriving AP
; 0x7F00  ap_arrived        : u32 counter (locked xadd slot claim)
; 0x7F04  ap_ready          : u32 flag incremented once parked
; 0x7F08  bsp_cr3           : u32, filled by smp.c before SIPI

BITS 16
org 0x7000

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00                 ; scratch stack in free low RAM

    ; real -> protected mode (CS 0x08 = 32-bit code, D=1, L=0)
    o32 lgdt [tr_gdtr]
    mov eax, cr0
    or  eax, 1                     ; CR0.PE
    mov cr0, eax
    jmp dword 0x08:pm32

BITS 32
pm32:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax

    ; protected -> long mode
    mov eax, cr4
    or  eax, (1 << 5)              ; CR4.PAE
    mov cr4, eax

    mov ecx, 0xC0000080            ; IA32_EFER
    rdmsr
    or  eax, (1 << 8)              ; EFER.LME
    wrmsr

    mov eax, [0x7F08]              ; BSP cr3 (identity low RAM map)
    mov cr3, eax

    mov eax, cr0
    or  eax, (1 << 31)             ; CR0.PG (PE still set)
    mov cr0, eax

    jmp dword 0x18:lm64            ; CS 0x18 = 64-bit code (L=1, D=0)

BITS 64
lm64:
    xor eax, eax                   ; full 32-bit clear (zero-extends)
    mov ds, eax
    mov es, eax
    mov ss, eax

    ; claim a slot
    mov eax, 1
    lock xadd dword [abs 0x7F00], eax   ; eax = old value = our slot
    mov r14d, eax                  ; keep slot

    ; per-AP stack: 512 bytes, descending below 0x7000
    lea eax, [r14 + 1]
    shl eax, 9                     ; *512
    mov rsp, 0x6F00
    sub rsp, rax

    ; announce: store our LAPIC id (Initial APIC ID)
    mov eax, 1
    cpuid
    shr ebx, 24                    ; EBX[31:24] = initial APIC id
    lea rdi, [abs 0x7E00]
    mov dword [rdi + r14*4], ebx

    inc dword [abs 0x7F04]         ; ap_ready++

.park:
    cli
    hlt
    jmp .park

; embedded GDT
align 16
tr_gdt:
    dq 0                           ; null
    dq 0x00CF9A000000FFFF          ; 0x08: 32-bit code (D=1, L=0, G=1)
    dq 0x00CF92000000FFFF          ; 0x10: data (D/B=1, G=1, writable)
    dq 0x00209A0000000000          ; 0x18: 64-bit code (L=1, D=0)

tr_gdtr:
    dw tr_gdtr - tr_gdt - 1
    dq tr_gdt

; keep the blob comfortably inside one page
times (0xFE0 - ($ - $$)) db 0
