#include "fs/kobj.h"
#include "exec/syscall_table.h"
#include "exec/process.h"
#include "drivers/memory/vmm.h"
#include "drivers/timer/pit.h"
#include "console/print.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/*
 * kobj.c - kernel-object fds: AF_UNIX sockets (this file) plus the
 * event/timer/shmem objects (kobj_evt.c).  See kobj.h for the design.
 */

extern uint64_t pit_ticks(void);

/* dispatchers implemented in kobj_evt.c */
extern int kobj_evt_read(vfs_node_t *n, void *buf, size_t len, size_t off);
extern int kobj_evt_write(vfs_node_t *n, const void *buf, size_t len, size_t off);
extern int kobj_evt_close(vfs_node_t *n);
extern int kobj_evt_poll(vfs_node_t *n, int events);

/* errno (matches exec/syscall_table.h) */
#define E_PERM      1
#define E_NOENT     2
#define E_BADF      9
#define E_AGAIN     11
#define E_NOMEM     12
#define E_FAULT     14
#define E_NOTCONN   107
#define E_CONNREFUSED 111
#define E_INVAL     22
#define E_PIPE      32
#define E_NOSYS     38
#define E_NOTSOCK   88
#define E_ISCONN    106
#define E_CONNABORTED 103
#define E_MSGSIZE   90

#ifndef E_NOTCONN_DEF
#endif

/* Object header (kobj_hdr_t lives in kobj.h) */
int kobj_node_kind(vfs_node_t *node) {
    if (!node || node->type != VFS_TYPE_KOBJ || !node->data) return 0;
    return ((kobj_hdr_t *)node->data)->kind;
}

/* VFS ops shim */
static int kobj_read_ops(vfs_node_t *n, void *buf, size_t len, size_t off);
static int kobj_write_ops(vfs_node_t *n, const void *buf, size_t len, size_t off);
static int kobj_close_ops(vfs_node_t *n);

/* vfs_open() on a named kernel object (i.e. a /dev/shm file) takes
 * a reference that close() releases.  Objects installed directly into the
 * fd table (sockets, epoll, memfd_create ...) never go through vfs_open, so
 * their accounting is unchanged. */
static int kobj_open_ops(vfs_node_t *n, int flags) {
    (void)flags;
    if (n && n->data && ((kobj_hdr_t *)n->data)->kind == KOBJ_MEMFD)
        ((kobj_hdr_t *)n->data)->fd_refs++;
    return 0;
}

static const vfs_ops_t kobj_ops = {
    .open  = kobj_open_ops,
    .read  = kobj_read_ops,
    .write = kobj_write_ops,
    .close = kobj_close_ops,
};

vfs_node_t *kobj_new_node_impl(const char *name, void *obj) {
    vfs_node_t *n = vfs_create_node(name, VFS_TYPE_KOBJ, &kobj_ops, obj);
    if (n) n->pinned = 0;
    return n;
}

static int kobj_install(const char *name, void *obj, int flags) {
    vfs_node_t *n = kobj_new_node_impl(name, obj);
    if (!n) return -(int)E_NOMEM;
    int fd = vfs_install_fd(n, flags);
    if (fd < 0) vfs_free_node(n);
    return fd;
}

/* find the kobj object behind an fd, NULL if not a kobj fd.
 * Non-static: kobj_evt.c (epoll/timerfd/...) shares this lookup. */
void *kobj_of(int fd, int kind) {
    if (fd < 0 || fd >= VFS_MAX_FDS) return NULL;
    vfs_node_t *n = vfs_node_from_fd(fd);
    if (!n || n->type != VFS_TYPE_KOBJ || !n->data) return NULL;
    kobj_hdr_t *h = (kobj_hdr_t *)n->data;
    if (kind && h->kind != kind) return NULL;
    return n->data;
}



/*
 *  AF_UNIX stream sockets
 */
#define UNIX_PATH_MAX 108
#define SOCK_BUF_SIZE 8192
#define SOCK_MAX_PENDING  4
#define SOCK_MAX_FDSQ     8

