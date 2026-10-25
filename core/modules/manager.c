/*
 * manager.c - RMM module manager.
 *
 * Owns the lifecycle of every module: state machine, per-module loading
 * lock, demand loading through page faults, quiescing, reload, crash
 * records and the crash-loop policy.  Architecture-independent.
 */
#include <stdatomic.h>
#include <rmm/rmm.h>
#include <rmm/module.h>
#include <rmm/fault.h>
#include <rmm/arch.h>
#include <rmm/modules.generated.h>
#include <rmm/resource.h>
#include "console/print.h"
#include "cpu/percpu.h"

/* loader (loader.c) */
int  loader_load_image(rmm_module_t *m);
void loader_unload_image(rmm_module_t *m);

static inline bool rmm_streq(const char *a, const char *b) {
    if (!a || !b) return false;
    while (*a && *a && *a == *b) { a++; b++; }
    return *a == *b;
}

/* first byte of the reserved module-slot VA window (arch document:
 * x86_64 places module slots in PML4[510], see arch/x86_64/mm) */
#define RMM_SLOT_VA_LO 0xFFFFFF8000000000ull
#define RMM_SLOT_VA_HI 0xFFFFFF8080000000ull

rmm_module_t *rmm_module_find(rmm_module_id_t id) {
    if (id >= RMM_MODULE_COUNT) return NULL;
    return rmm_module_table[id];
}

rmm_module_t *rmm_module_by_name(const char *name) {
    for (uint32_t i = 0; i < RMM_MODULE_COUNT; i++)
        if (rmm_streq(rmm_module_table[i]->name, name))
            return rmm_module_table[i];
    return NULL;
}

/* which slot owns this VA? */
rmm_module_t *rmm_find_slot(uint64_t va) {
    if (va < RMM_SLOT_VA_LO || va >= RMM_SLOT_VA_HI) return NULL;
    for (uint32_t i = 0; i < RMM_MODULE_COUNT; i++) {
        struct rmm_module_descriptor *m = rmm_module_table[i];
        if (va >= m->base && va < m->base + m->size) return m;
    }
    return NULL;
}

static const char *state_name(uint32_t s) {
    static const char *names[] = {
        "UNLOADED", "LOADING", "READY", "RUNNING",
        "FAULTED", "QUIESCING", "RELOADING", "DISABLED",
    };
    return s < 8 ? names[s] : "?";
}

void rmm_panic_simple(const char *reason) {
    writeWarn("RMM panic");
    print_str(reason);
    print_str("\n");
    arch_irq_disable();
    for (;;) arch_halt();
}

void rmm_modules_init(void) {
    rmm_resources_init();          /* resource ledger (recovery cleanup) */
    for (uint32_t i = 0; i < RMM_MODULE_COUNT; i++) {
        struct rmm_module_descriptor *m = rmm_module_table[i];
        if (m->id != i)
            rmm_panic_simple("module table id mismatch");
        m->domain.module_id = m->id;
    }
    writeInfo("RMM module manager ready");
    for (uint32_t i = 0; i < RMM_MODULE_COUNT; i++) {
        struct rmm_module_descriptor *m = rmm_module_table[i];
        if (m->flags & RMM_FLAG_RESIDENT) continue;
        printf("  [slot %s base=%p size=%uK %s]\n", m->name,
               (void *)(uintptr_t)m->base, (unsigned)(m->size >> 10),
               m->flags & RMM_FLAG_OPTIONAL ? "reserved" : "demand");
    }
}

bool rmm_can_fault_load(void) {
    /* never demand-load from an interrupt handler */
    struct rmm_cpu_context *cpu = arch_cpu_ctx();
    return cpu->interrupt_depth == 0;
}

/* Generated glue (final phase): module entry stub addresses */
extern const struct rmm_entry_stub rmm_entry_stubs[];
extern const uint32_t rmm_entry_stub_count;

static int module_init_call(struct rmm_module_descriptor *m) {
    printf("[dbg] init_call %s count=%u\n", m->name, rmm_entry_stub_count);
    for (uint32_t i = 0; i < rmm_entry_stub_count; i++)
        if (rmm_streq(rmm_entry_stubs[i].name, m->name)) {
            int (*init)(void) = (int (*)(void))rmm_entry_stubs[i].stub;
            printf("[dbg] init_call %s stub=%p\n", m->name,
                   (void *)(uintptr_t)rmm_entry_stubs[i].stub);
            int rc = init();
            printf("[dbg] init_call %s rc=%d\n", m->name, rc);
            return rc;
        }
    printf("[dbg] init_call %s: no entry\n", m->name);
    return 0;   /* module without an entry, fine */
}

