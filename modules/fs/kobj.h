#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "fs/vfs.h"

/*
 * kobj - kernel-object file descriptors (the Wayland-ready IPC layer)
 *
 * One implementation family behind VFS_TYPE_KOBJ nodes:
 *
 *   KOBJ_UNIX_SOCK  AF_UNIX stream sockets, socketpair, bind/listen/
 *                   accept/connect, sendmsg/recvmsg with SCM_RIGHTS fd
 *                   passing and SO_PASSCRED/SO_PEERCRED.  This is the
 *                   Wayland transport (wl_display runs over one).
 *   KOBJ_EPOLL      epoll instances (create1/ctl/wait) with real
 *                   readiness probing across pipes, sockets, eventfds,
 *                   timerfds and the console.
 *   KOBJ_EVENTFD    eventfd counters (libwayland wl_event_loop wakeup).
 *   KOBJ_TIMERFD    timerfd (frame pacing in event loops).
 *   KOBJ_SIGNALFD   signalfd4 (signal-to-fd conversion).
 *   KOBJ_MEMFD      in-memory files with page-list backing, resize via
 *                   ftruncate and real MAP_SHARED mappings (wl_shm pools
 *                   need pages, not copies).
 *
 * All objects live in the normal fd table: they dup, close, inherit
 * across fork and pass over SCM_RIGHTS exactly like files.
 */

/* Linux ABI constants (x86-64) */
#define KO_AF_UNIX      1
#define KO_SOCK_STREAM  1
#define KO_SOCK_DGRAM   2
#define KO_SOCK_CLOEXEC   0x80000
#define KO_SOCK_NONBLOCK  0x800

#define KO_MSG_NOSIGNAL   0x4000
#define KO_MSG_DONTWAIT   0x40
#define KO_MSG_PEEK       0x2
#define KO_MSG_CMSG_CLOEXEC 0x40000000

#define KO_SOL_SOCKET     1
#define KO_SO_TYPE        3
#define KO_SO_SNDBUF      7
#define KO_SO_RCVBUF      8
#define KO_SO_PASSCRED    16
#define KO_SO_PEERCRED    17

#define KO_SCM_RIGHTS     1
#define KO_SCM_CREDENTIALS 2

/* struct ucred (Linux) */
typedef struct {
    int32_t pid;
    uint32_t uid;
    uint32_t gid;
} ko_ucred_t;

/* Object kinds */

/* Every kernel object starts with this header (shared by kobj.c and
 * kobj_evt.c dispatchers). */
typedef struct {
    int kind;
    int fd_refs;
} kobj_hdr_t;

/* node constructor for kobj objects (implemented in kobj.c) */
vfs_node_t *kobj_new_node_impl(const char *name, void *obj);

/* fd lookup for kobj objects (implemented in kobj.c) */
void *kobj_of(int fd, int kind);
enum {
    KOBJ_UNIX_SOCK = 1,
    KOBJ_EPOLL,
    KOBJ_EVENTFD,
    KOBJ_TIMERFD,
    KOBJ_SIGNALFD,
    KOBJ_MEMFD,
};

/* Sockets */
int  kobj_socketpair(int domain, int type, int protocol, int fds[2]);
int  kobj_socket(int domain, int type, int protocol);       /* fd */
int  kobj_bind(int fd, const void *addr, uint32_t addrlen);
int  kobj_listen(int fd, int backlog);
int  kobj_accept4(int fd, void *addr, uint32_t *addrlen, int flags);
int  kobj_connect(int fd, const void *addr, uint32_t addrlen);
int  kobj_shutdown(int fd, int how);
int  kobj_getsockname(int fd, void *addr, uint32_t *addrlen);
int  kobj_getpeername(int fd, void *addr, uint32_t *addrlen);
int  kobj_setsockopt(int fd, int level, int optname,
                     const void *optval, uint32_t optlen);
int  kobj_getsockopt(int fd, int level, int optname,
                     void *optval, uint32_t *optlen);

/* struct msghdr - LINUX x86-64 layout (musl passes this verbatim) */
typedef struct {
    void    *msg_name;        /*  0  optional address            */
    uint32_t msg_namelen;     /*  8                              */
    uint32_t __pad0;
    void    *msg_iov;         /* 16  iovec array                 */
    size_t   msg_iovlen;      /* 24                              */
    void    *msg_control;     /* 32  ancillary buffer            */
    size_t   msg_controllen;  /* 40                              */
    int32_t  msg_flags;       /* 48                              */
} ko_msghdr_t;

typedef struct {
    uint64_t cmsg_len;        /* byte count incl. this header    */
    int32_t  cmsg_level;
    int32_t  cmsg_type;
} ko_cmsghdr_t;               /* data follows, size_t-aligned    */

