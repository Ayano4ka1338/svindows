/* File mapping, virtual queries, timer queues and character helpers.
 * Included by the owning translation unit. */
/* ------------------------------------------------------------------ */
/* File mapping                                                        */
/* ------------------------------------------------------------------ */

typedef struct _FILE_MAPPING_K {
    void *Base;
    ULONGLONG Size;
    int Used;
} FILE_MAPPING_K;

static FILE_MAPPING_K g_file_mappings[16];

__declspec(dllexport) HANDLE CreateFileMappingW(HANDLE file, void *sa,
                                                DWORD protect,
                                                DWORD max_high, DWORD max_low,
                                                const WCHAR *name)
{
    (void)sa;
    (void)protect;
    (void)name;
    ULONGLONG size = ((ULONGLONG)max_high << 32) | max_low;
    if (!size && file && file != NT_INVALID_HANDLE) {
        DWORD high = 0;
        DWORD low = GetFileSize(file, &high);
        size = ((ULONGLONG)high << 32) | low;
    }
    if (!size) {
        SetLastError(1006); /* ERROR_FILE_INVALID */
        return 0;
    }
    for (int i = 0; i < 16; i++) {
        if (g_file_mappings[i].Used)
            continue;
        void *base = nt_alloc(size);
        if (!base) {
            SetLastError(8);
            return 0;
        }
        if (file && file != NT_INVALID_HANDLE) {
            DWORD high = 0;
            DWORD low = GetFileSize(file, &high);
            ULONGLONG file_size = ((ULONGLONG)high << 32) | low;
            ULONGLONG remaining = file_size < size ? file_size : size;
            SetFilePointerEx(file, 0, 0, 0);
            BYTE *dst = (BYTE *)base;
            while (remaining) {
                DWORD chunk = remaining > 0x10000 ? 0x10000 : (DWORD)remaining;
                DWORD done = 0;
                if (!ReadFile(file, dst, chunk, &done, 0) || !done)
                    break;
                dst += done;
                remaining -= done;
            }
        }
        g_file_mappings[i].Used = 1;
        g_file_mappings[i].Base = base;
        g_file_mappings[i].Size = size;
        return (HANDLE)(ULONGLONG)(i + 1);
    }
    SetLastError(8);
    return 0;
}

__declspec(dllexport) void *MapViewOfFile(HANDLE mapping, DWORD access,
                                          DWORD off_high, DWORD off_low,
                                          SIZE_T bytes)
{
    (void)access;
    UINT index = (UINT)(ULONGLONG)mapping;
    if (!index || index > 16 || !g_file_mappings[index - 1].Used) {
        SetLastError(6);
        return 0;
    }
    FILE_MAPPING_K *m = &g_file_mappings[index - 1];
    ULONGLONG off = ((ULONGLONG)off_high << 32) | off_low;
    if (off > m->Size || (!bytes && off == m->Size)) {
        SetLastError(113); /* ERROR_OFFSET_ALIGNMENT */
        return 0;
    }
    if (!bytes)
        bytes = (SIZE_T)(m->Size - off);
    if (bytes > m->Size - off) {
        SetLastError(113);
        return 0;
    }
    return (BYTE *)m->Base + off;
}

__declspec(dllexport) BOOL UnmapViewOfFile(const void *view)
{
    for (int i = 0; i < 16; i++) {
        if (g_file_mappings[i].Used &&
            (const BYTE *)view >= (const BYTE *)g_file_mappings[i].Base &&
            (const BYTE *)view < (const BYTE *)g_file_mappings[i].Base +
                                 g_file_mappings[i].Size)
            return 1;
    }
    SetLastError(158); /* ERROR_NOT_LOCKED */
    return 0;
}

/* ------------------------------------------------------------------ */
/* File misc                                                           */
/* ------------------------------------------------------------------ */

__declspec(dllexport) BOOL FlushFileBuffers(HANDLE file)
{
    IO_STATUS_BLOCK iosb;
    NTSTATUS status = NtFlushBuffersFile(file, &iosb);
    if (!NT_SUCCESS(status)) {
        SetLastError(ntstatus_to_win32(status));
        return 0;
    }
    SetLastError(0);
    return 1;
}

