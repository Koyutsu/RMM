/*
 * arch/arm64/arch.c - include/rmm/arch.h implementation, skeleton.
 *
 * Not buildable.  This file documents the surface an ARM64 port must
 * fill in, every function body is a stub.  See ../README.md.
 */
#include <stdint.h>
#include <stddef.h>
#include <rmm/rmm.h>
#include <rmm/module.h>
#include <rmm/fault.h>
#include <rmm/arch.h>

/* per-CPU
 * TPIDR_EL1 holds the per-CPU area base.  struct rmm_cpu_context lives
 * at a fixed offset inside the per-CPU area (arm64 analogue of the x86_64
 * GS layout - offsets must be published for asm entry, like offsets.txt
 * on x86_64). */

struct rmm_cpu_context *arch_cpu_ctx(void) {
    /* uint64_t base; asm("mrs %0, TPIDR_EL1" : "=r"(base));
     * return (struct rmm_cpu_context *)(base + RMM_CTX_OFFSET); */
    return (void *)0;   /* Stub */
}

void arch_percpu_bsp_init(void) { /* Stub: set TPIDR_EL1 (BSP area) */ }
void *arch_percpu_this(void)    { return (void *)0; /* Stub */ }

/* PMM visibility */
uint64_t arch_page_total(void) { return 0; /* Stub */ }
uint64_t arch_page_used(void)  { return 0; /* Stub */ }

/* IRQ control */
void arch_irq_enable(void)  { /* asm volatile("msr daifclr, #2"); */ }
void arch_irq_disable(void) { /* asm volatile("msr daifset, #2"); */ }
void arch_halt(void)        { /* asm volatile("wfi"); */ }
void arch_irq_register(uint32_t irq, arch_irq_handler_t fn) { (void)irq; (void)fn; /* GIC */ }
void arch_irq_ack(uint32_t irq) { (void)irq; /* GIC EOIR */ }

/* Timer */
void arch_timer_init(uint32_t hz) { (void)hz; /* CNTFRQ/CNTP */ }
uint64_t arch_time_ms(void)       { return 0; /* Stub: CNTVCT */ }

/* Fault/recovery - the port-sensitive part
 * Rewrite the machine trap frame so the CPU returns around the broken
 * module: restore callee-saved x19..x28 from the call frame, set
 * ELR_ELx = frame->return_address (PAC-authticated), SP = caller SP,
 * x0 = err.  Mirrors arch/x86_64's arch_recovery_patch. */
int arch_recovery_patch(uint64_t *regs, struct rmm_call_frame *f, uint64_t err) {
    (void)regs; (void)f; (void)err;
    return 0;   /* Stub */
}
