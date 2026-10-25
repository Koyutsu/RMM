#pragma once
#include <stddef.h>
#include <stdint.h>

/*
 * RMM syscall interface - Linux x86-64 ABI
 *
 * Primary path:  syscall instruction  (used by musl)
 *   rax = nr,  rdi/rsi/rdx/r10/r8/r9 = args 1-6
 *   rcx/r11 clobbered by cpu, return value in rax
 *
 * Secondary path:  INT 0x80  (legacy, same register convention)
 *
 * Syscall numbers match Linux x86-64 (arch/x86/entry/syscalls/syscall_64.tbl)
 * so that musl compiled for x86_64-linux works without renumbering.
 *
 * Important: every user-visible structure and constant in this file follows
 * the LINUX ABI, not newlib's.  musl passes kernel structs straight through.
 */

/* Core I/O */
#define SYS_READ 0
#define SYS_WRITE 1
#define SYS_OPEN 2
#define SYS_CLOSE 3
#define SYS_STAT 4
#define SYS_FSTAT 5
#define SYS_LSTAT 6
#define SYS_POLL 7
#define SYS_LSEEK 8
#define SYS_MMAP 9
#define SYS_MPROTECT 10
#define SYS_MUNMAP 11
#define SYS_BRK 12

/* Signals */
#define SYS_RT_SIGACTION 13
#define SYS_RT_SIGPROCMASK 14
#define SYS_RT_SIGRETURN 15
#define SYS_SIGALTSTACK 131
#define SYS_KILL 62
#define SYS_TKILL 200
#define SYS_TGKILL 234

/* Timer / alarm */
#define SYS_ALARM 37
#define SYS_SCHED_YIELD 24
#define SYS_RT_SIGTIMEDWAIT 128 /* sigwaitinfo/sigtimedwait */
#define SYS_SETITIMER 38
#define SYS_GETITIMER 36
#define SYS_NANOSLEEP 35
#define SYS_PAUSE 34

/* syscall round-out (Linux x86-64 numbers) */
#define SYS_MINCORE 27
#define SYS_FLOCK 73
#define SYS_SCHED_SETAFFINITY 203
#define SYS_SCHED_GETAFFINITY 204
#define SYS_TIMER_CREATE 222
#define SYS_TIMER_SETTIME 223
#define SYS_TIMER_GETTIME 224
#define SYS_TIMER_GETOVERRUN 225
#define SYS_TIMER_DELETE 226
#define SYS_GETCPU 309
#define SYS_FACCESSAT2 439

/* Session / pgroup */
#define SYS_SETSID 112
#define SYS_GETSID 124

/* I/O control */
#define SYS_IOCTL 16
#define SYS_PREAD64 17
#define SYS_PWRITE64 18
#define SYS_READV 19
#define SYS_WRITEV 20
#define SYS_ACCESS 21
#define SYS_PIPE 22
#define SYS_SELECT 23
#define SYS_DUP 32
#define SYS_DUP2 33
#define SYS_DUP3 292
#define SYS_GETPID 39
#define SYS_SENDFILE 40

/* Process */
#define SYS_FORK 57
#define SYS_VFORK 58
#define SYS_CLONE 56
#define SYS_EXECVE 59
#define SYS_EXIT 60
#define SYS_WAIT4 61
#define SYS_WAITID 247
#define SYS_UNAME 63
#define SYS_SETHOSTNAME 170
#define SYS_FCNTL 72
#define SYS_GETCWD 79

/* tee(2)/splice(2) family - dash probes tee(0, pipefd[1], ...) on startup
 * to decide whether it can tee console input into a pipe.  Linux returns
 * EINVAL when the fds are not pipes, dash falls back to plain read().  */
