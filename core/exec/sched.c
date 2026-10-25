/*
 * sched.c - BORE scheduler (Bursty-Oriented Round-robin Enhancement)
 *
 * Replaces the "parent sleeps / run-child-inline" execution model with
 * real single-CPU time-sharing.  See sched.h for the policy writeup.
 *
 * Control flow map
 *
 *   sched_start()        boot: build init ctx, install IRQ0 stub, idle
 *   sched_irq0_stub      naked asm: push all 15 GP, call sched_irq0_c
 *   sched_irq0_c         EOI + accounting + wakeups + preemption
 *   sched_switch_to      full state switch, resumes next task
 *                        (kernel continuation OR user context)
 *   sched_kernel_park    setjmp-style park inside a blocking kernel loop
 *   sched_exit_switch    switch away from a ZOMBIE, idle if nobody ready
 *
 * Invariants that make this safe on a single CPU:
 *   - The tick only preempts USER-mode frames - a syscall always runs to
 *     completion, so kernel static buffers never interleave.
 *   - Parked kernel continuations keep their whole kernel stack intact,
 *     nothing else runs on that stack while the task is parked.
 *   - The live VFS fd table is swapped (save/restore) on every switch.
 *   - At most one task is RUNNING, g_current_proc switches atomically
 *     inside sched_switch_to with interrupts disabled (IRQ gate).
 */

#include "exec/sched.h"
#include <rmm/arch.h>
#include <rmm/module.h>
#include "cpu/cpu.h"
#include "cpu/gdt.h"
#include "exec/process.h"
#include "exec/timers.h"

#include "exec/syscall_table.h"
#include "cpu/gdt.h"
#include "cpu/idt.h"
#include "drivers/memory/vmm.h"
#include "drivers/serial/serial.h"
#include "drivers/timer/pit.h"
#include "drivers/pci/io.h"
#include <stdint.h>
#include <stddef.h>

#define SCHED_PARK_TRACE 0   /* trace every park (rip/rsp sanity) */



/* tiny libc */
static void sched_zero(void *dst, size_t n) {
    uint8_t *b = (uint8_t *)dst;
    while (n--) *b++ = 0;
}

uint64_t pit_ticks(void);           /* also exported by pit.h             */
extern uint64_t syscall_ksp;        /* defined in syscall_entry.S         */
void sched_dbg_dec(uint64_t v);     /* serial diagnostics, defined below  */
void sched_dbg_hex(uint64_t v);

/* scheduler globals */
static bool     g_sched_active = false;
static uint64_t g_kernel_root_cr3 = 0;   /* boot root, used for idle     */
static uint64_t g_active_cr3      = 0;   /* root currently installed     */
__attribute__((used)) static volatile uint64_t g_idle_rsp = 0;
__attribute__((used)) static volatile uint64_t g_idle_rip = 0;
static bool     g_idle_warned = false;

bool sched_active(void)             { return g_sched_active; }

/*
 *  BORE policy
 */

static uint32_t bore_ilog2(uint64_t v) {
    if (!v) return 0;
    return (uint32_t)(63 - __builtin_clzll(v));
}

static uint32_t bore_slice_for(uint32_t score) {
    uint32_t s = BORE_BASE_SLICE_MS + score * BORE_SLICE_PER_SCORE;
    if (s > BORE_MAX_SLICE_MS) s = BORE_MAX_SLICE_MS;
    if (s < BORE_BASE_SLICE_MS) s = BORE_BASE_SLICE_MS;
    return s;
}

/* Close the wake-cycle burst: classify runtime with ilog2, decay the old
 * score a little, take the max, exactly BORE's update_burst_score(). */
static void bore_close_burst(process_t *p) {
    uint32_t raw     = bore_ilog2(p->burst_ms + 1);
    uint32_t decayed = (p->burst_score > BORE_DECAY)
                     ? p->burst_score - BORE_DECAY : 0;
    uint32_t s       = (raw > decayed) ? raw : decayed;
    if (s > BORE_MAX_SCORE) s = BORE_MAX_SCORE;
    p->burst_score = s;
    p->burst_ms    = 0;
}

/* Lowest vruntime over READY/RUNNING tasks (the "queue's" reference). */
static uint64_t bore_min_vruntime(void) {
    uint64_t best = 0;
    bool have = false;
    for (int i = 0; i < PROC_MAX; i++) {
        process_t *p = &g_proc_table[i];
        if (p->state != PROC_STATE_READY && p->state != PROC_STATE_RUNNING)
            continue;
        if (!have || p->vruntime < best) { best = p->vruntime; have = true; }
    }
    return best;   /* 0 when nothing runnable */
}

