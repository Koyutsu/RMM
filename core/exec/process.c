#include "exec/timers.h"
#include "fs/locks.h"
#include "exec/process.h"
#include "exec/elf.h"
#include "exec/sched.h"
#include "fs/vfs.h"
#include "cpu/gdt.h"
#include "drivers/memory/vmm.h"
#include "console/display.h"
#include "drivers/serial/serial.h"
#include <stdint.h>
#include <stddef.h>

/* exec failure diagnostics (serial console only)
 * A failed execve after proc_free_user_pages() must never silently return
 * to the destroyed old image (the syscall tail irets to the stale user
 * RIP -> #PF on freed pages, the "flaky exec crash").  These tags mark
 * exactly which post-free step failed so the trigger can be found.     */
#define EXEC_DEBUG 1
#if EXEC_DEBUG
static void execdbg_hex(uint64_t v) {
    static const char hx[] = "0123456789abcdef";
    char buf[19];
    buf[0] = '0'; buf[1] = 'x';
    for (int i = 0; i < 16; i++) buf[2 + i] = hx[(v >> ((15 - i) * 4)) & 0xF];
    buf[18] = '\0';
    serial_print_string(buf);
}
#define EXEC_FAIL(tag, rc) do {                                          \
        serial_print_string("[execdbg] FAIL(fatal) " tag " rc=");        \
        execdbg_hex((uint64_t)(uint32_t)(rc));                           \
        serial_print_string("\n");                                       \
        /* The old image is already destroyed: returning would iret into   \
         * freed pages (the historic "flaky exec" #PF).  Kill the process  \
         * cleanly instead - the parent sees exit status 127.              \
         * At boot (proc_create of init) there is no current process:      \
         * return the error so the kernel can try its fallback paths.   */ \
        if (g_current_proc) {                                            \
            proc_exit(g_current_proc, 127);                              \
            for (;;) __asm__ volatile ("hlt");                           \
        }                                                                \
        return (rc);                                                     \
    } while (0)
#define EXEC_RET(tag, rc) do {                                           \
        serial_print_string("[execdbg] fail(pre-free) " tag " rc=");     \
        execdbg_hex((uint64_t)(uint32_t)(rc));                           \
        serial_print_string("\n");                                       \
        return (rc);                                                     \
    } while (0)
#else
#define EXEC_FAIL(tag, rc) do { (void)(tag); (void)(rc);                 \
        if (g_current_proc) proc_exit(g_current_proc, 127);              \
        for (;;) __asm__ volatile ("hlt");                               \
    } while (0)
#define EXEC_RET(tag, rc) (return (rc))
#endif

/* Temporary fork trail (serial console), matches FORK_DEBUG in
 * syscall_table.c.  Remove both when the boot path is stable. */
#define FORK_DEBUG 0
#define THREAD_DEBUG 0
#define EXIT_DEBUG 0
#if EXIT_DEBUG
static void exitdbg_dec(long v) {
    char b[24]; int i = 23; int neg = v < 0;
    unsigned long u = neg ? (unsigned long)(-v) : (unsigned long)v;
    b[--i] = '\0';
    do { b[--i] = '0' + (char)(u % 10); u /= 10; } while (u);
    if (neg) b[--i] = '-';
    serial_print_string(&b[i]);
}
#define EXITDBG(s) serial_print_string(s)
#else
#define EXITDBG(s)
#endif
#include <stdbool.h>

/* Forward declaration: EXEC_FAIL (post-free exec failure) kills the
 * current process cleanly instead of returning into freed user pages. */
void proc_exit(process_t *proc, int code);

process_t  g_proc_table[PROC_MAX];
process_t *g_current_proc = NULL;

static int g_next_pid = 1;

/* Convert a syscall frame (the `syscall`-instruction register save) into
 * a scheduler user context.  Used by fork (child inherits the parent's
 * frame with rax=0) and by in-syscall parks. */
static void ctx_from_sysframe_helper(sched_ctx_t *c,
                                     const syscall_frame_t *f) {
    c->rax = f->rax;   c->rbx = f->rbx;
    c->rcx = 0;        /* `syscall` clobbers user rcx (holds rip)     */
    c->rdx = f->rdx;   c->rsi = f->rsi;   c->rdi = f->rdi;
    c->rbp = f->rbp;
    c->r8  = f->r8;    c->r9  = f->r9;    c->r10 = f->r10;
    c->r11 = 0;        /* `syscall` clobbers user r11 (holds rflags)  */
    c->r12 = f->r12;   c->r13 = f->r13;   c->r14 = f->r14;   c->r15 = f->r15;
    c->rip = f->rcx;   c->rflags = f->r11;    c->user_rsp = f->user_rsp;
}

static void proc_memzero(void *p, size_t n) {
    uint8_t *b = (uint8_t *)p; while (n--) *b++ = 0;
}

