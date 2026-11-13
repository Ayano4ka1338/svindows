/*
 * user/w/ntdll/Ntdllrtl.c - the C side of SVINDOWS ntdll.dll.
 *
 * Real Windows binaries import substantial runtime support directly from
 * ntdll: the Rtl heap, critical sections, UNICODE_STRING helpers and a small
 * CRT surface. Until now those imports resolved to return-0 stubs, which
 * silently broke user32/gdi32 internals (every RtlAllocateHeap returned NULL,
 * critical sections did nothing). This module provides honest minimal
 * implementations over the native Nt* services.
 */

typedef void              *HANDLE;
typedef unsigned char      BYTE;
typedef unsigned short     WORD;
typedef unsigned int       UINT;
typedef unsigned long      DWORD;
typedef int                BOOL;
typedef long               LONG;
typedef unsigned long      ULONG;
typedef long long          LONGLONG;
typedef unsigned long long ULONGLONG;
typedef unsigned long long SIZE_T;
typedef unsigned short     WCHAR;
typedef unsigned short     USHORT;
typedef unsigned int       UINT32;
typedef int                INT32;
typedef unsigned short      UINT16;
typedef long               NTSTATUS;
typedef const WCHAR       *PCWSTR;
typedef WCHAR             *PWSTR;

#define NT_SUCCESS(s)   ((NTSTATUS)(s) >= 0)
#define TRUE  1
#define FALSE 0
typedef int BOOLEAN;
#define STATUS_SUCCESS           ((NTSTATUS)0)
#define STATUS_NO_MEMORY         ((NTSTATUS)0xC0000017)
#define STATUS_INVALID_PARAMETER ((NTSTATUS)0xC000000D)
#define STATUS_NOT_FOUND         ((NTSTATUS)0xC0000225)
#define STATUS_NOT_IMPLEMENTED   ((NTSTATUS)0xC0000002L)
_Static_assert(sizeof(SIZE_T) == 8, "Win64 SIZE_T must be pointer-sized");

/* Native services exported by the asm half of this DLL. */
extern NTSTATUS NtAllocateVirtualMemory(HANDLE Process, void **BaseAddress,
                                        ULONGLONG ZeroBits, ULONGLONG *RegionSize,
                                        DWORD AllocationType, DWORD Protect);
extern void     NtDisplayString(const char *text);
extern NTSTATUS NtDequeueUserApc(void *apc);

typedef struct _NTDLL_USER_APC {
    void (*Routine)(void *, void *, ULONG);
    void *Context;
    void *Argument1;
    ULONGLONG Argument2;
} NTDLL_USER_APC;

/* Called by the three alertable native stubs after STATUS_USER_APC. Keeping
 * the dequeue here leaves callback execution in the target thread's user
 * address space, with ordinary Win64 call/return and exception semantics. */
void KiUserApcDispatcher(void)
{
    NTDLL_USER_APC apc;
    if (NtDequeueUserApc(&apc) == STATUS_SUCCESS && apc.Routine)
        apc.Routine(apc.Context, apc.Argument1, (ULONG)apc.Argument2);
}

static DWORD read_teb_dword(ULONGLONG off)
{
    DWORD v;
    __asm__ volatile("movl %%gs:(%1), %0" : "=r"(v) : "r"(off));
    return v;
}

static ULONGLONG read_teb_qword(ULONGLONG off)
{
    ULONGLONG v;
    __asm__ volatile("movq %%gs:(%1), %0" : "=r"(v) : "r"(off));
    return v;
}

static void write_teb_dword(ULONGLONG off, DWORD val)
{
    __asm__ volatile("movl %0, %%gs:(%1)" : : "r"(val), "r"(off));
}

void *memset(void *dst, int v, SIZE_T n);
void *memcpy(void *dst, const void *src, SIZE_T n);


#include "Rtl/MemorySync.inc"
#include "Rtl/Strings.inc"
#include "Rtl/Paths.inc"
#include "Rtl/FileCompat.inc"
#include "Rtl/ResourceRanges.inc"
#include "Rtl/Compat.inc"
#include "Rtl/Exception.inc"
static void rtl_heap_raise(NTSTATUS status)
{
    NT_EXCEPTION_RECORD64 record = {0};
    record.ExceptionCode = status;
    record.ExceptionAddress = (U64)__builtin_return_address(0);
    RtlRaiseException(&record);
}
#include "Rtl/ElapsedTime.inc"