/* Bursty tasks get a vruntime credit on wake (jump the queue); hogs get
 * none and keep their (higher) place. */
static void bore_apply_wake_credit(process_t *p) {
    uint64_t min_vr = bore_min_vruntime();
    if (!min_vr) return;
    uint64_t credit = (uint64_t)(BORE_MAX_SCORE - p->burst_score)
                    * BORE_WAKE_CREDIT_MS * BORE_VRUNIT / BORE_MAX_SCORE;
    if (p->vruntime < min_vr && (min_vr - p->vruntime) > credit)
        p->vruntime = min_vr - credit;
}

void sched_fork_inherit(process_t *child) {
    process_t *par = g_current_proc;
    child->vruntime     = par ? par->vruntime : 0;
    child->burst_score  = par ? par->burst_score / 2 : 0;
    child->burst_ms     = 0;
    child->slice_ticks  = bore_slice_for(child->burst_score);
    child->slice_used   = 0;
    /* Note: sched_ctx_valid is owned by proc_fork (it installs the fork
     * frame as the child's first user context after calling this), and
     * kctx_valid=false is guaranteed by the struct copy from the running
     * parent - a forking task cannot be kernel-parked. */
    child->wake_at_ms      = 0;
}

/*
 *  Runqueue (O(n) scan - PROC_MAX is 8)
 */

static process_t *sched_pick_excluding(const process_t *ex) {
    process_t *best = NULL;
    for (int i = 0; i < PROC_MAX; i++) {
        process_t *p = &g_proc_table[i];
        if (p->state != PROC_STATE_READY || p == ex) continue;
        if (!best || p->vruntime < best->vruntime ||
            (p->vruntime == best->vruntime && p->pid < best->pid))
            best = p;
    }
    return best;
}

process_t *sched_pick(void) { return sched_pick_excluding(NULL); }

uint32_t sched_runnable_count(void) {
    uint32_t n = 0;
    for (int i = 0; i < PROC_MAX; i++)
        if (g_proc_table[i].state == PROC_STATE_READY ||
            g_proc_table[i].state == PROC_STATE_RUNNING) n++;
    return n;
}

/*
 *  Context helpers
 */

/* Full register file as pushed by sched_irq0_stub (matching order). */
typedef struct {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t rip, cs, rflags, rsp, ss;
} irq_frame_t;

static void ctx_from_irq(sched_ctx_t *c, const irq_frame_t *f) {
    c->rax = f->rax;   c->rbx = f->rbx;   c->rcx = f->rcx;   c->rdx = f->rdx;
    c->rsi = f->rsi;   c->rdi = f->rdi;   c->rbp = f->rbp;
    c->r8  = f->r8;    c->r9  = f->r9;    c->r10 = f->r10;   c->r11 = f->r11;
    c->r12 = f->r12;   c->r13 = f->r13;   c->r14 = f->r14;   c->r15 = f->r15;
    c->rip = f->rip;   c->rflags = f->rflags; c->user_rsp = f->rsp;
}

static void ctx_from_sysframe(sched_ctx_t *c, const syscall_frame_t *f) {
    c->rax = f->rax;   c->rbx = f->rbx;
    c->rcx = 0;        /* `syscall` clobbers user rcx (holds rip here)   */
    c->rdx = f->rdx;   c->rsi = f->rsi;   c->rdi = f->rdi;   c->rbp = f->rbp;
    c->r8  = f->r8;    c->r9  = f->r9;    c->r10 = f->r10;
    c->r11 = 0;        /* `syscall` clobbers user r11 (holds rflags)     */
    c->r12 = f->r12;   c->r13 = f->r13;   c->r14 = f->r14;   c->r15 = f->r15;
    c->rip = f->rcx;   c->rflags = f->r11;    c->user_rsp = f->user_rsp;
}

static void wrfsbase(uint64_t v) {
    __asm__ volatile("wrmsr"
                     :: "a"((uint32_t)v), "d"((uint32_t)(v >> 32)),
                        "c"(0xC0000100u));
}

/*
 *  The two resume paths
 */

/* Resume a task's user context (sched_ctx).  Modeled on the proven
 * proc_exec_forked asm: iret5 pushed first, selectors + FS base set, then
 * all GP registers restored from the context, rsi freed last.  Never
 * returns (iretq into ring 3). */
