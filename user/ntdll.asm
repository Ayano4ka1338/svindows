; ============================================================================
; user/ntdll.asm - the SVINDOWS user-mode system library (ntdll.dll).
;
; Each exported Nt* function is the canonical Windows syscall stub: move the
; first argument from RCX (Windows calling convention) to R10 (because SYSCALL
; clobbers RCX), load the service number into EAX, and issue SYSCALL. The kernel
; returns via SYSRET with the status in RAX.
;
; Built into a real ntdll.dll (with an export table) by the Makefile; user
; programs import these instead of emitting raw syscalls.
; ============================================================================
bits 64
default rel

section .text

; Process bootstrap used by the kernel loader. RCX points to:
;   +0    final executable entry
;   +8    DLL count
;   +16   repeated { module base, DLL entry point } (64 slots reserved)
;   +1040 TLS callback count
;   +1048 repeated { module base, callback array } (64 slots reserved)
; TLS callbacks run first (DLL_PROCESS_ATTACH), then each DLL entry is called as
; DllMain(base, DLL_PROCESS_ATTACH, NULL), then control jumps to the EXE entry
; with the original process-entry stack restored.
extern LdrpRegisterWowHandlers
extern RtlDispatchException

; LDR_INIT_BLOCK fixed layout (see kernel/ldr/pe/process_modules.inc).
%define LDR_BLOCK_ENTRIES     16
%define LDR_BLOCK_TLS_COUNT   1040
%define LDR_BLOCK_TLS_ARRAYS  1048

global LdrInitializeProcess
LdrInitializeProcess:
    push    rbx
    push    rsi
    push    rdi
    push    rbp
    push    r12
    mov     rbx, rcx

    ; --- TLS callbacks: for each {base, array}, walk the null-terminated
    ; array and call callback(base, DLL_PROCESS_ATTACH, NULL). Loop state uses
    ; callee-saved registers so the callbacks cannot clobber it.
    mov     rsi, [rbx + LDR_BLOCK_TLS_COUNT]
    lea     rdi, [rbx + LDR_BLOCK_TLS_ARRAYS]
.tls_loop:
    test    rsi, rsi
    jz      .dll_start
    mov     r12, [rdi]              ; module base
    mov     rbp, [rdi + 8]          ; callback array
    test    rbp, rbp
    jz      .tls_next
.tls_cb_loop:
    mov     rax, [rbp]
    test    rax, rax
    jz      .tls_next
    mov     rcx, r12                ; hModule
    mov     edx, 1                  ; DLL_PROCESS_ATTACH
    xor     r8d, r8d
    sub     rsp, 0x20               ; Win64 shadow space
    call    rax
    add     rsp, 0x20
    add     rbp, 8
    jmp     .tls_cb_loop
.tls_next:
    add     rdi, 16
    dec     rsi
    jmp     .tls_loop

.dll_start:
    mov     rsi, [rbx + 8]
    lea     rdi, [rbx + LDR_BLOCK_ENTRIES]
.dll_loop:
    test    rsi, rsi
    jz      .start_exe
    mov     rax, [rdi + 8]
    test    rax, rax
    jz      .next_dll
    mov     rcx, [rdi]
    mov     edx, 1              ; DLL_PROCESS_ATTACH
    xor     r8d, r8d
    sub     rsp, 0x20           ; Win64 shadow space; stack stays aligned
    call    rax
    add     rsp, 0x20
    test    eax, eax
    jnz     .next_dll
    ; A failed DLL_PROCESS_ATTACH is fatal on Windows. Log the failing module
    ; base while the bootstrap still continues, so dependency bring-up can be
    ; diagnosed without losing the original fault site.
    mov     rcx, [rdi]
    mov     r10, rcx
    mov     eax, 0xF1           ; SVINDOWS-private NtDisplayNumber
    syscall
.next_dll:
    add     rdi, 16
    dec     rsi
    jmp     .dll_loop
