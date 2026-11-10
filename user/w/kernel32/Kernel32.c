/*
 * user/w/kernel32/Kernel32.c - a minimal Win32 subsystem library (kernel32.dll).
 *
 * Implements a handful of the classic Win32 API on top of the native SVINDOWS
 * system calls exported by ntdll. Built for the x86-64 Windows target so a
 * normal Win32 program can link against it, exactly as it would on Windows.
 * The chain at runtime is: app.exe -> kernel32.dll -> ntdll.dll -> syscall.
 */

typedef void              *HANDLE;
typedef void              *LPVOID;
typedef unsigned char      BYTE;
typedef unsigned short     WORD;
typedef unsigned long      DWORD;
typedef long               LONG;
typedef unsigned long long ULONGLONG;
typedef unsigned long long SIZE_T;
typedef unsigned long long DWORD_PTR;
typedef int                BOOL;
typedef void             (*FARPROC)(void);
typedef DWORD (*LPTHREAD_START_ROUTINE)(LPVOID);

__declspec(dllexport) void SetLastError(DWORD error);

/* libgcc's SEH personality imports this helper even when an image never
 * throws. Full frame unwinding remains owned by ntdll; this facade currently
 * provides the ABI entry point for C-only plug-ins. */
__declspec(dllexport) void RtlUnwindEx(void *frame, void *target,
                                       void *record, void *return_value,
                                       void *context, void *history)
{
    (void)frame; (void)target; (void)record; (void)return_value;
    (void)context; (void)history;
}

extern void *RtlEncodePointer(void *pointer);
extern void *RtlDecodePointer(void *pointer);

__declspec(dllexport) void *EncodePointer(void *pointer)
{
    return RtlEncodePointer(pointer);
}

__declspec(dllexport) void *DecodePointer(void *pointer)
{
    return RtlDecodePointer(pointer);
}

__declspec(dllexport) BOOL Beep(DWORD frequency, DWORD duration)
{
    (void)frequency;
    (void)duration;
    SetLastError(50); /* ERROR_NOT_SUPPORTED: no speaker device is present. */
    return 0;
}

#define STD_OUTPUT_HANDLE ((DWORD)-11)
#define WAIT_OBJECT_0     0
#define HEAP_ZERO_MEMORY  0x00000008
#define LANG_ENGLISH_US   0x0409

typedef unsigned short WCHAR;
typedef long           NTSTATUS;
typedef struct _FILETIME { DWORD Low, High; } FILETIME;
typedef struct _SYSTEMTIME {
    WORD Year, Month, DayOfWeek, Day;
    WORD Hour, Minute, Second, Milliseconds;
} SYSTEMTIME;
typedef struct _COORD { short X, Y; } COORD;
typedef struct _SMALL_RECT { short Left, Top, Right, Bottom; } SMALL_RECT;
typedef struct _CONSOLE_SCREEN_BUFFER_INFO {
    COORD Size;
    COORD CursorPosition;
    WORD Attributes;
    SMALL_RECT Window;
    COORD MaximumWindowSize;
} CONSOLE_SCREEN_BUFFER_INFO;
typedef struct _CONSOLE_CURSOR_INFO {
    DWORD Size;
    BOOL Visible;
} CONSOLE_CURSOR_INFO;

/* NT parameter structures (Windows x64 layout). */
typedef struct _UNICODE_STRING_K { WCHAR Length, MaximumLength; WCHAR *Buffer; }
    UNICODE_STRING_K; /* note: Buffer sits at +8 by alignment (as on Windows) */

typedef struct _OBJECT_ATTRIBUTES {
    DWORD             Length;
    HANDLE            RootDirectory;
    UNICODE_STRING_K *ObjectName;
    DWORD             Attributes;
    void             *SecurityDescriptor;
    void             *SecurityQualityOfService;
} OBJECT_ATTRIBUTES;

typedef struct _IO_STATUS_BLOCK {
    union { NTSTATUS Status; void *Pointer; };
    ULONGLONG Information;
} IO_STATUS_BLOCK;