#define SYS_TEE 276
#define SYS_CHDIR 80
#define SYS_FCHDIR 81
#define SYS_MKDIR 83
#define SYS_RMDIR 84
#define SYS_CREAT 85
#define SYS_LINK 86
#define SYS_UNLINK 87
#define SYS_SYMLINK 88
#define SYS_READLINK 89
#define SYS_CHMOD 90
#define SYS_FCHMOD 91
#define SYS_CHOWN 92
#define SYS_FCHOWN 93
#define SYS_LCHOWN 94
#define SYS_SETREUID 113
#define SYS_SETREGID 114
#define SYS_SETGROUPS 116
#define SYS_SETRESUID 117
#define SYS_GETRESUID 118
#define SYS_SETRESGID 119
#define SYS_GETRESGID 120
#define SYS_MKNOD 133
#define SYS_MOUNT 165
#define SYS_UMOUNT2 166
#define SYS_UTIMENSAT 280
#define SYS_GETUID 102
#define SYS_GETGID 104
#define SYS_GETEUID 107
#define SYS_GETEGID 108
#define SYS_GETPPID 110
#define SYS_GETPGRP 111
#define SYS_GETPGID 121
#define SYS_SETPGID 109
#define SYS_UMASK 95
#define SYS_GETTIMEOFDAY 96
#define SYS_GETRUSAGE 98
#define SYS_TIMES 100
#define SYS_GETRANDOM 318
#define SYS_SETUID 105
#define SYS_SETGID 106

/* Memory / TLS */
#define SYS_MREMAP 25
#define SYS_MSYNC 26
#define SYS_MADVISE 28
#define SYS_ARCH_PRCTL 158
#define SYS_SET_TID_ADDRESS 218

/* Directory iteration */
#define SYS_GETDENTS 78 /* old getdents  */
#define SYS_GETDENTS64 217

/* Thread/tid */
#define SYS_GETTID 186
#define SYS_EXIT_GROUP 231
#define SYS_FUTEX 202
#define SYS_SET_ROBUST_LIST 273
#define SYS_GET_ROBUST_LIST 274

/* Modern openat family */
#define SYS_OPENAT 257
#define SYS_MKDIRAT 258
#define SYS_MKNODAT 259
#define SYS_FCHOWNAT 260
#define SYS_FSTATAT 262    /* newfstatat */
#define SYS_UNLINKAT 263
#define SYS_RENAMEAT 264
#define SYS_SYMLINKAT 266
#define SYS_READLINKAT 267
#define SYS_FCHMODAT 268
#define SYS_FACCESSAT 269

/* Legacy/compat numbers (kept for the dispatcher) */
#define SYS_RENAME 82      /* legacy rename            */
#define SYS_GETGROUPS 115  /* getgroups                */
#define SYS_TIME 201       /* legacy time()            */
#define SYS_PIPE2 293      /* pipe2                    */

/* Phase-1 completeness */
#define SYS_STATFS 137        /* statfs(2) - struct statfs, 120 bytes      */
#define SYS_FSTATFS 138       /* fstatfs(2)                                 */
#define SYS_RENAMEAT2 316     /* renameat2(2) - RENAME_NOREPLACE supported  */
#define SYS_COPY_FILE_RANGE 327

#define RENAME_NOREPLACE 1u
#define RENAME_EXCHANGE  2u
#define RENAME_WHITEOUT  4u

/* RMM-private (above 511 to avoid Linux collisions) */
/* Deprecated RMM-private syscalls, kept for
 * the existing userland only, Linux-standard replacements exist:
 * PCILIST -> /proc/bus/pci/devices, FBINFO -> /dev/fb0 (FBIOGET_*) +
 * /proc/fb, CPUINFO -> /proc/cpuinfo, PUTS/GETCHAR -> write/read,
 * UPTIME -> /proc/uptime.  Do not use these in new userland. */