/* Load */
static void canary(const char *tag) {
    percpu_t *pc = percpu_this();
    if (pc->self != (uint64_t)(uintptr_t)pc)
        printf("[canary] %s self=%p\n", tag, (void *)(uintptr_t)pc->self);
}

int rmm_module_load(struct rmm_module_descriptor *m) {
    if (m->flags & RMM_FLAG_RESIDENT) return 0;
    printf("[dbg] load %s enter\n", m->name);

    spin_lock(&m->domain.lock);
    uint32_t st = atomic_load(&m->domain.state);
    if (st == RMM_READY || st == RMM_RUNNING) {
        spin_unlock(&m->domain.lock);
        return 0;
    }
    if (st == RMM_DISABLED) {
        spin_unlock(&m->domain.lock);
        return RMM_ERR_MODULE_UNAVAILABLE;
    }
    if (st == RMM_LOADING || st == RMM_RELOADING || st == RMM_QUIESCING) {
        /* another CPU is the loader, wait for it (bounded) */
        spin_unlock(&m->domain.lock);
        for (int i = 0; i < 100000000; i++) {
            uint32_t s2 = atomic_load(&m->domain.state);
            if (s2 == RMM_READY || s2 == RMM_RUNNING) return 0;
            if (s2 == RMM_DISABLED || s2 == RMM_FAULTED)
                return RMM_ERR_MODULE_UNAVAILABLE;
            arch_halt();
        }
        return RMM_ERR_MODULE_LOAD;
    }
    if (st != RMM_UNLOADED) {
        spin_unlock(&m->domain.lock);
        return RMM_ERR_MODULE_UNAVAILABLE;
    }

    atomic_store(&m->domain.state, RMM_LOADING);
    spin_unlock(&m->domain.lock);
    printf("[dbg] %s LOADING\n", m->name);

    /* dependencies load before the parent becomes READY */
    for (uint32_t i = 0; i < m->dependency_count; i++) {
        struct rmm_module_descriptor *d =
            rmm_module_find(m->dependencies[i]);
        if (!d || (d->flags & RMM_FLAG_RESIDENT)) continue;
        if (atomic_load(&d->domain.state) == RMM_UNLOADED) {
            int rc = rmm_module_load(d);
            if (rc) {
                atomic_store(&m->domain.state, RMM_FAULTED);
                return rc;
            }
        }
    }

    canary("pre-loader");
    printf("[dbg] %s calling loader\n", m->name);
    int rc = loader_load_image(m);
    canary("post-loader");
    printf("[dbg] loader rc=%d\n", rc);
    if (rc) {
        writeWarn("RMM: image load failed");
        atomic_store(&m->domain.state, RMM_FAULTED);
        return RMM_ERR_MODULE_LOAD;
    }
    m->domain.generation++;
    printf("  [RMM demand-load %s gen %u - %u KiB mapped]\n",
           m->name, m->domain.generation,
           (unsigned)((m->code_size + m->data_size) >> 10));

    printf("[dbg] %s READY\n", m->name);
    atomic_store(&m->domain.state, RMM_READY);
    {
        int irc = module_init_call(m);
        canary("post-init");
        printf("[dbg] module_entry rc=%d\n", irc);              /* module_init */
        if (irc) {
            atomic_store(&m->domain.state, RMM_FAULTED);
            return RMM_ERR_MODULE_LOAD;
        }
    }
    return 0;
}

/* rmm_module_enter - slow entry used by stubs when the fast state
 * gate fails.  Returns 0 = caller may proceed, else RMM_ERR_*. */
int rmm_module_enter(struct rmm_module_descriptor *m) {
    printf("[dbg] enter %s\n", m->name);
    uint32_t st = atomic_load(&m->domain.state);
    switch (st) {
    case RMM_READY:
    case RMM_RUNNING:
        return 0;
    case RMM_UNLOADED:
        if (!rmm_can_fault_load())
            return RMM_ERR_MODULE_NOT_RESIDENT;
        return rmm_module_load(m);
    case RMM_LOADING:
    case RMM_RELOADING:
        return RMM_ERR_MODULE_RELOADING;
    case RMM_FAULTED:
    case RMM_QUIESCING:
    case RMM_DISABLED:
    default:
        return RMM_ERR_MODULE_UNAVAILABLE;
    }
}

/* Trampoline slow path (called from generated stub asm).  SysV:
 * rdi = module, rsi = frame.  Returns 0 to proceed. */
__attribute__((used))
int rmm_stub_enter_slow(struct rmm_module_descriptor *m,
                        struct rmm_call_frame *frame) {
    int rc = rmm_module_enter(m);
    if (rc) return rc;
    /* stale generation guard, the frame was built before a load */
    if (frame && frame->callee_generation &&
        frame->callee_generation != m->domain.generation)
        return RMM_ERR_MODULE_GENERATION;
    return 0;
}

