/*
 *  locks - flock(2) + POSIX/OFD byte-range record locks
 *
 *  Two lock families share one per-file lock table:
 *
 *    +- flock:      owner = fd-slot address (the open file description)
 *    +- record:     owner = pid (POSIX) or fd-slot address (OFD)
 *
 *  Blocking (F_SETLKW, flock without LOCK_NB) parks in a probe loop via
 *  sched_kernel_park(), the same model as every other blocking syscall in
 *  this kernel.  Wakers: releasing a lock simply makes capacity visible,
 *  the parked probe (10 ms quantum) picks it up - bounded latency, no
 *  dedicated wait queues, no missed-wake window.
 *
 *  Ranges are absolute byte offsets (whence resolved at call time, l_len
 *  0 = to EOF-and-beyond, stored as OFF_MAX).
 */

#include "fs/locks.h"
#include "exec/process.h"
#include "exec/sched.h"
#include "drivers/timer/pit.h"
#include <rmm/module.h>
#include <rmm/modules.generated.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define EWOULDBLOCK EAGAIN
#define OFF_MAX   0x7FFFFFFFFFFFFFFFLL
#define RANGE_MAX 24      /* byte-range locks per file          */
#define FILE_MAX  24      /* files with any locks at once       */

/* RMM recovery epoch (+ manager fault handling)
 * Blocking loops here park on state that only this module (fs) changes.
 * If the module is recovered mid-wait (fault attribution), the
 * parked process must not hang forever: every loop captures the module
 * generation on entry, the manager bumps it in rmm_module_fault(), so
 * the probe detects the change and aborts with -EIO.  (A crash loop
 * that disables the module is covered by the state check.) */
static uint32_t fs_gen_capture(void) {
    return rmm_module_table[RMM_MODID_FS]->domain.generation;
}
static bool fs_gen_dead(uint32_t gen) {
    const rmm_module_t *m = rmm_module_table[RMM_MODID_FS];
    if (m->domain.generation != gen) return true;
    uint32_t st = atomic_load(&m->domain.state);
    return st == RMM_FAULTED || st == RMM_DISABLED;
}

typedef struct {
    bool     used;
    bool     is_flock;        /* flock entry vs record-lock range       */
    /* owner identity - slot pointer for flock/OFD, pid for POSIX */
    void    *slot;
    int      pid;
    int      type;            /* LK_RDLCK / LK_WRLCK (LK_UNLCK never stored) */
    int64_t  start;
    int64_t  end;             /* inclusive, OFF_MAX = to infinity       */
} lock_ent_t;

typedef struct {
    bool        used;
    vfs_node_t *node;
    lock_ent_t  ents[RANGE_MAX];
} file_locks_t;

static file_locks_t lock_tab[FILE_MAX];

/* tiny helpers */
static int64_t file_size(vfs_node_t *n) { return (int64_t)n->size; }

static bool sig_pending(void) {
    process_t *me = g_current_proc;
    return me && (me->pending_sigs & ~me->sig_mask) != 0;
}

/* per-file table lookup / allocation */
static file_locks_t *locks_for(vfs_node_t *node, bool create) {
    file_locks_t *free_slot = NULL;
    for (int i = 0; i < FILE_MAX; i++) {
        if (lock_tab[i].used && lock_tab[i].node == node) return &lock_tab[i];
        if (!lock_tab[i].used && !free_slot) free_slot = &lock_tab[i];
    }
    if (!create) return NULL;
    if (!free_slot) return NULL;
    free_slot->used = true;
    free_slot->node = node;
    for (int i = 0; i < RANGE_MAX; i++) free_slot->ents[i].used = false;
    return free_slot;
}

static void locks_maybe_free(file_locks_t *fl) {
    for (int i = 0; i < RANGE_MAX; i++)
        if (fl->ents[i].used) return;
    fl->used = false;
    fl->node = NULL;
}

/*
 *  flock(2)
 */
static lock_ent_t *flock_find(file_locks_t *fl, void *slot) {
    for (int i = 0; i < RANGE_MAX; i++)
        if (fl->ents[i].used && fl->ents[i].is_flock && fl->ents[i].slot == slot)
            return &fl->ents[i];
    return NULL;
}

