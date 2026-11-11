/* Identity, TLS, directory enumeration, synchronization, threadpool and VM.
 * Included by the owning translation unit. */
/* ------------------------------------------------------------------ */
/* Thread / process information (from the TEB)                         */
/* ------------------------------------------------------------------ */

/* TEB offsets: ClientId.UniqueProcess 0x40, UniqueThread 0x48, LastError 0x68. */
__declspec(dllexport) DWORD  GetLastError(void)        { return read_gs_dword(0x68); }
__declspec(dllexport) void   SetLastError(DWORD e)     { write_gs_dword(0x68, e); }
__declspec(dllexport) DWORD  GetCurrentProcessId(void) { return (DWORD)read_gs_qword(0x40); }
__declspec(dllexport) DWORD  GetCurrentThreadId(void)  { return (DWORD)read_gs_qword(0x48); }
__declspec(dllexport) HANDLE GetCurrentProcess(void)   { return (HANDLE)(ULONGLONG)-1; }
__declspec(dllexport) HANDLE GetCurrentThread(void)    { return (HANDLE)(ULONGLONG)-2; }

extern NTSTATUS NtOpenThread(HANDLE *thread, DWORD desired_access,
                             void *attributes, const void *client_id);

__declspec(dllexport) HANDLE OpenThread(DWORD desired_access,
                                        BOOL inherit_handle, DWORD thread_id)
{
    struct { HANDLE Process; HANDLE Thread; } id = {
        0, (HANDLE)(ULONGLONG)thread_id
    };
    (void)inherit_handle; /* Handle inheritance is attached at process spawn. */
    HANDLE thread = 0;
    NTSTATUS status = NtOpenThread(&thread, desired_access, 0, &id);
    if (!NT_SUCCESS(status)) {
        SetLastError(ntstatus_to_win32(status));
        return 0;
    }
    return thread;
}

/* Anonymous waitable timers are kernel dispatcher objects, not user slots. */
__declspec(dllexport) HANDLE CreateWaitableTimerW(void *attributes, BOOL manual,
                                                  const WCHAR *name)
{
    if (attributes || name) { SetLastError(120); return 0; }
    HANDLE timer = 0;
    NTSTATUS status = NtCreateTimer(&timer, 0x001F0003, 0, manual ? 0 : 1);
    if (!NT_SUCCESS(status)) { SetLastError(ntstatus_to_win32(status)); return 0; }
    return timer;
}
__declspec(dllexport) HANDLE CreateWaitableTimerA(void *attributes, BOOL manual,
                                                  const char *name)
{
    if (name) { SetLastError(120); return 0; }
    return CreateWaitableTimerW(attributes, manual, 0);
}
__declspec(dllexport) BOOL SetWaitableTimer(HANDLE timer, const long long *due,
                                            long period, void *callback,
                                            void *context, BOOL resume)
{
    NTSTATUS status = NtSetTimer(timer, due, callback, context, resume, period, 0);
    if (!NT_SUCCESS(status)) { SetLastError(ntstatus_to_win32(status)); return 0; }
    return 1;
}
__declspec(dllexport) BOOL CancelWaitableTimer(HANDLE timer)
{
    NTSTATUS status = NtCancelTimer(timer, 0);
    if (!NT_SUCCESS(status)) { SetLastError(ntstatus_to_win32(status)); return 0; }
    return 1;
}

/* ------------------------------------------------------------------ */
/* Thread-local storage                                                */
/* ------------------------------------------------------------------ */

/* Win64 has 64 inline TLS slots.  Keep the process allocation bitmap in
 * kernel32 and hang each thread's slot vector from TEB+0x58
 * (ThreadLocalStoragePointer).  The real TEB also mirrors these values at
 * TlsSlots, but using the canonical pointer is sufficient for the public
 * Tls* API and avoids requiring an oversized TEB while that layout grows. */
#define TLS_MINIMUM_AVAILABLE 64
#define TLS_OUT_OF_INDEXES    ((DWORD)-1)

static volatile ULONGLONG g_tls_bitmap;

/* The kernel allocates static TLS indices (one per module carrying a PE TLS
 * directory) from 0 upward and publishes the next free index in the PEB. Mark
 * those bits used so TlsAlloc continues after them instead of handing a
 * static index to a caller. */
static ULONGLONG tls_static_mask(void)
{
    PEB *peb = NtCurrentPeb();
    if (!peb)
        return 0;
    DWORD next = *(volatile DWORD *)((BYTE *)peb + 0x100);
    if (next > TLS_MINIMUM_AVAILABLE)
        next = TLS_MINIMUM_AVAILABLE;
    return next >= 64 ? ~(ULONGLONG)0 : (((ULONGLONG)1 << next) - 1);
}

static void **tls_slots(BOOL create)
{
    void **slots = (void **)(ULONGLONG)read_gs_qword(0x58);
    if (!slots && create) {
        void **fresh = (void **)nt_alloc(TLS_MINIMUM_AVAILABLE * sizeof(void *));
        if (!fresh)
            return 0;
        memset(fresh, 0, TLS_MINIMUM_AVAILABLE * sizeof(void *));

        /* Only the current thread writes its own TEB, so no cross-thread CAS
         * is needed here. */
        __asm__ volatile("movq %0, %%gs:0x58" : : "r"(fresh) : "memory");
        slots = fresh;
    }
    return slots;
}

