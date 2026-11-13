/* Public clipboard APIs over the station-owned broker. Cached client handles
 * are process-local views; the broker owns the authoritative transferable data. */
#include "../../../kernel/include/abi/Clipboard.h"
__declspec(dllimport) UINT64 SvNtUserClipboardControl(UINT operation, UINT64 argument, UINT64 buffer, UINT64 capacity);
__declspec(dllimport) UINT32 SvNtUserWriteClipboard(UINT format, const void *data, UINT64 bytes);
__declspec(dllimport) UINT32 SvNtUserReadClipboard(UINT format, void *data, UINT64 bytes, SV_CLIPBOARD_DATA *metadata);
__declspec(dllimport) ULONG RtlNtStatusToDosError(UINT32 status);
__declspec(dllimport) HANDLE GlobalAlloc(UINT flags, UINT64 bytes);
__declspec(dllimport) HANDLE GlobalFree(HANDLE memory);
__declspec(dllimport) UINT64 GlobalSize(HANDLE memory);
__declspec(dllimport) void *GlobalLock(HANDLE memory);
__declspec(dllimport) BOOL GlobalUnlock(HANDLE memory);
__declspec(dllimport) HANDLE GetProcessHeap(void);
__declspec(dllimport) void *HeapAlloc(HANDLE heap, DWORD flags, UINT64 bytes);
__declspec(dllimport) BOOL HeapFree(HANDLE heap, DWORD flags, void *data);
__declspec(dllimport) DWORD WaitForSingleObject(HANDLE handle, DWORD timeout);
__declspec(dllimport) BOOL CloseHandle(HANDLE handle);
__declspec(dllimport) int MultiByteToWideChar(UINT codepage, DWORD flags, const char *text, int bytes, WCHAR *wide, int count);
__declspec(dllimport) int WideCharToMultiByte(UINT codepage, DWORD flags, const WCHAR *wide, int count,
    char *text, int bytes, const char *fallback, BOOL *used);
__declspec(dllimport) void EnterCriticalSection(void *section);
__declspec(dllimport) void LeaveCriticalSection(void *section);
USER32_API HANDLE GetProcessWindowStation(void);

typedef struct {
    void *DebugInfo;
    LONG LockCount, RecursionCount;
    HANDLE Owner, Semaphore;
    UINT64 SpinCount;
} SV_CLIPBOARD_LOCK;
static SV_CLIPBOARD_LOCK g_clipboard_cache_lock = {.LockCount = -1};
typedef struct _SV_CLIPBOARD_VIEW {
    struct _SV_CLIPBOARD_VIEW *Next;
    HANDLE Station, Memory;
    UINT64 Revision, Version;
    UINT Format;
} SV_CLIPBOARD_VIEW;
static SV_CLIPBOARD_VIEW *g_clipboard_views;

static void sv_clipboard_expire(HANDLE station, UINT64 revision)
{
    EnterCriticalSection(&g_clipboard_cache_lock);
    SV_CLIPBOARD_VIEW **link = &g_clipboard_views;
    while (*link) {
        SV_CLIPBOARD_VIEW *view = *link;
        if (view->Station != station || view->Revision == revision) { link = &view->Next; continue; }
        *link = view->Next;
        BOOL shared = FALSE;
        for (SV_CLIPBOARD_VIEW *other = g_clipboard_views; other; other = other->Next)
            if (other->Memory == view->Memory) { shared = TRUE; break; }
        if (!shared) GlobalFree(view->Memory);
        HeapFree(GetProcessHeap(), 0, view);
    }
    LeaveCriticalSection(&g_clipboard_cache_lock);
}

static HANDLE sv_clipboard_find(HANDLE station, UINT format, const SV_CLIPBOARD_DATA *metadata)
{
    HANDLE result = NULL;
    EnterCriticalSection(&g_clipboard_cache_lock);
    for (SV_CLIPBOARD_VIEW *view = g_clipboard_views; view; view = view->Next)
        if (view->Station == station && view->Format == format &&
            view->Revision == metadata->Revision && view->Version == metadata->Version) {
            result = view->Memory; break;
        }
    LeaveCriticalSection(&g_clipboard_cache_lock);
    return result;
}

