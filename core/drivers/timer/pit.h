#pragma once
#include <stdint.h>

/*
 * PIT - 8253/8254 Programmable Interval Timer
 *
 * Configured as a 1000 Hz tick (1 ms resolution).
 * After pit_init(), a global tick counter increments every IRQ0.
 */

void     pit_init(uint32_t hz);         /* call after idt_init()   */
uint64_t pit_ticks(void);               /* total ticks since boot  */
void     pit_tick_advance(void);        /* tick bump from BORE IRQ0 path */
void     pit_sleep_ms(uint32_t ms);     /* busy-sleep (interrupts must be on) */
uint32_t pit_uptime_secs(void);         /* whole seconds since boot */
void pit_uptime_hms(uint32_t *h, uint32_t *m, uint32_t *s);
