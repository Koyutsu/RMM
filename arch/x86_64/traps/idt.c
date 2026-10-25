#include "cpu/idt.h"
#include "console/print.h"
#include "console/display.h"
#include "exec/process.h"
#include "drivers/pci/io.h"
#include "cpu/percpu.h"
#include "drivers/memory/vmm.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <rmm/rmm.h>
#include <rmm/fault.h>
#include <rmm/arch.h>
#include <rmm/module.h>

/* IDT entry */
typedef struct __attribute__((packed)) {
    uint16_t offset_low;
    uint16_t selector;      /* kernel code segment */
    uint8_t  ist;           /* interrupt stack table index (0 = none) */
    uint8_t  type_attr;     /* type + DPL + present */
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t reserved;
} idt_entry_t;

typedef struct __attribute__((packed)) {
    uint16_t limit;
    uint64_t base;
} idtr_t;

#define IDT_SIZE 256

static idt_entry_t idt[IDT_SIZE];
static idtr_t      idtr;

/* IRQ handler table (C functions) */
static irq_handler_t irq_handlers[16];

/* 8259 PIC ports */
#define PIC1_CMD  0x20
#define PIC1_DATA 0x21
#define PIC2_CMD  0xA0
#define PIC2_DATA 0xA1
#define PIC_EOI   0x20

static void io_wait(void) {
    outportb(0x80, 0);   /* write to unused port - ~1 us delay */
}

/* PIC remap */
static void pic_remap(void) {
    /* Save masks */
    uint8_t m1 = inportb(PIC1_DATA);
    uint8_t m2 = inportb(PIC2_DATA);

    /* Start init sequence (cascade mode) */
    outportb(PIC1_CMD,  0x11); io_wait();
    outportb(PIC2_CMD,  0x11); io_wait();

    /* Vector offsets */
    outportb(PIC1_DATA, IRQ_BASE);      io_wait();   /* IRQ0-7  -> 0x20-0x27 */
    outportb(PIC2_DATA, IRQ_BASE + 8);  io_wait();   /* IRQ8-15 -> 0x28-0x2F */

    /* Cascade wiring */
    outportb(PIC1_DATA, 0x04); io_wait();   /* slave on IRQ2 */
    outportb(PIC2_DATA, 0x02); io_wait();   /* slave cascade id */

    /* 8086 mode */
    outportb(PIC1_DATA, 0x01); io_wait();
    outportb(PIC2_DATA, 0x01); io_wait();

    /* Restore masks */
    outportb(PIC1_DATA, m1);
    outportb(PIC2_DATA, m2);
}

void pic_eoi(uint8_t irq) {
    if (irq >= 8) outportb(PIC2_CMD, PIC_EOI);
    outportb(PIC1_CMD, PIC_EOI);
}

void pic_mask(uint8_t irq) {
    uint16_t port = (irq < 8) ? PIC1_DATA : PIC2_DATA;
    uint8_t  bit  = irq & 7;
    outportb(port, inportb(port) | (uint8_t)(1 << bit));
}

/* unmasking a slave-PIC IRQ (8..15) must also unmask the
 * cascade line (IRQ2) on the master, after pic_remap restores the
 * firmware masks, IRQ2 can be masked, which silently kills all slave
 * interrupts (RTC IRQ8, PS/2 mouse IRQ12, ...) while master IRQs (PIT,
 * keyboard) keep working.  Observed as: PS/2 mouse packets queued by
 * the i8042 but IRQ12 never raised. */
void pic_unmask(uint8_t irq) {
    if (irq >= 8) {
        /* open the cascade on the master, then the line on the slave */
        outportb(PIC1_DATA, inportb(PIC1_DATA) & (uint8_t)~(1u << 2));
        outportb(PIC2_DATA, inportb(PIC2_DATA) & (uint8_t)~(1u << (irq & 7)));
    } else {
        outportb(PIC1_DATA, inportb(PIC1_DATA) & (uint8_t)~(1u << irq));
    }
}

/* IDT entry helpers */
#define IDT_TYPE_INTERRUPT 0x8E   /* present, ring0, 64-bit interrupt gate */
#define IDT_TYPE_TRAP      0x8F   /* present, ring0, 64-bit trap gate      */
#define KERNEL_CS          0x08