__declspec(dllexport) DWORD TlsAlloc(void)
{
    ULONGLONG reserved = tls_static_mask();
    for (;;) {
        ULONGLONG old = g_tls_bitmap;
        ULONGLONG combined = old | reserved;
        if (combined == ~(ULONGLONG)0) {
            SetLastError(8); /* ERROR_NOT_ENOUGH_MEMORY */
            return TLS_OUT_OF_INDEXES;
        }
        DWORD index = 0;
        while (combined & ((ULONGLONG)1 << index))
            index++;
        ULONGLONG updated = old | ((ULONGLONG)1 << index);
        if (__sync_bool_compare_and_swap(&g_tls_bitmap, old, updated)) {
            SetLastError(0);
            return index;
        }
    }
}

__declspec(dllexport) BOOL TlsFree(DWORD index)
{
    if (index >= TLS_MINIMUM_AVAILABLE) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return 0;
    }
    ULONGLONG mask = (ULONGLONG)1 << index;
    ULONGLONG old;
    do {
        old = g_tls_bitmap;
        if (!(old & mask)) {
            SetLastError(87);
            return 0;
        }
    } while (!__sync_bool_compare_and_swap(&g_tls_bitmap, old, old & ~mask));

    void **slots = tls_slots(0);
    if (slots)
        slots[index] = 0;
    SetLastError(0);
    return 1;
}

__declspec(dllexport) void *TlsGetValue(DWORD index)
{
    if (index >= TLS_MINIMUM_AVAILABLE ||
        !(g_tls_bitmap & ((ULONGLONG)1 << index))) {
        SetLastError(87);
        return 0;
    }
    void **slots = tls_slots(0);
    SetLastError(0); /* NULL is a valid stored TLS value. */
    return slots ? slots[index] : 0;
}

__declspec(dllexport) BOOL TlsSetValue(DWORD index, void *value)
{
    if (index >= TLS_MINIMUM_AVAILABLE ||
        !(g_tls_bitmap & ((ULONGLONG)1 << index))) {
        SetLastError(87);
        return 0;
    }
    void **slots = tls_slots(1);
    if (!slots) {
        SetLastError(8);
        return 0;
    }
    slots[index] = value;
    SetLastError(0);
    return 1;
}

__declspec(dllexport) BOOL TerminateProcess(HANDLE process, DWORD code)
{
    extern NTSTATUS NtTerminateProcess(HANDLE, NTSTATUS);
    NTSTATUS status = NtTerminateProcess(process, (NTSTATUS)code);
    if (!NT_SUCCESS(status)) {
        SetLastError(ntstatus_to_win32(status));
        return 0;
    }
    return 1;
}

__declspec(dllexport) DWORD SetThreadUILanguage(DWORD language)
{
    /* Zero selects the process/user default and must still return a valid
     * LANGID.  Several inbox command-line utilities treat zero as fatal. */
    return language ? language : LANG_ENGLISH_US;
}

__declspec(dllexport) DWORD GetFileType(HANDLE file)
{
    (void)file;
    return 2; /* FILE_TYPE_CHAR: our standard handles are the console */
}

/* A deliberately small UTF-16 -> narrow converter.  ASCII is enough for the
 * hostname and current console; non-ASCII input is replaced with '?'. */
__declspec(dllexport) int WideCharToMultiByte(unsigned code_page, DWORD flags,
                                              const WCHAR *wide, int wide_len,
                                              char *out, int out_len,
                                              const char *default_char,
                                              BOOL *used_default)
{
    (void)code_page; (void)flags; (void)default_char;
    int n = 0;
    BOOL replaced = 0;
    if (!wide)
        return 0;
    if (wide_len < 0) {
        while (wide[n]) n++;
        wide_len = n + 1;
    }
    if (!out || out_len == 0)
        return wide_len;
    for (n = 0; n < wide_len && n < out_len; n++) {
        WCHAR c = wide[n];
        if (c <= 0x7f)
            out[n] = (char)c;
        else {
            out[n] = '?';
            replaced = 1;
        }
    }
    if (used_default)
        *used_default = replaced;
    return n;
}

__declspec(dllexport) int MultiByteToWideChar(unsigned code_page, DWORD flags,
                                              const char *text, int text_len,
                                              WCHAR *out, int out_len)
{
    (void)code_page;
    (void)flags;
    if (!text)
        return 0;
    if (text_len < 0) {
        text_len = 0;
        while (text[text_len++])
            ;
    }
    if (!out || out_len == 0)
        return text_len;
    int n = 0;
    for (; n < text_len && n < out_len; n++)
        out[n] = (WCHAR)(BYTE)text[n];
    return n;
}

__declspec(dllexport) DWORD ExpandEnvironmentStringsA(const char *source,
                                                       char *out, DWORD size)
{
    if (!source || (size && !out)) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return 0;
    }

    HANDLE heap = GetProcessHeap();
    int source_length = MultiByteToWideChar(0, 0, source, -1, 0, 0);
    if (source_length <= 0) return 0;
    WCHAR *wide_source = HeapAlloc(heap, 0, (SIZE_T)source_length * sizeof(WCHAR));
    if (!wide_source) {
        SetLastError(8); /* ERROR_NOT_ENOUGH_MEMORY */
        return 0;
    }
    MultiByteToWideChar(0, 0, source, -1, wide_source, source_length);

    DWORD capacity = ExpandEnvironmentStringsW(wide_source, 0, 0);
    DWORD needed = 0;
    while (capacity && capacity <= 0x7fffffffu) {
        WCHAR *expanded = HeapAlloc(heap, 0, (SIZE_T)capacity * sizeof(WCHAR));
        if (!expanded) {
            SetLastError(8);
            break;
        }
        DWORD result = ExpandEnvironmentStringsW(wide_source, expanded, capacity);
        if (result && result <= capacity) {
            needed = (DWORD)WideCharToMultiByte(0, 0, expanded, (int)result,
                                                0, 0, 0, 0);
            if (needed && size >= needed)
                WideCharToMultiByte(0, 0, expanded, (int)result, out,
                                    (int)needed, 0, 0);
            else if (out && size)
                out[0] = 0;
        }
        HeapFree(heap, 0, expanded);
        if (result <= capacity) break;
        capacity = result;
    }
    HeapFree(heap, 0, wide_source);
    return needed;
}

