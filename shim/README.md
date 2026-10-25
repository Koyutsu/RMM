# shim/ - include path compatibility

Older sources include headers like `<cpu/cpu.h>` or `"drivers/ata/ata.h"`.
Those paths don't exist in the real tree anymore, so this directory has
small forwarding headers for them. Each file is a single `#include` of the
real header and has no content of its own.

They are plain files and not symlinks on purpose. Zip on Windows and git
with `core.symlinks=false` turn symlinks into text files, and then
nothing builds.

If you add a header to a directory that has a shim path (`core/`,
`modules/fs/`, the driver dirs), the old path only works once you add a
forwarder here. New code should include the real paths, or better
`include/rmm/arch.h`. When nothing uses the old paths anymore, this
directory goes away.
