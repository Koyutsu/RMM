/*
 *  pty - BSD-style pseudo-terminal pairs
 *
 *  /dev/ptmx is a cloning device: open() allocates a pty pair and the fd
 *  refers to a fresh master node.  /dev/pts/N resolves through the devpts
 *  finddir hook to the pair's slave node.
 *
 *  Master write  -> input line discipline (canonical editing + echo)  -> slave
 *  Slave write   -> output ring                                       -> master
 *
 *  Blocking model: readers/writers park via sched_kernel_park() in a probe
 *  loop (same pattern as the console read path and poll).  Parked reads
 *  abort with -EINTR when an unblocked signal becomes pending for the
 *  reader - this is what makes ^C (ISIG on the master side) actually
 *  interrupt a foreground job blocked on the slave.
 *
 *  Lifecycle / refcounts (pipe semantics): one reference per open end.
 *  fork takes its own reference for every inherited end (pty_refs_bump is
 *  called from vfs_pipe_refs_bump so both fork sites are covered), closing
 *  an end drops its reference, when both reach zero the two nodes and the
 *  pty slot are released.  Nodes are pinned, they are freed only here, so
 *  the generic close path must not free them.
 */

#include "fs/pty.h"
#include "fs/vfs.h"
#include "exec/process.h"
#include "exec/sched.h"
#include "drivers/timer/pit.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* ioctl numbers not in the shared header */
#define PTY_TIOCGPTN    0x80045430u
#define PTY_TIOCSPTLCK  0x40045431u
#define PTY_TIOCGPTPEER 0x80045432u
#define PTY_FIONREAD    0x541Bu

/* termios lflag bits (mirror of the console TTY layer) */
#define PTY_L_ISIG   0x00000001u
#define PTY_L_ICANON 0x00000002u
#define PTY_L_ECHO   0x00000008u

/* c_cc indices (Linux) */
#define CC_VINTR  0
#define CC_VQUIT  1
#define CC_VERASE 2
#define CC_VEOF   4

typedef struct {
    uint8_t  buf[PTY_BUF_SIZE];
    uint16_t rpos;
    uint16_t wpos;
    uint16_t len;
} pty_ring_t;

struct pty {
    bool        used;
    int         index;            /* pts number (0..PTY_MAX-1)              */
    vfs_node_t *master_node;
    vfs_node_t *slave_node;
    int         master_open;      /* open references on each end            */
    int         slave_open;
    bool        slave_ever_open;
    bool        locked;           /* TIOCSPTLCK state                       */
    bool        eof_pending;      /* ^D at empty line -> one-shot slave EOF  */

    /* slave-side termios (TCGETS/TCSETS on both ends address these) */
    uint32_t iflag, oflag, cflag, lflag;
    uint8_t  cc[32];

    /* input: master writes, slave reads (post line discipline) */
    pty_ring_t in;
    uint8_t    line[PTY_LINE_MAX];
    uint16_t   line_len;

    /* output: slave writes, master reads */
    pty_ring_t out;

    uint16_t   ws_row, ws_col;
    int        fg_pgrp;           /* foreground pgrp for ISIG (0 = none)    */
    int        creator_pgid;      /* fallback ISIG target                   */
};

static pty_t pty_table[PTY_MAX];

/* tiny helpers (freestanding kernel, no libc) */
static void p_memset(void *d, int v, size_t n) {
    uint8_t *p = (uint8_t *)d;
    while (n--) *p++ = (uint8_t)v;
}

static bool ring_empty(const pty_ring_t *r) { return r->len == 0; }
static bool ring_full(const pty_ring_t *r)  { return r->len == PTY_BUF_SIZE; }

static bool ring_push(pty_ring_t *r, uint8_t c) {
    if (r->len >= PTY_BUF_SIZE) return false;
    r->buf[r->wpos] = c;
    r->wpos = (uint16_t)((r->wpos + 1) % PTY_BUF_SIZE);
    r->len++;
    return true;
}

static bool ring_pop(pty_ring_t *r, uint8_t *out) {
    if (r->len == 0) return false;
    *out = r->buf[r->rpos];
    r->rpos = (uint16_t)((r->rpos + 1) % PTY_BUF_SIZE);
    r->len--;
    return true;
}

