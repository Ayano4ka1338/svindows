#ifndef SVINDOWS_EXPLORER_WIN32_H
#define SVINDOWS_EXPLORER_WIN32_H
#include <Comctl32.h>

typedef unsigned long long SIZE_T;
typedef struct { DWORD low, high; } SHELL_FILETIME;
typedef struct {
    DWORD attributes;
    SHELL_FILETIME creation, access, write;
    DWORD size_high, size_low, reserved0, reserved1;
    WCHAR name[260], alternate[14];
} SHELL_FIND_DATA;
typedef struct {
    DWORD cb;
    WCHAR *reserved, *desktop, *title;
    DWORD x, y, width, height, chars_x, chars_y, fill, flags;
    WORD show, reserved_size;
    BYTE *reserved_data;
    HANDLE input, output, error;
} SHELL_STARTUP_INFO;
typedef struct { HANDLE process, thread; DWORD process_id, thread_id; } SHELL_PROCESS_INFO;
typedef DWORD (*SHELL_THREAD_START)(void *);
typedef struct {
    DWORD files_offset;
    POINT point;
    BOOL nonclient, wide;
} SHELL_DROPFILES;
typedef struct { UINT size,mask;int minimum,maximum;UINT page;int position,track; } SHELL_SCROLL_INFO;
typedef struct { WORD year,month,day_of_week,day,hour,minute,second,milliseconds; } SHELL_SYSTEMTIME;
typedef DWORD (*SHELL_COPY_PROGRESS)(long long, long long, long long, long long,
    DWORD, DWORD, HANDLE, HANDLE, void *);
typedef struct {
    DWORD size, flags; HWND owner;
    const WCHAR *verb, *file, *parameters, *directory;
    int show; HANDLE instance; void *id_list;
    const WCHAR *class_name; HANDLE class_key; DWORD hot_key;
    HANDLE icon; HANDLE process;
} SHELL_EXECUTE_INFO;

#define SHELL_KERNEL_API __declspec(dllimport)
SHELL_KERNEL_API HANDLE GetProcessHeap(void);
SHELL_KERNEL_API void *HeapAlloc(HANDLE, DWORD, SIZE_T);
SHELL_KERNEL_API void *HeapReAlloc(HANDLE, DWORD, void *, SIZE_T);
SHELL_KERNEL_API BOOL HeapFree(HANDLE, DWORD, void *);
SHELL_KERNEL_API DWORD GetLastError(void);
SHELL_KERNEL_API void SetLastError(DWORD);
SHELL_KERNEL_API WCHAR *GetCommandLineW(void);
SHELL_KERNEL_API int CompareStringOrdinal(const WCHAR *, int, const WCHAR *, int, BOOL);
SHELL_KERNEL_API int MultiByteToWideChar(UINT, DWORD, const char *, int, WCHAR *, int);
SHELL_KERNEL_API DWORD GetEnvironmentVariableW(const WCHAR *, WCHAR *, DWORD);
SHELL_KERNEL_API DWORD ExpandEnvironmentStringsW(const WCHAR *, WCHAR *, DWORD);
SHELL_KERNEL_API DWORD SearchPathW(const WCHAR *, const WCHAR *, const WCHAR *,
    DWORD, WCHAR *, WCHAR **);
SHELL_KERNEL_API DWORD GetPrivateProfileStringW(const WCHAR *, const WCHAR *,
    const WCHAR *, WCHAR *, DWORD, const WCHAR *);
SHELL_KERNEL_API BOOL CreateProcessW(const WCHAR *, WCHAR *, void *, void *, BOOL,
    DWORD, void *, const WCHAR *, SHELL_STARTUP_INFO *, SHELL_PROCESS_INFO *);
SHELL_KERNEL_API HANDLE CreateThread(void *, SIZE_T, SHELL_THREAD_START, void *, DWORD, DWORD *);
SHELL_KERNEL_API DWORD WaitForSingleObject(HANDLE, DWORD);
SHELL_KERNEL_API BOOL CloseHandle(HANDLE);
SHELL_KERNEL_API HANDLE OpenEventW(DWORD,BOOL,const WCHAR *);
SHELL_KERNEL_API DWORD GetTickCount(void);
SHELL_KERNEL_API DWORD GetCurrentProcessId(void);
SHELL_KERNEL_API void GetLocalTime(SHELL_SYSTEMTIME *);
SHELL_KERNEL_API void ExitProcess(DWORD);
SHELL_KERNEL_API HANDLE GetStdHandle(DWORD);
SHELL_KERNEL_API BOOL WriteFile(HANDLE, const void *, DWORD, DWORD *, void *);
SHELL_KERNEL_API HANDLE FindFirstFileW(const WCHAR *, SHELL_FIND_DATA *);
SHELL_KERNEL_API BOOL FindNextFileW(HANDLE, SHELL_FIND_DATA *);
SHELL_KERNEL_API BOOL FindClose(HANDLE);
SHELL_KERNEL_API DWORD GetFileAttributesW(const WCHAR *);
SHELL_KERNEL_API BOOL CreateDirectoryW(const WCHAR *, void *);
SHELL_KERNEL_API BOOL RemoveDirectoryW(const WCHAR *);
SHELL_KERNEL_API BOOL DeleteFileW(const WCHAR *);
SHELL_KERNEL_API BOOL MoveFileW(const WCHAR *, const WCHAR *);
SHELL_KERNEL_API BOOL CopyFileExW(const WCHAR *, const WCHAR *, SHELL_COPY_PROGRESS,
    void *, BOOL *, DWORD);
