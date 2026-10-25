#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "exec/elf.h"
#include "fs/vfs.h"
#include "exec/syscall_table.h"

/*
 * process - multi-process ring-3 execution with wait/exit lifecycle
 *
 * Lifecycle
 *
 *   proc_create() - load ELF, allocate stacks              -> READY
 *   proc_exec() - iretq to ring 3                        -> RUNNING
 *   SYS_EXIT - proc_exit(): free user pages, mark ZOMBIE, wake parent
 *   SYS_WAITPID - proc_waitpid(): reap ZOMBIE, free kstack -> empty
 *
 * "Parent sleeps" model
 *
 * There is no preemptive scheduler.  "Sleeping" means the parent's kernel
 * thread is blocked inside proc_waitpid() -> proc_exec(child), with
 * interrupts enabled so PIT/keyboard still fire.
 *
 * When child calls SYS_EXIT:
 *   1. proc_exit() frees child's user pages immediately.
 *   2. Marks child ZOMBIE, stores exit code.
 *   3. If parent is PROC_STATE_WAITING for this child, writes exit status
 *      into parent's wstatus_ptr, patches parent's wait_frame return value
 *      to child PID, then longjmps back through proc_exec into proc_waitpid.
 *   4. proc_waitpid() frees child kstack + proc_free(), returns child PID.
 *
 * Resource cleanup
 *
 *   proc_exit() - frees user pages (code + stack) of exiting process
 *   proc_waitpid() - frees kernel stack pages, calls proc_free()
 *
 * Memory layout per process
 *
 *   0x200000 ...        ELF load segments  (PTE_USER | PTE_PRESENT | PTE_WRITABLE)
 *   0x7fff0000 ...      user stack  (PROC_STACK_PAGES x 4 KiB, grows down)
 *
 * Segment selectors
 *   CS = GDT_USER_CODE | 3  = 0x23
 *   SS = GDT_USER_DATA | 3  = 0x1B
 */

#define PROC_STACK_BASE     0x7fff0000ULL
#define PROC_STACK_PAGES    4
#define PROC_STACK_TOP      (PROC_STACK_BASE + (uint64_t)PROC_STACK_PAGES * 4096)

#define PROC_KSTACK_PAGES   4
#define PROC_KSTACK_SIZE    ((uint64_t)PROC_KSTACK_PAGES * 4096)

#define PROC_MAX            24   /* 8 -> 24 - pthreads need slots */

#define PROC_USER_VA_START  0x200000ULL
#define PROC_USER_VA_END    PROC_STACK_TOP

/* Per-process anonymous mmap VA region */
#define PROC_MMAP_BASE   0x40000000ULL   /* 1 GiB - above heap, below stack */
#define PROC_MMAP_LIMIT  0x7ff00000ULL   /* just below stack                 */

/* Process state */
typedef enum {
    PROC_STATE_EMPTY   = 0,
    PROC_STATE_READY,
    PROC_STATE_RUNNING,
    PROC_STATE_WAITING,  /* blocked in waitpid, woken by proc_exit        */
    PROC_STATE_ZOMBIE,   /* exited, not yet reaped by parent               */
    PROC_STATE_SLEEPING, /* parked with (optional) timer, nanosleep/poll  */
} proc_state_t;

/* BORE scheduler context types
 * sched_ctx_t - a complete USER-mode register context, used for:
 *   - tasks preempted by the tick (saved from the IRQ0 frame),
 *   - tasks parked inside a blocking syscall (converted from the
 *     syscall frame),
 *   - freshly forked children (converted from the fork frame, rax=0),
 *   - the initial entry of a new image (built from entry/argc/argv).
 * One restore path (user_resume_asm) handles all of them.
 *
 * kctx_t - a KERNEL-mode continuation (setjmp-style) for tasks parked
 * Inside a blocking kernel loop (console read, poll, waitpid).  The
 * kernel stack below the parked call chain stays intact, so resuming
 * just restores rsp/rbp/callee-saved regs and jumps back.           */
typedef struct {
    uint64_t rax, rbx, rcx, rdx, rsi, rdi, rbp;
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
    uint64_t rip, rflags, user_rsp;
} sched_ctx_t;

