#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "fs/vfs.h"

/*
 * elf - user-image loader (static ET_EXEC + PIE ET_DYN + IA-32)
 *
 * Three image flavours reach the loader from proc_execve / proc_create:
 *
 *   1. ET_EXEC ELF64 (static, the original model) - p_vaddr used as-is
 *   2. ET_DYN  ELF64 (PIE / shared object), mapped at a load
 *      Bias, PT_INTERP names ld-musl-x86_64.so.1 which the kernel loads
 *      at its own bias and jumps to first (Linux exec semantics: kernel
 *      maps both images, the interpreter does the dynamic linking)
 *   3. ET_EXEC ELF32 EM_386 (IA-32 compat) - 32-bit p_vaddr in
 *      the low 4 GiB, 32-bit stack + auxv, enters via a 32-bit code
 *      segment (GDT_USER_CODE32) and int 0x80 for syscalls
 *
 * execve rejects anything else (ELF_NOT_EXEC / ELF_NOT_64).
 */

/* Result codes */
#define ELF_OK          0
#define ELF_NOT_ELF    -1   /* bad magic */
#define ELF_NOT_64     -2   /* not ELF64 (or not x86-64) */
#define ELF_NOT_EXEC   -3   /* not an executable */
#define ELF_NOT_STATIC -4   /* has dynamic sections (legacy code path) */
#define ELF_NO_MEM     -5   /* out of memory */
#define ELF_NO_ENTRY   -6   /* entry point is 0 */
#define ELF_NO_INTERP  -7   /* PT_INTERP named a missing interpreter */
#define ELF_BAD_IA32   -8   /* ELF32 that is not EM_386 ET_EXEC */

typedef struct {
    void    *load_base;    /* first loaded byte (convenience)  */
    uint64_t entry;        /* entry point VA                   */
    uint64_t load_end;     /* first VA past last PT_LOAD page  */
    int      status;       /* ELF_OK or ELF_*                  */

    /* dynamic-linking + ia32 metadata */
    bool     is_dyn;       /* ET_DYN: loaded at load_bias          */
    bool     is_ia32;      /* ELFCLASS32/EM_386 image              */
    uint64_t load_bias;    /* VA added to every p_vaddr (0 static) */
    uint64_t phdr_va;      /* user VA of the ELF program headers   */
    uint64_t phent;        /* e_phentsize (56 for ELF64)           */
    uint64_t phnum;        /* e_phnum                              */
    char     interp_path[64]; /* PT_INTERP target, "" if none      */
} elf_image_t;

/* Load-bias policy (fixed, no ASLR yet, deterministic for tests)
 * ET_EXEC        : p_vaddr as linked (0x200000..., page-mapped)
 * PIE main image : ELF_DYN_BASE  = 0x10000000 (below the mmap window at
 *                  0x40000000, above every conventional ET_EXEC link)
 * interpreter    : ELF_INTERP_BASE = 0x30000000 (between the two)
 * Both are far from PROC_MMAP_BASE and PROC_STACK_BASE so no allocator
 * can collide with them.                                        */
#define ELF_DYN_BASE       0x10000000ULL
#define ELF_INTERP_BASE    0x30000000ULL

/*
 * Load an ELF from a VFS node.
 * Returns ELF_OK on success, the image is ready to execute.
 * Call elf_exec() to run it.
 */
int elf_load(vfs_node_t *node, elf_image_t *out);

/*
 * Execute a loaded ELF image.
 * Returns the exit code (what the program returned from main).
 */
int elf_exec(const elf_image_t *img, int argc, char **argv);

/*
 * Convenience: load + exec from a VFS path.
 * Returns exit code, or a negative ELF_* error.
 */
int elf_run(const char *path, int argc, char **argv);