__attribute__((noinline, noreturn))
static void user_resume_asm(process_t *p) {
    __asm__ volatile (
        "mov   %c[fsbase](%[p]), %%rax\n\t"      /* FS base before rbx dies */
        "mov   %c[ucs](%[p]), %%r8\n\t"          /* per-task user CS
                                                  * (0x23 64-bit | 0x3B ia32) */
        "pushq $%c[uss]\n\t"                     /* SS   = GDT_USER_DATA|3  */
        "pushq 136(%%rsi)\n\t"                   /* user RSP                 */
        "pushq 128(%%rsi)\n\t"                   /* user RFLAGS              */
        "pushq %%r8\n\t"                         /* user CS                  */
        "pushq 120(%%rsi)\n\t"                   /* user RIP                 */

        "mov   $%c[uds], %%cx\n\t"
        "mov   %%cx, %%ds\n\t"
        "mov   %%cx, %%es\n\t"
        "mov   %%cx, %%fs\n\t"
        /* RMM V2: GS must not be reloaded.  The selector load would set
         * GS.base from the flat GDT descriptor (0), destroying the
         * per-CPU base that ring-0 code (stubs, irq_dispatch, exception
         * path) addresses per-CPU state through.  Ring 3 keeps the
         * kernel GS base, harmless: user page tables mark kernel pages
         * supervisor-only, so gs-relative access from CPL=3 faults on
         * U/S.  Per-user GS arrives with the swapgs/arch_prctl work. */

        "mov   %%rax, %%rdx\n\t"
        "shr   $32, %%rdx\n\t"
        "mov   $0xC0000100, %%ecx\n\t"
        "wrmsr\n\t"                              /* IA32_FS_BASE             */

        "mov    64(%%rsi), %%r9\n\t"
        "mov    56(%%rsi), %%r8\n\t"
        "mov    72(%%rsi), %%r10\n\t"
        "mov    24(%%rsi), %%rdx\n\t"
        "mov    40(%%rsi), %%rdi\n\t"
        "mov   112(%%rsi), %%r15\n\t"
        "mov   104(%%rsi), %%r14\n\t"
        "mov    96(%%rsi), %%r13\n\t"
        "mov    88(%%rsi), %%r12\n\t"
        "mov    48(%%rsi), %%rbp\n\t"
        /* rcx and r11 are live user registers when the
         * tick preempts user code (ctx_from_irq saves them).  They were
         * never restored here, every preempted task resumed with
         * rcx=0xC0000100 (wrmsr leftover) and a garbage r11, silently
         * corrupting user computation (observed: dash #PF writing to
         * NULL+0x34 right after a preemption).  The `syscall` ABI only
         * makes them clobbered AT syscalls, a tick is not a syscall. */
        "mov    16(%%rsi), %%rcx\n\t"            /* user rcx                 */
        "mov    80(%%rsi), %%r11\n\t"            /* user r11                 */
        "mov     0(%%rsi), %%rax\n\t"
        "mov     8(%%rsi), %%rbx\n\t"
        "mov    32(%%rsi), %%rsi\n\t"            /* free the ctx pointer     */

        "iretq\n\t"
        :
        : [p]      "b" (p),
          [f]      "S" (&p->sched_ctx),
          [fsbase] "i" (offsetof(process_t, fs_base)),
          [uss]    "i" (GDT_USER_DATA | 3),
          [ucs]    "i" (offsetof(process_t, user_cs)),
          [uds]    "i" (GDT_USER_DATA | 3)
        : "rax", "rcx", "rdx",
          "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15", "memory"
    );
    __builtin_unreachable();
}

/* Kernel continuation save, setjmp semantics done right.
 *
 * NAKED on purpose: the capture must read the TRUE entry rsp.  A C
 * function's compiler-generated prologue (pushes/sub) would shift %rsp
 * before the asm runs, skewing every saved offset by the prologue size -
 * the resumed continuation then runs with a corrupted stack pointer and
 * dies silently at an arbitrary later point.
 *
 * Entry (SysV): rdi = &kctx, rsp = E-8 where [E-8] = return address and
 * the caller's live rsp = E.  Returns 0 in eax on the normal pass, the
 * resume (kctx_resume) jumps back past the call site with eax = 1 and
 * rsp = E, so nothing below E is ever depended upon. */
