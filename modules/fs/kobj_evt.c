#include "fs/kobj.h"
#include "exec/syscall_table.h"
#include "exec/process.h"
#include "exec/sched.h"
#include "drivers/memory/vmm.h"
#include "drivers/memory/memmap.h"
#include "drivers/timer/pit.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/*
 * kobj_evt.c - epoll instances, eventfd, timerfd, signalfd and memfd
 * (shared-memory files with page-list backing for wl_shm-style pools).
 * See kobj.h / kobj.c for the shared plumbing.
 */

extern uint64_t pit_ticks(void);

#define E_BADF     9
#define E_AGAIN    11
#define E_NOMEM    12
#define E_FAULT    14
#define E_INVAL    22
#define E_NOSYS    38
#define E_NOTSUP   95

/* VFS op shims (called from kobj.c) */
int kobj_evt_read(vfs_node_t *n, void *buf, size_t len, size_t off);
int kobj_evt_write(vfs_node_t *n, const void *buf, size_t len, size_t off);
int kobj_evt_close(vfs_node_t *n);
int kobj_evt_poll(vfs_node_t *n, int events);

/*
 *  epoll
 */
#define EPOLL_MAX_ENTRIES 32
#define EPOLL_MAX_INSTANCES 4

typedef struct {
    bool     used;
    int      fd;                 /* watched fd                        */
    uint32_t events;             /* requested EPOLL* bits             */
    uint64_t data;               /* user data token                   */
} epoll_ent_t;

typedef struct {
    kobj_hdr_t   hdr;            /* kind = KOBJ_EPOLL                 */
    epoll_ent_t  ents[EPOLL_MAX_ENTRIES];
    int          nents;
} epoll_obj_t;

static epoll_obj_t g_epolls[EPOLL_MAX_INSTANCES];

int kobj_epoll_create1(int flags) {
    (void)flags;
    epoll_obj_t *ep = NULL;
    for (int i = 0; i < EPOLL_MAX_INSTANCES; i++) {
        if (g_epolls[i].hdr.kind != KOBJ_EPOLL || g_epolls[i].nents == 0) {
            ep = &g_epolls[i];
            break;
        }
    }
    if (!ep) return -(int)E_NOMEM;
    ep->hdr.kind = KOBJ_EPOLL;
    ep->hdr.fd_refs = 1;
    ep->nents = 0;
    extern int vfs_install_fd(vfs_node_t *node, int flags);
    /* install through the shared kobj node machinery */
    extern vfs_node_t *kobj_new_node_public(const char *name, void *obj);
    vfs_node_t *n = kobj_new_node_public("epoll", ep);
    if (!n) return -(int)E_NOMEM;
    int fd = vfs_install_fd(n, 0);
    if (fd < 0) ep->hdr.fd_refs = 0;
    return fd;
}

int kobj_epoll_ctl(int epfd, int op, int fd, ko_epoll_event_t *ev) {
    epoll_obj_t *ep = (epoll_obj_t *)kobj_of(epfd, KOBJ_EPOLL);
    if (!ep) return -(int)E_BADF;

    switch (op) {
    case KO_EPOLL_CTL_ADD: {
        if (!ev) return -(int)E_FAULT;
        if (ep->nents >= EPOLL_MAX_ENTRIES) return -(int)E_NOMEM;
        for (int i = 0; i < ep->nents; i++)
            if (ep->ents[i].used && ep->ents[i].fd == fd)
                return -(int)E_INVAL;   /* EEXIST-ish */
        epoll_ent_t *e = &ep->ents[ep->nents++];
        e->used   = true;
        e->fd     = fd;
        e->events = ev->events;
        e->data   = ev->data.u64;
        return 0;
    }
    case KO_EPOLL_CTL_MOD: {
        if (!ev) return -(int)E_FAULT;
        for (int i = 0; i < ep->nents; i++) {
            if (ep->ents[i].used && ep->ents[i].fd == fd) {
                ep->ents[i].events = ev->events;
                ep->ents[i].data   = ev->data.u64;
                return 0;
            }
        }
        return -(int)E_FAULT;   /* ENOENT */
    }
    case KO_EPOLL_CTL_DEL: {
        for (int i = 0; i < ep->nents; i++) {
            if (ep->ents[i].used && ep->ents[i].fd == fd) {
                ep->ents[i] = ep->ents[ep->nents - 1];
                ep->ents[ep->nents - 1].used = false;
                ep->nents--;
                return 0;
            }
        }
        return -(int)E_FAULT;
    }
    default:
        return -(int)E_INVAL;
    }
}

