bits 64
default rel

section .text
global FetchCpuInfo

; void FetchCpuInfo(char vendor[13], char brand[49]);
; Windows x64 ABI: RCX=vendor, RDX=brand.  RBX is nonvolatile.
FetchCpuInfo:
    push rbx
    mov r10, rcx
    mov r11, rdx

    xor eax, eax
    cpuid
    mov [r10 + 0], ebx
    mov [r10 + 4], edx
    mov [r10 + 8], ecx
    mov byte [r10 + 12], 0

    mov eax, 0x80000000
    cpuid
    cmp eax, 0x80000004
    jb .no_brand

    mov r8d, 0x80000002
.brand_loop:
    mov eax, r8d
    cpuid
    mov [r11 + 0], eax
    mov [r11 + 4], ebx
    mov [r11 + 8], ecx
    mov [r11 + 12], edx
    add r11, 16
    inc r8d
    cmp r8d, 0x80000005
    jb .brand_loop
    mov byte [r11], 0
    jmp .done

.no_brand:
    mov byte [r11], 0
.done:
    pop rbx
    ret
