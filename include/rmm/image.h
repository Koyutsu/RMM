#pragma once
/*
 * image.h - the .rmm module image format.
 *
 * An image is produced by linking a module at its fixed slot address and
 * objcopy-ing it to a flat binary.  There are no runtime relocations
 * : every cross-module reference is resolved at link time to
 * final addresses (direct calls to core, stub calls to other modules).
 *
 * Flat binary layout (offsets are fields below, all little-endian):
 *   [header][code][rodata][data]        (bss is not stored -)
 */
#include <rmm/rmm.h>

struct rmm_image_header {
    uint32_t magic;            /* RMM_MAGIC "RMM2"                     */
    uint16_t version;          /* image format version (1)             */
    uint16_t arch;             /* enum rmm_arch                        */
    uint32_t module_id;        /* must match modules.map               */

    uint64_t code_offset;      /* into the flat binary                 */
    uint64_t code_size;
    uint64_t rodata_offset;
    uint64_t rodata_size;
    uint64_t data_offset;
    uint64_t data_size;
    uint64_t bss_size;         /* zeroed at load, not stored           */

    uint64_t entry_offset;     /* module entry point (init), VA offset */
    uint64_t abi_version;      /* must match kernel RMM_ABI_VERSION    */
    uint64_t flags;

    uint8_t  build_id[32];     /* content hash -               */
    char     name[16];         /* NUL-terminated module name           */
};

#define RMM_IMAGE_VERSION 1

/* Validation checklist performed by the loader on every load:
 * magic, architecture, module id, ABI version, slot fit (code+rodata+data
 * +bss <= slot size), segment bounds, build id, dependencies READY. */