.start_exe:
    ; With no win32k, nobody performs user32's WOW-handler registration
    ; handshake; do it here so RegisterClassW passes its validation.
    call    LdrpRegisterWowHandlers
    mov     rax, [rbx]
    pop     r12
    pop     rbp
    pop     rdi
    pop     rsi
    pop     rbx
    xor     ecx, ecx
    jmp     rax

; void RtlCaptureContext(PCONTEXT Context /*rcx*/)
;   Fills the integer/control prefix of an AMD64 CONTEXT with the caller's
;   register state, so a software-raised exception can be unwound from the
;   point of the raise exactly like a CPU fault. RIP is the return address and
;   RSP is the value after the call; the non-volatile registers are captured
;   verbatim. ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER.
global RtlCaptureContext
RtlCaptureContext:
    mov     [rcx+0x78], rax
    mov     [rcx+0x80], rcx
    mov     [rcx+0x88], rdx
    mov     [rcx+0x90], rbx
    lea     rax, [rsp+8]
    mov     [rcx+0x98], rax          ; Rsp
    mov     [rcx+0xA0], rbp
    mov     [rcx+0xA8], rsi
    mov     [rcx+0xB0], rdi
    mov     [rcx+0xB8], r8
    mov     [rcx+0xC0], r9
    mov     [rcx+0xC8], r10
    mov     [rcx+0xD0], r11
    mov     [rcx+0xD8], r12
    mov     [rcx+0xE0], r13
    mov     [rcx+0xE8], r14
    mov     [rcx+0xF0], r15
    mov     rax, [rsp]
    mov     [rcx+0xF8], rax          ; Rip = return address
    mov     dword [rcx+0x30], 0x00100003 ; CONTEXT_CONTROL | CONTEXT_INTEGER
    ret

; PIMAGE_NT_HEADERS RtlImageNtHeader(PVOID Base)
global RtlImageNtHeader
RtlImageNtHeader:
    xor     eax, eax
    test    rcx, rcx
    jz      .image_done
    cmp     word [rcx], 0x5a4d       ; MZ
    jne     .image_done
    movsxd  rdx, dword [rcx + 0x3c]
    test    rdx, rdx
    js      .image_done
    add     rcx, rdx
    cmp     dword [rcx], 0x00004550  ; PE\0\0
    jne     .image_done
    mov     rax, rcx
.image_done:
    ret

; USER32 registers three client callback tables during process attach and then
; retrieves them through these ntdll helpers. Keeping the pointers in ntdll is
; sufficient for the single-process model and lets the real USER32 finish its
; internal function-table initialization.
global RtlInitializeNtUserPfn
RtlInitializeNtUserPfn:
    mov     [rel ntuser_pfn1], rcx
    mov     [rel ntuser_pfn2], r8
    mov     rax, [rsp + 0x28]       ; fifth argument: third table
    mov     [rel ntuser_pfn3], rax
    xor     eax, eax                ; STATUS_SUCCESS
    ret

global RtlRetrieveNtUserPfn
RtlRetrieveNtUserPfn:
    test    rcx, rcx
    jz      .retrieve_second
    mov     rax, [rel ntuser_pfn1]
    mov     [rcx], rax
.retrieve_second:
    test    rdx, rdx
    jz      .retrieve_third
    mov     rax, [rel ntuser_pfn2]
    mov     [rdx], rax
.retrieve_third:
    test    r8, r8
    jz      .retrieve_done
    mov     rax, [rel ntuser_pfn3]
    mov     [r8], rax
.retrieve_done:
    xor     eax, eax
    ret

global RtlResetNtUserPfn
RtlResetNtUserPfn:
    mov     qword [rel ntuser_pfn1], 0
    mov     qword [rel ntuser_pfn2], 0
    mov     qword [rel ntuser_pfn3], 0
    xor     eax, eax
    ret

; USER32 forwards DefWindowProcA/W here (NTDLL.NtdllDefWindowProc_W). The
; implementation lives in the C half of this DLL (ntdllrtl.c), where the
; per-message default results are readable.

