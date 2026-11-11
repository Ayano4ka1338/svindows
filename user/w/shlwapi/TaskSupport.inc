__declspec(dllimport) void SetLastError(unsigned long);
__declspec(dllimport) void *GetProcessHeap(void);
__declspec(dllimport) void *HeapAlloc(void *, unsigned long, unsigned long long);
__declspec(dllimport) int HeapFree(void *, unsigned long, void *);
__declspec(dllimport) void *CreateThread(void *, unsigned long long,
    unsigned long (*)(void *), void *, unsigned long, unsigned long *);
__declspec(dllimport) int CloseHandle(void *);

typedef struct {
    unsigned long (*Procedure)(void *), (*Callback)(void *);
    void *Data;
} SHELL_THREAD;
static unsigned long shell_thread_start(void *parameter)
{
    SHELL_THREAD thread = *(SHELL_THREAD *)parameter;
    HeapFree(GetProcessHeap(), 0, parameter);
    if (thread.Callback) thread.Callback(thread.Data);
    return thread.Procedure(thread.Data);
}
__declspec(dllexport) int SHCreateThread(unsigned long (*procedure)(void *),
    void *data, unsigned long flags, unsigned long (*callback)(void *))
{
    if (!procedure) { SetLastError(87); return 0; }
    /* COM initialization and shell reference flags need services we lack. */
    if (flags & ~1u) { SetLastError(50); return 0; }
    SHELL_THREAD *thread = HeapAlloc(GetProcessHeap(), 0, sizeof(*thread));
    if (!thread) { SetLastError(8); return 0; }
    *thread = (SHELL_THREAD){procedure, callback, data};
    void *handle = CreateThread(0, 0, shell_thread_start, thread, 0, 0);
    if (!handle) { HeapFree(GetProcessHeap(), 0, thread); return 0; }
    CloseHandle(handle);
    return 1;
}

__declspec(dllexport) int StrCmpICW(const WCHAR *a, const WCHAR *b)
{ return CompareStringOrdinal(a, -1, b, -1, 1) - 2; }

__declspec(dllexport) int SHGetFileDescriptionW(const WCHAR *path,
    const WCHAR *version_key, const WCHAR *display_name, WCHAR *out, unsigned *capacity)
{
    (void)path; (void)version_key; (void)display_name;
    if (out && capacity && *capacity) out[0] = 0;
    SetLastError(50); /* Executable version-description lookup is unavailable. */
    return 0;
}
__declspec(dllexport) int SHWindowsPolicy(const void *policy)
{
    (void)policy;
    SetLastError(50); /* No group-policy provider is installed. */
    return 0;
}