typedef struct unix_ep {
    kobj_hdr_t hdr;                 /* kind = KOBJ_UNIX_SOCK            */

    /* receive ring - the PEER writes here, we read from here          */
    char     buf[SOCK_BUF_SIZE];
    size_t   rpos, wpos, len;
    bool     rd_shutdown;           /* peer did shutdown(SHUT_WR)       */

    /* pairing                                                          */
    struct unix_ep *peer;           /* NULL = not connected             */
    bool     listening;
    struct unix_ep *pending[SOCK_MAX_PENDING];
    int      npending;

    /* identity                                                         */
    char     path[UNIX_PATH_MAX];
    bool     bound;
    int      type;                  /* KO_SOCK_STREAM                   */
    bool     nonblock;
    bool     passcred;
    int      creator_pid;

    /* SCM_RIGHTS queue: fd-table snapshots handed over by the peer     */
    struct {
        bool       used;
        vfs_node_t *node;
        size_t      offset;
        int         flags;
    } fdq[SOCK_MAX_FDSQ];
    int nfdq;
} unix_ep_t;

#define UNIX_MAX_EPS 24
static unix_ep_t g_unix_eps[UNIX_MAX_EPS];

/* bind table: path -> listening endpoint */
#define UNIX_MAX_BOUND 8
static struct {
    char       path[UNIX_PATH_MAX];
    unix_ep_t *listener;
} g_unix_bound[UNIX_MAX_BOUND];

/* tiny local strncmp to avoid string.h */
static bool strncmp_local(const char *a, const char *b) {
    while (*a && *b) { if (*a != *b) return false; a++; b++; }
    return *a == *b;
}

static unix_ep_t *ep_alloc(void) {
    for (int i = 0; i < UNIX_MAX_EPS; i++) {
        unix_ep_t *e = &g_unix_eps[i];
        if (e->hdr.fd_refs == 0 && !e->listening && !e->bound && !e->peer) {
            /* full reset */
            e->rpos = e->wpos = e->len = 0;
            e->rd_shutdown = false;
            e->peer = NULL;
            e->listening = false;
            e->npending = 0;
            e->bound = false;
            e->passcred = false;
            e->nonblock = false;
            e->nfdq = 0;
            for (int q = 0; q < SOCK_MAX_FDSQ; q++) e->fdq[q].used = false;
            e->path[0] = '\0';
            e->creator_pid = g_current_proc ? g_current_proc->pid : 1;
            e->hdr.kind = KOBJ_UNIX_SOCK;
            e->hdr.fd_refs = 0;
            return e;
        }
    }
    return NULL;
}

static void ep_release(unix_ep_t *e) {
    if (!e) return;
    if (e->hdr.fd_refs > 0) e->hdr.fd_refs--;
    if (e->hdr.fd_refs == 0) {
        /* detach from peer */
        if (e->peer) {
            e->peer->peer = NULL;
            e->peer->rd_shutdown = true;   /* peer sees EOF */
            e->peer = NULL;
        }
        e->listening = false;
        e->npending = 0;
        e->bound = false;
        if (e->path[0]) {
            for (int i = 0; i < UNIX_MAX_BOUND; i++) {
                if (g_unix_bound[i].listener == e &&
                    !strncmp_local(g_unix_bound[i].path, e->path)) {
                    g_unix_bound[i].listener = NULL;
                    g_unix_bound[i].path[0] = '\0';
                }
            }
            e->path[0] = '\0';
        }
        e->len = 0;
    }
}

/* ring helpers */
static size_t ring_write(unix_ep_t *e, const char *data, size_t n) {
    size_t put = 0;
    while (put < n && e->len < SOCK_BUF_SIZE) {
        e->buf[e->wpos] = data[put++];
        e->wpos = (e->wpos + 1) % SOCK_BUF_SIZE;
        e->len++;
    }
    return put;
}
static size_t ring_read(unix_ep_t *e, char *out, size_t n) {
    size_t got = 0;
    while (got < n && e->len > 0) {
        out[got++] = e->buf[e->rpos];
        e->rpos = (e->rpos + 1) % SOCK_BUF_SIZE;
        e->len--;
    }
    return got;
}

