; ============================================================================
; user/w/ntdll/Ntdll.asm - the SVINDOWS user-mode system library (ntdll.dll).
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

; Licensing values are not backed by a local license store. Return the native
; not-found status and initialize optional output fields so callers can take
; their normal unlicensed/default path.
global NtQueryLicenseValue
NtQueryLicenseValue:
    test    rdx, rdx
    jz      .no_type
    mov     dword [rdx], 0
.no_type:
    mov     r10, [rsp + 0x28]
    test    r10, r10
    jz      .no_size
    mov     dword [r10], 0
.no_size:
    mov     eax, 0xC0000034          ; STATUS_OBJECT_NAME_NOT_FOUND
    ret

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

; LDR_INIT_BLOCK fixed layout (see kernel/ldr/pe/ProcessModules.inc).
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
; implementation lives in the C half of this DLL (Ntdllrtl.c), where the
; per-message default results are readable.

; USER32 connects to the user CSR server during DLL_PROCESS_ATTACH and asks it
; to fill a 0x240-byte USERCONNECT block.  Its first payload member is
; SHAREDINFO, whose psi field points at the read-only SERVERINFO that win32k
; and USER32 share.  win32k builds that page (kernel/win32k/Serverinfo.c) at a
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
    mov     rax, 0x0000000001300000
    mov     [r8 + 8], rax
    ; SHAREDINFO.aheList and HeEntrySize. Process creation maps the native
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

; The kernel returns STATUS_USER_APC (0xC0) with one APC still queued for this
; thread. Invoke it in ring 3 before the native wait returns to its caller.
extern KiUserApcDispatcher
%macro ALERTABLE_SYSCALL_STUB 2
global %1
%1:
    mov     r10, rcx
    mov     eax, %2
    syscall
    cmp     eax, 0xC0
    jne     %%done
    sub     rsp, 0x28              ; Win64 shadow space and 16-byte alignment
    call    KiUserApcDispatcher
    add     rsp, 0x28
    mov     eax, 0xC0
%%done:
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

; Service numbers follow the supported native x64 ABI (public NT syscall-number tables);
; the kernel's KiServiceTable is indexed by the same values. Services without a
; verified native number use SVINDOWS-private slots at 0xF0+.
ALERTABLE_SYSCALL_STUB NtWaitForSingleObject, 0x01
SYSCALL_STUB NtSetEvent,              0x02
SYSCALL_STUB NtReadFile,              0x03
SYSCALL_STUB NtQueryInformationFile,  0x06
SYSCALL_STUB NtWriteFile,             0x05
SYSCALL_STUB NtClose,                 0x0C
SYSCALL_STUB NtOpenKey,               0x0F
SYSCALL_STUB NtAllocateVirtualMemory, 0x15
SYSCALL_STUB NtFreeVirtualMemory,     0x1B
SYSCALL_STUB NtQueryVirtualMemory,    0x20
SYSCALL_STUB NtSetInformationFile,    0x24
SYSCALL_STUB NtProtectVirtualMemory,  0x4D
SYSCALL_STUB NtQueryValueKey,         0x17
SYSCALL_STUB NtCreateKey,             0x1A
SYSCALL_STUB NtCreateEvent,           0x45
SYSCALL_STUB NtFlushBuffersFile,      0x48
SYSCALL_STUB NtCreateThreadEx,        0xA5
SYSCALL_STUB NtSuspendThread,         0x114
SYSCALL_STUB NtResumeThread,          0x115
SYSCALL_STUB NtSetInformationProcess, 0x116
SYSCALL_STUB NtQueryThreadPriority,    0x117
SYSCALL_STUB NtSetThreadPriority,      0x118
SYSCALL_STUB NtCreateIoCompletion,     0x119
SYSCALL_STUB NtSetIoCompletion,        0x11A
SYSCALL_STUB NtRemoveIoCompletion,     0x11B
SYSCALL_STUB NtAssociateIoCompletionFile, 0x11C
SYSCALL_STUB NtDuplicateObject,         0x11D
SYSCALL_STUB NtOpenProcess,              0x11E
SYSCALL_STUB NtCreateSection,            0x11F
SYSCALL_STUB NtMapViewOfSection,         0x120
SYSCALL_STUB NtUnmapViewOfSection,       0x121
SYSCALL_STUB NtOpenFile,                 0x122
SYSCALL_STUB NtTerminateThread,       0x50
ALERTABLE_SYSCALL_STUB NtDelayExecution, 0x31
SYSCALL_STUB NtCreateTimer,           0xA6
SYSCALL_STUB NtSetTimer,              0x5F
SYSCALL_STUB NtCancelTimer,           0x5E
SYSCALL_STUB NtQueryTimer,            0x35
SYSCALL_STUB NtQueryDirectoryFile,    0x32
SYSCALL_STUB NtQuerySystemInformation,0x33
SYSCALL_STUB NtCreateFile,            0x52
SYSCALL_STUB NtSetValueKey,           0x5D
SYSCALL_STUB NtDisplayString,         0xF0
SYSCALL_STUB NtDisplayNumber,         0xF1
SYSCALL_STUB NtLoadLibrary,           0xF2
SYSCALL_STUB NtQueryFileInfo,         0xF4
ALERTABLE_SYSCALL_STUB NtWaitForMultipleObjects, 0xF6
SYSCALL_STUB NtResetEvent,            0xF7
SYSCALL_STUB NtCreateSemaphore,       0xF8
SYSCALL_STUB NtReleaseSemaphore,      0xF9
SYSCALL_STUB NtQueryInformationProcess,0xFA
SYSCALL_STUB NtCreateUserProcessSimple,0xFD
SYSCALL_STUB NtFileSystemControlSimple,0xFE
SYSCALL_STUB NtSetFileSizeSimple,     0xFF
SYSCALL_STUB NtCreatePipeSimple,      0x100
SYSCALL_STUB NtConsoleControlSimple,  0x101
SYSCALL_STUB NtQueueApcThread,       0x103
SYSCALL_STUB NtDequeueUserApc,       0x104
ALERTABLE_SYSCALL_STUB NtTestAlert,  0x105
SYSCALL_STUB NtOpenProcessToken,     0x106
SYSCALL_STUB NtOpenThreadToken,      0x107
SYSCALL_STUB NtQueryInformationToken,0x108
SYSCALL_STUB NtDeleteKey,            0x109
SYSCALL_STUB NtDeleteValueKey,       0x10A
SYSCALL_STUB NtEnumerateKey,         0x10B
SYSCALL_STUB NtEnumerateValueKey,    0x10C
SYSCALL_STUB NtQueryKey,             0x10D
SYSCALL_STUB NtRenameKey,            0x1F09
SYSCALL_STUB NtAdjustPrivilegesToken,0x1F0A
SYSCALL_STUB SvNtGdiCreateDisplayDC,0x1F0B
SYSCALL_STUB SvNtUserGetCursorPos,   0x1F0C
SYSCALL_STUB SvNtUserChildWindowFromPoint, 0x1F0D
SYSCALL_STUB SvNtGdiViewportOrigin, 0x1F0E
SYSCALL_STUB SvNtGdiClipRect,        0x1F0F
SYSCALL_STUB SvNtUserIsIconic,      0x1F10
SYSCALL_STUB SvNtUserCreateIconHandle, 0x1F11
SYSCALL_STUB SvNtUserDestroyIcon,   0x1F12
SYSCALL_STUB SvNtUserGetDesktopWindow, 0x1F13
SYSCALL_STUB SvNtUserSetCursorPos,  0x1F14
SYSCALL_STUB SvNtUserGetMessagePos, 0x1F15
SYSCALL_STUB SvNtUserQueryWindowState, 0x1F16
SYSCALL_STUB NtOpenThread,           0x10E
SYSCALL_STUB NtOpenEvent,            0x10F
SYSCALL_STUB NtCreateJobObject,      0x110
SYSCALL_STUB NtAssignProcessToJobObject, 0x111
SYSCALL_STUB NtSetInformationJobObject, 0x112
SYSCALL_STUB NtQueryInformationJobObject, 0x113

