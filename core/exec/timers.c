/*
 *  timers - POSIX per-process interval timers + rt_sigtimedwait
 *
 *  Model: a 16-slot global timer table.  Expiry scanning happens on the
 *  1 ms PIT tick (posix_timers_tick from sched_irq0_c) - a timer fires by
 *  setting the target's pending-signal bit and waking it (the standard
 *  signal model of this kernel).  Overruns count expiries that happened
 *  while the previous signal was still pending, per POSIX.
 *
 *  Ownership: a timer belongs to the creating thread group (tgid).  Any
 *  member may set/get/delete it, ids die with the group.
 */

#include "exec/timers.h"
#include "exec/process.h"
#include "exec/sched.h"
#include "drivers/timer/pit.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* clock ids (Linux) */
#define CLK_REALTIME  0
#define CLK_MONOTONIC 1

typedef struct {
    bool     used;
    int      owner_tgid;      /* creating thread group                  */
    int32_t  clockid;
    uint64_t sigev_value;     /* sigval passed back in siginfo          */
    int32_t  signo;           /* signal to queue (0 for SIGEV_NONE)     */
    int32_t  notify;          /* SIGEV_*                                */
    int32_t  tid;             /* SIGEV_THREAD_ID target (0 = process)   */
    uint64_t interval_ms;     /* 0 = one-shot                           */
    uint64_t next_ms;         /* absolute PIT deadline (0 = disarmed)   */
    uint32_t overrun;         /* expiries while signal still pending    */
    uint32_t pending;         /* signal queued, not yet dequeued        */
} posix_timer_t;

static posix_timer_t g_timers[POSIX_TIMER_MAX];

/* true when the calling task has a deliverable (unblocked) signal */
static bool sig_pending_for_me(void) {
    process_t *me = g_current_proc;
    return me && (me->pending_sigs & ~me->sig_mask) != 0;
}

/* helpers */
static posix_timer_t *timer_of(int32_t id) {
    if (id < 0 || id >= POSIX_TIMER_MAX) return NULL;
    posix_timer_t *t = &g_timers[id];
    return t->used ? t : NULL;
}

static int timer_owned(posix_timer_t *t) {
    process_t *me = g_current_proc;
    if (!me) return 0;
    return t->owner_tgid == me->tgid;
}

/* deliver a timer signal to its target per sigev_notify */
static void timer_deliver(posix_timer_t *t) {
    if (t->notify == TMR_SIGEV_NONE || t->signo <= 0 || t->signo > 63) return;

    int target_pid = (t->notify == TMR_SIGEV_THREAD_ID) ? t->tid : 0;
    bool delivered = false;

    for (int i = 0; i < PROC_MAX; i++) {
        process_t *p = &g_proc_table[i];
        if (p->state == PROC_STATE_EMPTY) continue;
        if (target_pid) {
            if (p->pid != target_pid) continue;
        } else {
            if (p->tgid != t->owner_tgid) continue;
        }
        if (t->pending) t->overrun++;          /* previous sig not consumed */
        t->pending = 1;
        p->pending_sigs |= (1ull << t->signo);
        {
            extern void serial_print_string(const char *);
            char b[8]; int i2;
            serial_print_string("[dlv p=");
            i2 = 7; b[i2] = 0;
            uint64_t v = (uint64_t)p->pid; do { b[--i2] = '0' + (v % 10); v /= 10; } while (v && i2);
            serial_print_string(&b[i2]);
            serial_print_string("]\n");
        }
        sched_wake(p);                         /* parked sleepers re-check  */
        delivered = true;
        break;                                 /* one target is enough      */
    }
    (void)delivered;
}

/* expiry scan (PIT tick) */
void posix_timers_tick(void) {
    uint64_t now = pit_ticks();
    for (int i = 0; i < POSIX_TIMER_MAX; i++) {
        posix_timer_t *t = &g_timers[i];
        if (!t->used || t->next_ms == 0) continue;
        if (now < t->next_ms) continue;
        {
            extern void serial_print_string(const char *);
            char b[8]; int i2;
            serial_print_string("[fire i=");
            i2 = 7; b[i2] = 0;
            uint64_t v = (uint64_t)i; do { b[--i2] = '0' + (v % 10); v /= 10; } while (v && i2);
            serial_print_string(&b[i2]);
            serial_print_string(" sig=");
            i2 = 7; b[i2] = 0;
            v = (uint64_t)t->signo; do { b[--i2] = '0' + (v % 10); v /= 10; } while (v && i2);
            serial_print_string(&b[i2]);
            serial_print_string(" tid=");
            i2 = 7; b[i2] = 0;
            v = (uint64_t)t->tid; do { b[--i2] = '0' + (v % 10); v /= 10; } while (v && i2);
            serial_print_string(&b[i2]);
            serial_print_string(" not=");
            i2 = 7; b[i2] = 0;
            v = (uint64_t)t->notify; do { b[--i2] = '0' + (v % 10); v /= 10; } while (v && i2);
            serial_print_string(&b[i2]);
            serial_print_string("]\n");
        }
        timer_deliver(t);
        if (t->interval_ms) {
            /* re-arm, catch up at most to "now" to avoid burst-delivery
             * loops after a long stall */
            t->next_ms += t->interval_ms;
            if (t->next_ms <= now) t->next_ms = now + t->interval_ms;
        } else {
            t->next_ms = 0;                    /* one-shot complete         */
        }
    }
}