/* socket syscalls */
int kobj_socketpair(int domain, int type, int protocol, int fds[2]) {
    (void)protocol;
    if (domain != KO_AF_UNIX) return -(int)E_NOSYS;
    if ((type & 0xF) != KO_SOCK_STREAM && (type & 0xF) != KO_SOCK_DGRAM)
        return -(int)E_NOSYS;
    if (!fds) return -(int)E_FAULT;

    unix_ep_t *a = ep_alloc();
    unix_ep_t *b = ep_alloc();
    if (!a || !b) return -(int)E_NOMEM;
    a->peer = b; b->peer = a;
    a->hdr.fd_refs = 1; b->hdr.fd_refs = 1;
    a->type = b->type = (type & 0xF);

    int fa = kobj_install("sock", a, 0);
    int fb = kobj_install("sock", b, 0);
    if (fa < 0 || fb < 0) { ep_release(a); ep_release(b); return -(int)E_NOMEM; }
    fds[0] = fa; fds[1] = fb;
    return 0;
}

int kobj_socket(int domain, int type, int protocol) {
    (void)protocol;
    if (domain != KO_AF_UNIX) return -(int)E_NOSYS;
    unix_ep_t *e = ep_alloc();
    if (!e) return -(int)E_NOMEM;
    e->hdr.fd_refs = 1;
    e->type = (type & 0xF);
    if (type & KO_SOCK_NONBLOCK) e->nonblock = true;
    int fd = kobj_install("socket", e, 0);
    if (fd < 0) { e->hdr.fd_refs = 0; ep_release(e); }
    return fd;
}

struct ko_sockaddr_un {
    uint16_t sun_family;
    char     sun_path[UNIX_PATH_MAX];
};

int kobj_bind(int fd, const void *addr, uint32_t addrlen) {
    (void)addrlen;
    unix_ep_t *e = (unix_ep_t *)kobj_of(fd, KOBJ_UNIX_SOCK);
    if (!e) return -(int)E_NOTSOCK;
    const struct ko_sockaddr_un *sa = (const struct ko_sockaddr_un *)addr;
    if (!sa || sa->sun_family != KO_AF_UNIX) return -(int)E_INVAL;

    for (int i = 0; i < UNIX_MAX_BOUND; i++) {
        if (g_unix_bound[i].listener &&
            strncmp_local(g_unix_bound[i].path, sa->sun_path))
            return -(int)E_INVAL;   /* EADDRINUSE is 98; EINVAL fine here */
    }
    int slot = -1;
    for (int i = 0; i < UNIX_MAX_BOUND; i++)
        if (!g_unix_bound[i].listener) { slot = i; break; }
    if (slot < 0) return -(int)E_NOMEM;

    for (int i = 0; i < UNIX_PATH_MAX && sa->sun_path[i]; i++)
        e->path[i] = sa->sun_path[i];
    e->path[UNIX_PATH_MAX - 1] = '\0';
    e->bound = true;
    e->listening = true;            /* bind implies listener readiness   */
    g_unix_bound[slot].listener = e;
    for (int i = 0; i < UNIX_PATH_MAX; i++)
        g_unix_bound[slot].path[i] = e->path[i];
    return 0;
}

int kobj_listen(int fd, int backlog) {
    (void)backlog;
    unix_ep_t *e = (unix_ep_t *)kobj_of(fd, KOBJ_UNIX_SOCK);
    if (!e) return -(int)E_NOTSOCK;
    e->listening = true;
    return 0;
}

int kobj_connect(int fd, const void *addr, uint32_t addrlen) {
    (void)addrlen;
    unix_ep_t *c = (unix_ep_t *)kobj_of(fd, KOBJ_UNIX_SOCK);
    if (!c) return -(int)E_NOTSOCK;
    const struct ko_sockaddr_un *sa = (const struct ko_sockaddr_un *)addr;
    if (!sa || sa->sun_family != KO_AF_UNIX) return -(int)E_INVAL;

    unix_ep_t *lst = NULL;
    for (int i = 0; i < UNIX_MAX_BOUND; i++) {
        if (g_unix_bound[i].listener &&
            strncmp_local(g_unix_bound[i].path, sa->sun_path)) {
            lst = g_unix_bound[i].listener;
            break;
        }
    }
    if (!lst || !lst->listening) return -(int)E_CONNREFUSED;
    if (lst->npending >= SOCK_MAX_PENDING) return -(int)E_AGAIN;

    c->peer = NULL;                 /* completed at accept time          */
    lst->pending[lst->npending++] = c;
    return 0;
}