int kobj_epoll_wait(int epfd, ko_epoll_event_t *events, int maxevents,
                    int timeout_ms) {
    epoll_obj_t *ep = (epoll_obj_t *)kobj_of(epfd, KOBJ_EPOLL);
    if (!ep) return -(int)E_BADF;
    if (!events || maxevents <= 0) return -(int)E_INVAL;

    uint64_t deadline = (timeout_ms > 0) ? (pit_ticks() + (uint64_t)timeout_ms) : 0;

    for (;;) {
        int nout = 0;
        for (int i = 0; i < ep->nents && nout < maxevents; i++) {
            epoll_ent_t *e = &ep->ents[i];
            if (!e->used) continue;
            int ready = vfs_poll_ready(e->fd, (int)(e->events & 0x7FFFu));
            if (ready) {
                events[nout].events   = (uint32_t)ready;
                events[nout].data.u64 = e->data;
                nout++;
            }
        }
        if (nout > 0) return nout;
        if (timeout_ms == 0) return 0;
        if (timeout_ms > 0 && pit_ticks() >= deadline) return 0;

        /* (BORE): switchable park, other tasks stay runnable
         * while this epoll_wait waits (was: sti/hlt/cli of the only
         * kernel context). */
        if (sched_kernel_park(BORE_EVENT_POLL_MS) != 0)
            __asm__ volatile("sti; hlt; cli");
    }
}

/*
 *  eventfd
 */
#define EFD_MAX 8
typedef struct {
    kobj_hdr_t hdr;              /* kind = KOBJ_EVENTFD               */
    uint64_t   counter;
    bool       semaphore;
} eventfd_obj_t;

static eventfd_obj_t g_eventfds[EFD_MAX];

int kobj_eventfd(unsigned int initval, int flags) {
    eventfd_obj_t *o = NULL;
    for (int i = 0; i < EFD_MAX; i++) {
        if (g_eventfds[i].hdr.fd_refs == 0) { o = &g_eventfds[i]; break; }
    }
    if (!o) return -(int)E_NOMEM;
    o->hdr.kind = KOBJ_EVENTFD;
    o->hdr.fd_refs = 1;
    o->counter = initval;
    o->semaphore = (flags & KO_EFD_SEMAPHORE) != 0;

    extern vfs_node_t *kobj_new_node_public(const char *name, void *obj);
    vfs_node_t *n = kobj_new_node_public("eventfd", o);
    if (!n) return -(int)E_NOMEM;
    int fd = vfs_install_fd(n, flags & KO_EFD_NONBLOCK ? O_NONBLOCK : 0);
    if (fd < 0) o->hdr.fd_refs = 0;
    return fd;
}

/*
 *  timerfd
 */
#define TFD_MAX 8
typedef struct {
    kobj_hdr_t hdr;              /* kind = KOBJ_TIMERFD               */
    int        clockid;
    uint64_t   interval_ms;      /* 0 = one-shot                      */
    uint64_t   deadline_ms;
    uint64_t   expired;          /* pending expiry count              */
    bool       armed;
    bool       nonblock;
} timerfd_obj_t;

static timerfd_obj_t g_timerfds[TFD_MAX];

/* lazy expiry - no timer IRQ walks these, probed on poll/read */
static void timerfd_lazy(timerfd_obj_t *t) {
    if (!t->armed) return;
    uint64_t now = pit_ticks();
    while (t->deadline_ms <= now) {
        t->expired++;
        if (t->interval_ms) {
            t->deadline_ms += t->interval_ms;
            if (t->deadline_ms <= now)
                t->deadline_ms = now + t->interval_ms;   /* catch up */
        } else {
            t->armed = false;
            break;
        }
    }
}