static BOOL sv_clipboard_cache(HANDLE station, UINT format, HANDLE memory,
    UINT64 revision, UINT64 version)
{
    SV_CLIPBOARD_VIEW *view = HeapAlloc(GetProcessHeap(), 0, sizeof(*view));
    if (!view) { SetLastError(8); return FALSE; }
    *view = (SV_CLIPBOARD_VIEW){.Station=station, .Memory=memory,
        .Format=format, .Revision=revision, .Version=version};
    EnterCriticalSection(&g_clipboard_cache_lock);
    view->Next = g_clipboard_views; g_clipboard_views = view;
    LeaveCriticalSection(&g_clipboard_cache_lock);
    return TRUE;
}

USER32_API BOOL OpenClipboard(HWND owner)
{
    if (!SvNtUserClipboardControl(SV_CLIP_OPEN, (UINT_PTR)owner, 0, 0)) return FALSE;
    sv_clipboard_expire(GetProcessWindowStation(), SvNtUserClipboardControl(SV_CLIP_CONTENT_REVISION, 0, 0, 0));
    return TRUE;
}
USER32_API BOOL CloseClipboard(void)
{ return SvNtUserClipboardControl(SV_CLIP_CLOSE, 0, 0, 0) != 0; }
USER32_API BOOL EmptyClipboard(void)
{
    if (!SvNtUserClipboardControl(SV_CLIP_EMPTY, 0, 0, 0)) return FALSE;
    sv_clipboard_expire(GetProcessWindowStation(), SvNtUserClipboardControl(SV_CLIP_CONTENT_REVISION, 0, 0, 0));
    return TRUE;
}
USER32_API BOOL IsClipboardFormatAvailable(UINT format)
{ return SvNtUserClipboardControl(SV_CLIP_AVAILABLE, format, 0, 0) != 0; }
USER32_API UINT EnumClipboardFormats(UINT previous)
{ return (UINT)SvNtUserClipboardControl(SV_CLIP_ENUMERATE, previous, 0, 0); }
USER32_API int CountClipboardFormats(void)
{ return (int)SvNtUserClipboardControl(SV_CLIP_COUNT, 0, 0, 0); }
USER32_API DWORD GetClipboardSequenceNumber(void)
{ return (DWORD)SvNtUserClipboardControl(SV_CLIP_SEQUENCE, 0, 0, 0); }
USER32_API HWND GetClipboardOwner(void)
{ return (HWND)(UINT_PTR)SvNtUserClipboardControl(SV_CLIP_OWNER, 0, 0, 0); }
USER32_API HWND GetOpenClipboardWindow(void)
{ return (HWND)(UINT_PTR)SvNtUserClipboardControl(SV_CLIP_OPEN_WINDOW, 0, 0, 0); }
USER32_API BOOL AddClipboardFormatListener(HWND window)
{ return SvNtUserClipboardControl(SV_CLIP_ADD_LISTENER, (UINT_PTR)window, 0, 0) != 0; }
USER32_API BOOL RemoveClipboardFormatListener(HWND window)
{ return SvNtUserClipboardControl(SV_CLIP_REMOVE_LISTENER, (UINT_PTR)window, 0, 0) != 0; }
USER32_API UINT RegisterClipboardFormatW(const WCHAR *name)
{ return (UINT)SvNtUserClipboardControl(SV_CLIP_REGISTER_FORMAT, (UINT_PTR)name, 0, 0); }
USER32_API UINT RegisterClipboardFormatA(const char *name)
{
    WCHAR wide[256];
    if (!name || !MultiByteToWideChar(0, 0, name, -1, wide, 256)) return 0;
    return RegisterClipboardFormatW(wide);
}
USER32_API int GetClipboardFormatNameW(UINT format, WCHAR *name, int capacity)
{
    if (capacity <= 0) { SetLastError(87); return 0; }
    return (int)SvNtUserClipboardControl(SV_CLIP_FORMAT_NAME, format, (UINT_PTR)name, (UINT)capacity);
}
USER32_API int GetClipboardFormatNameA(UINT format, char *name, int capacity)
{
    WCHAR wide[256];
    if (!name || capacity <= 0 || !GetClipboardFormatNameW(format, wide, 256)) return 0;
    int bytes = WideCharToMultiByte(0, 0, wide, -1, name, capacity, NULL, NULL);
    return bytes > 0 ? bytes - 1 : 0;
}