static bool flock_conflict(file_locks_t *fl, void *slot, int type) {
    for (int i = 0; i < RANGE_MAX; i++) {
        lock_ent_t *e = &fl->ents[i];
        if (!e->used || !e->is_flock || e->slot == slot) continue;
        if (type == LK_WRLCK || e->type == LK_WRLCK) return true;
    }
    return false;
}

int64_t sys_flock(int fd, int op) {
    process_t *me = g_current_proc;
    if (!me) return -(int64_t)EINVAL;
    if (fd < 0 || fd >= VFS_MAX_FDS) return -(int64_t)EBADF;

    /* Node comes from the live fd table (fdt may be stale mid-burst);
     * the slot token is the owner's fdt address, stable across
     * save/restore, unique per open-file description. */
    vfs_node_t *node = vfs_fd_peek_node(fd);
    if (!node) return -(int64_t)EBADF;          /* bad fd / console pseudo  */
    void *slot = (void *)&me->fdt->t[fd];       /* OFD stand-in             */

    int mode = op & (LK_LOCK_SH | LK_LOCK_EX | LK_LOCK_UN);
    bool nb  = (op & LK_LOCK_NB) != 0;

    file_locks_t *fl = locks_for(node, mode != LK_LOCK_UN);
    if (!fl) return -(int64_t)ENOMEM;

    if (mode == LK_LOCK_UN) {
        lock_ent_t *e = flock_find(fl, slot);
        if (e) {
            e->used = false;
            locks_maybe_free(fl);
        }
        return 0;
    }
    if (mode != LK_LOCK_SH && mode != LK_LOCK_EX) return -(int64_t)EINVAL;
    /* flock's LOCK_SH/LOCK_EX live in a different numeric space than
     * the record-lock F_RDLCK/F_WRLCK, translate before storing or
     * comparing (LOCK_EX=2 would alias F_UNLCK and skip conflicts). */
    int lk_type = (mode == LK_LOCK_EX) ? LK_WRLCK : LK_RDLCK;

    /* upgrade/downgrade in place when we already hold it */
    for (;;) {
        lock_ent_t *mine = flock_find(fl, slot);
        if (!flock_conflict(fl, slot, lk_type)) {
            if (mine) {
                mine->type = lk_type;       /* convert SH <-> EX freely   */
            } else {
                lock_ent_t *e = NULL;
                for (int i = 0; i < RANGE_MAX; i++)
                    if (!fl->ents[i].used) { e = &fl->ents[i]; break; }
                if (!e) return -(int64_t)ENOMEM;
                e->used = true;
                e->is_flock = true;
                e->slot = slot;
                e->type = lk_type;
                e->start = 0;
                e->end = OFF_MAX;
            }
            return 0;
        }
        if (nb) return -(int64_t)EWOULDBLOCK;
        if (sig_pending()) return -(int64_t)EINTR;
        uint32_t gen = fs_gen_capture();
        if (sched_kernel_park(BORE_EVENT_POLL_MS) != 0)
            __asm__ volatile("sti; hlt; cli");
        if (fs_gen_dead(gen)) return -(int64_t)EIO;   /* module recovered */
    }
}

/*
 *  fcntl record locks
 */

/* Linux x86-64 struct flock */
typedef struct {
    int16_t l_type;
    int16_t l_whence;
    int64_t l_start;
    int64_t l_len;
    int32_t l_pid;
    int32_t __pad;
} lk_flock_t;

/* absolute byte range from a user flock struct, returns 0/-errno */
static int lk_range(vfs_node_t *node, const lk_flock_t *f,
                    int64_t *start, int64_t *end) {
    int64_t base;
    switch (f->l_whence) {
    case 0: base = 0; break;                       /* SEEK_SET            */
    case 1: base = 0; break;                       /* SEEK_CUR unsupported
                                                      without fd offset -
                                                      use 0 (rare in prac.) */
    case 2: base = file_size(node); break;         /* SEEK_END            */
    default: return -(int)EINVAL;
    }
    int64_t s = base + f->l_start;
    if (s < 0) s = 0;
    int64_t e = (f->l_len <= 0) ? OFF_MAX         /* 0 = to EOF+infinity  */
             : (s > OFF_MAX - (f->l_len - 1)) ? OFF_MAX
             : s + (f->l_len - 1);
    *start = s;
    *end   = e;
    return 0;
}

