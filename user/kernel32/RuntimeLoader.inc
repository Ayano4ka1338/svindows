/* Freestanding helpers, PEB/module lookup, exports, files and threads.
 * Included by the owning translation unit. */
/* ------------------------------------------------------------------ */
/* Freestanding helpers (no CRT)                                      */
/* ------------------------------------------------------------------ */

void *memset(void *dst, int v, SIZE_T n)
{
    BYTE *p = (BYTE *)dst;
    while (n--)
        *p++ = (BYTE)v;
    return dst;
}

void *memcpy(void *dst, const void *src, SIZE_T n)
{
    BYTE *d = (BYTE *)dst;
    const BYTE *s = (const BYTE *)src;
    while (n--)
        *d++ = *s++;
    return dst;
}

static int ascii_ieq(char a, char b)
{
    if (a >= 'A' && a <= 'Z') a += 32;
    if (b >= 'A' && b <= 'Z') b += 32;
    return a == b;
}

static int str_eq(const char *a, const char *b)
{
    for (; *a && *b; a++, b++)
        if (*a != *b)
            return 0;
    return *a == *b;
}

/* ------------------------------------------------------------------ */
/* PEB / loader module list (walked to find loaded modules)           */
/* ------------------------------------------------------------------ */

typedef struct _LIST_ENTRY { struct _LIST_ENTRY *Flink, *Blink; } LIST_ENTRY;
typedef struct _UNICODE_STRING { WORD Length, MaximumLength; WORD *Buffer; }
    UNICODE_STRING;

/* Field offsets match the Windows x64 layout the kernel loader writes. */
typedef struct _LDR_ENTRY {
    LIST_ENTRY     InLoadOrderLinks;           /* 0x00 */
    LIST_ENTRY     InMemoryOrderLinks;         /* 0x10 */
    LIST_ENTRY     InInitializationOrderLinks; /* 0x20 */
    void          *DllBase;                    /* 0x30 */
    void          *EntryPoint;                 /* 0x38 */
    DWORD          SizeOfImage;                /* 0x40 */
    UNICODE_STRING FullDllName;                /* 0x48 */
    UNICODE_STRING BaseDllName;                /* 0x58 */
} LDR_ENTRY;

typedef struct _PEB_LDR_DATA {
    DWORD      Length;                        /* 0x00 */
    BYTE       Initialized;                   /* 0x04 */
    void      *SsHandle;                      /* 0x08 */
    LIST_ENTRY InLoadOrderModuleList;         /* 0x10 */
    LIST_ENTRY InMemoryOrderModuleList;       /* 0x20 */
    LIST_ENTRY InInitializationOrderModuleList;/* 0x30 */
} PEB_LDR_DATA;

typedef struct _PEB {
    BYTE          Reserved[0x10];  /* 0x00 */
    void         *ImageBaseAddress;/* 0x10 */
    PEB_LDR_DATA *Ldr;             /* 0x18 */
    void         *ProcessParameters;/* 0x20 */
    void         *SubSystemData;   /* 0x28 */
    void         *ProcessHeap;     /* 0x30 */
} PEB;

static PEB *NtCurrentPeb(void)
{
    PEB *peb;
    __asm__ volatile("movq %%gs:0x60, %0" : "=r"(peb));
    return peb;
}

/* Case-insensitively compare a wide UNICODE_STRING to an ASCII C string. */
static int ustr_eq_ascii(const UNICODE_STRING *u, const char *ascii)
{
    unsigned n = u->Length / 2;
    unsigned i = 0;
    for (; i < n && ascii[i]; i++)
        if (!ascii_ieq((char)u->Buffer[i], ascii[i]))
            return 0;
    return i == n && ascii[i] == 0;
}

__declspec(dllexport) HANDLE GetModuleHandleA(const char *name)
{
    PEB *peb = NtCurrentPeb();
    if (!name)
        return (HANDLE)peb->ImageBaseAddress;
    PEB_LDR_DATA *ldr = peb->Ldr;
    if (!ldr)
        return 0;

    LIST_ENTRY *head = &ldr->InLoadOrderModuleList;
    for (LIST_ENTRY *p = head->Flink; p != head; p = p->Flink) {
        LDR_ENTRY *e = (LDR_ENTRY *)p; /* InLoadOrderLinks is at offset 0 */
        if (ustr_eq_ascii(&e->BaseDllName, name))
            return (HANDLE)e->DllBase;
    }
    return 0;
}

