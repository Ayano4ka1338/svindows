#include "Control.h"
__declspec(dllimport) HANDLE GetProcessHeap(void);
__declspec(dllimport) void *HeapAlloc(HANDLE heap, DWORD flags, ULONG_PTR size);
__declspec(dllimport) BOOL HeapFree(HANDLE heap, DWORD flags, void *pointer);
void *SvControlAlloc(unsigned long long bytes) { return HeapAlloc(GetProcessHeap(), 8, bytes); }
void SvControlFree(void *p) { if (p) HeapFree(GetProcessHeap(), 0, p); }
/* The list protects lookup/lifetime, never painting or application callbacks.
 * A synchronous WM_COMMAND can destroy the sending control recursively. */
static SV_CONTROL *controls;
static unsigned list_lock;
static void lock(void) { while (__atomic_exchange_n(&list_lock, 1, __ATOMIC_ACQUIRE)) {} }
static void unlock(void) { __atomic_store_n(&list_lock, 0, __ATOMIC_RELEASE); }
static SV_CONTROL *acquire(HWND hwnd)
{
    lock();
    SV_CONTROL *c = controls;
    while (c && c->hwnd != hwnd) c = c->next;
    if (c) ++c->refs;
    unlock(); return c;
}
static void release(SV_CONTROL *c)
{
    lock(); int dispose = !--c->refs && c->dead; unlock();
    if (dispose) { CloseThemeData(c->theme); SvEditDispose(&c->model); SvControlFree(c); }
}
void SvControlNotify(SV_CONTROL *c, UINT code)
{
    if (!c->dead && c->parent)
        SendMessageW(c->parent, WM_COMMAND, (WPARAM)MAKELONG(c->id, code), (LPARAM)c->hwnd);
}
void SvControlSelectAutoRadio(SV_CONTROL *c)
{
    lock();
    for (SV_CONTROL *s=controls;s;s=s->next) {
        if (s!=c && !s->dead && s->parent==c->parent && s->kind==SV_BUTTON &&
            (s->style&BS_TYPEMASK)==BS_AUTORADIOBUTTON && s->checked) {
            s->checked=0;
            /* InvalidateRect only queues damage; it does not call application code. */
            InvalidateRect(s->hwnd,NULL,FALSE);
        }
    }
    c->checked=1;
    unlock();
}
void SvControlInvalidate(SV_CONTROL *c)
{
    if (!c->dead) InvalidateRect(c->hwnd, NULL, FALSE);
}
void SvControlFill(HDC dc, int x, int y, int w, int h, COLORREF color)
{
    if (w <= 0 || h <= 0) return;
    HBRUSH brush = CreateSolidBrush(color);
    if (!brush) return;
    HGDIOBJ old = SelectObject(dc, brush);
    PatBlt(dc, x, y, w, h, PATCOPY);
    SelectObject(dc, old); DeleteObject(brush);
}
static int class_kind(const WCHAR *name)
{
    const WCHAR *names[] = {L"BUTTON", L"EDIT", L"STATIC"};
    if (IS_INTRESOURCE(name)) return 0;
    for (int k = 0; k < 3; ++k) {
        unsigned i = 0;
        for (;; ++i) {
            WCHAR ch = name[i];
            if (ch >= 'a' && ch <= 'z') ch -= 'a' - 'A';
            if (ch != names[k][i]) break;
            if (!ch) return k + 1;
        }
    }
    return 0;
}
static LRESULT ControlWndProc(int kind, HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_NCCREATE) {
        const CREATESTRUCTW *cs = (const CREATESTRUCTW *)lp;
        if (!cs) return FALSE;
        if (!kind) return FALSE;
        /* Refuse unimplemented variants rather than drawing them incorrectly
         * (in particular, never expose password text as a plain EDIT). */
        UINT type = (UINT)cs->style & 15;
        if (kind == SV_BUTTON && type != BS_PUSHBUTTON && type != BS_DEFPUSHBUTTON &&
            type != BS_CHECKBOX && type != BS_AUTOCHECKBOX && type != BS_RADIOBUTTON &&
            type != BS_GROUPBOX && type != BS_AUTORADIOBUTTON) return FALSE;
        if (kind == SV_EDIT && (cs->style & (ES_PASSWORD|ES_CENTER|ES_RIGHT))) return FALSE;
        if (kind == SV_STATIC && type != SS_LEFT && type != SS_CENTER &&
            type != SS_RIGHT && type != SS_SIMPLE && type != SS_LEFTNOWORDWRAP) return FALSE;
        SV_CONTROL *c = SvControlAlloc(sizeof(*c));
        if (!c) return FALSE;
        c->hwnd = hwnd; c->parent = cs->hwndParent;
        c->id = (UINT)(UINT_PTR)cs->hMenu; c->kind = (UINT)kind;
        c->style = (DWORD)cs->style; c->ex_style = cs->dwExStyle;
        c->font = GetStockObject(DEFAULT_GUI_FONT);
        c->model.limit = kind == SV_EDIT && !(c->style & ES_MULTILINE) ? 32767 : 0x7FFFFFFE;
        c->theme = OpenThemeData(hwnd, kind == SV_BUTTON ? L"BUTTON" : kind == SV_EDIT ? L"EDIT" : L"STATIC");
        if ((!c->theme && type != BS_GROUPBOX) || !SvEditSetText(&c->model, cs->lpszName)) {
            if (c->theme) CloseThemeData(c->theme);
            SvEditDispose(&c->model); SvControlFree(c); return FALSE;
        }
        lock(); c->next = controls; controls = c; unlock();
        return TRUE;
    }
    SV_CONTROL *c = acquire(hwnd);
    if (!c) return DefWindowProcW(hwnd, msg, wp, lp);
    LRESULT result = 0;
    switch (msg) {
    case WM_NCDESTROY: {
        lock(); SV_CONTROL **link = &controls;
        while (*link && *link != c) link = &(*link)->next;
        if (*link) *link = c->next;
        c->dead = 1; unlock(); break;
    }
    case WM_GETTEXTLENGTH: result = c->model.length; break;
    case WM_GETTEXT:
        if (wp && lp) {
            int n = c->model.length;
            if ((UINT_PTR)n >= wp) n = (int)(wp - 1);
            WCHAR *out = (WCHAR *)lp;
            for (int i = 0; i < n; ++i) out[i] = c->model.text[i];
            out[n] = 0; result = n;
        }
        break;
    case WM_SETTEXT:
        result = SvEditSetText(&c->model, (const WCHAR *)lp);
        if (result) {
            c->first_line = c->scroll_x = 0; SvControlInvalidate(c);
            if (c->kind == SV_EDIT && !(c->style & ES_MULTILINE)) SvControlNotify(c, EN_CHANGE);
        }
        break;
    case WM_SETFONT:
        c->font = wp ? (HFONT)wp : GetStockObject(DEFAULT_GUI_FONT);
        if (lp) SvControlInvalidate(c);
        break;
    case WM_GETFONT: result = (LRESULT)c->font; break;
    case WM_ENABLE:
        if (wp) c->style &= ~WS_DISABLED; else c->style |= WS_DISABLED;
        SvControlInvalidate(c); break;
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
        c->focused = msg == WM_SETFOCUS;
        if (!c->focused) c->pressed = 0;
        SvControlInvalidate(c);
        if (c->kind == SV_EDIT) SvControlNotify(c, c->focused ? EN_SETFOCUS : EN_KILLFOCUS);
        break;
    case WM_ERASEBKGND: result = 1; break;
    case WM_CREATE: break;
    default:
        if (c->kind == SV_BUTTON) result = SvButtonMessage(c, msg, wp, lp);
        else if (c->kind == SV_EDIT) result = SvEditMessage(c, msg, wp, lp);
        else result = SvStaticMessage(c, msg, wp, lp);
        break;
    }
    release(c); return result;
}
static LRESULT ButtonWndProc(HWND h, UINT m, WPARAM w, LPARAM l) { return ControlWndProc(SV_BUTTON, h, m, w, l); }
static LRESULT EditWndProc(HWND h, UINT m, WPARAM w, LPARAM l) { return ControlWndProc(SV_EDIT, h, m, w, l); }
static LRESULT StaticWndProc(HWND h, UINT m, WPARAM w, LPARAM l) { return ControlWndProc(SV_STATIC, h, m, w, l); }
int SvRegisterControl(const WCHAR *name)
{
    int kind = class_kind(name);
    if (!kind) return 1; /* Ordinary application class. */
    /* Registration itself is process scoped and idempotent in win32k. */
    WNDCLASSEXW cls = {0};
    cls.cbSize = sizeof(cls); cls.lpfnWndProc = kind == SV_BUTTON ? ButtonWndProc : kind == SV_EDIT ? EditWndProc : StaticWndProc;
    cls.hCursor = LoadCursorW(NULL, IDC_ARROW);
    cls.lpszClassName = name;
    return RegisterClassExW(&cls) != 0;
}
