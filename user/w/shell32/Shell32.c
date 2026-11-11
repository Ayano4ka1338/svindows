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

#include "CommandLine.inc"

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

__declspec(dllimport) void *CoTaskMemAlloc(unsigned long long bytes);
__declspec(dllimport) void CoTaskMemFree(void *memory);
__declspec(dllimport) DWORD GetFullPathNameW(const WCHAR *, DWORD, WCHAR *, WCHAR **);
__declspec(dllimport) DWORD GetFileAttributesW(const WCHAR *);
__declspec(dllimport) int CompareStringOrdinal(const WCHAR *, int, const WCHAR *, int, BOOL);

typedef struct {
    DWORD Data1;
    unsigned short Data2, Data3;
    unsigned char Data4[8];
} SHELL_GUID;

typedef struct SHELL_ITEM SHELL_ITEM;
typedef struct {
    HRESULT (*QueryInterface)(SHELL_ITEM *, const SHELL_GUID *, void **);
    DWORD (*AddRef)(SHELL_ITEM *);
    DWORD (*Release)(SHELL_ITEM *);
    HRESULT (*BindToHandler)(SHELL_ITEM *, void *, const SHELL_GUID *,
                            const SHELL_GUID *, void **);
    HRESULT (*GetParent)(SHELL_ITEM *, SHELL_ITEM **);
    HRESULT (*GetDisplayName)(SHELL_ITEM *, DWORD, WCHAR **);
    HRESULT (*GetAttributes)(SHELL_ITEM *, DWORD, DWORD *);
    HRESULT (*Compare)(SHELL_ITEM *, SHELL_ITEM *, DWORD, int *);
} SHELL_ITEM_VTABLE;

struct SHELL_ITEM {
    const SHELL_ITEM_VTABLE *Vtable;
    DWORD References, Attributes;
    WCHAR Path[MAX_PATH];
};

static const SHELL_GUID iid_shell_item = {0x43826d1e, 0xe718, 0x42ee,
    {0xbc, 0x55, 0xa1, 0xe2, 0x61, 0xc3, 0x7b, 0xfe}};
static const SHELL_GUID iid_unknown = {0, 0, 0, {0xc0, 0, 0, 0, 0, 0, 0, 0x46}};

#define SHELL_E_POINTER ((HRESULT)0x80004003u)
#define SHELL_E_NOTIMPL ((HRESULT)0x80004001u)
#define SHELL_E_NOINTERFACE ((HRESULT)0x80004002u)

static BOOL equal_guid(const SHELL_GUID *a, const SHELL_GUID *b)
{
    if (!a || a->Data1 != b->Data1 || a->Data2 != b->Data2 || a->Data3 != b->Data3)
        return 0;
    for (unsigned i = 0; i < sizeof(a->Data4); i++)
        if (a->Data4[i] != b->Data4[i]) return 0;
    return 1;
}

static DWORD shell_item_add_ref(SHELL_ITEM *item)
{
    return __atomic_add_fetch(&item->References, 1, __ATOMIC_RELAXED);
}

static DWORD shell_item_release(SHELL_ITEM *item)
{
    DWORD count = __atomic_sub_fetch(&item->References, 1, __ATOMIC_ACQ_REL);
    if (!count) CoTaskMemFree(item);
    return count;
}

static HRESULT shell_item_query(SHELL_ITEM *item, const SHELL_GUID *iid, void **out)
{
    if (!out) return SHELL_E_POINTER;
    *out = 0;
    if (!equal_guid(iid, &iid_shell_item) && !equal_guid(iid, &iid_unknown))
        return SHELL_E_NOINTERFACE;
    *out = item;
    shell_item_add_ref(item);
    return S_OK;
}

static HRESULT shell_item_bind(SHELL_ITEM *item, void *ctx, const SHELL_GUID *handler,
    const SHELL_GUID *iid, void **out)
{
    (void)item; (void)ctx; (void)handler; (void)iid;
    if (out) *out = 0;
    return SHELL_E_NOTIMPL;
}

static HRESULT make_shell_item(const WCHAR *path, SHELL_ITEM **out);

static const WCHAR *item_basename(const WCHAR *path)
{
    const WCHAR *name = path;
    for (const WCHAR *p = path; *p; p++)
        if (*p == '\\' && p[1]) name = p + 1;
    return name;
}

static HRESULT shell_item_parent(SHELL_ITEM *item, SHELL_ITEM **out)
{
    if (!out) return SHELL_E_POINTER;
    *out = 0;
    const WCHAR *name = item_basename(item->Path);
    if (name == item->Path) return (HRESULT)0x80070490u;
    WCHAR parent[MAX_PATH];
    unsigned n = (unsigned)(name - item->Path);
    if (n > 3) n--;
    for (unsigned i = 0; i < n; i++) parent[i] = item->Path[i];
    parent[n] = 0;
    return make_shell_item(parent, out);
}