__declspec(dllexport) HANDLE GetModuleHandleW(const WCHAR *name)
{
    if (!name)
        return GetModuleHandleA(0);
    char ascii[64];
    unsigned i = 0;
    for (; name[i] && i < sizeof(ascii) - 1; i++)
        ascii[i] = (char)name[i];
    ascii[i] = 0;
    return GetModuleHandleA(ascii);
}

__declspec(dllexport) BOOL GetModuleHandleExW(DWORD flags,
                                              const WCHAR *name_or_address,
                                              HANDLE *module)
{
    const DWORD FROM_ADDRESS = 0x00000004u;
    if (!module || (flags & ~0x00000007u) || !name_or_address) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return 0;
    }
    *module = 0;

    if (!(flags & FROM_ADDRESS)) {
        *module = GetModuleHandleW(name_or_address);
    } else {
        ULONGLONG address = (ULONGLONG)name_or_address;
        PEB *peb = NtCurrentPeb();
        PEB_LDR_DATA *ldr = peb ? peb->Ldr : 0;
        if (ldr) {
            LIST_ENTRY *head = &ldr->InLoadOrderModuleList;
            for (LIST_ENTRY *p = head->Flink; p != head; p = p->Flink) {
                LDR_ENTRY *entry = (LDR_ENTRY *)p;
                ULONGLONG base = (ULONGLONG)entry->DllBase;
                if (address >= base && address < base + entry->SizeOfImage) {
                    *module = entry->DllBase;
                    break;
                }
            }
        }
    }

    if (!*module) {
        SetLastError(126); /* ERROR_MOD_NOT_FOUND */
        return 0;
    }
    SetLastError(0);
    return 1;
}

__declspec(dllexport) HANDLE LoadLibraryA(const char *name)
{
    /* Already loaded? Return the existing module (Windows semantics). */
    HANDLE existing = GetModuleHandleA(name);
    if (existing)
        return existing;
    /* Otherwise ask the kernel loader to map it and link it into PEB->Ldr. */
    HANDLE module = NtLoadLibrary(name);
    if (!module)
        return 0;

    /* Runtime loads do not pass through the initial ntdll bootstrap block, so
     * perform DLL_PROCESS_ATTACH here after relocation/import binding. This is
     * required by control-panel and shell extension DLLs before any exported
     * entry point is called. */
    BYTE *image = (BYTE *)module;
    DWORD pe_offset = *(DWORD *)(image + 0x3c);
    if (*(DWORD *)(image + pe_offset) == 0x00004550) {
        BYTE *optional = image + pe_offset + 24;
        DWORD entry_rva = *(DWORD *)(optional + 16);
        if (entry_rva) {
            BOOL (*entry)(HANDLE, DWORD, void *) =
                (BOOL (*)(HANDLE, DWORD, void *))(image + entry_rva);
            if (!entry(module, 1 /* DLL_PROCESS_ATTACH */, 0)) {
                SetLastError(1114); /* ERROR_DLL_INIT_FAILED */
                return 0;
            }
        }
    }
    return module;
}

__declspec(dllexport) HANDLE LoadLibraryW(const WCHAR *name)
{
    if (!name)
        return 0;
    char ascii[96];
    unsigned i = 0;
    for (; name[i] && i < sizeof(ascii) - 1; i++)
        ascii[i] = (char)name[i];
    ascii[i] = 0;
    return LoadLibraryA(ascii);
}

__declspec(dllexport) HANDLE LoadLibraryExW(const WCHAR *name, HANDLE file,
                                             DWORD flags)
{
    (void)file;
    (void)flags;
    return LoadLibraryW(name);
}

__declspec(dllexport) HANDLE LoadLibraryExA(const char *name, HANDLE file,
                                             DWORD flags)
{
    (void)file;
    (void)flags;
    return LoadLibraryA(name);
}

