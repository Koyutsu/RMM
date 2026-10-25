# module-sdk, headers for module authors

The public module ABI headers show up here as small forwarding headers,
so module authors can point their include path at this directory.

```
include/rmm/image.h     module image format (144 byte header)
include/rmm/module.h    descriptors, call frames, lifecycle
include/rmm/sfi.h       SFI domain helpers (rmm_confine)
include/rmm/fault.h     fault classes, what a module can trigger
include/rmm/resource.h  resource ledger policy (recovery cleanup)
```

Each file is a single `#include` of the real header in `../include/rmm/`.
The kernel tree is the only source of truth, nothing here is a copy. They
are plain files and not symlinks because zip on Windows and git with
`core.symlinks=false` turn symlinks into text files.

Rules for module authors:

- Compile with `-mcmodel=large`, `-ffreestanding` and
  `-fcf-protection=branch`. Never use privileged instructions, the
  post-link validator rejects the image.
- Import core data only through the generated aliases. Don't compute core
  addresses by hand, modules.map is authoritative.
- Expect fault attribution. A bad pointer you hand to a core service
  faults at the core PC and is attributed to your module, recovery then
  returns `-RMM_ERR_MODULE_FAULT` to your caller.
- Pages you allocate from the core while running are ledgered. On
  recovery they are inventoried, and freed only if they were tagged as
  exclusively owned (see `include/rmm/resource.h`).
