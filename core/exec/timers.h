#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "exec/syscall_table.h"

/*
 *  timers - POSIX per-process interval timers
 *
 *  timer_create(222) / timer_settime(223) / timer_gettime(224) /
 *  timer_getoverrun(225) / timer_delete(226) - the Linux x86-64 numbers
 *  (note: 222-226, not the 32-bit table's 259-263).
 *
 *  The kernel<->musl ABI is musl's `struct ksigevent` (not POSIX struct
 *  sigevent - musl's timer_create.c re-packs it):
 *
 *      union sigval sigev_value;   // 8 bytes  @0
 *      int   sigev_signo;          //          @8
 *      int   sigev_notify;         //          @12
 *      int   sigev_tid;            //          @16
 *
 *  sigev_notify: SIGEV_NONE(0) -> arm silently, SIGEV_SIGNAL(1) -> deliver
 *  to the owning process, SIGEV_THREAD_ID(4) -> deliver to the task with
 *  pid == sigev_tid (this is how musl implements SIGEV_THREAD: a helper
 *  thread sigwaitinfo()s on signal 32 = SIGTIMER).  A NULL sigevent
 *  defaults to SIGEV_SIGNAL + SIGALRM.
 *
 *  Delivery sets the pending bit and wakes the target, the signal is
 *  actually delivered (or kills) at the target's next syscall return /
 *  tick - the same model as alarm(2)/kill(2) in this kernel.
 */

#define POSIX_TIMER_MAX 16

/* sigev_notify values (LINUX UAPI, not the textbook order:
 * SIGEV_SIGNAL is 0 and SIGEV_NONE is 1, matching musl/kernel) */
#define TMR_SIGEV_SIGNAL    0
#define TMR_SIGEV_NONE      1
#define TMR_SIGEV_THREAD    2
#define TMR_SIGEV_THREAD_ID 4

/* TIMER_ABSTIME flag for timer_settime */
#define TMR_ABSTIME 1

/* kernel-side ksigevent (musl layout, see above) */
typedef struct {
    uint64_t sigev_value;
    int32_t  sigev_signo;
    int32_t  sigev_notify;
    int32_t  sigev_tid;
    int32_t  __pad;
} ksigevent_t;

/* struct itimerspec (musl x86-64): two struct timespec (16 bytes each) */
typedef struct {
    int64_t it_interval_sec;
    int64_t it_interval_nsec;
    int64_t it_value_sec;
    int64_t it_value_nsec;
} kitimerspec_t;

/* Timer expiry scan, called from the IRQ0 tick (and cheap enough to
 * call from the syscall tail): fires every armed timer past its deadline,
 * queues its signal and re-arms. */
void posix_timers_tick(void);

/* Exit teardown: drop every timer owned by the exiting thread group so
 * ids can be reused. */
void posix_timers_release(int tgid);

/* syscall impls (called from the dispatcher) */
int64_t sys_timer_create(int32_t clockid, const ksigevent_t *sev, int32_t *out_id);
int64_t sys_timer_settime(int32_t id, int32_t flags,
                          const kitimerspec_t *newv, kitimerspec_t *oldv);
int64_t sys_timer_gettime(int32_t id, kitimerspec_t *cur);
int64_t sys_timer_getoverrun(int32_t id);
int64_t sys_timer_delete(int32_t id);

/* rt_sigtimedwait(23) - dequeue a pending signal matching mask (user
 * bit numbering, bit sig-1) into *info, park until one arrives or the
 * Timeout (ms, negative = forever) expires.  Returns the signal number
 * or -EAGAIN (timeout) / -EINVAL / -EFAULT. */
int64_t sys_rt_sigtimedwait(const uint64_t *umask, void *info,
                            const linux_timespec_t *timeout, size_t sigsetsize);