/* ------------------------------------------------------------------ */
/* Root directory enumeration (first Win32 filesystem-search layer)    */
/* ------------------------------------------------------------------ */

typedef struct _WIN32_FIND_DATAW {
    DWORD    dwFileAttributes;
    FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime;
    DWORD    nFileSizeHigh, nFileSizeLow;
    DWORD    dwReserved0, dwReserved1;
    WCHAR    cFileName[260];
    WCHAR    cAlternateFileName[14];
} WIN32_FIND_DATAW;

typedef struct _FIND_HANDLE {
    HANDLE DirectoryHandle;
    WCHAR Pattern[260];
} FIND_HANDLE;

typedef struct _FILE_DIRECTORY_INFORMATION_K {
    DWORD NextEntryOffset;
    DWORD FileIndex;
    long long CreationTime;
    long long LastAccessTime;
    long long LastWriteTime;
    long long ChangeTime;
    long long EndOfFile;
    long long AllocationSize;
    DWORD FileAttributes;
    DWORD FileNameLength;
    WCHAR FileName[260];
} FILE_DIRECTORY_INFORMATION_K;

static WCHAR fold_char(WCHAR c)
{
    return (c >= 'a' && c <= 'z') ? (WCHAR)(c - ('a' - 'A')) : c;
}

static BOOL wildcard_match(const WCHAR *pattern, const WCHAR *name)
{
    while (*pattern) {
        if (*pattern == '*') {
            pattern++;
            if (!*pattern) return 1;
            while (*name) {
                if (wildcard_match(pattern, name)) return 1;
                name++;
            }
            return wildcard_match(pattern, name);
        }
        if (!*name || (*pattern != '?' &&
            fold_char(*pattern) != fold_char(*name)))
            return 0;
        pattern++;
        name++;
    }
    return *name == 0;
}

static void fill_find_data(WIN32_FIND_DATAW *out,
                           const FILE_DIRECTORY_INFORMATION_K *in)
{
    memset(out, 0, sizeof(*out));
    out->dwFileAttributes = in->FileAttributes;
    out->nFileSizeHigh = (DWORD)((ULONGLONG)in->EndOfFile >> 32);
    out->nFileSizeLow = (DWORD)in->EndOfFile;
    out->ftCreationTime.Low = (DWORD)in->CreationTime;
    out->ftCreationTime.High = (DWORD)((ULONGLONG)in->CreationTime >> 32);
    out->ftLastAccessTime.Low = (DWORD)in->LastAccessTime;
    out->ftLastAccessTime.High = (DWORD)((ULONGLONG)in->LastAccessTime >> 32);
    out->ftLastWriteTime.Low = (DWORD)in->LastWriteTime;
    out->ftLastWriteTime.High = (DWORD)((ULONGLONG)in->LastWriteTime >> 32);
    DWORD chars = in->FileNameLength / sizeof(WCHAR);
    if (chars > 259) chars = 259;
    for (DWORD i = 0; i < chars; i++) out->cFileName[i] = in->FileName[i];
    out->cFileName[chars] = 0;
}

static BOOL find_next_match(FIND_HANDLE *find, WIN32_FIND_DATAW *out)
{
    FILE_DIRECTORY_INFORMATION_K entry;
    IO_STATUS_BLOCK iosb;
    for (;;) {
        memset(&entry, 0, sizeof(entry));
        NTSTATUS status = NtQueryDirectoryFile(find->DirectoryHandle, 0, 0, 0,
                                               &iosb, &entry, sizeof(entry),
                                               1, 1, 0, 0);
        if (!NT_SUCCESS(status))
            break;
        DWORD chars = entry.FileNameLength / sizeof(WCHAR);
        if (chars > 259) chars = 259;
        entry.FileName[chars] = 0;
        if (wildcard_match(find->Pattern, entry.FileName)) {
            fill_find_data(out, &entry);
            return 1;
        }
    }
    SetLastError(18); /* ERROR_NO_MORE_FILES */
    return 0;
}

__declspec(dllexport) DWORD GetFileAttributesW(const WCHAR *path)
{
    if (!path || !*path) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return (DWORD)-1;
    }

    char utf8[260];
    if (!wide_path_to_utf8(path, utf8)) {
        SetLastError(206);
        return (DWORD)-1;
    }
    NTSTATUS status = NtFileSystemControlSimple(5, utf8, 0);
    if (!NT_SUCCESS(status)) { SetLastError(2); return (DWORD)-1; }
    SetLastError(0);
    return (DWORD)status;
}