__attribute__((naked, used))
static int kctx_save(kctx_t *k) {
    __asm__ volatile (
        "movq  (%rsp), %rax\n\t"           /* return address into caller
         * (at naked entry [rsp] is the return address, the earlier
         * [rsp+8] read grabbed a garbage slot and resumes jumped to
         * it, #UD-ing at RIP=3)                          */
        "movq  %rax, 0(%rdi)\n\t"          /* kctx.rip                    */
        "leaq  8(%rsp), %rax\n\t"          /* caller's live rsp (E)       */
        "movq  %rax, 8(%rdi)\n\t"          /* kctx.rsp                    */
        "movq  %rbp, 16(%rdi)\n\t"
        "movq  %rbx, 24(%rdi)\n\t"
        "movq  %r12, 32(%rdi)\n\t"
        "movq  %r13, 40(%rdi)\n\t"
        "movq  %r14, 48(%rdi)\n\t"
        "movq  %r15, 56(%rdi)\n\t"
        "xorl  %eax, %eax\n\t"             /* first pass: return 0        */
        "ret\n\t"
    );
}

__attribute__((noreturn))
static void kctx_resume(const kctx_t *k) {
    __asm__ volatile (
        "movq  %[k], %%r11\n\t"
        "movq  8(%%r11), %%rsp\n\t"        /* caller's live rsp           */
        "movq  16(%%r11), %%rbp\n\t"
        "movq  24(%%r11), %%rbx\n\t"
        "movq  32(%%r11), %%r12\n\t"
        "movq  40(%%r11), %%r13\n\t"
        "movq  48(%%r11), %%r14\n\t"
        "movq  56(%%r11), %%r15\n\t"
        "movl  $1, %%eax\n\t"              /* "resumed" indicator         */
        "jmp   *(%%r11)\n\t"              /* into the caller post-call   */
        :
        : [k]"m"(k)
        : "rax", "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11",
          "memory"
    );
    __builtin_unreachable();
}

/*
 *  Core switch
 */