#define SYS_RMM_PUTS 512    /* fast puts(str), not in Linux (deprec.) */
#define SYS_RMM_GETCHAR 513 /* keyboard getchar() (deprecated)         */
#define SYS_RMM_UPTIME 514  /* ms since boot (deprecated)              */
#define SYS_RMM_PCILIST 515 /* PCI device table as text (deprecated)   */
#define SYS_RMM_FBINFO 516  /* framebuffer geometry {w,h,bpp} (deprec.)*/
#define SYS_RMM_CPUINFO 517 /* CPUID brand string into (buf,len)(deprec.) */
#define SYS_SYSCONF 152        /* not a Linux syscall, stub for old code  */

/* AF_UNIX socket family (Linux x86-64 numbers) */
#define SYS_SOCKET 41
#define SYS_CONNECT 42
#define SYS_ACCEPT 43
#define SYS_ACCEPT4 288
#define SYS_SENDTO 44
#define SYS_RECVFROM 45
#define SYS_SENDMSG 46
#define SYS_RECVMSG 47
#define SYS_SHUTDOWN 48
#define SYS_BIND 49
#define SYS_LISTEN 50
#define SYS_GETSOCKNAME 51
#define SYS_GETPEERNAME 52
#define SYS_SOCKETPAIR 53
#define SYS_SETSOCKOPT 54
#define SYS_GETSOCKOPT 55

/* Event loop family (epoll / eventfd / timerfd / signalfd) */
#define SYS_SIGNALFD 282
#define SYS_EPOLL_WAIT 232
#define SYS_EVENTFD 284
#define SYS_EPOLL_PWAIT 281
#define SYS_EPOLL_CTL 233
#define SYS_EVENTFD2 290
#define SYS_EPOLL_CREATE1 291
#define SYS_SIGNALFD4 289
#define SYS_TIMERFD_CREATE 283
#define SYS_TIMERFD_SETTIME 286
#define SYS_TIMERFD_GETTIME 287

/* Shared memory / file resize */
#define SYS_TRUNCATE 76
#define SYS_FTRUNCATE 77
#define SYS_MEMFD_CREATE 319

/* Resource limits (stubbed) */
#define SYS_GETRLIMIT 97
#define SYS_SETRLIMIT 160
#define SYS_PRLIMIT64 302
#define SYS_SYSINFO 99
#define SYS_PRCTL    157

/* statx (musl 1.2.x calls this first in fstatat, no fallback!) */
#define SYS_STATX 332

/* Linux struct statx (include/uapi/linux/stat.h) - 256 bytes */
#define STATX_TYPE 0x0001
#define STATX_MODE 0x0002
#define STATX_NLINK 0x0004
#define STATX_UID 0x0008
#define STATX_GID 0x0010
#define STATX_ATIME 0x0020
#define STATX_MTIME 0x0040
#define STATX_CTIME 0x0080
#define STATX_INO 0x0100
#define STATX_SIZE 0x0200
#define STATX_BLOCKS 0x0400
#define STATX_BASIC_STATS 0x07ff

typedef struct {
    int64_t tv_sec;
    uint32_t tv_nsec;
    int32_t __reserved;
} linux_statx_timestamp_t;

typedef struct {
    uint32_t stx_mask;           /* 0x00 */
    uint32_t stx_blksize;        /* 0x04 */
    uint64_t stx_attributes;     /* 0x08 */
    uint32_t stx_nlink;          /* 0x10 */
    uint32_t stx_uid;            /* 0x14 */
    uint32_t stx_gid;            /* 0x18 */
    uint16_t stx_mode;           /* 0x1c */
    uint16_t __spare0[1];        /* 0x1e */
    uint64_t stx_ino;            /* 0x20 */
    uint64_t stx_size;           /* 0x28 */
    uint64_t stx_blocks;         /* 0x30 */
    uint64_t stx_attributes_mask;/* 0x38 */
    linux_statx_timestamp_t stx_atime;  /* 0x40 */
    linux_statx_timestamp_t stx_btime;  /* 0x50 */
    linux_statx_timestamp_t stx_ctime;  /* 0x60 */
    linux_statx_timestamp_t stx_mtime;  /* 0x70 */
    uint32_t stx_rdev_major;     /* 0x80 */
    uint32_t stx_rdev_minor;     /* 0x84 */
    uint32_t stx_dev_major;      /* 0x88 */
    uint32_t stx_dev_minor;      /* 0x8c */
    uint64_t __spare2[14];       /* 0x90 */
} linux_statx_t;                 /* 256 bytes */