__declspec(dllexport) HANDLE FindFirstFileExW(const WCHAR *path, int info_level,
                                              WIN32_FIND_DATAW *out,
                                              int search_op, LPVOID filter,
                                              DWORD flags)
{
    (void)info_level; (void)search_op; (void)filter; (void)flags;
    if (!path || !out) {
        SetLastError(87);
        return (HANDLE)(ULONGLONG)-1;
    }
    FIND_HANDLE *find = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                  sizeof(*find));
    if (!find) {
        SetLastError(8);
        return (HANDLE)(ULONGLONG)-1;
    }
    WCHAR directory[260];
    const WCHAR *leaf = path;
    const WCHAR *last_separator = 0;
    for (const WCHAR *p = path; *p; p++)
        if (*p == '\\' || *p == '/') { leaf = p + 1; last_separator = p; }
    DWORD directory_length = last_separator ? (DWORD)(last_separator - path) : 0;
    if (directory_length == 2 && path[1] == ':') directory_length = 3;
    if (!directory_length) {
        directory[0] = 'C'; directory[1] = ':';
        directory[2] = '\\'; directory[3] = 0;
    } else {
        DWORD d = 0;
        for (; d < directory_length && d < 259; d++)
            directory[d] = path[d];
        directory[d] = 0;
    }
    int i = 0;
    for (; leaf[i] && i < 259; i++) find->Pattern[i] = leaf[i];
    find->Pattern[i] = 0;
    find->DirectoryHandle = CreateFileW(directory, 0x80000000u, 7, 0, 3,
                                        0x02000000u, 0);
    if (find->DirectoryHandle == (HANDLE)(ULONGLONG)-1) {
        HeapFree(GetProcessHeap(), 0, find);
        SetLastError(3);
        return (HANDLE)(ULONGLONG)-1;
    }
    if (!find_next_match(find, out)) {
        CloseHandle(find->DirectoryHandle);
        HeapFree(GetProcessHeap(), 0, find);
        SetLastError(2);
        return (HANDLE)(ULONGLONG)-1;
    }
    return find;
}

__declspec(dllexport) HANDLE FindFirstFileW(const WCHAR *path,
                                            WIN32_FIND_DATAW *out)
{
    return FindFirstFileExW(path, 0, out, 0, 0, 0);
}

__declspec(dllexport) BOOL FindNextFileW(HANDLE handle, WIN32_FIND_DATAW *out)
{
    return handle && handle != (HANDLE)(ULONGLONG)-1 && out
               ? find_next_match((FIND_HANDLE *)handle, out) : 0;
}

__declspec(dllexport) BOOL FindClose(HANDLE handle)
{
    if (!handle || handle == (HANDLE)(ULONGLONG)-1) return 0;
    FIND_HANDLE *find = (FIND_HANDLE *)handle;
    CloseHandle(find->DirectoryHandle);
    return HeapFree(GetProcessHeap(), 0, find);
}

/* ------------------------------------------------------------------ */
/* Interlocked operations                                              */
/* ------------------------------------------------------------------ */

typedef long LONG;

__declspec(dllexport) int GetThreadPriority(HANDLE thread)
{
    LONG priority = 0;
    NTSTATUS status = NtQueryThreadPriority(thread, &priority);
    if (!NT_SUCCESS(status)) {
        SetLastError(ntstatus_to_win32(status));
        return 0x7FFFFFFF;
    }
    return (int)priority;
}

__declspec(dllexport) BOOL SetThreadPriority(HANDLE thread, int priority)
{
    NTSTATUS status = NtSetThreadPriority(thread, (LONG)priority);
    if (!NT_SUCCESS(status)) {
        SetLastError(ntstatus_to_win32(status));
        return 0;
    }
    return 1;
}
#define WINAPI
typedef unsigned int UINT;
typedef long long LONG_PTR;
typedef HANDLE *PHANDLE;

__declspec(dllexport) LONG InterlockedIncrement(LONG volatile *p)
{
    return __sync_add_and_fetch(p, 1);
}
__declspec(dllexport) LONG InterlockedDecrement(LONG volatile *p)
{
    return __sync_sub_and_fetch(p, 1);
}
__declspec(dllexport) LONG InterlockedExchange(LONG volatile *p, LONG value)
{
    return __sync_lock_test_and_set(p, value);
}
__declspec(dllexport) LONG InterlockedCompareExchange(LONG volatile *dst,
                                                      LONG exchange, LONG compare)
{
    return __sync_val_compare_and_swap(dst, compare, exchange);
}

/* ------------------------------------------------------------------ */
/* Critical sections (recursive lock)                                  */
/* ------------------------------------------------------------------ */

/* Share the native 40-byte x64 layout instead of a private 12-byte lock.
 * Win32 and native callers can safely use the same critical section. */
extern NTSTATUS RtlInitializeCriticalSection(void *);
extern NTSTATUS RtlInitializeCriticalSectionAndSpinCount(void *, DWORD);
extern NTSTATUS RtlEnterCriticalSection(void *);
extern NTSTATUS RtlLeaveCriticalSection(void *);
extern BOOL RtlTryEnterCriticalSection(void *);
extern NTSTATUS RtlDeleteCriticalSection(void *);

__declspec(dllexport) void InitializeCriticalSection(void *cs)
{
    RtlInitializeCriticalSection(cs);
}

__declspec(dllexport) void EnterCriticalSection(void *cs)
{
    RtlEnterCriticalSection(cs);
}

__declspec(dllexport) BOOL TryEnterCriticalSection(void *cs)
{
    return RtlTryEnterCriticalSection(cs);
}

__declspec(dllexport) void LeaveCriticalSection(void *cs)
{
    RtlLeaveCriticalSection(cs);
}

__declspec(dllexport) void DeleteCriticalSection(void *cs)
{
    RtlDeleteCriticalSection(cs);
}