__attribute__((noreturn))
static void sched_switch_to(process_t *next) {
    process_t *cur = g_current_proc;

    /* extended FPU/SSE/AVX state, save the outgoing task's
     * image before any kernel work (kernel is -mno-sse so nothing below
     * can clobber it) and restore the incoming task's image before its
     * context resumes.  Covers both resume paths (kctx + user ctx). */
    if (cur && cur != next) fpu_save(cur->fpu_area);

    /* RMM module execution context: the outgoing task keeps its
     * stub-frame chain and module identity, the incoming task gets its
     * own back.  A task with no snapshot resumes in core context. */
    {
        struct rmm_cpu_context *rc = arch_cpu_ctx();
        if (cur) {
            cur->rmm_ctx.current_module     = rc->current_module;
            cur->rmm_ctx.module_id          = rc->module_id;
            cur->rmm_ctx.module_generation  = rc->module_generation;
            cur->rmm_ctx.current_call_frame = rc->current_call_frame;
            cur->rmm_ctx.valid              = true;
        }
        if (next->rmm_ctx.valid) {
            rc->current_module     = (struct rmm_module_descriptor *)next->rmm_ctx.current_module;
            rc->module_id          = next->rmm_ctx.module_id;
            rc->module_generation  = next->rmm_ctx.module_generation;
            rc->current_call_frame = (struct rmm_call_frame *)next->rmm_ctx.current_call_frame;
        } else {
            rc->current_module     = NULL;
            rc->module_id          = 0;
            rc->module_generation  = 0;
            rc->current_call_frame = NULL;
        }
    }

    /* fd table: the live table belongs to whoever ran last
     * (CLONE_FILES threads save/restore through the same table) */
    if (cur && cur->fdt) vfs_fd_save(cur->fdt->t);
    if (next->fdt) vfs_fd_restore(next->fdt->t);

    /* address space */
    uint64_t target = next->cr3 ? next->cr3 : g_kernel_root_cr3;
    if (target != g_active_cr3) {
        vmm_switch_root(target);
        g_active_cr3 = target;
    }

    /* kernel stacks: TSS rsp0 (IRQ/#PF from ring 3) + syscall ksp */
    gdt_set_tss_rsp0(next->kstack_top);
    syscall_ksp = next->kstack_top;

    /* TLS pointer - always written (kernel tasks are 0) */
    wrfsbase(next->fs_base);

    /* fs-debug: a user task must never be dispatched with a zero
     * TLS base (musl keeps its errno/pthread self-pointer in %fs:0).
     * Observed : dash faulted in __syscall_ret writing errno
     * at 0x34 = %fs:0(=NULL)+0x34 - trace when that state appears.
     * compiled out by default, the print fires for every plain
     * execve-less dispatch and spams the console ("logs print twice"). */
#define SCHED_FS_DEBUG 0
#if SCHED_FS_DEBUG
    if (next->pid && !next->fs_base) {
        serial_print_string("[fsdbg] pid=");
        sched_dbg_dec(next->pid);
        serial_print_string(" fs_base=0 state=");
        sched_dbg_dec(next->state);
        serial_print_string(" kctx=");
        sched_dbg_dec(next->kctx_valid);
        serial_print_string(" sctx=");
        sched_dbg_dec(next->sched_ctx_valid);
        serial_print_string(" cr3=");
        sched_dbg_hex(next->cr3);
        serial_print_string("\n");
    }
#endif /* SCHED_FS_DEBUG */

    g_current_proc  = next;
    next->state     = PROC_STATE_RUNNING;
    next->slice_ticks = bore_slice_for(next->burst_score);
    next->slice_used  = 0;

    /* load the incoming task's extended state (fresh tasks get
     * an initialised neutral image, forked children inherit the image
     * their struct copy carried). */
    if (next->fpu_valid) fpu_restore(next->fpu_area);
    else {
        fpu_restore_init(next->fpu_area);
        next->fpu_valid = true;
    }

    /* Gate the kernel continuation before jumping through it.
     * A parked kctx.rip is always a return address inside kernel text
     * (kctx_save captures `(%rsp)` = the return slot of the kctx_save
     * call).  Anything outside the kernel image means the kctx was
     * corrupted - observed once as a ring-0 #UD at RIP=0x7fff39fa
     * (a stale user-stack address) when init respawned the shell after
     * a fatal exception.  Never jump through garbage: log it, dump the
     * task and resume its user context (or kill it) instead. */
    if (next->kctx_valid) {
        extern char kernel_phys_start[];
        extern char kernel_phys_end[];
        uint64_t rip = next->kctx.rip;
        uint64_t rsp = next->kctx.rsp;
        bool rip_ok = ((uint64_t)kernel_phys_start <= rip &&
                       rip < (uint64_t)kernel_phys_end);
        bool rsp_ok = (rsp >= next->kstack_base &&
                       rsp <  next->kstack_top);
        if (!rip_ok || !rsp_ok) {
            serial_print_string("[sched] CORRUPT kctx pid=");
            sched_dbg_dec(next->pid);
            serial_print_string(" rip=");
            sched_dbg_hex(rip);
            serial_print_string(" rsp=");
            sched_dbg_hex(rsp);
            serial_print_string(" kbase=");
            sched_dbg_hex(next->kstack_base);
            serial_print_string(" rbp=");
            sched_dbg_hex(next->kctx.rbp);
            serial_print_string(" rbx=");
            sched_dbg_hex(next->kctx.rbx);
            serial_print_string("\n");
            next->kctx_valid = false;
            if (!next->sched_ctx_valid) {
                /* no user context to fall back to - kill it cleanly */
                proc_exit(next, 128 + 9);          /* SIGKILL semantics */
            }
            /* fall through: user_resume_asm continues from the saved
             * user context, the abandoned syscall returns whatever rax
             * held - survivable, and vastly better than a ring-0 #UD */
        } else {
            kctx_resume(&next->kctx);
        }
    }
    if (next->sched_ctx_valid) user_resume_asm(next);

    /* Nothing resumable - the task slot is inconsistent, kill it. */
    serial_print_string("[sched] pid ");
    sched_dbg_dec(next->pid);
    serial_print_string(" has no valid context - killing\n");
    proc_exit(next, 128 + 9);
    /* noreturn */
    __builtin_unreachable();
}

/* Jump back to the idle loop (used when nothing is runnable). */
__attribute__((noreturn))
static void enter_idle(void) {
    __asm__ volatile (
        "mov   $%c[kds], %%cx\n\t"
        "mov   %%cx, %%ds\n\t"
        "mov   %%cx, %%es\n\t"
        "mov   %%cx, %%fs\n\t"
        /* RMM V2: never reload GS - see user_resume_asm.  The kernel
         * data selector's descriptor base is 0 and would destroy the
         * per-CPU GS base. */
        "movq  g_idle_rsp(%%rip), %%rsp\n\t"
        "movq  g_idle_rip(%%rip), %%rax\n\t"
        "jmp   *%%rax\n\t"
        :
        : [kds]"i"(GDT_KERNEL_DATA)
        : "rax", "rcx", "memory"
    );
    __builtin_unreachable();
}

