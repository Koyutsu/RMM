/*
 * exceptions.c - RMM fault classification and module recovery
 * .
 *
 * Architecture-independent policy.  The arch layer fills the common
 * trap frame and performs the machine-level recovery patch, this file
 * decides what happens.
 */
#include <stdatomic.h>
#include <rmm/rmm.h>
#include <rmm/module.h>
#include <rmm/fault.h>
#include <rmm/arch.h>
#include <rmm/modules.generated.h>
#include "console/print.h"

/* core services */
int   vmm_handle_fault(uint64_t cr2, uint64_t error);   /* COW/stack triage */

int rmm_module_fault(rmm_module_t *m, void *frame);
void rmm_crash_record_add(const struct rmm_crash_record *r);

/* fault report */
static const char *class_name(uint32_t c) {
    switch (c) {
    case RMM_EXC_PAGE_FAULT:     return "page fault";
    case RMM_EXC_NULL:           return "null dereference";
    case RMM_EXC_ILLEGAL_INSTR:  return "illegal instruction";
    case RMM_EXC_SFI_VIOLATION:  return "SFI violation";
    case RMM_EXC_STACK_FAULT:    return "stack fault";
    case RMM_EXC_BREAKPOINT:     return "breakpoint";
    default:                     return "general";
    }
}

static void fault_report(const struct rmm_trap_frame *f,
                         struct rmm_module_descriptor *m,
                         const char *reason) {
    print_str("\n[rmm] FAULT\n");
    if (m)
        printf("  module: %s  generation: %u\n", m->name, m->domain.generation);
    printf("  pc: %p  sp: %p\n", (void *)(uintptr_t)f->pc,
           (void *)(uintptr_t)f->sp);
    printf("  fault address: %p  type: %s\n",
           (void *)(uintptr_t)f->fault_address, class_name(f->exception));
    if (f->module && f->module != m)
        printf("  caller: %s\n", f->module->name);
    if (reason) {
        print_str("  reason: ");
        print_str(reason);
        print_str("\n");
    }
    /* symbolicate via the generated ABI tables when possible */
    {
        extern const struct rmm_abi_table rmm_abi_tables[];
        extern const uint32_t rmm_abi_table_count;
        for (uint32_t i = 0; i < rmm_abi_table_count; i++) {
            const struct rmm_abi_table *t = &rmm_abi_tables[i];
            struct rmm_module_descriptor *mod = rmm_module_by_name(t->module);
            if (!mod || !t->count) continue;
            if (f->pc < mod->base || f->pc >= mod->base + mod->size) continue;
            uint64_t best = 0;
            const char *bsym = NULL;
            for (uint32_t e = 0; e < t->count; e++) {
                if (t->entries[e].offset <= (f->pc - mod->base) &&
                    t->entries[e].offset >= best) {
                    best = t->entries[e].offset;
                    bsym = t->entries[e].name;
                }
            }
            if (bsym)
                printf("  symbolicate: %s!%s+0x%x\n", t->module, bsym,
                       (unsigned)(f->pc - mod->base - best));
        }
    }
}

void rmm_panic(const struct rmm_trap_frame *frame, const char *reason) {
    fault_report(frame, frame ? frame->module : NULL, reason);
    print_str("\nSystem halted. Please reset.\n");
    arch_irq_disable();
    for (;;) arch_halt();
}

/* classification */

static struct rmm_module_descriptor *pc_owner(uint64_t pc) {
    /* core is identity-mapped below 16 MiB, slots live in the module
     * window, everything else is unmapped wilderness */
    if (pc < 0x1000000ull) return NULL;                 /* core        */
    for (uint32_t i = 0; i < RMM_MODULE_COUNT; i++) {
        struct rmm_module_descriptor *m = rmm_module_table[i];
        if (pc >= m->base && pc < m->base + m->size) return m;
    }
    return (struct rmm_module_descriptor *)(uintptr_t)1;  /* wilderness */
}

static struct rmm_cpu_context *cpu_ctx(void) { return arch_cpu_ctx(); }

static uint32_t map_class(uint32_t vec, uint64_t fault_addr) {
    if (vec == RMM_EXC_ILLEGAL_INSTR) return RMM_EXC_ILLEGAL_INSTR;
    if (fault_addr == 0) return RMM_EXC_NULL;
    return vec;
}

/* Find the innermost call frame of module `victim` in this CPU's chain */
static struct rmm_call_frame *find_victim_frame(struct rmm_cpu_context *cpu,
                                                struct rmm_module_descriptor *victim) {
    struct rmm_call_frame *f = cpu->current_call_frame;
    while (f) {
        if (f->callee == victim) return f;
        f = f->prev;
    }
    return NULL;
}