/* Clocks */
#define SYS_CLOCK_GETTIME 228
#define SYS_CLOCK_GETRES 229

/* errno values (POSIX / Linux) */
#define EPERM 1
#define ENOENT 2
#define ESRCH 3
#define EINTR 4
#define EIO 5
#define ENXIO 6
#define EBADF 9
#define ECHILD 10
#define EAGAIN 11
#define EWOULDBLOCK EAGAIN
#define ENOMEM 12
#define EACCES 13
#define EFAULT 14
#define EBUSY 16
#define EEXIST 17
#define EXDEV 18
#define ENODEV 19
#define ENOTDIR 20
#define EISDIR 21
#define EINVAL 22
#define ENFILE 23
#define EMFILE 24
#define ENOTTY 25
#define EFBIG 27
#define ENOSPC 28
#define ESPIPE 29
#define EROFS 30
#define EMLINK 31
#define EPIPE 32
#define EDOM 33
#define ERANGE 34
#define ENOSYS 38
#define ETIMEDOUT 110
#define ENOTEMPTY 39
#define ELOOP 40
#define ENAMETOOLONG 36
#define ENOEXEC 8

/* S_IF* mode bits (POSIX / Linux) */
#define S_IFMT 0170000
#define S_IFREG 0100000
#define S_IFDIR 0040000
#define S_IFCHR 0020000
#define S_IFBLK 0060000
#define S_IFIFO 0010000
#define S_IFLNK 0120000
#define S_IFSOCK 0140000

#define S_IRWXU 00700
#define S_IRUSR 00400
#define S_IWUSR 00200
#define S_IXUSR 00100
#define S_IRWXG 00070
#define S_IRGRP 00040
#define S_IRWXO 00007
#define S_IROTH 00004

/*
 * struct stat - LINUX x86-64 layout (asm-generic/stat.h as implemented on
 * x86_64).  musl's <sys/stat.h> on x86_64 is byte-for-byte this struct and
 * passes the user buffer straight into the kernel, so the kernel must fill
 * exactly this layout (144 bytes).
 *
 *   dev_t/ino_t/nlink_t/off_t/blksize_t/blkcnt_t = unsigned long/long (8)
 *   mode_t/uid_t/gid_t = unsigned int (4)
 */
typedef struct {
    uint64_t st_dev;       /*   0 */
    uint64_t st_ino;       /*   8 */
    uint64_t st_nlink;     /*  16 */
    uint32_t st_mode;      /*  24 */
    uint32_t st_uid;       /*  28 */
    uint32_t st_gid;       /*  32 */
    uint32_t __pad0;       /*  36 */
    uint64_t st_rdev;      /*  40 */
    int64_t  st_size;      /*  48 */
    int64_t  st_blksize;   /*  56 */
    int64_t  st_blocks;    /*  64 */
    int64_t  st_atim_sec;  /*  72 */
    int64_t  st_atim_nsec; /*  80 */
    int64_t  st_mtim_sec;  /*  88 */
    int64_t  st_mtim_nsec; /*  96 */
    int64_t  st_ctim_sec;  /* 104 */
    int64_t  st_ctim_nsec; /* 112 */
    int64_t  __unused[3];  /* 120..143 */
} linux_stat_t;            /* total = 144 bytes */

/*
 * Linux x86-64 struct dirent64  (used by getdents64)
 * d_off is opaque, d_reclen includes the entire struct + name + NUL.
 */
typedef struct {
    uint64_t d_ino;
    int64_t d_off;
    uint16_t d_reclen;
    uint8_t d_type;
    char d_name[1]; /* variable length, NUL-terminated */
} linux_dirent64_t;