static size_t ring_read(pty_ring_t *r, uint8_t *dst, size_t len) {
    size_t got = 0;
    while (got < len && r->len) {
        dst[got++] = r->buf[r->rpos];
        r->rpos = (uint16_t)((r->rpos + 1) % PTY_BUF_SIZE);
        r->len--;
    }
    return got;
}

static size_t ring_write(pty_ring_t *r, const uint8_t *src, size_t len) {
    size_t put = 0;
    while (put < len && r->len < PTY_BUF_SIZE) {
        r->buf[r->wpos] = src[put++];
        r->wpos = (uint16_t)((r->wpos + 1) % PTY_BUF_SIZE);
        r->len++;
    }
    return put;
}

static size_t ring_space(const pty_ring_t *r) { return PTY_BUF_SIZE - r->len; }

/* node identity */
static const vfs_ops_t pty_master_ops;
static const vfs_ops_t pty_slave_ops;
extern const vfs_ops_t pty_pts_dir_ops;

/* legacy struct termio (TCGETA/TCSETA - 26 bytes, do not write more) */
typedef struct {
    uint16_t iflag, oflag, cflag, lflag, line;
    uint8_t  cc[8];
} pty_termio_t;

bool pty_node_is_master(vfs_node_t *n) {
    return n && n->type == VFS_TYPE_DEV && n->ops == &pty_master_ops;
}

bool pty_node_is_slave(vfs_node_t *n) {
    return n && n->type == VFS_TYPE_DEV && n->ops == &pty_slave_ops;
}

static pty_t *node_pty(vfs_node_t *n) { return (pty_t *)n->data; }

int pty_node_index(vfs_node_t *n) {
    pty_t *pt = node_pty(n);
    if (!pt || !pt->used) return -1;
    return pt->index;
}

/* Called by pseudo.c's /proc/self/fd readlink for pty fds. */
bool pty_node_paths(vfs_node_t *n, char *out, size_t outsz) {
    if (!n || !out || outsz == 0) return false;
    pty_t *pt = node_pty(n);
    if (!pt || !pt->used) return false;
    const char *fmt = pty_node_is_master(n) ? "/dev/ptmx" : "/dev/pts/%d";
    if (fmt[6] == 'p') {                                  /* /dev/ptmx   */
        size_t i = 0;
        const char *s = "/dev/ptmx";
        while (s[i] && i < outsz - 1) { out[i] = s[i]; i++; }
        out[i] = 0;
        return true;
    }
    /* /dev/pts/<index> */
    size_t p = 0;
    const char *pre = "/dev/pts/";
    while (pre[p] && p < outsz - 1) { out[p] = pre[p]; p++; }
    int v = pt->index;
    char tmp[8]; int t = 0;
    if (v == 0) tmp[t++] = '0';
    while (v > 0 && t < 8) { tmp[t++] = (char)('0' + v % 10); v /= 10; }
    while (t > 0 && p < outsz - 1) out[p++] = tmp[--t];
    out[p] = 0;
    return true;
}

/* ISIG delivery */
static void pty_raise_signal(pty_t *pt, int sig) {
    int target = pt->fg_pgrp ? pt->fg_pgrp : pt->creator_pgid;
    if (!target) return;
    for (int i = 0; i < PROC_MAX; i++) {
        process_t *p = &g_proc_table[i];
        if (p->state == PROC_STATE_EMPTY) continue;
        if (p->pgid != target && p->pid != target) continue;
        if (p->sigactions[sig].sa_handler == SIG_IGN) continue;
        p->pending_sigs |= (1ull << sig);
        sched_wake(p);                    /* parked readers see it -> EINTR */
    }
}

/* Parked-read abort: true when the calling task has a deliverable signal */
static bool sig_pending_for_me(void) {
    process_t *me = g_current_proc;
    return me && (me->pending_sigs & ~me->sig_mask) != 0;
}

/* input line discipline (master -> slave) */
static void pty_line_flush(pty_t *pt) {
    for (uint16_t i = 0; i < pt->line_len; i++) ring_push(&pt->in, pt->line[i]);
    pt->line_len = 0;
}

