; Small SSE2 bulk primitives for validated DOS REP string spans.
; SysV AMD64 ABI: copy(dst, src, byte_count), fill32(dst, value, dword_count).
; XMM registers are caller-saved. The kernel enables SSE2 and saves/restores
; x87/SSE state in the scheduler before another thread can run.

bits 64
default rel

section .text

global DosSimdCopy
DosSimdCopy:
    mov     rax, rdi
    cmp     rdx, 64
    jb      .copy_tail
.copy_64:
    movdqu  xmm0, [rsi]
    movdqu  xmm1, [rsi + 16]
    movdqu  xmm2, [rsi + 32]
    movdqu  xmm3, [rsi + 48]
    movdqu  [rdi], xmm0
    movdqu  [rdi + 16], xmm1
    movdqu  [rdi + 32], xmm2
    movdqu  [rdi + 48], xmm3
    add     rsi, 64
    add     rdi, 64
    sub     rdx, 64
    cmp     rdx, 64
    jae     .copy_64
.copy_16:
    cmp     rdx, 16
    jb      .copy_tail
    movdqu  xmm0, [rsi]
    movdqu  [rdi], xmm0
    add     rsi, 16
    add     rdi, 16
    sub     rdx, 16
    jmp     .copy_16
.copy_tail:
    test    rdx, rdx
    jz      .copy_done
.copy_byte:
    mov     cl, [rsi]
    mov     [rdi], cl
    inc     rsi
    inc     rdi
    dec     rdx
    jnz     .copy_byte
.copy_done:
    ret

global DosSimdFill32
DosSimdFill32:
    mov     rax, rdi
    test    rdx, rdx
    jz      .fill_done
    movd    xmm0, esi
    pshufd  xmm0, xmm0, 0
    cmp     rdx, 16
    jb      .fill_tail
.fill_16:
    movdqu  [rdi], xmm0
    movdqu  [rdi + 16], xmm0
    movdqu  [rdi + 32], xmm0
    movdqu  [rdi + 48], xmm0
    add     rdi, 64
    sub     rdx, 16
    cmp     rdx, 16
    jae     .fill_16
.fill_tail:
    test    rdx, rdx
    jz      .fill_done
.fill_one:
    mov     [rdi], esi
    add     rdi, 4
    dec     rdx
    jnz     .fill_one
.fill_done:
    ret