#define DT_UNKNOWN 0
#define DT_FIFO 1
#define DT_CHR 2
#define DT_DIR 4
#define DT_BLK 6
#define DT_REG 8
#define DT_LNK 10
#define DT_SOCK 12

/*
 * Linux struct utsname  (used by uname syscall)
 */
typedef struct {
    char sysname[65];
    char nodename[65];
    char release[65];
    char version[65];
    char machine[65];
    char domainname[65];
} linux_utsname_t;

/*
 * Linux struct timespec / timeval
 */
typedef struct {
    int64_t tv_sec;
    int64_t tv_nsec;
} linux_timespec_t;
typedef struct {
    int64_t tv_sec;
    int64_t tv_usec;
} linux_timeval_t;

/* open flags - LINUX x86-64 values (asm-generic/fcntl.h)
 * musl passes these straight through.  O_ACCMODE = 0x3.
 */
#define O_ACCMODE 0x0003
#define O_RDONLY 0x0000
#define O_WRONLY 0x0001
#define O_RDWR 0x0002
#define O_CREAT 0x0040    /* 0100     */
#define O_EXCL 0x0080     /* 0200     */
#define O_NOCTTY 0x0100   /* 0400     */
#define O_TRUNC 0x0200    /* 01000    */
#define O_APPEND 0x0400   /* 02000    */
#define O_NONBLOCK 0x0800 /* 04000    */
#define O_DSYNC 0x1000    /* 010000   */
#define O_DIRECTORY 0x10000  /* 0200000  */
#define O_NOFOLLOW 0x20000   /* 0400000  */
#define O_CLOEXEC 0x80000    /* 02000000 */

/* AT_* constants for *at() syscalls */
#define AT_FDCWD -100
#define AT_REMOVEDIR 0x200
#define AT_SYMLINK_NOFOLLOW 0x100

/* arch_prctl constants */
#define ARCH_SET_GS 0x1001
#define ARCH_SET_FS 0x1002
#define ARCH_GET_FS 0x1003
#define ARCH_GET_GS 0x1004

/* clone flags */
#define CLONE_VM 0x00000100
#define CLONE_FS 0x00000200
#define CLONE_FILES 0x00000400
#define CLONE_SIGHAND 0x00000800
#define CLONE_VFORK 0x00004000
#define CLONE_THREAD 0x00010000
#define CLONE_SYSVSEM 0x00040000
#define CLONE_SETTLS 0x00080000
#define CLONE_PARENT_SETTID 0x00100000
#define CLONE_CHILD_CLEARTID 0x00200000
#define CLONE_DETACHED 0x00400000
#define CLONE_CHILD_SETTID 0x01000000

/* fcntl commands */
#define F_DUPFD 0
#define F_GETFD 1
#define F_SETFD 2
#define F_GETFL 3
#define F_SETFL 4
#define F_DUPFD_CLOEXEC 1030
#define FD_CLOEXEC 1
/* record-lock commands */
#define F_GETLK 5
#define F_SETLK 6
#define F_SETLKW 7
#define F_OFD_GETLK 36
#define F_OFD_SETLK 37
#define F_OFD_SETLKW 38
#define F_RDLCK 0
#define F_WRLCK 1
#define F_UNLCK 2

/* mmap flags */
#define PROT_NONE 0
#define PROT_READ 1
#define PROT_WRITE 2
#define PROT_EXEC 4
#define MAP_SHARED 0x01
#define MAP_PRIVATE 0x02
#define MAP_FIXED 0x10
#define MAP_ANONYMOUS 0x20
#define MAP_ANON MAP_ANONYMOUS
#define MAP_FAILED ((void *)-1)
#define MREMAP_MAYMOVE 1
#define MREMAP_FIXED 2

