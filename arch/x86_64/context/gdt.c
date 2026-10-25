#include "cpu/gdt.h"
#include <stdint.h>
#include <stddef.h>

/* Raw descriptor types */

typedef struct __attribute__((packed)) {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  granularity;   /* limit_high[3:0] | flags[7:4] */
    uint8_t  base_high;
} gdt_entry_t;

/* TSS descriptor is 16 bytes (two consecutive GDT slots) */
typedef struct __attribute__((packed)) {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  granularity;
    uint8_t  base_high;
    uint32_t base_upper;
    uint32_t reserved;
} gdt_tss_entry_t;

typedef struct __attribute__((packed)) {
    uint16_t limit;
    uint64_t base;
} gdtr_t;

/* Task State Segment */
typedef struct __attribute__((packed)) {
    uint32_t reserved0;
    uint64_t rsp0;          /* kernel stack pointer (ring 0) */
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist[7];        /* interrupt stack table */
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iopb_offset;
} tss_t;

/* GDT storage */
/*  6 normal entries + 1 ia32 code + 1 TSS (takes 2 slots = 16 bytes)  */
#define GDT_ENTRIES 8

static uint8_t gdt_storage[GDT_ENTRIES * 8];   /* raw bytes */
static gdtr_t  gdtr;
static tss_t   tss;

/* Helpers */
static void gdt_set_entry(int idx, uint32_t base, uint32_t limit,
                           uint8_t access, uint8_t gran) {
    gdt_entry_t *e = (gdt_entry_t *)(gdt_storage + idx * 8);
    e->limit_low  = (uint16_t)(limit & 0xFFFF);
    e->base_low   = (uint16_t)(base  & 0xFFFF);
    e->base_mid   = (uint8_t)((base  >> 16) & 0xFF);
    e->access     = access;
    e->granularity= (uint8_t)(((limit >> 16) & 0x0F) | (gran & 0xF0));
    e->base_high  = (uint8_t)((base  >> 24) & 0xFF);
}

static void gdt_set_tss_entry(int idx, uint64_t base, uint32_t limit) {
    gdt_tss_entry_t *e = (gdt_tss_entry_t *)(gdt_storage + idx * 8);
    e->limit_low  = (uint16_t)(limit & 0xFFFF);
    e->base_low   = (uint16_t)(base  & 0xFFFF);
    e->base_mid   = (uint8_t)((base  >> 16) & 0xFF);
    e->access     = 0x89;   /* present, ring0, 64-bit TSS available */
    e->granularity= (uint8_t)(((limit >> 16) & 0x0F));
    e->base_high  = (uint8_t)((base  >> 24) & 0xFF);
    e->base_upper = (uint32_t)(base  >> 32);
    e->reserved   = 0;
}

/* Public API */
void gdt_init(void) {
    /* Null */
    gdt_set_entry(0, 0, 0, 0, 0);

    /* Kernel code: present, ring0, code, 64-bit, readable */
    gdt_set_entry(1, 0, 0xFFFFF, 0x9A, 0xA0);

    /* Kernel data: present, ring0, data, writable */
    gdt_set_entry(2, 0, 0xFFFFF, 0x92, 0xC0);

    /* User data: present, ring3, data, writable  (slot 3 = 0x18) */
    gdt_set_entry(3, 0, 0xFFFFF, 0xF2, 0xC0);

    /* User code: present, ring3, code, 64-bit, readable  (slot 4 = 0x20) */
    gdt_set_entry(4, 0, 0xFFFFF, 0xFA, 0xA0);

    /* IA-32 compat user code (slot 7 = 0x38): 32-bit code
     * segment (G=1 4-GiB limit, D=1 default operand size, L=0 - not
     * long mode).  Entered only via iretq (sysretq can't switch to a
     * non-64-bit CS), syscalls go through int 0x80. */
    gdt_set_entry(7, 0, 0xFFFFF, 0xFA, 0xC0);

    /* TSS (slots 5 + 6) */
    for (size_t i = 0; i < sizeof(tss); i++)
        ((uint8_t *)&tss)[i] = 0;
    tss.iopb_offset = (uint16_t)sizeof(tss);

    gdt_set_tss_entry(5, (uint64_t)(uintptr_t)&tss, sizeof(tss) - 1);
    /* slot 6 is the upper 8 bytes of the TSS descriptor, already written */

    /* Load GDTR */
    gdtr.limit = (uint16_t)(GDT_ENTRIES * 8 - 1);
    gdtr.base  = (uint64_t)(uintptr_t)gdt_storage;

    __asm__ volatile(
        "lgdt %0\n\t"
        /* Reload CS via a far return */
        "push %1\n\t"
        "lea  1f(%%rip), %%rax\n\t"
        "push %%rax\n\t"
        "lretq\n\t"
        "1:\n\t"
        /* Reload data segments */
        "mov %2, %%ax\n\t"
        "mov %%ax, %%ds\n\t"
        "mov %%ax, %%es\n\t"
        "mov %%ax, %%fs\n\t"
        "mov %%ax, %%gs\n\t"
        "mov %%ax, %%ss\n\t"
        /* Load TSS */
        "mov %3, %%ax\n\t"
        "ltr %%ax\n\t"
        :
        : "m"(gdtr),
          "i"((uint64_t)GDT_KERNEL_CODE),
          "i"((uint16_t)GDT_KERNEL_DATA),
          "i"((uint16_t)GDT_TSS)
        : "rax", "memory"
    );
}

void gdt_set_tss_rsp0(uint64_t rsp0) {
    tss.rsp0 = rsp0;
}