__declspec(dllexport) BOOL FreeLibrary(HANDLE module)
{
    (void)module; /* unloading is a no-op for now (no per-module refcounts) */
    return 1;
}

/* ------------------------------------------------------------------ */
/* GetProcAddress: parse a module's PE export directory               */
/* ------------------------------------------------------------------ */

typedef struct _IMAGE_EXPORT_DIRECTORY {
    DWORD Characteristics, TimeDateStamp;
    WORD  MajorVersion, MinorVersion;
    DWORD Name, Base, NumberOfFunctions, NumberOfNames;
    DWORD AddressOfFunctions, AddressOfNames, AddressOfNameOrdinals;
} IMAGE_EXPORT_DIRECTORY;

static IMAGE_EXPORT_DIRECTORY *pe_export_dir(const char *base)
{
    DWORD lfanew = *(const DWORD *)(base + 0x3C);
    const char *nt = base + lfanew;
    if (*(const DWORD *)nt != 0x00004550) /* "PE\0\0" */
        return 0;
    const char *opt = nt + 0x18; /* signature(4) + file header(20) */
    DWORD rva = *(const DWORD *)(opt + 0x70); /* DataDirectory[0] (export) */
    return rva ? (IMAGE_EXPORT_DIRECTORY *)(base + rva) : 0;
}

__declspec(dllexport) FARPROC GetProcAddress(HANDLE module, const char *name)
{
    const char *base = (const char *)module;
    if (!base)
        return 0;
    IMAGE_EXPORT_DIRECTORY *ed = pe_export_dir(base);
    if (!ed)
        return 0;

    DWORD *funcs = (DWORD *)(base + ed->AddressOfFunctions);

    /* An "ordinal" argument has a zero high half (MAKEINTRESOURCE). */
    if (((ULONGLONG)(void *)name >> 16) == 0) {
        DWORD idx = (DWORD)(ULONGLONG)(void *)name - ed->Base;
        if (idx >= ed->NumberOfFunctions)
            return 0;
        return (FARPROC)(void *)(base + funcs[idx]);
    }

    DWORD *names = (DWORD *)(base + ed->AddressOfNames);
    WORD  *ords  = (WORD  *)(base + ed->AddressOfNameOrdinals);
    for (DWORD i = 0; i < ed->NumberOfNames; i++) {
        if (str_eq(base + names[i], name))
            return (FARPROC)(void *)(base + funcs[ords[i]]);
    }
    return 0;
}

__declspec(dllexport) HANDLE GetStdHandle(DWORD which)
{
    PEB *peb = NtCurrentPeb();
    if (peb) {
        BYTE *params = (BYTE *)peb->ProcessParameters;
        if (params) {
            HANDLE inherited = 0;
            if (which == (DWORD)-10) inherited = *(HANDLE *)(params + 0x20);
            else if (which == (DWORD)-11) inherited = *(HANDLE *)(params + 0x28);
            else if (which == (DWORD)-12) inherited = *(HANDLE *)(params + 0x30);
            if (inherited) return inherited;
        }
    }
    static HANDLE g_std_console;
    if (!g_std_console)
        g_std_console = nt_open("\\Device\\Console");
    return g_std_console;
}

__declspec(dllexport) BOOL GetConsoleMode(HANDLE console, DWORD *mode)
{
    if (!console || console == NT_INVALID_HANDLE || !mode) {
        SetLastError(6); /* ERROR_INVALID_HANDLE */
        return 0;
    }
    ULONGLONG value = 0;
    NTSTATUS status = NtConsoleControlSimple(console, 1, 0, 0, 0, &value);
    if (!NT_SUCCESS(status)) { SetLastError(6); return 0; }
    *mode = (DWORD)value;
    SetLastError(0);
    return 1;
}

__declspec(dllexport) BOOL SetConsoleMode(HANDLE console, DWORD mode)
{
    NTSTATUS status = NtConsoleControlSimple(console, 2, mode, 0, 0, 0);
    if (!NT_SUCCESS(status)) { SetLastError(87); return 0; }
    SetLastError(0); return 1;
}

