; Embedded SMP trampoline blob (built by `nasm -f bin` from
; src/cpu/smp_trampoline.asm, see the Makefile rule for build/tramp.bin).
; incbin keeps the flat binary's fixed addresses intact.

section .rodata
global smp_trampoline_bin
global smp_trampoline_bin_end

smp_trampoline_bin:
    incbin "build/tramp.bin"
smp_trampoline_bin_end:
