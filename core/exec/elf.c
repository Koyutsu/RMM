#include "exec/elf.h"
#include "fs/vfs.h"
#include "drivers/memory/vmm.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define ELF_MAGIC   0x464C457F
#define ET_EXEC     2
#define EM_X86_64   62
#define PT_LOAD     1
#define PT_DYNAMIC  2

typedef struct __attribute__((packed)) {
    uint32_t e_magic;
    uint8_t  e_class;
    uint8_t  e_data;
    uint8_t  e_version;
    uint8_t  e_osabi;
    uint8_t  e_abiversion;
    uint8_t  e_pad[7];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version2;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} elf64_hdr_t;

typedef struct __attribute__((packed)) {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} elf64_phdr_t;

static void mem_zero(void *p, size_t n) {
    uint8_t *b = (uint8_t *)p; while (n--) *b++ = 0;
}


/* Primary entry point: load from vfs_node_t directly */
int elf_load(vfs_node_t *node, elf_image_t *out) {
    if (!node || !out) return ELF_NOT_ELF;
    out->load_base = NULL; out->entry = 0; out->status = ELF_NOT_ELF;

    /* Read via node ops directly (bypassing fd offset tracking) */
    elf64_hdr_t hdr;
    if (node->ops->read(node, &hdr, sizeof(hdr), 0) != (int)sizeof(hdr))
        return ELF_NOT_ELF;

    if (hdr.e_magic   != ELF_MAGIC)   return ELF_NOT_ELF;
    if (hdr.e_class   != 2)           return ELF_NOT_64;
    if (hdr.e_type    != ET_EXEC)     return ELF_NOT_EXEC;
    if (hdr.e_machine != EM_X86_64)   return ELF_NOT_64;
    if (hdr.e_entry   == 0)           return ELF_NO_ENTRY;
    if (hdr.e_phentsize != sizeof(elf64_phdr_t)) return ELF_NOT_ELF;
    if (hdr.e_phnum == 0 || hdr.e_phnum > 16)    return ELF_NOT_ELF;

    elf64_phdr_t phdrs[16];
    size_t phsz = (size_t)hdr.e_phnum * sizeof(elf64_phdr_t);
    if (node->ops->read(node, phdrs, phsz, (size_t)hdr.e_phoff) != (int)phsz)
        return ELF_NOT_ELF;

    for (int i = 0; i < hdr.e_phnum; i++)
        if (phdrs[i].p_type == PT_DYNAMIC) return ELF_NOT_STATIC;

    uint64_t load_min = (uint64_t)-1, load_max = 0;
    for (int i = 0; i < hdr.e_phnum; i++) {
        if (phdrs[i].p_type != PT_LOAD) continue;
        if (phdrs[i].p_vaddr < load_min) load_min = phdrs[i].p_vaddr;
        uint64_t end = phdrs[i].p_vaddr + phdrs[i].p_memsz;
        if (end > load_max) load_max = end;
    }
    if (load_max == 0) return ELF_NOT_ELF;

    size_t   total = (size_t)(load_max - load_min);
    size_t   pages = (total + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t base_phys = 0;
    for (size_t i = 0; i < pages; i++) {
        uint64_t p = pmm_alloc_any();
        if (!p) return ELF_NO_MEM;
        if (!base_phys) base_phys = p;
    }
    void *base = (void *)(uintptr_t)base_phys;
    mem_zero(base, total);

    for (int i = 0; i < hdr.e_phnum; i++) {
        elf64_phdr_t *ph = &phdrs[i];
        if (ph->p_type != PT_LOAD || ph->p_filesz == 0) continue;
        uint8_t *dest = (uint8_t *)base + (ph->p_vaddr - load_min);
        int got = node->ops->read(node, dest,
                                   (size_t)ph->p_filesz, (size_t)ph->p_offset);
        if (got < 0) return ELF_NO_MEM;
    }

    out->load_base = base;
    out->entry     = hdr.e_entry - load_min + (uint64_t)(uintptr_t)base;
    out->status    = ELF_OK;
    return ELF_OK;
}

int elf_exec(const elf_image_t *img, int argc, char **argv) {
    if (!img || img->status != ELF_OK) return -1;
    typedef int (*entry_fn_t)(int, char **);
    entry_fn_t entry = (entry_fn_t)(uintptr_t)img->entry;
    return entry(argc, argv);
}

int elf_run(const char *path, int argc, char **argv) {
    /* Resolve node directly so we can use node->ops->read with offset */
    vfs_node_t *node = vfs_resolve(path);
    if (!node) return ELF_NOT_ELF;
    if (!node->ops || !node->ops->read) return ELF_NOT_ELF;

    elf_image_t img;
    int r = elf_load(node, &img);
    if (r != ELF_OK) return r;
    return elf_exec(&img, argc, argv);
}

/* Suppress fd_pread unused warning */

