/*
 * arch.c - x86_64 implementation of the RMM arch contract (include/rmm/arch.h).
 *
 * Everything here is a thin adapter over the
 * kernel primitives (vmm/pmm, gdt/idt, lapic, pit).  The rest of the
 * kernel never sees an x86 detail.
 */
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <rmm/rmm.h>
#include <rmm/arch.h>
#include <rmm/module.h>
#include <rmm/fault.h>
#include "cpu/percpu.h"
#include "drivers/memory/vmm.h"
#include "drivers/memory/common.h"
#include "cpu/gdt.h"
#include "cpu/idt.h"
#include "drivers/timer/pit.h"
#include "exec/process.h"
#include "console/print.h"

/* Physical memory */
uint64_t arch_page_alloc(void)            { return pmm_alloc(); }
uint64_t arch_page_alloc_contig(size_t n) { return pmm_alloc_contig(n); }
void     arch_page_free(uint64_t phys)    { pmm_free(phys); }
uint64_t arch_page_used(void)             { return pmm_used_pages(); }
uint64_t arch_page_total(void)            { return pmm_total_pages(); }

/* Virtual memory */
static uint64_t mapflags(uint64_t f) {
    uint64_t out = PTE_PRESENT;
    if (f & RMM_MAP_WRITE) out |= PTE_WRITABLE;
    if (f & RMM_MAP_USER)  out |= PTE_USER;
    if (!(f & RMM_MAP_EXEC)) out |= PTE_NX;
    if (f & RMM_MAP_DEVICE) out |= (1ull << 4);      /* PCD: uncached */
    return out;
}

bool arch_map(uint64_t va, uint64_t phys, uint64_t flags) {
    return vmm_map(va, phys, mapflags(flags));
}

void arch_unmap(uint64_t va) { vmm_unmap(va); }

void arch_protect(uint64_t va, uint64_t flags) {
    /* toggle NX/W bits in place (vmm_mprotect semantics, kernel VA) */
    uint64_t pte = vmm_peek_pte(va);
    if (!pte) return;
    uint64_t want = mapflags(flags);
    /* rebuild: keep phys, set new flags */
    uint64_t phys = pte & 0x000FFFFFFFFFF000ull;
    vmm_map(va, phys, want);
}

uint64_t arch_translate(uint64_t va) { return vmm_virt_to_phys(va); }

uint64_t arch_root_switch(uint64_t root)  { return vmm_switch_root(root); }
uint64_t arch_root_current(void)          { return vmm_get_cr3(); }

/* SMP TLB sync - v1 is single-CPU, invlpg already issued per page
 * by vmm_map/vmm_unmap.  When SMP lands this broadcasts an IPI. */
void arch_tlb_sync_range(uint64_t start, uint64_t end) {
    (void)start; (void)end;
}

/* Module slots live in PML4[510], vmm_map builds interior tables on
 * demand, so no pre-flight work is needed, assert the invariant. */
void arch_slot_region_init(void) { }

/* Per-CPU */


void arch_percpu_init(void *area, size_t size, uint32_t cpu_id) {
    (void)size;
    percpu_init_cpu((percpu_t *)area, cpu_id, cpu_id);
}

/* BSP per-CPU bring-up: GS base must be live before interrupts open -
 * the trampolines and irq_dispatch address everything GS-relative. */
void arch_percpu_bsp_init(void) {
    percpu_init_cpu(&g_percpu[0], 0, 0);
    g_percpu[0].self = (uint64_t)(uintptr_t)&g_percpu[0];
}

void *arch_percpu_this(void) { return (void *)percpu_this(); }

struct rmm_cpu_context *arch_cpu_ctx(void) {
    return &percpu_this()->rmm;
}

/* layout asserts - the generated trampolines hardcode these offsets
 * (offsets.txt is the single source, this keeps the C side in sync) */
#include <stddef.h>
_Static_assert(offsetof(percpu_t, rmm) == 0x38,
               "percpu_t.rmm offset must match the generated offsets");

/* IRQ control */
void arch_irq_enable(void)  { __asm__ volatile("sti"); }
void arch_irq_disable(void) { __asm__ volatile("cli"); }
void arch_halt(void)        { __asm__ volatile("hlt"); }

void arch_irq_register(uint32_t irq, arch_irq_handler_t fn) {
    if (irq < 16) idt_register_irq((uint8_t)irq, (irq_handler_t)fn);
}
void arch_irq_ack(uint32_t irq) { pic_eoi((uint8_t)irq); }

