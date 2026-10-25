# RMM

RMM (Recoverable Modular Monolithic Kernel), version 2.

Drivers and filesystems run as modules in fixed slots of
the kernel address space. They are loaded on demand and called through
generated trampolines (plain `call`/`jmp`, no IPC, no message passing,
CR3 never changes), so every module execution is identified,
counted, state-checked and recoverable after a fault.

RMM is OS-independent: it boots a userland from the initrd (/init)
and does not ship or assume any particular distribution. It is mostly Linux
x86-64 syscall ABI compatible.


## Kernel and OS boundary

Rootfs population, user homes and the hostname are OS policy and live in
the OS initramfs, the kernel only mounts its own trees and defaults

## Repository layout

```
arch/          arch backends + porting contract (see arch/README.md)
               x86_64: complete and booting.  arm64: skeleton (NOT buildable).
               
core/          resident core kernel (boot, VMM, PMM, loader, exceptions,
               recovery, scheduler, process/exec/ELF, syscalls, timers,
               module manager, resource ledger)
include/rmm/   kernel + module ABI headers (image.h, module.h, sfi.h,
               fault.h, resource.h, arch.h)
               
modules/       demand-loaded modules: fs, drv_bus, drv_gfx, drv_input

linker/        modules.map: the authoritative fixed-slot manifest

tools/         RMM-Tools submodule: rmm-gen implements the
               whole build pipeline: manifest parse/validate, stub
               assignment, linker scripts, veneers, trampolines, ABI
               tables, module archive + binary validator
               
module-sdk/    SDK for module authors (headers mirrored from include/rmm/)

sfi/           SFI runtime scaffolding

shim/          include-path compatibility only (forwarding headers):
               zero duplicated content; new code includes arch headers
               directly
```

## Build

Requirements: gcc (or x86_64-elf-gcc via CROSS_PREFIX), nasm,
cc (host, for rmm-gen)
binary built from the tools/ submodule on first use.

tools/ is a git submodule (https://github.com/Koyutsu/RMM-Tools).
Clone with submodules, or fetch them in an existing clone:

```
git clone --recurse-submodules https://github.com/Koyutsu/RMM.git
git submodule update --init
```

```
make            # core pass1/pass2 + 4 modules + archive -> build/rmm.elf
make report     # generator/validator report
make clean
```

## License

GPL-3.0, see `LICENSE`. The embedded Terminus console font
(`modules/drv_gfx/graphics/fb_font_terminus.h`) is licensed separately
under the SIL Open Font License 1.1.