int kobj_accept4(int fd, void *addr, uint32_t *addrlen, int flags) {
    unix_ep_t *lst = (unix_ep_t *)kobj_of(fd, KOBJ_UNIX_SOCK);
    if (!lst) return -(int)E_NOTSOCK;
    if (!lst->listening || lst->npending == 0) {
        if (lst->npending == 0) {
            if (flags & KO_SOCK_NONBLOCK) return -(int)E_AGAIN;
            /* cooperative: nothing can fill the queue while we block */
            return -(int)E_AGAIN;
        }
    }
    unix_ep_t *client = lst->pending[0];
    for (int i = 1; i < lst->npending; i++)
        lst->pending[i-1] = lst->pending[i];
    lst->npending--;

    unix_ep_t *srv = ep_alloc();
    if (!srv) return -(int)E_NOMEM;
    srv->hdr.fd_refs = 1;
    srv->peer = client;
    client->peer = srv;

    if (addr && addrlen && *addrlen >= sizeof(struct ko_sockaddr_un)) {
        struct ko_sockaddr_un sa;
        sa.sun_family = KO_AF_UNIX;
        for (int i = 0; i < UNIX_PATH_MAX; i++) sa.sun_path[i] = 0;
        /* client address is its own bound path or anonymous */
        *(struct ko_sockaddr_un *)addr = sa;
        *addrlen = sizeof(sa);
    }
    (void)flags;
    return kobj_install("accepted", srv, 0);
}

int kobj_shutdown(int fd, int how) {
    unix_ep_t *e = (unix_ep_t *)kobj_of(fd, KOBJ_UNIX_SOCK);
    if (!e) return -(int)E_NOTSOCK;
    if (how == 0) {                  /* SHUT_RD: our recv side ends       */
        e->len = 0; e->rpos = e->wpos = 0;
    } else if (how == 1 || how == 2) { /* SHUT_WR/SHUT_RDWR: peer sees EOF */
        if (e->peer) e->peer->rd_shutdown = true;
        e->peer = NULL;
    }
    return 0;
}

int kobj_getsockname(int fd, void *addr, uint32_t *addrlen) {
    unix_ep_t *e = (unix_ep_t *)kobj_of(fd, KOBJ_UNIX_SOCK);
    if (!e) return -(int)E_NOTSOCK;
    if (!addr || !addrlen) return -(int)E_FAULT;
    struct ko_sockaddr_un sa;
    sa.sun_family = KO_AF_UNIX;
    for (int i = 0; i < UNIX_PATH_MAX; i++) sa.sun_path[i] = e->path[i];
    uint32_t n = *addrlen;
    if (n > sizeof(sa)) n = sizeof(sa);
    char *dst = (char *)addr;
    const char *src = (const char *)&sa;
    for (uint32_t i = 0; i < n; i++) dst[i] = src[i];
    *addrlen = sizeof(sa);
    return 0;
}

int kobj_getpeername(int fd, void *addr, uint32_t *addrlen) {
    unix_ep_t *e = (unix_ep_t *)kobj_of(fd, KOBJ_UNIX_SOCK);
    if (!e) return -(int)E_NOTSOCK;
    if (!e->peer) return -(int)E_NOTCONN;
    if (!addr || !addrlen) return -(int)E_FAULT;
    struct ko_sockaddr_un sa;
    sa.sun_family = KO_AF_UNIX;
    for (int i = 0; i < UNIX_PATH_MAX; i++) sa.sun_path[i] = e->peer->path[i];
    uint32_t n = *addrlen;
    if (n > sizeof(sa)) n = sizeof(sa);
    char *dst = (char *)addr;
    const char *src = (const char *)&sa;
    for (uint32_t i = 0; i < n; i++) dst[i] = src[i];
    *addrlen = sizeof(sa);
    return 0;
}

