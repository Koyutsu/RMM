#pragma once
/*
 * arch.h - the entire architecture contract of RMM V2.
 *
 * Everything outside arch/ compiles against this header only.  A port to
 * a new architecture implements every operation below (see
 * arch/README.md for the checklist, arch/arm64/ is a concrete skeleton).
 *
 * The contract is deliberately small: trap entry/fill, page tables,
 * TLB sync, per-CPU context, context switching, IRQ control,
 * timer and console.
 */
#include <rmm/rmm.h>
#include <rmm/fault.h>

/* Boot */
/* Early C entry from the arch bootstrap.  Receives the bootloader info
 * blob (multiboot2 info on x86_64, DTB on ARM64, etc.) as an opaque
 * physical address.  Never returns. */
void arch_main(uint64_t boot_info);

/* Console (early print, must work before any allocation) */
void arch_console_putc(char c);

/* Physical memory */
/* Parse the bootloader memory map, returns usable RAM in bytes (0 = map
 * invalid).  Fills the arch-private PMM state. */
uint64_t arch_mem_init(uint64_t boot_info, uint64_t kernel_phys_start,
                       uint64_t kernel_phys_end);
uint64_t arch_page_alloc(void);            /* one zeroed 4 KiB page, 0=OOM  */
uint64_t arch_page_alloc_contig(size_t n); /* n zeroed contiguous pages     */
void     arch_page_free(uint64_t phys);
uint64_t arch_page_used(void);
uint64_t arch_page_total(void);

/* Virtual memory / page tables */
/* Flags are arch-neutral intents, each port maps them to its PTE bits. */
#define RMM_MAP_READ    0x1ull
#define RMM_MAP_WRITE   0x2ull
#define RMM_MAP_EXEC    0x4ull
#define RMM_MAP_USER    0x8ull
#define RMM_MAP_DEVICE  0x10ull   /* MMIO: never cache, never speculate   */

bool     arch_map(uint64_t va, uint64_t phys, uint64_t flags);
void     arch_unmap(uint64_t va);
void     arch_protect(uint64_t va, uint64_t flags);
uint64_t arch_translate(uint64_t va);      /* 0 if unmapped                   */
uint64_t arch_root_switch(uint64_t root);  /* CR3 / TTBR0_EL1 ..., returns prev */
uint64_t arch_root_current(void);
/* Broadcast TLB invalidation for a VA range (SMP-safe) -. */
void     arch_tlb_sync_range(uint64_t start, uint64_t end);

/* Module slot regions */
/* Ensure the page-table *interior* for the module slot region exists.
 * Slots live in a fixed, dedicated region of the VA space (x86_64:
 * PML4[510]), each port documents where and guarantees nothing else
 * uses it. */
void arch_slot_region_init(void);

/* Per-CPU */
/* Install `area` as this CPU's per-CPU base (GS base / TPIDR_EL1).
 * Returns the arch per-CPU struct, the RMM cpu context is embedded. */
struct rmm_cpu_context;
void  arch_percpu_init(void *area, size_t size, uint32_t cpu_id);
/* BSP per-CPU bring-up - must run before interrupts open. */
void  arch_percpu_bsp_init(void);
void *arch_percpu_this(void);
struct rmm_cpu_context *arch_cpu_ctx(void);   /* cheap per-CPU ctx access */

/* Context switch (bootstrap scheduler) */
/* Save/restore a user-mode task context.  The exact mechanics are arch
 * private (x86_64: BORE sched_ctx in process_t, ARM64: callee-saved +
 * ELR/SPSR).  The core only orchestrates. */
void arch_sched_start_first(void *proc);      /* never returns             */
void arch_irq_enable(void);
void arch_irq_disable(void);
void arch_halt(void);

/* Timer */
void     arch_timer_init(uint32_t hz);       /* periodic tick + IRQ        */
uint64_t arch_time_ms(void);                 /* monotonic milliseconds    */

/* Interrupt control for the module system */
/* Register a module-owned IRQ handler.  The manager records the owning
 * module so dispatch can verify residency. */
typedef void (*arch_irq_handler_t)(void);
void arch_irq_register(uint32_t irq, arch_irq_handler_t fn);
void arch_irq_ack(uint32_t irq);

/* Atomic/spin primitives shared by core code */
/* struct spinlock is defined in rmm.h, ports may upgrade internally. */
void spin_init(struct spinlock *l, const char *name);
void spin_lock(struct spinlock *l);
void spin_unlock(struct spinlock *l);

/* Recovery stack */
/* Switch this CPU to its trusted recovery stack and call fn(arg).  Used
 * by the arch trap path when a module fault requires the controlled
 * return.  The recovery stack never touches module memory. */
void arch_run_on_recovery_stack(void *fn, void *arg);

/* Controlled module-fault return: patch the machine trap frame
 * so that the iret returns to `frame`'s recorded caller with `err` in
 * the ABI return register, restoring the frame's saved callee-saved
 * registers.  `regs` is the native register image captured by the arch
 * trap entry.  Returns false when patching is impossible (caller must
 * panic). */
bool arch_recovery_patch(void *regs, struct rmm_call_frame *frame,
                         uint64_t err);

/* Cache/TLB synchronization for code replacement */
/* After replacing module code pages: flush instruction-side state.
 * x86_64: TLB invalidation suffices (coherent I-cache).
 * ARM64:  DSB, TLBI, DSB, ISB + cache clean to PoU.         */
void arch_sync_code_range(uint64_t start, uint64_t end);