__attribute__((noreturn))
void sched_exit_switch(void) {
    g_current_proc = NULL;
    process_t *n = sched_pick();
    if (n) sched_switch_to(n);

    /* nothing runnable: back to the idle loop on the boot stack */
    if (g_active_cr3 != g_kernel_root_cr3) {
        vmm_switch_root(g_kernel_root_cr3);
        g_active_cr3 = g_kernel_root_cr3;
    }
    enter_idle();
}

/*
 *  Parks & wakes
 */

void sched_wake(process_t *p) {
    if (!p) return;
    if (p->state != PROC_STATE_SLEEPING && p->state != PROC_STATE_WAITING)
        return;
    bore_close_burst(p);
    bore_apply_wake_credit(p);
    p->state = PROC_STATE_READY;
}

static void sched_wake_sleepers(void) {
    uint64_t now = pit_ticks();
    for (int i = 0; i < PROC_MAX; i++) {
        process_t *p = &g_proc_table[i];
        if (p->state != PROC_STATE_SLEEPING && p->state != PROC_STATE_WAITING)
            continue;
        if (p->wake_at_ms && now >= p->wake_at_ms) sched_wake(p);
    }
}

/* Parked chains must live below the IRQ/exception frame zone at the top
 * of the kernel stack: while a task runs in user mode, ticks (TSS rsp0)
 * and page faults push ~350+ bytes onto the stack top.  Without a gap
 * they overwrite the parked call chain (observed as silent chain death
 * after the first kctx resume).  The reserve is added by the wrapper. */
#define SCHED_PARK_RESERVE 4096

__attribute__((noinline))
static int sched_kernel_park_inner(uint64_t timeout_ms) {
    process_t *me = g_current_proc;
    if (!me || !g_sched_active) return -1;

    if (kctx_save(&me->kctx) == 0) {
        /* first pass: park + switch away (never returns to this frame
         * through the normal path, we come back via kctx_resume) */
        me->kctx_valid = true;
#if SCHED_PARK_TRACE
        serial_print_string("[park] pid=");
        sched_dbg_dec(me->pid);
        serial_print_string(" rip=");
        sched_dbg_hex(me->kctx.rip);
        serial_print_string(" rsp=");
        sched_dbg_hex(me->kctx.rsp);
        serial_print_string(" ktop=");
        sched_dbg_hex(me->kstack_top);
        serial_print_string("\n");
#endif
        if (me->state == PROC_STATE_RUNNING)
            me->state = PROC_STATE_SLEEPING;
        me->wake_at_ms = timeout_ms ? pit_ticks() + timeout_ms : 0;

        /* The parked task's live fd table must be captured before the
         * switch - switch_to skips the save when g_current_proc is NULL. */
        if (me->fdt) vfs_fd_save(me->fdt->t);
        /* the switch below will not see us as "current" - save
         * the extended state explicitly. */
        fpu_save(me->fpu_area);
        g_current_proc = NULL;

        process_t *n = sched_pick_excluding(me);
        if (n) sched_switch_to(n);
        enter_idle();
    }

    /* resumed: sched_switch_to already re-installed CR3/TSS/FS/fds */
    me->kctx_valid = false;
    me->wake_at_ms = 0;
    return 0;
}

/* tiny serial debug helpers (sched diagnostics) */
void sched_dbg_dec(uint64_t v) {
    char b[24]; int i = 23; b[i] = 0;
    do { b[--i] = '0' + (v % 10); v /= 10; } while (v && i);
    serial_print_string(&b[i]);
}

void sched_dbg_hex(uint64_t v) {
    static const char hx[] = "0123456789abcdef";
    char b[19]; int i;
    b[0] = '0'; b[1] = 'x';
    for (i = 0; i < 16; i++) b[2 + i] = hx[(v >> ((15 - i) * 4)) & 0xF];
    b[18] = 0;
    serial_print_string(b);
}

int sched_kernel_park(uint64_t timeout_ms) {
    /* Drop rsp behind the reserve so the saved continuation and every
     * frame it grows on resume sit below the IRQ/exception zone.  The
     * add-back only executes on the resume-return path, the park path
     * switches away and never comes back through here. */
    __asm__ volatile ("subq %0, %%rsp" :: "i"(SCHED_PARK_RESERVE) :);
    int r = sched_kernel_park_inner(timeout_ms);
    __asm__ volatile ("addq %0, %%rsp" :: "i"(SCHED_PARK_RESERVE) :);
    return r;
}

/*
 *  Tick - preemption & accounting
 */