SYSCALL_STUB NtCreateMutant,         0x192
SYSCALL_STUB NtReleaseMutant,        0x193

; --- USER (win32u) services used by user32.dll -------------------------------
; The numbers are the win32u service indices registered by the kernel's
; KiServiceTable (kernel/ke/Syscall.c, SN_NtUser* definitions) for the matching
; Windows 10 user-mode build. Only the services the client-side USER layer
; actually calls are stubbed here; a service the kernel registers but this file
; omits is one no Svindows component issues.
SYSCALL_STUB NtUserGetMessage,               0x1009
SYSCALL_STUB NtUserPeekMessage,              0x1004
SYSCALL_STUB NtUserGetDC,                    0x100D
SYSCALL_STUB NtUserGetForegroundWindow,      0x103F
SYSCALL_STUB NtUserGetAtomName,              0x10AB
SYSCALL_STUB NtUserRegisterWindowMessage,    0x103A
SYSCALL_STUB NtUserFindExistingCursorIcon,   0x1041
SYSCALL_STUB NtUserCreateWindowEx,           0x1078
SYSCALL_STUB NtUserDestroyWindow,            0x109F
SYSCALL_STUB NtUserMessageCall,              0x100A
SYSCALL_STUB NtUserPostMessage,              0x1012
SYSCALL_STUB NtUserPostThreadMessage,        0x1061
SYSCALL_STUB NtUserBeginPaint,               0x101A
SYSCALL_STUB NtUserEndPaint,                 0x101C
SYSCALL_STUB NtUserSetWindowPos,             0x1027
SYSCALL_STUB NtUserDispatchMessage,          0x1039
SYSCALL_STUB NtUserShowWindow,               0x105A
SYSCALL_STUB NtUserMoveWindow,               0x1060
SYSCALL_STUB NtUserInvalidateRect,           0x1007
SYSCALL_STUB NtUserValidateRect,             0x10CF
SYSCALL_STUB NtUserUpdateWindow,             0x10E9
SYSCALL_STUB NtUserRegisterClassExWOW,       0x10B2
SYSCALL_STUB NtUserUnregisterClass,          0x10BD
SYSCALL_STUB NtUserTranslateMessage,         0x1010
SYSCALL_STUB NtUserSetActiveWindow,          0x10E1
SYSCALL_STUB NtUserSetCapture,               0x104C
SYSCALL_STUB NtUserReleaseCapture,           0x1017
SYSCALL_STUB NtUserSetCursor,                0x101D
SYSCALL_STUB NtUserSetTimer,                 0x101B
SYSCALL_STUB NtUserKillTimer,                0x101E
SYSCALL_STUB NtUserSetWinEventHook,          0x1105
SYSCALL_STUB NtUserUnhookWinEvent,           0x1106
; Window-manager queries. These use Svindows-assigned numbers inside the USER
; range (kernel/ke/Syscall.c) rather than a native Windows index: no Windows
; build exports an equivalent NtUserGetClientRect for the client to call.
SYSCALL_STUB NtUserGetWindowRect,            0x1031
SYSCALL_STUB NtUserGetClientRect,            0x1032
SYSCALL_STUB NtUserSetFocus,                 0x1033
SYSCALL_STUB NtUserGetFocus,                 0x1034
SYSCALL_STUB NtUserSetWindowText,            0x1035
SYSCALL_STUB NtUserGetWindowText,            0x1036
SYSCALL_STUB NtUserWindowFromPoint,          0x1037
SYSCALL_STUB NtUserQueryWindow,              0x1013
SYSCALL_STUB NtUserGetAncestor,              0x10B4
SYSCALL_STUB NtUserSetWindowLongPtr,          0x1471
SYSCALL_STUB NtUserClassLong,                 0x1F02
SYSCALL_STUB NtUserMapWindowPoints,           0x1F03
SYSCALL_STUB NtGdiReadUserBuffer,             0x1F04
; Svindows private USER ABI (numbers are not Windows-build syscall indices).
SYSCALL_STUB NtUserGetKeyState,              0x1F00
SYSCALL_STUB NtUserTrackMouseEvent,          0x1F01
SYSCALL_STUB NtUserGetSystemMetrics,         0x1050
SYSCALL_STUB NtUserGetSysColor,              0x1051