void posix_timers_release(int tgid) {
    for (int i = 0; i < POSIX_TIMER_MAX; i++)
        if (g_timers[i].used && g_timers[i].owner_tgid == tgid)
            g_timers[i].used = false;
}

/* ms <-> timespec conversion */
static uint64_t spec_to_ms(int64_t sec, int64_t nsec) {
    if (sec < 0) sec = 0;
    if (nsec < 0) nsec = 0;
    uint64_t ms = (uint64_t)sec * 1000u + (uint64_t)nsec / 1000000u;
    return ms;
}

static void ms_to_spec(uint64_t ms, int64_t *sec, int64_t *nsec) {
    *sec  = (int64_t)(ms / 1000u);
    *nsec = (int64_t)((ms % 1000u) * 1000000u);
}

/*
 *  syscalls
 */

int64_t sys_timer_create(int32_t clockid, const ksigevent_t *sev, int32_t *out_id) {
    process_t *me = g_current_proc;
    if (!me) return -(int64_t)EINVAL;
    if (clockid != CLK_REALTIME && clockid != CLK_MONOTONIC)
        return -(int64_t)EINVAL;

    posix_timer_t *t = NULL;
    for (int i = 0; i < POSIX_TIMER_MAX; i++)
        if (!g_timers[i].used) { t = &g_timers[i]; break; }
    if (!t) return -(int64_t)EAGAIN;

    int32_t slot = (int32_t)(t - g_timers);
    t->used       = true;
    t->owner_tgid = me->tgid;
    t->clockid    = clockid;
    t->next_ms    = 0;
    t->interval_ms= 0;
    t->overrun    = 0;
    t->pending    = 0;
    t->tid        = 0;
    t->sigev_value= 0;
    t->signo      = SIGALRM;                 /* kernel default: SIGALRM    */
    t->notify     = TMR_SIGEV_SIGNAL;        /* = 0 (Linux UAPI)           */

    if (sev) {
        t->sigev_value = sev->sigev_value;
        t->signo       = sev->sigev_signo;
        t->notify      = sev->sigev_notify;
        t->tid         = sev->sigev_tid;
        if (t->notify != TMR_SIGEV_NONE && t->notify != TMR_SIGEV_SIGNAL &&
            t->notify != TMR_SIGEV_THREAD_ID) {
            t->used = false;
            return -(int64_t)EINVAL;         /* SIGEV_THREAD is userspace  */
        }
        if (t->signo < 0 || t->signo > 63) {
            t->used = false;
            return -(int64_t)EINVAL;
        }
    }

    if (out_id) *out_id = slot;
    return 0;
}

int64_t sys_timer_settime(int32_t id, int32_t flags,
                          const kitimerspec_t *newv, kitimerspec_t *oldv) {
    if (flags != 0 && flags != TMR_ABSTIME) return -(int64_t)EINVAL;
    posix_timer_t *t = timer_of(id);
    if (!t || !timer_owned(t)) return -(int64_t)EINVAL;
    if (!newv) return -(int64_t)EFAULT;

    /* report the previous setting */
    if (oldv) {
        uint64_t now = pit_ticks();
        uint64_t rem = (t->next_ms > now) ? t->next_ms - now : 0;
        ms_to_spec(rem, &oldv->it_value_sec, &oldv->it_value_nsec);
        ms_to_spec(t->interval_ms, &oldv->it_interval_sec, &oldv->it_interval_nsec);
    }

    uint64_t val_ms  = spec_to_ms(newv->it_value_sec,  newv->it_value_nsec);
    uint64_t int_ms  = spec_to_ms(newv->it_interval_sec, newv->it_interval_nsec);

    t->interval_ms = (newv->it_interval_sec || newv->it_interval_nsec) ? int_ms : 0;
    if (val_ms == 0) {
        t->next_ms = 0;                      /* disarm                     */
        t->pending = 0;
        return 0;
    }
    t->next_ms = pit_ticks() + val_ms;
    return 0;
}