static void idt_set_gate(uint8_t vec, uintptr_t handler,
                          uint8_t type, uint8_t ist) {
    idt[vec].offset_low  = (uint16_t)(handler & 0xFFFF);
    idt[vec].selector    = KERNEL_CS;
    idt[vec].ist         = ist;
    idt[vec].type_attr   = type;
    idt[vec].offset_mid  = (uint16_t)((handler >> 16) & 0xFFFF);
    idt[vec].offset_high = (uint32_t)((handler >> 32) & 0xFFFFFFFF);
    idt[vec].reserved    = 0;
}

/* Exception stubs (declared in idt_stubs.asm) */
/* We define them inline in C using naked-like wrappers. Because x86-64 gcc
 * doesn't support __attribute__((naked)) reliably for complex stubs, we use
 * a macro to emit a minimal asm stub that saves state and calls a C handler. */

/* Exception frame pushed by CPU (+ our vector/error) */
typedef struct __attribute__((packed)) {
    uint64_t rip, cs, rflags, rsp, ss;   /* CPU-pushed */
} exception_frame_t;

static const char *exception_names[] = {
    "Division by Zero",       "Debug",
    "NMI",                    "Breakpoint",
    "Overflow",               "Bound Range Exceeded",
    "Invalid Opcode",         "Device Not Available",
    "Double Fault",           "Coprocessor Segment Overrun",
    "Invalid TSS",            "Segment Not Present",
    "Stack-Segment Fault",    "General Protection Fault",
    "Page Fault",             "Reserved",
    "x87 FPU Error",          "Alignment Check",
    "Machine Check",          "SIMD FP Exception",
    "Virtualization",         "Control Protection",
    "Reserved","Reserved","Reserved","Reserved","Reserved","Reserved",
    "Hypervisor Injection",   "VMM Communication",
    "Security Exception",     "Reserved",
};

static void dump_page_for(uint64_t cr3, uint64_t addr, const char *label) {
    uint64_t pml4e = ((uint64_t*)cr3)[(addr >> 39) & 0x1FF];
    display_printf("%s PML4[%u]=%p\n", label, (unsigned)((addr >> 39) & 0x1FF), (void*)pml4e);
    if (pml4e & 1) {
        uint64_t *pdpt = (uint64_t*)(uintptr_t)(pml4e & 0x000FFFFFFFFFF000ULL);
        uint64_t pdpte = pdpt[(addr >> 30) & 0x1FF];
        display_printf("%s PDPT[%u]=%p\n", label, (unsigned)((addr >> 30) & 0x1FF), (void*)pdpte);
        if (pdpte & 1) {
            uint64_t *pd = (uint64_t*)(uintptr_t)(pdpte & 0x000FFFFFFFFFF000ULL);
            uint64_t pde = pd[(addr >> 21) & 0x1FF];
            display_printf("%s PD[%u]=%p\n", label, (unsigned)((addr >> 21) & 0x1FF), (void*)pde);
            if ((pde & 1) && !(pde & 0x80)) {
                uint64_t *pt = (uint64_t*)(uintptr_t)(pde & 0x000FFFFFFFFFF000ULL);
                uint64_t pte = pt[(addr >> 12) & 0x1FF];
                display_printf("%s PT[%u]=%p\n", label, (unsigned)((addr >> 12) & 0x1FF), (void*)pte);
            }
        }
    }
}

/* Called from assembly stubs.
 * regs points to saved GPR array in the stack frame:
 *   regs[0]=r15, [1]=r14, [2]=r13, [3]=r12, [4]=r11,
 *   [5]=r10, [6]=r9, [7]=r8, [8]=rbp, [9]=rdi,
 *   [10]=rsi, [11]=rdx, [12]=rcx, [13]=rbx, [14]=rax
 */
/* RMM interrupt-depth tracking: IRQ stubs call irq_dispatch, the
 * count guards demand-loading and fault attribution. */
void irq_dispatch(uint8_t irq);