#define FILE_OPEN            1
#define MEM_COMMIT           0x1000
#define MEM_RESERVE          0x2000
#define PAGE_READWRITE       0x04
#define NT_INVALID_HANDLE    ((HANDLE)(ULONGLONG)-1)
#define NT_SUCCESS(s)        ((NTSTATUS)(s) >= 0)

/* Svindows uses UTF-8 for its compact kernel-internal path strings while the
 * public W APIs retain native UTF-16. */
static BOOL utf8_path_to_wide(const char *text, WCHAR out[260])
{
    DWORD units = 0;
    if (!text) return 0;
    while (*text) {
        const BYTE *s = (const BYTE *)text;
        DWORD cp, count;
        if (s[0] < 0x80) { cp = s[0]; count = 1; }
        else if ((s[0] & 0xe0) == 0xc0) { cp = s[0] & 0x1f; count = 2; }
        else if ((s[0] & 0xf0) == 0xe0) { cp = s[0] & 0x0f; count = 3; }
        else if ((s[0] & 0xf8) == 0xf0) { cp = s[0] & 7; count = 4; }
        else return 0;
        for (DWORD i = 1; i < count; i++) {
            if (!s[i] || (s[i] & 0xc0) != 0x80) return 0;
            cp = (cp << 6) | (s[i] & 0x3f);
        }
        if ((count == 2 && cp < 0x80) || (count == 3 && cp < 0x800) ||
            (count == 4 && cp < 0x10000) || cp > 0x10ffff ||
            (cp >= 0xd800 && cp <= 0xdfff)) return 0;
        text += count;
        if (cp <= 0xffff) {
            if (units >= 259) return 0;
            out[units++] = (WCHAR)cp;
        } else {
            if (units >= 258) return 0;
            cp -= 0x10000;
            out[units++] = (WCHAR)(0xd800 + (cp >> 10));
            out[units++] = (WCHAR)(0xdc00 + (cp & 0x3ff));
        }
    }
    out[units] = 0;
    return 1;
}

static BOOL wide_path_to_utf8(const WCHAR *wide, char out[260])
{
    DWORD used = 0;
    if (!wide) return 0;
    for (DWORD i = 0; wide[i]; i++) {
        DWORD cp = wide[i];
        if (cp >= 0xd800 && cp <= 0xdbff) {
            DWORD low = wide[++i];
            if (low < 0xdc00 || low > 0xdfff) return 0;
            cp = 0x10000 + ((cp - 0xd800) << 10) + low - 0xdc00;
        } else if (cp >= 0xdc00 && cp <= 0xdfff) return 0;
        DWORD bytes = cp < 0x80 ? 1 : cp < 0x800 ? 2 : cp < 0x10000 ? 3 : 4;
        if (used + bytes >= 260) return 0;
        if (bytes == 1) out[used++] = (char)cp;
        else if (bytes == 2) {
            out[used++] = (char)(0xc0 | (cp >> 6));
            out[used++] = (char)(0x80 | (cp & 0x3f));
        } else if (bytes == 3) {
            out[used++] = (char)(0xe0 | (cp >> 12));
            out[used++] = (char)(0x80 | ((cp >> 6) & 0x3f));
            out[used++] = (char)(0x80 | (cp & 0x3f));
        } else {
            out[used++] = (char)(0xf0 | (cp >> 18));
            out[used++] = (char)(0x80 | ((cp >> 12) & 0x3f));
            out[used++] = (char)(0x80 | ((cp >> 6) & 0x3f));
            out[used++] = (char)(0x80 | (cp & 0x3f));
        }
    }
    out[used] = 0;
    return 1;
}

/* Native services imported from ntdll, at their real Windows signatures. */
extern NTSTATUS NtCreateFile(HANDLE *FileHandle, DWORD DesiredAccess,
                             OBJECT_ATTRIBUTES *ObjectAttributes,
                             IO_STATUS_BLOCK *IoStatusBlock, void *AllocationSize,
                             DWORD FileAttributes, DWORD ShareAccess,
                             DWORD CreateDisposition, DWORD CreateOptions,
                             void *EaBuffer, DWORD EaLength);