/* shared ELF constants (now also ET_DYN / PT_INTERP / ELF32) */
    typedef struct __attribute__((packed)) {
        uint32_t e_magic; uint8_t e_class; uint8_t e_data;
        uint8_t e_version; uint8_t e_osabi; uint8_t e_abiversion;
        uint8_t e_pad[7]; uint16_t e_type; uint16_t e_machine;
        uint32_t e_version2; uint64_t e_entry; uint64_t e_phoff;
        uint64_t e_shoff; uint32_t e_flags; uint16_t e_ehsize;
        uint16_t e_phentsize; uint16_t e_phnum; uint16_t e_shentsize;
        uint16_t e_shnum; uint16_t e_shstrndx;
    } elf64_hdr_t;
    typedef struct __attribute__((packed)) {
        uint32_t p_type; uint32_t p_flags; uint64_t p_offset;
        uint64_t p_vaddr; uint64_t p_paddr; uint64_t p_filesz;
        uint64_t p_memsz; uint64_t p_align;
    } elf64_phdr_t;
    typedef struct __attribute__((packed)) {
        uint8_t  e_ident[16]; uint16_t e_type; uint16_t e_machine;
        uint32_t e_version; uint32_t e_entry; uint32_t e_phoff;
        uint32_t e_shoff; uint32_t e_flags; uint16_t e_ehsize;
        uint16_t e_phentsize; uint16_t e_phnum; uint16_t e_shentsize;
        uint16_t e_shnum; uint16_t e_shstrndx;
    } elf32_hdr_t;
    typedef struct __attribute__((packed)) {
        uint32_t p_type; uint32_t p_offset; uint32_t p_vaddr;
        uint32_t p_paddr; uint32_t p_filesz; uint32_t p_memsz;
        uint32_t p_flags; uint32_t p_align;
    } elf32_phdr_t;
    #define ELF_MAGIC    0x464C457F
    #define ELFCLASS64   2
    #define ELFCLASS32   1
    #define ET_EXEC      2
    #define ET_DYN       3
    #define EM_X86_64    62
    #define EM_386       3
    #define PT_LOAD      1
    #define PT_DYNAMIC   2
    #define PT_INTERP    3

    /* Map the ELF64 image's PT_LOAD segments at (bias + p_vaddr).
     * ET_EXEC passes bias 0; ET_DYN (PIE) passes ELF_DYN_BASE.  All
     * pages come zeroed from pmm_alloc, so p_memsz > p_filesz (BSS)
     * is correct without extra work.  The kernel can write user VAs
     * directly (address spaces are shared halves), so segment content
     * is read straight into the mapped pages with retries, the ext2/
     * ATA path can transiently return short reads under QEMU TCG. */
    static int elf_map_image64(vfs_node_t *node, elf_image_t *out,
                               uint64_t bias) {
        elf64_hdr_t hdr;
        if (node->ops->read(node, &hdr, sizeof(hdr), 0) != (int)sizeof(hdr))
            EXEC_FAIL("elf hdr-read", ELF_NOT_ELF);
        if (hdr.e_magic != ELF_MAGIC)            EXEC_FAIL("elf magic", ELF_NOT_ELF);
        if (hdr.e_class != ELFCLASS64)           EXEC_FAIL("elf class", ELF_NOT_64);
        if (hdr.e_machine != EM_X86_64)          EXEC_FAIL("elf machine", ELF_NOT_64);
        if (hdr.e_type != ET_EXEC && hdr.e_type != ET_DYN)
                                                 EXEC_FAIL("elf type", ELF_NOT_EXEC);
        if (hdr.e_entry == 0)                    EXEC_FAIL("elf entry0", ELF_NO_ENTRY);
        if (hdr.e_phentsize != sizeof(elf64_phdr_t)) EXEC_FAIL("elf phentsize", ELF_NOT_ELF);
        if (hdr.e_phnum == 0 || hdr.e_phnum > 32)    EXEC_FAIL("elf phnum", ELF_NOT_ELF);

        elf64_phdr_t phdrs[32];
        size_t phsz = (size_t)hdr.e_phnum * sizeof(elf64_phdr_t);
        if (node->ops->read(node, phdrs, phsz, (size_t)hdr.e_phoff) != (int)phsz)
            EXEC_FAIL("elf phdr-read", ELF_NOT_ELF);

        uint64_t load_end = 0;
        uint64_t phdr_va  = 0;
        for (int i = 0; i < hdr.e_phnum; i++) {
            elf64_phdr_t *ph = &phdrs[i];
            if (ph->p_type != PT_LOAD) {
                if (ph->p_type == PT_INTERP && ph->p_filesz > 1 &&
                    ph->p_filesz < sizeof(out->interp_path)) {
                    if (node->ops->read(node, out->interp_path,
                                        (size_t)ph->p_filesz,
                                        (size_t)ph->p_offset) ==
                            (int)ph->p_filesz) {
                        out->interp_path[ph->p_filesz - 1] = '\0';
                    }
                }
                continue;
            }

            uint64_t vstart = ph->p_vaddr & ~(uint64_t)(PAGE_SIZE - 1);
            uint64_t vend   = (ph->p_vaddr + ph->p_memsz + PAGE_SIZE - 1)
                              & ~(uint64_t)(PAGE_SIZE - 1);
            size_t   npages = (size_t)((vend - vstart) / PAGE_SIZE);
            if (vend > load_end) load_end = vend;

            /* user VA of the program-header array: the PT_LOAD that
             * covers file offset e_phoff (always the first one) */
            if (ph->p_offset <= hdr.e_phoff &&
                hdr.e_phoff < ph->p_offset + ph->p_filesz) {
                phdr_va = bias + ph->p_vaddr + (hdr.e_phoff - ph->p_offset);
            }

            for (size_t p = 0; p < npages; p++) {
                uint64_t phys = pmm_alloc();
                if (!phys) {
                    serial_print_string("[execdbg] FAIL elf pmm page=");
                    execdbg_hex((uint64_t)p);
                    serial_print_string("\n");
                    return ELF_NO_MEM;
                }
                uint64_t vpage = bias + vstart + p * PAGE_SIZE;
                if (!vmm_map(vpage, phys,
                             PTE_PRESENT | PTE_WRITABLE | PTE_USER)) {
                    serial_print_string("[execdbg] FAIL elf vmm_map page=");
                    execdbg_hex((uint64_t)p);
                    serial_print_string("\n");
                    return ELF_NO_MEM;
                }
            }
            if (ph->p_filesz > 0) {
                uint8_t *dest = (uint8_t *)(uintptr_t)(bias + ph->p_vaddr);
                int got = -1;
                for (int attempt = 0; attempt < 3; attempt++) {
                    got = node->ops->read(node, dest,
                                          (size_t)ph->p_filesz,
                                          (size_t)ph->p_offset);
                    if (got == (int)ph->p_filesz) break;
                }
                if (got != (int)ph->p_filesz) {
                    serial_print_string("[execdbg] FAIL elf seg-read seg=");
                    execdbg_hex((uint64_t)i);
                    serial_print_string(" filesz=");
                    execdbg_hex(ph->p_filesz);
                    serial_print_string(" off=");
                    execdbg_hex(ph->p_offset);
                    serial_print_string(" got=");
                    execdbg_hex((uint64_t)(int64_t)got);
                    serial_print_string("\n");
                    return ELF_NO_MEM;
                }
            }
        }

        out->is_dyn    = (hdr.e_type == ET_DYN);
        out->is_ia32   = false;
        out->load_bias = bias;
        out->load_base = (void *)(uintptr_t)bias;
        out->entry     = hdr.e_entry + bias;
        out->load_end  = load_end + bias;
        out->phdr_va   = phdr_va ? phdr_va : bias + hdr.e_phoff;
        out->phent     = hdr.e_phentsize;
        out->phnum     = hdr.e_phnum;
        out->status    = ELF_OK;
        return ELF_OK;
    }

    /* IA-32 (ELFCLASS32 / EM_386) static executable: the 32-bit compat
     * model - segments mapped as-linked in the low user region, 32-bit
     * stack + auxv (build_startup_stack32), CS = GDT_USER_CODE32,
     * syscalls via int 0x80.  Dynamic ELF32 (ld-musl-i386) is future
     * work, rejected here with ELF_NOT_EXEC. */
    static int elf_load_user32(vfs_node_t *node, elf_image_t *out) {
        elf32_hdr_t hdr;
        if (node->ops->read(node, &hdr, sizeof(hdr), 0) != (int)sizeof(hdr))
            EXEC_FAIL("elf32 hdr-read", ELF_NOT_ELF);
        {
            uint32_t m32 = (uint32_t)hdr.e_ident[0] |
                           ((uint32_t)hdr.e_ident[1] << 8) |
                           ((uint32_t)hdr.e_ident[2] << 16) |
                           ((uint32_t)hdr.e_ident[3] << 24);
            if (m32 != ELF_MAGIC)            EXEC_FAIL("elf32 magic", ELF_NOT_ELF);
            if (hdr.e_ident[4] != ELFCLASS32) EXEC_FAIL("elf32 class", ELF_NOT_64);
        }
        if (hdr.e_machine != EM_386)             EXEC_FAIL("elf32 machine", ELF_BAD_IA32);
        if (hdr.e_type != ET_EXEC)               EXEC_FAIL("elf32 type", ELF_NOT_EXEC);
        if (hdr.e_entry == 0)                    EXEC_FAIL("elf32 entry0", ELF_NO_ENTRY);
        if (hdr.e_phentsize != sizeof(elf32_phdr_t)) EXEC_FAIL("elf32 phentsize", ELF_NOT_ELF);
        if (hdr.e_phnum == 0 || hdr.e_phnum > 32)    EXEC_FAIL("elf32 phnum", ELF_NOT_ELF);

        elf32_phdr_t phdrs[32];
        size_t phsz = (size_t)hdr.e_phnum * sizeof(elf32_phdr_t);
        if (node->ops->read(node, phdrs, phsz, (size_t)hdr.e_phoff) != (int)phsz)
            EXEC_FAIL("elf32 phdr-read", ELF_NOT_ELF);

        uint64_t load_end = 0;
        uint64_t phdr_va  = 0;
        for (int i = 0; i < hdr.e_phnum; i++) {
            elf32_phdr_t *ph = &phdrs[i];
            if (ph->p_type != PT_LOAD) continue;

            uint64_t vstart = ph->p_vaddr & ~(uint64_t)(PAGE_SIZE - 1);
            uint64_t vend   = (ph->p_vaddr + ph->p_memsz + PAGE_SIZE - 1)
                              & ~(uint64_t)(PAGE_SIZE - 1);
            size_t   npages = (size_t)((vend - vstart) / PAGE_SIZE);
            if (vend > load_end) load_end = vend;
            if (vend > PROC_STACK_BASE) EXEC_FAIL("elf32 vaddr", ELF_NOT_EXEC);

            if (ph->p_offset <= hdr.e_phoff &&
                hdr.e_phoff < (uint64_t)ph->p_offset + ph->p_filesz) {
                phdr_va = ph->p_vaddr + (hdr.e_phoff - ph->p_offset);
            }

            for (size_t p = 0; p < npages; p++) {
                uint64_t phys = pmm_alloc();
                if (!phys) return ELF_NO_MEM;
                if (!vmm_map(vstart + p * PAGE_SIZE, phys,
                             PTE_PRESENT | PTE_WRITABLE | PTE_USER))
                    return ELF_NO_MEM;
            }
            if (ph->p_filesz > 0) {
                uint8_t *dest = (uint8_t *)(uintptr_t)ph->p_vaddr;
                int got = -1;
                for (int attempt = 0; attempt < 3; attempt++)
                    got = node->ops->read(node, dest,
                                          (size_t)ph->p_filesz,
                                          (size_t)ph->p_offset);
                if (got != (int)ph->p_filesz) return ELF_NO_MEM;
            }
        }

        out->is_dyn    = false;
        out->is_ia32   = true;
        out->load_bias = 0;
        out->load_base = (void *)(uintptr_t)0;
        out->entry     = hdr.e_entry;
        out->load_end  = load_end;
        out->phdr_va   = phdr_va;
        out->phent     = hdr.e_phentsize;
        out->phnum     = hdr.e_phnum;
        out->interp_path[0] = '\0';
        out->status    = ELF_OK;
        return ELF_OK;
    }

    static int elf_load_user(vfs_node_t *node, elf_image_t *out) {
        if (!node || !out) return ELF_NOT_ELF;
        out->load_base = NULL; out->entry = 0; out->status = ELF_NOT_ELF;
        out->is_dyn = false; out->is_ia32 = false; out->load_bias = 0;
        out->phdr_va = 0; out->phent = 0; out->phnum = 0;
        out->interp_path[0] = '\0';

        uint8_t head[20];
        if (node->ops->read(node, head, sizeof(head), 0) != (int)sizeof(head))
            EXEC_FAIL("elf head-read", ELF_NOT_ELF);
        if ((head[0] != 0x7F || head[1] != 'E' || head[2] != 'L' ||
             head[3] != 'F'))                     EXEC_FAIL("elf magic", ELF_NOT_ELF);

        uint16_t e_type    = (uint16_t)(head[16] | (head[17] << 8));
        uint16_t e_machine = (uint16_t)(head[18] | (head[19] << 8));

        if (head[4] == ELFCLASS64) {
            uint64_t bias = (e_type == ET_DYN) ? ELF_DYN_BASE : 0;
            return elf_map_image64(node, out, bias);
        }
        if (head[4] == ELFCLASS32) {
            if (e_machine != EM_386)          EXEC_FAIL("elf32 em", ELF_BAD_IA32);
            if (e_type != ET_EXEC)            EXEC_FAIL("elf32 dyn", ELF_NOT_EXEC);
            return elf_load_user32(node, out);
        }
        EXEC_FAIL("elf class", ELF_NOT_ELF);
    }

/*
 *  Linux x86-64 ABI startup stack builder
 *
 *  Layout (top of stack = PROC_STACK_TOP, grows down):
 *      env/argv strings
 *      16 random bytes (AT_RANDOM)
 *      padding to 16
 *      auxv pairs (AT_* ... AT_NULL)
 *      NULL + envp pointers
 *      NULL + argv pointers
 *      argc                        <- new RSP (16-byte aligned)
 *
 *  musl's _start reads argc/argv/envp from the stack and scans auxv right
 *  after envp's NULL, so this layout must be exact.
 */
static void stk_push64(uint8_t **sp, uint64_t v) {
    *sp -= 8;
    *(uint64_t *)(uintptr_t)*sp = v;
}

/* extra auxv entries the dynamic linker (and any conforming
 * runtime) needs to bootstrap.  base/entry are the AT_BASE/AT_ENTRY
 * values (0/0 for plain static images - Linux also emits AT_BASE for
 * static binaries, musl ignores it there). */
typedef struct {
    uint64_t phdr_va, phent, phnum;   /* AT_PHDR / AT_PHENT / AT_PHNUM */
    uint64_t base;                    /* AT_BASE - interpreter bias    */
    uint64_t entry;                   /* AT_ENTRY - main image entry   */
} startup_auxv_t;

static uint64_t build_startup_stack(char **argv, int argc,
                                    char **envp, int envc,
                                    const startup_auxv_t *ax) {
    uint8_t *sp = (uint8_t *)(uintptr_t)PROC_STACK_TOP;
    char *argv_va[64];
    char *envp_va[128];

    if (argc > 64)  argc = 64;
    if (envc > 128) envc = 128;

    /* strings (push reversed so they read in order) */
    for (int i = envc - 1; i >= 0; i--) {
        const char *s = envp ? envp[i] : "";
        size_t slen = 0; while (s[slen]) slen++; slen++;
        sp -= slen;
        for (size_t j = 0; j < slen; j++) sp[j] = (uint8_t)s[j];
        envp_va[i] = (char *)(uintptr_t)sp;
    }
    for (int i = argc - 1; i >= 0; i--) {
        const char *s = argv ? argv[i] : "";
        size_t slen = 0; while (s[slen]) slen++; slen++;
        sp -= slen;
        for (size_t j = 0; j < slen; j++) sp[j] = (uint8_t)s[j];
        argv_va[i] = (char *)(uintptr_t)sp;
    }

    /* 16 random bytes for AT_RANDOM (PIT-tick derived xorshift) */
    extern uint64_t pit_ticks(void);
    uint64_t rnd = pit_ticks() * 2654435761ULL + 0x9E3779B97F4A7C15ULL;
    rnd ^= rnd << 13; rnd ^= rnd >> 7; rnd ^= rnd << 17;
    sp -= 16;
    ((uint64_t *)(uintptr_t)sp)[0] = rnd;
    ((uint64_t *)(uintptr_t)sp)[1] = ~rnd;
    uint64_t random_va = (uint64_t)(uintptr_t)sp;

    /* align */
    sp = (uint8_t *)((uintptr_t)sp & ~15ULL);

    /* alignment pad
     * rule (still true): the pad lives below the auxv block, never
     * between argc and argv.  27 auxv slots + (envc+1) + (argc+1) + 1. */
    if (((argc + envc) & 1) == 0)
        stk_push64(&sp, 0);

    /* auxv (pairs of type/value, order irrelevant except AT_NULL) */
    stk_push64(&sp, 0);            stk_push64(&sp, AT_NULL);       /* terminator */
    stk_push64(&sp, random_va);    stk_push64(&sp, AT_RANDOM);
    stk_push64(&sp, (uint64_t)(uintptr_t)argv_va[0]); stk_push64(&sp, AT_EXECFN);
    stk_push64(&sp, 0);            stk_push64(&sp, AT_SECURE);
    stk_push64(&sp, 0);            stk_push64(&sp, AT_HWCAP);
    stk_push64(&sp, 0);            stk_push64(&sp, AT_HWCAP2);
    stk_push64(&sp, 100);          stk_push64(&sp, AT_CLKTCK);
    stk_push64(&sp, 0);            stk_push64(&sp, AT_EGID);
    stk_push64(&sp, 0);            stk_push64(&sp, AT_GID);
    stk_push64(&sp, 0);            stk_push64(&sp, AT_EUID);
    stk_push64(&sp, 0);            stk_push64(&sp, AT_UID);
    stk_push64(&sp, 4096);         stk_push64(&sp, AT_PAGESZ);
    /* dynamic-linking auxv - AT_PHDR points at the program
     * header array inside the mapped image (the loader computed the
     * covering PT_LOAD), AT_BASE is where ld-musl was mapped, AT_ENTRY
     * the main image's entry so the interpreter can jump to it after
     * linking.  AT_PHENT = byte size of one Elf64_Phdr (56). */
    if (ax) {
        stk_push64(&sp, ax->entry);   stk_push64(&sp, AT_ENTRY);
        stk_push64(&sp, ax->base);    stk_push64(&sp, AT_BASE);
        stk_push64(&sp, ax->phdr_va); stk_push64(&sp, AT_PHDR);
        stk_push64(&sp, ax->phent);   stk_push64(&sp, AT_PHENT);
        stk_push64(&sp, ax->phnum);   stk_push64(&sp, AT_PHNUM);
    }

    /* envp */
    stk_push64(&sp, 0);                                   /* NULL terminator */
    for (int i = envc - 1; i >= 0; i--)
        stk_push64(&sp, (uint64_t)(uintptr_t)envp_va[i]);

    /* argv */
    stk_push64(&sp, 0);                                   /* NULL terminator */
    for (int i = argc - 1; i >= 0; i--)
        stk_push64(&sp, (uint64_t)(uintptr_t)argv_va[i]);

    /* argc
     * argc sits exactly at the final rsp, the argv array follows it
     * Contiguously (musl reads argv = rsp+8, envp = argv+argc+1).
     * 16-byte alignment of rsp itself is achieved by the pad below the
     * auxv above - never by inserting anything between argc and argv. */
    stk_push64(&sp, (uint64_t)(uint32_t)argc);
    /* sp now points exactly at argc */
    return (uint64_t)(uintptr_t)sp;
}