void exception_handler_c(uint64_t vec, uint64_t error,
                          uint64_t rip, uint64_t cs,
                          uint64_t rflags, uint64_t rsp,
                          uint64_t *regs) {
    (void)rflags;

    /* RMM V2 routing
     * Build the common trap frame and let the architecture-independent
     * policy decide: demand load -> retry, module fault -> controlled
     * return, user fault -> process kill, core fault -> panic. */
    static struct rmm_trap_frame tf;   /* static: no stack pressure */
    tf.pc = rip;
    tf.sp = rsp;
    tf.flags = rflags;
    for (int i = 0; i < RMM_TRAP_REGS_MAX; i++) tf.regs[i] = 0;
    /* regs[0]=r15 ... regs[14]=rax (see stub layout above) */
    for (int i = 0; i < 15; i++) tf.regs[i] = regs[i];
    tf.fault_address = 0;
    if (vec == 14) __asm__ volatile("mov %%cr2, %0" : "=r"(tf.fault_address));
    switch (vec) {
    case 14: tf.exception = RMM_EXC_PAGE_FAULT; break;
    case 6:  tf.exception = RMM_EXC_ILLEGAL_INSTR; break;
    case 3:
    case 1:  tf.exception = RMM_EXC_BREAKPOINT; break;
    case 13: tf.exception = RMM_EXC_SFI_VIOLATION; break;  /* GP */
    default: tf.exception = RMM_EXC_GENERAL; break;
    }
    tf.access_type = (error & 2) ? RMM_ACCESS_WRITE
                   : (error & 16) ? RMM_ACCESS_EXEC : RMM_ACCESS_READ;
    tf.error_code = (uint32_t)error;
    tf.from_user = (cs & 3) == 3;
    tf.module = NULL;

    enum rmm_fault_result r = rmm_handle_exception(&tf);
    if (r == RMM_FAULT_HANDLED) return;          /* retry the instruction */
    if (r == RMM_FAULT_MODULE_CRASH) return;     /* frame already patched */
    /* RMM_FAULT_FATAL falls through to the policies below */

    /* User-mode fault -> kill the offending process (SIGSEGV style)
     * previously we printed a panic dump and continued
     * (and the dump itself dereferenced the user stack, which could
     * nested-fault the kernel).  Linux semantics: the process dies, the
     * system lives.  proc_exit never returns, it resumes the parent. */
    if ((cs & 3) == 3 && g_current_proc) {
        display_set_color(0xFFFFFF, 0xC00000);
        display_puts("\n[rmm] ");
        display_printf("pid %d: fatal exception %d (err=0x%x) at rip=%p",
                       g_current_proc->pid, (int)vec,
                       (unsigned)error, (void *)rip);
        if (vec == 14) {
            uint64_t cr2;
            __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
            display_printf(" fault-addr=%p", (void *)cr2);
        }
        display_printf(" rsi=%p rbx=%p rsp=%p",
                       (void *)regs[10], (void *)regs[13], (void *)rsp);
        {
            /* dump 4 qwords at the faulting user rsp (page-guarded) */
            uint64_t va = rsp & ~7ULL;
            for (int q = 0; q < 4; q++, va += 8) {
                if (!vmm_virt_to_phys(va & ~0xFFFULL)) break;
                display_printf(" [%p]=%p", (void *)va,
                               *(void **)(uintptr_t)va);
            }
            display_puts("\n");
        }
        display_puts("\n");
        display_set_color(0xFFFFFF, 0x000000);
        proc_exit(g_current_proc, 128 + 11);   /* 128+SIGSEGV */
        /* noreturn */
    }

    /* Kernel-mode fault: real kernel bug, full dump + halt (below). */
    (void)cs;

    /* Don't clear the screen, we want to see the last user output */
    display_set_color(0xFFFFFF, 0xFF0000);
    display_puts("\n\n*** KERNEL PANIC ***\n\n");

    display_puts("Exception: ");
    if (vec < 32) display_puts(exception_names[vec]);
    else          display_puts("Unknown");
    display_printf(" (vector %d)\n", (int)vec);

    display_printf("Error code : 0x%x\n", (unsigned)error);
    display_printf("RIP: %p  CS=%p\n", (void*)rip, (void*)cs);
    display_printf("RSP: %p  RFLAGS=%p\n", (void*)rsp, (void*)rflags);

    /* Saved registers */
    display_puts("\nRegister dump:\n");
    display_printf("RAX=%p  RBX=%p  RCX=%p\n", (void*)regs[14], (void*)regs[13], (void*)regs[12]);
    display_printf("RDX=%p  RSI=%p  RDI=%p\n", (void*)regs[11], (void*)regs[10], (void*)regs[9]);
    display_printf("RBP=%p  R8 =%p  R9 =%p\n", (void*)regs[8], (void*)regs[7], (void*)regs[6]);
    display_printf("R10=%p  R11=%p  R12=%p\n", (void*)regs[5], (void*)regs[4], (void*)regs[3]);
    display_printf("R13=%p  R14=%p  R15=%p\n", (void*)regs[2], (void*)regs[1], (void*)regs[0]);

    /* Dump user stack (if user-mode fault), but only pages that are
     * actually mapped, so the dump can never nested-fault the kernel. */
    if (error & 4) {
        uint64_t phys = vmm_virt_to_phys(rsp & ~0xFFFULL);
        if (phys) {
            uint64_t *user_stack = (uint64_t *)(uintptr_t)rsp;
            display_puts("\nUser stack (top 32 bytes):\n");
            for (int i = 0; i < 4; i++) {
                uint64_t va = (uint64_t)(uintptr_t)(user_stack + i);
                uint64_t p2 = vmm_virt_to_phys(va & ~0xFFFULL);
                if (!p2) break;
                display_printf("  [%d] %p\n", i, (void *)user_stack[i]);
            }
        }
    }

    if (vec == 14) {
        uint64_t cr2;
        __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
        uint64_t cr3;
        __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
        display_printf("\nFault addr : 0x%x\n", (unsigned)cr2);
        display_printf("CR3: %p\n", (void*)cr3);
        display_puts("Reason     : ");
        if (!(error & 1)) display_puts("not present ");
        if (  error & 2)  display_puts("write ");
        if (  error & 4)  display_puts("user-mode ");
        if (  error & 8)  display_puts("reserved-bit ");
        if (  error & 16) display_puts("instruction-fetch ");
        display_puts("\n");

        dump_page_for(cr3, cr2, "FAULT");
        dump_page_for(cr3, rip, "RIP  ");

        /* Dump raw bytes at RIP's physical address */
        {
            int rip_mapped = 0;
            uint64_t rip_phys = 0;
            uint64_t l4 = ((uint64_t*)(uintptr_t)cr3)[(rip >> 39) & 0x1FF];
            if (l4 & 1) {
                uint64_t l3 = ((uint64_t*)(uintptr_t)(l4 & 0x000FFFFFFFFFF000))[(rip >> 30) & 0x1FF];
                if (l3 & 1) {
                    uint64_t l2 = ((uint64_t*)(uintptr_t)(l3 & 0x000FFFFFFFFFF000))[(rip >> 21) & 0x1FF];
                    if (l2 & 1) {
                        if (l2 & 0x80) {
                            rip_phys = (l2 & 0x000FFFFFE00000ULL) + (rip & 0x1FFFFF);
                            rip_mapped = 1;
                        } else {
                            uint64_t l1 = ((uint64_t*)(uintptr_t)(l2 & 0x000FFFFFFFFFF000))[(rip >> 12) & 0x1FF];
                            if (l1 & 1) {
                                rip_phys = (l1 & 0x000FFFFFFFFFF000) + (rip & 0xFFF);
                                rip_mapped = 1;
                            }
                        }
                    }
                }
            }
            display_printf("RIP phys addr: %p (mapped=%d)\n", (void*)rip_phys, rip_mapped);
            if (rip_mapped) {
                uint8_t *p = (uint8_t*)(uintptr_t)rip_phys;
                display_puts("Bytes at RIP: ");
                for (int i = 0; i < 16; i++)
                    display_printf("%02x ", p[i]);
                display_puts("\n");
            }
        }

        /* Dump 32 bytes before the return address to find the call instruction */
        if ((error & 4) && rip == 0) {
            uint64_t ret_addr = ((uint64_t*)(uintptr_t)rsp)[0];
            uint64_t start = ret_addr - 24;
            /* Walk page tables for the start address */
            int call_mapped = 0;
            uint64_t call_phys = 0;
            uint64_t cv = start;
            uint64_t cl4 = ((uint64_t*)(uintptr_t)cr3)[(cv >> 39) & 0x1FF];
            if (cl4 & 1) {
                uint64_t cl3 = ((uint64_t*)(uintptr_t)(cl4 & 0x000FFFFFFFFFF000))[(cv >> 30) & 0x1FF];
                if (cl3 & 1) {
                    uint64_t cl2 = ((uint64_t*)(uintptr_t)(cl3 & 0x000FFFFFFFFFF000))[(cv >> 21) & 0x1FF];
                    if (cl2 & 1) {
                        if (cl2 & 0x80) {
                            call_phys = (cl2 & 0x000FFFFFE00000ULL) + (cv & 0x1FFFFF);
                            call_mapped = 1;
                        } else {
                            uint64_t cl1 = ((uint64_t*)(uintptr_t)(cl2 & 0x000FFFFFFFFFF000))[(cv >> 12) & 0x1FF];
                            if (cl1 & 1) {
                                call_phys = (cl1 & 0x000FFFFFFFFFF000) + (cv & 0xFFF);
                                call_mapped = 1;
                            }
                        }
                    }
                }
            }
            if (call_mapped) {
                uint8_t *p = (uint8_t*)(uintptr_t)call_phys;
                display_printf("Before ret=%p (phys=%p): ", (void*)ret_addr, (void*)call_phys);
                for (int i = 0; i < 32; i++)
                    display_printf("%02x ", p[i]);
                display_puts("\n");
            }
        }
    }

    display_puts("\nSystem halted. Please reset.\n");
    __asm__ volatile("cli; hlt");
    while (1) {}
}