__declspec(dllexport) BOOL GetConsoleScreenBufferInfo(
    HANDLE console, CONSOLE_SCREEN_BUFFER_INFO *info)
{
    NTSTATUS status = NtConsoleControlSimple(console, 3,
                                              (ULONGLONG)info, 0, 0, 0);
    if (!NT_SUCCESS(status)) { SetLastError(6); return 0; }
    SetLastError(0); return 1;
}

__declspec(dllexport) BOOL SetConsoleTextAttribute(HANDLE console,
                                                   WORD attribute)
{
    NTSTATUS status = NtConsoleControlSimple(console, 4, attribute, 0, 0, 0);
    if (!NT_SUCCESS(status)) { SetLastError(6); return 0; }
    SetLastError(0); return 1;
}

__declspec(dllexport) BOOL SetConsoleCursorPosition(HANDLE console,
                                                    COORD position)
{
    DWORD packed = (WORD)position.X | ((DWORD)(WORD)position.Y << 16);
    NTSTATUS status = NtConsoleControlSimple(console, 5, packed, 0, 0, 0);
    if (!NT_SUCCESS(status)) { SetLastError(87); return 0; }
    SetLastError(0); return 1;
}

__declspec(dllexport) BOOL GetConsoleCursorInfo(HANDLE console,
                                                CONSOLE_CURSOR_INFO *info)
{
    NTSTATUS status = NtConsoleControlSimple(console, 6,
                                              (ULONGLONG)info, 0, 0, 0);
    if (!NT_SUCCESS(status)) { SetLastError(6); return 0; }
    SetLastError(0); return 1;
}

__declspec(dllexport) BOOL SetConsoleCursorInfo(HANDLE console,
                                                const CONSOLE_CURSOR_INFO *info)
{
    NTSTATUS status = NtConsoleControlSimple(console, 7,
                                              (ULONGLONG)info, 0, 0, 0);
    if (!NT_SUCCESS(status)) { SetLastError(87); return 0; }
    SetLastError(0); return 1;
}

__declspec(dllexport) BOOL FillConsoleOutputCharacterA(
    HANDLE console, char character, DWORD length, COORD origin, DWORD *written)
{
    ULONGLONG count = 0;
    DWORD packed = (WORD)origin.X | ((DWORD)(WORD)origin.Y << 16);
    NTSTATUS status = NtConsoleControlSimple(console, 8, (BYTE)character,
                                              length, packed, &count);
    if (written) *written = (DWORD)count;
    if (!NT_SUCCESS(status)) { SetLastError(87); return 0; }
    SetLastError(0); return 1;
}

__declspec(dllexport) BOOL FillConsoleOutputAttribute(
    HANDLE console, WORD attribute, DWORD length, COORD origin, DWORD *written)
{
    ULONGLONG count = 0;
    DWORD packed = (WORD)origin.X | ((DWORD)(WORD)origin.Y << 16);
    NTSTATUS status = NtConsoleControlSimple(console, 9, attribute,
                                              length, packed, &count);
    if (written) *written = (DWORD)count;
    if (!NT_SUCCESS(status)) { SetLastError(87); return 0; }
    SetLastError(0); return 1;
}

__declspec(dllexport) BOOL WriteFile(HANDLE h, const void *buffer, DWORD len,
                                     DWORD *written, LPVOID overlapped)
{
    (void)overlapped;
    IO_STATUS_BLOCK iosb;
    NTSTATUS st = NtWriteFile(h, 0, 0, 0, &iosb, buffer, len, 0, 0);
    if (written)
        *written = (DWORD)iosb.Information;
    return NT_SUCCESS(st);
}

__declspec(dllexport) BOOL WriteConsoleW(HANDLE console, const WCHAR *text,
                                         DWORD char_count, DWORD *written,
                                         LPVOID reserved)
{
    char bytes[256];
    DWORD done = 0;
    (void)reserved;

    if (!text) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return 0;
    }

    while (done < char_count) {
        DWORD count = char_count - done;
        DWORD byte_count;
        if (count > sizeof(bytes)) count = sizeof(bytes);
        for (byte_count = 0; byte_count < count; byte_count++) {
            WCHAR c = text[done + byte_count];
            bytes[byte_count] = c <= 0x7f ? (char)c : '?';
        }
        if (!WriteFile(console, bytes, byte_count, 0, 0)) {
            if (written) *written = done;
            return 0;
        }
        done += count;
    }
    if (written) *written = done;
    return 1;
}

