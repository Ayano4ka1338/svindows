; ============================================================================
; arch/x86_64/SyscallEntry.asm - the SYSCALL fast-path entry and the ring-3
; transition.
;
; SYSCALL does not switch stacks, so the entry must do it by hand: swapgs to
; reach the per-CPU block (KPCR) via GS, stash the user RSP, and load the
; current thread's kernel stack. The user syscall ABI matches Windows x64: the
; caller's first argument (RCX in the Windows calling convention) is moved to
; R10 by the ntdll stub, since SYSCALL clobbers RCX (and R11):
;
;   RAX = service number
;   R10, RDX, R8, R9 = arguments 1..4
;   arguments 5.. are on the user stack at [user_rsp + 0x28] (past the stub's
;     return address and the 4-slot home space), exactly as in a Windows call
;   return value in RAX
;
; The entry captures four register arguments; the C dispatcher probes and
; captures twelve more stack arguments, supporting up to sixteen parameters.
;
; KPCR layout (see ke/Syscall.c): +0 = user RSP scratch, +8 = kernel RSP.
; ============================================================================
bits 64
section .text

extern KiSystemServiceDispatch
extern KiRejectUserReturn

global KiSystemCallEntry
; KTHREAD field offsets for the callback redirect (see Ke.h; _Static_asserts in
; ke/Syscall.c fail the build when this table drifts). KPCR+16 holds
; CurrentThread.
%define KTH_USER_RIP      0xAC0
%define KTH_USER_RSP      0xAC8
%define KTH_USER_RFLAGS   0xAD0
%define KTH_CB_RIP        0xAD8
%define KTH_CB_RSP        0xAE0
%define KTH_CB_FLAGS      0xAE8
%define KTH_REDIRECT      0xB29
%define KTH_LINUX_THREAD  0x11A8

KiSystemCallEntry:
    swapgs                        ; GS -> kernel KPCR
    mov     [gs:0], rsp           ; momentary scratch for the user RSP
    mov     rsp, [gs:8]           ; load this thread's kernel stack top

    ; Save the user RSP on THIS thread's kernel stack, not the shared KPCR slot:
    ; a syscall may block and switch to another thread whose own syscall entry
    ; would clobber the per-CPU scratch. Keeping it on the kernel stack makes it
    ; per-thread and safe across a block.
    push    qword [gs:0]          ; user RSP
    push    rcx                   ; user RIP  (SYSCALL saved it in RCX)
    push    r11                   ; user RFLAGS (SYSCALL saved it in R11)

    ; RDI and RSI are non-volatile in the Windows calling convention but scratch
    ; in the kernel's SysV ABI, so preserve the caller's values. (RBX, RBP, and
    ; R12-R15 are preserved automatically by the C dispatcher.)
    push    rdi
    push    rsi

    ; clone children must resume with the parent's preserved user registers.
    ; Save these for Linux clone(56) and fork(57). The Linux context
    ; begins with LINUX_CLONE_REGISTERS (asserted in linux/Thread.c).
    cmp     rax, 56
    je      .capture_clone
    cmp     rax, 57
    jne     .skip_clone_capture
.capture_clone:
    mov     r11, [gs:16]
    mov     r11, [r11+KTH_LINUX_THREAD]
    test    r11, r11
    jz      .skip_clone_capture
    mov     [r11+0x00], rbx
    mov     [r11+0x08], rbp
    mov     [r11+0x10], r12
    mov     [r11+0x18], r13
    mov     [r11+0x20], r14
    mov     [r11+0x28], r15
    mov     [r11+0x30], rdi
    mov     [r11+0x38], rsi
    mov     [r11+0x40], rdx
    mov     [r11+0x48], r8
    mov     [r11+0x50], r9
    mov     [r11+0x58], r10