/* IRQ dispatcher called from asm stubs.
 * track interrupt depth, demand loading is forbidden from
 * this context, and module IRQ handlers only run when their module is
 * resident (the manager's rmm_module_prefetch covers load order). */

/* kernel stack guard (loud failure)
 * The boot/runtime stack lives in .bss (main.asm).  The v1 16 KiB stack
 * overflowed silently: rsp descended through the boot page tables into
 * g_percpu[] before anything noticed, and the canary's "heal" write
 * through the resulting wild self pointer corrupted code.  The lowest
 * 64 bytes of the stack are painted with a magic pattern at boot, PIT
 * ticks sample it and halt before the overflow reaches page tables. */
extern uint8_t stack_bottom[];
extern uint8_t stack_top[];
#define STACK_GUARD_MAGIC 0x524D4D3247554152ULL   /* "RMM2GUAR" */

void stack_guard_init(void) {
    uint64_t *g = (uint64_t *)stack_bottom;
    for (unsigned i = 0; i < 8; i++) g[i] = STACK_GUARD_MAGIC;
}

static bool stack_guard_ok(void) {
    const uint64_t *g = (const uint64_t *)stack_bottom;
    for (unsigned i = 0; i < 8; i++)
        if (g[i] != STACK_GUARD_MAGIC) return false;
    return true;
}

