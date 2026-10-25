# arch/arm64 - skeleton, not a port

This directory follows the x86_64 layout and has documented stubs, so an
ARM64 port means filling in blanks instead of a redesign. It does not
build and it does not boot. The porting checklist is in `../README.md`.

```
arch.c              arch.h ops: per-CPU, IRQ, timer, console, recovery patch
mm/vmm.c            AArch64 4-level page table model (48-bit VA, 4 KiB)
traps/traps.c       ESR_ELx/FAR_ELx to enum rmm_fault_class mapping
context/percpu.c    TPIDR_EL1 per-CPU area
boot/               notes only (no multiboot2 on arm64, so EFI stub or DT)
```

Notes the stubs carry:

- Fault classes: ESR.EC and ISS map to `RMM_EXC_PAGE_FAULT` (data and
  instruction aborts), `RMM_EXC_NULL` (FAR == 0), `RMM_EXC_ILLEGAL_INSTR`
  (unknown/UNALLOC), `RMM_EXC_SFI_VIOLATION` (permission fault on a slot
  page) and `RMM_EXC_STACK_FAULT` (SP abort).
- Per-CPU: `TPIDR_EL1` holds the base of the per-CPU area. The
  `rmm_cpu_context` (current module, call frame chain, recovery stack)
  sits at a fixed offset in it, like the GS-relative layout on x86_64,
  and fault attribution depends on it just as much.
- SFI masking: `base | (p & mask)` compiles to `and` plus `orr` on
  AArch64, no address-bit tricks needed. Document the sequence per module
  in the generated ABI table.
- PAC: signing the return addresses in `struct rmm_call_frame` is the
  arm64 bonus layer, comparable to IBT on x86_64. Sign when recording,
  authenticate in the recovery patch.
- Recovery patch: `arch_recovery_patch()` has to restore the frame per
  the AArch64 procedure call ABI and set ELR_ELx to the recorded return
  address of the stub, with the error code in x0. It is the exact
  analogue of the x86_64 version.