void rmm_module_leave(struct rmm_module_descriptor *m) {
    atomic_fetch_sub(&m->active_calls, 1);
}

/* Unload / quiesce / reload */
int rmm_module_quiesce(struct rmm_module_descriptor *m) {
    spin_lock(&m->domain.lock);
    uint32_t st = atomic_load(&m->domain.state);
    if (st == RMM_READY || st == RMM_RUNNING)
        atomic_store(&m->domain.state, RMM_QUIESCING);
    spin_unlock(&m->domain.lock);
    /* no new entries from here on, existing ones drain */
    for (int i = 0; i < 500000000; i++) {
        if (atomic_load(&m->active_calls) == 0) return 0;
        arch_halt();
    }
    return RMM_ERR_MODULE_UNAVAILABLE;
}

int rmm_module_unload(struct rmm_module_descriptor *m) {
    if (m->flags & RMM_FLAG_RESIDENT) return RMM_ERR_MODULE_UNAVAILABLE;
    int rc = rmm_module_quiesce(m);
    if (rc) return rc;
    loader_unload_image(m);
    atomic_store(&m->domain.state, RMM_UNLOADED);
    printf("  [RMM unload %s - slot pages freed]\n", m->name);
    return 0;
}

int rmm_module_reload(struct rmm_module_descriptor *m) {
    /* fresh pages, quiesce before replace, whole-module v1 */
    if (atomic_load(&m->domain.state) == RMM_RUNNING ||
        atomic_load(&m->domain.state) == RMM_READY) {
        int rc = rmm_module_quiesce(m);
        if (rc) return rc;
        loader_unload_image(m);
        atomic_store(&m->domain.state, RMM_UNLOADED);
    }
    return rmm_module_load(m);
}

/* Faults */
static struct rmm_crash_record crash_ring[RMM_CRASH_RECORDS];
static uint32_t crash_head;
static struct spinlock crash_lock = { 0, "rmm.crash" };

void rmm_crash_record_add(const struct rmm_crash_record *r) {
    spin_lock(&crash_lock);
    crash_ring[crash_head++ % RMM_CRASH_RECORDS] = *r;
    spin_unlock(&crash_lock);
}

const struct rmm_crash_record *rmm_crash_record_last(rmm_module_id_t id) {
    for (uint32_t i = 0; i < RMM_CRASH_RECORDS; i++) {
        const struct rmm_crash_record *r =
            &crash_ring[(crash_head + RMM_CRASH_RECORDS - 1 - i) %
                        RMM_CRASH_RECORDS];
        if (r->module_id == id) return r;
    }
    return NULL;
}

#define CRASH_LIMIT      3
#define CRASH_WINDOW_MS  10000

int rmm_module_fault(struct rmm_module_descriptor *m, void *rawframe) {
    (void)rawframe;
    uint64_t now = arch_time_ms();
    m->failure_count++;
    if (m->last_failure_ms && now - m->last_failure_ms > CRASH_WINDOW_MS)
        m->failure_count = 1;                        /* window reset  */
    m->last_failure_ms = now;

    spin_lock(&m->domain.lock);
    atomic_store(&m->domain.state, RMM_FAULTED);
    spin_unlock(&m->domain.lock);

    /* Recovery epoch: bump the generation so every blocking loop that
     * captured it before the fault (fs flock/record locks, any future
     * module wait) detects the crash and aborts with -EIO instead of
     * parking forever on state only the crashed module would change. */
    m->domain.generation++;

    /* Reclaim what the module held outside its slot: ledger entries are
     * inventoried (and RECLAIM-tagged ranges freed), see resource.h.
     * In-slot pages stay with the loader's unload path. */
    rmm_resources_reclaim(m);

    writeWarn("RMM: module FAULTED");
    printf("  [module %s gen %u fault #%u]\n",
           m->name, m->domain.generation, m->failure_count);

    if (m->failure_count >= CRASH_LIMIT) {
        atomic_store(&m->domain.state, RMM_DISABLED);
        writeWarn("RMM: crash-loop policy - module DISABLED");
    }
    return 0;
}

void rmm_modules_dump(void) {
    printf("  %s %s %s %s\n", "SLOT", "STATE", "GEN", "ACTIVE");
    for (uint32_t i = 0; i < RMM_MODULE_COUNT; i++) {
        struct rmm_module_descriptor *m = rmm_module_table[i];
        printf("  %s %s gen%u act%u\n", m->name,
               state_name(atomic_load(&m->domain.state)),
               m->domain.generation, atomic_load(&m->active_calls));
    }
}

/* prefetch: explicit early load for critical paths */
void rmm_module_prefetch(const char *name) {
    struct rmm_module_descriptor *m = rmm_module_by_name(name);
    if (m) rmm_module_load(m);
}