extern NTSTATUS NtReadFile(HANDLE File, HANDLE Event, void *ApcRoutine,
                           void *ApcContext, IO_STATUS_BLOCK *IoStatusBlock,
                           void *Buffer, DWORD Length, void *ByteOffset,
                           void *Key);
extern NTSTATUS NtWriteFile(HANDLE File, HANDLE Event, void *ApcRoutine,
                            void *ApcContext, IO_STATUS_BLOCK *IoStatusBlock,
                            const void *Buffer, DWORD Length, void *ByteOffset,
                            void *Key);
extern NTSTATUS NtAllocateVirtualMemory(HANDLE Process, void **BaseAddress,
                                        ULONGLONG ZeroBits, ULONGLONG *RegionSize,
                                        DWORD AllocationType, DWORD Protect);
extern NTSTATUS NtFreeVirtualMemory(HANDLE Process, void **BaseAddress,
                                    ULONGLONG *RegionSize, DWORD FreeType);
extern NTSTATUS NtQueryVirtualMemory(HANDLE Process, const void *Address,
                                     DWORD InformationClass, void *Buffer,
                                     ULONGLONG Length,
                                     ULONGLONG *ReturnLength);
extern long      NtClose(HANDLE h);
extern NTSTATUS  NtCreateThreadEx(HANDLE *ThreadHandle, DWORD DesiredAccess,
                                  void *ObjectAttributes, HANDLE ProcessHandle,
                                  void *StartRoutine, void *Argument,
                                  DWORD CreateFlags, ULONGLONG ZeroBits,
                                  ULONGLONG StackSize, ULONGLONG MaxStackSize,
                                  void *AttributeList);
extern NTSTATUS  NtSuspendThread(HANDLE ThreadHandle, DWORD *PreviousCount);
extern NTSTATUS  NtResumeThread(HANDLE ThreadHandle, DWORD *PreviousCount);
extern NTSTATUS  NtQueryThreadPriority(HANDLE ThreadHandle, long *Priority);
extern NTSTATUS  NtSetThreadPriority(HANDLE ThreadHandle, long Priority);
extern NTSTATUS  NtWaitForSingleObject(HANDLE Handle, int Alertable,
                                       long long *Timeout);
extern NTSTATUS  NtWaitForMultipleObjects(DWORD Count, const HANDLE *Handles,
                                          DWORD WaitType, int Alertable,
                                          long long *Timeout);
extern NTSTATUS  NtCreateEvent(HANDLE *EventHandle, DWORD DesiredAccess,
                               void *ObjectAttributes, int EventType,
                               BOOL InitialState);
extern NTSTATUS  NtOpenEvent(HANDLE *EventHandle, DWORD DesiredAccess,
                             void *ObjectAttributes);
extern NTSTATUS NtCreateJobObject(HANDLE *job, DWORD access, void *attributes);
extern NTSTATUS NtAssignProcessToJobObject(HANDLE job, HANDLE process);
extern NTSTATUS NtSetInformationJobObject(HANDLE job, DWORD info_class,
                                          const void *buffer, DWORD length);
extern NTSTATUS NtQueryInformationJobObject(HANDLE job, DWORD info_class,
                                            void *buffer, DWORD length,
                                            DWORD *return_length);
extern NTSTATUS  NtSetEvent(HANDLE EventHandle, long *PreviousState);
extern NTSTATUS  NtResetEvent(HANDLE EventHandle, long *PreviousState);
extern NTSTATUS  NtCreateSemaphore(HANDLE *SemaphoreHandle, DWORD DesiredAccess,
                                   void *ObjectAttributes, long InitialCount,
                                   long MaximumCount);
extern NTSTATUS  NtReleaseSemaphore(HANDLE SemaphoreHandle, long ReleaseCount,
                                    long *PreviousCount);
extern NTSTATUS  NtCreateMutant(HANDLE *MutantHandle, DWORD DesiredAccess,
                                void *ObjectAttributes, BOOL InitialOwner);
extern NTSTATUS  NtReleaseMutant(HANDLE MutantHandle, long *PreviousCount);
extern NTSTATUS  NtTerminateThread(HANDLE ThreadHandle, NTSTATUS ExitStatus);
extern NTSTATUS NtCreateIoCompletion(HANDLE *port, DWORD access,
                                     void *attributes, DWORD concurrency);