int kobj_setsockopt(int fd, int level, int optname,
                    const void *optval, uint32_t optlen) {
    unix_ep_t *e = (unix_ep_t *)kobj_of(fd, KOBJ_UNIX_SOCK);
    if (!e) return -(int)E_NOTSOCK;
    if (level != KO_SOL_SOCKET) return -(int)E_NOSYS;
    switch (optname) {
    case KO_SO_PASSCRED: {
        if (!optval || optlen < 4) return -(int)E_INVAL;
        e->passcred = *(const int *)optval != 0;
        return 0;
    }
    case KO_SO_SNDBUF:
    case KO_SO_RCVBUF:
        return 0;                    /* accepted, fixed-size ring        */
    default:
        return 0;                    /* tolerate unknown options         */
    }
}

int kobj_getsockopt(int fd, int level, int optname,
                    void *optval, uint32_t *optlen) {
    unix_ep_t *e = (unix_ep_t *)kobj_of(fd, KOBJ_UNIX_SOCK);
    if (!e) return -(int)E_NOTSOCK;
    if (level != KO_SOL_SOCKET) return -(int)E_NOSYS;
    if (!optval || !optlen) return -(int)E_FAULT;
    switch (optname) {
    case KO_SO_TYPE: {
        if (*optlen < 4) return -(int)E_INVAL;
        *(int *)optval = e->type;
        *optlen = 4;
        return 0;
    }
    case KO_SO_PEERCRED: {
        if (*optlen < sizeof(ko_ucred_t)) return -(int)E_INVAL;
        ko_ucred_t *u = (ko_ucred_t *)optval;
        u->pid = e->peer ? e->peer->creator_pid : e->creator_pid;
        u->uid = 0;
        u->gid = 0;
        *optlen = sizeof(ko_ucred_t);
        return 0;
    }
    case KO_SO_PASSCRED: {
        if (*optlen < 4) return -(int)E_INVAL;
        *(int *)optval = e->passcred ? 1 : 0;
        *optlen = 4;
        return 0;
    }
    case KO_SO_SNDBUF:
    case KO_SO_RCVBUF: {
        if (*optlen < 4) return -(int)E_INVAL;
        *(int *)optval = SOCK_BUF_SIZE;
        *optlen = 4;
        return 0;
    }
    default:
        return -(int)E_NOSYS;
    }
}

/* sendmsg / recvmsg with SCM_RIGHTS */
int64_t kobj_sendmsg(int fd, const ko_msghdr_t *mh, int flags) {
    unix_ep_t *e = (unix_ep_t *)kobj_of(fd, KOBJ_UNIX_SOCK);
    if (!e) return -(int)E_NOTSOCK;
    if (!mh) return -(int)E_FAULT;

    /* ancillary first: SCM_RIGHTS enqueues fd snapshots for the peer */
    if (mh->msg_control && mh->msg_controllen) {
        const uint8_t *c = (const uint8_t *)mh->msg_control;
        const uint8_t *end = c + mh->msg_controllen;
        while (c + sizeof(ko_cmsghdr_t) <= end) {
            const ko_cmsghdr_t *ch = (const ko_cmsghdr_t *)c;
            if (ch->cmsg_len < sizeof(ko_cmsghdr_t)) break;
            if (ch->cmsg_level == KO_SOL_SOCKET &&
                ch->cmsg_type == KO_SCM_RIGHTS) {
                const int *fda = (const int *)(c + sizeof(ko_cmsghdr_t));
                size_t nfds = (ch->cmsg_len - sizeof(ko_cmsghdr_t)) / 4;
                for (size_t i = 0; i < nfds; i++) {
                    if (!e->peer || e->peer->nfdq >= SOCK_MAX_FDSQ)
                        return -(int)E_PIPE;
                    unix_ep_t *dst = e->peer;
                    vfs_fd_t snap;
                    if (vfs_fd_snapshot(fda[i], &snap) < 0)
                        return -(int)E_BADF;
                    dst->fdq[dst->nfdq].used   = true;
                    dst->fdq[dst->nfdq].node   = snap.node;
                    dst->fdq[dst->nfdq].offset = snap.offset;
                    dst->fdq[dst->nfdq].flags  = snap.flags;
                    dst->nfdq++;
                }
            }
            c += KO_CMSG_ALIGN(ch->cmsg_len);
        }
    }

    if (!e->peer) return -(int)E_NOTCONN;

    /* payload through the iovecs */
    typedef struct { uint64_t base; uint64_t len; } kiovec_t;
    const kiovec_t *iov = (const kiovec_t *)mh->msg_iov;
    int64_t total = 0;
    for (size_t i = 0; i < mh->msg_iovlen; i++) {
        const char *src = (const char *)(uintptr_t)iov[i].base;
        size_t left = iov[i].len;
        while (left) {
            if (e->peer->len >= SOCK_BUF_SIZE) {
                if (e->nonblock || (flags & KO_MSG_DONTWAIT)) {
                    return total ? total : -(int)E_AGAIN;
                }
                return total ? total : -(int)E_AGAIN;  /* no blocking send */
            }
            size_t put = ring_write(e->peer, src, left);
            if (!put) break;
            src += put; left -= put; total += (int64_t)put;
        }
    }
    if (total == 0 && mh->msg_iovlen == 0) return 0;
    return total;
}

