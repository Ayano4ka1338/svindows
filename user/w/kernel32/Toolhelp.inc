/* Process and thread snapshots backed by the kernel's SystemProcessInformation
 * query. Snapshot entries remain stable while the caller enumerates them. */
typedef struct _TOOLHELP_PROCESSENTRY32W {
    DWORD dwSize, cntUsage, th32ProcessID;
    ULONGLONG th32DefaultHeapID;
    DWORD th32ModuleID, cntThreads, th32ParentProcessID;
    LONG pcPriClassBase;
    DWORD dwFlags;
    WCHAR szExeFile[260];
} TOOLHELP_PROCESSENTRY32W;

typedef struct _TOOLHELP_THREADENTRY32 {
    DWORD dwSize, cntUsage, th32ThreadID, th32OwnerProcessID;
    LONG tpBasePri, tpDeltaPri;
    DWORD dwFlags;
} TOOLHELP_THREADENTRY32;

typedef struct _TOOLHELP_SNAPSHOT {
    ULONGLONG Magic;
    DWORD Flags, ProcessCount, ThreadCount;
    LONG ProcessCursor, ThreadCursor;
    TOOLHELP_PROCESSENTRY32W Entries[1];
} TOOLHELP_SNAPSHOT;

#define TOOLHELP_MAGIC 0x53565754484C5031ULL
#define TH32CS_SNAPPROCESS 0x00000002u
#define TH32CS_SNAPTHREAD  0x00000004u
#define TH32CS_SNAPMODULE  0x00000008u
#define TH32CS_SNAPMODULE32 0x00000010u

static TOOLHELP_SNAPSHOT *g_toolhelp_snapshots[32];
static volatile LONG g_toolhelp_lock;
extern long NtQuerySystemInformation(DWORD, void *, DWORD, DWORD *);

static void toolhelp_lock(void)
{
    while (!__sync_bool_compare_and_swap(&g_toolhelp_lock, 0, 1))
        __asm__ volatile("pause");
}

static void toolhelp_unlock(void)
{
    __sync_lock_release(&g_toolhelp_lock);
}

static TOOLHELP_THREADENTRY32 *toolhelp_threads(TOOLHELP_SNAPSHOT *snapshot)
{
    return (TOOLHELP_THREADENTRY32 *)(snapshot->Entries +
                                      snapshot->ProcessCount);
}

static int toolhelp_valid(TOOLHELP_SNAPSHOT *snapshot)
{
    if (!snapshot) return 0;
    for (DWORD i = 0; i < 32; ++i)
        if (g_toolhelp_snapshots[i] == snapshot &&
            snapshot->Magic == TOOLHELP_MAGIC)
            return 1;
    return 0;
}

BOOL ToolhelpCloseSnapshot(HANDLE handle)
{
    TOOLHELP_SNAPSHOT *snapshot = handle;
    toolhelp_lock();
    for (DWORD i = 0; i < 32; ++i) {
        if (g_toolhelp_snapshots[i] == snapshot &&
            snapshot && snapshot->Magic == TOOLHELP_MAGIC) {
            g_toolhelp_snapshots[i] = 0;
            snapshot->Magic = 0;
            toolhelp_unlock();
            HeapFree(GetProcessHeap(), 0, snapshot);
            return 1;
        }
    }
    toolhelp_unlock();
    return 0;
}