__declspec(dllexport) DWORD SetFilePointer(HANDLE file, LONG distance,
                                           LONG *high, DWORD method)
{
    long long d = high ? (long long)(((ULONGLONG)(DWORD)*high << 32) |
                                   (DWORD)distance) : distance;
    long long position = 0;
    if (!SetFilePointerEx(file, d, &position, method))
        return 0xFFFFFFFFu;
    if (high)
        *high = (LONG)((ULONGLONG)position >> 32);
    return (DWORD)position;
}

__declspec(dllexport) BOOL SetEndOfFile(HANDLE file)
{
    SVINDOWS_FILE_INFO info;
    NTSTATUS status = NtQueryFileInfo(file, &info);
    IO_STATUS_BLOCK iosb;
    long long end = (long long)info.Position;
    if (NT_SUCCESS(status))
        status = NtSetInformationFile(file, &iosb, &end, sizeof(end), 20);
    if (!NT_SUCCESS(status)) {
        SetLastError(ntstatus_to_win32(status));
        return 0;
    }
    SetLastError(0);
    return 1;
}

__declspec(dllexport) BOOL WritePrivateProfileStringW(const WCHAR *section,
                                                      const WCHAR *key,
                                                      const WCHAR *value,
                                                      const WCHAR *file)
{
    (void)section;
    (void)key;
    (void)value;
    (void)file;
    return 1;
}

typedef struct _FILE_RENAME_INFORMATION_K {
    BYTE ReplaceIfExists;
    BYTE Padding[7];
    HANDLE RootDirectory;
    DWORD FileNameLength;
    WCHAR FileName[260];
} FILE_RENAME_INFORMATION_K;

static BOOL set_delete_disposition(const WCHAR *path, BOOL directory)
{
    DWORD attributes = GetFileAttributesW(path);
    if (attributes == (DWORD)-1 ||
        (!!(attributes & 0x10) != !!directory)) {
        SetLastError(directory ? 3 : 2);
        return 0;
    }
    HANDLE file = CreateFileW(path, 0x00010000u, 7, 0, 3,
                              directory ? 0x02000000u : 0, 0);
    if (file == NT_INVALID_HANDLE)
        return 0;
    BYTE disposition = 1;
    IO_STATUS_BLOCK iosb;
    NTSTATUS status = NtSetInformationFile(file, &iosb, &disposition,
                                           sizeof(disposition), 13);
    CloseHandle(file);
    if (!NT_SUCCESS(status)) {
        SetLastError(ntstatus_to_win32(status));
        return 0;
    }
    SetLastError(0);
    return 1;
}

__declspec(dllexport) BOOL DeleteFileW(const WCHAR *path)
{ return path ? set_delete_disposition(path, 0) : 0; }

__declspec(dllexport) BOOL MoveFileW(const WCHAR *from, const WCHAR *to)
{
    if (!from || !to) { SetLastError(87); return 0; }
    HANDLE file = CreateFileW(from, 0x00010000u, 7, 0, 3,
                              0x02000000u, 0);
    if (file == NT_INVALID_HANDLE)
        return 0;
    FILE_RENAME_INFORMATION_K rename;
    memset(&rename, 0, sizeof(rename));
    DWORD chars = 0;
    while (to[chars] && chars < 260) {
        rename.FileName[chars] = to[chars];
        chars++;
    }
    if (to[chars]) {
        CloseHandle(file);
        SetLastError(206);
        return 0;
    }
    rename.FileNameLength = chars * sizeof(WCHAR);
    IO_STATUS_BLOCK iosb;
    NTSTATUS status = NtSetInformationFile(file, &iosb, &rename,
        20 + rename.FileNameLength, 10);
    CloseHandle(file);
    if (!NT_SUCCESS(status)) {
        SetLastError(ntstatus_to_win32(status));
        return 0;
    }
    SetLastError(0);
    return 1;
}

