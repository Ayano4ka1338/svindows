/* Small shell32 compatibility surface needed to load the Windows 7 inbox
 * calculator. Keep the API contract Win32-shaped; optional shell integration
 * reports the same unsupported result instead of pretending to launch a verb.
 */
typedef void *HANDLE;
typedef void *HWND;
typedef void *HICON;
typedef unsigned short WCHAR;
typedef unsigned long DWORD;
typedef long HRESULT;
typedef int BOOL;

__declspec(dllimport) BOOL CreateDirectoryW(const WCHAR *path,
                                            void *security);
__declspec(dllimport) void SetLastError(DWORD error);
__declspec(dllimport) DWORD GetLastError(void);
__declspec(dllexport) int SHCreateDirectory(HWND owner, const WCHAR *path);

#define S_OK ((HRESULT)0)
#define E_FAIL ((HRESULT)0x80004005u)
#define ERROR_CALL_NOT_IMPLEMENTED 120
#define MAX_PATH 260

static WCHAR *copy_path(WCHAR *out, const WCHAR *src)
{
    unsigned i = 0;
    if (!out || !src) return 0;
    while (src[i] && i + 1 < MAX_PATH) { out[i] = src[i]; i++; }
    if (src[i]) return 0;
    out[i] = 0;
    return out;
}

static const WCHAR *folder_path(int folder)
{
    static const WCHAR root[] = { 'C', ':', '\\', 0 };
    static const WCHAR profile[] = {
        'C', ':', '\\', 'U','s','e','r','s','\\','u','s','e','r',0
    };
    static const WCHAR appdata[] = {
        'C', ':', '\\', 'U','s','e','r','s','\\','u','s','e','r',
        '\\','A','p','p','D','a','t','a','\\','R','o','a','m','i','n','g',0
    };
    static const WCHAR localappdata[] = {
        'C', ':', '\\', 'U','s','e','r','s','\\','u','s','e','r',
        '\\','A','p','p','D','a','t','a','\\','L','o','c','a','l',0
    };
    switch (folder & 0xff) {
    case 0x05: /* CSIDL_PERSONAL */
    case 0x10: /* CSIDL_MYDOCUMENTS */
    case 0x28: /* CSIDL_PROFILE */
        return profile;
    case 0x1a: /* CSIDL_APPDATA */
        return appdata;
    case 0x1c: /* CSIDL_LOCAL_APPDATA */
        return localappdata;
    default:
        return root;
    }
}

__declspec(dllexport) BOOL SHGetSpecialFolderPathW(HWND owner,
                    WCHAR *path, int folder, BOOL create)
{
    (void)owner;
    if (create) (void)SHCreateDirectory(0, folder_path(folder));
    return copy_path(path, folder_path(folder)) != 0;
}

__declspec(dllexport) HRESULT SHGetFolderPathW(HWND owner, int folder,
                    HANDLE token, DWORD flags, WCHAR *path)
{
    (void)owner; (void)token; (void)flags;
    return copy_path(path, folder_path(folder)) ? S_OK : E_FAIL;
}

__declspec(dllexport) BOOL ShellAboutW(HWND owner, const WCHAR *title,
                                       const WCHAR *message, HICON icon)
{
    (void)owner; (void)title; (void)message; (void)icon;
    return 1;
}

__declspec(dllexport) int SHCreateDirectory(HWND owner, const WCHAR *path)
{
    (void)owner;
    if (!path) return 87;
    if (CreateDirectoryW(path, 0)) return 0;
    return (int)GetLastError();
}

typedef struct _SHELLEXECUTEINFOW {
    DWORD cbSize, fMask;
    HWND hwnd;
    const WCHAR *lpVerb, *lpFile, *lpParameters, *lpDirectory;
    int nShow;
    HANDLE hInstApp;
    void *lpIDList;
    const WCHAR *lpClass;
    HANDLE hkeyClass;
    DWORD dwHotKey;
    union { HANDLE hIcon; HANDLE hMonitor; } DUMMYUNIONNAME;
    HANDLE hProcess;
} SHELLEXECUTEINFOW;

__declspec(dllexport) BOOL ShellExecuteExW(SHELLEXECUTEINFOW *info)
{
    if (info) info->hInstApp = (HANDLE)(unsigned long long)2;
    SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
    return 0;
}

__declspec(dllimport) long GetWindowLongW(HWND window, int index);
__declspec(dllimport) long SetWindowLongW(HWND window, int index, long value);
__declspec(dllimport) void *GlobalLock(HANDLE memory);
__declspec(dllimport) BOOL GlobalUnlock(HANDLE memory);
__declspec(dllimport) unsigned long long GlobalSize(HANDLE memory);
__declspec(dllimport) HANDLE GlobalFree(HANDLE memory);

typedef struct {
    DWORD FileOffset;
    long X, Y;
    BOOL NonClient, Wide;
} DROPFILES;

__declspec(dllexport) void DragAcceptFiles(HWND window, BOOL accept)
{
    const int exstyle = -20; /* GWL_EXSTYLE */
    const long accept_files = 0x10; /* WS_EX_ACCEPTFILES */
    long style = GetWindowLongW(window, exstyle);
    style = accept ? style | accept_files : style & ~accept_files;
    SetWindowLongW(window, exstyle, style);
}

__declspec(dllexport) unsigned DragQueryFileW(HANDLE drop, unsigned index,
    WCHAR *out, unsigned capacity)
{
    unsigned long long size = GlobalSize(drop);
    if (size < sizeof(DROPFILES) || size == (unsigned long long)-1) return 0;
    const DROPFILES *files = GlobalLock(drop);
    if (!files) return 0;
    unsigned result = 0;
    unsigned offset = files->FileOffset;
    if (offset >= sizeof(*files) && offset < size && (!files->Wide || !(offset & 1))) {
        const unsigned char *names = (const unsigned char *)files + offset;
        unsigned step = files->Wide ? sizeof(WCHAR) : 1;
        unsigned long long length = (size - offset) / step;
        unsigned count = 0;
        for (unsigned long long start = 0; start < length;) {
            unsigned long long end = start;
            while (end < length && (files->Wide ? ((const WCHAR *)names)[end] : names[end]))
                end++;
            if (end == start || end == length) break;
            if (index == count) {
                result = (unsigned)(end - start);
                if (out && capacity) {
                    if (result >= capacity) result = capacity - 1;
                    for (unsigned i = 0; i < result; i++)
                        out[i] = files->Wide ? ((const WCHAR *)names)[start + i] : names[start + i];
                    out[result] = 0;
                }
                break;
            }
            count++;
            start = end + 1;
        }
        if (index == (unsigned)-1) result = count;
    }
    GlobalUnlock(drop);
    return result;
}

__declspec(dllexport) void DragFinish(HANDLE drop)
{
    GlobalFree(drop);
}