int64_t sys_timer_gettime(int32_t id, kitimerspec_t *cur) {
    posix_timer_t *t = timer_of(id);
    if (!t || !timer_owned(t)) return -(int64_t)EINVAL;
    if (!cur) return -(int64_t)EFAULT;
    uint64_t now = pit_ticks();
    uint64_t rem = (t->next_ms > now) ? t->next_ms - now : 0;
    ms_to_spec(rem, &cur->it_value_sec, &cur->it_value_nsec);
    ms_to_spec(t->interval_ms, &cur->it_interval_sec, &cur->it_interval_nsec);
    return 0;
}

int64_t sys_timer_getoverrun(int32_t id) {
    posix_timer_t *t = timer_of(id);
    if (!t || !timer_owned(t)) return -(int64_t)EINVAL;
    int64_t r = (int64_t)t->overrun;
    t->overrun = 0;                          /* POSIX: read-and-reset      */
    return r;
}

int64_t sys_timer_delete(int32_t id) {
    posix_timer_t *t = timer_of(id);
    if (!t || !timer_owned(t)) return -(int64_t)EINVAL;
    t->used = false;
    return 0;
}

/*
 *  rt_sigtimedwait(23) - the dequeue side of POSIX timers
 *
 *  siginfo_t (musl x86-64) fields we fill:
 *      si_signo @0, si_errno @4, si_code @8 (SI_TIMER = -2),
 *      union @16 -> union sigval (8 bytes)
 *  The struct is 128 bytes, everything past the fields above is zeroed.
 */

#define SI_TIMER_CODE (-2)

typedef struct {
    int32_t  si_signo;
    int32_t  si_errno;
    int32_t  si_code;
    int32_t  __pad;
    uint64_t si_value;        /* union sigval                            */
} ksiginfo_t;

/* caller-side view with Linux SI_TIMER fields (siginfo offset 16..31) */
typedef struct {
    int32_t  si_signo;
    int32_t  si_errno;
    int32_t  si_code;
    int32_t  __pad;
    int32_t  si_timerid;
    int32_t  si_overrun;
    uint64_t si_value;        /* union sigval @ offset 24                */
} ktimer_siginfo_t;

int64_t sys_rt_sigtimedwait(const uint64_t *umask, void *info,
                            const linux_timespec_t *timeout, size_t sigsetsize) {
    process_t *me = g_current_proc;
    if (!me) return -(int64_t)EINVAL;
    if (sigsetsize < 8) return -(int64_t)EINVAL;

    /* user bit N-1 <-> kernel bit N */
    uint64_t want = 0;
    if (umask) {
        want = (*umask) << 1;
        if (!want) return -(int64_t)EINVAL;  /* empty set: Linux -> EINVAL  */
    }

    uint64_t deadline = 0;
    bool     timed    = false;
    if (timeout && (timeout->tv_sec >= 0 || timeout->tv_nsec > 0)) {
        deadline = pit_ticks() + spec_to_ms(timeout->tv_sec, timeout->tv_nsec);
        timed = true;
    }

    for (;;) {
        /* POSIX sigwait semantics: the want mask selects among all pending
         * signals, including ones the caller has blocked (blocking the
         * signal first is exactly how sigwait* is meant to be used). */
        uint64_t cand = me->pending_sigs & want;
        if (cand) {
            /* lowest-numbered matching signal wins (Linux scans upward) */
            for (int s = 1; s < 64; s++) {
                if (!(cand & (1ull << s))) continue;
                me->pending_sigs &= ~(1ull << s);
                if (info) {
                    ksiginfo_t *si = (ksiginfo_t *)info;
                    uint8_t *z = (uint8_t *)si;
                    for (size_t k = 0; k < 128; k++) z[k] = 0;
                    si->si_signo = s;
                    si->si_errno = 0;
                    si->si_code  = SI_TIMER_CODE;
                    si->si_value = 0;
                    /* attach the originating timer's sigval when the
                     * signal is a timer signal still tracking it.
                     * Linux SI_TIMER siginfo: si_overrun @16,
                     * si_timerid @20, si_value @24. */
                    for (int i = 0; i < POSIX_TIMER_MAX; i++) {
                        posix_timer_t *t = &g_timers[i];
                        if (t->used && t->signo == s && t->pending) {
                            ktimer_siginfo_t *ti = (ktimer_siginfo_t *)info;
                            ti->si_value    = t->sigev_value;
                            ti->si_timerid  = i;
                            ti->si_overrun  = (int32_t)t->overrun;
                            break;
                        }
                    }
                }
                return s;
            }
        }
        if (sig_pending_for_me()) return -(int64_t)EINTR;
        if (timed && pit_ticks() >= deadline) return -(int64_t)EAGAIN;
        if (sched_kernel_park(BORE_EVENT_POLL_MS) != 0)
            __asm__ volatile("sti; hlt; cli");
    }
}
