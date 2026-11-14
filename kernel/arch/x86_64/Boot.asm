; ============================================================================
; arch/x86_64/Boot.asm - Multiboot2 entry and the 32-bit -> 64-bit trampoline.
;
; Limine loads this Multiboot2 ELF64 image and enters `_start` in 32-bit protected mode with
; paging disabled. We then:
;   1. save the Multiboot2 magic + info pointer,
;   2. build boot page tables that identity-map the low 4 GiB (the Multiboot2
;      information pointer is 32-bit) and map the first 1 GiB at the kernel's
;      higher-half base (-2 GiB),
;   3. enable PAE, set EFER.LME, turn on paging  -> IA-32e mode,
;   4. load a 64-bit GDT and far-jump into 64-bit code,
;   5. jump to the higher-half virtual address and call KiSystemStartup.
; ============================================================================

KERNEL_VBASE      equ 0xFFFFFFFF80000000

; The trampoline addresses physical boot data directly before the high-half
; mapping is active. Keep NASM's absolute-address default explicit instead of
; relying on its deprecated implicit setting.
default abs

; Multiboot2 header constants.
MB2_MAGIC         equ 0xE85250D6
MB2_ARCH_I386     equ 0

; Page/table flags.
PTE_PRESENT       equ 0x1
PTE_WRITE         equ 0x2
PTE_HUGE          equ 0x80          ; 2 MiB page (PS bit) in a PD entry

; A small COM1 trace is available before the C runtime and logger exist. Each
; byte is also sent to POST port 80h so a hardware POST card can show the last
; trampoline stage if the machine resets before the kernel starts.
%macro BOOT_MARK 1
    mov     dx, 0x3FD                 ; COM1 line status
    mov     ecx, 0x100000
%%wait_tx:
    in      al, dx
    test    al, 0x20
    jnz     %%send
    loop    %%wait_tx
%%send:
    mov     dx, 0x3F8
    mov     al, %1
    out     dx, al
    mov     dx, 0x80
    out     dx, al
%endmacro

; ---------------------------------------------------------------------------
; Multiboot2 header - must be 8-byte aligned and within the first 32 KiB.
; ---------------------------------------------------------------------------
section .multiboot progbits alloc noexec nowrite align=8
mb_header_start:
    dd  MB2_MAGIC
    dd  MB2_ARCH_I386
    dd  mb_header_end - mb_header_start
    dd  -(MB2_MAGIC + MB2_ARCH_I386 + (mb_header_end - mb_header_start))

    ; Framebuffer request tag (type = 5): ask the bootloader for a linear framebuffer.
    align 8
    dw  5                       ; type = framebuffer
    dw  1                       ; flags: optional; the kernel applies it on VBE adapters
    dd  20                      ; size
    dd  1200                    ; preferred width
    dd  800                     ; preferred height
    dd  32                      ; preferred bits per pixel

    ; End tag (type = 0, size = 8).
    align 8
    dw  0
    dw  0
    dd  8
mb_header_end:

; ---------------------------------------------------------------------------
; 32-bit trampoline.
; ---------------------------------------------------------------------------
section .boot.text progbits alloc exec nowrite align=16
bits 32
global _start
extern KiSystemStartup

_start:
    cld
    cli
    mov     esp, boot_stack_top       ; a stack in low identity memory
    ; Preserve the Multiboot2 hand-off before touching EAX for the UART setup.
    mov     [saved_magic], eax
    mov     [saved_mbi], ebx

    ; Match HalInitializeSerial's 115200 8N1 setup so failures in the mode
    ; switch are visible before KeLogInit can run.
    mov     dx, 0x3F9
    xor     eax, eax
    out     dx, al                    ; disable UART interrupts
    mov     dx, 0x3FB
    mov     al, 0x80
    out     dx, al                    ; divisor latch access
    mov     dx, 0x3F8
    mov     al, 1
    out     dx, al                    ; divisor = 1 (115200 baud)
    mov     dx, 0x3F9
    xor     eax, eax
    out     dx, al
    mov     dx, 0x3FB
    mov     al, 3                     ; 8 data bits, no parity, one stop
    out     dx, al
    mov     dx, 0x3FA
    mov     al, 0xC7
    out     dx, al                    ; enable and clear FIFO
    mov     dx, 0x3FC
    mov     al, 0x0B
    out     dx, al                    ; DTR, RTS, OUT2
    BOOT_MARK '0'

    ; --- Zero the boot paging structures (PML4 + PDPT + four PDs). ----------
    mov     edi, boot_pml4
    xor     eax, eax
    mov     ecx, (6 * 4096) / 4
    rep     stosd
    BOOT_MARK '1'

    ; --- Top-level entries. --------------------------------------------------
    ; PML4[0]   -> PDPT   (identity, low canonical half)
    ; PML4[511] -> PDPT   (kernel higher half, -2 GiB)
    mov     eax, boot_pdpt
    or      eax, PTE_PRESENT | PTE_WRITE
    mov     [boot_pml4 + 0 * 8], eax
    mov     [boot_pml4 + 511 * 8], eax

    ; PDPT[0..3] -> four PDs (identity 0..4 GiB). Limine's Multiboot2
    ; hand-off pointer is 32-bit and may point above the first GiB.
    ; PDPT[510] -> first PD (higher half maps physical 0..1 GiB).
    mov     eax, boot_pd
    or      eax, PTE_PRESENT | PTE_WRITE
    mov     [boot_pdpt + 0 * 8], eax
    mov     [boot_pdpt + 510 * 8], eax
    mov     eax, boot_pd + 4096
    or      eax, PTE_PRESENT | PTE_WRITE
    mov     [boot_pdpt + 1 * 8], eax
    mov     eax, boot_pd + 8192
    or      eax, PTE_PRESENT | PTE_WRITE
    mov     [boot_pdpt + 2 * 8], eax
    mov     eax, boot_pd + 12288
    or      eax, PTE_PRESENT | PTE_WRITE
    mov     [boot_pdpt + 3 * 8], eax

    ; --- Fill four PDs with 2048 * 2 MiB identity pages (0 .. 4 GiB). ------
    mov     edi, boot_pd
    mov     eax, PTE_PRESENT | PTE_WRITE | PTE_HUGE   ; phys 0 | flags
    mov     ecx, 2048
