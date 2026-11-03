; Real-mode SIPI trampoline, linked at physical 0x8000 (vector 8).
default abs
section .ap_trampoline progbits alloc exec write align=4096

bits 16
global ap_trampoline_start
global ap_trampoline_end
global ap_trampoline_cr3
global ap_trampoline_stack
global ap_trampoline_processor
extern KiApplicationProcessorEntry

ap_trampoline_start:
    cli
    cld
    xor     ax, ax
    mov     ds, ax
    mov     es, ax
    mov     ss, ax
    mov     sp, 0x7ff0
    lgdt    [cs:ap_trampoline_gdtr - ap_trampoline_start]

    mov     eax, [dword 0x8000 + ap_trampoline_cr3 - ap_trampoline_start]
    mov     cr3, eax
    mov     eax, cr4
    or      eax, 1 << 5                 ; PAE
    mov     cr4, eax

    mov     ecx, 0xC0000080             ; IA32_EFER
    rdmsr
    or      eax, 1 << 8                 ; LME
    wrmsr

    mov     eax, cr0
    or      eax, 1                       ; protected mode
    mov     cr0, eax
    jmp     dword 0x08:(0x8000 + ap_trampoline_protected - ap_trampoline_start)

bits 32
ap_trampoline_protected:
    mov     ax, 0x10
    mov     ds, ax
    mov     es, ax
    mov     ss, ax
    mov     eax, cr0
    or      eax, 1 << 31                ; paging -> long mode
    mov     cr0, eax
    jmp     dword 0x18:(0x8000 + ap_trampoline_long - ap_trampoline_start)

bits 64
ap_trampoline_long:
    mov     ax, 0x10
    mov     ds, ax
    mov     es, ax
    mov     ss, ax
    mov     rsp, [abs 0x8000 + ap_trampoline_stack - ap_trampoline_start]
    and     rsp, -16
    mov     edi, [abs 0x8000 + ap_trampoline_processor - ap_trampoline_start]
    mov     rax, KiApplicationProcessorEntry
    call    rax
.ap_returned:
    cli
    hlt
    jmp     .ap_returned

align 8
ap_trampoline_gdt:
    dq 0
    dq 0x00CF9A000000FFFF                ; 32-bit protected-mode code
    dq 0x00CF92000000FFFF                ; data
    dq 0x00AF9A000000FFFF                ; 64-bit code
ap_trampoline_gdtr:
    dw ap_trampoline_gdtr - ap_trampoline_gdt - 1
    dd 0x8000 + ap_trampoline_gdt - ap_trampoline_start

align 8
ap_trampoline_cr3:       dd 0
ap_trampoline_processor: dd 0
ap_trampoline_stack:     dq 0

; The kernel reserves all memory below 1 MiB. Keep this trampoline within the
; single SIPI page, padded so the copied blob has a fixed, auditable footprint.
times 4096 - ($ - ap_trampoline_start) db 0
ap_trampoline_end:
