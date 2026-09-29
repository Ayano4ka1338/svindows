/*
 * user/msvcrt/Msvcrt.c - a tiny C runtime (msvcrt.dll).
 *
 * The handful of CRT functions a small C program needs, implemented over the
 * Win32 API: printf/puts write through WriteFile, malloc/free run on the
 * process heap, plus the usual string and memory helpers. Built and linked like
 * a real msvcrt import; programs link against it instead of a host libc.
 */

typedef void              *HANDLE;
typedef unsigned long      DWORD;
typedef int                BOOL;
typedef unsigned long long SIZE_T;
typedef unsigned long long ULONGLONG;
typedef unsigned short     WCHAR;

#define STD_OUTPUT_HANDLE ((DWORD)-11)

__declspec(dllimport) HANDLE GetStdHandle(DWORD which);
__declspec(dllimport) BOOL   WriteFile(HANDLE, const void *, DWORD, DWORD *, void *);
__declspec(dllimport) HANDLE GetProcessHeap(void);
__declspec(dllimport) void  *HeapAlloc(HANDLE, DWORD, SIZE_T);
__declspec(dllimport) BOOL   HeapFree(HANDLE, DWORD, void *);
__declspec(dllimport) void   ExitProcess(DWORD code);
__declspec(dllimport) DWORD  GetEnvironmentVariableW(const WCHAR *name,
                                                      WCHAR *out, DWORD size);
__declspec(dllimport) HANDLE GetModuleHandleA(const char *name);
__declspec(dllimport) void  *GetProcAddress(HANDLE module, const char *name);

/* Data exports used by the classic MSVC startup linked into Windows inbox
 * utilities.  They are writable through the executable's IAT. */
__declspec(dllexport) int _fmode;
__declspec(dllexport) int _commode;

/* MSVCRT exposes the raw command lines as data symbols, not functions: the
 * Microsoft CRT startup imports the *address* of these pointers and
 * dereferences it before calling main/wWinMain. This DLL is built /noentry and
 * so has no DllMain to fill them in; PsCreateUserProcess seeds both from the
 * process parameters once those exist (see LdrSeedCrtCommandLine). Until then
 * they point at valid empty strings rather than at nothing. */
static char  g_acmdline[] = "";
static WCHAR g_wcmdline[] = L"";
__declspec(dllexport) char  *_acmdln = g_acmdline;
__declspec(dllexport) WCHAR *_wcmdln = g_wcmdline;

static int g_errno;

__declspec(dllexport) int *_errno(void)
{
    return &g_errno;
}


#include "Core.inc"
#include "Compat.inc"