; --- GDI services used by gdi32.dll -----------------------------------------
SYSCALL_STUB NtGdiGetEntry,                  0x12AE
SYSCALL_STUB NtGdiCreateSolidBrush,          0x10BA
SYSCALL_STUB NtGdiCreatePen,                 0x1059
SYSCALL_STUB NtGdiHfontCreate,               0x105F
SYSCALL_STUB NtGdiStretchBlt,                0x1062
SYSCALL_STUB NtGdiSelectBrush,               0x12F6
SYSCALL_STUB NtGdiSelectPen,                 0x12F8
SYSCALL_STUB NtGdiSelectFont,                0x103C
SYSCALL_STUB NtGdiSelectBitmap,              0x100E
SYSCALL_STUB NtGdiDeleteObjectApp,           0x1026
SYSCALL_STUB NtGdiPatBlt,                    0x105C
SYSCALL_STUB NtGdiBitBlt,                    0x100B
SYSCALL_STUB NtGdiBlitUserBuffer,            0x1052
SYSCALL_STUB NtGdiExtTextOutW,               0x103B
SYSCALL_STUB NtGdiGetTextMetricsW,           0x1077
SYSCALL_STUB NtGdiGetTextExtent,             0x1096
SYSCALL_STUB NtGdiSetPixel,                  0x10B1
SYSCALL_STUB NtGdiRectangle,                 0x1092
SYSCALL_STUB NtGdiMoveTo,                    0x12DC
SYSCALL_STUB NtGdiLineTo,                    0x1044
SYSCALL_STUB NtGdiCreateCompatibleDC,        0x1057
SYSCALL_STUB NtGdiCreateCompatibleBitmap,    0x104E
SYSCALL_STUB NtGdiCreateBitmap,              0x106F
SYSCALL_STUB NtGdiGetDeviceCaps,             0x12A6

section .data
align 8
ntuser_pfn1: dq 0
ntuser_pfn2: dq 0
ntuser_pfn3: dq 0

; Keep syscall entry points in executable code, separate from callback data.
section .text
SYSCALL_STUB NtUserMenuBar,                  0x1F05

SYSCALL_STUB NtUserWindowRelation,            0x1F06
SYSCALL_STUB NtGdiCreatePatternBrush,         0x1F07
SYSCALL_STUB NtGdiClearWindowBackground,     0x1F08

SYSCALL_STUB NtUserGetClassInfoEx, 0x10BB
SYSCALL_STUB NtUserGetThreadDesktop, 0x1085
SYSCALL_STUB NtUserSetThreadDesktop, 0x1093
SYSCALL_STUB NtUserCallOneParam, 0x1005
SYSCALL_STUB NtUserGetProcessWindowStation, 0x1025
SYSCALL_STUB NtUserOpenDesktop, 0x10A9
SYSCALL_STUB NtUserCloseDesktop, 0x10A8
SYSCALL_STUB NtUserGetObjectInformation, 0x106E
SYSCALL_STUB SvNtUserEnumDesktopWindows, 0x1F1C
SYSCALL_STUB SvNtQueryNetworkInterface, 0x1F1D
SYSCALL_STUB NtQueryTimerResolution, 0x1F1E
SYSCALL_STUB NtTerminateProcess, 0x1F1F
SYSCALL_STUB SvNtQueryThreadExitStatus, 0x1F19
SYSCALL_STUB SvNtReadProcessMemory, 0x1F1A
SYSCALL_STUB NtGdiGetDCObject, 0x1038
SYSCALL_STUB SvNtQueryGuiResources, 0x1F1B