/* ioctl codes */
#define TCGETS 0x5401
#define TCSETS 0x5402
#define TCSETSW 0x5403
#define TCSETSF 0x5404
#define TCGETA 0x5405
#define TCSETA 0x5406
#define TCSETAW 0x5407
#define TCSETAF 0x5408
#define TIOCGWINSZ 0x5413
#define TIOCSWINSZ 0x5414
#define TIOCGPGRP 0x540F
#define TIOCSPGRP 0x5410
#define TIOCGSID 0x5429
#define TIOCSCTTY 0x540E

/* Signal numbers */
#define SIGHUP 1
#define SIGINT 2
#define SIGQUIT 3
#define SIGILL 4
#define SIGTRAP 5
#define SIGABRT 6
#define SIGBUS 7
#define SIGFPE 8
#define SIGKILL 9
#define SIGUSR1 10
#define SIGSEGV 11
#define SIGUSR2 12
#define SIGPIPE 13
#define SIGALRM 14
#define SIGTERM 15
#define SIGSTKFLT 16
#define SIGCHLD 17
#define SIGCONT 18
#define SIGSTOP 19
#define SIGTSTP 20
#define SIGTTIN 21
#define SIGTTOU 22
#define SIGWINCH 28
/* 64-signal space (real-time signals, musl reserves 32=SIGTIMER
 * for POSIX-timer SIGEV_THREAD and 33=SIGCANCEL for pthread_cancel). */
#define NSIG 65
#define SIGRTMIN 34
#define SIGRTMAX 64

#define SIG_DFL ((uint64_t)0)
#define SIG_IGN ((uint64_t)1)
#define SIG_ERR ((uint64_t)-1)

#define SIG_BLOCK 0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2

/* SA_* flags (Linux) */
#define SA_NOCLDSTOP 1
#define SA_NOCLDWAIT 2
#define SA_SIGINFO 4
#define SA_ONSTACK 0x08000000
#define SA_RESTART 0x10000000
#define SA_NODEFER 0x40000000
#define SA_RESETHAND 0x80000000
#define SA_RESTORER 0x04000000

/* struct sigaction (Linux x86-64 kernel layout)
 * Critical ABI note: the kernel-side struct is exactly 32 bytes -
 * sa_mask is one 64-bit word (sigsetsize=8).  A 40-byte struct here
 * made rt_sigaction's old-value copy write 8 bytes past musl's struct
 * on the caller's stack, clobbering the saved %rbx of any program
 * that called signal()/sigaction() (observed: ptytest's master fd
 * silently became 0 after signal()). */
typedef struct {
    uint64_t sa_handler; /* SIG_DFL / SIG_IGN / handler VA */
    uint64_t sa_flags;
    uint64_t sa_restorer;
    uint64_t sa_mask;    /* 64-bit signal mask (user bit N-1) */
} linux_sigaction_t;

/* struct itimerval */
typedef struct {
    linux_timeval_t it_interval;
    linux_timeval_t it_value;
} linux_itimerval_t;

/* struct pollfd */
typedef struct {
    int fd;
    int16_t events;
    int16_t revents;
} linux_pollfd_t;

#define POLLIN 0x0001
#define POLLPRI 0x0002
#define POLLOUT 0x0004
#define POLLERR 0x0008
#define POLLHUP 0x0010
#define POLLNVAL 0x0020

/* auxv entry types (Linux) */
#define AT_NULL 0
#define AT_PHDR 3
#define AT_PHENT 4
#define AT_PHNUM 5
#define AT_PAGESZ 6
#define AT_BASE 7
#define AT_FLAGS 8
#define AT_ENTRY 9
#define AT_UID 11
#define AT_EUID 12
#define AT_GID 13
#define AT_EGID 14
#define AT_HWCAP 16
#define AT_CLKTCK 17
#define AT_SECURE 23
#define AT_RANDOM 25
#define AT_EXECFN 31
#define AT_HWCAP2 26   /* second feature word (0 on x86-64 Linux) */

/* Kernel-side init */
void syscall_init(void);
void syscall_table_init(void);
