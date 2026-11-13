/* Native console client APIs. Screen/input state and attachment lifetime
 * live in executive console objects, including redirected process handles. */
#include "../../../kernel/include/abi/ConsoleHost.h"
#define CONSOLE_ACCESS_RW 0xc0000000u
extern NTSTATUS SvNtConsoleManage(DWORD, ULONGLONG, ULONGLONG, ULONGLONG, ULONGLONG *);
extern int MultiByteToWideChar(UINT, DWORD, const char *, int, WCHAR *, int);
extern int WideCharToMultiByte(UINT, DWORD, const WCHAR *, int, char *, int, const char *, BOOL *);

static BOOL console_control(HANDLE handle, DWORD operation, ULONGLONG a, ULONGLONG b, ULONGLONG c,
                            ULONGLONG *out) {
    NTSTATUS status = NtConsoleControlSimple(handle, operation, a, b, c, out);
    if (!NT_SUCCESS(status)) {
        SetLastError(ntstatus_to_win32(status));
        return FALSE;
    }
    return TRUE;
}
static BOOL console_manage(DWORD operation, ULONGLONG a, ULONGLONG b, ULONGLONG c, ULONGLONG *out) {
    NTSTATUS status = SvNtConsoleManage(operation, a, b, c, out);
    if (!NT_SUCCESS(status)) {
        SetLastError(ntstatus_to_win32(status));
        return FALSE;
    }
    return TRUE;
}
static HANDLE console_open(BOOL input) {
    return OpenConsoleW(input ? L"CONIN$" : L"CONOUT$", CONSOLE_ACCESS_RW, FALSE, 3);
}
static UINT console_codepage(BOOL input) {
    HANDLE handle = console_open(input);
    if (handle == NT_INVALID_HANDLE)
        return 0;
    ULONGLONG value = 0;
    NTSTATUS status = NtConsoleControlSimple(
        handle, input ? SV_CONSOLE_CONTROL_GET_INPUT_CP : SV_CONSOLE_CONTROL_GET_OUTPUT_CP, 0, 0, 0,
        &value);
    CloseHandle(handle);
    if (status == (NTSTATUS)0xc000000du)
        return 437; /* immutable boot-console encoding */
    if (!NT_SUCCESS(status)) {
        SetLastError(ntstatus_to_win32(status));
        return 0;
    }
    return (UINT)value;
}
__declspec(dllexport) UINT GetConsoleCP(void) { return console_codepage(TRUE); }
static BOOL console_set_codepage(UINT codepage, BOOL input) {
    HANDLE handle = console_open(input);
    if (handle == NT_INVALID_HANDLE)
        return FALSE;
    NTSTATUS status = NtConsoleControlSimple(
        handle, input ? SV_CONSOLE_CONTROL_SET_INPUT_CP : SV_CONSOLE_CONTROL_SET_OUTPUT_CP,
        codepage, 0, 0, 0);
    CloseHandle(handle);
    if (status == (NTSTATUS)0xc000000du && codepage == 437)
        return TRUE;
    if (!NT_SUCCESS(status)) {
        SetLastError(ntstatus_to_win32(status));
        return FALSE;
    }
    return TRUE;
}
__declspec(dllexport) BOOL SetConsoleCP(UINT codepage) {
    return console_set_codepage(codepage, TRUE);
}
__declspec(dllexport) BOOL SetConsoleOutputCP(UINT codepage) {
    return console_set_codepage(codepage, FALSE);
}
static void console_reset_handlers(void);
__declspec(dllexport) BOOL AllocConsole(void) {
    BOOL ok = console_manage(SV_CONSOLE_ALLOCATE, 0, 0, 0, 0);
    if (ok)
        console_reset_handlers();
    return ok;
}
__declspec(dllexport) BOOL FreeConsole(void) {
    BOOL ok = console_manage(SV_CONSOLE_FREE, 0, 0, 0, 0);
    if (ok)
        console_reset_handlers();
    return ok;
}
__declspec(dllexport) BOOL AttachConsole(DWORD process) {
    BOOL ok = console_manage(SV_CONSOLE_ATTACH, process, 0, 0, 0);
    if (ok)
        console_reset_handlers();
    return ok;
}
__declspec(dllexport) HANDLE GetConsoleWindow(void) {
    ULONGLONG value = 0;
    return console_manage(SV_CONSOLE_GET_WINDOW, 0, 0, 0, &value) ? (HANDLE)value : 0;
}
__declspec(dllexport) DWORD GetConsoleProcessList(DWORD *processes, DWORD capacity) {
    ULONGLONG count = 0;
    return console_manage(SV_CONSOLE_GET_PROCESS_LIST, (ULONGLONG)processes, capacity, 0, &count)
               ? (DWORD)count
               : 0;
}
__declspec(dllexport) HANDLE CreateConsoleScreenBuffer(DWORD access, DWORD share, void *security,
                                                       DWORD flags, void *data) {
    if (flags != 1 || data || access & ~CONSOLE_ACCESS_RW || share & ~3u) {
        SetLastError(87);
        return NT_INVALID_HANDLE;
    }
    ULONGLONG handle = 0;
    return console_manage(SV_CONSOLE_CREATE_BUFFER, access, share, (ULONGLONG)security, &handle)
               ? (HANDLE)handle
               : NT_INVALID_HANDLE;
}
__declspec(dllexport) BOOL SetConsoleActiveScreenBuffer(HANDLE handle) {
    return console_manage(SV_CONSOLE_ACTIVATE_BUFFER, (ULONGLONG)handle, 0, 0, 0);
}
__declspec(dllexport) BOOL FillConsoleOutputCharacterW(HANDLE handle, WCHAR character, DWORD count,
                                                       COORD origin, DWORD *written) {
    ULONGLONG done = 0;
    BOOL ok = console_control(handle, SV_CONSOLE_CONTROL_FILL_WIDE, character, count,
                              (WORD)origin.X | ((DWORD)(WORD)origin.Y << 16), &done);
    if (written)
        *written = (DWORD)done;
    return ok;
}
__declspec(dllexport) COORD GetLargestConsoleWindowSize(HANDLE handle) {
    CONSOLE_SCREEN_BUFFER_INFO info;
    return GetConsoleScreenBufferInfo(handle, &info) ? info.MaximumWindowSize : (COORD){0, 0};
}
__declspec(dllexport) BOOL SetConsoleScreenBufferSize(HANDLE handle, COORD size) {
    return console_control(handle, SV_CONSOLE_CONTROL_RESIZE_BUFFER,
                           (WORD)size.X | ((DWORD)(WORD)size.Y << 16), 0, 0, 0);
}
__declspec(dllexport) BOOL SetConsoleWindowInfo(HANDLE handle, BOOL absolute,
                                                const SMALL_RECT *window) {
    return console_control(handle, SV_CONSOLE_CONTROL_SET_WINDOW, (ULONGLONG)window, absolute != 0,
                           0, 0);
}
__declspec(dllexport) BOOL FlushConsoleInputBuffer(HANDLE handle) {
    return console_control(handle, SV_CONSOLE_CONTROL_FLUSH_INPUT, 0, 0, 0, 0);
}
__declspec(dllexport) BOOL GetNumberOfConsoleInputEvents(HANDLE handle, DWORD *count) {
    if (!count) {
        SetLastError(87);
        return FALSE;
    }
    ULONGLONG value = 0;
    BOOL ok = console_control(handle, SV_CONSOLE_CONTROL_COUNT_INPUT, 0, 0, 0, &value);
    *count = (DWORD)value;
    return ok;
}
__declspec(dllexport) BOOL PeekConsoleInputW(HANDLE handle, void *records, DWORD capacity,
                                             DWORD *read) {
    ULONGLONG done = 0;
    BOOL ok = console_control(handle, SV_CONSOLE_CONTROL_PEEK_INPUT, (ULONGLONG)records, capacity,
                              0, &done);
    if (read)
        *read = (DWORD)done;
    return ok;
}
__declspec(dllexport) BOOL ReadConsoleInputW(HANDLE handle, void *records, DWORD capacity,
                                             DWORD *read) {
    ULONGLONG done = 0;
    BOOL ok = console_control(handle, SV_CONSOLE_CONTROL_READ_INPUT, (ULONGLONG)records, capacity,
                              0, &done);
    if (read)
        *read = (DWORD)done;
    return ok;
}
__declspec(dllexport) BOOL WriteConsoleInputW(HANDLE handle, const void *records, DWORD count,
                                              DWORD *written) {
    ULONGLONG done = 0;
    BOOL ok = console_control(handle, SV_CONSOLE_CONTROL_WRITE_INPUT, (ULONGLONG)records, count, 0,
                              &done);
    if (written)
        *written = (DWORD)done;
    return ok;
}
__declspec(dllexport) BOOL ReadConsoleW(HANDLE handle, void *buffer, DWORD capacity, DWORD *read,
                                        void *control) {
    ULONGLONG done = 0;
    BOOL ok = console_control(handle, SV_CONSOLE_CONTROL_READ_WIDE, (ULONGLONG)buffer, capacity,
                              (ULONGLONG)control, &done);
    if (read)
        *read = (DWORD)done;
    return ok;
}
__declspec(dllexport) BOOL ReadConsoleA(HANDLE handle, void *buffer, DWORD capacity, DWORD *read,
                                        void *reserved) {
    if (reserved) {
        SetLastError(87);
        if (read)
            *read = 0;
        return FALSE;
    }
    DWORD mode;
    if (!GetConsoleMode(handle, &mode))
        return FALSE;
    IO_STATUS_BLOCK io = {0};
    NTSTATUS status = NtReadFile(handle, 0, 0, 0, &io, buffer, capacity, 0, 0);
    if (read)
        *read = (DWORD)io.Information;
    if (!NT_SUCCESS(status)) {
        SetLastError(ntstatus_to_win32(status));
        return FALSE;
    }
    return TRUE;
}
__declspec(dllexport) BOOL WriteConsoleA(HANDLE handle, const char *text, DWORD count,
                                         DWORD *written, void *reserved) {
    if (reserved || count > 0x7fffffff || (!text && count)) {
        SetLastError(87);
        if (written)
            *written = 0;
        return FALSE;
    }
    UINT codepage = GetConsoleOutputCP();
    int chars = count ? MultiByteToWideChar(codepage, 0, text, (int)count, 0, 0) : 0;
    if (count && !chars) {
        if (written)
            *written = 0;
        return FALSE;
    }
    WCHAR *wide = chars ? HeapAlloc(GetProcessHeap(), 0, (SIZE_T)chars * sizeof(WCHAR)) : 0;
    if (chars && !wide) {
        SetLastError(8);
        if (written)
            *written = 0;
        return FALSE;
    }
    if (chars && !MultiByteToWideChar(codepage, 0, text, (int)count, wide, chars)) {
        HeapFree(GetProcessHeap(), 0, wide);
        return FALSE;
    }
    DWORD done = 0;
    BOOL ok = WriteConsoleW(handle, wide, (DWORD)chars, &done, 0);
    if (written)
        *written = ok ? count : 0;
    if (wide)
        HeapFree(GetProcessHeap(), 0, wide);
    return ok;
}
static BOOL console_input_ansi(HANDLE handle, void *records, DWORD count, DWORD *done, BOOL peek,
                               BOOL write) {
    if (count > 0x7fffffff / sizeof(SV_CONSOLE_INPUT_RECORD) || (!records && count)) {
        SetLastError(87);
        return FALSE;
    }
    SV_CONSOLE_INPUT_RECORD *temporary =
        count ? HeapAlloc(GetProcessHeap(), 0, (SIZE_T)count * sizeof(*temporary)) : 0;
    if (count && !temporary) {
        SetLastError(8);
        return FALSE;
    }
    DWORD transferred = 0;
    BOOL ok;
    if (write) {
        memcpy(temporary, records, (SIZE_T)count * sizeof(*temporary));
        for (DWORD i = 0; i < count; i++)
            if (temporary[i].EventType == 1) {
                char byte = (char)temporary[i].Event.Key.Character;
                WCHAR wide = 0;
                if (!MultiByteToWideChar(GetConsoleCP(), 0, &byte, 1, &wide, 1)) {
                    HeapFree(GetProcessHeap(), 0, temporary);
                    return FALSE;
                }
                temporary[i].Event.Key.Character = wide;
            }
        ok = WriteConsoleInputW(handle, temporary, count, &transferred);
    } else {
        ok = peek ? PeekConsoleInputW(handle, temporary, count, &transferred)
                  : ReadConsoleInputW(handle, temporary, count, &transferred);
        if (ok) {
            for (DWORD i = 0; i < transferred; i++)
                if (temporary[i].EventType == 1) {
                    char byte = '?';
                    WCHAR wide = temporary[i].Event.Key.Character;
                    if (!WideCharToMultiByte(GetConsoleCP(), 0, &wide, 1, &byte, 1, 0, 0)) {
                        ok = FALSE;
                        break;
                    }
                    temporary[i].Event.Key.Character = (BYTE)byte;
                }
            if (ok && transferred)
                memcpy(records, temporary, (SIZE_T)transferred * sizeof(*temporary));
        }
    }
    if (done)
        *done = transferred;
    if (temporary)
        HeapFree(GetProcessHeap(), 0, temporary);
    return ok;
}
__declspec(dllexport) BOOL PeekConsoleInputA(HANDLE handle, void *records, DWORD capacity,
                                             DWORD *read) {
    return console_input_ansi(handle, records, capacity, read, TRUE, FALSE);
}
__declspec(dllexport) BOOL ReadConsoleInputA(HANDLE handle, void *records, DWORD capacity,
                                             DWORD *read) {
    return console_input_ansi(handle, records, capacity, read, FALSE, FALSE);
}
__declspec(dllexport) BOOL WriteConsoleInputA(HANDLE handle, const void *records, DWORD count,
                                              DWORD *written) {
    return console_input_ansi(handle, (void *)records, count, written, FALSE, TRUE);
}
static BOOL console_output(HANDLE handle, void *cells, COORD size, COORD origin, SMALL_RECT *region,
                           BOOL write) {
    if (!region) {
        SetLastError(87);
        return FALSE;
    }
    SV_CONSOLE_BUFFER_TRANSFER transfer = {
        .Cells = (ULONGLONG)cells,
        .Width = size.X,
        .Height = size.Y,
        .X = origin.X,
        .Y = origin.Y,
        .Region = {region->Left, region->Top, region->Right, region->Bottom}};
    BOOL ok = console_control(
        handle, write ? SV_CONSOLE_CONTROL_WRITE_OUTPUT : SV_CONSOLE_CONTROL_READ_OUTPUT,
        (ULONGLONG)&transfer, 0, 0, 0);
    if (ok)
        *region = (SMALL_RECT){transfer.Region.Left, transfer.Region.Top, transfer.Region.Right,
                               transfer.Region.Bottom};
    return ok;
}
__declspec(dllexport) BOOL ReadConsoleOutputW(HANDLE handle, void *cells, COORD size, COORD origin,
                                              SMALL_RECT *region) {
    return console_output(handle, cells, size, origin, region, FALSE);
}
__declspec(dllexport) BOOL WriteConsoleOutputW(HANDLE handle, const void *cells, COORD size,
                                               COORD origin, SMALL_RECT *region) {
    return console_output(handle, (void *)cells, size, origin, region, TRUE);
}
__declspec(dllexport) BOOL GetConsoleScreenBufferInfoEx(HANDLE handle,
                                                        SV_CONSOLE_SCREEN_INFO_EX *info) {
    return console_control(handle, SV_CONSOLE_CONTROL_GET_INFO_EX, (ULONGLONG)info, 0, 0, 0);
}
__declspec(dllexport) BOOL SetConsoleScreenBufferInfoEx(HANDLE handle,
                                                        SV_CONSOLE_SCREEN_INFO_EX *info) {
    return console_control(handle, SV_CONSOLE_CONTROL_SET_INFO_EX, (ULONGLONG)info, 0, 0, 0);
}
__declspec(dllexport) DWORD GetConsoleTitleW(WCHAR *title, DWORD capacity) {
    HANDLE handle = console_open(FALSE);
    if (handle == NT_INVALID_HANDLE)
        return 0;
    ULONGLONG count = 0;
    BOOL ok = console_control(handle, SV_CONSOLE_CONTROL_GET_TITLE, (ULONGLONG)title, capacity, 0,
                              &count);
    CloseHandle(handle);
    return ok ? (DWORD)count : 0;
}
__declspec(dllexport) BOOL SetConsoleTitleW(const WCHAR *title) {
    if (!title) {
        SetLastError(87);
        return FALSE;
    }
    SIZE_T count = 0;
    while (title[count] && count < 32768)
        count++;
    if (count == 32768) {
        SetLastError(87);
        return FALSE;
    }
    HANDLE handle = console_open(FALSE);
    if (handle == NT_INVALID_HANDLE)
        return FALSE;
    BOOL ok = console_control(handle, SV_CONSOLE_CONTROL_SET_TITLE, (ULONGLONG)title, count, 0, 0);
    CloseHandle(handle);
    return ok;
}
__declspec(dllexport) BOOL SetConsoleTitleA(const char *title) {
    if (!title) {
        SetLastError(87);
        return FALSE;
    }
    int chars = MultiByteToWideChar(GetConsoleCP(), 0, title, -1, 0, 0);
    if (!chars)
        return FALSE;
    WCHAR *wide = HeapAlloc(GetProcessHeap(), 0, (SIZE_T)chars * sizeof(WCHAR));
    if (!wide) {
        SetLastError(8);
        return FALSE;
    }
    BOOL ok =
        MultiByteToWideChar(GetConsoleCP(), 0, title, -1, wide, chars) && SetConsoleTitleW(wide);
    HeapFree(GetProcessHeap(), 0, wide);
    return ok;
}
__declspec(dllexport) DWORD GetConsoleTitleA(char *title, DWORD capacity) {
    if (!title || !capacity) {
        SetLastError(87);
        return 0;
    }
    WCHAR *wide = HeapAlloc(GetProcessHeap(), 0, 32768 * sizeof(WCHAR));
    if (!wide) {
        SetLastError(8);
        return 0;
    }
    DWORD count = GetConsoleTitleW(wide, 32768);
    int bytes = count ? WideCharToMultiByte(GetConsoleCP(), 0, wide, (int)count + 1, title,
                                            (int)capacity, 0, 0)
                      : 0;
    if (!count)
        title[0] = 0;
    HeapFree(GetProcessHeap(), 0, wide);
    return bytes > 0 ? (DWORD)bytes - 1 : 0;
}