/* IA-32 startup stack, same layout with 32-bit words
 * i386 SysV: argc at [esp], argv follows, envp after argv's NULL, auxv
 * after envp's NULL.  All pointers/values are 4 bytes.  Final rsp is
 * 16-byte aligned via pad slots pushed below the auxv (never between
 * argc and argv). */
static void stk_push32(uint8_t **sp, uint32_t v) {
    *sp -= 4;
    *(uint32_t *)(uintptr_t)*sp = v;
}

static uint64_t build_startup_stack32(char **argv, int argc,
                                      char **envp, int envc,
                                      const startup_auxv_t *ax) {
    uint8_t *sp = (uint8_t *)(uintptr_t)PROC_STACK_TOP;
    uint32_t argv_va[64];
    uint32_t envp_va[128];

    if (argc > 64)  argc = 64;
    if (envc > 128) envc = 128;

    for (int i = envc - 1; i >= 0; i--) {
        const char *s = envp ? envp[i] : "";
        size_t slen = 0; while (s[slen]) slen++; slen++;
        sp -= slen;
        for (size_t j = 0; j < slen; j++) sp[j] = (uint8_t)s[j];
        envp_va[i] = (uint32_t)(uintptr_t)sp;
    }
    for (int i = argc - 1; i >= 0; i--) {
        const char *s = argv ? argv[i] : "";
        size_t slen = 0; while (s[slen]) slen++; slen++;
        sp -= slen;
        for (size_t j = 0; j < slen; j++) sp[j] = (uint8_t)s[j];
        argv_va[i] = (uint32_t)(uintptr_t)sp;
    }

    extern uint64_t pit_ticks(void);
    uint64_t rnd = pit_ticks() * 2654435761ULL + 0x9E3779B97F4A7C15ULL;
    rnd ^= rnd << 13; rnd ^= rnd >> 7; rnd ^= rnd << 17;
    sp -= 16;
    ((uint32_t *)(uintptr_t)sp)[0] = (uint32_t)rnd;
    ((uint32_t *)(uintptr_t)sp)[1] = (uint32_t)(rnd >> 32);
    ((uint32_t *)(uintptr_t)sp)[2] = (uint32_t)~rnd;
    ((uint32_t *)(uintptr_t)sp)[3] = (uint32_t)(~rnd >> 32);
    uint32_t random_va = (uint32_t)(uintptr_t)sp;

    /* deterministic alignment: after aligning sp, the pointer block is
     * 28 auxv slots + (envc+1) + (argc+1) + 1 words, pad to 16 bytes */
    sp = (uint8_t *)((uintptr_t)sp & ~15ULL);
    size_t slots = 28u + (size_t)argc + 1u + (size_t)envc + 1u + 1u;
    size_t bytes = slots * 4u;
    size_t pad   = (bytes & 15u) ? (16u - (bytes & 15u)) / 4u : 0u;
    for (size_t i = 0; i < pad; i++) stk_push32(&sp, 0);

    stk_push32(&sp, 0);            stk_push32(&sp, AT_NULL);
    stk_push32(&sp, random_va);    stk_push32(&sp, AT_RANDOM);
    stk_push32(&sp, argv_va[0]);   stk_push32(&sp, AT_EXECFN);
    stk_push32(&sp, 0);            stk_push32(&sp, AT_SECURE);
    stk_push32(&sp, 0);            stk_push32(&sp, AT_HWCAP);
    stk_push32(&sp, 0);            stk_push32(&sp, AT_HWCAP2);
    stk_push32(&sp, 100);          stk_push32(&sp, AT_CLKTCK);
    stk_push32(&sp, 0);            stk_push32(&sp, AT_EGID);
    stk_push32(&sp, 0);            stk_push32(&sp, AT_GID);
    stk_push32(&sp, 0);            stk_push32(&sp, AT_EUID);
    stk_push32(&sp, 0);            stk_push32(&sp, AT_UID);
    stk_push32(&sp, 4096);         stk_push32(&sp, AT_PAGESZ);
    if (ax) {
        stk_push32(&sp, (uint32_t)ax->entry);   stk_push32(&sp, AT_ENTRY);
        stk_push32(&sp, (uint32_t)ax->base);    stk_push32(&sp, AT_BASE);
        stk_push32(&sp, (uint32_t)ax->phdr_va); stk_push32(&sp, AT_PHDR);
        stk_push32(&sp, (uint32_t)ax->phent);   stk_push32(&sp, AT_PHENT);
        stk_push32(&sp, (uint32_t)ax->phnum);   stk_push32(&sp, AT_PHNUM);
    }

    stk_push32(&sp, 0);
    for (int i = envc - 1; i >= 0; i--) stk_push32(&sp, envp_va[i]);

    stk_push32(&sp, 0);
    for (int i = argc - 1; i >= 0; i--) stk_push32(&sp, argv_va[i]);

    stk_push32(&sp, (uint32_t)argc);
    return (uint64_t)(uintptr_t)sp;
}

int proc_create(process_t *proc, const char *path,
                int argc, char **argv, char **envp) {
#ifdef DEBUG_ELF
    display_printf("proc_create: path=%s argc=%d\n", path, argc);
#endif
    /* proc_alloc() already assigned our pid, memzero would wipe it. */
    int keep_pid = proc->pid;
    proc_memzero(proc, sizeof(process_t));
    proc->pid = keep_pid;

    /* memzero also wipes the credential defaults set by proc_alloc -
     * re-apply them (root identity, umask 0022, root supplementary group).
     * Without this, PID 1 runs with umask 0 and files created by the
     * boot session land world-writable. */
    proc->umask   = 0022;
    proc->ngroups = 1;
    proc->groups[0] = 0;

    vfs_node_t *node = vfs_resolve(path);
#ifdef DEBUG_ELF
    display_printf("proc_create: node=%p\n", (void*)node);
#endif
    if (!node || !node->ops || !node->ops->read) return ELF_NOT_ELF;

#ifdef DEBUG_ELF
    display_puts("proc_create: calling elf_load_user\n");
#endif
    int r = elf_load_user(node, &proc->image);
    if (r != ELF_OK) return r;

    for (int i = 0; i < PROC_STACK_PAGES; i++) {
        uint64_t phys = pmm_alloc();
        if (!phys) return ELF_NO_MEM;
        uint64_t virt = PROC_STACK_BASE + (uint64_t)i * PAGE_SIZE;
        if (!vmm_map(virt, phys, PTE_PRESENT | PTE_WRITABLE | PTE_USER))
            return ELF_NO_MEM;
    }

    /* the kernel stack must be one contiguous physical block
     * (kstack_top = base + PROC_KSTACK_SIZE and every IRQ/syscall/park
     * frame lives inside that identity range).  Four independent
     * pmm_alloc() calls return scattered pages once the bitmap
     * fragments - the computed stack then overlaps other tasks' live
     * Stacks (observed: login over init -> parked-chain clobber ->
     * ring-0 #UD).  pmm_alloc_contig also zeroes the block. */
    uint64_t kstack_base = pmm_alloc_contig(PROC_KSTACK_PAGES);
    if (!kstack_base) return ELF_NO_MEM;
    proc->kstack_base = kstack_base;
    proc->kstack_top  = kstack_base + PROC_KSTACK_SIZE;

    proc->entry      = proc->image.entry;
    proc->heap_start = proc->image.load_end;
    proc->heap_brk   = proc->image.load_end;

    /* per-task code segment + identity (64-bit by default,
     * ia32 execve rewrites both).  Identity = argv[0] basename until
     * prctl(PR_SET_NAME) overrides it. */
    proc->user_cs = (uint64_t)(GDT_USER_CODE | 3);
    proc->is_ia32 = false;
    {
        const char *b = path;
        for (const char *q = path; *q; q++) if (*q == '/') b = q + 1;
        size_t i = 0;
        for (; b[i] && i < sizeof(proc->comm) - 1; i++) proc->comm[i] = b[i];
        proc->comm[i] = '\0';
    }
    {
        size_t i = 0;
        for (; path[i] && i < sizeof(proc->exec_path) - 1; i++)
            proc->exec_path[i] = path[i];
        proc->exec_path[i] = '\0';
    }

    /* Build Linux x86-64 ABI startup stack (argc/argv/envp/auxv) */
    int envc = 0;
    if (envp) while (envp[envc]) envc++;
    startup_auxv_t ax = { 0 };
    ax.phdr_va = proc->image.phdr_va;
    ax.phent   = proc->image.phent;
    ax.phnum   = proc->image.phnum;
    ax.entry   = proc->image.entry;
    ax.base    = 0;                     /* boot images are static      */
    proc->user_rsp = build_startup_stack(argv, argc, envp, envc, &ax);

    proc->argc     = argc;
    proc->state    = PROC_STATE_READY;
    proc->mmap_bump = PROC_MMAP_BASE;
    proc->cwd[0] = '/';
    proc->cwd[1] = '\0';

    /* every task owns one fd-table reference from birth. */
    proc->tgid      = proc->pid;
    proc->is_thread = false;
    proc->clear_child_tid = 0;
    proc->fdt = vfs_fdt_new();
    if (proc->fdt) vfs_fd_save(proc->fdt->t);

    return ELF_OK;
}

/*
 *  Parent-callee-saved preservation
 *
 *  proc_exec() "returns" from inside the child's exit path via a longjmp-style
 *  jump to its label 0.  Only RAX/RSP/RBP/RBX are defined at that point -
 *  R12-R15 still hold the child's user values.  Callers (proc_fork,
 *  proc_waitpid) must save/restore their callee-saved registers around the
 *  call, these helpers do exactly that.
 */
