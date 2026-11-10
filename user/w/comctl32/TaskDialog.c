#include "Internal.h"

__declspec(dllimport) LONG_PTR GetWindowLongPtrW(HWND, int);
__declspec(dllimport) LONG_PTR SetWindowLongPtrW(HWND, int, LONG_PTR);
__declspec(dllimport) BOOL EnableWindow(HWND, BOOL);
__declspec(dllimport) BOOL IsWindowEnabled(HWND);

#pragma pack(push, 1)
typedef struct {
    int Id;
    const WCHAR *Text;
} SV_TASK_BUTTON;

typedef struct {
    UINT Size;
    HWND Owner;
    HINSTANCE Instance;
    UINT Flags, CommonButtons;
    const WCHAR *Title, *Icon, *Instruction, *Content;
    UINT ButtonCount;
    const SV_TASK_BUTTON *Buttons;
    int DefaultButton;
    UINT RadioCount;
    const SV_TASK_BUTTON *Radio;
    int DefaultRadio;
    const WCHAR *Verification, *Expanded, *ExpandedLabel, *CollapsedLabel;
    const WCHAR *FooterIcon, *Footer;
    LONG (*Callback)(HWND, UINT, WPARAM, LPARAM, LONG_PTR);
    LONG_PTR Data;
    UINT Width;
} SV_TASK_CONFIG;
#pragma pack(pop)
_Static_assert(sizeof(SV_TASK_CONFIG) == 160, "TASKDIALOGCONFIG x64");

typedef struct {
    const SV_TASK_CONFIG *Config;
    HWND Verification;
    int Choice;
    BOOL Selected, CanCancel;
} SV_TASK_STATE;

static void task_select(HWND window, SV_TASK_STATE *state, int button)
{
    const SV_TASK_CONFIG *config = state->Config;
    if (config->Callback &&
        config->Callback(window, 2 /* TDN_BUTTON_CLICKED */, button, 0,
                         config->Data) == 1)
        return;
    state->Choice = button;
    state->Selected = TRUE;
}