int kobj_timerfd_create(int clockid, int flags) {
    timerfd_obj_t *o = NULL;
    for (int i = 0; i < TFD_MAX; i++) {
        if (g_timerfds[i].hdr.fd_refs == 0) { o = &g_timerfds[i]; break; }
    }
    if (!o) return -(int)E_NOMEM;
    o->hdr.kind = KOBJ_TIMERFD;
    o->hdr.fd_refs = 1;
    o->clockid = clockid;
    o->interval_ms = 0;
    o->deadline_ms = 0;
    o->expired = 0;
    o->armed = false;
    o->nonblock = (flags & KO_TFD_NONBLOCK) != 0;

    extern vfs_node_t *kobj_new_node_public(const char *name, void *obj);
    vfs_node_t *n = kobj_new_node_public("timerfd", o);
    if (!n) return -(int)E_NOMEM;
    int fd = vfs_install_fd(n, flags & KO_TFD_NONBLOCK ? O_NONBLOCK : 0);
    if (fd < 0) o->hdr.fd_refs = 0;
    return fd;
}

int kobj_timerfd_settime(int fd, int flags,
                         const ko_itimerspec_t *newt,
                         ko_itimerspec_t *oldt) {
    timerfd_obj_t *o = (timerfd_obj_t *)kobj_of(fd, KOBJ_TIMERFD);
    if (!o) return -(int)E_BADF;
    if (oldt) {
        timerfd_lazy(o);
        oldt->it_value.tv_sec    = o->armed ? (int64_t)((o->deadline_ms - pit_ticks()) / 1000) : 0;
        oldt->it_value.tv_nsec   = o->armed ? (int64_t)(((o->deadline_ms - pit_ticks()) % 1000) * 1000000) : 0;
        oldt->it_interval.tv_sec  = (int64_t)(o->interval_ms / 1000);
        oldt->it_interval.tv_nsec = (int64_t)((o->interval_ms % 1000) * 1000000);
    }
    if (!newt) return -(int)E_FAULT;

    uint64_t ms = (uint64_t)newt->it_value.tv_sec * 1000
                + (uint64_t)newt->it_value.tv_nsec / 1000000;
    o->interval_ms = (uint64_t)newt->it_interval.tv_sec * 1000
                   + (uint64_t)newt->it_interval.tv_nsec / 1000000;
    o->expired = 0;
    if (ms == 0) {
        o->armed = false;
    } else if (flags & KO_TFD_TIMER_ABSTIME) {
        o->deadline_ms = ms;
        o->armed = true;
    } else {
        o->deadline_ms = pit_ticks() + ms;
        o->armed = true;
    }
    return 0;
}

int kobj_timerfd_gettime(int fd, ko_itimerspec_t *curr) {
    timerfd_obj_t *o = (timerfd_obj_t *)kobj_of(fd, KOBJ_TIMERFD);
    if (!o) return -(int)E_BADF;
    if (!curr) return -(int)E_FAULT;
    timerfd_lazy(o);
    if (o->armed && o->deadline_ms > pit_ticks()) {
        uint64_t rem = o->deadline_ms - pit_ticks();
        curr->it_value.tv_sec  = (int64_t)(rem / 1000);
        curr->it_value.tv_nsec = (int64_t)((rem % 1000) * 1000000);
    } else {
        curr->it_value.tv_sec = 0;
        curr->it_value.tv_nsec = 0;
    }
    curr->it_interval.tv_sec  = (int64_t)(o->interval_ms / 1000);
    curr->it_interval.tv_nsec = (int64_t)((o->interval_ms % 1000) * 1000000);
    return 0;
}

/*
 *  signalfd
 */
#define SFD_MAX 4
typedef struct {
    kobj_hdr_t hdr;              /* kind = KOBJ_SIGNALFD              */
    uint64_t   mask;             /* 64-bit signal mask                */
} signalfd_obj_t;