__attribute__((unused)) /* legacy: kept for the dormant inline-run path */
static void callee_regs_save(uint64_t sv[5]) {
    __asm__ volatile (
        "movq %%rbx, %0\n\t"
        "movq %%r12, %1\n\t"
        "movq %%r13, %2\n\t"
        "movq %%r14, %3\n\t"
        "movq %%r15, %4"
        : "=m"(sv[0]), "=m"(sv[1]), "=m"(sv[2]), "=m"(sv[3]), "=m"(sv[4])
    );
}
__attribute__((unused))
static void callee_regs_restore(uint64_t sv[5]) {
    __asm__ volatile (
        "movq %0, %%rbx\n\t"
        "movq %1, %%r12\n\t"
        "movq %2, %%r13\n\t"
        "movq %3, %%r14\n\t"
        "movq %4, %%r15"
        :: "m"(sv[0]), "m"(sv[1]), "m"(sv[2]), "m"(sv[3]), "m"(sv[4])
        : "memory"
    );
}

/*
 * Run a process that resumes from a fork: restore all user registers from
 * proc->fork_frame (rax already 0 for the child) and iretq.
 */
__attribute__((noinline))
static int proc_exec_forked(process_t *proc) {
    proc->state = PROC_STATE_RUNNING;
    g_current_proc = proc;

    /* Activate the child's private address space
     * Everything below (iretq frame, user resumes, child syscalls, the
     * child's execve) runs against the child's page tree.  The parent's
     * root is restored right after the asm block, on the label-0 path
     * (i.e. once the child has exited and control returned here).       */
    uint64_t parent_cr3 = vmm_get_cr3();
    if (proc->cr3) vmm_switch_root(proc->cr3);

#if FORK_DEBUG
    display_printf("[fd] resume: pid=%d rip=0x%lx rsp=0x%lx fs=0x%lx\n",
                   proc->pid, (unsigned long)proc->fork_frame.rcx,
                   (unsigned long)proc->fork_frame.user_rsp,
                   (unsigned long)proc->fs_base);
#endif

    if (proc->fdt) vfs_fd_restore(proc->fdt->t);

    gdt_set_tss_rsp0(proc->kstack_top);
    extern uint64_t syscall_ksp;
    syscall_ksp = proc->kstack_top;

    proc->iretq_ss = (uint64_t)(GDT_USER_DATA | 3);
    proc->iretq_cs = (uint64_t)(GDT_USER_CODE | 3);

    syscall_frame_t *f = &proc->fork_frame;

    __asm__ volatile (
        "lea   0f(%%rip), %%rax\n\t"
        "mov   %%rax, %c[ret_rip](%[p])\n\t"
        "mov   %%rsp, %c[ret_rsp](%[p])\n\t"
        "mov   %%rbp, %c[ret_rbp](%[p])\n\t"

        "mov   %c[iretq_ss](%[p]), %%rax\n\t"
        "mov   %%ax, %%ds\n\t"
        "mov   %%ax, %%es\n\t"
        "mov   %%ax, %%fs\n\t"
        /* RMM V2: never reload GS (selector load sets GS.base from the
         * flat GDT descriptor = 0, destroying the per-CPU base).       */

        /* Restore the child's TLS pointer (IA32_FS_BASE)
         * The selector loads above reset the FS-base MSR to the GDT
         * descriptor base (0).  Write back the value captured at fork
         * time.  rax/rdx are scratch here, both are reloaded from the
         * frame further down before the iretq.                            */
        "mov   %c[fsbase](%[p]), %%rax\n\t"
        "mov   %%rax, %%rdx\n\t"
        "shr   $32, %%rdx\n\t"
        "mov   $0xC0000100, %%ecx\n\t"
        "wrmsr\n\t"

        /* iretq frame (RIP on top)
         * Note: [p] (rbx) addressing must be done before user rbx is
         * restored, and the user rbx value is loaded straight from the
         * frame - never via push/pop (a pop here would steal the iretq
         * frame's RIP slot and corrupt the return).                     */
        "pushq %c[iretq_ss](%[p])\n\t"   /* SS                            */
        "pushq 120(%%rsi)\n\t"           /* user RSP                      */
        "pushq 104(%%rsi)\n\t"           /* user RFLAGS (frame->r11)      */
        "pushq %c[iretq_cs](%[p])\n\t"   /* CS                            */
        "pushq 112(%%rsi)\n\t"           /* user RIP   (frame->rcx)       */

        /* restore GP registers from the frame (rsi = frame pointer) */
        "mov    0(%%rsi), %%r9\n\t"
        "mov    8(%%rsi), %%r8\n\t"
        "mov   16(%%rsi), %%r10\n\t"
        "mov   24(%%rsi), %%rdx\n\t"
        "mov   40(%%rsi), %%rdi\n\t"
        "mov   56(%%rsi), %%r15\n\t"
        "mov   64(%%rsi), %%r14\n\t"
        "mov   72(%%rsi), %%r13\n\t"
        "mov   80(%%rsi), %%r12\n\t"
        "mov   96(%%rsi), %%rbp\n\t"
        "mov   48(%%rsi), %%rax\n\t"     /* syscall return (0 in child)  */
        "mov   88(%%rsi), %%rbx\n\t"     /* user rbx (frees [p])         */
        "mov   32(%%rsi), %%rsi\n\t"     /* user rsi - frame ptr freed   */

        "iretq\n\t"

        "0:\n\t"
        "mov   $%c[kds], %%cx\n\t"
        "mov   %%cx, %%ds\n\t"
        "mov   %%cx, %%es\n\t"
        "mov   %%cx, %%fs\n\t"
        "sti\n\t"
        "mov   %%eax, %c[exit_code](%[p])\n\t"

        :
        : [p]           "b" (proc),
          [f]           "S" (f),
          [ret_rip]     "i" (offsetof(process_t, ret_rip)),
          [ret_rsp]     "i" (offsetof(process_t, ret_rsp)),
          [ret_rbp]     "i" (offsetof(process_t, ret_rbp)),
          [iretq_ss]    "i" (offsetof(process_t, iretq_ss)),
          [iretq_cs]    "i" (offsetof(process_t, iretq_cs)),
          [fsbase]      "i" (offsetof(process_t, fs_base)),
          [exit_code]   "i" (offsetof(process_t, exit_code_tmp)),
          [kds]         "i" (GDT_KERNEL_DATA)
        : "rax", "rcx", "rdx", "rdi",
          "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15",
          "memory"
    );

    proc->state    = PROC_STATE_ZOMBIE;
    g_current_proc = NULL;

    /* Back in the parent's kernel continuation, restore its address
     * space before touching anything that lives in user-relevant VAs.  */
    if (proc->cr3) vmm_switch_root(parent_cr3);

#if FORK_DEBUG
    display_printf("[fd] child exit: code=%d\n", proc->exit_code_tmp);
#endif

    return proc->exit_code_tmp;
}

__attribute__((noinline))
int proc_exec(process_t *proc) {
    /* Forked children resume their full register state instead of a fresh
     * argc/argv entry. */
    if (proc->has_fork_frame)
        return proc_exec_forked(proc);

    proc->state = PROC_STATE_RUNNING;
    g_current_proc = proc;

    if (proc->fdt) vfs_fd_restore(proc->fdt->t);

    gdt_set_tss_rsp0(proc->kstack_top);
    extern uint64_t syscall_ksp;
    syscall_ksp = proc->kstack_top;

    proc->iretq_ss     = (uint64_t)(GDT_USER_DATA | 3);
    proc->iretq_cs     = (uint64_t)(GDT_USER_CODE | 3);
    proc->iretq_rsp    = proc->user_rsp;
    proc->iretq_rip    = proc->entry;
    proc->iretq_rflags = 0x202ULL;

    int exit_code;

#ifdef DEBUG_PROC
    display_printf("proc_exec: entry=0x%x rsp=0x%x heap=0x%x ksp=0x%x\n",
           (unsigned)proc->entry, (unsigned)proc->user_rsp,
           (unsigned)proc->heap_brk, (unsigned)syscall_ksp);
#endif

    __asm__ volatile (
        "lea   0f(%%rip), %%rax\n\t"
        "mov   %%rax, %c[ret_rip](%[p])\n\t"
        "mov   %%rsp, %c[ret_rsp](%[p])\n\t"
        "mov   %%rbp, %c[ret_rbp](%[p])\n\t"

        "mov   %c[iretq_ss](%[p]), %%rax\n\t"
        "mov   %%ax, %%ds\n\t"
        "mov   %%ax, %%es\n\t"
        "mov   %%ax, %%fs\n\t"

        "andq  $-16, %%rsp\n\t"
        "sub   $8, %%rsp\n\t"

        "pushq %c[iretq_ss](%[p])\n\t"
        "pushq %c[iretq_rsp](%[p])\n\t"
        "pushq %c[iretq_rflags](%[p])\n\t"
        "pushq %c[iretq_cs](%[p])\n\t"
        "pushq %c[iretq_rip](%[p])\n\t"

        "mov   %c[argc](%[p]), %%edi\n\t"
        "mov   %c[argv](%[p]), %%rsi\n\t"
        "mov   %c[envp](%[p]), %%rdx\n\t"

        "xor   %%eax, %%eax\n\t"
        "xor   %%ecx, %%ecx\n\t"
        "xor   %%r8d, %%r8d\n\t"
        "xor   %%r9d, %%r9d\n\t"
        "xor   %%r10d,%%r10d\n\t"
        "xor   %%r11d,%%r11d\n\t"
        "xor   %%r12d,%%r12d\n\t"
        "xor   %%r13d,%%r13d\n\t"
        "xor   %%r14d,%%r14d\n\t"
        "xor   %%r15d,%%r15d\n\t"
        "xor   %%ebp, %%ebp\n\t"

        "iretq\n\t"

        "0:\n\t"
        "mov   $%c[kds], %%cx\n\t"
        "mov   %%cx, %%ds\n\t"
        "mov   %%cx, %%es\n\t"
        "mov   %%cx, %%fs\n\t"
        "sti\n\t"
        "mov   %%eax, %c[exit_code](%[p])\n\t"

        :
        : [p]           "b" (proc),
          [ret_rip]     "i" (offsetof(process_t, ret_rip)),
          [ret_rsp]     "i" (offsetof(process_t, ret_rsp)),
          [ret_rbp]     "i" (offsetof(process_t, ret_rbp)),
          [iretq_ss]    "i" (offsetof(process_t, iretq_ss)),
          [iretq_rsp]   "i" (offsetof(process_t, iretq_rsp)),
          [iretq_rflags]"i" (offsetof(process_t, iretq_rflags)),
          [iretq_cs]    "i" (offsetof(process_t, iretq_cs)),
          [iretq_rip]   "i" (offsetof(process_t, iretq_rip)),
          [argc]        "i" (offsetof(process_t, argc)),
          [argv]        "i" (offsetof(process_t, argv)),
          [envp]        "i" (offsetof(process_t, envp)),
          [exit_code]   "i" (offsetof(process_t, exit_code_tmp)),
          [kds]         "i" (GDT_KERNEL_DATA)
        : "rax", "rcx", "rdx", "rsi", "rdi",
          "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15",
          "memory"
    );

    proc->state    = PROC_STATE_ZOMBIE;
    g_current_proc = NULL;

    exit_code = proc->exit_code_tmp;
    return exit_code;
}

process_t *proc_alloc(void) {
    for (int i = 0; i < PROC_MAX; i++) {
        if (g_proc_table[i].state == PROC_STATE_EMPTY) {
            proc_memzero(&g_proc_table[i], sizeof(process_t));
            g_proc_table[i].pid = g_next_pid++;
            /* Credentials: memzero already yields uid/euid/suid 0 (root),
             * which is what PID 1 must run as.  Set the parts that are not
             * "0 = unset" explicitly. */
            g_proc_table[i].umask   = 0022;
            g_proc_table[i].ngroups = 1;
            g_proc_table[i].groups[0] = 0;   /* root group */
            return &g_proc_table[i];
        }
    }
    return NULL;
}