; USER32 connects to the user CSR server during DLL_PROCESS_ATTACH and asks it
; to fill a 0x240-byte USERCONNECT block.  Its first payload member is
; SHAREDINFO, whose psi field points at the read-only SERVERINFO that win32k
; and USER32 share.  win32k builds that page (kernel/win32k/serverinfo.c) at a
; fixed client-visible address and fills in the screen metrics and system
; colours, so GetSystemMetrics/GetSysColor answer correctly without a syscall.
;
; NTSTATUS CsrClientConnectToServer(
;     PWSTR ObjectDirectory, ULONG ServerId, PVOID ConnectionInfo,
;     PULONG ConnectionInfoLength, PBOOLEAN CalledFromServer)
global CsrClientConnectToServer
CsrClientConnectToServer:
    test    r8, r8
    jz      .csr_invalid_parameter
    ; The first eight bytes are the CSR connection header. USER32 copies the
    ; build-specific 0x238-byte payload starting at ConnectionInfo+8; its first
    ; payload member is psi (the shared SERVERINFO pointer).
    mov     rax, 0x0000000001320000
    mov     [r8 + 8], rax
    ; SHAREDINFO.aheList and HeEntrySize. PsCreateUserProcess maps the native
    ; 24-byte USER HANDLEENTRY array at this fixed client-visible address.
    mov     rax, 0x0000000001180000
    mov     [r8 + 16], rax
    mov     dword [r8 + 24], 24
    ; USER32 supplied a writable 0x240-byte block which was already cleared by
    ; its caller.  Preserve the remaining build-specific fields as zero.
    xor     eax, eax                ; STATUS_SUCCESS
    ret
.csr_invalid_parameter:
    mov     eax, 0xc000000d         ; STATUS_INVALID_PARAMETER
    ret

%macro SYSCALL_STUB 2   ; %1 = name, %2 = service number
global %1
%1:
    mov     r10, rcx
    mov     eax, %2
    syscall
    ret
%endmacro

; Kernel exception entry: RCX=EXCEPTION_RECORD, RDX=CONTEXT.  Vectored
; handlers may edit the context and request continuation.  NtContinue below
; acknowledges the dispatch in the kernel and restores the integer/control
; prefix of the native AMD64 CONTEXT directly in ring 3.
global KiUserExceptionDispatcher
KiUserExceptionDispatcher:
    push    rbx
    push    rsi
    mov     rsi, rcx                    ; exception record
    mov     rbx, rdx                    ; context
    sub     rsp, 0x38                   ; shadow + EXCEPTION_POINTERS + align
    mov     [rsp + 0x20], rsi
    mov     [rsp + 0x28], rbx
    lea     rcx, [rsp + 0x20]
    call    RtlDispatchException
    test    eax, eax
    jz      .unhandled_exception
    mov     rcx, rbx
    xor     edx, edx
    call    NtContinue                  ; successful continuation never returns
.unhandled_exception:
    xor     ecx, ecx
    mov     edx, [rsi]                  ; exception code -> thread exit status
    call    NtTerminateThread
    ud2

global NtContinue
NtContinue:
    push    rbx
    mov     rbx, rcx                    ; context survives the syscall
    mov     r10, rcx
    mov     eax, 0x194
    syscall
    test    eax, eax
    jnz     .continue_failed
    mov     r11, rbx
    push    qword [r11 + 68]            ; EFlags
    popfq
    mov     rsp, [r11 + 152]
    push    qword [r11 + 248]           ; target RIP, consumed by ret
    mov     rax, [r11 + 120]
    mov     rcx, [r11 + 128]
    mov     rdx, [r11 + 136]
    mov     rbx, [r11 + 144]
    mov     rbp, [r11 + 160]
    mov     rsi, [r11 + 168]
    mov     rdi, [r11 + 176]
    mov     r8,  [r11 + 184]
    mov     r9,  [r11 + 192]
    mov     r10, [r11 + 200]
    mov     r12, [r11 + 216]
    mov     r13, [r11 + 224]
    mov     r14, [r11 + 232]
    mov     r15, [r11 + 240]
    mov     r11, [r11 + 208]
    ret
.continue_failed:
    pop     rbx
    ret

