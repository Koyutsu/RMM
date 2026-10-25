#pragma once
/*
 * version.h - single source of truth for the kernel version strings.
 *
 * Every version string the kernel owns comes from here: uname(2),
 * the boot banner and /proc/version.  Bump the
 * release here and the whole system reports the same version.
 */

#define RMM_KERNEL_NAME "RMM"
#define RMM_VERSION     "0.9.3"
#define RMM_RELEASE     RMM_VERSION "-rmm"