/* Credential accessors used by the VFS permission layer
 * vfs.c must not depend on exec/process.h (fs/vfs.h is included from it),
 * so the permission layer calls these hooks instead of reading
 * g_current_proc directly. */
uint32_t vfs_cred_euid(void) {
    return g_current_proc ? g_current_proc->euid : 0;
}
uint32_t vfs_cred_egid(void) {
    return g_current_proc ? g_current_proc->egid : 0;
}
bool vfs_cred_is_root(void) {
    return !g_current_proc || g_current_proc->euid == 0;
}
bool vfs_cred_in_group(uint32_t gid) {
    if (!g_current_proc) return true;
    if (g_current_proc->egid == gid) return true;
    for (int i = 0; i < g_current_proc->ngroups &&
                    i < PROC_NGROUPS_MAX; i++)
        if (g_current_proc->groups[i] == gid) return true;
    return false;
}
uint32_t vfs_cred_umask(void) {
    return g_current_proc ? g_current_proc->umask : 0022;
}

void proc_free(process_t *proc) {
    proc->state = PROC_STATE_EMPTY;
}

static void proc_free_user_pages(void) {
    for (uint64_t va = PROC_USER_VA_START;
         va < PROC_USER_VA_END;
         va += PAGE_SIZE)
    {
        uint64_t phys = vmm_virt_to_phys(va);
        if (!phys) continue;
        if (phys == va) continue;
        /* r5: COW-aware release.  vmm_cow_release_page decrements
         * the share count of COW frames (freeing only on the last share),
 * frees private frames outright, and never frees memfd-owned
         * MAP_SHARED frames.  It also unmaps va. */
        vmm_cow_release_page(va);
    }
}

#define FORK_MAX_PAGES  ((PROC_USER_VA_END - PROC_USER_VA_START) / PAGE_SIZE)

typedef struct { uint64_t va; uint64_t parent_phys; uint64_t child_phys; } page_map_t;
/* fork_map backs the eager fork path only (~1.2 MB of BSS); the COW
 * path needs none of it. */
#if FORK_EAGER
static page_map_t fork_map[FORK_MAX_PAGES];
#else
#define fork_map ((page_map_t *)0)   /* never dereferenced: nmap == 0 */
#endif

/* IA32_FS_BASE - musl keeps its TLS self-pointer here. */
#define MSR_IA32_FS_BASE 0xC0000100u

static uint64_t proc_rdmsr_fs(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(MSR_IA32_FS_BASE));
    return ((uint64_t)hi << 32) | lo;
}

static void proc_wrmsr_fs(uint64_t val) {
    __asm__ volatile("wrmsr" :: "a"((uint32_t)val),
                     "d"((uint32_t)(val >> 32)), "c"(MSR_IA32_FS_BASE));
}

int proc_fork(syscall_frame_t *frame) {
    process_t *child = proc_alloc();
    if (!child) return -12;
#if FORK_DEBUG
    display_printf("[fd] fork step1: alloc ok pid=%d\n", child->pid);
#endif

    /* COW fork
     * Phase 1: flip the parent's writable user PTEs to read-only + COW
     * and take one refcount per frame.  No page is copied here at all.
     *
     * After this point the parent's data pages are write-protected:
     * the first write (user OR kernel mode, read() into a COW buffer,
     * signal frame, etc.) raises #PF, and vmm_handle_fault() breaks the
     * COW copy transparently.  The old eager path survives as a debug
     * fallback under FORK_EAGER for A/B testing.                      */
#if FORK_EAGER
    size_t nmap = 0;
    for (uint64_t va = PROC_USER_VA_START;
         va < PROC_USER_VA_END && nmap < FORK_MAX_PAGES;
         va += PAGE_SIZE)
    {
        uint64_t parent_phys = vmm_virt_to_phys(va);
        if (!parent_phys) continue;
        if (parent_phys == va) continue;

        uint64_t child_phys = pmm_alloc();
        if (!child_phys) { proc_free(child); return -12; }

        uint8_t *src = (uint8_t *)(uintptr_t)parent_phys;
        uint8_t *dst = (uint8_t *)(uintptr_t)child_phys;
        for (size_t b = 0; b < PAGE_SIZE; b++) dst[b] = src[b];

        fork_map[nmap].va          = va;
        fork_map[nmap].parent_phys = parent_phys;
        fork_map[nmap].child_phys  = child_phys;
        nmap++;
    }
#else
    {
        size_t nflip = 0;
        /* NULL arrays: we only need the side effect (flip + refcount). */
        if (!vmm_cow_flip_parent(NULL, NULL, (size_t)-1, &nflip)) {
            proc_free(child);
            return -12;
        }
#if FORK_DEBUG
        display_printf("[fd] cow flip: %zu pages RO+COW\n", (unsigned long)nflip);
#endif
    }
    size_t nmap = 0;   /* COW path copies no pages */
#endif
#if FORK_DEBUG
    display_printf("[fd] fork step2: pages copied nmap=%zu\n", nmap);
#endif

    int child_pid = child->pid;
    *child = *g_current_proc;
    child->pid        = child_pid;
    child->parent_pid = g_current_proc->pid;
    child->state      = PROC_STATE_READY;
    /* fork creates a new thread group, the struct copy above
     * brought the parent's fdt pointer along, replace it with a fresh
     * table (fork semantics: inherited fds, independent slots). */
    child->tgid            = child_pid;
    child->is_thread       = false;
    child->clear_child_tid = 0;
    child->fdt             = NULL;
    /* FD inheritance: snapshot the live fd table, not a stale copy
     * The parent may have opened/dup2'd fds since its last save (e.g. dash
     * setting up redirections before fork).  The live table is the truth. */
    child->fdt = vfs_fdt_new();
    if (child->fdt) {
        vfs_fd_save(child->fdt->t);
        vfs_pipe_refs_bump(child->fdt->t);   /* own refs to pipe ends */
    }
    else child->fdt = vfs_fdt_share(g_current_proc->fdt);  /* degraded */

    child->mmap_bump = g_current_proc->mmap_bump;
    for (int i = 0; i < VFS_PATH_MAX; i++) child->cwd[i] = g_current_proc->cwd[i];

    /* contiguous kstack block, see proc_create for the full
     * rationale (scattered pages stacked over other tasks' live stacks). */
    uint64_t kstack_base = pmm_alloc_contig(PROC_KSTACK_PAGES);
    if (!kstack_base) { proc_free(child); return -12; }
    child->kstack_base = kstack_base;
    child->kstack_top  = kstack_base + PROC_KSTACK_SIZE;

    /* Private address space: deep-copy the user-half table tree
     * COW path: nmap == 0, so the child's leaf PTEs come over verbatim
     * from the (now read-only+COW) parent tables, same frames, no data
     * copied.  FORK_EAGER path: leaves are rewritten to the fresh copies.
     * The kernel half of the tree is shared by reference in both cases.
     * This is what makes a child's execve safe: it destroys only the
     * Child's mappings, never the parent's.                              */
    uint64_t child_cr3 = vmm_fork_address_space((uint64_t *)fork_map, nmap);
    if (!child_cr3) {
#if FORK_EAGER
        for (size_t i = 0; i < nmap; i++) pmm_free(fork_map[i].child_phys);
#else
        /* Undo the COW flips: restore W, clear COW, drop refcounts. */
        vmm_cow_rollback_parent();
#endif
        for (int i = 0; i < PROC_KSTACK_PAGES; i++)
            pmm_free(kstack_base + (uint64_t)i * PAGE_SIZE);
        proc_free(child);
        return -12;
    }
    child->cr3 = child_cr3;

    /* Full register copy: child resumes after the syscall, rax = 0 */
    child->fork_frame     = *frame;
    child->fork_frame.rax = 0;          /* fork() returns 0 in the child  */
    child->has_fork_frame = 1;

    /* Preserve the TLS pointer
     * proc_exec_forked loads segment selectors (which resets the FS base
     * MSR to the GDT descriptor base, i.e. 0) before the iretq.  musl's
     * very first instructions in the child read %fs:0 to find its pthread
     * struct - without this save/restore the child dereferences linear
     * address 0 and dies.  Grab the parent's (== child's) FS base now,
     * while it is still live in the MSR. */
    child->fs_base = proc_rdmsr_fs();

    /* (BORE): the child is enqueued, not run inline
     * The parent returns from fork() immediately, the child becomes a
     * READY task with a saved user context (rax=0) that the scheduler
     * will dispatch like any other.  This is what makes background jobs
     * and pipelines actually time-share now. */
    ctx_from_sysframe_helper(&child->sched_ctx, frame);
    child->sched_ctx.rax   = 0;            /* fork() returns 0 in child   */
    child->sched_ctx_valid = true;
    child->state           = PROC_STATE_READY;

    sched_fork_inherit(child);             /* BORE vruntime/score inherit */

#if FORK_DEBUG
    serial_print_string("[fd] fork: child pid queued, parent continues\n");
#endif

    return child_pid;
}