void sched_irq0_c(void *framep) {
    pit_tick_advance();
    pic_eoi(0);

    if (!g_sched_active) return;

    irq_frame_t *f   = (irq_frame_t *)framep;
    process_t   *cur = g_current_proc;

    sched_wake_sleepers();
    /* POSIX interval timers fire on the tick, they must run even
     * on idle ticks (owner may be parked or user-blocked). */
    posix_timers_tick();
    if (!cur) return;                       /* idle tick */

    /* accounting */
    cur->burst_ms++;
    cur->total_runtime_ms++;
    cur->vruntime += BORE_VRUNIT
                   + (uint64_t)cur->burst_score * BORE_HOG_PER_SCORE;
    cur->slice_used++;

    if (cur->alarm_deadline_ms && pit_ticks() >= cur->alarm_deadline_ms) {
        cur->alarm_deadline_ms = 0;
        cur->pending_sigs |= (1ull << SIGALRM);
    }

    /* fatal default-action signals kill even busy loops */
    uint64_t pend = cur->pending_sigs & ~cur->sig_mask;
    static const int fatal_sigs[] = {
        SIGINT, SIGTERM, SIGHUP, SIGPIPE, SIGQUIT, SIGKILL
    };
    for (unsigned i = 0; i < sizeof(fatal_sigs) / sizeof(fatal_sigs[0]); i++) {
        int s = fatal_sigs[i];
        if ((pend & (1ull << s)) &&
            cur->sigactions[s].sa_handler == SIG_DFL) {
            proc_exit(cur, 128 + s);        /* noreturn - switches away */
        }
    }

    /* preemption: only from user mode, only with a rival */
    if (f->cs != (GDT_USER_CODE | 3) &&
        f->cs != (GDT_USER_CODE32 | 3)) return;   /* ia32 too */
    if (cur->slice_used < cur->slice_ticks) return;

#ifdef SCHED_NO_PREEMPT
    cur->slice_used = 0;
    return;
#endif
    process_t *next = sched_pick_excluding(cur);
    if (!next) { cur->slice_used = 0; return; }

    ctx_from_irq(&cur->sched_ctx, f);
    cur->sched_ctx_valid = true;
    cur->state           = PROC_STATE_READY;
    sched_switch_to(next);                  /* noreturn */
}

/* naked IRQ0 stub, pushes all 15 GP registers so a preemption switch can
 * capture a complete user context (rcx/r11 included, unlike the syscall
 * path where the CPU clobbers them). */
__attribute__((naked, used))
void sched_irq0_stub(void) {
    __asm__ volatile (
        "pushq %rax\n\t"
        "pushq %rbx\n\t"
        "pushq %rcx\n\t"
        "pushq %rdx\n\t"
        "pushq %rsi\n\t"
        "pushq %rdi\n\t"
        "pushq %rbp\n\t"
        "pushq %r8\n\t"
        "pushq %r9\n\t"
        "pushq %r10\n\t"
        "pushq %r11\n\t"
        "pushq %r12\n\t"
        "pushq %r13\n\t"
        "pushq %r14\n\t"
        "pushq %r15\n\t"
        "movq  %rsp, %rdi\n\t"
        "call  sched_irq0_c\n\t"
        "popq  %r15\n\t"
        "popq  %r14\n\t"
        "popq  %r13\n\t"
        "popq  %r12\n\t"
        "popq  %r11\n\t"
        "popq  %r10\n\t"
        "popq  %r9\n\t"
        "popq  %r8\n\t"
        "popq  %rbp\n\t"
        "popq  %rdi\n\t"
        "popq  %rsi\n\t"
        "popq  %rdx\n\t"
        "popq  %rcx\n\t"
        "popq  %rbx\n\t"
        "popq  %rax\n\t"
        "iretq\n\t"
    );
}

/*
 *  Idle loop & boot
 */

