#include "User32.h"
#include <stddef.h>

__declspec(dllimport) LONG_PTR GetWindowLongPtrW(HWND, int);
__declspec(dllimport) LONG_PTR SetWindowLongPtrW(HWND, int, LONG_PTR);
__declspec(dllimport) BOOL EnableWindow(HWND, BOOL);
__declspec(dllimport) BOOL IsWindowEnabled(HWND);
__declspec(dllimport) DWORD GetFullPathNameW(const WCHAR *, DWORD, WCHAR *, WCHAR **);
__declspec(dllimport) DWORD GetFileAttributesW(const WCHAR *);

#define OFN_OVERWRITEPROMPT 0x00000002u
#define OFN_PATHMUSTEXIST   0x00000800u
#define OFN_FILEMUSTEXIST   0x00001000u
#define CDERR_STRUCTSIZE   1u
#define CDERR_DIALOGFAILURE 0xffffu
#define CDERR_INITIALIZATION 2u
#define FNERR_BUFFERTOOSMALL 0x3003u
#define PDERR_NODEVICES    0x1007u
#define GWLP_USERDATA      (-21)
#define MAX_PATH           260

typedef struct {
    DWORD Size;
    HWND Owner;
    HINSTANCE Instance;
    const WCHAR *Filter;
    WCHAR *CustomFilter;
    DWORD MaxCustomFilter, FilterIndex;
    WCHAR *File;
    DWORD MaxFile;
    WCHAR *FileTitle;
    DWORD MaxFileTitle;
    const WCHAR *InitialDir, *Title;
    DWORD Flags;
    WORD FileOffset, FileExtension;
    const WCHAR *DefaultExt;
    LPARAM Data;
    void *Hook;
    const WCHAR *Template;
    void *ReservedPointer;
    DWORD Reserved, FlagsEx;
} SV_OPENFILENAMEW;
_Static_assert(sizeof(SV_OPENFILENAMEW) == 152, "OPENFILENAMEW x64");

typedef struct {
    HWND Edit, Status, Confirm;
    int Choice;
    WCHAR Replace[MAX_PATH];
} SV_PATH_DIALOG;

__declspec(dllimport) DWORD TlsAlloc(void);
__declspec(dllimport) BOOL TlsFree(DWORD index);
__declspec(dllimport) void *TlsGetValue(DWORD index);
__declspec(dllimport) BOOL TlsSetValue(DWORD index, void *value);

static DWORD dialog_tls_slot;

static void set_dialog_error(DWORD error)
{
    DWORD slot = __atomic_load_n(&dialog_tls_slot, __ATOMIC_ACQUIRE);
    if (!slot && error) {
        DWORD index = TlsAlloc();
        if (index == (DWORD)-1) return;
        if (__atomic_compare_exchange_n(&dialog_tls_slot, &slot, index + 1, 0,
                                        __ATOMIC_RELEASE, __ATOMIC_ACQUIRE))
            slot = index + 1;
        else
            TlsFree(index);
    }
    if (slot) TlsSetValue(slot - 1, (void *)(UINT_PTR)error);
}

__declspec(dllexport) DWORD CommDlgExtendedError(void)
{
    DWORD slot = __atomic_load_n(&dialog_tls_slot, __ATOMIC_ACQUIRE);
    return slot ? (DWORD)(UINT_PTR)TlsGetValue(slot - 1) : 0;
}

static const WCHAR *file_name(const WCHAR *path)
{
    const WCHAR *name = path;
    for (const WCHAR *p = path; *p; p++)
        if (*p == ':' || *p == '/' || *p == '\\') name = p + 1;
    return name;
}

static DWORD wide_length(const WCHAR *text)
{
    DWORD count = 0;
    while (text[count]) count++;
    return count;
}

static void copy_text(WCHAR *out, DWORD capacity, const WCHAR *text)
{
    if (!out || !capacity) return;
    DWORD n = 0;
    while (n + 1 < capacity && text[n]) {
        out[n] = text[n];
        n++;
    }
    out[n] = 0;
}

__declspec(dllexport) short GetFileTitleW(const WCHAR *path, WCHAR *title, WORD capacity)
{
    if (!path || !*path || !title) return -1;
    const WCHAR *name = file_name(path);
    if (!*name) return -1;
    for (const WCHAR *p = name; *p; p++)
        if (*p == '*' || *p == '?' || *p == '[' || *p == ']') return -1;
    DWORD need = wide_length(name) + 1;
    if (need > 32767) return -1;
    if (need > capacity) return (short)need;
    copy_text(title, capacity, name);
    return 0;
}

