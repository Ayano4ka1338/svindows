; Verify every Win64 nonvolatile integer/SIMD register across a syscall. The
; blocking case goes through NtDelayExecution and therefore a context switch.
bits 64
default rel

section .text
global abi_check_registers
extern __imp_NtDelayExecution
extern __imp_NtAbiShadowProbe

%macro SAVE_XMM 2
    movdqu [rsp + %2], xmm%1
%endmacro
%macro RESTORE_XMM 2
    movdqu xmm%1, [rsp + %2]
%endmacro
%macro CHECK_GPR 2
    mov rax, %2
    cmp %1, rax
    jne .failed
%endmacro
%macro CHECK_XMM 1
    pcmpeqd xmm0, xmm0
    pcmpeqb xmm0, xmm%1
    pmovmskb eax, xmm0
    cmp eax, 0xffff
    jne .failed
%endmacro

abi_check_registers:
    ; Entry RSP is 8 mod 16. 0x148 bytes makes it aligned at both calls and
    ; provides their mandatory 32-byte shadow space.
    sub rsp, 0x148
    mov [rsp + 0x20], ecx
    mov [rsp + 0x30], rbx
    mov [rsp + 0x38], rbp
    mov [rsp + 0x40], rdi
    mov [rsp + 0x48], rsi
    mov [rsp + 0x50], r12
    mov [rsp + 0x58], r13
    mov [rsp + 0x60], r14
    mov [rsp + 0x68], r15
    SAVE_XMM 6,  0x80
    SAVE_XMM 7,  0x90
    SAVE_XMM 8,  0xa0
    SAVE_XMM 9,  0xb0
    SAVE_XMM 10, 0xc0
    SAVE_XMM 11, 0xd0
    SAVE_XMM 12, 0xe0
    SAVE_XMM 13, 0xf0
    SAVE_XMM 14, 0x100
    SAVE_XMM 15, 0x110

    mov rbx, 0x1122334455667701
    mov rbp, 0x1122334455667702
    mov rdi, 0x1122334455667703
    mov rsi, 0x1122334455667704
    mov r12, 0x1122334455667705
    mov r13, 0x1122334455667706
    mov r14, 0x1122334455667707
    mov r15, 0x1122334455667708
    pcmpeqd xmm6, xmm6
    movdqa xmm7, xmm6
    movdqa xmm8, xmm6
    movdqa xmm9, xmm6
    movdqa xmm10, xmm6
    movdqa xmm11, xmm6
    movdqa xmm12, xmm6
    movdqa xmm13, xmm6
    movdqa xmm14, xmm6
    movdqa xmm15, xmm6

    cmp dword [rsp + 0x20], 0
    jne .blocking
    call [rel __imp_NtAbiShadowProbe]
    jmp .check
.blocking:
    mov qword [rsp + 0x28], -200000 ; relative 20 ms, in 100 ns units
    xor ecx, ecx
    lea rdx, [rsp + 0x28]
    call [rel __imp_NtDelayExecution]
.check:
    CHECK_GPR rbx, 0x1122334455667701
    CHECK_GPR rbp, 0x1122334455667702
    CHECK_GPR rdi, 0x1122334455667703
    CHECK_GPR rsi, 0x1122334455667704
    CHECK_GPR r12, 0x1122334455667705
    CHECK_GPR r13, 0x1122334455667706
    CHECK_GPR r14, 0x1122334455667707
    CHECK_GPR r15, 0x1122334455667708
    CHECK_XMM 6
    CHECK_XMM 7
    CHECK_XMM 8
    CHECK_XMM 9
    CHECK_XMM 10
    CHECK_XMM 11
    CHECK_XMM 12
    CHECK_XMM 13
    CHECK_XMM 14
    CHECK_XMM 15
    mov dword [rsp + 0x24], 1
    jmp .restore
.failed:
    mov dword [rsp + 0x24], 0
.restore:
    RESTORE_XMM 6,  0x80
    RESTORE_XMM 7,  0x90
    RESTORE_XMM 8,  0xa0
    RESTORE_XMM 9,  0xb0
    RESTORE_XMM 10, 0xc0
    RESTORE_XMM 11, 0xd0
    RESTORE_XMM 12, 0xe0
    RESTORE_XMM 13, 0xf0
    RESTORE_XMM 14, 0x100
    RESTORE_XMM 15, 0x110
    mov rbx, [rsp + 0x30]
    mov rbp, [rsp + 0x38]
    mov rdi, [rsp + 0x40]
    mov rsi, [rsp + 0x48]
    mov r12, [rsp + 0x50]
    mov r13, [rsp + 0x58]
    mov r14, [rsp + 0x60]
    mov r15, [rsp + 0x68]
    mov eax, [rsp + 0x24]
    add rsp, 0x148
    ret
