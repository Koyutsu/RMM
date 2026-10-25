#pragma once
/*
 * lapic.h - Local APIC (xAPIC mode)
 */
#include <stdint.h>
#include <stdbool.h>

/* Detect (CPUID), map 0xFEE00000, enable globally + spurious vector,
 * install a spurious stub.  Safe no-op when the CPU has no APIC. */
void lapic_init(void);

bool     lapic_present(void);
uint32_t lapic_id(void);
void     lapic_eoi(void);

/* IPIs (all-excluding-self shorthand) used by the SMP boot code. */
void lapic_send_init(void);            /* INIT, assert + deassert */
void lapic_send_sipi(uint8_t vector);  /* Startup IPI, page = vec << 12 */