USER32_API HANDLE SetClipboardData(UINT format, HANDLE memory)
{
    SV_CLIPBOARD_VIEW *pending = memory ? HeapAlloc(GetProcessHeap(), 0, sizeof(*pending)) : NULL;
    if (memory && !pending) { SetLastError(8); return NULL; }
    void *data = NULL;
    UINT64 bytes = 0;
    if (memory) {
        bytes = GlobalSize(memory);
        data = GlobalLock(memory);
        if (!data) { HeapFree(GetProcessHeap(), 0, pending); return NULL; }
    }
    UINT32 status = SvNtUserWriteClipboard(format, data, bytes);
    if (memory) GlobalUnlock(memory);
    if ((INT32)status < 0) {
        if (pending) HeapFree(GetProcessHeap(), 0, pending);
        SetLastError(RtlNtStatusToDosError(status)); return NULL;
    }
    if (memory) {
        HANDLE station = GetProcessWindowStation();
        UINT64 revision = SvNtUserClipboardControl(SV_CLIP_CONTENT_REVISION, 0, 0, 0);
        UINT64 version = SvNtUserClipboardControl(SV_CLIP_SEQUENCE, 0, 0, 0);
        *pending = (SV_CLIPBOARD_VIEW){.Station=station, .Memory=memory,
            .Format=format, .Revision=revision, .Version=version};
        EnterCriticalSection(&g_clipboard_cache_lock);
        pending->Next = g_clipboard_views; g_clipboard_views = pending;
        LeaveCriticalSection(&g_clipboard_cache_lock);
    }
    if (!memory) SetLastError(0); /* successful delayed-render registration */
    return memory;
}

static UINT32 sv_clipboard_read(UINT format, void *data, UINT64 bytes, SV_CLIPBOARD_DATA *metadata)
{
    for (;;) {
        UINT32 status = SvNtUserReadClipboard(format, data, bytes, metadata);
        if (status != 0x103u) return status; /* STATUS_PENDING: delayed owner rendering */
        HANDLE changed = (HANDLE)(UINT_PTR)SvNtUserClipboardControl(SV_CLIP_CHANGE_EVENT, 0, 0, 0);
        if (!changed) return 0xc0000017u;
        DWORD waited = WaitForSingleObject(changed, (DWORD)-1);
        CloseHandle(changed);
        if (waited != 0) return 0xc0000001u;
    }
}

