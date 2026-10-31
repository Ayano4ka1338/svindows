/* UTF-16 editing, allocation rollback and synchronous parent destruction. */
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#define __declspec(x)
#define SV_USER32_BUILD
#include "../../user/w/user32/controls/Control.h"
static int budget = -1, allocations, frees, destroy_on_change;
static WNDPROC procedures[3];
HANDLE GetProcessHeap(void) { return (HANDLE)1; }
void *HeapAlloc(HANDLE heap, DWORD flags, ULONG_PTR size)
{
    (void)heap; (void)flags;
    if (!budget) return NULL;
    if (budget > 0) --budget;
    void *p = calloc(1, size); if (p) ++allocations; return p;
}
BOOL HeapFree(HANDLE h, DWORD f, void *p) { (void)h; (void)f; if (p) { ++frees; free(p); } return TRUE; }
HGDIOBJ GetStockObject(int id) { return (HGDIOBJ)(UINT_PTR)id; }
HTHEME OpenThemeData(void *h, const WCHAR *n) { (void)h; (void)n; return HeapAlloc(0, 0, 8); }
HRESULT CloseThemeData(HTHEME t) { HeapFree(0, 0, t); return S_OK; }
HCURSOR LoadCursorW(HINSTANCE h, const WCHAR *n) { (void)h; (void)n; return (HCURSOR)1; }
ATOM RegisterClassExW(const WNDCLASSEXW *c)
{
    int k = c->lpszClassName[0] == 'B' ? 0 : c->lpszClassName[0] == 'E' ? 1 : 2;
    procedures[k] = c->lpfnWndProc; return (ATOM)(k + 1);
}
LRESULT DefWindowProcW(HWND h, UINT m, WPARAM w, LPARAM l) { (void)h; (void)m; (void)w; (void)l; return 0; }
BOOL InvalidateRect(HWND h, LPCRECT r, BOOL e) { (void)h; (void)r; (void)e; return TRUE; }
LRESULT SendMessageW(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    assert(h == (HWND)1 && msg == WM_COMMAND && LOWORD(wp) == 42);
    assert(HIWORD(wp) == EN_CHANGE);
    if (destroy_on_change) procedures[1]((HWND)lp, WM_NCDESTROY, 0, 0);
    return 0;
}
void SvEditEnsureVisible(SV_CONTROL *c) { (void)c; }
LRESULT SvButtonMessage(SV_CONTROL *c, UINT m, WPARAM w, LPARAM l) { (void)c; (void)m; (void)w; (void)l; return 0; }
LRESULT SvEditMessage(SV_CONTROL *c, UINT m, WPARAM w, LPARAM l) { (void)c; (void)m; (void)w; (void)l; return 0; }
LRESULT SvStaticMessage(SV_CONTROL *c, UINT m, WPARAM w, LPARAM l) { (void)c; (void)m; (void)w; (void)l; return 0; }
#include "../../user/w/user32/controls/EditModel.c"
#include "../../user/w/user32/controls/Controls.c"
static void equals(EDIT_MODEL *m, const WCHAR *s)
{
    int n = 0; while (s[n]) { assert(m->text[n] == s[n]); ++n; }
    assert(m->length == n && !m->text[n]);
}
int main(void)
{
    EDIT_MODEL m = {.limit = 0x7FFFFFFE};
    assert(SvEditSetText(&m, L"one\r\ntwo\tend"));
    assert(SvEditLineStart(&m, 1) == 5 && SvEditLineEnd(&m, 0) == 3);
    assert(SvEditLineFromOffset(&m, 9) == 1 && SvEditLineStart(&m, 2) == -1);
    SvEditSelect(&m, 5, 5); assert(SvEditDelete(&m, 1)); equals(&m, L"onetwo\tend");
    SvEditSelect(&m, 0, 3); assert(SvEditReplace(&m, m.text+3, 3)); equals(&m, L"twotwo\tend");
    SvEditSelect(&m, 0, -1); m.limit = 2;
    assert(!SvEditReplace(&m, L"too long", -1)); equals(&m, L"twotwo\tend");
    assert(m.anchor == 0 && m.caret == m.length);
    m.limit = 0x7FFFFFFE;
    WCHAR large[160]; for (int i = 0; i < 159; ++i) large[i] = 'x'; large[159] = 0;
    budget = 1; /* Temporary insertion succeeds; buffer growth fails. */
    assert(!SvEditReplace(&m, large, -1)); equals(&m, L"twotwo\tend");
    assert(m.anchor == 0 && m.caret == m.length);
    budget = -1;
    const WCHAR pair[] = {'a',0xD83D,0xDE00,'b',0};
    assert(SvEditSetText(&m, pair)); SvEditSelect(&m, 3, 3);
    assert(SvEditDelete(&m, 1)); equals(&m, L"ab");
    assert(SvEditSetText(&m, pair)); SvEditSelect(&m, 1, 1);
    assert(SvEditDelete(&m, 0)); equals(&m, L"ab");
    SvEditDispose(&m); assert(allocations == frees);
    puts("PASS UTF-16 selection, self replacement, CRLF/surrogate deletion and allocation rollback");

    assert(SvRegisterControl(L"EDIT"));
    CREATESTRUCTW cs = {0}; cs.hwndParent = (HWND)1; cs.hMenu = (HMENU)42;
    cs.lpszName = L"first";
    WNDPROC proc = procedures[1];
    assert(proc((HWND)2, WM_NCCREATE, 0, (LPARAM)&cs));
    cs.lpszName = L"second";
    assert(proc((HWND)3, WM_NCCREATE, 0, (LPARAM)&cs));
    WCHAR text[16]; assert(proc((HWND)2, WM_GETTEXT, 16, (LPARAM)text) == 5);
    assert(text[0] == 'f' && proc((HWND)3, WM_GETTEXTLENGTH, 0, 0) == 6);
    destroy_on_change = 1;
    assert(proc((HWND)2, WM_SETTEXT, 0, (LPARAM)L"parent destroys sender") == TRUE);
    assert(!acquire((HWND)2));
    proc((HWND)3, WM_NCDESTROY, 0, 0);
    assert(!controls && allocations == frees);
    budget = 0; assert(!proc((HWND)4, WM_NCCREATE, 0, (LPARAM)&cs));
    budget = 1; assert(!proc((HWND)4, WM_NCCREATE, 0, (LPARAM)&cs));
    assert(!controls && allocations == frees);
    puts("PASS independent control instances, WM_GETTEXT and destruction during synchronous EN_CHANGE");
    return 0;
}