static signalfd_obj_t g_signalfds[SFD_MAX];

int kobj_signalfd4(int fd, const uint64_t *mask, size_t sigsetsize, int flags) {
    if (sigsetsize < 8) return -(int)E_INVAL;
    signalfd_obj_t *o;
    if (fd == -1) {
        o = NULL;
        for (int i = 0; i < SFD_MAX; i++) {
            if (g_signalfds[i].hdr.fd_refs == 0) { o = &g_signalfds[i]; break; }
        }
        if (!o) return -(int)E_NOMEM;
        o->hdr.kind = KOBJ_SIGNALFD;
        o->hdr.fd_refs = 1;
        o->mask = 0;
        extern vfs_node_t *kobj_new_node_public(const char *name, void *obj);
        vfs_node_t *n = kobj_new_node_public("signalfd", o);
        if (!n) return -(int)E_NOMEM;
        int nfd = vfs_install_fd(n, flags & KO_SFD_NONBLOCK ? O_NONBLOCK : 0);
        if (nfd < 0) { o->hdr.fd_refs = 0; return nfd; }
        if (mask) o->mask = (((uint64_t)*mask) << 1) & 0xFFFFFFFFFFFFFFFEULL;
        return nfd;
    }
    /* update mask on existing signalfd */
    o = (signalfd_obj_t *)kobj_of(fd, KOBJ_SIGNALFD);
    if (!o) return -(int)E_BADF;
    if (mask) o->mask = (((uint64_t)*mask) << 1) & 0xFFFFFFFFFFFFFFFEULL;
    return fd;
}

/*
 *  memfd - page-list backed in-memory file (wl_shm pools)
 */
#define MEMFD_MAX_PAGES 1024    /* 4 MiB per object  */
#define MEMFD_MAX 16

typedef struct {
    kobj_hdr_t hdr;              /* kind = KOBJ_MEMFD                 */
    uint64_t   pages[MEMFD_MAX_PAGES];
    size_t     npages;
    uint64_t   size;
    int        map_refs;
    char       name[64];
} memfd_obj_t;

static memfd_obj_t g_memfds[MEMFD_MAX];

static bool memfd_ensure_pages(memfd_obj_t *m, size_t npages) {
    if (npages > MEMFD_MAX_PAGES) return false;
    while (m->npages < npages) {
        uint64_t phys = pmm_alloc();
        if (!phys) return false;
        uint8_t *p = (uint8_t *)(uintptr_t)phys;
        for (size_t b = 0; b < PAGE_SIZE; b++) p[b] = 0;   /* zero-fill */
        m->pages[m->npages++] = phys;
    }
    return true;
}

int kobj_memfd_create(const char *name, int flags) {
    (void)flags;
    memfd_obj_t *o = NULL;
    for (int i = 0; i < MEMFD_MAX; i++) {
        if (g_memfds[i].hdr.fd_refs == 0 && g_memfds[i].npages == 0) {
            o = &g_memfds[i];
            break;
        }
    }
    if (!o) return -(int)E_NOMEM;
    o->hdr.kind = KOBJ_MEMFD;
    o->hdr.fd_refs = 1;
    o->npages = 0;
    o->size = 0;
    o->map_refs = 0;
    size_t i = 0;
    if (name) for (; name[i] && i < sizeof(o->name) - 1; i++) o->name[i] = name[i];
    o->name[i] = '\0';

    extern vfs_node_t *kobj_new_node_public(const char *name, void *obj);
    vfs_node_t *n = kobj_new_node_public(o->name, o);
    if (!n) return -(int)E_NOMEM;
    int fd = vfs_install_fd(n, 0);
    if (fd < 0) o->hdr.fd_refs = 0;
    return fd;
}