.skip_clone_capture:

    ; Mirror the user trap frame into the KTHREAD so in-service callbacks can
    ; redirect the return path. R11 and RDI are scratch here (their user values
    ; were just pushed); RAX must survive: it carries the service number.
    mov     r11, [gs:16]          ; CurrentThread
    mov     [r11+KTH_USER_RIP], rcx
    mov     rdi, [rsp+0x10]       ; user RFLAGS (pushed above)
    mov     [r11+KTH_USER_RFLAGS], rdi
    mov     rdi, [rsp+0x20]       ; user RSP (pushed above)
    mov     [r11+KTH_USER_RSP], rdi

    ; Keep the Windows register arguments plus the saved user RDI/RSI. The
    ; dispatcher selects Windows or Linux register order from the process ABI.
    ; Hand it user RSP for Windows stack arguments.
    ; (probing each, since a low-argument call may leave RSP near the top of the
    ; stack where [RSP+0x28] is unmapped). Interrupts are masked (SFMASK clears
    ; IF), so [gs:0] is a stable copy of the user RSP.
    sub     rsp, 80               ; 6 args + 4 Linux-preserved registers
    mov     [rsp+0x00], r10       ; arg1
    mov     [rsp+0x08], rdx       ; arg2
    mov     [rsp+0x10], r8        ; arg3
    mov     [rsp+0x18], r9        ; arg4
    mov     [rsp+0x30], rdx
    mov     [rsp+0x38], r8
    mov     [rsp+0x40], r9
    mov     [rsp+0x48], r10
    mov     rdi, [rsp+0x58]       ; saved user RDI
    mov     [rsp+0x20], rdi
    mov     rdi, [rsp+0x50]       ; saved user RSI
    mov     [rsp+0x28], rdi

    mov     rdi, rax              ; num       -> arg1
    mov     rsi, rsp              ; reg args  -> arg2 (pointer)
    mov     rdx, [gs:0]           ; user RSP  -> arg3

    ; Realign to 16: 5 pushes (40) + 80 = 120, so drop 8 more.
    sub     rsp, 8
    call    KiSystemServiceDispatch
    add     rsp, 8
    mov     rdx, [rsp+0x30]
    mov     r8,  [rsp+0x38]
    mov     r9,  [rsp+0x40]
    mov     r10, [rsp+0x48]
    add     rsp, 80               ; drop arguments and saved registers
    ; return value already in RAX for the user

    pop     rsi                   ; restore caller's RSI
    pop     rdi                   ; restore caller's RDI
    ; execve replaces the image while keeping this kernel thread. Enter its
    ; new entry point and stack after returning from the dispatcher.
    mov     r11, [gs:16]
    mov     r11, [r11+KTH_LINUX_THREAD]
    test    r11, r11
    jz      .check_callback
    cmp     byte [r11+113], 0
    je      .check_callback
    mov     r10, r11              ; keep the Linux thread context
    mov     byte [r11+113], 0
    cmp     byte [r10+114], 0     ; WindowsExec: execve loaded a PE image
    jne     .windows_exec
    mov     r11, [gs:16]
    mov     rcx, [r11+KTH_USER_RIP]
    mov     [rsp+8], rcx
    mov     rcx, [r11+KTH_USER_RSP]
    mov     [rsp+16], rcx
    xor     eax, eax
    xor     edx, edx
    jmp     .check_callback

.windows_exec:
    ; A PE image was exec'd. SYSRET forces RCX = RIP, so it cannot deliver the
    ; Windows first argument; build an IRETQ frame instead (as KiEnterUserMode
    ; does) and pass the LDR init block in RCX. The stack contract mirrors
    ; KiEnterUserMode: RSP is 16-aligned minus 8 with mapped home space.
    mov     r11, [gs:16]
    mov     rcx, [r11+KTH_USER_RIP]   ; PE bootstrap entry
    mov     rdx, [r11+KTH_USER_RSP]   ; PE stack top
    mov     r9,  [r10+120]            ; entry argument
    mov     r8, rcx
    shr     r8, 47
    jnz     .bad_normal_return
    mov     r8, rdx
    shr     r8, 47
    jnz     .bad_normal_return
    and     rdx, -16
    sub     rdx, 0x28
    mov     qword [rdx], 0            ; stray `ret` from the entry faults at 0
    add     rsp, 24                   ; drop this frame's RFLAGS/RIP/RSP
    mov     eax, 0x1B                 ; user data selectors
    mov     ds, ax
    mov     es, ax
    push    0x1B                      ; SS
    push    rdx                       ; RSP
    push    0x202                     ; RFLAGS: IF=1, reserved bit set
    push    0x23                      ; CS
    push    rcx                       ; RIP
    mov     rcx, r9                   ; entry argument (Win64 first argument)
    xor     eax, eax
    xor     edx, edx
    swapgs
    iretq