/* Restore per-CPU module context from a frame's predecessor (the same
 * logic the stub's leave path performs inline). */
static void cpu_ctx_restore_from(struct rmm_cpu_context *cpu,
                                 struct rmm_call_frame *prev) {
    if (prev) {
        cpu->current_module = prev->callee;
        cpu->module_id = prev->callee->id;
        cpu->module_generation = prev->callee_generation;
        cpu->current_call_frame = prev;
    } else {
        cpu->current_module = NULL;
        cpu->module_id = 0;
        cpu->module_generation = 0;
        cpu->current_call_frame = NULL;
    }
}

/* refinement: core-PC fault with a module mid-call
 * A module that hands a bad pointer to a core service faults inside core
 * code (the dereference happens at the core PC).  Treating that as a
 * core fault would panic the whole machine for a module bug, and a
 * module can cause it without any fault of the core's own.  When a
 * module is mid-call on this CPU (not in interrupt context, an active
 * stub frame exists), the fault is therefore attributed to that module
 * and recovered through the normal path: the module is
 * marked FAULTED and the core caller receives -RMM_ERR_MODULE_FAULT.
 *
 * True core bugs still panic:
 *   - no module active (core faulted on its own),
 *   - fault in interrupt context (the suspended module did not cause it),
 *   - recovery patch failure.
 * Note: attribution catches the trapping cases.  A module handing a core
 * service a valid core pointer still corrupts the core silently, that
 * is the missing-isolation gap closed only by Stage 4/5 (see README). */
static bool recover_core_fault(struct rmm_trap_frame *frame,
                               struct rmm_cpu_context *cpu) {
    struct rmm_module_descriptor *cm = cpu->current_module;
    if (!cm || cpu->interrupt_depth != 0) return false;
    struct rmm_call_frame *vf = find_victim_frame(cpu, cm);
    if (!vf) return false;                     /* raw/IRQ entry: core bug */

    frame->module = cm;
    frame->exception = map_class(frame->exception, frame->fault_address);

    struct rmm_crash_record rec = {
        .module_id = cm->id,
        .generation = cm->domain.generation,
        .pc = frame->pc, .sp = frame->sp,
        .fault_address = frame->fault_address,
        .exception = frame->exception,
        .access_type = frame->access_type,
        .timestamp_ms = arch_time_ms(),
    };
    rmm_crash_record_add(&rec);

    fault_report(frame, cm,
                 "fault in core PC - attributed to the calling module "
                 "(bad pointer across the module boundary)");

    rmm_module_fault(cm, frame);
    cpu_ctx_restore_from(cpu, vf->prev);
    atomic_fetch_sub(&cm->active_calls, 1);
    if (arch_recovery_patch(frame->regs, vf, RMM_ERR_MODULE_FAULT))
        return true;

    fault_report(frame, cm, "recovery patch failed");
    return false;
}

enum rmm_fault_result rmm_handle_exception(struct rmm_trap_frame *frame) {
    struct rmm_cpu_context *cpu = cpu_ctx();
    cpu->exception_depth++;

    /* user-mode faults (user stays outside RMM)
     * COW break / stack growth fix -> retry, anything else -> the
     * process dies, the system lives (Linux semantics, the arch trap
     * layer performs the kill on fatal). */
    if (frame->from_user) {
        if (frame->exception == RMM_EXC_PAGE_FAULT &&
            vmm_handle_fault(frame->fault_address, frame->error_code)) {
            cpu->exception_depth--;
            return RMM_FAULT_HANDLED;
        }
        cpu->exception_depth--;
        return RMM_FAULT_FATAL;
    }

    struct rmm_module_descriptor *owner = pc_owner(frame->pc);
    enum rmm_fault_result res = RMM_FAULT_FATAL;
    void *victim_frame = NULL;

    /* first: PC faulted into an UNLOADED slot -> demand load */
    if (owner && (uintptr_t)owner > 1 &&
        frame->exception == RMM_EXC_PAGE_FAULT &&
        atomic_load(&owner->domain.state) == RMM_UNLOADED &&
        rmm_can_fault_load()) {
        int rc = rmm_module_load(owner);
        cpu->exception_depth--;
        return rc ? RMM_FAULT_FATAL : RMM_FAULT_HANDLED;
    }

