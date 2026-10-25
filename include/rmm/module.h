#pragma once
/*
 * module.h - module descriptors, lifecycle state machine, execution
 * context and call frames.  Architecture-independent.
 */
#include <stdatomic.h>
#include <rmm/rmm.h>

/* Lifecycle state machine */
enum rmm_module_state {
    RMM_UNLOADED  = 0,
    RMM_LOADING   = 1,
    RMM_READY     = 2,
    RMM_RUNNING   = 3,
    RMM_FAULTED   = 4,
    RMM_QUIESCING = 5,
    RMM_RELOADING = 6,
    RMM_DISABLED  = 7,
};

/* Canonical module type: descriptors are generated into
 * rmm_module_table[]; "struct rmm_module" is the public spelling. */
typedef struct rmm_module_descriptor rmm_module_t;

/* ABI offset table, generated into abi_table.c */
struct rmm_abi_entry { const char *name; uint64_t offset; };
struct rmm_abi_table { const char *module; uint32_t count;
                       const struct rmm_abi_entry *entries; };

/* Descriptor flags (from modules.map, enforced by the generator) */
#define RMM_FLAG_RESIDENT    0x1u
#define RMM_FLAG_REPLACEABLE 0x2u
#define RMM_FLAG_OPTIONAL    0x4u
#define RMM_FLAG_RECOVERABLE 0x8u

/*
 * rmm_domain - the module's SFI address domain.
 * mask == size - 1 so confinement is `base | (ptr & mask)`.
 * State/lock/generation drive the lifecycle.
 */
struct rmm_domain {
    uint64_t          base;
    uint64_t          size;
    uint64_t          mask;
    rmm_module_id_t   module_id;
    uint32_t          generation;      /* bumped on every load/reload */
    atomic_uint       state;           /* enum rmm_module_state              */
    struct spinlock   lock;            /* per-module loading lock      */
};

/*
 * rmm_module_descriptor - generated from modules.map.  Never
 * duplicate these addresses by hand anywhere else.
 */
struct rmm_module_descriptor {
    uint32_t              id;
    uint64_t              base;
    uint64_t              size;
    uint64_t              code_size;
    uint64_t              data_size;
    const char           *name;
    const char           *image;

    const uint32_t       *dependencies;
    uint32_t              dependency_count;

    uint32_t              flags;
    uint32_t              class;             /* enum rmm_class            */

    /* runtime side (owned by the module manager) */
    struct rmm_domain     domain;

    /* mapped segment bounds (filled by the loader on each load) -
     * guard pages live between them, the unload path walks
     * exactly these ranges to free every page */
    uint64_t seg_code_start, seg_code_end;
    uint64_t seg_ro_start,   seg_ro_end;
    uint64_t seg_data_start, seg_data_end;
    uint64_t seg_bss_start,  seg_bss_end;
    uint64_t image_bytes;

    atomic_uint           active_calls;
    uint32_t              failure_count;     /* crash-loop policy     */
    uint64_t              last_failure_ms;
    uint32_t              reload_count;
    bool                  irq_users;         /* registered IRQ handlers   */
};

/*
 * rmm_cpu_context - per-CPU module execution context.
 * Lives inside the architecture per-CPU area, access must be extremely
 * cheap (GS-relative on x86_64, TPIDR_EL1-relative on ARM64).
 */
struct rmm_call_frame;                       /* forward, below            */

struct rmm_cpu_context {
    rmm_module_t             *current_module;   /* NULL = executing core  */
    rmm_module_id_t           module_id;        /* 0 = core               */
    uint64_t                  module_generation;
    uint32_t                  exception_depth;
    uint32_t                  interrupt_depth;
    uint32_t                  preempt_disable;
    void                     *active_trap;      /* struct rmm_trap_frame* */
    struct rmm_call_frame    *current_call_frame;  /* newest stub frame   */
    uint64_t                  recovery_sp;      /* trusted recovery stack */
    void                     *active_syscall_frame; /* syscall_frame_t*   */
};

/* Call frame laid down by every cross-module stub
 * The stub builds this struct IN place on the task's kernel stack:
 *   push %rbp, mov %rsp,%rbp          -> caller_rbp lives at stub rbp+0
 *   push r15,r14,r13,r12,rbx          -> stub rbp-8 .. rbp-40
 *   sub $40; payload slots            -> stub rbp-48 .. rbp-80
 * The frame pointer the stub publishes (current_call_frame) is the base
 * of this struct = stub rbp-80.  Layout below must mirror the generated
 * stub in rmm-gen byte-for-byte, offsets.txt is derived from
 * this header and drives the stub's leave path.  The struct doubles as
 * the recovery unwind record: the arch recovery path restores
 * rbx/r12-r15/rbp/rsp/rip from it. */
struct rmm_call_frame {
    uint64_t caller_rsp;             /* +0   = stub rbp-80 (live rsp)   */
    uint64_t return_address;         /* +8   = stub rbp-72 (copy)       */
    uint64_t callee_generation;      /* +16  = stub rbp-64 check   */
    rmm_module_t         *callee;    /* +24  = stub rbp-56              */
    struct rmm_call_frame *prev;     /* +32  = stub rbp-48 (caller's)   */
    uint64_t rbx;                    /* +40  = stub rbp-40              */
    uint64_t r12;                    /* +48  = stub rbp-32              */
    uint64_t r13;                    /* +56  = stub rbp-24              */
    uint64_t r14;                    /* +64  = stub rbp-16              */
    uint64_t r15;                    /* +72  = stub rbp-8               */
    uint64_t caller_rbp;             /* +80  = stub rbp+0 (saved %rbp)  */
};

#define RMM_FRAME_NO_MODULE 0u

/* Module manager API */
rmm_module_t *rmm_module_find(rmm_module_id_t id);
rmm_module_t *rmm_module_by_name(const char *name);
rmm_module_t *rmm_find_slot(uint64_t va);       /* slot lookup     */
int   rmm_module_load(rmm_module_t *m);         /* may demand-load     */
int   rmm_module_unload(rmm_module_t *m);
int   rmm_module_reload(rmm_module_t *m);
int   rmm_module_quiesce(rmm_module_t *m);
int   rmm_module_enter(rmm_module_t *m);        /* slow entry (load if needed) */
void  rmm_module_leave(rmm_module_t *m);
int   rmm_module_fault(rmm_module_t *m, void *raw_trap_frame);
bool  rmm_can_fault_load(void);
struct rmm_entry_stub { const char *name; void *stub; };

void  rmm_module_prefetch(const char *name);
void  rmm_modules_init(void);                        /* manager bootstrap   */
void  rmm_modules_dump(void);                        /* debug: table print  */

/* Called by the trampolines' slow path.  Returns 0 on success (caller may
 * proceed into the module), or an RMM_ERR_* code that the stub returns
 * directly to its caller. */
int rmm_stub_enter_slow(rmm_module_t *m, struct rmm_call_frame *frame);

/* Recovery: unwind to the first frame belonging to
 * `m` and return through it with `err`.  Implemented per-arch (needs to
 * rewrite the machine trap frame). */
void rmm_recovery_return(struct rmm_cpu_context *cpu, uint64_t err);

/* Crash records */
#define RMM_CRASH_RECORDS 8
struct rmm_crash_record {
    uint32_t module_id;
    uint32_t generation;
    uint64_t pc, sp, fault_address;
    uint32_t exception, access_type;
    uint64_t timestamp_ms;
};
void rmm_crash_record_add(const struct rmm_crash_record *r);
const struct rmm_crash_record *rmm_crash_record_last(rmm_module_id_t id);
