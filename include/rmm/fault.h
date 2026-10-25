#pragma once
/*
 * fault.h - common trap abstraction and fault classification
 * .
 *
 * The architecture layer (arch/<arch>/traps) fills struct rmm_trap_frame
 * from its native machine frame and hands it to rmm_handle_exception().
 * Nothing outside arch/ may poke at native registers.
 */
#include <rmm/rmm.h>
#include <rmm/module.h>

/* Exception classes, architecture-neutral */
enum rmm_fault_class {
    RMM_EXC_PAGE_FAULT        = 1,   /* residency / protection             */
    RMM_EXC_NULL              = 2,   /* fault_address == 0    */
    RMM_EXC_ILLEGAL_INSTR     = 3,   /* #UD / undefined    */
    RMM_EXC_SFI_VIOLATION     = 4,   /* domain breach    */
    RMM_EXC_STACK_FAULT       = 5,   /* guard page hit    */
    RMM_EXC_GENERAL           = 6,   /* GPF, divide error, ...             */
    RMM_EXC_BREAKPOINT        = 7,   /* int3 / debug, pass through        */
};

/* Access type for page faults */
enum rmm_access_type {
    RMM_ACCESS_READ  = 0,
    RMM_ACCESS_WRITE = 1,
    RMM_ACCESS_EXEC  = 2,
};

/*
 * Common trap frame.  `regs` is architecture-specific in layout but
 * opaque to the core: only arch code and the per-arch recovery path
 * interpret it.  The core reads the common fields only.
 */
#define RMM_TRAP_REGS_MAX 32

struct rmm_trap_frame {
    uint64_t pc;                       /* RIP / ELR_EL1                    */
    uint64_t sp;                       /* RSP / SP_ELx                     */
    uint64_t flags;                    /* RFLAGS / SPSR_EL1                */

    uint64_t regs[RMM_TRAP_REGS_MAX];  /* native register image            */

    uint64_t fault_address;            /* CR2 / FAR_EL1                    */
    uint32_t exception;                /* enum rmm_fault_class             */
    uint32_t access_type;              /* enum rmm_access_type             */
    uint32_t error_code;               /* native error code (x86)          */
    uint32_t from_user;                /* 1 = fault came from ring 3       */

    rmm_module_t *module;              /* attributed module (may be NULL)  */
};

/* Outcome of core fault handling */
enum rmm_fault_result {
    RMM_FAULT_HANDLED     = 0,   /* fixed - retry the faulting instruction */
    RMM_FAULT_RETRY       = 1,   /* retry after state changed              */
    RMM_FAULT_MODULE_CRASH= 2,   /* module faulted - arch must recover     */
    RMM_FAULT_FATAL       = 3,   /* panic                                  */
};

/* Core entry point for every exception. */
enum rmm_fault_result rmm_handle_exception(struct rmm_trap_frame *frame);

/* Page-fault path: COW/stack-growth (existing behaviour) plus
 * RMM demand loading. */
enum rmm_fault_result rmm_page_fault(struct rmm_trap_frame *frame);

/* Panic - called when classification says fatal.  Prints the-style
 * fault report (module, generation, PC, fault address, caller) then halts. */
void rmm_panic(const struct rmm_trap_frame *frame, const char *reason)
    __attribute__((noreturn));
