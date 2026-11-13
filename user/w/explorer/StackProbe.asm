; Microsoft x64 compiler stack-allocation probe. RAX is the requested frame
; size; RAX and RCX are preserved and every crossed page is touched before the
; caller adjusts RSP. Stack faults therefore occur at the guard boundary.
bits 64
section .text align=16
global __chkstk
__chkstk:
    push rcx
    push rax
.prologue_end:
    lea rcx, [rsp+24]
    cmp rax, 4096
    jb .tail
.page:
    sub rcx, 4096
    test dword [rcx], eax
    sub rax, 4096
    cmp rax, 4096
    jae .page
.tail:
    sub rcx, rax
    test dword [rcx], eax
    pop rax
    pop rcx
    ret
.end:

section .xdata rdata align=4
.unwind:
    db 1, __chkstk.prologue_end-__chkstk, 1, 0
    db __chkstk.prologue_end-__chkstk, 0x12 ; UWOP_ALLOC_SMALL, 16 bytes
    dw 0

section .pdata rdata align=4
    dd __chkstk wrt ..imagebase
    dd __chkstk.end wrt ..imagebase
    dd .unwind wrt ..imagebase