__declspec(dllexport) BOOL InitializeCriticalSectionAndSpinCount(void *cs,
                                                                 DWORD count)
{
    return NT_SUCCESS(RtlInitializeCriticalSectionAndSpinCount(cs, count));
}

__declspec(dllexport) BOOL InitializeCriticalSectionEx(void *cs, DWORD count,
                                                        DWORD flags)
{
    (void)flags;
    return InitializeCriticalSectionAndSpinCount(cs, count);
}

/* ------------------------------------------------------------------ */
/* Slim reader/writer locks                                            */
/* ------------------------------------------------------------------ */

/* SRWLOCK is one pointer-sized word in the public ABI. The first correct
 * implementation serializes shared and exclusive acquisitions alike. This is
 * intentionally conservative but provides real cross-thread exclusion and
 * preserves the layout expected by unmodified Windows binaries. */
static void srw_acquire(void *lock)
{
    ULONGLONG volatile *word = (ULONGLONG volatile *)lock;
    while (!__sync_bool_compare_and_swap(word, 0, 1))
        __asm__ volatile("pause");
}

__declspec(dllexport) void AcquireSRWLockExclusive(void *lock)
{
    srw_acquire(lock);
}

__declspec(dllexport) void InitializeSRWLock(void *lock)
{
    *(ULONGLONG volatile *)lock = 0;
}

__declspec(dllexport) void AcquireSRWLockShared(void *lock)
{
    srw_acquire(lock);
}

__declspec(dllexport) BOOL TryAcquireSRWLockExclusive(void *lock)
{
    return __sync_bool_compare_and_swap((ULONGLONG volatile *)lock, 0, 1);
}

__declspec(dllexport) BOOL TryAcquireSRWLockShared(void *lock)
{
    return TryAcquireSRWLockExclusive(lock);
}

static void srw_release(void *lock)
{
    __sync_lock_release((ULONGLONG volatile *)lock);
}

__declspec(dllexport) void ReleaseSRWLockExclusive(void *lock)
{
    srw_release(lock);
}

__declspec(dllexport) void ReleaseSRWLockShared(void *lock)
{
    srw_release(lock);
}

/* ------------------------------------------------------------------ */
/* One-time initialization                                             */
/* ------------------------------------------------------------------ */

#define INIT_ONCE_CHECK_ONLY  0x00000001
#define INIT_ONCE_INIT_FAILED 0x00000004

typedef BOOL (*INIT_ONCE_FN)(void *once, void *parameter, void **context);

/* State values 0/1/2 mean uninitialized/in progress/completed with a NULL
 * context.  A completed non-NULL context is stored directly; Win32 requires
 * such context pointers to have their low two bits clear. */
__declspec(dllexport) BOOL InitOnceBeginInitialize(void *init_once, DWORD flags,
                                                   BOOL *pending, void **context)
{
    void *volatile *state = (void *volatile *)init_once;
    if (!state || !pending)
        return 0;

    for (;;) {
        void *value = *state;
        if (!value) {
            if (flags & INIT_ONCE_CHECK_ONLY) {
                *pending = 1;
                if (context) *context = 0;
                return 1;
            }
            if (__sync_bool_compare_and_swap(state, 0, (void *)1)) {
                *pending = 1;
                if (context) *context = 0;
                return 1;
            }
            continue;
        }
        if (value == (void *)1) {
            Sleep(0);
            continue;
        }
        *pending = 0;
        if (context)
            *context = value == (void *)2 ? 0 : value;
        return 1;
    }
}

__declspec(dllexport) BOOL InitOnceComplete(void *init_once, DWORD flags,
                                            void *context)
{
    void *volatile *state = (void *volatile *)init_once;
    if (!state)
        return 0;
    if (flags & INIT_ONCE_INIT_FAILED) {
        __sync_lock_test_and_set(state, 0);
        return 1;
    }
    if ((ULONGLONG)context & 3)
        return 0;
    __sync_lock_test_and_set(state, context ? context : (void *)2);
    return 1;
}

__declspec(dllexport) BOOL InitOnceExecuteOnce(void *init_once,
                                               INIT_ONCE_FN callback,
                                               void *parameter, void **context)
{
    BOOL pending;
    void *result = 0;
    if (!callback || !InitOnceBeginInitialize(init_once, 0, &pending, &result))
        return 0;
    if (pending) {
        if (!callback(init_once, parameter, &result)) {
            InitOnceComplete(init_once, INIT_ONCE_INIT_FAILED, 0);
            return 0;
        }
        if (!InitOnceComplete(init_once, 0, result))
            return 0;
    }
    if (context)
        *context = result;
    return 1;
}

/* ------------------------------------------------------------------ */
/* Minimal thread-pool work objects                                    */
/* ------------------------------------------------------------------ */

typedef void (*THREADPOOL_WORK_CALLBACK)(void *callback_instance,
                                         void *context, void *work);

#define MAX_THREADPOOL_WORK_ITEMS 32
typedef struct _THREADPOOL_WORK_ITEM {
    BOOL Used;
    BOOL Submitted;
    BOOL Running;
    THREADPOOL_WORK_CALLBACK Callback;
    void *Context;
    void *Environment;
    DWORD (*LegacyCallback)(void *);
    void (*SimpleCallback)(void *, void *);
    struct _THREADPOOL_WORK_ITEM *Next;
} THREADPOOL_WORK_ITEM;