__declspec(dllexport) BOOL ReadFile(HANDLE h, void *buffer, DWORD len,
                                    DWORD *read, LPVOID overlapped)
{
    (void)overlapped;
    IO_STATUS_BLOCK iosb;
    NTSTATUS st = NtReadFile(h, 0, 0, 0, &iosb, buffer, len, 0, 0);
    if (read)
        *read = (DWORD)iosb.Information;
    return NT_SUCCESS(st);
}

typedef struct _SVINDOWS_FILE_INFO {
    ULONGLONG Size;
    ULONGLONG Position;
    DWORD Attributes;
    DWORD IsConsole;
    WORD FatWriteDate;
    WORD FatWriteTime;
    DWORD Reserved;
} SVINDOWS_FILE_INFO;

static BOOL leap_year(DWORD year)
{
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

static ULONGLONG fat_datetime_ticks(WORD date, WORD time)
{
    static const BYTE month_days[12] =
        {31,28,31,30,31,30,31,31,30,31,30,31};
    DWORD year = 1980 + (date >> 9);
    DWORD month = (date >> 5) & 15;
    DWORD day = date & 31;
    if (!date || month < 1 || month > 12 || day < 1)
        return 0;
    ULONGLONG days = 0;
    for (DWORD y = 1601; y < year; y++)
        days += leap_year(y) ? 366 : 365;
    for (DWORD m = 1; m < month; m++) {
        days += month_days[m - 1];
        if (m == 2 && leap_year(year)) days++;
    }
    days += day - 1;
    ULONGLONG seconds = days * 86400ULL;
    seconds += ((time >> 11) & 31) * 3600ULL;
    seconds += ((time >> 5) & 63) * 60ULL;
    seconds += (time & 31) * 2ULL;
    return seconds * 10000000ULL;
}

static FILETIME ticks_to_filetime(ULONGLONG ticks)
{
    FILETIME ft;
    ft.Low = (DWORD)ticks;
    ft.High = (DWORD)(ticks >> 32);
    return ft;
}

__declspec(dllexport) DWORD GetFileSize(HANDLE file, DWORD *high)
{
    SVINDOWS_FILE_INFO info;
    NTSTATUS status = NtQueryFileInfo(file, &info);
    if (!NT_SUCCESS(status)) {
        SetLastError(ntstatus_to_win32(status));
        return (DWORD)-1;
    }
    if (high) *high = (DWORD)(info.Size >> 32);
    return (DWORD)info.Size;
}

typedef struct _BY_HANDLE_FILE_INFORMATION {
    DWORD    dwFileAttributes;
    FILETIME ftCreationTime;
    FILETIME ftLastAccessTime;
    FILETIME ftLastWriteTime;
    DWORD    dwVolumeSerialNumber;
    DWORD    nFileSizeHigh;
    DWORD    nFileSizeLow;
    DWORD    nNumberOfLinks;
    DWORD    nFileIndexHigh;
    DWORD    nFileIndexLow;
} BY_HANDLE_FILE_INFORMATION;

__declspec(dllexport) BOOL GetFileInformationByHandle(
    HANDLE file, BY_HANDLE_FILE_INFORMATION *out)
{
    SVINDOWS_FILE_INFO info;
    if (!out) {
        SetLastError(87);
        return 0;
    }
    NTSTATUS status = NtQueryFileInfo(file, &info);
    if (!NT_SUCCESS(status)) {
        SetLastError(ntstatus_to_win32(status));
        return 0;
    }
    memset(out, 0, sizeof(*out));
    out->dwFileAttributes = info.Attributes;
    out->nFileSizeHigh = (DWORD)(info.Size >> 32);
    out->nFileSizeLow = (DWORD)info.Size;
    out->nNumberOfLinks = 1;
    out->ftLastWriteTime = ticks_to_filetime(
        fat_datetime_ticks(info.FatWriteDate, info.FatWriteTime));
    return 1;
}

__declspec(dllexport) BOOL SetFilePointerEx(HANDLE file, long long distance,
                                             long long *new_position,
                                             DWORD move_method)
{
    SVINDOWS_FILE_INFO info;
    NTSTATUS status = NtQueryFileInfo(file, &info);
    if (!NT_SUCCESS(status)) {
        SetLastError(ntstatus_to_win32(status));
        return 0;
    }

    ULONGLONG base;
    switch (move_method) {
    case 0: base = 0; break;             /* FILE_BEGIN */
    case 1: base = info.Position; break; /* FILE_CURRENT */
    case 2: base = info.Size; break;     /* FILE_END */
    default:
        SetLastError(87);
        return 0;
    }
    if (distance < 0 && (ULONGLONG)(-(distance + 1)) + 1 > base) {
        SetLastError(131); /* ERROR_NEGATIVE_SEEK */
        return 0;
    }
    ULONGLONG target = distance < 0
        ? base - ((ULONGLONG)(-(distance + 1)) + 1)
        : base + (ULONGLONG)distance;
    IO_STATUS_BLOCK iosb;
    long long position = (long long)target;
    status = NtSetInformationFile(file, &iosb, &position, sizeof(position), 14);
    if (!NT_SUCCESS(status)) {
        SetLastError(ntstatus_to_win32(status));
        return 0;
    }
    if (new_position) *new_position = position;
    return 1;
}

__declspec(dllexport) HANDLE CreateFileA(const char *name, DWORD access,
                                         DWORD share, LPVOID sa, DWORD disp,
                                         DWORD flags, HANDLE templ)
{
    (void)sa; (void)templ;
    if (!name) {
        SetLastError(87);
        return NT_INVALID_HANDLE;
    }
    WCHAR wide[260];
    if (!utf8_path_to_wide(name, wide)) {
        SetLastError(206);
        return NT_INVALID_HANDLE;
    }
    return nt_open_w(wide, access, share, disp, flags);
}

__declspec(dllexport) HANDLE CreateFileW(const WCHAR *name, DWORD access,
                                         DWORD share, LPVOID sa, DWORD disp,
                                         DWORD flags, HANDLE templ)
{
    (void)sa; (void)templ;
    return nt_open_w(name, access, share, disp, flags);
}

__declspec(dllexport) BOOL CloseHandle(HANDLE h)
{
    NtClose(h);
    return 1;
}

__declspec(dllexport) BOOL CreatePipe(HANDLE *read_pipe, HANDLE *write_pipe,
                                      void *attributes, DWORD size)
{
    (void)attributes; (void)size;
    NTSTATUS status = NtCreatePipeSimple(read_pipe, write_pipe);
    if (!NT_SUCCESS(status)) {
        SetLastError(ntstatus_to_win32(status));
        return 0;
    }
    SetLastError(0);
    return 1;
}

__declspec(dllexport) void ExitThread(DWORD code)
{
    NtTerminateThread(0, (NTSTATUS)code);
}

__declspec(dllexport) void FreeLibraryAndExitThread(HANDLE module, DWORD code)
{
    FreeLibrary(module);
    ExitThread(code);
}

__declspec(dllexport) void ExitProcess(DWORD code)
{
    void *caller = __builtin_return_address(0);
    extern long NtDisplayString(const char *text);
    static const char hex[] = "0123456789abcdef";
    char message[64];
    int n = 0;
    const char *p = "[k32] ExitProcess(code=0x";
    while (*p)
        message[n++] = *p++;
    for (int i = 0; i < 8; i++)
        message[n++] = hex[(code >> (28 - 4 * i)) & 0xF];
    p = ") caller=0x";
    while (*p)
        message[n++] = *p++;
    for (int i = 0; i < 16; i++)
        message[n++] = hex[((ULONGLONG)caller >> (60 - 4 * i)) & 0xF];
    message[n++] = '\n';
    message[n] = 0;
    NtDisplayString(message);
    NtTerminateThread(0, (NTSTATUS)code); /* simplified: ends the main thread */
}

/* Per-thread hand-off block, so the thread entry can run the user routine and
 * then terminate when it returns (like BaseThreadInitThunk on Windows). */
typedef struct {
    LPTHREAD_START_ROUTINE Start;
    LPVOID                 Param;
} THREAD_INFO;

static DWORD BaseThreadStart(THREAD_INFO *info)
{
    DWORD code = info->Start(info->Param);
    ExitThread(code); /* does not return */
    return code;
}

__declspec(dllexport) HANDLE CreateThread(LPVOID sa, ULONGLONG stack_size,
                                          LPTHREAD_START_ROUTINE start,
                                          LPVOID param, DWORD flags, DWORD *tid)
{
    (void)sa; (void)stack_size; (void)flags; (void)tid;
    THREAD_INFO *info = (THREAD_INFO *)nt_alloc(sizeof(THREAD_INFO));
    if (!info)
        return 0;
    info->Start = start;
    info->Param = param;

    HANDLE h = 0;
    NtCreateThreadEx(&h, 0, 0, NT_INVALID_HANDLE, (void *)BaseThreadStart, info,
                     0, 0, 0, 0, 0);
    return h;
}

__declspec(dllexport) DWORD WaitForSingleObject(HANDLE h, DWORD ms)
{
    long long interval;
    long long *timeout = 0;
    if (ms != (DWORD)-1) {
        interval = ms ? -(long long)ms * 10000 : 0;
        timeout = &interval;
    }
    NTSTATUS status = NtWaitForSingleObject(h, 0, timeout);
    if (status == 0x102)
        return 0x102; /* WAIT_TIMEOUT */
    if (!NT_SUCCESS(status)) {
        SetLastError(ntstatus_to_win32(status));
        return (DWORD)-1; /* WAIT_FAILED */
    }
    SetLastError(0);
    return (DWORD)status; /* WAIT_OBJECT_0 or WAIT_ABANDONED */
}

__declspec(dllexport) DWORD WaitForSingleObjectEx(HANDLE h, DWORD ms,
                                                   BOOL alertable)
{
    long long interval;
    long long *timeout = 0;
    if (ms != (DWORD)-1) {
        interval = ms ? -(long long)ms * 10000 : 0;
        timeout = &interval;
    }
    NTSTATUS status = NtWaitForSingleObject(h, alertable, timeout);
    if (status == 0x102)
        return 0x102;
    if (!NT_SUCCESS(status)) {
        SetLastError(ntstatus_to_win32(status));
        return (DWORD)-1;
    }
    SetLastError(0);
    return (DWORD)status;
}

__declspec(dllexport) DWORD WaitForMultipleObjectsEx(DWORD count,
                                                      const HANDLE *handles,
                                                      BOOL wait_all, DWORD ms,
                                                      BOOL alertable)
{
    long long interval;
    long long *timeout = 0;
    if (ms != (DWORD)-1) {
        interval = ms ? -(long long)ms * 10000 : 0;
        timeout = &interval;
    }
    NTSTATUS status = NtWaitForMultipleObjects(count, handles,
                                                wait_all ? 0 : 1,
                                                alertable, timeout);
    if (status == 0x102)
        return 0x102;
    if (!NT_SUCCESS(status)) {
        SetLastError(ntstatus_to_win32(status));
        return (DWORD)-1;
    }
    SetLastError(0);
    return (DWORD)status; /* WAIT_OBJECT_0 + signaled index */
}

__declspec(dllexport) DWORD WaitForMultipleObjects(DWORD count,
                                                    const HANDLE *handles,
                                                    BOOL wait_all, DWORD ms)
{
    return WaitForMultipleObjectsEx(count, handles, wait_all, ms, 0);
}

__declspec(dllexport) HANDLE CreateEventW(LPVOID security_attributes,
                                          BOOL manual_reset,
                                          BOOL initial_state,
                                          const WCHAR *name)
{
    (void)security_attributes;
    (void)name; /* named kernel objects are not exposed to Win32 yet */
    HANDLE event = 0;
    NTSTATUS status = NtCreateEvent(&event, 0x001F0003u, 0,
                                    manual_reset ? 0 : 1, initial_state);
    if (!NT_SUCCESS(status)) {
        SetLastError(ntstatus_to_win32(status));
        return 0;
    }
    SetLastError(0);
    return event;
}

__declspec(dllexport) HANDLE CreateEventA(LPVOID security_attributes,
                                          BOOL manual_reset,
                                          BOOL initial_state,
                                          const char *name)
{
    (void)name;
    return CreateEventW(security_attributes, manual_reset, initial_state, 0);
}

__declspec(dllexport) HANDLE CreateEventExW(LPVOID security_attributes,
                                             const WCHAR *name, DWORD flags,
                                             DWORD desired_access)
{
    const DWORD CREATE_EVENT_MANUAL_RESET = 0x1;
    const DWORD CREATE_EVENT_INITIAL_SET = 0x2;
    (void)desired_access;
    if (flags & ~(CREATE_EVENT_MANUAL_RESET | CREATE_EVENT_INITIAL_SET)) {
        SetLastError(87);
        return 0;
    }
    return CreateEventW(security_attributes,
                        (flags & CREATE_EVENT_MANUAL_RESET) != 0,
                        (flags & CREATE_EVENT_INITIAL_SET) != 0, name);
}

__declspec(dllexport) HANDLE CreateMutexExW(LPVOID security_attributes,
                                             const WCHAR *name, DWORD flags,
                                             DWORD desired_access)
{
    (void)desired_access;
    (void)security_attributes;
    (void)name;
    HANDLE mutex = 0;
    NTSTATUS status = NtCreateMutant(&mutex, desired_access, 0,
                                     (flags & 1) != 0);
    if (!NT_SUCCESS(status)) {
        SetLastError(ntstatus_to_win32(status));
        return 0;
    }
    SetLastError(0);
    return mutex;
}

__declspec(dllexport) HANDLE CreateMutexW(LPVOID security_attributes,
                                           BOOL initial_owner,
                                           const WCHAR *name)
{
    return CreateMutexExW(security_attributes, name,
                          initial_owner ? 1 : 0, 0x001f0001u);
}

__declspec(dllexport) BOOL ReleaseMutex(HANDLE mutex)
{
    NTSTATUS status = NtReleaseMutant(mutex, 0);
    if (!NT_SUCCESS(status)) {
        SetLastError(ntstatus_to_win32(status));
        return 0;
    }
    return 1;
}

__declspec(dllexport) BOOL SetEvent(HANDLE event)
{
    NTSTATUS status = NtSetEvent(event, 0);
    if (!NT_SUCCESS(status)) {
        SetLastError(ntstatus_to_win32(status));
        return 0;
    }
    SetLastError(0);
    return 1;
}

__declspec(dllexport) BOOL ResetEvent(HANDLE event)
{
    NTSTATUS status = NtResetEvent(event, 0);
    if (!NT_SUCCESS(status)) {
        SetLastError(ntstatus_to_win32(status));
        return 0;
    }
    SetLastError(0);
    return 1;
}

__declspec(dllexport) HANDLE CreateSemaphoreExW(
    LPVOID security_attributes, long initial_count, long maximum_count,
    const WCHAR *name, DWORD flags, DWORD desired_access)
{
    (void)security_attributes;
    (void)name;
    (void)desired_access;
    if (flags) {
        SetLastError(87);
        return 0;
    }
    HANDLE semaphore = 0;
    NTSTATUS status = NtCreateSemaphore(&semaphore, 0x001F0003u, 0,
                                        initial_count, maximum_count);
    if (!NT_SUCCESS(status)) {
        SetLastError(ntstatus_to_win32(status));
        return 0;
    }
    SetLastError(0);
    return semaphore;
}

__declspec(dllexport) HANDLE CreateSemaphoreW(LPVOID security_attributes,
                                               long initial_count,
                                               long maximum_count,
                                               const WCHAR *name)
{
    return CreateSemaphoreExW(security_attributes, initial_count,
                              maximum_count, name, 0, 0x001F0003u);
}

__declspec(dllexport) BOOL ReleaseSemaphore(HANDLE semaphore,
                                             long release_count,
                                             long *previous_count)
{
    NTSTATUS status = NtReleaseSemaphore(semaphore, release_count,
                                         previous_count);
    if (!NT_SUCCESS(status)) {
        SetLastError(ntstatus_to_win32(status));
        return 0;
    }
    SetLastError(0);
    return 1;
}