/* Timer */
void arch_timer_init(uint32_t hz) { pit_init(hz); }

uint64_t arch_time_ms(void) {
    extern uint32_t pit_uptime_secs(void);
    extern uint64_t pit_ticks(void);
    return pit_uptime_secs() * 1000ull + (pit_ticks() % 1000);
}

/* Console (early) */
void arch_console_putc(char c) {
    void display_putchar(char);
    display_putchar(c);
}

/* Spinlocks (single-CPU v1: cli/sti critical sections) */
void spin_init(struct spinlock *l, const char *name) {
    l->locked = 0;
    l->name = name;
}

static inline uint64_t rflags_save(void) {
    uint64_t f;
    __asm__ volatile("pushfq; popq %0" : "=r"(f));
    return f;
}

void spin_lock(struct spinlock *l) {
    while (__atomic_test_and_set(&l->locked, __ATOMIC_ACQUIRE)) {
        /* single-CPU: deadlock means we already hold it - the only way
         * out is an interrupt pre-empting us, keep interrupts live */
        __asm__ volatile("pause");
    }
    l->_irqflags = rflags_save();
    arch_irq_disable();
}

void spin_unlock(struct spinlock *l) {
    __atomic_clear(&l->locked, __ATOMIC_RELEASE);
    if (l->_irqflags & 0x200) arch_irq_enable();
}

/* Recovery */
/*
 * exception_handler_c's register image (see idt.c): regs[0]=r15 ...
 * regs[14]=rax, then the CPU frame: [15]=vec [16]=err [17]=rip [18]=cs
 * [19]=rflags [20]=rsp [21]=ss.  Patching regs[] patches what the stub
 * pops and what iretq restores, the controlled return path.
 */
bool arch_recovery_patch(void *regs_, struct rmm_call_frame *frame,
                         uint64_t err) {
    uint64_t *regs = (uint64_t *)regs_;
    if (!frame || !regs) return false;
    regs[0]  = frame->r15;
    regs[1]  = frame->r14;
    regs[2]  = frame->r13;
    regs[3]  = frame->r12;
    regs[13] = frame->rbx;
    regs[14] = err;                    /* rax = RMM_ERR_MODULE_FAULT  */
    regs[8]  = frame->caller_rbp;
    regs[17] = frame->return_address;  /* rip -> caller               */
    regs[20] = frame->caller_rsp;      /* rsp -> caller's live stack  */
    return true;
}

/* v1 executes the recovery ON the trap stack by patching the frame -
 * we never run code on a module-corrupted stack (stronger than a
 * recovery-stack switch).  The dedicated stack exists for future
 * in-kernel unwinding. */
static uint8_t recovery_stack[4096] __attribute__((aligned(16)));

void arch_run_on_recovery_stack(void *fn, void *arg) {
    (void)fn; (void)arg; (void)recovery_stack;
}

/* x86 I-caches are coherent, TLB invalidation (done per page) is
 * sufficient.  Serialise anyway so no fetch reorders past the mapping. */
void arch_sync_code_range(uint64_t start, uint64_t end) {
    (void)start; (void)end;
    __asm__ volatile("sfence" ::: "memory");
}

/* MMIO / DMA */
void *rmm_mmio_map(uint64_t phys, uint64_t size) {
    uint64_t pages = (size + 4095) / 4096;
    for (uint64_t i = 0; i < pages; i++)
        if (!arch_map(phys + i * 4096, phys + i * 4096,
                      RMM_MAP_READ | RMM_MAP_WRITE | RMM_MAP_DEVICE))
            return NULL;
    return (void *)(uintptr_t)phys;   /* identity window (v1) */
}

void rmm_mmio_unmap(void *va, uint64_t size) {
    uint64_t p = (uint64_t)(uintptr_t)va;
    for (uint64_t i = 0; i < (size + 4095) / 4096; i++) arch_unmap(p + i * 4096);
}

void *rmm_dma_alloc(uint64_t size, uint64_t *phys_out) {
    uint64_t phys = arch_page_alloc_contig((size + 4095) / 4096);
    if (phys_out) *phys_out = phys;
    return (void *)(uintptr_t)phys;   /* identity window (v1) */
}

void rmm_dma_free(void *va, uint64_t size) {
    uint64_t p = (uint64_t)(uintptr_t)va;
    for (uint64_t i = 0; i < (size + 4095) / 4096; i++) arch_page_free(p + i * 4096);
}