SHELL_KERNEL_API HANDLE CreateFileW(const WCHAR *, DWORD, DWORD, void *, DWORD, DWORD, HANDLE);
SHELL_KERNEL_API BOOL FlushFileBuffers(HANDLE);
SHELL_KERNEL_API DWORD FormatMessageW(DWORD, const void *, DWORD, DWORD, WCHAR *, DWORD, void *);
SHELL_KERNEL_API HANDLE GlobalAlloc(UINT, SIZE_T);
SHELL_KERNEL_API void *GlobalLock(HANDLE);
SHELL_KERNEL_API BOOL GlobalUnlock(HANDLE);
SHELL_KERNEL_API SIZE_T GlobalSize(HANDLE);
SHELL_KERNEL_API HANDLE GlobalFree(HANDLE);
SHELL_KERNEL_API HANDLE LocalFree(HANDLE);

USER32_API BOOL EnumWindows(BOOL (*)(HWND, LPARAM), LPARAM);
USER32_API BOOL SetShellWindow(HWND);
USER32_API BOOL RegisterShellHookWindow(HWND);
USER32_API BOOL DeregisterShellHookWindow(HWND);
USER32_API UINT RegisterWindowMessageW(const WCHAR *);
USER32_API BOOL SystemParametersInfoW(UINT, UINT, void *, UINT);
USER32_API BOOL SetForegroundWindow(HWND);
USER32_API BOOL IsWindow(HWND);
USER32_API BOOL IsWindowVisible(HWND);
USER32_API BOOL IsIconic(HWND);
USER32_API HWND GetAncestor(HWND, UINT);
USER32_API LONG_PTR GetWindowLongPtrW(HWND, int);
USER32_API LONG_PTR SetWindowLongPtrW(HWND, int, LONG_PTR);
USER32_API int GetClassNameW(HWND, WCHAR *, int);
USER32_API DWORD GetWindowThreadProcessId(HWND, DWORD *);
USER32_API BOOL EnableWindow(HWND, BOOL);
USER32_API LRESULT CallWindowProcW(WNDPROC, HWND, UINT, WPARAM, LPARAM);
USER32_API UINT_PTR SetTimer(HWND, UINT_PTR, UINT, void (*)(HWND, UINT, UINT_PTR, DWORD));
USER32_API BOOL KillTimer(HWND, UINT_PTR);
USER32_API int SetScrollInfo(HWND,int,const SHELL_SCROLL_INFO *,BOOL);
USER32_API BOOL GetScrollInfo(HWND,int,SHELL_SCROLL_INFO *);
USER32_API int FillRect(HDC, const RECT *, HBRUSH);
USER32_API HMENU CreatePopupMenu(void);
USER32_API BOOL AppendMenuW(HMENU, UINT, UINT_PTR, const WCHAR *);
USER32_API BOOL DestroyMenu(HMENU);
USER32_API BOOL TrackPopupMenu(HMENU, UINT, int, int, int, HWND, const RECT *);
USER32_API BOOL OpenClipboard(HWND);
USER32_API BOOL CloseClipboard(void);
USER32_API BOOL EmptyClipboard(void);
USER32_API HANDLE GetClipboardData(UINT);
USER32_API HANDLE SetClipboardData(UINT, HANDLE);
USER32_API UINT RegisterClipboardFormatW(const WCHAR *);
USER32_API DWORD GetClipboardSequenceNumber(void);
__declspec(dllimport) BOOL ShellExecuteExW(SHELL_EXECUTE_INFO *);
__declspec(dllimport) WCHAR **CommandLineToArgvW(const WCHAR *,int *);

#define GWLP_WNDPROC (-4)
#define GWLP_USERDATA (-21)
#define GWL_STYLE (-16)
#define GWL_EXSTYLE (-20)
#define HWND_BOTTOM ((HWND)(UINT_PTR)1)
#define HWND_TOPMOST ((HWND)(INT_PTR)-1)
#define WS_EX_TOOLWINDOW 0x80u
#define WS_EX_TOPMOST 0x8u
#define WS_EX_NOACTIVATE 0x08000000u
#define CS_DBLCLKS 0x8u
#define SPI_SETWORKAREA 0x2Fu
#define SPI_GETWORKAREA 0x30u
#define SPIF_SENDCHANGE 2u
#define WM_APP 0x8000u
#define WM_MOUSEWHEEL 0x020Au
#define WM_CONTEXTMENU 0x007Bu
#define WM_DISPLAYCHANGE 0x007Eu
#define WM_SETTINGCHANGE 0x001Au
#define VK_F2 0x71u
#define VK_F5 0x74u
#define VK_F10 0x79u
#define VK_F4 0x73u
#define VK_PRIOR 0x21u
#define VK_NEXT 0x22u
#define VK_LWIN 0x5Bu
#define VK_RWIN 0x5Cu
#define MF_STRING 0u
#define MF_SEPARATOR 0x800u
#define MF_GRAYED 1u
#define TPM_RETURNCMD 0x100u
#define CF_HDROP 15u
#define GMEM_MOVEABLE 2u
#define INVALID_HANDLE_VALUE ((HANDLE)(INT_PTR)-1)
#define INVALID_FILE_ATTRIBUTES 0xFFFFFFFFu
#define FILE_ATTRIBUTE_DIRECTORY 0x10u
#define FILE_ATTRIBUTE_HIDDEN 2u
#define FILE_ATTRIBUTE_SYSTEM 4u
#define FILE_ATTRIBUTE_NORMAL 0x80u
#define HEAP_ZERO_MEMORY 8u
#define INFINITE 0xFFFFFFFFu
#define GA_ROOT 2u
#define GA_ROOTOWNER 3u
#endif