    if ((uintptr_t)owner > 1) {
        /* fault inside module code */
        frame->module = owner;
        uint32_t cls = map_class(frame->exception, frame->fault_address);
        frame->exception = cls;

        struct rmm_crash_record rec = {
            .module_id = owner->id,
            .generation = owner->domain.generation,
            .pc = frame->pc, .sp = frame->sp,
            .fault_address = frame->fault_address,
            .exception = frame->exception,
            .access_type = frame->access_type,
            .timestamp_ms = arch_time_ms(),
        };
        rmm_crash_record_add(&rec);

        if (cls == RMM_EXC_BREAKPOINT) {
            cpu->exception_depth--;
            return RMM_FAULT_HANDLED;                  /* pass through */
        }

        struct rmm_call_frame *vf = find_victim_frame(cpu, owner);
        victim_frame = vf;
        if (vf) {
            /* mark, leave on the faulting CPU's behalf,
             * return around the broken code to the caller */
            rmm_module_fault(owner, frame);

            cpu_ctx_restore_from(cpu, vf->prev);
            atomic_fetch_sub(&owner->active_calls, 1);
            if (arch_recovery_patch(frame->regs, vf, RMM_ERR_MODULE_FAULT)) {
                cpu->exception_depth--;
                return RMM_FAULT_MODULE_CRASH;
            }
            res = RMM_FAULT_FATAL;
            fault_report(frame, owner, "recovery patch failed");
        } else {
            /* entered via raw function pointer / IRQ, no caller
             * frame exists, the module is dead and the context is not
             * unwinding-able */
            rmm_module_fault(owner, frame);
            fault_report(frame, owner,
                         "no recovery frame (raw function-pointer entry)");
            res = RMM_FAULT_FATAL;
        }
    } else if (owner == NULL &&
               frame->exception == RMM_EXC_PAGE_FAULT) {
        /* core-mode page fault: demand-load or die */
        struct rmm_module_descriptor *slot = rmm_find_slot(frame->fault_address);
        struct rmm_module_descriptor *pcslot = rmm_find_slot(frame->pc);
        if (pcslot && atomic_load(&pcslot->domain.state) == RMM_UNLOADED &&
            rmm_can_fault_load()) {
            /* PC faulted into an UNLOADED slot, demand load */
            int rc = rmm_module_load(pcslot);
            cpu->exception_depth--;
            return rc ? RMM_FAULT_FATAL : RMM_FAULT_HANDLED;
        }
        if (slot && pcslot && slot != pcslot) {
            /* module reached into another slot's pages */
            frame->exception = RMM_EXC_SFI_VIOLATION;
            if ((uintptr_t)pc_owner(frame->pc) > 1) {
                rmm_module_fault(pcslot, frame);
                /* fallthrough to fatal for cross-slot breach (
                 * treat the module as compromised) */
            }
        }
        /* COW / stack-growth triage for user pages (existing kernel
         * behaviour, the arch supplies, the core decides) */
        if (vmm_handle_fault(frame->fault_address, frame->error_code)) {
            cpu->exception_depth--;
            return RMM_FAULT_HANDLED;
        }
        /* refinement: module handed a bad pointer to a core service
         * (the dereference faulted at the core PC) - blame the module,
         * recover around it, return -RMM_ERR_MODULE_FAULT to the core
         * caller.  Genuine core bugs (no module active / IRQ context /
         * no stub frame) still panic. */
        if (recover_core_fault(frame, cpu)) {
            cpu->exception_depth--;
            return RMM_FAULT_MODULE_CRASH;
        }
        res = RMM_FAULT_FATAL;
    } else if (owner == NULL) {
        /* core fault, non-page-fault (#GP, #UD, ...): same attribution
         * rule as above (refinement) */
        if (recover_core_fault(frame, cpu)) {
            cpu->exception_depth--;
            return RMM_FAULT_MODULE_CRASH;
        }
        /* core fault (core is the TCB -> panic) */
        res = RMM_FAULT_FATAL;
    } else {
        /* wilderness PC - if a module is mid-call it jumped through a
         * bad function pointer, recover around it.  Otherwise this is a
         * corrupted core PC -> panic. */
        if (recover_core_fault(frame, cpu)) {
            cpu->exception_depth--;
            return RMM_FAULT_MODULE_CRASH;
        }
        res = RMM_FAULT_FATAL;
    }

    cpu->exception_depth--;
    (void)victim_frame;
    return res;
}

/* architecture-independent page-fault entry */
enum rmm_fault_result rmm_page_fault(struct rmm_trap_frame *frame) {
    return rmm_handle_exception(frame);
}