; Service numbers target Windows 7 SP1 x64 (public NT syscall-number tables);
; the kernel's KiServiceTable is indexed by the same values. Private SVINDOWS-only
; services (no Windows equivalent) sit above the real range at 0xF0+.
SYSCALL_STUB NtWaitForSingleObject,   0x01
SYSCALL_STUB NtSetEvent,              0x02
SYSCALL_STUB NtReadFile,              0x03
SYSCALL_STUB NtWriteFile,             0x05
SYSCALL_STUB NtClose,                 0x0C
SYSCALL_STUB NtOpenKey,               0x0F
SYSCALL_STUB NtAllocateVirtualMemory, 0x15
SYSCALL_STUB NtFreeVirtualMemory,     0x1B
SYSCALL_STUB NtQueryVirtualMemory,    0x20
SYSCALL_STUB NtProtectVirtualMemory,  0x4D
SYSCALL_STUB NtQueryValueKey,         0x17
SYSCALL_STUB NtCreateKey,             0x1A
SYSCALL_STUB NtCreateEvent,           0x48
SYSCALL_STUB NtCreateThreadEx,        0xA5
SYSCALL_STUB NtTerminateThread,       0x50
SYSCALL_STUB NtDelayExecution,        0x31
SYSCALL_STUB NtQuerySystemInformation,0x33
SYSCALL_STUB NtCreateFile,            0x52
SYSCALL_STUB NtSetValueKey,           0x5D
SYSCALL_STUB NtDisplayString,         0xF0
SYSCALL_STUB NtDisplayNumber,         0xF1
SYSCALL_STUB NtLoadLibrary,           0xF2
SYSCALL_STUB NtEnumerateRootFiles,    0xF3
SYSCALL_STUB NtQueryFileInfo,         0xF4
SYSCALL_STUB NtSetFilePosition,       0xF5
SYSCALL_STUB NtWaitForMultipleObjects,0xF6
SYSCALL_STUB NtResetEvent,            0xF7
SYSCALL_STUB NtCreateSemaphore,       0xF8
SYSCALL_STUB NtReleaseSemaphore,      0xF9
SYSCALL_STUB NtQueryInformationProcess,0xFA
SYSCALL_STUB NtCreateUserProcessSimple,0xFD
SYSCALL_STUB NtFileSystemControlSimple,0xFE
SYSCALL_STUB NtSetFileSizeSimple,     0xFF
SYSCALL_STUB NtCreatePipeSimple,      0x100
SYSCALL_STUB NtConsoleControlSimple,  0x101
SYSCALL_STUB NtEnumerateDirectorySimple, 0x102

; Private ABI-regression probes. Probe N accepts exactly N Win64 arguments;
; the kernel validates both the register and stack argument slots.
SYSCALL_STUB NtAbiProbe0,            0x180
SYSCALL_STUB NtAbiProbe1,            0x181
SYSCALL_STUB NtAbiProbe2,            0x182
SYSCALL_STUB NtAbiProbe3,            0x183
SYSCALL_STUB NtAbiProbe4,            0x184
SYSCALL_STUB NtAbiProbe5,            0x185
SYSCALL_STUB NtAbiProbe6,            0x186
SYSCALL_STUB NtAbiProbe7,            0x187
SYSCALL_STUB NtAbiProbe8,            0x188
SYSCALL_STUB NtAbiProbe9,            0x189
SYSCALL_STUB NtAbiProbe10,           0x18A
SYSCALL_STUB NtAbiProbe11,           0x18B
SYSCALL_STUB NtAbiProbe12,           0x18C
SYSCALL_STUB NtAbiProbe13,           0x18D
SYSCALL_STUB NtAbiProbe14,           0x18E
SYSCALL_STUB NtAbiProbe15,           0x18F
SYSCALL_STUB NtAbiProbe16,           0x190
SYSCALL_STUB NtAbiShadowProbe,       0x191
SYSCALL_STUB NtCreateMutant,         0x192
SYSCALL_STUB NtReleaseMutant,        0x193

section .data
align 8
ntuser_pfn1: dq 0
ntuser_pfn2: dq 0
ntuser_pfn3: dq 0