static void pty_input_byte(pty_t *pt, uint8_t c) {
    bool canonical = (pt->lflag & PTY_L_ICANON) != 0;

    if (!canonical) { ring_push(&pt->in, c); return; }

    /* ERASE */
    if (c == '\b' || c == 0x7f || c == pt->cc[CC_VERASE]) {
        if (pt->line_len > 0) {
            pt->line_len--;
            if (pt->lflag & PTY_L_ECHO) {
                ring_push(&pt->out, '\b');
                ring_push(&pt->out, ' ');
                ring_push(&pt->out, '\b');
            }
        }
        return;
    }
    /* end of line: flush the assembled line + '\n' */
    if (c == '\n' || c == '\r') {
        if (pt->lflag & PTY_L_ECHO) ring_push(&pt->out, '\n');
        pty_line_flush(pt);
        ring_push(&pt->in, '\n');
        return;
    }
    /* INTR / QUIT */
    if ((pt->lflag & PTY_L_ISIG) && c == pt->cc[CC_VINTR]) {
        pt->line_len = 0;
        if (pt->lflag & PTY_L_ECHO) {
            ring_push(&pt->out, '^'); ring_push(&pt->out, 'C'); ring_push(&pt->out, '\n');
        }
        pty_raise_signal(pt, SIGINT);
        return;
    }
    if ((pt->lflag & PTY_L_ISIG) && c == pt->cc[CC_VQUIT]) {
        pt->line_len = 0;
        if (pt->lflag & PTY_L_ECHO) {
            ring_push(&pt->out, '^'); ring_push(&pt->out, '\\'); ring_push(&pt->out, '\n');
        }
        pty_raise_signal(pt, SIGQUIT);
        return;
    }
    /* EOF: at line start -> one-shot read EOF, else flush partial line */
    if (c == pt->cc[CC_VEOF]) {
        if (pt->line_len == 0) pt->eof_pending = true;
        else pty_line_flush(pt);
        return;
    }
    /* printable + other bytes accumulate into the line */
    if (pt->line_len < PTY_LINE_MAX) {
        if (pt->lflag & PTY_L_ECHO && c >= 0x20 && c < 0x7f)
            ring_push(&pt->out, c);
        pt->line[pt->line_len++] = c;
    }
}

/* hangup detection */
static bool master_gone(pty_t *pt)  { return pt->master_open == 0; }
static bool slave_gone(pty_t *pt) {
    return pt->slave_ever_open && pt->slave_open == 0;
}

/* read paths */
static int pty_master_read(vfs_node_t *node, void *buf, size_t len, size_t off) {
    (void)off;
    pty_t *pt = node_pty(node);
    if (!pt || !pt->used) return -(int)VFS_EIO;
    {
        extern void serial_print_string(const char *);
        char b[64]; const char *hx = "0123456789abcdef";
        uint64_t pp = (uint64_t)(uintptr_t)pt;
        b[0]='m'; b[1]='r'; b[2]=' '; b[3]='p'; b[4]='t'; b[5]='=';
        for (int k = 0; k < 12; k++) b[6 + k] = hx[(pp >> ((11 - k) * 4)) & 0xF];
        b[18] = ' '; b[19] = 'l'; b[20] = 'e'; b[21] = 'n'; b[22] = '=';
        int i = 23; { uint64_t v = len; char t[12]; int n = 0;
                      do { t[n++] = '0' + (v % 10); v /= 10; } while (v);
                      while (n) b[i++] = t[--n]; }
        b[i] = 0;
        serial_print_string(b);
        serial_print_string("\n");
    }

    for (;;) {
        if (!ring_empty(&pt->out))
            return (int)ring_read(&pt->out, (uint8_t *)buf, len);
        if (slave_gone(pt)) return -(int)VFS_EIO;      /* hangup            */
        if (sig_pending_for_me()) return -(int)EINTR;
        if (sched_kernel_park(BORE_EVENT_POLL_MS) != 0)
            __asm__ volatile("sti; hlt; cli");
    }
}

static int pty_slave_read(vfs_node_t *node, void *buf, size_t len, size_t off) {
    (void)off;
    pty_t *pt = node_pty(node);
    if (!pt || !pt->used) return -(int)VFS_EIO;

    for (;;) {
        if (master_gone(pt)) return -(int)VFS_EIO;     /* hangup            */
        if (pt->eof_pending) {
            pt->eof_pending = false;
            return 0;
        }
        if (!ring_empty(&pt->in))
            return (int)ring_read(&pt->in, (uint8_t *)buf, len);
        if (sig_pending_for_me()) return -(int)EINTR;
        if (sched_kernel_park(BORE_EVENT_POLL_MS) != 0)
            __asm__ volatile("sti; hlt; cli");
    }
}

/* write paths */
static int pty_master_write(vfs_node_t *node, const void *buf, size_t len,
                            size_t off) {
    (void)off;
    pty_t *pt = node_pty(node);
    if (!pt || !pt->used) return -(int)VFS_EIO;
    const uint8_t *s = (const uint8_t *)buf;
    size_t put = 0;

    while (put < len) {
        if (slave_gone(pt) && ring_empty(&pt->in) && pt->line_len == 0)
            return -(int)VFS_EIO;                      /* hangup            */
        if (pt->lflag & PTY_L_ICANON) {
            /* keep room for one more line flush in the input ring */
            if (pt->line_len >= PTY_LINE_MAX &&
                ring_space(&pt->in) > pt->line_len)
                pty_line_flush(pt);
            if (pt->line_len < PTY_LINE_MAX) {
                pty_input_byte(pt, s[put++]);
                continue;
            }
        } else {
            if (ring_space(&pt->in) > 0) {
                ring_push(&pt->in, s[put++]);
                continue;
            }
        }
        if (sig_pending_for_me()) return put ? (int)put : -(int)EINTR;
        if (sched_kernel_park(BORE_EVENT_POLL_MS) != 0)
            __asm__ volatile("sti; hlt; cli");
    }
    return (int)put;
}

static int pty_slave_write(vfs_node_t *node, const void *buf, size_t len,
                           size_t off) {
    (void)off;
    pty_t *pt = node_pty(node);
    if (!pt || !pt->used) return -(int)VFS_EIO;


    size_t put = 0;
    while (put < len) {
        if (master_gone(pt)) return -(int)VFS_EIO;     /* hangup            */
        size_t n = ring_write(&pt->out, (const uint8_t *)buf + put, len - put);
        put += n;
        if (put == len) break;
        if (sig_pending_for_me()) return put ? (int)put : -(int)EINTR;
        if (sched_kernel_park(BORE_EVENT_POLL_MS) != 0)
            __asm__ volatile("sti; hlt; cli");
    }
    return (int)put;
}

/* termios (byte layout identical to the console TTY layer) */
static void pty_termios_get(pty_t *pt, uint8_t *p) {
    p[0]  = (uint8_t)(pt->iflag);        p[1]  = (uint8_t)(pt->iflag >> 8);
    p[2]  = (uint8_t)(pt->iflag >> 16);  p[3]  = (uint8_t)(pt->iflag >> 24);
    p[4]  = (uint8_t)(pt->oflag);        p[5]  = (uint8_t)(pt->oflag >> 8);
    p[6]  = (uint8_t)(pt->oflag >> 16);  p[7]  = (uint8_t)(pt->oflag >> 24);
    p[8]  = (uint8_t)(pt->cflag);        p[9]  = (uint8_t)(pt->cflag >> 8);
    p[10] = (uint8_t)(pt->cflag >> 16);  p[11] = (uint8_t)(pt->cflag >> 24);
    p[12] = (uint8_t)(pt->lflag);        p[13] = (uint8_t)(pt->lflag >> 8);
    p[14] = (uint8_t)(pt->lflag >> 16);  p[15] = (uint8_t)(pt->lflag >> 24);
    p[16] = 0;
    for (int i = 0; i < 32; i++) p[17 + i] = pt->cc[i];
    for (int i = 49; i < 60; i++) p[i] = 0;
}

static void pty_termios_set(pty_t *pt, const uint8_t *p, bool flush) {
    pt->iflag = (uint32_t)p[0]  | ((uint32_t)p[1]  << 8)
              | ((uint32_t)p[2]  << 16) | ((uint32_t)p[3]  << 24);
    pt->oflag = (uint32_t)p[4]  | ((uint32_t)p[5]  << 8)
              | ((uint32_t)p[6]  << 16) | ((uint32_t)p[7]  << 24);
    pt->cflag = (uint32_t)p[8]  | ((uint32_t)p[9]  << 8)
              | ((uint32_t)p[10] << 16) | ((uint32_t)p[11] << 24);
    pt->lflag = (uint32_t)p[12] | ((uint32_t)p[13] << 8)
              | ((uint32_t)p[14] << 16) | ((uint32_t)p[15] << 24);
    for (int i = 0; i < 32; i++) pt->cc[i] = p[17 + i];
    if (flush) {
        pt->in.rpos = pt->in.wpos = pt->in.len = 0;
        pt->line_len = 0;
        pt->eof_pending = false;
    }
}

/* winsize */
typedef struct { uint16_t row, col, xp, yp; } pty_winsize_t;

/* ioctl router */
int64_t pty_ioctl(vfs_node_t *n, int fd, uint64_t req, uint64_t arg) {
    pty_t *pt = node_pty(n);
    if (!pt || !pt->used) return -(int64_t)ENXIO;
    bool is_master = pty_node_is_master(n);

    switch (req) {
    /* master-only pty allocation ioctls */
    case PTY_TIOCGPTN: {
        if (!is_master) return -(int64_t)ENOTTY;
        int *out = (int *)(uintptr_t)arg;
        if (out) *out = pt->index;
        return 0;
    }
    case PTY_TIOCSPTLCK: {
        if (!is_master) return -(int64_t)ENOTTY;
        int *in = (int *)(uintptr_t)arg;
        if (in) pt->locked = (*in != 0);
        return 0;
    }
    case PTY_TIOCGPTPEER: {
        if (!is_master) return -(int64_t)ENOTTY;
        vfs_fd_t snap;
        int r = pty_open_slave_fd(pt, (int)arg, &snap);
        if (r < 0) return r;
        int nfd = vfs_install_snapshot(&snap);
        return nfd;
    }

    /* terminal attributes (both ends, shared per-pty state) */
    case TCGETS: {
        uint8_t *p = (uint8_t *)(uintptr_t)arg;
        if (!p) return -(int64_t)EFAULT;
        pty_termios_get(pt, p);
        return 0;
    }
    case TCGETA: {
        pty_termio_t *t = (pty_termio_t *)(uintptr_t)arg;
        if (!t) return -(int64_t)EFAULT;
        t->iflag = (uint16_t)pt->iflag;
        t->oflag = (uint16_t)pt->oflag;
        t->cflag = (uint16_t)pt->cflag;
        t->lflag = (uint16_t)pt->lflag;
        t->line  = 0;
        for (int i = 0; i < 8; i++) t->cc[i] = pt->cc[i];
        return 0;
    }
    case TCSETS: case TCSETSW: case TCSETSF: {
        const uint8_t *p = (const uint8_t *)(uintptr_t)arg;
        if (!p) return -(int64_t)EFAULT;
        pty_termios_set(pt, p, req == TCSETSF);
        return 0;
    }

    /* window size */
    case TIOCGWINSZ: {
        pty_winsize_t *ws = (pty_winsize_t *)(uintptr_t)arg;
        if (ws) { ws->row = pt->ws_row; ws->col = pt->ws_col; ws->xp = 0; ws->yp = 0; }
        return 0;
    }
    case TIOCSWINSZ: {
        pty_winsize_t *ws = (pty_winsize_t *)(uintptr_t)arg;
        if (ws && ws->row) pt->ws_row = ws->row;
        if (ws && ws->col) pt->ws_col = ws->col;
        return 0;
    }

    /* foreground process group */
    case TIOCGPGRP: {
        int *pgrp = (int *)(uintptr_t)arg;
        if (pgrp) {
            if (pt->fg_pgrp) *pgrp = pt->fg_pgrp;
            else if (g_current_proc)
                *pgrp = g_current_proc->pgid ? g_current_proc->pgid
                                             : g_current_proc->pid;
            else *pgrp = 0;
        }
        return 0;
    }
    case TIOCSPGRP: {
        int *pgrp = (int *)(uintptr_t)arg;
        if (pgrp) pt->fg_pgrp = *pgrp;
        return 0;
    }
    case TIOCGSID: {
        if (!is_master) return -(int64_t)ENOTTY;
        int *sid = (int *)(uintptr_t)arg;
        if (sid && g_current_proc)
            *sid = g_current_proc->sid ? g_current_proc->sid
                                       : g_current_proc->pid;
        return 0;
    }
    case TIOCSCTTY:
        return 0;                       /* accept - no full session audit */

    /* byte counts */
    case PTY_FIONREAD: {
        int *out = (int *)(uintptr_t)arg;
        if (out) *out = is_master ? (int)pt->out.len
                                  : (int)pt->in.len + (int)pt->line_len;
        return 0;
    }

    default:
        (void)fd;
        return -(int64_t)ENOTTY;
    }
}

