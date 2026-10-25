# arch/arm64/boot, boot protocol notes (no code yet)

There is no multiboot2 on AArch64. Options, in the order the port would
implement them:

1. **EFI handoff / Linux-boot protocol image**: a PE/COFF EFI stub that
   installs a flat identity map for the core image, hands over
   (memory map from GetMemoryMap), and jumps to an entry that mirrors
   `arch/x86_64/boot/main*.asm`: build early stack (64 KiB, same
   bring-up invariant), L0/L1 identity map, TPIDR_EL1, then
   `kernel_main(0)`.
2. **Device tree**: DTB address passed in x0; parse /memory for the
   memory map (the analogue of the multiboot2 memmap parse) and the
   serial/console node for early output.

Invariants carried over from x86_64 bring-up that the arm64 entry must
preserve: 64 KiB boot stack, core image mapped before any C runs, no
interrupts until the per-CPU area is installed, and the module-slot VA
window must not collide with the identity map.
