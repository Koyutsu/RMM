/*
 * arch/arm64/context/percpu.c - per-CPU areas, skeleton.  Not buildable.
 *
 * One area per CPU, TPIDR_EL1 = area base.  Layout contract:
 *   [0 .. )   scheduler / arch fields
 *   [fixed)   struct rmm_cpu_context rmm   <- fault attribution reads
 *             current_module / current_call_frame here on every fault,
 *             the fixed offset must be published for asm (like
 *             offsets.txt on x86_64) and asserted in C.
 *
 * Secondary CPUs: spin-table or PSCI bring-up writes TPIDR_EL1 before
 * the first interrupt can fire (recovery stacks allocated per CPU).
 */
#include <stdint.h>

uint64_t arm64_percpu_area_size(void) { return 0x4000; /* documented default */ }