static bool overlap(int64_t s1, int64_t e1, int64_t s2, int64_t e2) {
    return s1 <= e2 && s2 <= e1;
}

static bool rec_conflict(file_locks_t *fl, void *slot, int pid,
                         int type, int64_t s, int64_t e) {
    for (int i = 0; i < RANGE_MAX; i++) {
        lock_ent_t *lk = &fl->ents[i];
        if (!lk->used || lk->is_flock) continue;
        bool same = (lk->slot && lk->slot == slot) || (pid >= 0 && lk->pid == pid);
        if (same) continue;
        if (type == LK_RDLCK && lk->type == LK_RDLCK) continue;  /* R vs R ok */
        if (!overlap(s, e, lk->start, lk->end)) continue;
        return true;
    }
    return false;
}

/* drop the [s,e] region from every lock owned by (slot|pid) */
static void rec_remove(file_locks_t *fl, void *slot, int pid,
                       int64_t s, int64_t e) {
    for (int i = 0; i < RANGE_MAX; i++) {
        lock_ent_t *lk = &fl->ents[i];
        if (!lk->used || lk->is_flock) continue;
        bool same = (lk->slot && lk->slot == slot) || (pid >= 0 && lk->pid == pid);
        if (!same || !overlap(s, e, lk->start, lk->end)) continue;

        bool head = lk->start < s;
        bool tail = lk->end > e;
        if (!head && !tail) { lk->used = false; continue; }
        if (head && tail) {
            /* split: shrink this one to the head, find a free slot for
             * the tail */
            lock_ent_t *n2 = NULL;
            for (int j = 0; j < RANGE_MAX; j++)
                if (!fl->ents[j].used) { n2 = &fl->ents[j]; break; }
            if (n2) {
                *n2 = *lk;
                n2->start = e + 1;
                lk->end = s - 1;
            } else {
                lk->end = s - 1;     /* pool exhausted: drop the tail */
            }
            continue;
        }
        if (head) lk->end = s - 1;
        else      lk->start = e + 1;
    }
}

/* replace any same-owner locks inside [s,e] (POSIX merge semantics) */
static void rec_clear_owner(file_locks_t *fl, void *slot, int pid,
                            int64_t s, int64_t e) {
    rec_remove(fl, slot, pid, s, e);
}

static lock_ent_t *rec_add(file_locks_t *fl, void *slot, int pid, int type,
                           int64_t s, int64_t e) {
    for (int i = 0; i < RANGE_MAX; i++) {
        lock_ent_t *lk = &fl->ents[i];
        if (lk->used) continue;
        lk->used = true;
        lk->is_flock = false;
        lk->slot = slot;
        lk->pid = pid;
        lk->type = type;
        lk->start = s;
        lk->end = e;
        return lk;
    }
    return NULL;
}

