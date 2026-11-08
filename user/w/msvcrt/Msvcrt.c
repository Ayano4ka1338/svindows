/*
 * user/w/msvcrt/Msvcrt.c - a tiny C runtime (msvcrt.dll).
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
__declspec(dllimport) HANDLE CreateFileW(const WCHAR *, DWORD, DWORD, void *, DWORD, DWORD, HANDLE);
__declspec(dllimport) BOOL DeleteFileA(const char *);
__declspec(dllimport) BOOL MoveFileA(const char *, const char *);
__declspec(dllimport) BOOL CreateDirectoryA(const char *, void *);
__declspec(dllimport) HANDLE FindFirstFileW(const WCHAR *, void *);
__declspec(dllimport) BOOL FindNextFileW(HANDLE, void *);
__declspec(dllimport) BOOL FindClose(HANDLE);
__declspec(dllimport) DWORD GetFileSize(HANDLE, DWORD *);
__declspec(dllimport) BOOL CloseHandle(HANDLE);
__declspec(dllimport) DWORD GetFileAttributesW(const WCHAR *);
__declspec(dllimport) BOOL ReadFile(HANDLE, void *, DWORD, DWORD *, void *);
__declspec(dllimport) BOOL SetFilePointerEx(HANDLE, long long, long long *, DWORD);
__declspec(dllimport) BOOL SetCurrentDirectoryW(const WCHAR *);
__declspec(dllimport) HANDLE CreateThread(void *, SIZE_T, DWORD (*)(void *),
                                           void *, DWORD, DWORD *);
__declspec(dllimport) void ExitThread(DWORD);

typedef struct _QG_FILE FILE;
typedef struct { DWORD dwLowDateTime, dwHighDateTime; } QG_FILETIME;
typedef struct {
    DWORD dwFileAttributes;
    QG_FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime;
    DWORD nFileSizeHigh, nFileSizeLow, dwReserved0, dwReserved1;
    WCHAR cFileName[260], cAlternateFileName[14];
} WIN32_FIND_DATAW;
#define EOF (-1)
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
int qg_vfprintf(FILE *, const char *, __builtin_va_list);
int qg_fflush(FILE *);
int qg_is_file(FILE *);
int qg_vsnprintf(char *, unsigned long long, const char *, __builtin_va_list);
void qg_run_atexit(void);
__declspec(dllexport) unsigned long long wcslen(const WCHAR *s);
__declspec(dllexport) long strtol(const char *, char **, int);
__declspec(dllexport) unsigned long strtoul(const char *, char **, int);
__declspec(dllexport) FILE *fopen(const char *, const char *);
__declspec(dllexport) int fclose(FILE *);
__declspec(dllexport) double atan(double);
__declspec(dllexport) long long _lseek(int, long long, int);

/* Data exports used by the classic MSVC startup linked into Windows inbox
 * utilities.  They are writable through the executable's IAT. */
__declspec(dllexport) int _fmode;
__declspec(dllexport) int _commode;
/* MSVC floating-point objects probe this marker during startup. */
__declspec(dllexport) int _fltused = 0x9875;
static char *g_empty_environment[] = { 0 };
__declspec(dllexport) char **__initenv = g_empty_environment;

/* MSVCRT exposes the raw command lines as data symbols, not functions: the
 * Microsoft CRT startup imports the *address* of these pointers and
 * dereferences it before calling main/wWinMain. This DLL is built /noentry and
 * so has no DllMain to fill them in; process creation seeds both from the
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
#include "FileStream.inc"
#include "FileCompat.inc"
#include "Locale.inc"