int proc_execve(const char *path, char *const argv[], char *const envp[],
                syscall_frame_t *frame) {
    /* Snapshot path/argv/envp before touching user memory
     * User VAs share the kernel address space, and the caller's strings
     * live in pages this function is about to free and reuse for the new
     * image.  Reading them after proc_free_user_pages() faults (or worse,
     * silently reads freed memory).  Linux does the same thing via
     * copy_from_user() in do_execve().  Static buffers are safe here:
     * the cooperative kernel never runs two syscalls concurrently.      */
    static char        kpath[VFS_PATH_MAX];
    static char        kargbuf[2048];
    static const char *kargv[64];
    static char        kenvbuf[2048];
    static const char *kenvp[128];
    /* shebang scratch (interpreter / optional arg / script path) */
    static char        s_interp[256];
    static char        s_arg[256];
    static char        s_script[VFS_PATH_MAX];
    static const char *s_argv[70];

    {
        size_t n = 0;
        while (path[n] && n < sizeof(kpath) - 1) { kpath[n] = path[n]; n++; }
        kpath[n] = '\0';

        char *ap = kargbuf, *aend = kargbuf + sizeof(kargbuf);
        int   na = 0;
        if (argv) {
            while (argv[na] && na < 64) {
                const char *s = argv[na];
                size_t m = 0;
                while (s[m] && ap + m + 1 < aend) { ap[m] = s[m]; m++; }
                ap[m] = '\0';
                kargv[na] = ap;
                ap += m + 1;
                na++;
            }
        }
        kargv[na] = NULL;
        int argc_snap = na;

        char *ep = kenvbuf, *eend = kenvbuf + sizeof(kenvbuf);
        int   ne = 0;
        if (envp) {
            while (envp[ne] && ne < 128) {
                const char *s = envp[ne];
                size_t m = 0;
                while (s[m] && ep + m + 1 < eend) { ep[m] = s[m]; m++; }
                ep[m] = '\0';
                kenvp[ne] = ep;
                ep += m + 1;
                ne++;
            }
        }
        kenvp[ne] = NULL;
        int envc_snap = ne;

        /* argv robustness
         * musl's __init_libc unconditionally dereferences argv[0] when
         * building __progname, so an execve with argv=NULL or an empty
         * argv crashes the new image with #GP inside libc startup (seen
         * when a forked child's register state degenerates across the
         * fork on some boot paths).  Linux tolerates argc==0, musl does
         * not - synthesise { path } so the new image always starts with
         * a sane argv[0]. */
        if (!argv || !argv[0]) {
            kargv[0] = kpath;
            kargv[1] = NULL;
            argv = (char *const *)kargv;
        }

        /* shadow local copies used below */
        path = kpath;
        argv = (char *const *)kargv;
        (void)argc_snap; (void)envc_snap;
        envp = (char *const *)kenvp;
    }

    vfs_node_t *node = vfs_resolve(path);
    if (!node || !node->ops || !node->ops->read) EXEC_RET("resolve", -2);

    /* Execute permission check (Linux EACCES semantics)
     * The script node is checked first, shebang re-resolution below
     * re-checks the interpreter via the same helper. */
    if (!vfs_check_exec(node)) EXEC_RET("execperm", -13);   /* -EACCES */

    /* Shebang support: "#!/interp [-arg]" scripts
     * Rewrite the exec target as (interp, [arg,] script, argv...) and
     * re-resolve, up to 5 nesting levels like Linux.  The interpreter
     * must be an absolute path (Linux returns ENOENT otherwise). */
    for (int d = 0; d < 5; d++) {
        uint8_t magic[2] = {0, 0};
        if (node->ops->read(node, magic, 2, 0) != 2) break;
        if (magic[0] != '#' || magic[1] != '!') break;

        char line[296];
        int n = node->ops->read(node, line, 295, 0);
        if (n < 2) EXEC_RET("shebang-read", -8);                 /* ENOEXEC */
        line[n] = '\0';
        char *p = line + 2;
        char *nl = p;
        while (*nl && *nl != '\n' && *nl != '\r') nl++;
        *nl = '\0';
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) EXEC_RET("shebang-empty", -8);

        char *w = s_interp;
        while (*p && *p != ' ' && *p != '\t' && w < s_interp + 255) *w++ = *p++;
        *w = '\0';
        while (*p == ' ' || *p == '\t') p++;
        s_arg[0] = '\0';
        if (*p) {
            char *a = s_arg;
            while (*p && *p != ' ' && *p != '\t' && a < s_arg + 255) *a++ = *p++;
            *a = '\0';
            while (*p == ' ' || *p == '\t') p++;
            if (*p) EXEC_RET("shebang-arg", -8);   /* strict: one optional arg only */
        }
        if (s_interp[0] != '/') EXEC_RET("shebang-rel", -2);

        /* snapshot the script path, then rebuild argv */
        for (size_t i = 0; i < VFS_PATH_MAX; i++) {
            s_script[i] = kpath[i];
            if (!kpath[i]) break;
        }
        int nnew = 0;
        s_argv[nnew++] = s_interp;
        if (s_arg[0]) s_argv[nnew++] = s_arg;
        s_argv[nnew++] = s_script;
        for (int i = 1; kargv[i] && nnew < 69; i++) s_argv[nnew++] = kargv[i];
        s_argv[nnew] = NULL;

        for (size_t i = 0; i < VFS_PATH_MAX; i++) {
            kpath[i] = s_interp[i];
            if (!s_interp[i]) break;
        }
        argv = (char *const *)s_argv;

        node = vfs_resolve(kpath);
        if (!node || !node->ops || !node->ops->read) EXEC_RET("shebang-resolve", -2);
        if (!vfs_check_exec(node)) EXEC_RET("shebang-perm", -13);
    }

    /* setuid / setgid file bits (Linux exec semantics)
     * execve is the only place privileges change automatically: if the
     * image file carries the set-user-ID bit the effective uid becomes
     * the file owner (same for set-group + group).  Real/saved ids are
     * kept so setuid()/setresuid() can still drop back.  This is what
     * makes /bin/passwd writable-by-root-only while runnable by anyone.
     * Only plain files qualify (never directories/devices/pipes). */
    if (node->type == VFS_TYPE_FILE) {
        if (node->mode & 04000) {                     /* S_ISUID  */
            g_current_proc->euid = node->uid;
        }
        if (node->mode & 02000) {                     /* S_ISGID  */
            g_current_proc->egid = node->gid;
        }
    }

    /* Point OF no return
     * Everything above is recoverable: on failure the old image is still
     * intact and the syscall tail simply returns -errno to the caller
     * (dash's PATH search depends on that).  From here on the old image
     * is destroyed, so any failure must KILL the process (EXEC_FAIL),
     * never return into freed pages.                                  */
    /* (Linux exec semantics): the whole thread group dies with
     * the old image, siblings share the address space we are about to
     * destroy, so they must be gone before the first page is freed. */
    proc_kill_group_threads(g_current_proc);

    proc_free_user_pages();
    vfs_fd_close_cloexec();

    elf_image_t img;
    int r = elf_load_user(node, &img);
    if (r != ELF_OK) EXEC_FAIL("elfload", -2);

    /* load the interpreter for dynamic images
     * Linux exec semantics: the kernel maps both the main image and the
     * PT_INTERP interpreter (ld-musl-x86_64.so.1); the interpreter is
     * entered first, self-relocates, links the main image against
     * libc.so (its own file, musl ships them merged) and finally jumps
     * to AT_ENTRY.  The interpreter is an ET_DYN loaded at its own
     * fixed bias, AT_BASE tells it where.                            */
    elf_image_t interp = { 0 };
    int         have_interp = 0;
    if (img.is_dyn && !img.is_ia32) {
        if (!img.interp_path[0])
            EXEC_FAIL("no-interp", -2);
        vfs_node_t *in = vfs_resolve(img.interp_path);
        if (!in || !in->ops || !in->ops->read)
            EXEC_FAIL("interp-resolve", -2);      /* ENOENT */
        if (!vfs_check_exec(in)) EXEC_FAIL("interp-perm", -13);
        int ir = elf_map_image64(in, &interp, ELF_INTERP_BASE);
        if (ir != ELF_OK) EXEC_FAIL("interp-load", -2);
        have_interp = 1;
    }

    /* Allocate user stack pages */
    for (int i = 0; i < PROC_STACK_PAGES; i++) {
        uint64_t phys = pmm_alloc();
        if (!phys) EXEC_FAIL("stack-pmm", -12);
        uint64_t virt = PROC_STACK_BASE + (uint64_t)i * PAGE_SIZE;
        if (!vmm_map(virt, phys, PTE_PRESENT | PTE_WRITABLE | PTE_USER))
            EXEC_FAIL("stack-map", -12);
    }

    /* Build Linux x86-64 ABI startup stack (argc/argv/envp/auxv)
     * Written into the freshly mapped user stack pages which the kernel can
     * access directly (user VA space is shared with the kernel page tables).
     */
    int  argc = 0;
    if (argv) while (argv[argc]) argc++;
    int  envc = 0;
    if (envp) while (envp[envc]) envc++;

    startup_auxv_t ax = { 0 };
    ax.phdr_va = img.phdr_va;
    ax.phent   = img.phent;
    ax.phnum   = img.phnum;
    ax.entry   = img.entry;
    ax.base    = have_interp ? interp.load_bias : 0;

    uint64_t new_rsp = img.is_ia32
        ? build_startup_stack32((char **)argv, argc, (char **)envp, envc, &ax)
        : build_startup_stack ((char **)argv, argc, (char **)envp, envc, &ax);

#if FORK_DEBUG
    /* Read the startup stack back exactly the way musl's _start_c will:
     * p[0]=argc, argv=p+1, envp=argv+argc+1.  If this prints correctly
     * but userspace sees mangled args, the corruption happens after
     * SYSRETQ - not in the builder. */
    {
        uint64_t *spv = (uint64_t *)(uintptr_t)new_rsp;
        char **ua  = (char **)(spv + 1);
        char **uev = ua + argc + 1;
        display_printf("[fd] stack: argc=%lu raw=[%lx %lx %lx %lx] a0=%s\n",
                       (unsigned long)spv[0],
                       (unsigned long)spv[1], (unsigned long)spv[2],
                       (unsigned long)spv[3], (unsigned long)spv[4],
                       ua[0] ? ua[0] : "?");
    }
#endif

    /* musl's _start reads argc/argv/envp from the stack, keep kernel-side
     * copies for reference/debugging (user VAs).  Word size differs for
     * the ia32 model (4-byte pointers, argc at [esp]). */
    size_t  ws   = img.is_ia32 ? 4 : 8;
    char  **user_argv = (char **)(uintptr_t)(new_rsp + ws);
    char  **user_envp = (char **)(uintptr_t)
                        ((uintptr_t)user_argv + (size_t)(argc + 1) * ws);

    /* Update process state */
    g_current_proc->image      = img;
    g_current_proc->entry      = img.entry;
    g_current_proc->user_rsp   = new_rsp;
    g_current_proc->heap_start = img.load_end;
    g_current_proc->heap_brk   = img.load_end;
    g_current_proc->mmap_bump  = PROC_MMAP_BASE;
    g_current_proc->argc       = argc;
    g_current_proc->argv       = user_argv;
    g_current_proc->envp       = user_envp;
    /* identity + code segment for the new image */
    {
        const char *b = kpath;
        for (const char *q = kpath; *q; q++) if (*q == '/') b = q + 1;
        size_t i = 0;
        for (; b[i] && i < sizeof(g_current_proc->comm) - 1; i++)
            g_current_proc->comm[i] = b[i];
        g_current_proc->comm[i] = '\0';
    }
    {
        size_t i = 0;
        for (; kpath[i] && i < sizeof(g_current_proc->exec_path) - 1; i++)
            g_current_proc->exec_path[i] = kpath[i];
        g_current_proc->exec_path[i] = '\0';
    }
    g_current_proc->user_cs = img.is_ia32
        ? (uint64_t)(GDT_USER_CODE32 | 3) : (uint64_t)(GDT_USER_CODE | 3);
    g_current_proc->is_ia32 = img.is_ia32;
    /* Initialise pgroup/session to self (matches a fresh setsid) */
    g_current_proc->pgid       = g_current_proc->pid;
    g_current_proc->sid        = g_current_proc->pid;
    g_current_proc->fg_pgrp    = g_current_proc->pid;
    /* Clear signal table, new image starts with default dispositions */
    proc_memzero(g_current_proc->sigactions,
                 sizeof(g_current_proc->sigactions));
    g_current_proc->pending_sigs       = 0;
    g_current_proc->alarm_deadline_ms  = 0;

    if (g_current_proc->fdt) vfs_fd_save(g_current_proc->fdt->t);

    /* Enter the new image
     * 64-bit: patch the syscall frame and let the tail sysretq back in
     * (rcx = user RIP).  The entry RIP is the interpreter's when one was
     * loaded (ld-musl jumps to AT_ENTRY after linking).
     * ia32: sysretq cannot return into a 32-bit code segment (CS comes
     * from MSR STAR), so install a complete user context and hand
     * control to the scheduler's resume path (iretq with user_cs).      */
    if (img.is_ia32) {
        sched_ctx_t *c = &g_current_proc->sched_ctx;
        proc_memzero(c, sizeof(*c));
        c->rip      = have_interp ? interp.entry : img.entry;
        c->user_rsp = new_rsp;
        c->rflags   = 0x202ULL;
        c->rax      = 0;
        c->rdi      = (uint64_t)(uint32_t)argc;
        c->rsi      = (uint64_t)(uintptr_t)user_argv;
        c->rdx      = (uint64_t)(uintptr_t)user_envp;
        sched_resume_new_image(g_current_proc);       /* noreturn */
    }

    frame->rcx      = have_interp ? interp.entry : img.entry;
    frame->user_rsp = new_rsp;     /* user RSP: points at argc      */
    frame->r11      = 0x202ULL;    /* user RFLAGS: IF set           */
    frame->rax      = 0;           /* execve returns 0              */
    frame->rdi      = (uint64_t)(uint32_t)argc;              /* argc */
    frame->rsi      = (uint64_t)(uintptr_t)user_argv;        /* argv */
    frame->rdx      = (uint64_t)(uintptr_t)user_envp;        /* envp */

    return 0;
}