int64_t kobj_recvmsg(int fd, ko_msghdr_t *mh, int flags) {
    unix_ep_t *e = (unix_ep_t *)kobj_of(fd, KOBJ_UNIX_SOCK);
    if (!e) return -(int)E_NOTSOCK;
    if (!mh) return -(int)E_FAULT;
    if (mh->msg_name) { /* no address for connected unix streams */ }

    if (e->len == 0 && e->rd_shutdown) return 0;   /* EOF */
    if (e->len == 0) {
        if (e->nonblock || (flags & KO_MSG_DONTWAIT))
            return -(int)E_AGAIN;
        /* cooperative: a blocking read here can only be satisfied by
         * our own process (socketpair), treat like nonblock+retry     */
        return -(int)E_AGAIN;
    }

    typedef struct { uint64_t base; uint64_t len; } kiovec_t;
    kiovec_t *iov = (kiovec_t *)mh->msg_iov;
    int64_t total = 0;
    bool peek = (flags & KO_MSG_PEEK) != 0;
    for (size_t i = 0; i < mh->msg_iovlen; i++) {
        char *dst = (char *)(uintptr_t)iov[i].base;
        size_t left = iov[i].len;
        if (!left) continue;
        if (peek) {
            /* peek: copy without consuming */
            size_t n = left < e->len ? left : e->len;
            for (size_t k = 0; k < n; k++)
                dst[k] = e->buf[(e->rpos + total + k) % SOCK_BUF_SIZE];
            total += (int64_t)n;
            break;
        }
        size_t got = ring_read(e, dst, left);
        total += (int64_t)got;
        if (got < left) break;
    }

    /* deliver ancillary: SCM_CREDENTIALS (if passcred) then SCM_RIGHTS */
    if (mh->msg_control && mh->msg_controllen) {
        uint8_t *c = (uint8_t *)mh->msg_control;
        size_t cap = mh->msg_controllen;
        size_t used = 0;

        if (e->passcred && cap - used >= sizeof(ko_cmsghdr_t) + sizeof(ko_ucred_t)) {
            ko_cmsghdr_t *ch = (ko_cmsghdr_t *)(c + used);
            ch->cmsg_len   = sizeof(ko_cmsghdr_t) + sizeof(ko_ucred_t);
            ch->cmsg_level = KO_SOL_SOCKET;
            ch->cmsg_type  = KO_SCM_CREDENTIALS;
            ko_ucred_t *u = (ko_ucred_t *)(c + used + sizeof(ko_cmsghdr_t));
            u->pid = e->creator_pid;
            u->uid = 0;
            u->gid = 0;
            used += KO_CMSG_ALIGN(ch->cmsg_len);
        }

        if (e->nfdq > 0 && cap - used >= sizeof(ko_cmsghdr_t)) {
            ko_cmsghdr_t *ch = (ko_cmsghdr_t *)(c + used);
            size_t nfds = (size_t)e->nfdq;
            size_t need = sizeof(ko_cmsghdr_t) + nfds * 4;
            if (cap - used >= need) {
                ch->cmsg_len   = need;
                ch->cmsg_level = KO_SOL_SOCKET;
                ch->cmsg_type  = KO_SCM_RIGHTS;
                int *out = (int *)(c + used + sizeof(ko_cmsghdr_t));
                for (size_t i = 0; i < nfds; i++) {
                    vfs_fd_t inst;
                    inst.node   = e->fdq[i].node;
                    inst.offset = e->fdq[i].offset;
                    inst.flags  = e->fdq[i].flags;
                    inst.used   = true;
                    out[i] = vfs_install_snapshot(&inst);
                    e->fdq[i].used = false;
                }
                e->nfdq = 0;
                used += need;
            }
        }
        mh->msg_controllen = used;
    } else {
        e->nfdq = 0;   /* no control buffer: passed fds are dropped */
        for (int i = 0; i < SOCK_MAX_FDSQ; i++) e->fdq[i].used = false;
    }

    mh->msg_flags = 0;
    return total ? total : -(int)E_AGAIN;
}