typedef struct {
    uint64_t rip, rsp, rbp, rbx, r12, r13, r14, r15;
} kctx_t;

/*
 * Syscall frame layout as pushed by syscall_entry.S (the `syscall`
 * instruction path) - 16 registers, 128 bytes total.
 *
 * Push order (last push = lowest address, offset 0):
 *   r9   a6      0
 *   r8   a5      8
 *   r10  a4     16
 *   rdx  a3     24
 *   rsi  a2     32
 *   rdi  a1     40
 *   rax  nr     48
 *   r15         56
 *   r14         64
 *   r13         72
 *   r12         80
 *   rbx         88
 *   rbp         96
 *   r11  rflags 104
 *   rcx  rip    112
 *   user_rsp    120
 *
 * Return is via sysretq: restores rip from rcx, rflags from r11,
 * and the stub pops user_rsp into %rsp explicitly.
 */
typedef struct {
    uint64_t r9;       /*   0  a6                */
    uint64_t r8;       /*   8  a5                */
    uint64_t r10;      /*  16  a4                */
    uint64_t rdx;      /*  24  a3                */
    uint64_t rsi;      /*  32  a2                */
    uint64_t rdi;      /*  40  a1                */
    uint64_t rax;      /*  48  syscall nr / retval */
    uint64_t r15;      /*  56                    */
    uint64_t r14;      /*  64                    */
    uint64_t r13;      /*  72                    */
    uint64_t r12;      /*  80                    */
    uint64_t rbx;      /*  88                    */
    uint64_t rbp;      /*  96                    */
    uint64_t r11;      /* 104  user RFLAGS       */
    uint64_t rcx;      /* 112  user RIP          */
    uint64_t user_rsp; /* 120  user RSP          */
} syscall_frame_t;

