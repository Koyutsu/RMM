/*
 * syscall_entry.S - `syscall` instruction handler for RMM x86-64
 *
 * CPU state on entry
 *   rcx = saved user rip   (sysretq restores rip from rcx)
 *   r11 = saved user rflags (sysretq restores rflags from r11)
 *   rsp = user stack        (we switch to kernel stack)
 *   IF  = 0 (cleared by SFMASK)
 *
 * Linux syscall register convention
 *   rax=nr  rdi=a1  rsi=a2  rdx=a3  r10=a4  r8=a5  r9=a6
 *
 * Kernel stack frame layout (rsp->slot 0 after all pushes)
 *   off   0   r9   (a6)
 *   off   8   r8   (a5)
 *   off  16   r10  (a4)   <- original r10; saved before stack switch
 *   off  24   rdx  (a3)
 *   off  32   rsi  (a2)
 *   off  40   rdi  (a1)
 *   off  48   rax  (nr / return value)
 *   off  56   r15
 *   off  64   r14
 *   off  72   r13
 *   off  80   r12
 *   off  88   rbx
 *   off  96   rbp
 *   off 104   r11  (user rflags, sysretq reads this)
 *   off 112   rcx  (user rip, sysretq reads this)
 *   off 120   user_rsp
 * 128 bytes total (syscall_ksp must be 16-byte aligned)
 *
 * Dispatcher signature
 *   int64_t syscall_dispatch_c(nr, a1, a2, a3, a4, a5, a6, frame*)
 *   SysV:  rdi rsi rdx rcx r8  r9  [rsp+8] [rsp+16]  (after call insn)
 */

    .text
    .align 16
    .globl syscall_entry
    .type  syscall_entry, @function

syscall_entry:
    /* Stack switch *
     * r10 is caller-saved and we save it into the frame, so we can use it
     * as scratch here without losing any user state that matters across the
     * syscall boundary.  Save the user a4 (r10) and user rsp first.
     */
    mov    %r10, syscall_saved_r10(%rip)  /* stash a4 before clobbering r10 */
    mov    %rsp, syscall_user_rsp(%rip)   /* stash user rsp                  */
    mov    syscall_ksp(%rip), %rsp        /* switch to kernel stack           */

    /* Save user context */
    mov    syscall_user_rsp(%rip), %r10
    push   %r10                    /* off 120: user_rsp                     */
    push   %rcx                    /* off 112: user_rip  (rcx from cpu)     */
    push   %r11                    /* off 104: user_rflags (r11 from cpu)   */
    push   %rbp                    /* off  96                               */
    push   %rbx                    /* off  88                               */
    push   %r12                    /* off  80                               */
    push   %r13                    /* off  72                               */
    push   %r14                    /* off  64                               */
    push   %r15                    /* off  56                               */
    push   %rax                    /* off  48: nr                           */
    push   %rdi                    /* off  40: a1                           */
    push   %rsi                    /* off  32: a2                           */
    push   %rdx                    /* off  24: a3                           */
    mov    syscall_saved_r10(%rip), %r10
    push   %r10                    /* off  16: a4 (original r10)            */
    push   %r8                     /* off   8: a5                           */
    push   %r9                     /* off   0: a6                           */

    /* Call dispatcher *
     * SysV args 1-6 in registers, args 7 (a6) and 8 (frame*) on stack.
     * Stack before call: push 8th then 7th so callee sees [rsp+8]=a6,
     * [rsp+16]=frame after call pushes retaddr at [rsp+0].
     *
     * Alignment: 128-byte frame.  We push 3 more qwords (a6, frame, pad)
     * = 24 bytes -> 152 bytes.  call pushes retaddr -> 160 bytes.
     * 160 % 16 = 0 OK
     */
    mov    48(%rsp), %rdi          /* nr  */
    mov    40(%rsp), %rsi          /* a1  */
    mov    32(%rsp), %rdx          /* a2  */
    mov    24(%rsp), %rcx          /* a3  */
    mov    16(%rsp), %r8           /* a4  */
    mov     8(%rsp), %r9           /* a5  */
    mov    %rsp,     %r10          /* r10 = frame ptr                        */
    mov    0(%rsp),  %r11          /* r11 = a6                               */
    sub    $8, %rsp                /* alignment pad                          */
    push   %r10                    /* 8th arg: frame*                        */
    push   %r11                    /* 7th arg: a6                            */
    call   syscall_dispatch_c
    add    $24, %rsp               /* pop a6, frame*, pad                    */

    /* Return */
    mov    %rax, 48(%rsp)          /* patch saved-rax slot with return value */
    pop    %r9                     /* a6   */
    pop    %r8                     /* a5   */
    pop    %r10                    /* a4   */
    pop    %rdx                    /* a3   */
    pop    %rsi                    /* a2   */
    pop    %rdi                    /* a1   */
    pop    %rax                    /* return value */
    pop    %r15
    pop    %r14
    pop    %r13
    pop    %r12
    pop    %rbx
    pop    %rbp
    pop    %r11                    /* user rflags -> sysretq reads r11       */
    pop    %rcx                    /* user rip    -> sysretq reads rcx       */
    pop    %rsp                    /* restore user stack                    */
    sysretq

    .size syscall_entry, .-syscall_entry