void irq_dispatch(uint8_t irq) {
    percpu_t *pc = percpu_this();
    /* percpu_this() returns the self field, validate it against
     * g_percpu[] before trusting or writing through it.  A wild value
     * means the kernel stack already blew through .bss: report and
     * halt.  Never "heal" through a garbage pointer. */
    bool pc_valid = false;
    for (int i = 0; i < SMP_MAX_CPUS; i++)
        if (pc == &g_percpu[i]) { pc_valid = true; break; }
    if (!pc_valid) {
        display_puts("\n[rmm] FATAL: percpu self outside g_percpu[] - kernel stack overflow\n");
        __asm__ volatile("cli; hlt");
        while (1) {}
    }
    if (pc->self != (uint64_t)(uintptr_t)pc) {
        printf("[canary] self=%p expect=%p irq=%u\n",
               (void *)(uintptr_t)pc->self, (void *)pc, irq);
        pc->self = (uint64_t)(uintptr_t)pc;   /* restore to keep going */
    }
    struct rmm_cpu_context *cpu = arch_cpu_ctx();
    cpu->interrupt_depth++;
    if (irq_handlers[irq])
        irq_handlers[irq]();
    cpu->interrupt_depth--;
    pic_eoi(irq);
    /* stack-guard sample on the PIT tick, every 512 ticks */
    if (irq == 0 && (pc->ticks & 511) == 0 && !stack_guard_ok()) {
        display_puts("\n[rmm] FATAL: kernel stack guard clobbered - overflow in progress\n");
        __asm__ volatile("cli; hlt");
        while (1) {}
    }
}

/* Assembly stubs */
/*
 * We use a macro to generate a stub for each exception/IRQ.
 * Each stub pushes a dummy error code (for exceptions that don't have one),
 * the vector number, saves all GPRs, calls the C handler, restores, and irets.
 *
 * For x86-64 we can use __attribute__((interrupt)) when targeting GCC 6+.
 * Since we're on GCC 13, we'll use it for IRQs and a raw asm block for
 * exceptions (which need the error code).
 */