/* poll readiness */
#define POLLIN_L  0x0001
#define POLLOUT_L 0x0004
#define POLLHUP_L 0x0010
#define POLLERR_L 0x0008

int pty_poll_node(vfs_node_t *n, int events) {
    pty_t *pt = node_pty(n);
    if (!pt || !pt->used) return POLLHUP_L;
    bool is_master = pty_node_is_master(n);
    int ev = 0;

    if (is_master) {
        if (events & POLLIN_L) {
            if (!ring_empty(&pt->out)) ev |= POLLIN_L;
        }
        if (events & POLLOUT_L) {
            if (pt->line_len < PTY_LINE_MAX || ring_space(&pt->in) > 0)
                ev |= POLLOUT_L;
        }
        if (slave_gone(pt)) ev |= POLLHUP_L;
    } else {
        if (events & POLLIN_L) {
            if (!ring_empty(&pt->in) || pt->eof_pending) ev |= POLLIN_L;
        }
        if (events & POLLOUT_L) {
            if (ring_space(&pt->out) > 0) ev |= POLLOUT_L;
        }
        if (master_gone(pt)) ev |= POLLHUP_L | POLLERR_L;
    }
    return ev & events;
}

/* open / close plumbing */
static int pty_node_open(vfs_node_t *node, int flags) {
    pty_t *pt = node_pty(node);
    if (!pt || !pt->used) return -(int)VFS_ENODEV;
    if (pty_node_is_slave(node)) {
        pt->slave_open++;
        pt->slave_ever_open = true;
    } else if (pty_node_is_master(node)) {
        pt->master_open++;
    }
    (void)flags;
    return 0;
}

int pty_open_slave_fd(pty_t *pt, int flags, vfs_fd_t *out) {
    if (!pt || !pt->used || !out) return -(int)ENXIO;
    out->used   = true;
    out->node   = pt->slave_node;
    out->offset = 0;
    out->flags  = flags;
    return 0;
}

int pty_open_ptmx(vfs_node_t *node, vfs_fd_t *slot, int flags) {
    (void)node;
    if (!slot) return -(int)EINVAL;

    /* allocate a pty slot */
    pty_t *pt = NULL;
    for (int i = 0; i < PTY_MAX; i++) {
        if (!pty_table[i].used) { pt = &pty_table[i]; break; }
    }
    if (!pt) return -(int)ENODEV;       /* "out of ptys" - like Linux EIO  */

    p_memset(pt, 0, sizeof *pt);
    pt->used      = true;
    pt->index     = (int)(pt - pty_table);
    pt->iflag     = 0x00000000u;
    pt->oflag     = 0x00000000u;
    pt->cflag     = 0x000004BFu;
    pt->lflag     = 0x0000003Bu;        /* ISIG|ICANON|ECHO|ECHOE|ECHOK    */
    p_memset(pt->cc, 0, sizeof pt->cc);
    pt->cc[CC_VINTR]  = 3;              /* ^C   */
    pt->cc[CC_VQUIT]  = 28;             /* ^\   */
    pt->cc[CC_VERASE] = 0x7f;           /* DEL  */
    pt->cc[CC_VEOF]   = 4;              /* ^D   */
    pt->cc[6]         = 1;              /* VMIN */
    pt->ws_row = 24; pt->ws_col = 80;
    if (g_current_proc)
        pt->creator_pgid = g_current_proc->pgid ? g_current_proc->pgid
                                                : g_current_proc->pid;

    /* two nodes (pinned, freed only with the pty) */
    pt->master_node = vfs_create_node("ptmx", VFS_TYPE_DEV,
                                      &pty_master_ops, pt);
    pt->slave_node  = vfs_create_node("pts",  VFS_TYPE_DEV,
                                      &pty_slave_ops, pt);
    if (!pt->master_node || !pt->slave_node) {
        if (pt->master_node) vfs_free_node(pt->master_node);
        if (pt->slave_node)  vfs_free_node(pt->slave_node);
        pt->used = false;
        return -(int)ENOMEM;
    }
    pt->master_node->mode  = 0620 | 0020000;   /* S_IFCHR, owner-writable */
    pt->master_node->inode = 4200 + pt->index * 2;
    pt->master_node->pinned = 1;
    pt->slave_node->mode   = 0620 | 0020000;
    pt->slave_node->inode  = 4201 + pt->index * 2;
    pt->slave_node->pinned  = 1;

    slot->used   = true;
    slot->node   = pt->master_node;
    slot->offset = 0;
    slot->flags  = flags;
    pt->master_open = 1;
    return 0;
}