int64_t sys_fcntl_lock(int fd, int cmd, uint64_t uarg) {
    process_t *me = g_current_proc;
    if (!me) return -(int64_t)EINVAL;
    if (fd < 0 || fd >= VFS_MAX_FDS) return -(int64_t)EBADF;
    lk_flock_t *uf = (lk_flock_t *)(uintptr_t)uarg;
    if (!uf) return -(int64_t)EFAULT;

    vfs_node_t *node = vfs_fd_peek_node(fd);
    if (!node) return -(int64_t)EBADF;
    void *slot = (void *)&me->fdt->t[fd];

    bool ofd = (cmd == LK_F_OFD_GETLK || cmd == LK_F_OFD_SETLK ||
                cmd == LK_F_OFD_SETLKW);
    int  owner_pid = ofd ? -1 : (int)me->tgid;

    int64_t s, e;
    int r = lk_range(node, uf, &s, &e);
    if (r < 0) return r;

    file_locks_t *fl = locks_for(node, uf->l_type != LK_UNLCK);
    if (!fl) return -(int64_t)ENOMEM;

    switch (cmd) {
    case LK_F_GETLK:
    case LK_F_OFD_GETLK: {
        /* test: report the first conflicting lock (or l_type = F_UNLCK) */
        for (int i = 0; i < RANGE_MAX; i++) {
            lock_ent_t *lk = &fl->ents[i];
            if (!lk->used || lk->is_flock) continue;
            bool same = (lk->slot && lk->slot == slot) ||
                        (owner_pid >= 0 && lk->pid == owner_pid);
            if (same) continue;
            if (uf->l_type == LK_RDLCK && lk->type == LK_RDLCK) continue;
            if (!overlap(s, e, lk->start, lk->end)) continue;
            uf->l_type   = (int16_t)lk->type;
            uf->l_whence = (int16_t)0;
            uf->l_start  = lk->start;
            uf->l_len    = (lk->end >= OFF_MAX) ? 0 : lk->end - lk->start + 1;
            uf->l_pid    = lk->pid;
            return 0;
        }
        uf->l_type = LK_UNLCK;
        uf->l_pid  = 0;
        return 0;
    }
    case LK_F_SETLK:
    case LK_F_OFD_SETLK:
    case LK_F_SETLKW:
    case LK_F_OFD_SETLKW: {
        bool wait = (cmd == LK_F_SETLKW || cmd == LK_F_OFD_SETLKW);
        if (uf->l_type == LK_UNLCK) {
            rec_remove(fl, slot, owner_pid, s, e);
            locks_maybe_free(fl);
            return 0;
        }
        if (uf->l_type != LK_RDLCK && uf->l_type != LK_WRLCK)
            return -(int64_t)EINVAL;

        for (;;) {
            if (!rec_conflict(fl, slot, owner_pid, uf->l_type, s, e)) {
                rec_clear_owner(fl, slot, owner_pid, s, e);
                if (!rec_add(fl, slot, owner_pid, uf->l_type, s, e))
                    return -(int64_t)ENOMEM;
                return 0;
            }
            if (!wait) return -(int64_t)EAGAIN;
            if (sig_pending()) return -(int64_t)EINTR;
            uint32_t gen = fs_gen_capture();
            if (sched_kernel_park(BORE_EVENT_POLL_MS) != 0)
                __asm__ volatile("sti; hlt; cli");
            if (fs_gen_dead(gen)) return -(int64_t)EIO;   /* module recovered */
        }
    }
    default:
        return -(int64_t)EINVAL;
    }
}

/*
 *  release hooks
 */

/* POSIX semantics: closing any fd for this file drops all record locks
 * the calling process holds on it (flock/OFD locks stay with the OFD). */
static void posix_release_node(vfs_node_t *node, int tgid) {
    file_locks_t *fl = locks_for(node, false);
    if (!fl) return;
    for (int i = 0; i < RANGE_MAX; i++) {
        lock_ent_t *lk = &fl->ents[i];
        if (lk->used && !lk->is_flock && lk->pid == tgid) lk->used = false;
    }
    locks_maybe_free(fl);
}

void locks_release_fd(int fd) {
    process_t *me = g_current_proc;
    if (!me || !me->fdt) return;
    if (fd < 0 || fd >= VFS_MAX_FDS) return;
    vfs_node_t *node = vfs_fd_peek_node(fd);
    if (!node) return;
    void *slot = (void *)&me->fdt->t[fd];

    /* flock + OFD record locks die with the description */
    file_locks_t *fl = locks_for(node, false);
    if (fl) {
        for (int i = 0; i < RANGE_MAX; i++) {
            lock_ent_t *lk = &fl->ents[i];
            if (lk->used && lk->slot == slot) lk->used = false;
        }
        locks_maybe_free(fl);
    }
    /* POSIX record locks die with any close by the process */
    posix_release_node(node, (int)me->tgid);
}

void locks_release_process(int tgid) {
    for (int f = 0; f < FILE_MAX; f++) {
        file_locks_t *fl = &lock_tab[f];
        if (!fl->used) continue;
        for (int i = 0; i < RANGE_MAX; i++) {
            lock_ent_t *lk = &fl->ents[i];
            if (!lk->used) continue;
            if (!lk->is_flock && lk->pid == tgid) lk->used = false;
            /* flock/OFD locks survive process death if another task
             * inherited the slot, not expressible here, they die with
             * the fd slot via locks_release_fd. */
        }
        locks_maybe_free(fl);
    }
}
