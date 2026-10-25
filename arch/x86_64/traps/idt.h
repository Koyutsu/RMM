#pragma once
#include <stdint.h>

/*
 * IDT - Interrupt Descriptor Table
 *
 * Handles:
 *   0x00-0x1F  CPU exceptions  (page fault, GPF, divide-by-zero, etc.)
 *   0x20       PIT timer IRQ   (remapped from IRQ0)
 *   0x21       PS/2 keyboard   (remapped from IRQ1)
 *   0x22-0x2F  other IRQs      (remapped, acknowledged but ignored)
 *
 * The 8259 PIC is remapped so IRQ0-15 -> vectors 0x20-0x2F,
 * avoiding collision with CPU exception vectors 0x00-0x1F.
 */

/* IRQ vector base after PIC remap */
#define IRQ_BASE        0x20
#define IRQ_TIMER       (IRQ_BASE + 0)
#define IRQ_KEYBOARD    (IRQ_BASE + 1)

void idt_init(void);
void stack_guard_init(void);   /* paint the stack-bottom magic (see idt.c) */

/* Register a handler for a given vector.
 * handler is a C function: void handler(void) */
typedef void (*irq_handler_t)(void);
void idt_register_irq(uint8_t irq_num, irq_handler_t handler);

/* Send End-Of-Interrupt to the PIC */
void pic_eoi(uint8_t irq);

/* Enable / disable a specific IRQ line */
void pic_unmask(uint8_t irq);
void pic_mask(uint8_t irq);

/*
 * Register the INT 0x80 syscall stub.
 * Called once by syscall_init() after IDT is loaded.
 * Uses DPL=3 trap gate so future ring-3 code can INT 0x80 without GPF.
 */
void idt_register_syscall(void *stub);