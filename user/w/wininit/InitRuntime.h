#ifndef SV_INIT_RUNTIME_H
#define SV_INIT_RUNTIME_H
#include <Win32Session.h>
typedef HANDLE HKEY;
typedef struct _INIT_STARTUPINFO {
    DWORD cb; WCHAR *reserved, *desktop, *title;
    DWORD x,y,cx,cy,xchars,ychars,fill,flags;
    WORD show,reserved_bytes; BYTE *reserved_data;
    HANDLE input,output,error;
} INIT_STARTUPINFO;
typedef struct _INIT_PROCESSINFO { HANDLE process,thread; DWORD pid,tid; } INIT_PROCESSINFO;
#define INIT_HKLM ((HKEY)(UINT_PTR)0x80000002u)
#define INIT_HKCU ((HKEY)(UINT_PTR)0x80000001u)
#define INIT_HKCR ((HKEY)(UINT_PTR)0x80000000u)
#define INIT_INFINITE 0xFFFFFFFFu
#define INIT_SHUTDOWN_EVENT L"Global\\Svindows.Session.1.Shutdown"
#define INIT_READY_EVENT L"Global\\Svindows.Session.1.Ready"
#define INIT_SHELL_READY_EVENT L"Global\\Svindows.Session.1.ShellReady"
#define INIT_WINLOGON_KEY L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon"
#define INIT_SESSION_KEY L"SOFTWARE\\Svindows\\Sessions\\1"

__declspec(dllimport) void OutputDebugStringW(const WCHAR *);
__declspec(dllimport) DWORD GetLastError(void);
__declspec(dllimport) void SetLastError(DWORD);
__declspec(dllimport) DWORD GetEnvironmentVariableW(const WCHAR *, WCHAR *, DWORD);
__declspec(dllimport) BOOL SetEnvironmentVariableW(const WCHAR *, const WCHAR *);
__declspec(dllimport) DWORD ExpandEnvironmentStringsW(const WCHAR *, WCHAR *, DWORD);
__declspec(dllimport) BOOL SetCurrentDirectoryW(const WCHAR *);
__declspec(dllimport) DWORD GetFileAttributesW(const WCHAR *);
__declspec(dllimport) BOOL CreateDirectoryW(const WCHAR *, void *);
__declspec(dllimport) HANDLE CreateFileW(const WCHAR *, DWORD, DWORD, void *, DWORD, DWORD, HANDLE);
__declspec(dllimport) BOOL WriteFile(HANDLE, const void *, DWORD, DWORD *, void *);
__declspec(dllimport) BOOL FlushFileBuffers(HANDLE);
__declspec(dllimport) BOOL CloseHandle(HANDLE);
__declspec(dllimport) HANDLE CreateEventW(void *, BOOL, BOOL, const WCHAR *);
__declspec(dllimport) BOOL SetEvent(HANDLE);
__declspec(dllimport) BOOL ResetEvent(HANDLE);
__declspec(dllimport) HANDLE CreateMutexW(void *, BOOL, const WCHAR *);
__declspec(dllimport) BOOL ReleaseMutex(HANDLE);
__declspec(dllimport) DWORD WaitForSingleObject(HANDLE,DWORD);
__declspec(dllimport) DWORD WaitForMultipleObjects(DWORD,const HANDLE *,BOOL,DWORD);
__declspec(dllimport) BOOL CreateProcessW(const WCHAR *, WCHAR *, void *, void *, BOOL, DWORD,
    void *,const WCHAR *,void *,INIT_PROCESSINFO *);
__declspec(dllimport) BOOL GetExitCodeProcess(HANDLE,DWORD *);
__declspec(dllimport) BOOL TerminateProcess(HANDLE,DWORD);
__declspec(dllimport) DWORD GetTickCount(void);
__declspec(dllimport) DWORD GetCurrentThreadId(void);
__declspec(dllimport) void Sleep(DWORD);
__declspec(dllimport) LONG RegCreateKeyExW(HKEY,const WCHAR *,DWORD,WCHAR *,DWORD,DWORD,void *,HKEY *,DWORD *);
__declspec(dllimport) LONG RegOpenKeyExW(HKEY,const WCHAR *,DWORD,DWORD,HKEY *);
__declspec(dllimport) LONG RegSetValueExW(HKEY,const WCHAR *,DWORD,DWORD,const BYTE *,DWORD);
__declspec(dllimport) LONG RegQueryValueExW(HKEY,const WCHAR *,DWORD *,DWORD *,BYTE *,DWORD *);
__declspec(dllimport) LONG RegCloseKey(HKEY);
__declspec(dllimport) HANDLE GetCurrentProcess(void);
__declspec(dllimport) BOOL OpenProcessToken(HANDLE,DWORD,HANDLE *);
__declspec(dllimport) BOOL GetTokenInformation(HANDLE,int,void *,DWORD,DWORD *);
__declspec(dllimport) BOOL ConvertSidToStringSidW(void *,WCHAR **);
__declspec(dllimport) HANDLE LocalFree(HANDLE);

void InitLog(const WCHAR *role,const WCHAR *message,DWORD value);
BOOL InitDirectory(const WCHAR *path);
BOOL InitRegistryString(HKEY root,const WCHAR *key,const WCHAR *name,const WCHAR *value,BOOL replace,DWORD type);
BOOL InitRegistryDword(HKEY root,const WCHAR *key,const WCHAR *name,DWORD value,BOOL replace);
DWORD InitReadDword(HKEY root,const WCHAR *key,const WCHAR *name,DWORD fallback);
BOOL InitReadString(HKEY root,const WCHAR *key,const WCHAR *name,const WCHAR *fallback,WCHAR *out,DWORD chars);
BOOL InitSystem(void);
BOOL InitProfile(WCHAR profile[260]);
BOOL InitSpawn(const WCHAR *application,const WCHAR *command,const WCHAR *directory,const WCHAR *desktop,INIT_PROCESSINFO *process);
BOOL InitStopChild(INIT_PROCESSINFO *process,DWORD code);
BOOL InitSessionState(DWORD state,DWORD pid,DWORD exit_code);
BOOL InitCopy(WCHAR *out,DWORD chars,const WCHAR *text);
BOOL InitAppend(WCHAR *out,DWORD chars,const WCHAR *text);
BOOL InitSingleton(const WCHAR *name,HANDLE *mutex);
void InitCloseProcess(INIT_PROCESSINFO *process);
#endif