void pty_on_close(vfs_node_t *n, bool inherited) {
    pty_t *pt = node_pty(n);
    if (!pt || !pt->used) return;
    if (inherited) return;              /* fork's borrowed view: no release */

    if (pty_node_is_master(n)) {
        if (pt->master_open > 0) pt->master_open--;
    } else if (pty_node_is_slave(n)) {
        if (pt->slave_open > 0) pt->slave_open--;
    }

    if (pt->master_open == 0 && pt->slave_open == 0) {
        pt->used = false;
        vfs_node_t *m = pt->master_node;
        vfs_node_t *s = pt->slave_node;
        pt->master_node = NULL;
        pt->slave_node  = NULL;
        if (m) vfs_free_node(m);
        if (s) vfs_free_node(s);
    }
}

void pty_ref_bump_node(vfs_node_t *n) {
    pty_t *pt = node_pty(n);
    if (!pt || !pt->used) return;
    if (pty_node_is_master(n)) pt->master_open++;
    else if (pty_node_is_slave(n)) pt->slave_open++;
}

void pty_refs_bump(const vfs_fd_t table[VFS_MAX_FDS]) {
    for (int i = 0; i < VFS_MAX_FDS; i++) {
        vfs_node_t *n = table[i].node;
        if (!table[i].used || !n) continue;
        pty_t *pt = node_pty(n);
        if (!pt || !pt->used) continue;
        if (pty_node_is_master(n)) pt->master_open++;
        else if (pty_node_is_slave(n)) pt->slave_open++;
    }
}

/* devpts directory (/dev/pts) */
vfs_node_t *pty_pts_find(vfs_node_t *dir, const char *name) {
    (void)dir;
    /* name must be a plain decimal number */
    int v = 0;
    if (!name || !name[0]) return NULL;
    for (int i = 0; name[i]; i++) {
        if (name[i] < '0' || name[i] > '9') return NULL;
        v = v * 10 + (name[i] - '0');
        if (v > PTY_MAX * 10) return NULL;
    }
    if (v < 0 || v >= PTY_MAX) return NULL;
    pty_t *pt = &pty_table[v];
    if (!pt->used) return NULL;
    return pt->slave_node;
}

int pty_pts_readdir(vfs_node_t *dir, int idx, char *name_out,
                    uint8_t *type_out) {
    (void)dir;
    int seen = 0;
    for (int i = 0; i < PTY_MAX; i++) {
        if (!pty_table[i].used) continue;
        if (seen++ == idx) {
            /* decimal name */
            int v = pty_table[i].index;
            char tmp[8]; int t = 0;
            if (v == 0) tmp[t++] = '0';
            while (v > 0) { tmp[t++] = (char)('0' + v % 10); v /= 10; }
            int k = 0;
            while (t > 0 && k < VFS_NAME_MAX - 1) name_out[k++] = tmp[--t];
            name_out[k] = 0;
            *type_out = VFS_TYPE_DEV;
            return 0;
        }
    }
    return -1;
}

/* ops tables (identities are the node-kind markers) */
static const vfs_ops_t pty_master_ops = {
    .open  = pty_node_open,
    .read  = pty_master_read,
    .write = pty_master_write,
};

static const vfs_ops_t pty_slave_ops = {
    .open  = pty_node_open,
    .read  = pty_slave_read,
    .write = pty_slave_write,
};

/* devpts directory ops, referenced by pseudo.c (extern) */
const vfs_ops_t pty_pts_dir_ops = {
    .finddir = pty_pts_find,
    .readdir = pty_pts_readdir,
};