extern NTSTATUS NtSetIoCompletion(HANDLE port, void *key, void *overlapped,
                                  NTSTATUS status, ULONGLONG information);
extern NTSTATUS NtRemoveIoCompletion(HANDLE port, void **key,
                                     void **overlapped, IO_STATUS_BLOCK *result,
                                     const long long *timeout);
extern NTSTATUS NtAssociateIoCompletionFile(HANDLE file, HANDLE port,
                                             ULONGLONG key);
extern void     *RtlAddVectoredExceptionHandler(DWORD First, void *Handler);
extern DWORD     RtlRemoveVectoredExceptionHandler(void *Handle);
extern HANDLE    NtLoadLibrary(const char *name);
extern NTSTATUS  NtDelayExecution(int Alertable, long long *Interval);
extern NTSTATUS  NtQueueApcThread(HANDLE Thread, void *Routine,
                                  void *Context, void *Argument1,
                                  DWORD Argument2);
extern NTSTATUS NtCreateTimer(HANDLE *, DWORD, void *, int);
extern NTSTATUS NtSetTimer(HANDLE, const long long *, void *, void *, int, long, BYTE *);
extern NTSTATUS NtCancelTimer(HANDLE, BYTE *);
extern NTSTATUS  NtProtectVirtualMemory(HANDLE Process, void **BaseAddress,
                                        ULONGLONG *RegionSize, DWORD NewProtect,
                                        DWORD *OldProtect);
extern NTSTATUS  NtFileSystemControlSimple(DWORD operation,
                                           const char *first,
                                           const char *second);
extern NTSTATUS  NtCreatePipeSimple(HANDLE *read_pipe, HANDLE *write_pipe);
extern NTSTATUS  NtConsoleControlSimple(HANDLE console, DWORD operation,
                                        ULONGLONG argument1,
                                        ULONGLONG argument2,
                                        ULONGLONG argument3,
                                        ULONGLONG *result);
extern NTSTATUS  NtQueryDirectoryFile(HANDLE FileHandle, HANDLE Event,
                                      void *ApcRoutine, void *ApcContext,
                                      IO_STATUS_BLOCK *IoStatusBlock,
                                      void *FileInformation, DWORD Length,
                                      DWORD FileInformationClass,
                                      BOOL ReturnSingleEntry,
                                      UNICODE_STRING_K *FileName,
                                      BOOL RestartScan);
extern NTSTATUS  NtQueryFileInfo(HANDLE File, void *Info);
extern NTSTATUS  NtSetInformationFile(HANDLE File, IO_STATUS_BLOCK *IoStatus,
                                       void *Information, DWORD Length,
                                       DWORD InformationClass);
extern NTSTATUS  NtFlushBuffersFile(HANDLE File,
                                     IO_STATUS_BLOCK *IoStatus);

__declspec(dllexport) void *AddVectoredExceptionHandler(DWORD first,
                                                         void *handler)
{
    return RtlAddVectoredExceptionHandler(first, handler);
}

__declspec(dllexport) DWORD RemoveVectoredExceptionHandler(void *handle)
{
    return RtlRemoveVectoredExceptionHandler(handle);
}

typedef struct _EXCEPTION_RECORD_K {
    NTSTATUS ExceptionCode;
    DWORD    ExceptionFlags;
    void    *ExceptionRecord;
    void    *ExceptionAddress;
    DWORD    NumberParameters;
    DWORD    Padding;
    ULONGLONG ExceptionInformation[15];
} EXCEPTION_RECORD_K;

void *memset(void *dst, int v, SIZE_T n);
extern void RtlRaiseException(EXCEPTION_RECORD_K *record);
extern void *RtlSetUnhandledExceptionFilter(void *filter);
extern long RtlCallUnhandledExceptionFilter(void *pointers);

__declspec(dllexport) void *SetUnhandledExceptionFilter(void *filter)
{
    return RtlSetUnhandledExceptionFilter(filter);
}