/* Exception stubs - with and without error codes */
#define EXC_STUB_NO_ERR(vec)                                        \
    static void __attribute__((naked)) exc_stub_##vec(void) {       \
        __asm__ volatile(                                            \
            "pushq $0\n\t"          /* dummy error code */          \
            "pushq $" #vec "\n\t"   /* vector */                    \
            "jmp exc_common\n\t"                                     \
        );                                                           \
    }

#define EXC_STUB_ERR(vec)                                            \
    static void __attribute__((naked)) exc_stub_##vec(void) {        \
        __asm__ volatile(                                             \
            /* error code already on stack by CPU */                  \
            "pushq $" #vec "\n\t"                                     \
            "jmp exc_common\n\t"                                      \
        );                                                            \
    }

/* Common exception path */
__attribute__((naked,used)) static void exc_common(void) {
    __asm__ volatile(
        /* Stack at entry (bottom->top):
           [vec] [err] rip cs rflags rsp ss  */
        "push %%rax\n\t"
        "push %%rbx\n\t"
        "push %%rcx\n\t"
        "push %%rdx\n\t"
        "push %%rsi\n\t"
        "push %%rdi\n\t"
        "push %%rbp\n\t"
        "push %%r8\n\t"
        "push %%r9\n\t"
        "push %%r10\n\t"
        "push %%r11\n\t"
        "push %%r12\n\t"
        "push %%r13\n\t"
        "push %%r14\n\t"
        "push %%r15\n\t"

        /* Args: vec, error, rip, cs, rflags, rsp */
        /* After pushing 15 regs + vec + err + cpu frame(5): offset in stack:
           rsp+0..7*8  = r15..rax
           rsp+15*8    = vec
           rsp+16*8    = err
           rsp+17*8    = rip
           rsp+18*8    = cs
           rsp+19*8    = rflags
           rsp+20*8    = rsp_orig
           rsp+21*8    = ss        */
        "mov 15*8(%%rsp), %%rdi\n\t"   /* vec   */
        "mov 16*8(%%rsp), %%rsi\n\t"   /* error */
        "mov 17*8(%%rsp), %%rdx\n\t"   /* rip   */
        "mov 18*8(%%rsp), %%rcx\n\t"   /* cs    */
        "mov 19*8(%%rsp), %%r8\n\t"    /* rflags*/
        "mov 20*8(%%rsp), %%r9\n\t"    /* rsp   */
        "push %%rsp\n\t"               /* 7th arg: pointer to saved regs */
        "call exception_handler_c\n\t"
        "add $8, %%rsp\n\t"            /* pop regs pointer */

        "pop %%r15\n\t"
        "pop %%r14\n\t"
        "pop %%r13\n\t"
        "pop %%r12\n\t"
        "pop %%r11\n\t"
        "pop %%r10\n\t"
        "pop %%r9\n\t"
        "pop %%r8\n\t"
        "pop %%rbp\n\t"
        "pop %%rdi\n\t"
        "pop %%rsi\n\t"
        "pop %%rdx\n\t"
        "pop %%rcx\n\t"
        "pop %%rbx\n\t"
        "pop %%rax\n\t"
        "add $16, %%rsp\n\t"   /* pop vec + error */
        "iretq\n\t"
        ::: "memory"
    );
}

/* Generate exception stubs */
EXC_STUB_NO_ERR(0)   EXC_STUB_NO_ERR(1)   EXC_STUB_NO_ERR(2)
EXC_STUB_NO_ERR(3)   EXC_STUB_NO_ERR(4)   EXC_STUB_NO_ERR(5)
EXC_STUB_NO_ERR(6)   EXC_STUB_NO_ERR(7)
EXC_STUB_ERR(8)
EXC_STUB_NO_ERR(9)
EXC_STUB_ERR(10)  EXC_STUB_ERR(11)  EXC_STUB_ERR(12)
EXC_STUB_ERR(13)  EXC_STUB_ERR(14)
EXC_STUB_NO_ERR(15) EXC_STUB_NO_ERR(16)
EXC_STUB_ERR(17)
EXC_STUB_NO_ERR(18) EXC_STUB_NO_ERR(19)
EXC_STUB_NO_ERR(20) EXC_STUB_NO_ERR(21)