static THREADPOOL_WORK_ITEM g_threadpool_work_items[MAX_THREADPOOL_WORK_ITEMS];
static THREADPOOL_WORK_ITEM g_legacy_work_items[64];
static THREADPOOL_WORK_ITEM *g_threadpool_work_head;
static THREADPOOL_WORK_ITEM *g_threadpool_work_tail;
static volatile LONG g_threadpool_work_lock;
static volatile LONG g_threadpool_worker_state; /* 0=none, 1=starting, 2=ready */
static HANDLE g_threadpool_work_semaphore;
static HANDLE g_threadpool_workers[8];
static volatile LONG g_threadpool_worker_count;

static void threadpool_lock(void)
{
    while (!__sync_bool_compare_and_swap(&g_threadpool_work_lock, 0, 1))
        __asm__ volatile("pause");
}

static void threadpool_unlock(void)
{
    __sync_lock_release(&g_threadpool_work_lock);
}

__declspec(dllexport) void *CreateThreadpoolWork(
    THREADPOOL_WORK_CALLBACK callback, void *context, void *environment)
{
    if (!callback) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return 0;
    }

    for (int i = 0; i < MAX_THREADPOOL_WORK_ITEMS; i++) {
        THREADPOOL_WORK_ITEM *work = &g_threadpool_work_items[i];
        if (!work->Used) {
            work->Callback = callback;
            work->Context = context;
            work->Environment = environment;
            work->Used = 1;
            SetLastError(0);
            return work;
        }
    }

    SetLastError(8); /* ERROR_NOT_ENOUGH_MEMORY */
    return 0;
}

static DWORD threadpool_worker_start(void *parameter)
{
    (void)parameter;
    for (;;) {
        if (WaitForSingleObject(g_threadpool_work_semaphore, (DWORD)-1) !=
            WAIT_OBJECT_0)
            continue;

        threadpool_lock();
        THREADPOOL_WORK_ITEM *work = g_threadpool_work_head;
        if (work) {
            g_threadpool_work_head = work->Next;
            if (!g_threadpool_work_head)
                g_threadpool_work_tail = 0;
            work->Next = 0;
        }
        threadpool_unlock();

        if (!work)
            continue;
        if (work->SimpleCallback) {
            threadpool_lock();
            work->Running = 1;
            threadpool_unlock();
            work->SimpleCallback(work, work->Context);
            threadpool_lock();
            work->Running = 0;
            work->Submitted = 0;
            work->Used = 0;
            work->SimpleCallback = 0;
            threadpool_unlock();
        } else if (work->LegacyCallback) {
            work->LegacyCallback(work->Context);
            threadpool_lock();
            work->Submitted = 0;
            work->Used = 0;
            work->LegacyCallback = 0;
            threadpool_unlock();
        } else {
            threadpool_lock();
            work->Running = 1;
            threadpool_unlock();
            work->Callback(work, work->Context, work);
            threadpool_lock();
            work->Running = 0;
            work->Submitted = 0;
            threadpool_unlock();
        }
    }
}

static BOOL threadpool_ensure_workers(void)
{
    if (g_threadpool_worker_state == 2)
        return 1;

    if (__sync_bool_compare_and_swap(&g_threadpool_worker_state, 0, 1)) {
        g_threadpool_work_semaphore = CreateSemaphoreW(0, 0, 0x7fffffff, 0);
        if (!g_threadpool_work_semaphore) {
            __sync_lock_test_and_set(&g_threadpool_worker_state, 0);
            return 0;
        }
        for (DWORD i = 0; i < 4; i++) {
            g_threadpool_workers[i] =
                CreateThread(0, 0, threadpool_worker_start, 0, 0, 0);
            if (!g_threadpool_workers[i]) {
                __sync_lock_test_and_set(&g_threadpool_worker_state, 0);
                return 0;
            }
            __atomic_add_fetch(&g_threadpool_worker_count, 1, __ATOMIC_RELEASE);
        }
        __sync_lock_test_and_set(&g_threadpool_worker_state, 2);
        return 1;
    }

    while (g_threadpool_worker_state == 1)
        Sleep(0);
    return g_threadpool_worker_state == 2;
}

__declspec(dllexport) BOOL CallbackMayRunLong(void *callback_instance)
{
    THREADPOOL_WORK_ITEM *work = callback_instance;
    BOOL in_normal = work && work >= g_threadpool_work_items &&
        work < g_threadpool_work_items + MAX_THREADPOOL_WORK_ITEMS;
    BOOL in_legacy = work && work >= g_legacy_work_items &&
        work < g_legacy_work_items + 64;
    if (!in_normal && !in_legacy) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return 0;
    }
    threadpool_lock();
    BOOL valid = work->Used && work->Running;
    threadpool_unlock();
    if (!valid) {
        SetLastError(87);
        return 0;
    }
    LONG count = __atomic_load_n(&g_threadpool_worker_count, __ATOMIC_ACQUIRE);
    if (count < 8) {
        LONG slot = __atomic_fetch_add(&g_threadpool_worker_count, 1,
                                       __ATOMIC_ACQ_REL);
        if (slot < 8) {
            HANDLE worker = CreateThread(0, 0, threadpool_worker_start, 0, 0, 0);
            if (worker)
                g_threadpool_workers[slot] = worker;
            else
                __atomic_sub_fetch(&g_threadpool_worker_count, 1,
                                   __ATOMIC_RELEASE);
        } else {
            __atomic_sub_fetch(&g_threadpool_worker_count, 1,
                               __ATOMIC_RELEASE);
        }
    }
    SetLastError(0);
    return 1;
}

