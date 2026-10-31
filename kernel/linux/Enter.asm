; Linux ELF starts with an exact SysV initial stack, not a Windows call frame.
bits 64
section .text
global KiEnterLinuxUserMode
KiEnterLinuxUserMode:
    mov ax, 0x1b
    mov ds, ax
    mov es, ax
    and rsi, -16
    swapgs
    push 0x1b
    push rsi
    push 0x202
    push 0x23
    push rdi
    xor edx, edx              ; no dynamic linker fini callback
    iretq

; Resume a clone child at the instruction after the parent's SYSCALL.
; Arguments: entry, child RSP, saved RFLAGS, pointer to saved registers.
global KiEnterLinuxCloneUserMode
KiEnterLinuxCloneUserMode:
    mov ax, 0x1b
    mov ds, ax
    mov es, ax
    swapgs
    push 0x1b
    push rsi
    push rdx
    push 0x23
    push rdi
    mov rax, rcx
    mov rbx, [rax+0x00]
    mov rbp, [rax+0x08]
    mov r12, [rax+0x10]
    mov r13, [rax+0x18]
    mov r14, [rax+0x20]
    mov r15, [rax+0x28]
    mov rdi, [rax+0x30]
    mov rsi, [rax+0x38]
    mov rdx, [rax+0x40]
    mov r8,  [rax+0x48]
    mov r9,  [rax+0x50]
    mov r10, [rax+0x58]
    xor eax, eax              ; clone() returns zero to the child
    iretq