__attribute__((noreturn))
static void sched_idle_loop(void) {
    /* record the boot-stack continuation so enter_idle() can jump back */
    __asm__ volatile (
        "movq  %%rsp, g_idle_rsp(%%rip)\n\t"
        "leaq  0f(%%rip), %%rax\n\t"
        "movq  %%rax, g_idle_rip(%%rip)\n\t"
        "0:\n\t"
        ::: "rax", "memory"
    );
    for (;;) {
        /* Critical: pick + switch must run with IF=0.  A tick landing
         * inside sched_switch_to would push its frame onto a transient
         * stack and can #GP on the resume iretq (observed as a #GP at
         * sched_irq0_stub's iretq with a corrupted frame).  The switch
         * path itself (user_resume_asm iretq / kctx_resume) re-enables
         * interrupts correctly for the resumed context. */
        __asm__ volatile ("cli");
        process_t *n = sched_pick();
        if (n) sched_switch_to(n);
        if (!g_idle_warned) {
            /* only meaningful if PID 1 died, print once */
            bool any = false;
            for (int i = 0; i < PROC_MAX; i++)
                if (g_proc_table[i].state != PROC_STATE_EMPTY &&
                    g_proc_table[i].state != PROC_STATE_ZOMBIE &&
                    g_proc_table[i].pid != 1) any = true;
            if (!any && g_proc_table[0].state != PROC_STATE_EMPTY &&
                g_proc_table[0].pid == 1) {
                serial_print_string("[sched] all tasks gone - halting\n");
                g_idle_warned = true;
            }
        }
        __asm__ volatile ("sti; hlt; cli");
    }
}

void sched_start(process_t *init) {
    g_kernel_root_cr3 = vmm_get_cr3();
    g_active_cr3      = g_kernel_root_cr3;

    /* Build init's first user context, mirror of the old proc_exec asm:
     * all GP zero, argc/argv/envp in rdi/rsi/rdx, fresh iret frame. */
    sched_ctx_t *c = &init->sched_ctx;
    sched_zero(c, sizeof(*c));
    c->rip      = init->entry;
    c->user_rsp = init->user_rsp;
    c->rflags   = 0x202ULL;
    c->rdi      = (uint64_t)(uint32_t)init->argc;
    c->rsi      = (uint64_t)(uintptr_t)init->argv;
    c->rdx      = (uint64_t)(uintptr_t)init->envp;
    init->sched_ctx_valid = true;
    init->kctx_valid      = false;
    init->burst_ms        = 0;
    init->burst_score     = 0;
    init->vruntime        = 0;
    init->slice_ticks     = bore_slice_for(0);
    init->slice_used      = 0;
    init->state           = PROC_STATE_READY;

    g_sched_active = true;
    extern void idt_install_irq0(void *stub);
    idt_install_irq0((void *)sched_irq0_stub);

    serial_print_string("[sched] BORE scheduler online (irq0 preempt)\n");
    sched_idle_loop();
}

/*
 *  additions
 */

/* sched_yield(2): requeue the calling task behind every rival and switch.
 * Called from the syscall layer with the frame still live: the user
 * context is captured from the syscall frame (rip/rcx, rflags/r11) with
 * rax forced to 0 (yield's return value), so the task simply "returns
 * from the yield syscall" when it is next dispatched.  When nothing
 * else is runnable this returns immediately (no switch, no charge). */
void sched_yield_from_syscall(syscall_frame_t *frame) {
    process_t *me = g_current_proc;
    if (!me || !g_sched_active) return;
    if (!sched_pick_excluding(me)) return;      /* alone on the CPU */

    ctx_from_sysframe(&me->sched_ctx, frame);
    me->sched_ctx.rax = 0;                      /* yield() returns 0     */
    me->sched_ctx_valid = true;

    /* charge a full slice of vruntime so the next pick prefers every
     * rival - POSIX says "the caller is moved to the end of the list" */
    me->vruntime += (uint64_t)bore_slice_for(me->burst_score) * BORE_VRUNIT;

    me->state = PROC_STATE_READY;
    fpu_save(me->fpu_area);
    process_t *next = sched_pick_excluding(me);
    if (next) sched_switch_to(next);            /* noreturn (we resume later) */
    fpu_restore(me->fpu_area);
    user_resume_asm(me);                        /* noreturn */
    __builtin_unreachable();
}

/* Resume a freshly-built user image without returning into the syscall
 * tail - required for ia32 execve, because sysretq can only re-enter
 * 64-bit code (CS comes from MSR STAR) while a 32-bit image must be
 * entered via an iretq that swaps CS to GDT_USER_CODE32.  The new
 * image's complete user context has already been installed in P by the
 * execve path (sched_ctx + sched_ctx_valid); this helper just hands
 * control to the standard resume machinery. */
void sched_resume_new_image(process_t *p) {
    p->kctx_valid      = false;
    p->sched_ctx_valid = true;
    p->state           = PROC_STATE_READY;
    if (p->fpu_valid) fpu_restore(p->fpu_area);
    else {
        fpu_restore_init(p->fpu_area);
        p->fpu_valid = true;
    }
    process_t *next = sched_pick_excluding(p);
    if (next) sched_switch_to(next);            /* noreturn */
    user_resume_asm(p);                         /* noreturn */
    __builtin_unreachable();
}