/* /dev/shm namespace backing
 * kobj_shm_new_file(name) builds the same memfd-backed node but returns
 * it instead of installing an fd, the /dev/shm directory attaches it and
 * holds the initial fd_refs (the "directory entry" reference).  Every
 * open() bumps fd_refs (kobj_open_ops), close() drops one, and the pages
 * stay alive until the entry is unlinked and the last fd/map is gone -
 * POSIX shm semantics. */
static memfd_obj_t *memfd_from_node(vfs_node_t *n);   /* defined below */

vfs_node_t *kobj_shm_new_file(const char *name) {
    memfd_obj_t *o = NULL;
    for (int i = 0; i < MEMFD_MAX; i++) {
        if (g_memfds[i].hdr.fd_refs == 0 && g_memfds[i].npages == 0) {
            o = &g_memfds[i];
            break;
        }
    }
    if (!o) return NULL;
    o->hdr.kind = KOBJ_MEMFD;
    o->hdr.fd_refs = 1;              /* held by the /dev/shm entry */
    o->npages = 0;
    o->size = 0;
    o->map_refs = 0;
    size_t i = 0;
    if (name) for (; name[i] && i < sizeof(o->name) - 1; i++) o->name[i] = name[i];
    o->name[i] = '\0';

    extern vfs_node_t *kobj_new_node_public(const char *name, void *obj);
    vfs_node_t *n = kobj_new_node_public(o->name, o);
    if (!n) { o->hdr.fd_refs = 0; return NULL; }
    return n;
}

/* Drop the directory-entry reference after unlink (or failed attach). */
void kobj_shm_drop_dir_ref(vfs_node_t *n) {
    memfd_obj_t *m = memfd_from_node(n);
    if (!m) return;
    if (m->hdr.fd_refs > 0) m->hdr.fd_refs--;
    if (m->hdr.fd_refs == 0 && m->map_refs == 0) {
        for (size_t p = 0; p < m->npages; p++)
            pmm_free(m->pages[p]);
        m->npages = 0;
        m->size = 0;
    }
}

static memfd_obj_t *memfd_from_node(vfs_node_t *n) {
    if (!n || n->type != VFS_TYPE_KOBJ || !n->data) return NULL;
    kobj_hdr_t *h = (kobj_hdr_t *)n->data;
    if (h->kind != KOBJ_MEMFD) return NULL;
    return (memfd_obj_t *)n->data;
}

int64_t kobj_ftruncate(int fd, int64_t length) {
    if (length < 0) return -(int)E_INVAL;
    vfs_node_t *n = vfs_node_from_fd(fd);
    if (!n) return -(int)E_BADF;
    memfd_obj_t *m = memfd_from_node(n);
    if (!m) return -(int)E_NOSYS;   /* only memfd supports resize for now */

    size_t npages = ((uint64_t)length + PAGE_SIZE - 1) / PAGE_SIZE;
    if ((uint64_t)length > m->size) {
        if (!memfd_ensure_pages(m, npages)) return -(int)E_NOMEM;
    } else if ((uint64_t)length < m->size) {
        while (m->npages > npages) {
            m->npages--;
            pmm_free(m->pages[m->npages]);
        }
    }
    m->size = (uint64_t)length;
    n->size = (uint32_t)length;
    return 0;
}

int kobj_memfd_mmap_shared(vfs_node_t *node, uint64_t base, size_t npages,
                           uint64_t off) {
    memfd_obj_t *m = memfd_from_node(node);
    if (!m) return -(int)E_NOSYS;
    size_t first = (size_t)(off / PAGE_SIZE);
    if (!memfd_ensure_pages(m, first + npages)) return -(int)E_NOMEM;
    if (m->size < off + npages * PAGE_SIZE) {
        m->size = off + npages * PAGE_SIZE;
        node->size = (uint32_t)m->size;
    }
    for (size_t i = 0; i < npages; i++) {
        if (!vmm_map(base + i * PAGE_SIZE, m->pages[first + i],
                     PTE_PRESENT | PTE_WRITABLE | PTE_USER))
            return -(int)E_NOMEM;
    }
    m->map_refs++;
    return 0;
}