__declspec(dllexport) BOOL TrySubmitThreadpoolCallback(
    void (*callback)(void *, void *), void *context, void *environment)
{
    if (!callback) {
        SetLastError(87);
        return 0;
    }
    /* Callback environments are opaque ABI records. Until their initialize
     * and cleanup-group setters are available, reject them explicitly. */
    if (environment) {
        SetLastError(50); /* ERROR_NOT_SUPPORTED */
        return 0;
    }
    if (!threadpool_ensure_workers()) {
        SetLastError(8);
        return 0;
    }
    threadpool_lock();
    THREADPOOL_WORK_ITEM *work = 0;
    for (DWORD i = 0; i < 64; ++i) {
        if (!g_legacy_work_items[i].Used) {
            work = &g_legacy_work_items[i];
            break;
        }
    }
    if (!work) {
        threadpool_unlock();
        SetLastError(8);
        return 0;
    }
    work->Used = 1;
    work->Submitted = 1;
    work->Running = 0;
    work->Callback = 0;
    work->LegacyCallback = 0;
    work->SimpleCallback = callback;
    work->Context = context;
    work->Environment = 0;
    work->Next = 0;
    if (g_threadpool_work_tail)
        g_threadpool_work_tail->Next = work;
    else
        g_threadpool_work_head = work;
    g_threadpool_work_tail = work;
    threadpool_unlock();
    if (!ReleaseSemaphore(g_threadpool_work_semaphore, 1, 0)) {
        SetLastError(8);
        return 0;
    }
    SetLastError(0);
    return 1;
}

__declspec(dllexport) BOOL QueueUserWorkItem(DWORD (*callback)(void *),
                                              void *context, DWORD flags)
{
    if (!callback || (flags & ~0x10u)) {
        SetLastError(87);
        return 0;
    }
    if (!threadpool_ensure_workers()) {
        SetLastError(8);
        return 0;
    }
    threadpool_lock();
    THREADPOOL_WORK_ITEM *work = 0;
    for (DWORD i = 0; i < 64; i++) {
        if (!g_legacy_work_items[i].Used) {
            work = &g_legacy_work_items[i];
            break;
        }
    }
    if (!work) {
        threadpool_unlock();
        SetLastError(8);
        return 0;
    }
    work->Used = 1;
    work->Submitted = 1;
    work->Callback = 0;
    work->LegacyCallback = callback;
    work->Context = context;
    work->Next = 0;
    if (g_threadpool_work_tail)
        g_threadpool_work_tail->Next = work;
    else
        g_threadpool_work_head = work;
    g_threadpool_work_tail = work;
    threadpool_unlock();
    if (!ReleaseSemaphore(g_threadpool_work_semaphore, 1, 0)) {
        SetLastError(8);
        return 0;
    }
    return 1;
}

__declspec(dllexport) void SubmitThreadpoolWork(void *work_handle)
{
    THREADPOOL_WORK_ITEM *work = (THREADPOOL_WORK_ITEM *)work_handle;
    if (!work || !work->Used || !work->Callback || work->Submitted) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return;
    }

    if (!threadpool_ensure_workers()) {
        SetLastError(8);
        return;
    }
    if (!__sync_bool_compare_and_swap(&work->Submitted, 0, 1)) {
        SetLastError(87);
        return;
    }

    threadpool_lock();
    work->Next = 0;
    if (g_threadpool_work_tail)
        g_threadpool_work_tail->Next = work;
    else
        g_threadpool_work_head = work;
    g_threadpool_work_tail = work;
    threadpool_unlock();

    if (!ReleaseSemaphore(g_threadpool_work_semaphore, 1, 0)) {
        /* Leave the item queued: another release/submission can still let a
         * worker drain it, but report the synchronization failure. */
        work->Submitted = 0;
        SetLastError(8);
        return;
    }
    SetLastError(0);
}

typedef void (*THREADPOOL_WAIT_CALLBACK)(void *callback_instance,
                                         void *context, void *wait,
                                         DWORD wait_result);

#define MAX_THREADPOOL_WAIT_ITEMS 32
typedef struct _THREADPOOL_WAIT_ITEM {
    BOOL Used;
    BOOL Waiting;
    THREADPOOL_WAIT_CALLBACK Callback;
    void *Context;
    void *Environment;
    HANDLE Object;
} THREADPOOL_WAIT_ITEM;

static THREADPOOL_WAIT_ITEM g_threadpool_wait_items[MAX_THREADPOOL_WAIT_ITEMS];
static volatile LONG g_threadpool_wait_lock;
static volatile LONG g_threadpool_wait_state;
static HANDLE g_threadpool_wait_control;
static HANDLE g_threadpool_wait_monitor;

static void threadpool_wait_lock(void)
{
    while (!__sync_bool_compare_and_swap(&g_threadpool_wait_lock, 0, 1))
        __asm__ volatile("pause");
}

static void threadpool_wait_unlock(void)
{
    __sync_lock_release(&g_threadpool_wait_lock);
}

__declspec(dllexport) void *CreateThreadpoolWait(
    THREADPOOL_WAIT_CALLBACK callback, void *context, void *environment)
{
    if (!callback) {
        SetLastError(87);
        return 0;
    }
    for (int i = 0; i < MAX_THREADPOOL_WAIT_ITEMS; i++) {
        THREADPOOL_WAIT_ITEM *wait = &g_threadpool_wait_items[i];
        if (!wait->Used) {
            wait->Callback = callback;
            wait->Context = context;
            wait->Environment = environment;
            wait->Used = 1;
            SetLastError(0);
            return wait;
        }
    }
    SetLastError(8);
    return 0;
}