/* IRQ stubs */
#define IRQ_STUB(n)                                                   \
    static void __attribute__((naked)) irq_stub_##n(void) {          \
        __asm__ volatile(                                             \
            "push %%rax\npush %%rcx\npush %%rdx\n"                   \
            "push %%rsi\npush %%rdi\npush %%r8\n"                    \
            "push %%r9\npush %%r10\npush %%r11\n"                    \
            "mov $" #n ", %%edi\n"                                    \
            "call irq_dispatch\n"                                     \
            "pop %%r11\npop %%r10\npop %%r9\n"                       \
            "pop %%r8\npop %%rdi\npop %%rsi\n"                       \
            "pop %%rdx\npop %%rcx\npop %%rax\n"                      \
            "iretq\n"                                                 \
            ::: "memory"                                              \
        );                                                            \
    }

IRQ_STUB(0)  IRQ_STUB(1)  IRQ_STUB(2)  IRQ_STUB(3)
IRQ_STUB(4)  IRQ_STUB(5)  IRQ_STUB(6)  IRQ_STUB(7)
IRQ_STUB(8)  IRQ_STUB(9)  IRQ_STUB(10) IRQ_STUB(11)
IRQ_STUB(12) IRQ_STUB(13) IRQ_STUB(14) IRQ_STUB(15)

/* idt_init */
void idt_init(void) {
    /* Clear IDT */
    for (int i = 0; i < IDT_SIZE; i++) {
        idt[i] = (idt_entry_t){0};
        if (i < 16) irq_handlers[i] = (irq_handler_t)0;
    }

    /* Install exception handlers */
#define SET_EXC(v) idt_set_gate(v, (uintptr_t)exc_stub_##v, IDT_TYPE_TRAP, 0)
    SET_EXC(0);  SET_EXC(1);  SET_EXC(2);  SET_EXC(3);
    SET_EXC(4);  SET_EXC(5);  SET_EXC(6);  SET_EXC(7);
    SET_EXC(8);  SET_EXC(9);  SET_EXC(10); SET_EXC(11);
    SET_EXC(12); SET_EXC(13); SET_EXC(14); SET_EXC(15);
    SET_EXC(16); SET_EXC(17); SET_EXC(18); SET_EXC(19);
    SET_EXC(20); SET_EXC(21);
#undef SET_EXC

    /* Install IRQ stubs at vectors 0x20-0x2F */
#define SET_IRQ(n) idt_set_gate(IRQ_BASE+n, (uintptr_t)irq_stub_##n, IDT_TYPE_INTERRUPT, 0)
    SET_IRQ(0);  SET_IRQ(1);  SET_IRQ(2);  SET_IRQ(3);
    SET_IRQ(4);  SET_IRQ(5);  SET_IRQ(6);  SET_IRQ(7);
    SET_IRQ(8);  SET_IRQ(9);  SET_IRQ(10); SET_IRQ(11);
    SET_IRQ(12); SET_IRQ(13); SET_IRQ(14); SET_IRQ(15);
#undef SET_IRQ

    /* Remap PIC and mask all IRQs initially */
    pic_remap();
    outportb(PIC1_DATA, 0xFF);   /* mask all master IRQs */
    outportb(PIC2_DATA, 0xFF);   /* mask all slave IRQs  */

    /* Load IDT */
    idtr.limit = (uint16_t)(sizeof(idt) - 1);
    idtr.base  = (uint64_t)(uintptr_t)idt;
    __asm__ volatile("lidt %0" :: "m"(idtr) : "memory");

    /* Enable interrupts */
    __asm__ volatile("sti");
}

void idt_register_irq(uint8_t irq_num, irq_handler_t handler) {
    if (irq_num < 16)
        irq_handlers[irq_num] = handler;
}

/*
 * Install the BORE scheduler's full-context preemption stub on IRQ0.
 * The stub pushes all 15 GP registers (the regular IRQ_STUB(0) keeps
 * only the volatile ones) and calls sched_irq0_c, which may switch
 * tasks and never return to the stub.  Bypasses irq_dispatch entirely:
 * EOI is sent inside sched_irq0_c.
 */
void idt_install_irq0(void *stub) {
    idt_set_gate(IRQ_BASE + 0, (uintptr_t)stub, IDT_TYPE_INTERRUPT, 0);
}

/*
 * Register the INT 0x80 syscall handler.
 * Uses a trap gate (IF stays enabled during syscall) with DPL=3 so that
 * future user-mode code can invoke it without a GPF.
 */
void idt_register_syscall(void *stub) {
    /* 0xEF = present | DPL=3 | 64-bit trap gate */
    idt_set_gate(0x80, (uintptr_t)stub, 0xEF, 0);
}