typedef BOOL (*CONSOLE_HANDLER)(DWORD);
typedef struct {
    void *Debug;
    LONG LockCount, RecursionCount;
    HANDLE Owner, Semaphore;
    ULONGLONG Spin;
} CONSOLE_HANDLER_LOCK;
static CONSOLE_HANDLER_LOCK g_console_handler_lock = {.LockCount = -1};
static CONSOLE_HANDLER *g_console_handlers;
static SIZE_T g_console_handler_count, g_console_handler_capacity;
static BOOL g_console_ignore_ctrl_c;
static BOOL g_console_control_state_loaded;
static void console_reset_handlers(void) {
    EnterCriticalSection(&g_console_handler_lock);
    if (g_console_handlers)
        HeapFree(GetProcessHeap(), 0, g_console_handlers);
    g_console_handlers = 0;
    g_console_handler_count = g_console_handler_capacity = 0;
    g_console_ignore_ctrl_c = FALSE;
    g_console_control_state_loaded = FALSE;
    LeaveCriticalSection(&g_console_handler_lock);
}
static DWORD console_control_dispatch(void *parameter) {
    DWORD event = (DWORD)(ULONGLONG)parameter;
    EnterCriticalSection(&g_console_handler_lock);
    SIZE_T count = g_console_handler_count;
    CONSOLE_HANDLER *handlers =
        count ? HeapAlloc(GetProcessHeap(), 0, count * sizeof(*handlers)) : 0;
    if (handlers)
        memcpy(handlers, g_console_handlers, count * sizeof(*handlers));
    LeaveCriticalSection(&g_console_handler_lock);
    BOOL handled = FALSE;
    if (handlers) {
        for (SIZE_T i = count; i > 0; i--)
            if (handlers[i - 1](event)) {
                handled = TRUE;
                break;
            }
        HeapFree(GetProcessHeap(), 0, handlers);
    }
    if (!handled || event >= 2)
        ExitProcess(0xc000013au);
    __declspec(dllexport) void ExitThread(DWORD);
    ExitThread(0);
    return 0;
}
__declspec(dllexport) BOOL SetConsoleCtrlHandler(CONSOLE_HANDLER handler, BOOL add) {
    ULONGLONG window = 0;
    if (!console_manage(SV_CONSOLE_GET_WINDOW, 0, 0, 0, &window))
        return FALSE;
    EnterCriticalSection(&g_console_handler_lock);
    if (!g_console_control_state_loaded) {
        ULONGLONG ignored = 0;
        if (!console_manage(SV_CONSOLE_GET_CONTROL_STATE, 0, 0, 0, &ignored)) {
            LeaveCriticalSection(&g_console_handler_lock);
            return FALSE;
        }
        g_console_ignore_ctrl_c = ignored != 0;
        g_console_control_state_loaded = TRUE;
    }
    BOOL ok = TRUE;
    if (!handler)
        g_console_ignore_ctrl_c = add != 0;
    else if (add) {
        if (g_console_handler_count == g_console_handler_capacity) {
            SIZE_T capacity = g_console_handler_capacity ? g_console_handler_capacity * 2 : 8;
            CONSOLE_HANDLER *handlers =
                HeapAlloc(GetProcessHeap(), 0, capacity * sizeof(*handlers));
            if (!handlers) {
                SetLastError(8);
                ok = FALSE;
            } else {
                if (g_console_handler_count)
                    memcpy(handlers, g_console_handlers,
                           g_console_handler_count * sizeof(*handlers));
                if (g_console_handlers)
                    HeapFree(GetProcessHeap(), 0, g_console_handlers);
                g_console_handlers = handlers;
                g_console_handler_capacity = capacity;
            }
        }
        if (ok)
            g_console_handlers[g_console_handler_count++] = handler;
    } else {
        SIZE_T found = g_console_handler_count;
        while (found && g_console_handlers[found - 1] != handler)
            found--;
        if (!found) {
            SetLastError(87);
            ok = FALSE;
        } else {
            for (SIZE_T i = found; i < g_console_handler_count; i++)
                g_console_handlers[i - 1] = g_console_handlers[i];
            g_console_handler_count--;
        }
    }
    if (ok)
        ok = console_manage(SV_CONSOLE_REGISTER_CONTROL, (ULONGLONG)console_control_dispatch,
                            g_console_ignore_ctrl_c, 0, 0);
    LeaveCriticalSection(&g_console_handler_lock);
    return ok;
}
__declspec(dllexport) BOOL GenerateConsoleCtrlEvent(DWORD event, DWORD group) {
    if (event > 1) {
        SetLastError(87);
        return FALSE;
    }
    return console_manage(SV_CONSOLE_GENERATE_CONTROL, event, group, 0, 0);
}
