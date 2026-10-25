# arch/ - architecture backends

```
arch/
+-- x86_64/     the working port, boots
|   +-- boot/       multiboot2 header, 32 to 64 bit entry
|   +-- context/    GDT, per-CPU areas, LAPIC, MADT, cpu detection
|   +-- mm/         PMM bitmap, VMM (4-level paging), memmap
|   +-- traps/      IDT, exception entry -> rmm_trap_frame
|   +-- sfi/        x86 SFI notes (masking sequences, IBT)
|   +-- include/    x86_64 device headers (drivers/*) used across the tree
|   +-- arch.c      implementation of include/rmm/arch.h
+-- arm64/      skeleton, not buildable and not bootable (see below)
```

## The arch contract

`include/rmm/arch.h` is the interface the architecture-independent part
of the kernel talks to: module manager, loader, exceptions and recovery,
resource ledger. A port has to provide roughly this:

- trap frame fill and fault classification (`rmm_trap_frame`, `rmm_page_fault`)
- the recovery patch (`arch_recovery_patch`), which rewrites the trap
  frame so the CPU returns around the broken module
- the per-CPU context (`arch_cpu_ctx`, `arch_percpu_bsp_init`)
- page table map, unmap, translate and protect, plus TLB sync for a range
- IRQ control, timer, console and `arch_time_ms`
- `arch_page_used` and `arch_page_total` so the core can see PMM usage

There are two arch entry points: exception entry and syscall entry.

## How x86-specific is the rest of the tree?

More than the contract suggests. Right now:

- 8 core files contain x86-64 asm or privileged instructions:
  `core/drivers/serial/io.h`, `core/drivers/timer/pit.c`,
  `core/drivers/vga/vga_text.c`, `core/exec/process.c`,
  `core/exec/sched.c`, `core/exec/syscall_table.c` (rd/wrfsbase, swapgs),
  `core/exec/timers.c` and `core/kernel/main.c` (sti/hlt, percpu install).
  `core/exec/syscall_entry.S` is arch entry code anyway.
- 15 core files include `cpu/*` or x86 driver headers directly instead of
  going through `include/rmm/arch.h`.
- The modules are x86 device drivers: PS/2 port I/O, ATA, PCI config
  cycles, BGA and VBE. That is fine, drivers are device-specific. An arm64
  build would ship its own driver modules in the same slots, so don't
  expect them to be portable code.
- The policy layer is clean. Module manager, loader, exceptions, recovery,
  the resource ledger and the generator are plain C over `arch.h`.

Closing the gap means routing the asm and `inb/outb` call sites through
`arch.h` and moving the x86 console and timer glue under `arch/x86_64/`.
That is planned but not done. Until then, arch independence only covers
the policy layer.

## Porting checklist

What a new arch has to provide:

1. `arch/<arch>/boot/`: the boot protocol entry (multiboot2 or an EFI
   handoff), an early stack and a constant mapping of the core image.
2. `arch/<arch>/mm/`: a PMM over the memory map, page table code and
   `PAGE_SIZE` handling. RMM only needs map, unmap, translate, protect and
   a TLB sync for a range.
3. `arch/<arch>/traps/`: map vectors to `enum rmm_fault_class`, fill
   `rmm_trap_frame` and call `rmm_page_fault()`.
4. `arch/<arch>/context/`: the per-CPU area and `arch_cpu_ctx()`. The
   module manager reads `current_module` and `current_call_frame` from it
   on every fault.
5. `arch/<arch>/arch.c`: the `arch.h` operations. The hard one is
   `arch_recovery_patch()`, which has to return around the faulting
   module's stub frame. Look at the x86_64 version and the call-frame
   layout in `include/rmm/module.h`.
6. Module slots: pick the VA window, keep `base % size == 0` with
   power-of-two sizes, and set `RMM_SLOT_VA_LO` and `RMM_SLOT_VA_HI` in the
   port's manager config.
7. One asm file for syscall entry that matches the target ABI.

## arm64

`arch/arm64/` is a skeleton. It follows the x86_64 file layout and has
documented stubs for the page table model, the ESR/FAR to fault class
mapping, the TPIDR_EL1 per-CPU context and the SFI and/orr masking
sequences, plus notes on the role of PAC. It does not build
and does not boot. It is there so a port is filling in blanks instead of
a redesign, and so you can see what the surface looks like. The Makefile
only builds x86_64. Don't wire arm64 objects in before the checklist
above is done.
