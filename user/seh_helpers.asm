bits 64
default rel
section .text

global SehTriggerDivide
global SehDivideResume
SehTriggerDivide:
    mov     eax, 1
    xor     edx, edx
    xor     ecx, ecx
    div     rcx
SehDivideResume:
    mov     eax, 1
    ret

global SehTriggerAccess
global SehAccessResume
SehTriggerAccess:
    xor     eax, eax
    mov     qword [rax], 1
SehAccessResume:
    mov     eax, 1
    ret