__declspec(dllexport) BOOL CreateDirectoryW(const WCHAR *path, void *security)
{
    (void)security;
    char utf8[260];
    if (!path || !wide_path_to_utf8(path, utf8)) {
        SetLastError(87);
        return 0;
    }
    NTSTATUS status = NtFileSystemControlSimple(3, utf8, 0);
    if (!NT_SUCCESS(status)) {
        SetLastError(ntstatus_to_win32(status));
        return 0;
    }
    SetLastError(0);
    return 1;
}

__declspec(dllexport) BOOL RemoveDirectoryW(const WCHAR *path)
{ return path ? set_delete_disposition(path, 1) : 0; }

static BOOL ascii_path_call(DWORD operation, const char *first,
                            const char *second)
{
    WCHAR a[260], b[260];
    if (!first) { SetLastError(87); return 0; }
    if (!utf8_path_to_wide(first, a)) { SetLastError(206); return 0; }
    const WCHAR *bp = 0;
    if (second) {
        if (!utf8_path_to_wide(second, b)) { SetLastError(206); return 0; }
        bp = b;
    }
    if (operation == 1) return DeleteFileW(a);
    if (operation == 2) return MoveFileW(a, bp);
    if (operation == 4) return RemoveDirectoryW(a);
    char utf8[260];
    if (!wide_path_to_utf8(a, utf8)) { SetLastError(206); return 0; }
    NTSTATUS status = NtFileSystemControlSimple(operation, utf8, 0);
    if (!NT_SUCCESS(status)) {
        SetLastError(ntstatus_to_win32(status));
        return 0;
    }
    SetLastError(0);
    return 1;
}

__declspec(dllexport) BOOL DeleteFileA(const char *path)
{ return ascii_path_call(1, path, 0); }
__declspec(dllexport) BOOL MoveFileA(const char *from, const char *to)
{ return ascii_path_call(2, from, to); }
__declspec(dllexport) BOOL CreateDirectoryA(const char *path, void *security)
{ (void)security; return ascii_path_call(3, path, 0); }
__declspec(dllexport) BOOL RemoveDirectoryA(const char *path)
{ return ascii_path_call(4, path, 0); }

__declspec(dllexport) DWORD GetFileAttributesA(const char *path)
{
    NTSTATUS status = NtFileSystemControlSimple(5, path, 0);
    if (!NT_SUCCESS(status)) { SetLastError(2); return (DWORD)-1; }
    SetLastError(0);
    return (DWORD)status;
}

__declspec(dllexport) BOOL CopyFileW(const WCHAR *from, const WCHAR *to,
                                     BOOL fail_if_exists)
{
    HANDLE src = CreateFileW(from, 0x80000000u, 1, 0, 3, 0, 0);
    if (src == NT_INVALID_HANDLE)
        return 0;
    DWORD high = 0;
    DWORD low = GetFileSize(src, &high);
    ULONGLONG remaining = ((ULONGLONG)high << 32) | low;
    HANDLE dst = CreateFileW(to, 0x40000000u, 0, 0,
                             fail_if_exists ? 1 : 2, 0, 0);
    if (dst == NT_INVALID_HANDLE) {
        CloseHandle(src);
        return 0;
    }
    BYTE *buffer = nt_alloc(0x10000);
    BOOL ok = buffer != 0;
    while (ok && remaining) {
        DWORD chunk = remaining > 0x10000 ? 0x10000 : (DWORD)remaining;
        DWORD done = 0;
        ok = ReadFile(src, buffer, chunk, &done, 0) && done;
        if (ok) {
            DWORD written = 0;
            ok = WriteFile(dst, buffer, done, &written, 0) &&
                 written == done;
        }
        remaining -= done;
    }
    if (buffer)
        HeapFree(GetProcessHeap(), 0, buffer);
    CloseHandle(src);
    CloseHandle(dst);
    return ok;
}

/* ------------------------------------------------------------------ */
/* Virtual query                                                       */
/* ------------------------------------------------------------------ */

typedef struct _MBI_K {
    void *BaseAddress;
    void *AllocationBase;
    DWORD AllocationProtect;
    DWORD PartitionId;
    ULONGLONG RegionSize;
    DWORD State;
    DWORD Protect;
    DWORD Type;
    DWORD Reserved;
} MBI_K;