uint64_t proc_brk(uint64_t addr) {
    if (!g_current_proc) return (uint64_t)-1;

    if (addr == 0) {
        uint64_t brk = g_current_proc->heap_brk;
#ifdef DEBUG_SYSCALLS
        display_printf("proc_brk(0) -> 0x%x\n", (unsigned)brk);
#endif
        return brk;
    }

    uint64_t new_brk = (addr + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);

    if (new_brk < g_current_proc->heap_start) new_brk = g_current_proc->heap_start;
    if (new_brk >= PROC_STACK_BASE)            return (uint64_t)-1;

    uint64_t old_brk = g_current_proc->heap_brk;

    if (new_brk == old_brk) return old_brk;

#ifdef DEBUG_SYSCALLS
    display_printf("proc_brk: addr=0x%x old=0x%x new=0x%x\n",
           (unsigned)addr, (unsigned)old_brk, (unsigned)new_brk);
#endif

    if (new_brk > old_brk) {
        for (uint64_t va = old_brk; va < new_brk; va += PAGE_SIZE) {
#ifdef DEBUG_SYSCALLS
            display_printf("  map va=0x%x\n", (unsigned)va);
#endif
            uint64_t phys = pmm_alloc();
            if (!phys) {
#ifdef DEBUG_SYSCALLS
                display_printf("  pmm_alloc FAILED\n");
#endif
                return (uint64_t)-1;
            }
#ifdef DEBUG_SYSCALLS
            display_printf("  phys=0x%x\n", (unsigned)phys);
#endif
            if (!vmm_map(va, phys, PTE_PRESENT | PTE_WRITABLE | PTE_USER)) {
#ifdef DEBUG_SYSCALLS
                display_printf("  vmm_map FAILED\n");
#endif
                pmm_free(phys);
                return (uint64_t)-1;
            }
#ifdef DEBUG_SYSCALLS
            display_printf("  mapped OK\n");
#endif
        }
    } else {
        for (uint64_t va = new_brk; va < old_brk; va += PAGE_SIZE) {
            uint64_t phys = vmm_virt_to_phys(va);
            if (phys) pmm_free(phys);
            vmm_unmap(va);
        }
    }

    g_current_proc->heap_brk = new_brk;
    return new_brk;
}

static void proc_thread_exit(process_t *proc, int code) __attribute__((noreturn));

void proc_exit(process_t *proc, int code) {
    /* release POSIX timers + record locks owned by the group so
     * timer ids and lock ranges can be reused by later processes. */
    posix_timers_release(proc->tgid);
    locks_release_process(proc->tgid);
#if EXIT_DEBUG
    serial_print_string("[exitdbg] exit pid=");
    exitdbg_dec(proc->pid);
    serial_print_string(" code=");
    exitdbg_dec(code);
    serial_print_string("\n");
#endif
    /* a CLONE_THREAD task dies without zombie/zombie-reaping -
     * musl reaps threads via futexes in user memory. */
    if (proc->is_thread) proc_thread_exit(proc, code);

    /* Linux exit semantics: SYS_exit/SYS_exit_group from the group leader
     * terminates the whole thread group.  Siblings share the address space
     * torn down below, so they must be released first. */
    proc_kill_group_threads(proc);
    /* r5: COW-aware teardown.  The child's leaf PTEs may point at
     * frames still mapped by the parent (fork) or a sibling, freeing
     * them outright corrupted other processes' code/data (observed as
     * random #GP in dash).  vmm_cow_release_page decrements the share
     * count, frees only on the last share, and never frees memfd-owned
     * MAP_SHARED frames.  Runs under the exiting process's CR3. */
    proc_free_user_pages();

    vfs_fd_close_all();

    proc->exit_code     = code;
    proc->exit_code_tmp = code;
    proc->state         = PROC_STATE_ZOMBIE;

    process_t *parent = NULL;
    for (int i = 0; i < PROC_MAX; i++) {
        if (g_proc_table[i].pid == proc->parent_pid &&
            g_proc_table[i].state != PROC_STATE_EMPTY) {
            parent = &g_proc_table[i];
            break;
        }
    }

    if (parent && parent->state == PROC_STATE_WAITING) {
        int match = (parent->wait_for_pid == -1) ||
                    (parent->wait_for_pid == proc->pid);
        if (match) {
            /* (BORE): the parent is kernel-parked inside
             * proc_waitpid's scan loop, waking it is enough.  The
             * zombie re-scan there reads exit_code directly and does
             * the cleanup (CR3 tree, kstack, slot), so no frame
             * patching or cross-address-space status write is needed. */
            parent->wait_for_pid = 0;
            parent->wstatus_ptr  = NULL;
            parent->wait_frame   = NULL;
            sched_wake(parent);        /* Waiting -> READY + BORE credit */
        }
    }

    /* \u2500\u2500 Notify parent: set SIGCHLD pending \u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500
     * Dash registers SIGCHLD to reap children.  We can't preempt the parent
     * but we record the pending signal so proc_waitpid (or the next syscall)
     * can call proc_deliver_signal before returning.
     * \u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500 */
    if (parent) {
        parent->pending_sigs |= (1ull << SIGCHLD);
    }

    if (proc->fdt) vfs_fd_save(proc->fdt->t);

    /* (BORE): no longjmp, hand control to the scheduler.
     * sched_exit_switch picks the next READY task (or idles) and never
     * returns: this kernel context is abandoned with the exiting task. */
    sched_exit_switch();
}

int proc_waitpid(int pid, int *wstatus, int options, syscall_frame_t *frame) {
    (void)options;
#if EXIT_DEBUG
    serial_print_string("[exitdbg] waitpid enter pid=");
    exitdbg_dec(pid);
    serial_print_string(" opts=");
    exitdbg_dec(options);
#endif

    int my_pid = g_current_proc ? g_current_proc->pid : 0;

    /* Children reaped inside fork() - report them first */
    process_t *me = g_current_proc;
    if (me) {
        for (int i = 0; i < me->reaped_count; i++) {
            if (pid != -1 && me->reaped[i].pid != pid) continue;
            if (wstatus) *wstatus = me->reaped[i].status;
#if EXIT_DEBUG
            serial_print_string("[exitdbg] waitpid-reaped pid=");
            exitdbg_dec(me->reaped[i].pid);
            serial_print_string(" st=0x");
            exitdbg_dec(me->reaped[i].status);
            serial_print_string("\n");
#endif
            int cpid = me->reaped[i].pid;
            for (int j = i; j < me->reaped_count - 1; j++)
                me->reaped[j] = me->reaped[j + 1];
            me->reaped_count--;
#if EXIT_DEBUG
            serial_print_string(" ret=");
            exitdbg_dec(cpid);
            serial_print_string("\n");
#endif
            return cpid;
        }
    }

    /* (BORE): park-and-rescan wait loop
     * The child is not run inline anymore.  Instead this task parks
     * (kernel continuation, stack intact) until proc_exit() marks a
     * matching child ZOMBIE and wakes us (sched_wake), or a fallback
     * timer re-scans in case a wakeup was missed.  The zombie cleanup
     * below runs in the parent's context, which is exactly where the
     * CR3 tree and kstack frees want to happen. */
    for (;;) {
        process_t *child = NULL;
        for (int i = 0; i < PROC_MAX; i++) {
            process_t *p = &g_proc_table[i];
            if (p->state == PROC_STATE_EMPTY) continue;
            if (p->is_thread) continue;          /* threads are not waitable */
            if (p->parent_pid != my_pid) continue;
            if (pid != -1 && p->pid != pid) continue;
            child = p;
            break;
        }
        if (!child) {
            return -10;
        }

        if (child->state == PROC_STATE_ZOMBIE) {
            if (wstatus)
                *wstatus = WSTATUS_EXIT(child->exit_code);
            int cpid = child->pid;
#if EXIT_DEBUG
            serial_print_string(" ret(zombie)=");
            exitdbg_dec(cpid);
            serial_print_string("\n");
#endif
            vmm_free_tree(child->cr3);
            child->cr3 = 0;
            for (int i = 0; i < PROC_KSTACK_PAGES; i++)
                pmm_free(child->kstack_base + (uint64_t)i * PAGE_SIZE);
            vfs_fdt_put(child->fdt);
            child->fdt = NULL;
            proc_free(child);
            return cpid;
        }

        /* WNOHANG (POSIX value 1): non-blocking poll for dash job control */
        if (options & 1) return 0;

        if (g_current_proc) {
            g_current_proc->state        = PROC_STATE_WAITING;
            g_current_proc->wait_for_pid = (pid == -1) ? -1 : child->pid;
            g_current_proc->wstatus_ptr  = NULL;
            g_current_proc->wait_frame   = NULL;
        }
        sched_kernel_park(50);   /* fallback re-scan every 50 ms */
        if (g_current_proc) g_current_proc->wait_for_pid = 0;
        /* woken: loop re-scans, the zombie (if any) is reaped above */
    }
}

/*
 * proc_deliver_signal - attempt to deliver signal sig to proc.
 *
 * frame: the syscall frame that will be used to resume the process,
 *        if we install a user handler we redirect rcx (=RIP) to it.
 *
 * Signal model (cooperative, no preemption):
 *   SIG_IGN - discard.
 *   SIG_DFL - fatal signals terminate the process, SIGCHLD/SIGCONT/SIGSTOP
 *              are silently dropped (default ignore).
 *   handler - push a minimal signal frame on the user stack and redirect
 *              RIP to the handler.  The restorer is called after the handler
 *              returns so the process comes back into the kernel via
 *              SYS_RT_SIGRETURN, which is a no-op stub here.
 */