static DWORD threadpool_wait_monitor_start(void *parameter)
{
    (void)parameter;
    for (;;) {
        HANDLE handles[MAX_THREADPOOL_WAIT_ITEMS + 1];
        THREADPOOL_WAIT_ITEM *items[MAX_THREADPOOL_WAIT_ITEMS + 1];
        DWORD count = 1;
        handles[0] = g_threadpool_wait_control;
        items[0] = 0;

        threadpool_wait_lock();
        for (int i = 0; i < MAX_THREADPOOL_WAIT_ITEMS; i++) {
            THREADPOOL_WAIT_ITEM *wait = &g_threadpool_wait_items[i];
            if (wait->Used && wait->Waiting && wait->Object) {
                handles[count] = wait->Object;
                items[count] = wait;
                count++;
            }
        }
        threadpool_wait_unlock();

        DWORD result = WaitForMultipleObjects(count, handles, 0, (DWORD)-1);
        if (result == WAIT_OBJECT_0)
            continue; /* control event: rebuild the snapshot */
        if (result == (DWORD)-1 || result < WAIT_OBJECT_0 ||
            result >= WAIT_OBJECT_0 + count) {
            Sleep(10);
            continue;
        }

        DWORD index = result - WAIT_OBJECT_0;
        THREADPOOL_WAIT_ITEM *wait = items[index];
        BOOL invoke = 0;
        threadpool_wait_lock();
        if (wait && wait->Used && wait->Waiting &&
            wait->Object == handles[index]) {
            wait->Waiting = 0; /* one-shot until SetThreadpoolWait rearms it */
            invoke = 1;
        }
        threadpool_wait_unlock();
        if (invoke)
            wait->Callback(0, wait->Context, wait, WAIT_OBJECT_0);
    }
}

static BOOL threadpool_ensure_wait_monitor(void)
{
    if (g_threadpool_wait_state == 2)
        return 1;
    if (__sync_bool_compare_and_swap(&g_threadpool_wait_state, 0, 1)) {
        /* Auto-reset: one reconfiguration wake is enough to rebuild the full
         * object snapshot. */
        g_threadpool_wait_control = CreateEventW(0, 0, 0, 0);
        if (g_threadpool_wait_control)
            g_threadpool_wait_monitor = CreateThread(
                0, 0, threadpool_wait_monitor_start, 0, 0, 0);
        if (!g_threadpool_wait_control || !g_threadpool_wait_monitor) {
            __sync_lock_test_and_set(&g_threadpool_wait_state, 0);
            return 0;
        }
        __sync_lock_test_and_set(&g_threadpool_wait_state, 2);
        return 1;
    }
    while (g_threadpool_wait_state == 1)
        Sleep(0);
    return g_threadpool_wait_state == 2;
}

__declspec(dllexport) void SetThreadpoolWait(void *wait_handle,
                                             HANDLE object,
                                             const void *timeout)
{
    (void)timeout;
    THREADPOOL_WAIT_ITEM *wait = (THREADPOOL_WAIT_ITEM *)wait_handle;
    if (!wait || !wait->Used) {
        SetLastError(87);
        return;
    }
    if (!threadpool_ensure_wait_monitor()) {
        SetLastError(8);
        return;
    }

    threadpool_wait_lock();
    wait->Object = object;
    wait->Waiting = object != 0;
    threadpool_wait_unlock();
    SetEvent(g_threadpool_wait_control);
    SetLastError(0);
}

/* ------------------------------------------------------------------ */
/* Virtual memory                                                      */
/* ------------------------------------------------------------------ */

__declspec(dllexport) LPVOID VirtualAlloc(LPVOID addr, SIZE_T size, DWORD type,
                                          DWORD protect)
{
    void *base = addr;
    ULONGLONG region = size;
    NTSTATUS status = NtAllocateVirtualMemory(NT_INVALID_HANDLE, &base, 0,
                                               &region, type, protect);
    if (!NT_SUCCESS(status)) {
        SetLastError(ntstatus_to_win32(status));
        return 0;
    }
    SetLastError(0);
    return base;
}

__declspec(dllexport) BOOL VirtualFree(LPVOID addr, SIZE_T size, DWORD type)
{
    void *base = addr;
    ULONGLONG region = size;
    NTSTATUS status = NtFreeVirtualMemory(NT_INVALID_HANDLE, &base, &region,
                                           type);
    if (!NT_SUCCESS(status)) {
        SetLastError(ntstatus_to_win32(status));
        return 0;
    }
    SetLastError(0);
    return 1;
}

__declspec(dllexport) BOOL VirtualProtect(LPVOID addr, SIZE_T size,
                                          DWORD newProtect, DWORD *oldProtect)
{
    void *base = addr;
    ULONGLONG region = size;
    NTSTATUS st = NtProtectVirtualMemory(NT_INVALID_HANDLE, &base, &region,
                                         newProtect, oldProtect);
    if (!NT_SUCCESS(st)) {
        SetLastError(ntstatus_to_win32(st));
        return 0;
    }
    SetLastError(0);
    return 1;
}

/* ------------------------------------------------------------------ */
/* Performance counter (backed by KUSER_SHARED_DATA system time)       */
/* ------------------------------------------------------------------ */

__declspec(dllexport) BOOL QueryPerformanceCounter(long long *count)
{
    if (count)
        *count = (long long)read_ksystem_time(0x014); /* 100 ns units */
    return 1;
}

__declspec(dllexport) BOOL QueryPerformanceFrequency(long long *freq)
{
    if (freq)
        *freq = 10000000; /* 100 ns tick -> 10 MHz */
    return 1;
}