__declspec(dllexport) SIZE_T VirtualQuery(const void *address, MBI_K *buffer,
                                          SIZE_T length)
{
    if (!buffer || length < sizeof(MBI_K))
        return 0;
    ULONGLONG returned = 0;
    NTSTATUS status = NtQueryVirtualMemory(NT_INVALID_HANDLE, address, 0,
                                            buffer, length, &returned);
    if (!NT_SUCCESS(status)) {
        SetLastError(ntstatus_to_win32(status));
        return 0;
    }
    SetLastError(0);
    return returned;
}

/* ------------------------------------------------------------------ */
/* Machine, power, packages, timer queue                               */
/* ------------------------------------------------------------------ */

__declspec(dllexport) BOOL GetComputerNameW(WCHAR *buffer, DWORD *size)
{
    static const WCHAR name[] = {
        'S', 'V', 'I', 'N', 'D', 'O', 'W', 'S', 0
    };
    if (!size) {
        SetLastError(87);
        return 0;
    }
    if (!buffer || *size < 9) {
        *size = 9;
        SetLastError(122); /* ERROR_BUFFER_OVERFLOW */
        return 0;
    }
    for (int i = 0; i <= 8; i++)
        buffer[i] = name[i];
    *size = 8;
    return 1;
}

__declspec(dllexport) DWORD RegisterApplicationRestart(const WCHAR *cmdline,
                                                       DWORD flags)
{
    (void)cmdline;
    (void)flags;
    return 0;
}

__declspec(dllexport) BOOL SetTermsrvAppInstallMode(BOOL mode)
{
    (void)mode;
    return 1;
}

__declspec(dllexport) HANDLE PowerCreateRequest(void *context)
{
    (void)context;
    return nt_alloc(64);
}

__declspec(dllexport) BOOL PowerSetRequest(HANDLE request, DWORD type)
{
    (void)request;
    (void)type;
    return 1;
}

__declspec(dllexport) DWORD GetPackageFullName(HANDLE process, DWORD *length,
                                               WCHAR *name)
{
    (void)process;
    (void)name;
    if (length)
        *length = 0;
    return 15700; /* APPMODEL_ERROR_NO_PACKAGE */
}

__declspec(dllexport) DWORD ParseApplicationUserModelId(
    const WCHAR *package_id, WCHAR **package, WCHAR **app)
{
    (void)package_id;
    (void)package;
    (void)app;
    return 15700;
}

__declspec(dllexport) LONG FindPackagesByPackageFamily(
    const WCHAR *family, DWORD pin_options, DWORD *count, void **tokens,
    UINT *total)
{
    (void)family;
    (void)pin_options;
    (void)tokens;
    if (count)
        *count = 0;
    if (total)
        *total = 0;
    return 15700;
}

__declspec(dllexport) LONG GetPackagesByPackageFamily(const WCHAR *family,
                                                      UINT *count, void **tokens,
                                                      UINT *total)
{
    (void)family;
    (void)tokens;
    if (count)
        *count = 0;
    if (total)
        *total = 0;
    return 15700;
}

__declspec(dllexport) BOOL CheckElevationEnabled(DWORD *enabled)
{
    if (enabled)
        *enabled = 0;
    return 1;
}

typedef void (*WAITORTIMERCALLBACK_K)(void *param, int fired);

typedef struct _TIMERQ_K {
    WAITORTIMERCALLBACK_K Callback;
    void *Param;
    DWORD Due;
    DWORD Period;
    volatile LONG Stop;
    HANDLE Thread;
} TIMERQ_K;

static DWORD WINAPI timerq_thread(LPVOID argument)
{
    TIMERQ_K *timer = (TIMERQ_K *)argument;
    if (timer->Due)
        Sleep(timer->Due);
    while (!timer->Stop) {
        timer->Callback(timer->Param, 0);
        if (!timer->Period)
            break;
        Sleep(timer->Period);
    }
    timer->Callback(timer->Param, 1);
    return 0;
}

