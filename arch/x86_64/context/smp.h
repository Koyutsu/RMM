#pragma once
/*
 * smp.h - application processor bring-up
 */
#include <stdint.h>
#include <stdbool.h>

/* Copy the trampoline to 0x7000, INIT/SIPI every AP found in the MADT and
 * wait for them to park in their hlt loop.  Logs per-CPU progress.
 * Safe no-op on single-CPU / APIC-less machines. */
void smp_init(void);

/* How many CPUs the MADT reported (including the BSP). */
uint32_t smp_cpu_count(void);

/* How many APs actually arrived (completed the trampoline). */
uint32_t smp_ap_online(void);