static HRESULT shell_item_name(SHELL_ITEM *item, DWORD format, WCHAR **out)
{
    if (!out) return SHELL_E_POINTER;
    *out = 0;
    const WCHAR *text;
    switch (format) {
    case 0: /* SIGDN_NORMALDISPLAY */
    case 0x80080001u: /* SIGDN_PARENTRELATIVEEDITING */
        text = item_basename(item->Path);
        break;
    case 0x80028000u: /* SIGDN_DESKTOPABSOLUTEPARSING */
    case 0x80058000u: /* SIGDN_FILESYSPATH */
        text = item->Path;
        break;
    default: return SHELL_E_NOTIMPL;
    }
    unsigned size = 1;
    while (text[size - 1]) size++;
    WCHAR *copy = CoTaskMemAlloc(size * sizeof(*copy));
    if (!copy) return (HRESULT)0x8007000eu;
    for (unsigned i = 0; i < size; i++) copy[i] = text[i];
    *out = copy;
    return S_OK;
}

static HRESULT shell_item_attributes(SHELL_ITEM *item, DWORD mask, DWORD *out)
{
    if (!out) return SHELL_E_POINTER;
    *out = item->Attributes & mask;
    return *out == mask ? S_OK : 1; /* S_FALSE */
}

static HRESULT shell_item_compare(SHELL_ITEM *item, SHELL_ITEM *other, DWORD hint, int *order)
{
    (void)hint;
    if (!other || !order) return SHELL_E_POINTER;
    WCHAR *path = 0;
    HRESULT status = other->Vtable->GetDisplayName(other, 0x80058000u, &path);
    if (status < 0) return status;
    int result = CompareStringOrdinal(item->Path, -1, path, -1, 1);
    CoTaskMemFree(path);
    if (!result) return E_FAIL;
    *order = result - 2;
    return S_OK;
}

static const SHELL_ITEM_VTABLE shell_item_vtable = {
    shell_item_query, shell_item_add_ref, shell_item_release, shell_item_bind,
    shell_item_parent, shell_item_name, shell_item_attributes, shell_item_compare
};

static HRESULT make_shell_item(const WCHAR *path, SHELL_ITEM **out)
{
    WCHAR full[MAX_PATH];
    DWORD size = GetFullPathNameW(path, MAX_PATH, full, 0);
    if (!size || size >= MAX_PATH) return (HRESULT)0x80070057u;
    DWORD attributes = GetFileAttributesW(full);
    if (attributes == (DWORD)-1) return (HRESULT)0x80070002u;
    SHELL_ITEM *item = CoTaskMemAlloc(sizeof(*item));
    if (!item) return (HRESULT)0x8007000eu;
    item->Vtable = &shell_item_vtable;
    item->References = 1;
    item->Attributes = 0x40000000u; /* SFGAO_FILESYSTEM */
    if (attributes & 0x10) item->Attributes |= 0x20000000u; /* SFGAO_FOLDER */
    copy_path(item->Path, full);
    *out = item;
    return S_OK;
}

__declspec(dllexport) HRESULT SHCreateItemFromParsingName(const WCHAR *name,
    void *context, const SHELL_GUID *iid, void **out)
{
    (void)context;
    if (!out) return SHELL_E_POINTER;
    *out = 0;
    SHELL_ITEM *item = 0;
    HRESULT status = make_shell_item(name, &item);
    if (status < 0) return status;
    status = shell_item_query(item, iid, out);
    shell_item_release(item);
    return status;
}

__declspec(dllexport) void SHAddToRecentDocs(unsigned format, const void *item)
{
    /* The shell does not maintain a recent-items folder. */
    (void)format;
    (void)item;
}

/* Svindows has no stock Shell icon resources or system icon cache yet. */
typedef struct {
    DWORD Size;
    HANDLE Icon;
    int SystemIndex, ResourceIndex;
    WCHAR Path[MAX_PATH];
} SHELL_STOCK_ICON_INFO;
_Static_assert(sizeof(SHELL_STOCK_ICON_INFO) == 544, "SHSTOCKICONINFO x64");

__declspec(dllexport) HRESULT SHGetStockIconInfo(int id, unsigned flags,
                                                SHELL_STOCK_ICON_INFO *info)
{
    (void)flags;
    if (!info || info->Size != sizeof(*info) || id < 0 || id >= 181)
        return (HRESULT)0x80070057u;
    return SHELL_E_NOTIMPL;
}

#include "TaskSupport.inc"
