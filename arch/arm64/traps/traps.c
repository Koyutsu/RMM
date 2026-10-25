/*
 * arch/arm64/traps/traps.c - vector entry + ESR/FAR classification, skeleton.
 * Not buildable.  See ../README.md.
 *
 * VBAR_EL1 points at the vector table, the data-abort / instruction-abort
 * / unknown vectors assemble a struct rmm_trap_frame and call
 * rmm_page_fault() (same contract as arch/x86_64/traps).
 *
 * ESR_ELx.EC (exception class) -> enum rmm_fault_class:
 *   0b1001xx data abort        -> RMM_EXC_PAGE_FAULT (FAR_ELx in frame,
 *                                FAR == 0 -> RMM_EXC_NULL)
 *   0b1000xx instr abort       -> RMM_EXC_PAGE_FAULT (exec access)
 *   0b1111xx/UNALLOC           -> RMM_EXC_ILLEGAL_INSTR
 *   permission fault on slot   -> RMM_EXC_SFI_VIOLATION
 *   SP alignment / abort on SP -> RMM_EXC_STACK_FAULT
 *   BRK (EDB20xx)              -> RMM_EXC_BREAKPOINT (pass through)
 * access_type from WnR/LSB ISS bits -> enum rmm_access_type.
 */
#include <rmm/fault.h>

uint32_t arm64_esr_to_class(uint64_t esr, uint64_t far) {
    (void)esr; (void)far;
    return RMM_EXC_GENERAL;   /* Stub */
}