bool kobj_memfd_owns_phys(uint64_t phys) {
    for (int i = 0; i < MEMFD_MAX; i++) {
        memfd_obj_t *m = &g_memfds[i];
        if (m->hdr.fd_refs == 0 && m->npages == 0) continue;
        for (size_t p = 0; p < m->npages; p++)
            if (m->pages[p] == phys) return true;
    }
    return false;
}

/*
 *  generic dispatch (called from kobj.c)
 */
int kobj_evt_read(vfs_node_t *n, void *buf, size_t len, size_t off) {
    (void)off;
    kobj_hdr_t *h = (kobj_hdr_t *)n->data;
    switch (h->kind) {
    case KOBJ_EVENTFD: {
        eventfd_obj_t *o = (eventfd_obj_t *)n->data;
        if (len < 8) return -(int)E_INVAL;
        if (o->counter == 0) return -(int)E_AGAIN;
        uint64_t *out = (uint64_t *)buf;
        if (o->semaphore) {
            *out = 1;
            o->counter--;
        } else {
            *out = o->counter;
            o->counter = 0;
        }
        return 8;
    }
    case KOBJ_TIMERFD: {
        timerfd_obj_t *o = (timerfd_obj_t *)n->data;
        if (len < 8) return -(int)E_INVAL;
        timerfd_lazy(o);
        /* Linux semantics: read() on an armed timerfd blocks until the
         * next expiry (EAGAIN only with O_NONBLOCK).  (BORE):
         * park switchably between probes instead of halting the CPU. */
        while (o->expired == 0) {
            if (!o->armed || o->nonblock) return -(int)E_AGAIN;
            if (sched_kernel_park(BORE_EVENT_POLL_MS) != 0)
                __asm__ volatile("sti; hlt; cli");
            timerfd_lazy(o);
        }
        *(uint64_t *)buf = o->expired;
        o->expired = 0;
        return 8;
    }
    case KOBJ_SIGNALFD: {
        signalfd_obj_t *o = (signalfd_obj_t *)n->data;
        if (!g_current_proc) return -(int)E_BADF;
        if (len < sizeof(ko_signalfd_siginfo_t)) return -(int)E_INVAL;
        uint64_t pend = g_current_proc->pending_sigs & o->mask;
        if (!pend) return -(int)E_AGAIN;
        for (int s = 1; s < NSIG; s++) {
            if (pend & (1ull << s)) {
                g_current_proc->pending_sigs &= ~(1ull << s);
                ko_signalfd_siginfo_t *si = (ko_signalfd_siginfo_t *)buf;
                for (size_t z = 0; z < sizeof(*si); z++) ((uint8_t *)si)[z] = 0;
                si->ssi_signo = (uint32_t)s;
                si->ssi_pid   = (uint32_t)g_current_proc->pid;
                si->ssi_uid   = 0;
                return (int)sizeof(ko_signalfd_siginfo_t);
            }
        }
        return -(int)E_AGAIN;
    }
    case KOBJ_MEMFD: {
        memfd_obj_t *m = (memfd_obj_t *)n->data;
        if (off >= m->size) return 0;
        size_t can = (size_t)(m->size - off);
        if (len > can) len = can;
        size_t done = 0;
        while (done < len) {
            size_t pidx = (off + done) / PAGE_SIZE;
            size_t poff = (off + done) % PAGE_SIZE;
            size_t chunk = PAGE_SIZE - poff;
            if (chunk > len - done) chunk = len - done;
            const uint8_t *src = (const uint8_t *)(uintptr_t)m->pages[pidx] + poff;
            uint8_t *dst = (uint8_t *)buf + done;
            for (size_t k = 0; k < chunk; k++) dst[k] = src[k];
            done += chunk;
        }
        return (int)done;
    }
    default:
        return -(int)E_NOSYS;
    }
}