.fill_pd:
    mov     [edi], eax                ; low dword: phys | flags
    mov     dword [edi + 4], 0        ; high dword: 0 for the low 4 GiB
    add     eax, 0x200000             ; next 2 MiB frame
    add     edi, 8
    loop    .fill_pd
    BOOT_MARK '2'

    ; --- Turn on long mode. --------------------------------------------------
    ; Multiboot2 leaves most of CR4 implementation-defined. Start from a
    ; known mode instead of inheriting firmware state: LA57 would interpret
    ; our PML4 as a PML5, while SMAP/CET/PCIDE can also change early accesses.
    ; Paging is still off here, so explicitly select four-level PAE paging.
    xor     eax, eax
    bts     eax, 5                    ; CR4.PAE only
    mov     cr4, eax

    mov     eax, boot_pml4
    mov     cr3, eax
    BOOT_MARK '3'

    mov     ecx, 0xC0000080           ; IA32_EFER
    rdmsr
    or      eax, 1 << 8               ; EFER.LME
    wrmsr

    mov     eax, cr0
    or      eax, 1 << 31              ; CR0.PG
    mov     cr0, eax                  ; paging on -> IA-32e compatibility mode
    BOOT_MARK '4'

    lgdt    [gdt64_ptr]
    jmp     GDT64_CODE:long_mode_start

; ---------------------------------------------------------------------------
; 64-bit stub, still executing in low identity memory.
; ---------------------------------------------------------------------------
bits 64
long_mode_start:
    mov     ax, GDT64_DATA
    mov     ds, ax
    mov     es, ax
    mov     ss, ax
    mov     fs, ax
    mov     gs, ax

    ; Enable SSE. SSE2 is guaranteed on x86-64, and user code (and any real
    ; Windows binary) uses XMM registers; without this those instructions #UD.
    mov     rax, cr0
    and     ax, 0xFFFB                ; clear CR0.EM (no x87 emulation)
    or      ax, 0x0002                ; set   CR0.MP (monitor coprocessor)
    mov     cr0, rax
    mov     rax, cr4
    or      rax, 3 << 9               ; set CR4.OSFXSR | CR4.OSXMMEXCPT
    mov     cr4, rax
    BOOT_MARK '5'

    ; Absolute (64-bit) jump into the higher half.
    mov     rax, qword higher_half_start
    jmp     rax

; ---------------------------------------------------------------------------
; Boot data: GDT + saved bootloader hand-off values (low identity memory).
; ---------------------------------------------------------------------------
section .boot.data progbits alloc write align=16

align 16
gdt64:
    dq  0                              ; null descriptor
GDT64_CODE equ 0x08
    dq  0x00AF9A000000FFFF             ; 64-bit code: P,S,exec,read, L=1
GDT64_DATA equ 0x10
    dq  0x00AF92000000FFFF             ; data: P,S,read/write
gdt64_ptr:
    dw  gdt64_ptr - gdt64 - 1
    dq  gdt64

saved_magic: dd 0
saved_mbi:   dd 0

; ---------------------------------------------------------------------------
; Boot BSS: page tables + the trampoline stack (low identity memory).
; ---------------------------------------------------------------------------
section .boot.bss nobits alloc write align=4096
global boot_pml4
boot_pml4:  resb 4096
boot_pdpt:  resb 4096
boot_pd:    resb 4 * 4096

align 16
boot_stack:      resb 4096
boot_stack_top:

; ---------------------------------------------------------------------------
; Higher-half 64-bit entry (linked at the kernel's virtual base).
; ---------------------------------------------------------------------------
section .text progbits alloc exec nowrite align=16
bits 64
global higher_half_start
higher_half_start:
    lea     rsp, [rel kernel_stack_top]    ; switch to the real kernel stack
    xor     rbp, rbp                        ; terminate call-chain backtraces
    BOOT_MARK '6'

    mov     edi, [saved_magic]              ; arg1: Multiboot2 magic
    mov     esi, [saved_mbi]                ; arg2: Multiboot2 info (phys)
    BOOT_MARK '7'
    call    KiSystemStartup

.hang:
    cli
    hlt
    jmp     .hang

; ---------------------------------------------------------------------------
; Kernel stack (higher-half BSS).
;
; The PE loader recurses once per dependency edge (LdrpLoadModule ->
; LdrResolveImports -> LdrpLoadModule, plus forwarder chains), so large
; dependency graphs need
; far more than the 16 KiB this used to be. Overflowing here is silent and
; vicious: the stack sits in .bss directly above the pool's own globals, so an
; overflow rewrites the allocator's free list and the machine dies far from the
; cause. kernel_stack_guard sits immediately below the stack so the overflow is
; detected and named instead (see LdrpCheckStackGuard).
; ---------------------------------------------------------------------------
section .bss nobits alloc write align=16
global kernel_stack_guard
kernel_stack_guard:
    resq    2
global kernel_stack
kernel_stack:
    resb    0x20000                         ; 128 KiB
kernel_stack_top:
