#pragma once
#include <stdint.h>

/*
 * GDT - Global Descriptor Table
 *
 *   0x00  Null descriptor
 *   0x08  Kernel code  (ring 0, 64-bit)
 *   0x10  Kernel data  (ring 0)
 *   0x18  User data    (ring 3)          <- data before code (required for sysretq)
 *   0x20  User code    (ring 3, 64-bit)  <- code at +16 from data
 *   0x28  TSS low      (16 bytes total)
 *   0x30  TSS high
 *
 * Why user_data before user_code?
 *   sysretq sets CS = STAR[63:48]+16 and SS = STAR[63:48]+8.
 *   With STAR[63:48] = GDT_KERNEL_DATA = 0x10:
 *     SS = 0x10+8  = 0x18 = user data  OK
 *     CS = 0x10+16 = 0x20 = user code  OK
 *   (DPL=3 is OR-ed in by the CPU automatically.)
 *
 * Segment selectors (index * 8)
 */
#define GDT_KERNEL_CODE  0x08
#define GDT_KERNEL_DATA  0x10
#define GDT_USER_DATA    0x18   /* data comes first, required by sysretq */
#define GDT_USER_CODE    0x20
#define GDT_USER_CODE32  0x38   /* IA-32 compat user code (ring 3) */
#define GDT_TSS          0x28

void gdt_init(void);

/* Update RSP0 in the TSS (kernel stack for ring-3 -> ring-0 transitions) */
void gdt_set_tss_rsp0(uint64_t rsp0);