static LRESULT path_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    if (message == WM_NCCREATE) {
        const CREATESTRUCTW *create = (const CREATESTRUCTW *)lparam;
        SetWindowLongPtrW(window, GWLP_USERDATA, (LONG_PTR)create->lpCreateParams);
        return TRUE;
    }
    SV_PATH_DIALOG *dialog = (SV_PATH_DIALOG *)GetWindowLongPtrW(window, GWLP_USERDATA);
    if (dialog && message == WM_CLOSE) {
        dialog->Choice = IDCANCEL;
        return 0;
    }
    if (dialog && message == WM_COMMAND &&
        (LOWORD(wparam) == IDOK || LOWORD(wparam) == IDCANCEL)) {
        dialog->Choice = LOWORD(wparam);
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

static HWND path_control(HWND parent, const WCHAR *class_name, const WCHAR *text,
    DWORD style, int x, int y, int width, int height, UINT id)
{
    return CreateWindowExW(0, class_name, text, WS_CHILD | WS_VISIBLE | style,
        x, y, width, height, parent, (HMENU)(UINT_PTR)id, 0, 0);
}

static BOOL same_path(const WCHAR *a, const WCHAR *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static const WCHAR *validate_path(SV_OPENFILENAMEW *info, BOOL save,
    WCHAR *typed, WCHAR *full, WCHAR **name, DWORD *length, BOOL *exists)
{
    if (!*typed) return L"Enter a file path.";
    for (const WCHAR *p = typed; *p; p++)
        if (*p == '*' || *p == '?') return L"Enter a single file path.";

    const WCHAR *base = file_name(typed);
    BOOL extension = FALSE;
    for (const WCHAR *p = base; *p; p++)
        if (*p == '.') extension = TRUE;
    if (save && !extension && info->DefaultExt && *info->DefaultExt) {
        DWORD n = wide_length(typed);
        DWORD ext = wide_length(info->DefaultExt);
        if (n + ext + 2 > MAX_PATH) return L"The file path is too long.";
        typed[n++] = '.';
        copy_text(typed + n, MAX_PATH - n, info->DefaultExt);
    }

    WCHAR combined[MAX_PATH];
    const WCHAR *input = typed;
    if (info->InitialDir && *info->InitialDir && typed[1] != ':' &&
        typed[0] != '\\' && typed[0] != '/') {
        DWORD dir = wide_length(info->InitialDir), count = wide_length(typed);
        if (dir + count + 2 > MAX_PATH) return L"The file path is too long.";
        copy_text(combined, MAX_PATH, info->InitialDir);
        if (combined[dir - 1] != '\\' && combined[dir - 1] != '/') combined[dir++] = '\\';
        copy_text(combined + dir, MAX_PATH - dir, typed);
        input = combined;
    }
    *length = GetFullPathNameW(input, MAX_PATH, full, name);
    if (!*length || *length >= MAX_PATH || !*name || !**name)
        return L"Enter a valid file path.";

    DWORD attributes = GetFileAttributesW(full);
    *exists = attributes != (DWORD)-1;
    if (*exists && (attributes & 0x10)) return L"The path points to a directory.";
    if (!*exists && !save && (info->Flags & OFN_FILEMUSTEXIST))
        return L"The file does not exist.";
    if (!*exists && (info->Flags & OFN_PATHMUSTEXIST)) {
        WCHAR *separator = *name - 1;
        if (separator == full + 2) separator++;
        WCHAR saved = *separator;
        *separator = 0;
        attributes = GetFileAttributesW(full);
        *separator = saved;
        if (attributes == (DWORD)-1 || !(attributes & 0x10))
            return L"The directory does not exist.";
    }
    return 0;
}

static BOOL file_dialog(SV_OPENFILENAMEW *info, BOOL save)
{
    set_dialog_error(0);
    if (!info || (info->Size != sizeof(*info) &&
                  info->Size != offsetof(SV_OPENFILENAMEW, ReservedPointer))) {
        set_dialog_error(CDERR_STRUCTSIZE);
        return FALSE;
    }
    if (!info->File || !info->MaxFile) {
        set_dialog_error(CDERR_INITIALIZATION);
        return FALSE;
    }

    WNDCLASSEXW window_class = {0};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = path_proc;
    window_class.lpszClassName = L"SvCommonFileDialog";
    window_class.hbrBackground = (HBRUSH)(UINT_PTR)16;
    window_class.hCursor = LoadCursorW(0, IDC_ARROW);
    RegisterClassExW(&window_class);

    SV_PATH_DIALOG state = {0};
    const WCHAR *title = info->Title ? info->Title : (save ? L"Save As" : L"Open");
    HWND window = CreateWindowExW(0, window_class.lpszClassName, title,
        WS_CAPTION | WS_SYSMENU | WS_POPUP,
        (GetSystemMetrics(SM_CXSCREEN) - 490) / 2,
        (GetSystemMetrics(SM_CYSCREEN) - 166) / 2, 490, 166,
        info->Owner, 0, 0, &state);
    if (!window) {
        set_dialog_error(CDERR_DIALOGFAILURE);
        return FALSE;
    }
    WCHAR initial[MAX_PATH];
    DWORD capacity = info->MaxFile < MAX_PATH ? info->MaxFile : MAX_PATH;
    copy_text(initial, capacity, info->File);
    path_control(window, L"STATIC", L"File name", 0, 12, 12, 74, 23, 0);
    state.Edit = path_control(window, L"EDIT", initial,
        WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, 88, 12, 376, 25, 10);
    state.Status = path_control(window, L"STATIC", L"", 0, 12, 47, 452, 24, 0);
    state.Confirm = path_control(window, L"BUTTON", save ? L"Save" : L"Open",
        WS_TABSTOP | BS_DEFPUSHBUTTON, 288, 82, 82, 30, IDOK);
    HWND cancel = path_control(window, L"BUTTON", L"Cancel", WS_TABSTOP,
        382, 82, 82, 30, IDCANCEL);
    if (!state.Edit || !state.Status || !state.Confirm || !cancel) {
        set_dialog_error(CDERR_DIALOGFAILURE);
        DestroyWindow(window);
        return FALSE;
    }

    BOOL owner_enabled = info->Owner && IsWindowEnabled(info->Owner);
    if (owner_enabled) EnableWindow(info->Owner, FALSE);
    ShowWindow(window, SW_SHOW);
    SetFocus(state.Edit);
    SendMessageW(state.Edit, 0x00b1 /* EM_SETSEL */, 0, -1);

    BOOL ok = FALSE;
    MSG message;
    int result = -1;
    while ((result = GetMessageW(&message, 0, 0, 0)) > 0) {
        if (message.message == WM_KEYDOWN &&
            (message.hwnd == state.Edit || message.hwnd == window) &&
            (message.wParam == VK_RETURN || message.wParam == VK_ESCAPE))
            state.Choice = message.wParam == VK_RETURN ? IDOK : IDCANCEL;
        else {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (state.Choice == IDCANCEL) break;
        if (state.Choice != IDOK) continue;
        state.Choice = 0;
        WCHAR typed[MAX_PATH], full[MAX_PATH], *name = 0;
        DWORD length = 0;
        BOOL exists = FALSE;
        GetWindowTextW(state.Edit, typed, MAX_PATH);
        const WCHAR *error = validate_path(info, save, typed, full, &name, &length, &exists);
        if (!error && save && exists && (info->Flags & OFN_OVERWRITEPROMPT) &&
            !same_path(state.Replace, full)) {
            copy_text(state.Replace, MAX_PATH, full);
            SetWindowTextW(state.Confirm, L"Replace");
            SetWindowTextW(state.Status, L"The file already exists. Select Replace to confirm.");
            continue;
        }
        if (error) {
            state.Replace[0] = 0;
            SetWindowTextW(state.Confirm, save ? L"Save" : L"Open");
            SetWindowTextW(state.Status, error);
            continue;
        }
        if (length >= info->MaxFile) {
            info->File[0] = (WCHAR)(length + 1);
            set_dialog_error(FNERR_BUFFERTOOSMALL);
            break;
        }
        copy_text(info->File, info->MaxFile, full);
        copy_text(info->FileTitle, info->MaxFileTitle, name);
        info->FileOffset = (WORD)(name - full);
        info->FileExtension = 0;
        for (const WCHAR *p = name; *p; p++)
            if (*p == '.' && p[1]) info->FileExtension = (WORD)(p + 1 - full);
        ok = TRUE;
        break;
    }
    if (!result) PostQuitMessage((int)message.wParam);
    DestroyWindow(window);
    if (owner_enabled) {
        EnableWindow(info->Owner, TRUE);
        SetActiveWindow(info->Owner);
    }
    return ok;
}

__declspec(dllexport) BOOL GetOpenFileNameW(SV_OPENFILENAMEW *info)
{
    return file_dialog(info, FALSE);
}

__declspec(dllexport) BOOL GetSaveFileNameW(SV_OPENFILENAMEW *info)
{
    return file_dialog(info, TRUE);
}

__declspec(dllexport) BOOL PageSetupDlgW(void *dialog)
{
    (void)dialog;
    set_dialog_error(PDERR_NODEVICES);
    return FALSE;
}

__declspec(dllexport) long PrintDlgExW(void *dialog)
{
    (void)dialog;
    set_dialog_error(PDERR_NODEVICES);
    return (long)0x80070490u; /* HRESULT_FROM_WIN32(ERROR_NOT_FOUND) */
}

__declspec(dllimport) HANDLE GetProcessHeap(void);
__declspec(dllimport) void *HeapAlloc(HANDLE, DWORD, ULONG_PTR);
__declspec(dllimport) BOOL HeapFree(HANDLE, DWORD, void *);
__declspec(dllimport) UINT RegisterWindowMessageW(const WCHAR *);

typedef struct {
    DWORD Size;
    HWND Owner;
    HINSTANCE Instance;
    DWORD Flags;
    WCHAR *Find, *Replace;
    WORD FindCapacity, ReplaceCapacity;
    LPARAM Data;
    void *Hook;
    const WCHAR *Template;
} SV_FINDREPLACEW;
_Static_assert(sizeof(SV_FINDREPLACEW) == 80, "FINDREPLACEW x64");

enum { FIND_NEXT = 0x08, FIND_REPLACE = 0x10, FIND_REPLACE_ALL = 0x20,
       FIND_DIALOG_TERM = 0x40, FIND_WHOLE_WORD = 0x02, FIND_MATCH_CASE = 0x04 };

typedef struct {
    SV_FINDREPLACEW *Request;
    HWND Find, Replace, WholeWord, MatchCase;
    UINT Notification;
    BOOL Closing;
} SV_SEARCH_DIALOG;

static LRESULT search_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    if (message == WM_NCCREATE) {
        const CREATESTRUCTW *create = (const CREATESTRUCTW *)lparam;
        SetWindowLongPtrW(window, GWLP_USERDATA, (LONG_PTR)create->lpCreateParams);
        return TRUE;
    }
    SV_SEARCH_DIALOG *dialog = (SV_SEARCH_DIALOG *)GetWindowLongPtrW(window, GWLP_USERDATA);
    if (!dialog) return DefWindowProcW(window, message, wparam, lparam);
    if (message == WM_CLOSE) {
        DestroyWindow(window);
        return 0;
    }
    if (message == WM_NCDESTROY) {
        if (!dialog->Closing) {
            dialog->Closing = TRUE;
            dialog->Request->Flags &= ~(FIND_NEXT | FIND_REPLACE | FIND_REPLACE_ALL);
            dialog->Request->Flags |= FIND_DIALOG_TERM;
            SendMessageW(dialog->Request->Owner, dialog->Notification, 0,
                         (LPARAM)dialog->Request);
        }
        HeapFree(GetProcessHeap(), 0, dialog);
        return 0;
    }
    if (message == WM_COMMAND) {
        UINT command = LOWORD(wparam);
        if (command == IDCANCEL) {
            DestroyWindow(window);
            return 0;
        }
        if (command == IDOK || command == 11 || command == 12) {
            SV_FINDREPLACEW *request = dialog->Request;
            if (!GetWindowTextW(dialog->Find, request->Find, request->FindCapacity)) return 0;
            if (dialog->Replace)
                GetWindowTextW(dialog->Replace, request->Replace, request->ReplaceCapacity);
            request->Flags &= ~(FIND_NEXT | FIND_REPLACE | FIND_REPLACE_ALL |
                                FIND_WHOLE_WORD | FIND_MATCH_CASE);
            if (SendMessageW(dialog->WholeWord, BM_GETCHECK, 0, 0))
                request->Flags |= FIND_WHOLE_WORD;
            if (SendMessageW(dialog->MatchCase, BM_GETCHECK, 0, 0))
                request->Flags |= FIND_MATCH_CASE;
            request->Flags |= command == 12 ? FIND_REPLACE_ALL :
                              command == 11 ? FIND_REPLACE : FIND_NEXT;
            SendMessageW(request->Owner, dialog->Notification, 0, (LPARAM)request);
            return 0;
        }
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

static HWND search_dialog(SV_FINDREPLACEW *request, BOOL replace)
{
    set_dialog_error(0);
    if (!request || request->Size != sizeof(*request)) {
        set_dialog_error(CDERR_STRUCTSIZE);
        return 0;
    }
    if (!request->Owner || !request->Find || !request->FindCapacity ||
        (replace && (!request->Replace || !request->ReplaceCapacity))) {
        set_dialog_error(CDERR_INITIALIZATION);
        return 0;
    }
    UINT notification = RegisterWindowMessageW(L"commdlg_FindReplace");
    SV_SEARCH_DIALOG *state = notification ?
        HeapAlloc(GetProcessHeap(), 8, sizeof(*state)) : 0;
    if (!state) {
        set_dialog_error(CDERR_INITIALIZATION);
        return 0;
    }
    state->Request = request;
    state->Notification = notification;

    WNDCLASSEXW window_class = {0};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = search_proc;
    window_class.lpszClassName = L"SvFindReplaceDialog";
    window_class.hbrBackground = (HBRUSH)(UINT_PTR)16;
    window_class.hCursor = LoadCursorW(0, IDC_ARROW);
    RegisterClassExW(&window_class);
    HWND window = CreateWindowExW(0, window_class.lpszClassName, replace ? L"Replace" : L"Find",
        WS_CAPTION | WS_SYSMENU | WS_POPUP, 220, 160, 448, replace ? 203 : 170,
        request->Owner, 0, 0, state);
    if (!window) {
        HeapFree(GetProcessHeap(), 0, state);
        set_dialog_error(CDERR_DIALOGFAILURE);
        return 0;
    }
    path_control(window, L"STATIC", L"Find what", 0, 12, 13, 80, 23, 0);
    state->Find = path_control(window, L"EDIT", request->Find,
        WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, 100, 12, 204, 24, 20);
    int options_y = 49;
    if (replace) {
        path_control(window, L"STATIC", L"Replace with", 0, 12, 47, 80, 23, 0);
        state->Replace = path_control(window, L"EDIT", request->Replace,
            WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, 100, 46, 204, 24, 21);
        path_control(window, L"BUTTON", L"Replace", WS_TABSTOP, 320, 46, 102, 26, 11);
        path_control(window, L"BUTTON", L"Replace All", WS_TABSTOP, 320, 78, 102, 26, 12);
        options_y = 83;
    }
    state->WholeWord = path_control(window, L"BUTTON", L"Whole word", WS_TABSTOP | BS_AUTOCHECKBOX,
        100, options_y, 166, 24, 22);
    state->MatchCase = path_control(window, L"BUTTON", L"Match case", WS_TABSTOP | BS_AUTOCHECKBOX,
        100, options_y + 26, 166, 24, 23);
    path_control(window, L"BUTTON", L"Find Next", WS_TABSTOP | BS_DEFPUSHBUTTON,
        320, 12, 102, 26, IDOK);
    path_control(window, L"BUTTON", L"Cancel", WS_TABSTOP, 320, options_y + 33, 102, 26, IDCANCEL);
    if (!state->Find || (replace && !state->Replace)) {
        DestroyWindow(window);
        set_dialog_error(CDERR_DIALOGFAILURE);
        return 0;
    }
    SendMessageW(state->WholeWord, BM_SETCHECK, (request->Flags & FIND_WHOLE_WORD) != 0, 0);
    SendMessageW(state->MatchCase, BM_SETCHECK, (request->Flags & FIND_MATCH_CASE) != 0, 0);
    request->Flags |= 1; /* FR_DOWN: search forward. */
    ShowWindow(window, SW_SHOW);
    SetFocus(state->Find);
    return window;
}

__declspec(dllexport) HWND FindTextW(SV_FINDREPLACEW *request)
{
    return search_dialog(request, FALSE);
}

__declspec(dllexport) HWND ReplaceTextW(SV_FINDREPLACEW *request)
{
    return search_dialog(request, TRUE);
}

__declspec(dllexport) BOOL ChooseFontW(void *dialog)
{
    (void)dialog;
    set_dialog_error(CDERR_INITIALIZATION);
    return FALSE;
}