__declspec(dllexport) BOOL CreateTimerQueueTimer(PHANDLE *out, HANDLE queue,
                                                 WAITORTIMERCALLBACK_K callback,
                                                 void *param, DWORD due,
                                                 DWORD period, DWORD flags)
{
    (void)queue;
    (void)flags;
    if (!out || !callback)
        return 0;
    TIMERQ_K *timer = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                sizeof(*timer));
    if (!timer)
        return 0;
    timer->Callback = callback;
    timer->Param = param;
    timer->Due = due;
    timer->Period = period;
    timer->Thread = CreateThread(0, 0, timerq_thread, timer, 0, 0);
    if (!timer->Thread) {
        HeapFree(GetProcessHeap(), 0, timer);
        return 0;
    }
    *out = (PHANDLE)timer;
    return 1;
}

__declspec(dllexport) BOOL DeleteTimerQueueTimer(HANDLE queue, HANDLE timer,
                                                 HANDLE event)
{
    (void)queue;
    TIMERQ_K *t = (TIMERQ_K *)timer;
    if (!t)
        return 0;
    __sync_fetch_and_or(&t->Stop, 1);
    if (event == (HANDLE)(ULONGLONG)-1)
        WaitForSingleObject(t->Thread, 0xFFFFFFFFu);
    return 1;
}

__declspec(dllexport) BOOL ChangeTimerQueueTimer(HANDLE queue, HANDLE timer,
                                                 DWORD due, DWORD period)
{
    (void)queue;
    TIMERQ_K *t = (TIMERQ_K *)timer;
    if (!t)
        return 0;
    t->Due = due;
    t->Period = period;
    return 1;
}

typedef struct _WAITQ_K {
    WAITORTIMERCALLBACK_K Callback;
    void *Param;
    HANDLE Wait;
    DWORD Timeout;
    volatile LONG Stop;
} WAITQ_K;

static DWORD WINAPI waitq_thread(LPVOID argument)
{
    WAITQ_K *waiter = (WAITQ_K *)argument;
    DWORD status = WaitForSingleObject(waiter->Wait, waiter->Timeout);
    if (!waiter->Stop)
        waiter->Callback(waiter->Param, status != 0);
    return 0;
}

__declspec(dllexport) BOOL RegisterWaitForSingleObject(PHANDLE *out,
                                                       HANDLE object,
                                                       WAITORTIMERCALLBACK_K callback,
                                                       void *param,
                                                       DWORD timeout,
                                                       DWORD flags)
{
    (void)flags;
    if (!out || !callback)
        return 0;
    WAITQ_K *waiter = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                sizeof(*waiter));
    if (!waiter)
        return 0;
    waiter->Callback = callback;
    waiter->Param = param;
    waiter->Wait = object;
    waiter->Timeout = timeout;
    HANDLE thread = CreateThread(0, 0, waitq_thread, waiter, 0, 0);
    if (!thread) {
        HeapFree(GetProcessHeap(), 0, waiter);
        return 0;
    }
    *out = (PHANDLE)waiter;
    return 1;
}

__declspec(dllexport) BOOL UnregisterWaitEx(HANDLE wait, HANDLE event)
{
    (void)event;
    WAITQ_K *w = (WAITQ_K *)wait;
    if (!w)
        return 0;
    __sync_fetch_and_or(&w->Stop, 1);
    return 1;
}

__declspec(dllexport) BOOL DisableThreadLibraryCalls(HANDLE module)
{
    (void)module;
    return 1;
}

__declspec(dllexport) BOOL SetStdHandle(DWORD which, HANDLE handle)
{
    PEB *peb = NtCurrentPeb();
    unsigned char *pp = (unsigned char *)peb->ProcessParameters;
    if (!pp) {
        SetLastError(87);
        return 0;
    }
    DWORD offset = which == (DWORD)-10 ? 0x20 :
                   which == (DWORD)-11 ? 0x28 :
                   which == (DWORD)-12 ? 0x30 : 0;
    if (!offset) {
        SetLastError(87);
        return 0;
    }
    *(HANDLE *)(pp + offset) = handle;
    return 1;
}

/* ------------------------------------------------------------------ */
/* Character-walk helpers (api-ms-win-core-string / -stringansi)       */
/*                                                                     */
/* USER32 exports CharNextW and friends, but its own implementation is  */
/* a jump through its IAT into these contracts, which on Windows land   */
/* in KERNELBASE. Stubbing them to 0 is what made notepad's command     */
/* line parser dereference NULL: it advances through the command line   */
/* with CharNextW. No DBCS code page is active and surrogate pairs are  */
/* not split by these APIs, so a single code unit is one step.          */
/* ------------------------------------------------------------------ */

__declspec(dllexport) WCHAR *CharNextW(const WCHAR *text)
{
    if (!text)
        return 0;
    if (!*text)
        return (WCHAR *)text; /* stays on the terminator, as documented */
    return (WCHAR *)(text + 1);
}

__declspec(dllexport) WCHAR *CharPrevW(const WCHAR *start, const WCHAR *text)
{
    if (!start || !text || text <= start)
        return (WCHAR *)start;
    return (WCHAR *)(text - 1);
}

__declspec(dllexport) char *CharNextA(const char *text)
{
    if (!text)
        return 0;
    if (!*text)
        return (char *)text;
    return (char *)(text + 1);
}

__declspec(dllexport) char *CharPrevA(const char *start, const char *text)
{
    if (!start || !text || text <= start)
        return (char *)start;
    return (char *)(text - 1);
}

__declspec(dllexport) char *CharNextExA(WORD code_page, const char *text,
                                        DWORD flags)
{
    (void)code_page;
    (void)flags;
    return CharNextA(text);
}

__declspec(dllexport) char *CharPrevExA(WORD code_page, const char *start,
                                        const char *text, DWORD flags)
{
    (void)code_page;
    (void)flags;
    return CharPrevA(start, text);
}

/* ------------------------------------------------------------------ */
/* String resources                                                    */
/*                                                                     */
/* USER32's LoadStringW is a jump into KERNELBASE!LoadStringBaseExW,    */
/* which this facade hosts. Stubbing it to 0 is not a harmless partial  */
/* implementation: notepad loads 49 UI strings at startup and refuses   */
/* to run if any is missing, which presents as a clean exit code 0.     */
/* ------------------------------------------------------------------ */

#define RT_STRING_ID 6

/*
 * RT_STRING resources are stored sixteen strings to a block: the block's
 * resource id is id/16 + 1, and inside it each string is a 16-bit length
 * followed by that many UTF-16 code units, with empty entries for unused ids.
 */
/* Bring-up trace: the first few string loads, with the values that decide
 * whether a program that demands its whole string table starts at all. */
static void res_trace(UINT id, const void *info, const void *data, int len)
{
    extern long NtDisplayString(const char *text);
    static int traced;
    if (traced >= 16)
        return;
    traced++;
    static const char hex[] = "0123456789abcdef";
    char m[96];
    int n = 0;
    const char *p = "[k32] LoadString id=";
    while (*p) m[n++] = *p++;
    for (int i = 0; i < 4; i++) m[n++] = hex[(id >> (12 - 4 * i)) & 0xF];
    p = " info=0x";
    while (*p) m[n++] = *p++;
    for (int i = 0; i < 12; i++)
        m[n++] = hex[((ULONGLONG)info >> (44 - 4 * i)) & 0xF];
    p = " data=0x";
    while (*p) m[n++] = *p++;
    for (int i = 0; i < 12; i++)
        m[n++] = hex[((ULONGLONG)data >> (44 - 4 * i)) & 0xF];
    p = " len=";
    while (*p) m[n++] = *p++;
    for (int i = 0; i < 4; i++) m[n++] = hex[((UINT)len >> (12 - 4 * i)) & 0xF];
    m[n++] = '\n';
    m[n] = 0;
    NtDisplayString(m);
}