.check_callback:
    ; A kernel->user callback armed a one-shot redirect: send this sysret into
    ; the callback trampoline instead of the original caller. R11 is scratch
    ; until the normal path reloads the user RFLAGS into it below, and RAX (the
    ; service's return value) passes through untouched.
    mov     r11, [gs:16]          ; CurrentThread
    cmp     byte [r11+KTH_REDIRECT], 1
    jne     .return_normal
    mov     byte [r11+KTH_REDIRECT], 0
    mov     rcx, [r11+KTH_CB_RIP]
    shr     rcx, 47
    jnz     .bad_callback_return
    mov     rcx, [r11+KTH_CB_RSP]
    shr     rcx, 47
    jnz     .bad_callback_return
    add     rsp, 24               ; drop this frame's RFLAGS/RIP/RSP
    mov     rcx, [r11+KTH_CB_RIP]
    mov     rdx, [r11+KTH_CB_RSP]
    mov     r11, [r11+KTH_CB_FLAGS]
    and     r11d, 0x240fd7        ; user flags only: clear IOPL/NT/VM/RF
    or      r11d, 0x202           ; reserved bit and IF must be set
    mov     rsp, rdx
    swapgs                        ; GS -> user
    o64 sysret
.return_normal:
    ; SYSRET with a non-user or non-canonical RCX may #GP in ring 0. Reject
    ; the frame while still on the kernel stack, before swapping GS or RSP.
    mov     rcx, [rsp+8]
    shr     rcx, 47
    jnz     .bad_normal_return
    mov     rcx, [rsp+16]
    shr     rcx, 47
    jnz     .bad_normal_return
    pop     r11                   ; user RFLAGS
    pop     rcx                   ; user RIP
    pop     rsp                   ; user RSP (from this thread's kernel stack)
    and     r11d, 0x240fd7
    or      r11d, 0x202
    swapgs                        ; GS -> user
    o64 sysret                    ; back to ring 3 (RIP=RCX, RFLAGS=R11)

.bad_callback_return:
    mov     rdi, [r11+KTH_CB_RIP]
    mov     rsi, [r11+KTH_CB_RSP]
    jmp     .reject_return
.bad_normal_return:
    mov     rdi, [rsp+8]
    mov     rsi, [rsp+16]
.reject_return:
    sub     rsp, 8                ; align for the SysV C call
    call    KiRejectUserReturn    ; terminates the current thread
    ud2

; void KiEnterUserMode(UINT64 entry /*rdi*/, UINT64 user_stack /*rsi*/,
;                      UINT64 arg /*rdx*/);
;   Builds an IRETQ frame and drops to ring 3, passing `arg` in RCX (the Windows
;   first-argument register). Never returns.
global KiEnterUserMode
KiEnterUserMode:
    mov     ax, 0x1B              ; user data selector (RPL 3)
    mov     ds, ax
    mov     es, ax

    mov     rcx, rdx              ; entry-point argument (Windows: first arg = RCX)

    ; The entry is treated like a called function. Present RSP % 16 == 8 (as if
    ; a return address had been pushed onto a 16-aligned stack) AND leave the
    ; 32-byte "home"/shadow space above RSP mapped: a real Win64 prologue homes
    ; its non-volatile registers into [rsp+8]..[rsp+0x20] (the caller's shadow
    ; space) before allocating its own frame, so those slots must be writable.
    ; Reserving 0x28 (0x20 home + one return slot) keeps RSP % 16 == 8 and keeps
    ; the whole home space inside the mapped stack.
    and     rsi, -16
    sub     rsi, 0x28
    mov     qword [rsi], 0        ; return slot: a stray `ret` from the entry
                                  ; faults at RIP=0 (caught) instead of running
                                  ; off into mapped code. The CRT exits via
                                  ; ExitProcess and never returns here.

    ; Switch the active GS base to this thread's TEB (held in KERNEL_GS_BASE by
    ; the scheduler) before entering ring 3.
    swapgs

    push    0x1B                  ; SS  = user data | 3
    push    rsi                   ; RSP = user stack
    push    0x202                 ; RFLAGS: IF=1, reserved bit set
    push    0x23                  ; CS  = user code | 3
    push    rdi                   ; RIP = user entry
    iretq