/* Per-CPU scratch (single CPU, no SMP)
 * Defined here so the assembler emits them in the same object file,
 * syscall_table.c declares them extern.
 */
    .data
    .align 8
    .globl syscall_user_rsp
    .globl syscall_ksp
    .globl syscall_saved_r10

syscall_user_rsp:  .quad 0   /* user rsp saved during stack switch   */
syscall_ksp:       .quad 0   /* kernel stack top, updated each iretq */
syscall_saved_r10: .quad 0   /* stash for r10=a4 before clobbering   */
/*
 * IA-32 syscall entry (int 0x80 from a 32-bit compat task)
 *
 * Linux i386 ABI: eax = syscall number, args in ebx/ecx/edx/esi/edi/ebp,
 * return value in eax (negative errno).  The CPU pushed a 5-slot iret
 * frame (RIP, CS, RFLAGS, RSP, SS) - CS is GDT_USER_CODE32|3 here.
 *
 * The dispatcher:
 *   ia32_dispatch_c(nr, a1, a2, a3, a4, a5, a6, frame*)
 *     rdi = nr (eax)  rsi = a1 (ebx)  rdx = a2 (ecx)
 *     rcx = a3 (edx)  r8  = a4 (esi)  r9  = a5 (edi)
 *     stack: [a6 (ebp), frame*]
 *
 * Return path is IRETQ (sysretq cannot load a 32-bit CS): after popping
 * the 16-register frame, RSP lands exactly on the CPU's iret frame, so
 * a bare iretq restores RIP/CS/RFLAGS/RSP/SS and re-enters compat mode.
 */
    .text
    .align 16
    .globl ia32_syscall_entry
    .type  ia32_syscall_entry, @function
ia32_syscall_entry:
    /* CPU iret frame at entry (rsp = B): [B+0]=RIP [B+8]=CS [B+16]=RFLAGS
     * [B+24]=RSP [B+32]=SS.  Slots are pushed HIGH-FIRST so that after
     * all 16 pushes slot N sits at [rsp + (120-N)] - i.e. the offsets
     * below are relative to the final frame pointer, matching the
     * dispatcher reads (nr@48, a1@88, a2@16, a3@24, a4@32, a5@40,
     * a6@96) exactly like the `syscall` path in syscall_entry.S.      */
    mov    24(%rsp), %r10          /* user RSP   (rsp==B+0 here)         */
    push   %r10                    /* slot 120: user RSP                 */
    mov    8(%rsp),  %r11          /* user RIP    (rsp==B-8 -> B+0=rsp+8)  */
    push   %r11                    /* slot 112: user RIP                 */
    mov    32(%rsp), %r11          /* user RFLAGS (rsp==B-16 -> B+16=rsp+32)*/
    push   %r11                    /* slot 104: user RFLAGS              */
    push   %rbp                    /* slot  96: ia32 a6                  */
    push   %rbx                    /* slot  88: ia32 a1                  */
    push   %r12                    /* slot  80                           */
    push   %r13                    /* slot  72                           */
    push   %r14                    /* slot  64                           */
    push   %r15                    /* slot  56                           */
    push   %rax                    /* slot  48: nr                       */
    push   %rdi                    /* slot  40: ia32 a5                  */
    push   %rsi                    /* slot  32: ia32 a4                  */
    push   %rdx                    /* slot  24: ia32 a3                  */
    mov    %ecx,    %r10d          /* ecx is ia32 a2 - stash in r10      */
    push   %r10                    /* slot  16: ia32 a2                  */
    push   %r8                     /* slot   8                           */
    push   %r9                     /* slot   0                           */

    /* dispatcher args */
    mov    48(%rsp),  %rdi         /* nr   (eax)                         */
    mov    88(%rsp),  %rsi         /* a1   (ebx)                         */
    mov    16(%rsp),  %rdx         /* a2   (ecx)                         */
    mov    24(%rsp),  %rcx         /* a3   (edx)                         */
    mov    32(%rsp),  %r8          /* a4   (esi)                         */
    mov    40(%rsp),  %r9          /* a5   (edi)                         */
    mov    96(%rsp),  %r11         /* a6   (ebp)                         */
    mov    %rsp,      %r10         /* frame*                              */
    sub    $8, %rsp                /* alignment pad                       */
    push   %r10                    /* 8th arg: frame*                     */
    push   %r11                    /* 7th arg: a6                         */
    call   ia32_dispatch_c
    add    $24, %rsp               /* pop a6, frame*, pad                 */

    /* 32-bit return value, zero-extended into the full slot */
    mov    %eax, 48(%rsp)
    mov    48(%rsp), %rax
    shl    $32, %rax
    shr    $32, %rax
    mov    %rax, 48(%rsp)

    pop    %r9
    pop    %r8
    pop    %r10                    /* ia32 a2 (ecx), restored into r10   */
    pop    %rdx
    pop    %rsi
    pop    %rdi
    pop    %rax
    pop    %r15
    pop    %r14
    pop    %r13
    pop    %r12
    pop    %rbx
    pop    %rbp
    pop    %r11
    pop    %rcx
    add    $8, %rsp                /* discard the user-RSP slot, the CPU's
                                    * iret frame (RIP/CS/RFLAGS/RSP/SS) sits
                                    * right below and carries user RSP     */
    iretq                          /* RIP/CS/RFLAGS/RSP/SS -> compat mode  */

.size ia32_syscall_entry, .-ia32_syscall_entry
