#include "drivers/timer/pit.h"
#include "cpu/idt.h"
#include "drivers/pci/io.h"
#include <stdint.h>

#define PIT_CHANNEL0  0x40
#define PIT_COMMAND   0x43
#define PIT_BASE_HZ   1193182UL

static volatile uint64_t tick_count    = 0;
static          uint32_t ticks_per_sec = 0;   /* 0 = not initialized */

static void pit_irq_handler(void) {
    tick_count++;
}

extern void print_str(const char *);
void pit_init(uint32_t hz) {
    if (hz == 0) hz = 1000;
    ticks_per_sec = hz;
    print_str("[dbg] pit: entry\n");

    uint32_t divisor = (uint32_t)(PIT_BASE_HZ / hz);
    outportb(PIT_COMMAND, 0x36);
    outportb(PIT_CHANNEL0, (uint8_t)(divisor & 0xFF));
    outportb(PIT_CHANNEL0, (uint8_t)((divisor >> 8) & 0xFF));
    print_str("[dbg] pit: programmed\n");

    idt_register_irq(0, pit_irq_handler);
    print_str("[dbg] pit: registered\n");
    pic_unmask(0);
    print_str("[dbg] pit: unmasked\n");
}

uint64_t pit_ticks(void) { return tick_count; }

/* Advance the tick counter from the scheduler's own IRQ0 path (the BORE
 * preemption stub bypasses irq_dispatch, so pit_irq_handler never fires
 * once the scheduler owns vector 0x20). */
void pit_tick_advance(void) { tick_count++; }

void pit_sleep_ms(uint32_t ms) {
    if (!ticks_per_sec) return;
    uint64_t target = tick_count + (uint64_t)ms * ticks_per_sec / 1000 + 1;
    while (tick_count < target)
        __asm__ volatile("hlt");
}

uint32_t pit_uptime_secs(void) {
    if (!ticks_per_sec) return 0;
    return (uint32_t)(tick_count / (uint64_t)ticks_per_sec);
}

/* Returns uptime broken into h/m/s */
void pit_uptime_hms(uint32_t *h, uint32_t *m, uint32_t *s) {
    uint32_t total = pit_uptime_secs();
    *h = total / 3600; total %= 3600;
    *m = total / 60;
    *s = total % 60;
}