static int res_load_string(HANDLE module, UINT id, WCHAR *buf, int cch,
                           WORD lang)
{
    HANDLE info = FindResourceExW(module, (const WCHAR *)(ULONGLONG)RT_STRING_ID,
                                  (const WCHAR *)(ULONGLONG)(id / 16 + 1),
                                  lang);
    if (!info) {
        res_trace(id, 0, 0, -1);
        return 0;
    }
    const WCHAR *p = (const WCHAR *)LoadResource(module, info);
    if (!p) {
        res_trace(id, info, 0, -2);
        return 0;
    }
    for (UINT i = 0; i < (id % 16); i++)
        p += 1 + *p;
    int len = (int)*p++;
    res_trace(id, info, p, len);

    /* cch == 0 is the documented "give me a pointer into the resource" form. */
    if (cch == 0) {
        if (buf)
            *(const WCHAR **)buf = p;
        return len;
    }
    if (!buf)
        return 0;
    if (len > cch - 1)
        len = cch - 1;
    for (int i = 0; i < len; i++)
        buf[i] = p[i];
    buf[len] = 0;
    return len;
}

__declspec(dllexport) int LoadStringBaseExW(HANDLE module, UINT id, WCHAR *buf,
                                            int cch, WORD lang)
{
    return res_load_string(module, id, buf, cch, lang);
}

__declspec(dllexport) int LoadStringBaseW(HANDLE module, UINT id, WCHAR *buf,
                                          int cch)
{
    return res_load_string(module, id, buf, cch, 0);
}


__declspec(dllexport) int LoadStringA(HANDLE module, UINT id, char *buf,
                                     int cch)
{
    WCHAR wide[512];
    int len = res_load_string(module, id, wide,
                              cch > 512 ? 512 : cch, 0);
    if (!buf || cch <= 0)
        return len;
    int i = 0;
    for (; i < len && i < cch - 1; i++)
        buf[i] = (char)(wide[i] < 0x100 ? wide[i] : '?');
    buf[i] = 0;
    return i;
}

/* MinGW's Win64 CRT calls this while converting locale-sensitive text. */
__declspec(dllexport) BOOL IsDBCSLeadByteEx(UINT codepage, BYTE value)
{
    if (codepage == 932)
        return (value >= 0x81 && value <= 0x9f) ||
               (value >= 0xe0 && value <= 0xfc);
    if (codepage == 936 || codepage == 949 || codepage == 950)
        return value >= 0x81 && value <= 0xfe;
    return 0;
}
__declspec(dllexport) BOOL SetHandleInformation(HANDLE handle,
                                                 DWORD mask,
                                                 DWORD flags)
{
    (void)mask;
    (void)flags;
    if (!handle) {
        SetLastError(6); /* ERROR_INVALID_HANDLE */
        return 0;
    }
    SetLastError(0);
    return 1;
}

__declspec(dllexport) BOOL GetHandleInformation(HANDLE handle, DWORD *flags)
{
    if (!handle || !flags) { SetLastError(6); return 0; }
    *flags = 0;
    SetLastError(0);
    return 1;
}

__declspec(dllexport) BOOL GetProcessAffinityMask(HANDLE process,
                                                  DWORD_PTR *process_mask,
                                                  DWORD_PTR *system_mask)
{
    (void)process;
    if (!process_mask || !system_mask) { SetLastError(87); return 0; }
    *process_mask = 1; *system_mask = 1;
    return 1;
}
__declspec(dllexport) BOOL SetProcessAffinityMask(HANDLE process,
                                                  DWORD_PTR mask)
{
    (void)process;
    if (!mask) { SetLastError(87); return 0; }
    return 1;
}

__declspec(dllexport) BOOL GetProcessTimes(HANDLE process, void *creation,
                                            void *exit_time, void *kernel,
                                            void *user)
{
    (void)process;
    if (creation) memset(creation, 0, 8);
    if (exit_time) memset(exit_time, 0, 8);
    if (kernel) memset(kernel, 0, 8);
    if (user) memset(user, 0, 8);
    return 1;
}
__declspec(dllexport) BOOL GetThreadTimes(HANDLE thread, void *creation,
                                           void *exit_time, void *kernel,
                                           void *user)
{
    return GetProcessTimes(thread, creation, exit_time, kernel, user);
}
__declspec(dllexport) BOOL GetSystemTimeAdjustment(DWORD *adjustment,
                                                    DWORD *increment,
                                                    BOOL *disabled)
{
    if (adjustment) *adjustment = 0;
    if (increment) *increment = 10000;
    if (disabled) *disabled = 1;
    return 1;
}
__declspec(dllexport) BOOL SetSystemTime(const void *time)
{
    (void)time; SetLastError(5); return 0;
}
__declspec(dllexport) BOOL GetThreadContext(HANDLE thread, void *context)
{
    (void)thread; (void)context; SetLastError(50); return 0;
}
__declspec(dllexport) BOOL SetThreadContext(HANDLE thread, const void *context)
{
    (void)thread; (void)context; SetLastError(50); return 0;
}
__declspec(dllexport) BOOL IsDebuggerPresent(void) { return 0; }