/* generic fd read/write dispatch */
static int kobj_read_ops(vfs_node_t *n, void *buf, size_t len, size_t off) {
    if (!n || !n->data) return -(int)E_BADF;
    kobj_hdr_t *h = (kobj_hdr_t *)n->data;
    switch (h->kind) {
    case KOBJ_UNIX_SOCK: {
        /* recv without msghdr, plain read() on a socket */
        unix_ep_t *e = (unix_ep_t *)n->data;
        if (e->len == 0 && e->rd_shutdown) return 0;
        if (e->len == 0) return -(int)E_AGAIN;
        return (int)ring_read(e, (char *)buf, len);
    }
    default:
        return kobj_evt_read(n, buf, len, off);
    }
}

static int kobj_write_ops(vfs_node_t *n, const void *buf, size_t len, size_t off) {
    if (!n || !n->data) return -(int)E_BADF;
    kobj_hdr_t *h = (kobj_hdr_t *)n->data;
    switch (h->kind) {
    case KOBJ_UNIX_SOCK: {
        unix_ep_t *e = (unix_ep_t *)n->data;
        if (!e->peer) return -(int)E_PIPE;
        return (int)ring_write(e->peer, (const char *)buf, len);
    }
    default:
        return kobj_evt_write(n, buf, len, off);
    }
}

static int kobj_close_ops(vfs_node_t *n) {
    if (!n || !n->data) return 0;
    kobj_hdr_t *h = (kobj_hdr_t *)n->data;
    switch (h->kind) {
    case KOBJ_UNIX_SOCK:
        ep_release((unix_ep_t *)n->data);
        break;
    default:
        kobj_evt_close(n);
        break;
    }
    return 0;
}

/* readiness */
int kobj_node_poll(vfs_node_t *n, int events) {
    if (!n || !n->data) return 0;
    kobj_hdr_t *h = (kobj_hdr_t *)n->data;
    switch (h->kind) {
    case KOBJ_UNIX_SOCK: {
        unix_ep_t *e = (unix_ep_t *)n->data;
        int ev = 0;
        if (events & KO_EPOLLIN) {
            if (e->len > 0) ev |= KO_EPOLLIN;
            if (e->rd_shutdown) ev |= KO_EPOLLIN | KO_EPOLLHUP;
            if (e->listening && e->npending > 0) ev |= KO_EPOLLIN;
        }
        if (events & KO_EPOLLOUT) {
            if (e->peer && e->peer->len < SOCK_BUF_SIZE) ev |= KO_EPOLLOUT;
        }
        return ev;
    }
    default:
        return kobj_evt_poll(n, events);
    }
}