int kobj_evt_write(vfs_node_t *n, const void *buf, size_t len, size_t off) {
    (void)off;
    kobj_hdr_t *h = (kobj_hdr_t *)n->data;
    switch (h->kind) {
    case KOBJ_EVENTFD: {
        eventfd_obj_t *o = (eventfd_obj_t *)n->data;
        if (len < 8) return -(int)E_INVAL;
        uint64_t add = *(const uint64_t *)buf;
        if (add == 0xFFFFFFFFFFFFFFFFULL) return -(int)E_INVAL;
        if (o->counter + add < o->counter) return -(int)E_AGAIN;
        o->counter += add;
        return 8;
    }
    case KOBJ_MEMFD: {
        memfd_obj_t *m = (memfd_obj_t *)n->data;
        size_t need = off + len;
        if (need > m->size) {
            if (!memfd_ensure_pages(m, (need + PAGE_SIZE - 1) / PAGE_SIZE))
                return -(int)E_NOMEM;
            m->size = need;
            n->size = (uint32_t)need;
        }
        size_t done = 0;
        while (done < len) {
            size_t pidx = (off + done) / PAGE_SIZE;
            size_t poff = (off + done) % PAGE_SIZE;
            size_t chunk = PAGE_SIZE - poff;
            if (chunk > len - done) chunk = len - done;
            uint8_t *dst = (uint8_t *)(uintptr_t)m->pages[pidx] + poff;
            const uint8_t *src = (const uint8_t *)buf + done;
            for (size_t k = 0; k < chunk; k++) dst[k] = src[k];
            done += chunk;
        }
        return (int)done;
    }
    default:
        return -(int)E_NOSYS;
    }
}

int kobj_evt_close(vfs_node_t *n) {
    kobj_hdr_t *h = (kobj_hdr_t *)n->data;
    switch (h->kind) {
    case KOBJ_MEMFD: {
        memfd_obj_t *m = (memfd_obj_t *)n->data;
        if (m->hdr.fd_refs > 0) m->hdr.fd_refs--;
        /* free the backing pages only when the last fd is gone and no
         * MAP_SHARED mapping keeps them alive                       */
        if (m->hdr.fd_refs == 0 && m->map_refs == 0) {
            for (size_t p = 0; p < m->npages; p++)
                pmm_free(m->pages[p]);
            m->npages = 0;
            m->size = 0;
        }
        break;
    }
    default: {
        if (h->fd_refs > 0) h->fd_refs--;
        break;
    }
    }
    return 0;
}

int kobj_evt_poll(vfs_node_t *n, int events) {
    kobj_hdr_t *h = (kobj_hdr_t *)n->data;
    int ev = 0;
    switch (h->kind) {
    case KOBJ_EVENTFD: {
        eventfd_obj_t *o = (eventfd_obj_t *)n->data;
        if ((events & KO_EPOLLIN) && o->counter > 0) ev |= KO_EPOLLIN;
        if (events & KO_EPOLLOUT) ev |= KO_EPOLLOUT;
        break;
    }
    case KOBJ_TIMERFD: {
        timerfd_obj_t *o = (timerfd_obj_t *)n->data;
        timerfd_lazy(o);
        if ((events & KO_EPOLLIN) && o->expired > 0) ev |= KO_EPOLLIN;
        break;
    }
    case KOBJ_SIGNALFD: {
        signalfd_obj_t *o = (signalfd_obj_t *)n->data;
        if ((events & KO_EPOLLIN) && g_current_proc &&
            (g_current_proc->pending_sigs & o->mask))
            ev |= KO_EPOLLIN;
        break;
    }
    case KOBJ_EPOLL:
        if (events & KO_EPOLLIN)  ev |= KO_EPOLLIN;
        if (events & KO_EPOLLOUT) ev |= KO_EPOLLOUT;
        break;
    case KOBJ_MEMFD:
        if (events & KO_EPOLLIN)  ev |= KO_EPOLLIN;
        if (events & KO_EPOLLOUT) ev |= KO_EPOLLOUT;
        break;
    default:
        break;
    }
    return ev;
}

/* node constructor shim (kobj.c's kobj_new_node_impl via kobj.h) */
vfs_node_t *kobj_new_node_public(const char *name, void *obj) {
    return kobj_new_node_impl(name, obj);
}
