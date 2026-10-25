#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "fs/vfs.h"

/*
 *  locks - flock(2) + POSIX/OFD byte-range record locks
 *
 *  flock(2):
 *    LOCK_SH / LOCK_EX / LOCK_UN, LOCK_NB.  A flock is owned by the open
 *    File description - modelled here as the address of the fd slot in the
 *    live fd table (fork-copied slots therefore do not inherit flocks,
 *    which matches POSIX: locks are not inherited across fork).
 *
 *  fcntl(2) record locks:
 *    F_GETLK / F_SETLK / F_SETLKW, owner is the process (tgid);
 *      released when any fd to the file is closed by the process.
 *    F_OFD_GETLK / F_OFD_SETLK /
 *      F_OFD_SETLKW (36/37/38) - owner is the fd slot (OFD).
 *
 *  struct flock (x86-64, musl layout):
 *    i16 l_type @0, i16 l_whence @2, i64 l_start @8, i64 l_len @16,
 *    i32 l_pid @24 (+4 pad)
 */

#define LK_LOCK_SH 1
#define LK_LOCK_EX 2
#define LK_LOCK_NB 4
#define LK_LOCK_UN 8

/* fcntl lock commands (Linux) */
#define LK_F_GETLK     5
#define LK_F_SETLK     6
#define LK_F_SETLKW    7
#define LK_F_OFD_GETLK  36
#define LK_F_OFD_SETLK  37
#define LK_F_OFD_SETLKW 38

/* l_type values (Linux) */
#define LK_RDLCK 0
#define LK_WRLCK 1
#define LK_UNLCK 2

/* flock(2) - returns 0 or -errno.  May park for blocking mode. */
int64_t sys_flock(int fd, int op);

/* fcntl(2) record-lock commands, returns 0/-1(semantics of fcntl) or
 * -errno, F_GETLK overwrites *UF with the conflicting lock (l_pid set). */
int64_t sys_fcntl_lock(int fd, int cmd, uint64_t uarg);

/* Release every lock tied to a closing fd (slot), called from vfs_close
 * before the slot is cleared.  Covers flock (slot owner) and OFD record
 * locks, POSIX record locks use the process-wide release below. */
void locks_release_fd(int fd);

/* Release every lock owned by a process, called from proc_exit. */
void locks_release_process(int tgid);