#define KO_CMSG_ALIGN(x)  (((x) + 7) & ~7ULL)

int64_t kobj_sendmsg(int fd, const ko_msghdr_t *mh, int flags);
int64_t kobj_recvmsg(int fd, ko_msghdr_t *mh, int flags);

/* epoll */
#define KO_EPOLLIN      0x00000001
#define KO_EPOLLPRI     0x00000002
#define KO_EPOLLOUT     0x00000004
#define KO_EPOLLERR     0x00000008
#define KO_EPOLLHUP     0x00000010
#define KO_EPOLLRDHUP   0x00002000
#define KO_EPOLLET      0x80000000

#define KO_EPOLL_CTL_ADD 1
#define KO_EPOLL_CTL_MOD 2
#define KO_EPOLL_CTL_DEL 3

typedef union {
    void    *ptr;
    int64_t  fd_s;
    uint64_t u64;
} ko_epoll_data_t;

/* Note: Linux UAPI packs epoll_event on x86-64 (12 bytes, data at
 * offset 4) and musl matches, do not let the compiler pad it. */
typedef struct __attribute__((packed)) {
    uint32_t         events;   /* 0 */
    ko_epoll_data_t  data;     /* 4 (packed!) */
} ko_epoll_event_t;            /* 12 bytes */

int kobj_epoll_create1(int flags);
int kobj_epoll_ctl(int epfd, int op, int fd, ko_epoll_event_t *ev);
int kobj_epoll_wait(int epfd, ko_epoll_event_t *events, int maxevents,
                    int timeout_ms);

/* eventfd / timerfd / signalfd */
#define KO_EFD_SEMAPHORE 1
#define KO_EFD_CLOEXEC   0x80000
#define KO_EFD_NONBLOCK  0x800

#define KO_TFD_CLOEXEC   0x80000
#define KO_TFD_NONBLOCK  0x800

#define KO_SFD_CLOEXEC   0x80000
#define KO_SFD_NONBLOCK  0x800

#define KO_TFD_TIMER_ABSTIME 1

int kobj_eventfd(unsigned int initval, int flags);
int kobj_timerfd_create(int clockid, int flags);

typedef struct {
    int64_t  tv_sec;
    int64_t  tv_nsec;
} ko_itimerspec_time_t;

typedef struct {
    ko_itimerspec_time_t it_interval;
    ko_itimerspec_time_t it_value;
} ko_itimerspec_t;

int kobj_timerfd_settime(int fd, int flags,
                         const ko_itimerspec_t *newt,
                         ko_itimerspec_t *oldt);
int kobj_timerfd_gettime(int fd, ko_itimerspec_t *curr);

int kobj_signalfd4(int fd, const uint64_t *mask, size_t sigsetsize, int flags);

/* Linux signalfd_siginfo (first fields, whole struct is 128 bytes) */
typedef struct {
    uint32_t ssi_signo;
    int32_t  ssi_errno;
    int32_t  ssi_code;
    uint32_t ssi_pid;
    uint32_t ssi_uid;
    int32_t  ssi_fd;
    uint32_t ssi_tid;
    uint32_t ssi_band;
    uint32_t ssi_overrun;
    uint32_t ssi_trapno;
    int32_t  ssi_status;
    int32_t  ssi_int;
    uint64_t ssi_ptr;
    uint64_t ssi_utime;
    uint64_t ssi_stime;
    uint64_t ssi_addr;
    uint16_t ssi_addr_lsb;
    uint16_t __pad2;
    int32_t  ssi_syscall;
    uint64_t ssi_call_addr;
    uint32_t ssi_arch;
    uint8_t  __pad[28];
} ko_signalfd_siginfo_t;   /* 128 bytes */

/* memfd */
#define KO_MFD_CLOEXEC       0x0001
#define KO_MFD_ALLOW_SEALING 0x0002

int kobj_memfd_create(const char *name, int flags);

/* ftruncate(truncate) implementation - memfd resize, returns 0/-errno */
int64_t kobj_ftruncate(int fd, int64_t length);

/* MAP_SHARED mapping of a memfd: map the object's actual pages so
 * writes go straight through to the file (wl_shm pool semantics).
 * base is page-aligned user VA, npages pages starting at file offset
 * off (page-aligned). Returns 0/-errno. */
int kobj_memfd_mmap_shared(vfs_node_t *node, uint64_t base, size_t npages,
                           uint64_t off);

/* True when `phys` belongs to a live memfd (munmap must not free it). */
bool kobj_memfd_owns_phys(uint64_t phys);

/* shared plumbing */
/* readiness for poll/epoll: subset of events ready now */
int  kobj_node_poll(vfs_node_t *node, int events);
/* lookup node's object kind (0 if not a kobj) */
int  kobj_node_kind(vfs_node_t *node);