#include "FormatMessage.inc"

__declspec(dllexport) DWORD FormatMessageA(DWORD flags, const void *source,
                                            DWORD message, DWORD language,
                                            char *buffer, DWORD size,
                                            void *arguments)
{
    (void)flags; (void)source; (void)message; (void)language; (void)arguments;
    if (buffer && size) buffer[0] = 0;
    SetLastError(317);
    return 0;
}

__declspec(dllexport) BOOL GetFileSizeEx(HANDLE file, long long *size)
{
    if (!size) { SetLastError(87); return 0; }
    DWORD high = 0, low = GetFileSize(file, &high);
    if (low == 0xffffffffu) return 0;
    *size = ((long long)high << 32) | low;
    return 1;
}

__declspec(dllexport) BOOL GetDiskFreeSpaceExW(const WCHAR *path,
                                                ULONGLONG *available,
                                                ULONGLONG *total,
                                                ULONGLONG *free_bytes)
{
    (void)path;
    if (available) *available = 512ULL * 1024 * 1024;
    if (total) *total = 1024ULL * 1024 * 1024;
    if (free_bytes) *free_bytes = 512ULL * 1024 * 1024;
    return 1;
}

__declspec(dllexport) DWORD GetTempPathW(DWORD capacity, WCHAR *buffer)
{
    static const WCHAR path[] = { 'C', ':', '\\', 'T', 'E', 'M', 'P', '\\', 0 };
    DWORD n = 8;
    if (!buffer || capacity <= n) { SetLastError(122); return n + 1; }
    for (DWORD i = 0; i <= n; ++i) buffer[i] = path[i];
    return n;
}

__declspec(dllexport) BOOL GetVolumeInformationW(const WCHAR *root,
                                                   WCHAR *volume_name,
                                                   DWORD volume_capacity,
                                                   DWORD *serial, DWORD *max_component,
                                                   DWORD *flags, WCHAR *fs_name,
                                                   DWORD fs_capacity)
{
    (void)root;
    static const WCHAR volume[] = { 'S','v','i','n','d','o','w','s',0 };
    static const WCHAR fs[] = { 'F','A','T','3','2',0 };
    if (volume_name && volume_capacity >= 9)
        for (DWORD i = 0; i < 9; ++i) volume_name[i] = volume[i];
    if (fs_name && fs_capacity >= 6)
        for (DWORD i = 0; i < 6; ++i) fs_name[i] = fs[i];
    if (serial) *serial = 0x5356494e;
    if (max_component) *max_component = 255;
    if (flags) *flags = 0;
    return 1;
}

__declspec(dllexport) BOOL MoveFileExW(const WCHAR *from, const WCHAR *to,
                                        DWORD flags)
{
    (void)flags;
    return MoveFileW(from, to);
}

__declspec(dllexport) BOOL CreateHardLinkW(const WCHAR *link_name,
                                            const WCHAR *existing,
                                            void *security)
{
    (void)link_name; (void)existing; (void)security;
    SetLastError(1);
    return 0;
}

__declspec(dllexport) HANDLE FindFirstVolumeW(WCHAR *name, DWORD capacity)
{
    (void)name; (void)capacity;
    SetLastError(18);
    return (HANDLE)(ULONGLONG)-1;
}
__declspec(dllexport) BOOL FindNextVolumeW(HANDLE find, WCHAR *name, DWORD capacity)
{
    (void)find; (void)name; (void)capacity;
    SetLastError(18);
    return 0;
}
__declspec(dllexport) BOOL FindVolumeClose(HANDLE find)
{
    (void)find;
    return 1;
}