__declspec(dllexport) long UnhandledExceptionFilter(void *pointers)
{
    return RtlCallUnhandledExceptionFilter(pointers);
}

__declspec(dllexport) void RaiseException(DWORD code, DWORD flags,
                                          DWORD count, const ULONGLONG *args)
{
    EXCEPTION_RECORD_K record;
    memset(&record, 0, sizeof(record));
    record.ExceptionCode = (NTSTATUS)code;
    record.ExceptionFlags = flags;
    record.NumberParameters = count > 15 ? 15 : count;
    for (DWORD i = 0; i < record.NumberParameters; i++)
        record.ExceptionInformation[i] = args ? args[i] : 0;
    RtlRaiseException(&record);
}

/* The delay-load fallback is reached only after loading a DLL or resolving
 * its export has failed. Keep that failure observable by SEH callers. */
__declspec(dllexport) void *DelayLoadFailureHook(const char *dll,
                                                 const char *procedure)
{
    ULONGLONG args[2] = {(ULONGLONG)dll, (ULONGLONG)procedure};
    DWORD code = procedure ? 0xC06D007Fu : 0xC06D007Eu;
    RaiseException(code, 1, 2, args);
    return 0;
}

extern NTSTATUS NtQueryInformationProcess(HANDLE process, DWORD info_class,
                                           void *buffer, DWORD length,
                                           DWORD *return_length);
static DWORD ntstatus_to_win32(NTSTATUS status);

