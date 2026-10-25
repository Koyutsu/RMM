#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "exec/process.h"

/*
 * sched - BORE scheduler (Bursty-Oriented Round-robin Enhancement)
 *
 * Adaptation of Firelight's BORE patchset ideas to RMM:
 *
 *   - Every task carries a burst_score: 0 = highly interactive
 *     (runs in short bursts with sleeps between them), up to
 *     BORE_MAX_SCORE = "steady" CPU hog.  The score is updated when a
 *     task finally stops running (sleep/exit): the CPU time consumed
 *     in the just-ended wake cycle is classified with ilog2 and the
 *     old score decays a little each cycle (score = max(raw, score-2)),
 *     so sustained hogs stay high while a temporary burst fades away.
 *
 *   - vruntime burn rate scales with the score: an interactive task
 *     accrues 1024/ms while a max-score hog accrues ~11x that, so the
 *     pick-next rule (lowest vruntime) naturally re-balances CPU time
 *     toward interactive tasks.
 *
 *   - The time slice is dynamic: bursty tasks get short slices for
 *     snappy response, hogs get longer slices for throughput.
 *
 *   - On wake a task receives a vruntime credit inversely proportional
 *     to its score (up to BORE_WAKE_CREDIT_MS for the most bursty),
 *     letting interactive tasks jump the queue right after their sleep.
 *
 * Preemption model (single CPU):
 *   - IRQ0 (PIT 1 kHz) runs sched_irq0_c(): accounts runtime/vruntime,
 *     wakes expired sleepers, checks for fatal default-action signals
 *     and - if the tick caught the CPU in user mode and the slice is
 *     exhausted - saves the full register file into sched_ctx and
 *     switches straight to the next READY task (never returns to the
 *     interrupted stub).
 *   - Blocking syscalls (nanosleep, tty read, poll, waitpid) park the
 *     task via sched_kernel_park(): a setjmp-style kernel continuation
 *     is stored in kctx and the scheduler runs something else, on wake
 *     the parked call chain simply continues as if nothing happened.
 *   - syscalls themselves are never preempted mid-flight (the tick
 *     only switches from user mode), so all kernel static buffers stay
 *     exclusive to the one active syscall, same invariant as before.
 *
 * The old "parent sleeps / run-child-inline" model (proc_exec's
 * label-0 longjmp) is fully replaced:
 *   - fork(): child gets a saved user context (rax=0) and is enqueued;
 *     the parent returns immediately.
 *   - waitpid(): parent parks (kernel continuation) until a matching
 *     child turns ZOMBIE, then reaps it like the old zombie path.
 *   - exit(): marks ZOMBIE, wakes a waiting parent, then switches away
 *     and never returns.
 */

/* BORE policy knobs */
#define BORE_VRUNIT            1024u  /* vruntime units per 1 ms (2^10)      */
#define BORE_MAX_SCORE         20u    /* "steady" hog ceiling                */
#define BORE_DECAY             2u     /* score decay per closed burst        */
#define BORE_HOG_PER_SCORE     512u   /* extra vruntime burn per score unit  */
#define BORE_BASE_SLICE_MS     2u     /* slice floor (interactive)           */
#define BORE_SLICE_PER_SCORE   1u     /* +1 ms slice per score unit          */
#define BORE_MAX_SLICE_MS      20u    /* slice ceiling (hogs)                */
#define BORE_WAKE_CREDIT_MS    30u    /* max vruntime credit on wake         */
#define BORE_EVENT_POLL_MS     10u    /* park quantum for tty/poll probes    */
#define BORE_WAIT_FALLBACK_MS  50u    /* waitpid re-scan fallback            */

/* API */

/* Boot the scheduler: never returns.  Builds init's first user context,
 * installs the IRQ0 preemption stub and becomes the idle loop. */
void sched_start(process_t *init) __attribute__((noreturn));

/* IRQ0 C handler, called from sched_irq0_stub with a full register file.
 * Sends EOI itself.  Returns normally to resume the interrupted task, or
 * switches away and never returns. */
struct irq_frame;
void sched_irq0_c(void *frame);

/* Park the current task inside a blocking kernel loop.  Returns after the
 * task has been woken and its kernel continuation restored.  timeout_ms=0
 * means "event only" (no timer).  Returns -1 when the scheduler is not
 * active (pre-boot) so callers can fall back to hlt-loops. */
int  sched_kernel_park(uint64_t timeout_ms);

/* Make a task runnable again (SLEEPING/WAITING -> READY, applying the BORE
 * wake credit).  Safe on already-runnable tasks. */
void sched_wake(process_t *p);

/* BORE bookkeeping for a freshly forked child: inherits the parent's
 * vruntime (prevents fork-storms from jumping the queue) and half its
 * burst score.  Called from proc_fork before the child is enqueued. */
void sched_fork_inherit(process_t *child);

/* sched_yield(2) - voluntarily requeue behind every other
 * runnable task (vruntime charge of one full slice) and switch.  Called
 * with the live syscall frame, the task resumes user mode with rax=0
 * when it is next dispatched.  Returns (without switching) when no
 * other task is runnable. */
void sched_yield_from_syscall(syscall_frame_t *frame);

/* resume a freshly-built user image through the scheduler's
 * standard resume path instead of the syscall-tail sysretq, the only
 * way to ENTER a 32-bit (ia32) image, since sysretq always returns to
 * the 64-bit user code segment.  P's complete user context must already
 * be installed (sched_ctx + sched_ctx_valid).  Never returns. */
void sched_resume_new_image(process_t *p) __attribute__((noreturn));

/* Switch away from the exiting task and never return.  Called by
 * proc_exit() after ZOMBIE bookkeeping is complete. */
void sched_exit_switch(void) __attribute__((noreturn));

/* Pick the READY task with the lowest vruntime (NULL if none). */
process_t *sched_pick(void);

/* Scheduler introspection (neofetch / /proc style reporting). */
bool     sched_active(void);
uint32_t sched_runnable_count(void);

/* Context-switch assembly (used internally, exposed for tests). */
void sched_irq0_stub(void);