typedef struct {
    proc_state_t  state;
    int           pid;
    int           parent_pid;
    /* Thread group
     * Every task carries its thread-group id.  For plain processes tgid
     * == pid.  A CLONE_THREAD clone shares its leader's tgid, address
     * space (cr3) and (via vfs_fdt_t) fd table, getpid() returns tgid,
     * gettid() returns pid.  Threads never become zombies: their slot,
     * kstack and futex entries are released the moment they exit and
     * musl reaps them through futexes in user memory, not wait4().   */
    int           tgid;
    bool          is_thread;
    uint64_t      clear_child_tid;   /* CTID ptr - kernel writes 0 + wakes futex on exit */
    bool          futex_woken;       /* FUTEX_WAIT classification (woken vs timeout)     */
    int           exit_code;
    int           exit_code_tmp;   /* written by asm resume path in proc_exec */

    elf_image_t   image;

    uint64_t      entry;
    uint64_t      user_rsp;

    /* heap - managed by SYS_BRK */
    uint64_t      heap_start;   /* fixed: page after last ELF segment       */
    uint64_t      heap_brk;     /* current program break (grows up)         */

    /* kernel stack - base tracked so we can free it on waitpid reap */
    uint64_t      kstack_base;
    uint64_t      kstack_top;

    /* Per-process address space (PML4 physical address).
     * 0 = run in the bootstrap/root address space (kernel-launched init). */
    uint64_t      cr3;

    /* longjmp context saved by proc_exec, restored by proc_exit */
    uint64_t      ret_rsp;
    uint64_t      ret_rbp;
    uint64_t      ret_rip;

    /* iretq frame values populated before proc_exec's inline asm */
    uint64_t      iretq_ss;
    uint64_t      iretq_rsp;
    uint64_t      iretq_rflags;
    uint64_t      iretq_cs;
    uint64_t      iretq_rip;

    /*
     * waitpid state - populated when this process calls SYS_WAITPID and
     * the target child has not yet exited.
     */
    int               wait_for_pid;  /* child PID to wait for  (-1 = any) */
    int              *wstatus_ptr;   /* user VA to write encoded status    */
    syscall_frame_t  *wait_frame;    /* syscall frame of the waitpid call  */

    /* saved frame pointer for fork/execve */
    syscall_frame_t  *saved_frame;

    /*
     * Fork resume: when non-NULL, the process resumes in user mode from a
     * full copy of the parent's syscall frame (with rax = 0) instead of a
     * fresh argc/argv entry.  Set by proc_fork, consumed by proc_exec.
     */
    syscall_frame_t   fork_frame;
    int               has_fork_frame;

    /* FS base (TLS) for musl, written by ARCH_SET_FS arch_prctl */
    uint64_t          fs_base;

    int           argc;
    char        **argv;
    char        **envp;

    /* Process group / session */
    int           pgid;        /* process group ID (0 = same as pid)       */
    int           sid;         /* session ID (0 = same as pid)             */
    int           fg_pgrp;     /* foreground process group of this session */
    char          cwd[VFS_PATH_MAX]; /* Current working directory */

    /* Credentials (Linux uid model)
     * The kernel only tracks identity + enforces permissions.  The account
     * Database (/etc/passwd, /etc/group, /etc/shadow) is pure userspace.
     * Root = uid 0, which bypasses all VFS permission checks.
     * Inherited by fork via the struct copy in proc_fork, preserved across
     * execve (except the setuid/setgid file bits, applied in proc_execve). */
    uint32_t      uid,  euid,  suid;    /* real / effective / saved  uid */
    uint32_t      gid,  egid,  sgid;    /* real / effective / saved  gid */
    uint32_t      umask;               /* file mode creation mask       */
#define PROC_NGROUPS_MAX 16
    int           ngroups;             /* supplementary groups count    */
    uint32_t      groups[PROC_NGROUPS_MAX];

    /* Per-process signal action table */
    linux_sigaction_t sigactions[NSIG];

    /* Pending signals (bitmask, signals 1-31) */
    uint32_t      pending_sigs;

    /* Blocked-signal mask (rt_sigprocmask), masked signals stay
     * pending, signalfd(2) consumes them, delivery skips them */
    uint64_t      sig_mask;

    /* alarm deadline (PIT ms, 0 = not set) */
    uint64_t      alarm_deadline_ms; /* 0 = none */

    /* Per-process fd table snapshot
     * Saved by proc_fork before running the child, restored afterwards.
     * Also saved/restored across execve so the parent's fds survive.
     */
    /* Per-process fd table
     * Points at a vfs_fdt_t from the vfs.c pool.  fork/execve clone it,
     * a CLONE_FILES clone shares it (POSIX thread fd semantics).
     * The global live table in vfs.c is saved here on context switch and
     * restored from here on dispatch, exactly the old snapshot model,
     * except CLONE_FILES tasks save/restore through the same memory.
     */
    vfs_fdt_t    *fdt;

    /* Auto-reaped children
     * In the parent-sleeps model, fork() runs the child to completion and
     * reaps it before the parent's wait4() can see it.  Remember such
     * children here so waitpid still reports them (POSIX semantics).      */
    struct { int pid; int status; } reaped[8];
    int          reaped_count;

    /* Per-process mmap bump allocator
     * Each process gets its own anonymous-mmap VA region so processes don't
     * trample each other's mappings.  Initialised to MMAP_BASE on proc_create,
     * reset to MMAP_BASE on execve, saved/restored across fork like fd_table.
     */
    uint64_t      mmap_bump;

    /* dynamic linking + ia32 compat + FPU state */
    char          exec_path[VFS_PATH_MAX]; /* /proc/self/exe target      */
    char          comm[16];                /* prctl(PR_SET_NAME) name    */
    uint64_t      user_cs;                 /* 0x20 (64-bit) | 0x38 (ia32)*/
    bool          is_ia32;                 /* ia32 int-0x80 ABI in use   */
    /* Full extended (x87/SSE/AVX) state image, saved by the scheduler
     * around every switch (see cpu.c fpu_save/fpu_restore).  Inlined so
     * fork's struct copy hands the child a defined image. */
    uint8_t       fpu_area[4096] __attribute__((aligned(64)));
    bool          fpu_valid;

    /* RMM module execution context snapshot
     * Saved/restored by the scheduler around every switch: a task parked
     * inside a module call (blocking syscall) keeps its stub-frame chain
     * and module identity across other tasks' runs. */
    struct {
        void    *current_module;
        uint32_t module_id;
        uint32_t _pad;
        uint64_t module_generation;
        void    *current_call_frame;
        bool     valid;
    } rmm_ctx;

    /* BORE scheduler state
     * sched_ctx - saved user context for the resume path (see above)
     * kctx - kernel continuation for in-syscall parks
     * burst_ms - runtime accumulated in the current wake cycle, closed
     *              (-> burst_score) when the task finally sleeps/exits
     * burst_score - 0 = highly interactive (short bursts), grows with
     *              sustained CPU use ("steady" in BORE terms); drives both
     *              the vruntime burn rate and the dynamic time slice
     * vruntime - fixed-point 1024ths of a millisecond, runqueue order
     * wake_at_ms - SLEEPING/WAITING deadline in PIT ms (0 = event only) */
    sched_ctx_t   sched_ctx;
    bool          sched_ctx_valid;
    kctx_t        kctx;
    bool          kctx_valid;
    uint64_t      wake_at_ms;
    uint64_t      burst_ms;
    uint32_t      burst_score;
    uint32_t      slice_ticks;
    uint32_t      slice_used;
    uint64_t      vruntime;
    uint64_t      total_runtime_ms;
} process_t;