__declspec(dllexport) HANDLE CreateToolhelp32Snapshot(DWORD flags,
                                                        DWORD process_id)
{
    DWORD supported = TH32CS_SNAPPROCESS | TH32CS_SNAPTHREAD;
    if (!flags || (flags & ~supported)) {
        SetLastError(flags & (TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32)
                     ? 50 : 87);
        return (HANDLE)(ULONGLONG)-1;
    }
    DWORD required = 0;
    long status = NtQuerySystemInformation(5, 0, 0, &required);
    if (status >= 0 || !required) { SetLastError(6); return (HANDLE)-1; }
    BYTE *system = HeapAlloc(GetProcessHeap(), 0, required);
    if (!system) { SetLastError(8); return (HANDLE)-1; }
    status = NtQuerySystemInformation(5, system, required, &required);
    if (status < 0) {
        HeapFree(GetProcessHeap(), 0, system);
        SetLastError(6);
        return (HANDLE)(ULONGLONG)-1;
    }
    DWORD processes = 0, threads = 0;
    for (DWORD offset = 0; ; ) {
        BYTE *entry = system + offset;
        DWORD thread_count = *(DWORD *)(entry + 4);
        DWORD pid = (DWORD)*(ULONGLONG *)(entry + 80);
        if ((flags & TH32CS_SNAPPROCESS) ||
            ((flags & TH32CS_SNAPTHREAD) &&
             (!process_id || process_id == pid)))
            processes += !!(flags & TH32CS_SNAPPROCESS);
        if (flags & TH32CS_SNAPTHREAD)
            threads += process_id && process_id != pid ? 0 : thread_count;
        DWORD next = *(DWORD *)entry;
        if (!next) break;
        offset += next;
    }
    SIZE_T bytes = sizeof(TOOLHELP_SNAPSHOT) -
        sizeof(TOOLHELP_PROCESSENTRY32W) +
        (SIZE_T)processes * sizeof(TOOLHELP_PROCESSENTRY32W) +
        (SIZE_T)threads * sizeof(TOOLHELP_THREADENTRY32);
    TOOLHELP_SNAPSHOT *snapshot = HeapAlloc(GetProcessHeap(), 8, bytes);
    if (!snapshot) {
        HeapFree(GetProcessHeap(), 0, system);
        SetLastError(8);
        return (HANDLE)(ULONGLONG)-1;
    }
    snapshot->Magic = TOOLHELP_MAGIC;
    snapshot->Flags = flags;
    snapshot->ProcessCount = processes;
    snapshot->ThreadCount = threads;
    snapshot->ProcessCursor = snapshot->ThreadCursor = -1;
    TOOLHELP_PROCESSENTRY32W *pe = snapshot->Entries;
    TOOLHELP_THREADENTRY32 *te = toolhelp_threads(snapshot);
    DWORD pi = 0, ti = 0;
    for (DWORD offset = 0; ; ) {
        BYTE *entry = system + offset;
        DWORD thread_count = *(DWORD *)(entry + 4);
        DWORD pid = (DWORD)*(ULONGLONG *)(entry + 80);
        DWORD parent = (DWORD)*(ULONGLONG *)(entry + 88);
        DWORD image_bytes = *(unsigned short *)(entry + 56);
        const WCHAR *image = (const WCHAR *)(ULONGLONG)*(ULONGLONG *)(entry + 64);
        if (flags & TH32CS_SNAPPROCESS) {
            TOOLHELP_PROCESSENTRY32W *item = &pe[pi++];
            item->dwSize = sizeof(*item);
            item->th32ProcessID = pid;
            item->th32ParentProcessID = parent;
            item->cntThreads = thread_count;
            item->pcPriClassBase = *(LONG *)(entry + 72);
            DWORD n = image_bytes / sizeof(WCHAR);
            if (n > 259) n = 259;
            for (DWORD j = 0; j < n; ++j) item->szExeFile[j] = image[j];
            item->szExeFile[n] = 0;
        }
        if (flags & TH32CS_SNAPTHREAD &&
            (!process_id || process_id == pid)) {
            BYTE *native_thread = entry + 256;
            for (DWORD j = 0; j < thread_count; ++j) {
                TOOLHELP_THREADENTRY32 *item = &te[ti++];
                item->dwSize = sizeof(*item);
                item->th32OwnerProcessID = pid;
                item->th32ThreadID = (DWORD)*(ULONGLONG *)(native_thread + j * 80 + 48);
                item->tpBasePri = *(LONG *)(native_thread + j * 80 + 60);
            }
        }
        DWORD next = *(DWORD *)entry;
        if (!next) break;
        offset += next;
    }
    HeapFree(GetProcessHeap(), 0, system);
    toolhelp_lock();
    DWORD slot;
    for (slot = 0; slot < 32 && g_toolhelp_snapshots[slot]; ++slot) { }
    if (slot < 32) g_toolhelp_snapshots[slot] = snapshot;
    toolhelp_unlock();
    if (slot == 32) {
        HeapFree(GetProcessHeap(), 0, snapshot);
        SetLastError(8);
        return (HANDLE)(ULONGLONG)-1;
    }
    SetLastError(0);
    return snapshot;
}

__declspec(dllexport) BOOL Process32FirstW(HANDLE handle,
                                            TOOLHELP_PROCESSENTRY32W *entry)
{
    TOOLHELP_SNAPSHOT *snapshot = handle;
    if (!toolhelp_valid(snapshot) || !entry ||
        entry->dwSize != sizeof(*entry) || !snapshot->ProcessCount) {
        SetLastError(87);
        return 0;
    }
    snapshot->ProcessCursor = 0;
    *entry = snapshot->Entries[0];
    return 1;
}

__declspec(dllexport) BOOL Process32NextW(HANDLE handle,
                                           TOOLHELP_PROCESSENTRY32W *entry)
{
    TOOLHELP_SNAPSHOT *snapshot = handle;
    if (!toolhelp_valid(snapshot) || !entry ||
        entry->dwSize != sizeof(*entry)) { SetLastError(87); return 0; }
    LONG next = snapshot->ProcessCursor + 1;
    if (next < 0 || (DWORD)next >= snapshot->ProcessCount) return 0;
    snapshot->ProcessCursor = next;
    *entry = snapshot->Entries[next];
    return 1;
}

__declspec(dllexport) BOOL Thread32First(HANDLE handle,
                                          TOOLHELP_THREADENTRY32 *entry)
{
    TOOLHELP_SNAPSHOT *snapshot = handle;
    if (!toolhelp_valid(snapshot) || !entry ||
        entry->dwSize != sizeof(*entry) || !snapshot->ThreadCount) {
        SetLastError(87);
        return 0;
    }
    snapshot->ThreadCursor = 0;
    *entry = toolhelp_threads(snapshot)[0];
    return 1;
}

__declspec(dllexport) BOOL Thread32Next(HANDLE handle,
                                         TOOLHELP_THREADENTRY32 *entry)
{
    TOOLHELP_SNAPSHOT *snapshot = handle;
    if (!toolhelp_valid(snapshot) || !entry ||
        entry->dwSize != sizeof(*entry)) { SetLastError(87); return 0; }
    LONG next = snapshot->ThreadCursor + 1;
    if (next < 0 || (DWORD)next >= snapshot->ThreadCount) return 0;
    snapshot->ThreadCursor = next;
    *entry = toolhelp_threads(snapshot)[next];
    return 1;
}