static HANDLE sv_clipboard_convert_text(UINT format, UINT source, HANDLE memory,
    const SV_CLIPBOARD_DATA *metadata)
{
    void *data = GlobalLock(memory);
    if (!data) return NULL;
    UINT64 bytes = metadata->Bytes;
    WCHAR *wide = NULL;
    BOOL allocated_wide = FALSE;
    int chars = 0;
    if (source == 13) {
        if (bytes < sizeof(WCHAR) || (bytes & 1) || bytes / sizeof(WCHAR) > 0x7fffffff) goto invalid;
        wide = data;
        while ((UINT64)chars < bytes / sizeof(WCHAR) && wide[chars]) chars++;
        if ((UINT64)chars == bytes / sizeof(WCHAR)) goto invalid;
        chars++;
    } else {
        const char *text = data;
        UINT64 length = 0;
        while (length < bytes && text[length]) length++;
        if (length == bytes || length >= 0x7fffffff) goto invalid;
        chars = MultiByteToWideChar(source == 7 ? 1 : 0, 0, text, (int)length + 1, NULL, 0);
        if (!chars) goto invalid;
        wide = HeapAlloc(GetProcessHeap(), 0, (UINT64)chars * sizeof(WCHAR));
        if (!wide) goto no_memory;
        allocated_wide = TRUE;
        if (!MultiByteToWideChar(source == 7 ? 1 : 0, 0, text, (int)length + 1, wide, chars)) goto invalid;
    }
    if (format == 13 && chars > 0x7fffffff / (int)sizeof(WCHAR)) goto no_memory;
    int output_bytes = format == 13 ? chars * (int)sizeof(WCHAR) :
        WideCharToMultiByte(format == 7 ? 1 : 0, 0, wide, chars, NULL, 0, NULL, NULL);
    if (output_bytes <= 0) goto invalid;
    HANDLE output = GlobalAlloc(0x42, (UINT)output_bytes);
    if (!output) goto no_memory;
    void *target = GlobalLock(output);
    if (format == 13) {
        WCHAR *to = target;
        for (int i = 0; i < chars; ++i) to[i] = wide[i];
    } else if (!WideCharToMultiByte(format == 7 ? 1 : 0, 0, wide, chars,
                                      target, output_bytes, NULL, NULL)) {
        GlobalUnlock(output); GlobalFree(output); goto invalid;
    }
    GlobalUnlock(output);
    if (allocated_wide) HeapFree(GetProcessHeap(), 0, wide);
    GlobalUnlock(memory);
    return output;
invalid:
    if (allocated_wide) HeapFree(GetProcessHeap(), 0, wide);
    GlobalUnlock(memory); SetLastError(13); return NULL;
no_memory:
    if (allocated_wide) HeapFree(GetProcessHeap(), 0, wide);
    GlobalUnlock(memory); SetLastError(8); return NULL;
}

USER32_API HANDLE GetClipboardData(UINT format)
{
    UINT source = format;
    if (!SvNtUserClipboardControl(SV_CLIP_EXACT_FORMAT, format, 0, 0) &&
        (format == 1 || format == 7 || format == 13)) {
        source = SvNtUserClipboardControl(SV_CLIP_EXACT_FORMAT, 13, 0, 0) ? 13 :
                 SvNtUserClipboardControl(SV_CLIP_EXACT_FORMAT, 1, 0, 0) ? 1 : 7;
    }
    SV_CLIPBOARD_DATA metadata;
    UINT32 status = sv_clipboard_read(source, NULL, 0, &metadata);
    if ((INT32)status < 0) { SetLastError(RtlNtStatusToDosError(status)); return NULL; }
    HANDLE station = GetProcessWindowStation();
    sv_clipboard_expire(station, metadata.Revision);
    HANDLE cached = sv_clipboard_find(station, format, &metadata);
    if (cached) return cached;
    HANDLE memory = GlobalAlloc(0x42, metadata.Bytes ? metadata.Bytes : 1);
    if (!memory) { SetLastError(8); return NULL; }
    void *data = GlobalLock(memory);
    status = sv_clipboard_read(source, data, metadata.Bytes, &metadata);
    GlobalUnlock(memory);
    if ((INT32)status < 0) { GlobalFree(memory); SetLastError(RtlNtStatusToDosError(status)); return NULL; }
    if (source != format) {
        HANDLE converted = sv_clipboard_convert_text(format, source, memory, &metadata);
        GlobalFree(memory);
        memory = converted;
        if (!memory) return NULL;
    }
    if (!sv_clipboard_cache(station, format, memory, metadata.Revision, metadata.Version)) {
        GlobalFree(memory); return NULL;
    }
    return memory;
}

USER32_API int GetPriorityClipboardFormat(const UINT *formats, int count)
{
    if (!formats || count <= 0) { SetLastError(87); return 0; }
    if (!CountClipboardFormats()) return 0;
    for (int i = 0; i < count; ++i) if (IsClipboardFormatAvailable(formats[i])) return (int)formats[i];
    return -1;
}