/* Encode / decode wait status (POSIX WEXITSTATUS style) */
#define WSTATUS_EXIT(code)   (((code) & 0xff) << 8)
#define WEXITSTATUS(status)  (((status) >> 8) & 0xff)
#define WIFEXITED(status)    (((status) & 0xff) == 0)

/* Global process table */
extern process_t  g_proc_table[PROC_MAX];
extern process_t *g_current_proc;

/* API */

int        proc_create  (process_t *proc, const char *path,
                          int argc, char **argv, char **envp);
int        proc_exec    (process_t *proc);

/* POSIX-thread support
 * proc_clone implements the CLONE_VM side of SYS_clone:
 *   flags = a1, stack = a2, parent_tid = a3, child_tid = a4, tls = a5
 * (x86-64 Linux ABI, musl's __clone.s maps its libc args onto these).
 * CLONE_VM|CLONE_THREAD tasks share cr3 + vfs_fdt_t, CLONE_FILES,
 * CLONE_SETTLS, CLONE_PARENT_SETTID, CLONE_CHILD_CLEARTID and
 * CLONE_CHILD_SETTID are honoured, fork-like flag sets fall through to
 * proc_fork.  Returns the child TID, or -ENOMEM.                     */
int        proc_clone   (syscall_frame_t *frame);
void       proc_kill_group_threads(process_t *except);  /* exit_group/execve teardown */
int        proc_group_live_count(int tgid, process_t *except);
void       proc_clear_ctid_wake(process_t *proc);       /* *ctid=0 + futex wake       */
void       proc_deliver_signal(process_t *proc, int sig, syscall_frame_t *frame);
void       proc_exit    (process_t *proc, int code) __attribute__((noreturn));
int        proc_fork    (syscall_frame_t *frame);
int        proc_execve  (const char *path, char *const argv[], char *const envp[],
                          syscall_frame_t *frame);
process_t *proc_alloc   (void);
void       proc_free    (process_t *proc);

/*
 * waitpid(pid, wstatus, options)
 *
 * pid == -1  wait for any child of g_current_proc
 * pid >  0   wait for that specific child
 *
 * If the target is already ZOMBIE, reap immediately and return.
 * Otherwise mark this process waiting, store wait_frame/wstatus_ptr,
 * and run the child via proc_exec() - which parks this kernel thread
 * until proc_exit() longjmps back.
 *
 * Returns child PID on success, -ECHILD if no such child.
 */
int proc_waitpid(int pid, int *wstatus, int options, syscall_frame_t *frame);

/*
 * brk(addr) - set the program break.
 *
 * addr == 0  -> query: return current break without changing it
 * addr >  0  -> set break to addr (page-aligned up)
 *
 * Growing: allocates and maps pages from old break to new break.
 * Shrinking: unmaps and frees pages from new break to old break.
 * Clamped to [heap_start, PROC_STACK_BASE).
 *
 * Returns new break on success, (uint64_t)-1 on OOM/invalid.
 */
uint64_t proc_brk(uint64_t addr);