__declspec(dllexport) BOOL FlushInstructionCache(HANDLE process,
                                                  const void *base,
                                                  SIZE_T size)
{
    BYTE info[48];
    NTSTATUS status = NtQueryInformationProcess(process, 0, info,
                                               sizeof(info), 0);
    if (!NT_SUCCESS(status)) {
        SetLastError(ntstatus_to_win32(status));
        return 0;
    }
    (void)base;
    (void)size;
    /* This x86-64 target runs on one CPU. Data and instruction caches are
     * coherent; CPUID serializes the current instruction stream after writes. */
    unsigned eax = 0, ebx, ecx, edx;
    __asm__ volatile("cpuid" : "+a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                     : : "memory");
    return 1;
}

/* GS-relative TEB access (the TEB is at GS in ring 3). */
static DWORD read_gs_dword(ULONGLONG off)
{
    DWORD v;
    __asm__ volatile("movl %%gs:(%1), %0" : "=r"(v) : "r"(off));
    return v;
}
static void write_gs_dword(ULONGLONG off, DWORD val)
{
    __asm__ volatile("movl %0, %%gs:(%1)" : : "r"(val), "r"(off));
}
static ULONGLONG read_gs_qword(ULONGLONG off)
{
    ULONGLONG v;
    __asm__ volatile("movq %%gs:(%1), %0" : "=r"(v) : "r"(off));
    return v;
}
static void write_gs_qword(ULONGLONG off, ULONGLONG val)
{
    __asm__ volatile("movq %0, %%gs:(%1)" : : "r"(val), "r"(off) : "memory");
}

void *memset(void *dst, int v, SIZE_T n); /* defined below */

/* Allocate `bytes` of committed user memory (returns 0 on failure). */
static void *nt_alloc(ULONGLONG bytes)
{
    void *base = 0;
    ULONGLONG size = bytes;
    NTSTATUS st = NtAllocateVirtualMemory(NT_INVALID_HANDLE, &base, 0, &size,
                                          MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    return NT_SUCCESS(st) ? base : 0;
}

/* Open a file or namespace object by ASCII name via the real NtCreateFile. */
static HANDLE nt_open(const char *name)
{
    WCHAR wname[260];
    int n = 0;
    for (; name[n] && n < 259; n++)
        wname[n] = (WCHAR)(unsigned char)name[n];
    wname[n] = 0;

    UNICODE_STRING_K us = { (WCHAR)(n * 2), (WCHAR)(n * 2 + 2), wname };
    OBJECT_ATTRIBUTES oa;
    memset(&oa, 0, sizeof(oa));
    oa.Length = sizeof(oa);
    oa.ObjectName = &us;

    HANDLE h = 0;
    IO_STATUS_BLOCK iosb;
    NTSTATUS st = NtCreateFile(&h, 0, &oa, &iosb, 0, 0, 0, FILE_OPEN, 0, 0, 0);
    return NT_SUCCESS(st) ? h : NT_INVALID_HANDLE;
}

static DWORD ntstatus_to_win32(NTSTATUS status)
{
    switch ((DWORD)status) {
    case 0xC0000002: return 120; /* ERROR_CALL_NOT_IMPLEMENTED */
    case 0xC000000D: return 87;  /* ERROR_INVALID_PARAMETER */
    case 0xC0000034: return 2;  /* STATUS_OBJECT_NAME_NOT_FOUND */
    case 0xC0000008: return 6;  /* STATUS_INVALID_HANDLE */
    case 0xC0000022: return 5;  /* STATUS_ACCESS_DENIED */
    case 0xC0000017: return 8;  /* STATUS_NO_MEMORY */
    case 0xC0000033: return 123;/* ERROR_INVALID_NAME */
    case 0xC0000035: return 183;/* ERROR_ALREADY_EXISTS */
    case 0xC000003A: return 3;  /* ERROR_PATH_NOT_FOUND */
    case 0xC0000043: return 32; /* ERROR_SHARING_VIOLATION */
    case 0xC0000056: return 303;/* ERROR_DELETE_PENDING */
    case 0xC000007F: return 112;/* ERROR_DISK_FULL */
    case 0xC00000BB: return 50; /* ERROR_NOT_SUPPORTED */
    case 0xC0000101: return 145;/* ERROR_DIR_NOT_EMPTY */
    default:         return 1;  /* ERROR_INVALID_FUNCTION */
    }
}

static HANDLE nt_open_w(const WCHAR *name, DWORD access, DWORD share,
                        DWORD disposition, DWORD attributes)
{
    DWORD nt_disposition;
    switch (disposition) {
    case 1: nt_disposition = 2; break; /* CREATE_NEW -> FILE_CREATE */
    case 2: nt_disposition = 5; break; /* CREATE_ALWAYS -> FILE_OVERWRITE_IF */
    case 3: nt_disposition = 1; break; /* OPEN_EXISTING -> FILE_OPEN */
    case 4: nt_disposition = 3; break; /* OPEN_ALWAYS -> FILE_OPEN_IF */
    case 5: nt_disposition = 4; break; /* TRUNCATE_EXISTING -> FILE_OVERWRITE */
    default:
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return NT_INVALID_HANDLE;
    }
    if (!name) {
        SetLastError(87);
        return NT_INVALID_HANDLE;
    }

    DWORD chars = 0;
    while (name[chars] && chars < 32767) chars++;
    if (name[chars]) {
        SetLastError(206); /* ERROR_FILENAME_EXCED_RANGE */
        return NT_INVALID_HANDLE;
    }

    UNICODE_STRING_K us = {
        (WCHAR)(chars * sizeof(WCHAR)),
        (WCHAR)((chars + 1) * sizeof(WCHAR)),
        (WCHAR *)name
    };
    OBJECT_ATTRIBUTES oa;
    memset(&oa, 0, sizeof(oa));
    oa.Length = sizeof(oa);
    oa.ObjectName = &us;

    HANDLE handle = NT_INVALID_HANDLE;
    IO_STATUS_BLOCK iosb;
    NTSTATUS status = NtCreateFile(&handle, access, &oa, &iosb, 0,
                                   attributes & 0xFFFF, share,
                                   nt_disposition, 0, 0, 0);
    if (!NT_SUCCESS(status)) {
        SetLastError(ntstatus_to_win32(status));
        return NT_INVALID_HANDLE;
    }
    return handle;
}


#include "RuntimeLoader.inc"
#include "Jobs.inc"
#include "Completion.inc"
#include "SList.inc"
#include "HeapTime.inc"
#include "LocalMemory.inc"
#include "EnvironmentStrings.inc"
#include "ThreadsSync.inc"
#include "ComUser.inc"
#include "ResourcesSystem.inc"
#include "ActCtx.inc"
#include "Profile.inc"
#include "MappingMisc.inc"