void proc_deliver_signal(process_t *proc, int sig, syscall_frame_t *frame) {
    if (!proc || sig < 1 || sig >= NSIG) return;

    uint64_t handler = proc->sigactions[sig].sa_handler;

    if (handler == SIG_IGN) return;  /* explicitly ignored */

    if (handler == SIG_DFL) {
        /* Default actions - only a subset matters for dash */
        switch (sig) {
        /* Silently ignored by default */
        case SIGCHLD: case SIGCONT: case SIGWINCH:
            return;
        /* Terminal signals: kill the process */
        case SIGINT: case SIGTERM: case SIGHUP:
        case SIGPIPE: case SIGQUIT: case SIGKILL:
            proc_exit(proc, 128 + sig);
            /* proc_exit is noreturn */
        default:
            /* Other signals ignored for now */
            return;
        }
    }

    /*
     * User-space handler: push a full register-context frame on the user
     * stack and redirect RIP to the handler VA.
     *
     * Stack layout (addresses grow up, sp = handler's RSP):
     *   X-8        restorer address  <- handler RSP (its `ret` pops this)
     *   X .. X+128 saved syscall_frame_t copy (16 qwords, same layout as
     *              syscall_entry.S pushes)
     *
     * When the handler returns, it pops the restorer address, the restorer
     * (musl provides one via SA_RESTORER) issues SYS_RT_SIGRETURN with user
     * RSP == X, and the kernel restores all registers from the saved context.
     */
    if (!frame) return;  /* can't redirect without a frame */

    uint64_t ustack0 = frame->user_rsp;
    uint64_t X       = (ustack0 - 136) & ~15ULL;   /* 16-aligned ctx base  */

    /* validate the user-stack pages we are about to
     * write.  If the stack is unmapped (process already half-dead) a
     * blind write here page-faults inside the kernel and halts the
     * machine.  Treat the signal as fatal instead - Linux kills. */
    {
        uint64_t pg0 = X & ~0xFFFULL;
        uint64_t pg1 = (X + 135) & ~0xFFFULL;
        for (uint64_t pg = pg0; ; pg += 0x1000) {
            if (!vmm_virt_to_phys(pg)) proc_exit(proc, 128 + sig);
            if (pg >= pg1) break;
        }
    }

    uint64_t *ctx = (uint64_t *)(uintptr_t)X;
    ctx[0]  = frame->r9;
    ctx[1]  = frame->r8;
    ctx[2]  = frame->r10;
    ctx[3]  = frame->rdx;
    ctx[4]  = frame->rsi;
    ctx[5]  = frame->rdi;
    ctx[6]  = frame->rax;          /* original syscall return value */
    ctx[7]  = frame->r15;
    ctx[8]  = frame->r14;
    ctx[9]  = frame->r13;
    ctx[10] = frame->r12;
    ctx[11] = frame->rbx;
    ctx[12] = frame->rbp;
    ctx[13] = frame->r11;          /* user rflags */
    ctx[14] = frame->rcx;          /* user rip    */
    ctx[15] = frame->user_rsp;

    uint64_t restorer = proc->sigactions[sig].sa_restorer;
    if (!restorer) {
        /* musl always installs a SA_RESTORER stub, without one the handler
         * cannot return.  Point at 0 - will fault, same as before. */
        restorer = 0;
    }
    *(uint64_t *)(uintptr_t)(X - 8) = restorer;

    /* Redirect to handler: first arg (rdi) = signum */
    frame->user_rsp = X - 8;
    frame->rcx      = handler;                 /* new RIP */
    frame->rdi      = (uint64_t)(uint32_t)sig; /* signum arg */
    /* r11 (rflags) stays as-is */
}


/*
 *  POSIX threads (CLONE_VM|CLONE_THREAD)
 *
 * Kernel contract extracted from musl (src/thread/x86_64/clone.s +
 * pthread_create.c + pthread_join.c):
 *
 *   SYS_clone(flags, stack, parent_tid, child_tid, tls)
 *     flags = CLONE_VM|CLONE_FS|CLONE_FILES|CLONE_SIGHAND|CLONE_THREAD|
 *             CLONE_SYSVSEM|CLONE_SETTLS|CLONE_PARENT_SETTID|
 *             CLONE_CHILD_CLEARTID|CLONE_DETACHED
 *   child returns with rax = 0, rsp = stack, every other register equal
 *   to the parent's at syscall entry, in particular r9 (musl keeps the
 *   thread start fn there across the syscall) and rcx (return RIP).
 *
 *   Thread exit = SYS_exit(60) (not exit_group).  The kernel must then
 *   write 0 to *clear_child_tid and futex-wake that address, musl points
 *   ctid at &__thread_list_lock, so this is the thread-list-lock release.
 *
 *   pthread_join never calls wait4: it futex-waits on the target's
 *   detach_state in shared memory.  Threads are therefore auto-reaped
 *   by the kernel (no zombie, no slot held for the parent).
 */

int proc_group_live_count(int tgid, process_t *except) {
    int n = 0;
    for (int i = 0; i < PROC_MAX; i++) {
        process_t *p = &g_proc_table[i];
        if (p == except) continue;
        if (p->state == PROC_STATE_EMPTY || p->state == PROC_STATE_ZOMBIE)
            continue;
        if (p->tgid == tgid) n++;
    }
    return n;
}

void proc_clear_ctid_wake(process_t *proc) {
    if (!proc || !proc->clear_child_tid) return;
    /* The group's cr3 is active when one of its tasks runs kernel code. */
    *(uint32_t *)(uintptr_t)proc->clear_child_tid = 0;
    extern int k_futex_wake_uaddr(uint64_t cr3_key, uint32_t *uaddr, int n);
    k_futex_wake_uaddr(proc->cr3,
                       (uint32_t *)(uintptr_t)proc->clear_child_tid, 1);
    proc->clear_child_tid = 0;
}

void proc_kill_group_threads(process_t *except) {
    if (!except) return;
    for (int i = 0; i < PROC_MAX; i++) {
        process_t *p = &g_proc_table[i];
        if (p == except || p->state == PROC_STATE_EMPTY) continue;
        if (!p->is_thread || p->tgid != except->tgid) continue;

        proc_clear_ctid_wake(p);      /* release musl tl-lock waiters   */
        extern void k_futex_detach_task(process_t *p);
        k_futex_detach_task(p);       /* drop a parked futex entry, if any */

        p->wait_for_pid = 0;
        p->wstatus_ptr  = NULL;
        p->wait_frame   = NULL;
        p->alarm_deadline_ms = 0;

        for (int k = 0; k < PROC_KSTACK_PAGES; k++)
            pmm_free(p->kstack_base + (uint64_t)k * PAGE_SIZE);
        vfs_fdt_put(p->fdt);
        p->fdt  = NULL;
        p->cr3  = 0;   /* shared tree - freed with the leader            */
        p->state = PROC_STATE_EMPTY;
    }
}

/* Thread-flavoured exit - noreturn, called from proc_exit. */
static void proc_thread_exit(process_t *proc, int code) {
    (void)code;
#if EXIT_DEBUG
    serial_print_string("[exitdbg] thread-exit tid=");
    exitdbg_dec(proc->pid);
    serial_print_string("\n");
#endif
    proc_clear_ctid_wake(proc);
    extern void k_futex_detach_task(process_t *p);
    k_futex_detach_task(proc);

    proc->wait_for_pid = 0;
    proc->wstatus_ptr  = NULL;
    proc->wait_frame   = NULL;
    proc->alarm_deadline_ms = 0;

    bool last = (proc_group_live_count(proc->tgid, proc) == 0);
    if (last) {
        /* Abnormal: the whole group is gone, do the full teardown the
         * leader path would have done.  Our cr3 is active right now. */
        proc_free_user_pages();
        vmm_free_tree(proc->cr3);
        proc->cr3 = 0;
    }

    /* capture the live fd table before sched_exit_switch -
     * it NULLs g_current_proc, so switch_to skips the save and the
     * exiting thread's own opens would vanish from a CLONE_FILES-shared
     * table (observed: systest pthread-fdshare read EBADF).           */
    if (proc->fdt) vfs_fd_save(proc->fdt->t);

    for (int k = 0; k < PROC_KSTACK_PAGES; k++)
        pmm_free(proc->kstack_base + (uint64_t)k * PAGE_SIZE);
    vfs_fdt_put(proc->fdt);
    proc->fdt  = NULL;
    proc->state = PROC_STATE_EMPTY;    /* threads never become zombies */

    sched_exit_switch();               /* noreturn */
    __builtin_unreachable();
}

int proc_clone(syscall_frame_t *frame) {
    /* x86-64 Linux ABI (musl's __clone.s maps libc args onto these):
     * rdi=flags, rsi=stack, rdx=parent_tid, r10=child_tid, r8=tls.     */
    uint64_t flags = frame->rdi;
    uint64_t stack = frame->rsi;
    uint64_t ptid  = frame->rdx;
    uint64_t ctid  = frame->r10;
    uint64_t tls   = frame->r8;

    /* fork-like clone (musl fork() = clone(SIGCHLD, 0)) -> existing path */
    if (!(flags & CLONE_VM) || !(flags & CLONE_THREAD))
        return proc_fork(frame);

    process_t *parent = g_current_proc;
    if (!parent) return -12;                                    /* ENOMEM */

    process_t *child = proc_alloc();   /* zeroed slot, fresh pid       */
    if (!child) return -12;
    int tid = child->pid;

#if THREAD_DEBUG
    {
        extern void serial_print_string(const char *);
        extern void sched_dbg_hex(uint64_t);
        serial_print_string("[clone] flags=");
        sched_dbg_hex(flags);
        serial_print_string(" tid=");
        sched_dbg_hex((uint64_t)tid);
        serial_print_string(" stack=");
        sched_dbg_hex(stack);
        serial_print_string(" ptid=");
        sched_dbg_hex(ptid);
        serial_print_string(" ctid=");
        sched_dbg_hex(ctid);
        serial_print_string(" tls=");
        sched_dbg_hex(tls);
        serial_print_string(" ret_rip=");
        sched_dbg_hex(frame->rcx);
        serial_print_string("\n");
    }
#endif

    /* Snapshot the parent (creds, cwd, umask, sigactions, sig_mask,
     * heap/mmap state, image metadata), then fix identity fields.      */
    *child = *parent;
    child->pid             = tid;
    child->tgid            = parent->tgid;
    child->parent_pid      = parent->parent_pid;
    child->is_thread       = true;
    child->state           = PROC_STATE_READY;
    child->exit_code       = 0;
    child->exit_code_tmp   = 0;
    child->pending_sigs    = 0;
    child->reaped_count    = 0;
    child->wait_for_pid    = 0;
    child->wstatus_ptr     = NULL;
    child->wait_frame      = NULL;
    child->alarm_deadline_ms = 0;
    child->has_fork_frame  = 0;
    child->kctx_valid      = false;
    child->sched_ctx_valid = false;
    child->futex_woken     = false;

    /* Shared address space + fd table (CLONE_FILES). */
    child->cr3 = parent->cr3;
    child->fdt = (flags & CLONE_FILES) ? vfs_fdt_share(parent->fdt)
                                       : vfs_fdt_clone(parent->fdt);
    if (!child->fdt) child->fdt = vfs_fdt_share(parent->fdt);
    /* CLONE_FILES: shared table, no ref bump.  Private table
     * (rare for threads, and the fork fallback path): bump.        */
    if (!(flags & CLONE_FILES) && child->fdt && parent->fdt &&
        child->fdt != parent->fdt)
        vfs_pipe_refs_bump(child->fdt->t);

    /* Private kernel stack. */
    uint64_t kstack_base = pmm_alloc_contig(PROC_KSTACK_PAGES);
    if (!kstack_base) {
        vfs_fdt_put(child->fdt);
        child->fdt = NULL;
        proc_free(child);
        return -12;
    }
    child->kstack_base = kstack_base;
    child->kstack_top  = kstack_base + PROC_KSTACK_SIZE;

    /* TLS (CLONE_SETTLS): the child's %fs base points at its pthread
     * struct - musl's very first child-mode instruction reads %fs:0.   */
    child->fs_base = (flags & CLONE_SETTLS) ? tls : parent->fs_base;

    /* TID writes (32-bit, Linux semantics).  Under CLONE_VM the parent's
     * cr3 is active here and the child shares it - a direct store is
     * visible to both. */
    if ((flags & CLONE_PARENT_SETTID) && ptid)
        *(uint32_t *)(uintptr_t)ptid = (uint32_t)tid;
    if ((flags & CLONE_CHILD_SETTID) && ctid)
        *(uint32_t *)(uintptr_t)ctid = (uint32_t)tid;
    child->clear_child_tid =
        (flags & CLONE_CHILD_CLEARTID) ? ctid : 0;

    /* User context: identical to the parent's at syscall entry, with
     * rax = 0 and rsp = the requested stack.  rcx (return RIP) and r9
     * (thread start fn) survive via ctx_from_sysframe_helper.          */
    ctx_from_sysframe_helper(&child->sched_ctx, frame);
    child->sched_ctx.rax = 0;
    if (stack) child->sched_ctx.user_rsp = stack;
    child->sched_ctx_valid = true;
    child->state = PROC_STATE_READY;

    sched_fork_inherit(child);         /* BORE fairness inherit         */

    return tid;
}