static LRESULT task_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    if (message == WM_NCCREATE) {
        const CREATESTRUCTW *create = (const CREATESTRUCTW *)lparam;
        SetWindowLongPtrW(window, -21 /* GWLP_USERDATA */, (LONG_PTR)create->lpCreateParams);
        return TRUE;
    }
    SV_TASK_STATE *state = (SV_TASK_STATE *)GetWindowLongPtrW(window, -21);
    if (state && message == WM_CLOSE) {
        if (state->CanCancel) task_select(window, state, IDCANCEL);
        return 0;
    }
    if (state && message == WM_COMMAND && lparam) {
        UINT id = LOWORD(wparam);
        if ((HWND)lparam == state->Verification) {
            const SV_TASK_CONFIG *config = state->Config;
            if (config->Callback)
                config->Callback(window, 8 /* TDN_VERIFICATION_CLICKED */,
                    SendMessageW(state->Verification, BM_GETCHECK, 0, 0), 0, config->Data);
        } else {
            task_select(window, state, id);
        }
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

static const WCHAR *task_text(const SV_TASK_CONFIG *config,
    const WCHAR *text, WCHAR *buffer, UINT capacity)
{
    if (!text) return L"";
    if (!IS_INTRESOURCE(text)) return text;
    return LoadStringW(config->Instance, (UINT)(UINT_PTR)text, buffer, capacity)
        ? buffer : L"";
}

static HWND task_control(HWND parent, const WCHAR *name, const WCHAR *text,
    DWORD style, int x, int y, int width, int height, int id)
{
    return CreateWindowExW(0, name, text, WS_CHILD | WS_VISIBLE | style,
        x, y, width, height, parent, (HMENU)(UINT_PTR)id, 0, 0);
}

COMCTL32_API LONG TaskDialogIndirect(const SV_TASK_CONFIG *config, int *button,
    int *radio, BOOL *verification)
{
    if (button) *button = 0;
    if (radio) *radio = 0;
    if (verification) *verification = FALSE;
    if (!config || config->Size != sizeof(*config) || config->ButtonCount > 8 ||
        (config->ButtonCount && !config->Buttons))
        return (LONG)0x80070057u;
    if (config->RadioCount || config->Expanded)
        return (LONG)0x80004001u; /* Expanded content and radio groups require another layout. */

    static const SV_TASK_BUTTON common[] = {
        {IDOK, L"OK"}, {IDYES, L"Yes"}, {IDNO, L"No"}, {IDCANCEL, L"Cancel"},
        {IDRETRY, L"Retry"}, {8, L"Close"}
    };
    SV_TASK_BUTTON buttons[14];
    UINT count = 0;
    for (UINT i = 0; i < config->ButtonCount; i++) {
        if (config->Buttons[i].Id < 0 || config->Buttons[i].Id > 0xffff)
            return (LONG)0x80070057u;
        buttons[count++] = config->Buttons[i];
    }
    for (UINT i = 0; i < sizeof(common) / sizeof(common[0]); i++)
        if (config->CommonButtons & (1u << i)) buttons[count++] = common[i];
    if (!count) buttons[count++] = common[0];

    SV_TASK_STATE state = {config, 0, 0, FALSE, (config->Flags & 8) != 0};
    for (UINT i = 0; i < count; i++)
        if (buttons[i].Id == IDCANCEL) state.CanCancel = TRUE;
    const int width = count > 5 ? 672 : 540;
    const int columns = width / 112;
    const int height = 174 + ((count + columns - 1) / columns) * 33 +
                       (config->Verification ? 26 : 0) + (config->Footer ? 28 : 0);
    int default_button = config->DefaultButton ? config->DefaultButton : buttons[0].Id;

    WNDCLASSEXW cls = {0};
    cls.cbSize = sizeof(cls);
    cls.lpfnWndProc = task_proc;
    cls.lpszClassName = L"SvTaskDialog";
    cls.hbrBackground = (HBRUSH)(UINT_PTR)16;
    cls.hCursor = LoadCursorW(0, IDC_ARROW);
    RegisterClassExW(&cls);
    WCHAR text[1024];
    const WCHAR *title = config->Title ?
        task_text(config, config->Title, text, 1024) : L"Svindows";
    HWND window = CreateWindowExW(0, cls.lpszClassName, title,
        WS_POPUP | WS_CAPTION | (state.CanCancel ? WS_SYSMENU : 0),
        (GetSystemMetrics(SM_CXSCREEN) - width) / 2,
        (GetSystemMetrics(SM_CYSCREEN) - height) / 2,
        width, height, config->Owner, 0, 0, &state);
    if (!window) return (LONG)0x80004005u;

    int y = 12;
    if (config->Instruction) {
        task_control(window, L"STATIC", task_text(config, config->Instruction, text, 1024),
            0, 14, y, width - 32, 24, 0);
        y += 28;
    }
    task_control(window, L"STATIC", task_text(config, config->Content, text, 1024),
        0, 14, y, width - 32, 69, 0);
    y += 74;
    if (config->Footer) {
        task_control(window, L"STATIC", task_text(config, config->Footer, text, 1024),
            0, 14, y, width - 32, 22, 0);
        y += 28;
    }
    if (config->Verification && verification) {
        state.Verification = task_control(window, L"BUTTON",
            task_text(config, config->Verification, text, 1024),
            WS_TABSTOP | BS_AUTOCHECKBOX, 14, y, width - 32, 22, 1);
        SendMessageW(state.Verification, BM_SETCHECK, (config->Flags & 0x100) != 0, 0);
        y += 26;
    }
    HWND first = 0, focused = 0;
    for (UINT i = 0; i < count; i++) {
        HWND control = task_control(window, L"BUTTON",
            task_text(config, buttons[i].Text, text, 1024),
            WS_TABSTOP | (buttons[i].Id == default_button ? BS_DEFPUSHBUTTON : 0),
            12 + (i % columns) * 108, y + (i / columns) * 33, 102, 28, buttons[i].Id);
        if (!control) {
            DestroyWindow(window);
            return (LONG)0x80004005u;
        }
        if (!first) first = control;
        if (buttons[i].Id == default_button) focused = control;
    }

    BOOL owner_enabled = config->Owner && IsWindowEnabled(config->Owner);
    if (owner_enabled) EnableWindow(config->Owner, FALSE);
    ShowWindow(window, SW_SHOW);
    SetFocus(focused ? focused : first);
    if (config->Callback) config->Callback(window, 0 /* TDN_CREATED */, 0, 0, config->Data);

    MSG message;
    int status = -1;
    while (!state.Selected && (status = GetMessageW(&message, 0, 0, 0)) > 0) {
        if (message.message == WM_KEYDOWN && message.wParam == VK_ESCAPE &&
            state.CanCancel && (message.hwnd == window || GetParent(message.hwnd) == window)) {
            task_select(window, &state, IDCANCEL);
            continue;
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    if (!status) PostQuitMessage((int)message.wParam);
    if (state.Selected) {
        if (button) *button = state.Choice;
        if (verification && state.Verification)
            *verification = SendMessageW(state.Verification, BM_GETCHECK, 0, 0) != 0;
    }
    DestroyWindow(window);
    if (config->Callback) config->Callback(window, 5 /* TDN_DESTROYED */, 0, 0, config->Data);
    if (owner_enabled) {
        EnableWindow(config->Owner, TRUE);
        SetActiveWindow(config->Owner);
    }
    return state.Selected ? 0 : (LONG)0x80004005u;
}
