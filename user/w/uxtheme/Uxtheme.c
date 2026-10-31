#include "Uxtheme.h"

/* Theme handles own only a class identity. Painting is shared by every
 * process using the standard controls; applications do not carry a theme or
 * renderer copy. No user callbacks or syscalls occur while the list is locked. */
__declspec(dllimport) HANDLE GetProcessHeap(void);
__declspec(dllimport) void *HeapAlloc(HANDLE heap, DWORD flags, ULONG_PTR size);
__declspec(dllimport) BOOL HeapFree(HANDLE heap, DWORD flags, void *pointer);
typedef struct THEME { struct THEME *next; int kind; } THEME;
static THEME *themes;
static unsigned theme_lock;
static void lock(void) { while (__atomic_exchange_n(&theme_lock, 1, __ATOMIC_ACQUIRE)) {} }
static void unlock(void) { __atomic_store_n(&theme_lock, 0, __ATOMIC_RELEASE); }
static int equal(const WCHAR *a, const WCHAR *b)
{
    if (!a) return 0;
    for (unsigned i = 0; ; ++i) {
        WCHAR ch = a[i];
        if (ch >= 'a' && ch <= 'z') ch = (WCHAR)(ch - 'a' + 'A');
        if (ch != b[i]) return 0;
        if (!ch) return 1;
    }
}
static int kind(HTHEME handle)
{
    int result = 0;
    lock();
    for (THEME *theme = themes; theme; theme = theme->next)
        if (theme == handle) { result = theme->kind; break; }
    unlock();
    return result;
}
UXTHEME_API HTHEME OpenThemeData(void *hwnd, const WCHAR *name)
{
    (void)hwnd;
    int id = equal(name, L"BUTTON") ? 1 : equal(name, L"EDIT") ? 2 :
             equal(name, L"STATIC") ? 3 : 0;
    if (!id) return NULL;
    THEME *theme = HeapAlloc(GetProcessHeap(), 0, sizeof(*theme));
    if (!theme) return NULL;
    theme->kind = id;
    lock(); theme->next = themes; themes = theme; unlock();
    return theme;
}
UXTHEME_API HRESULT CloseThemeData(HTHEME handle)
{
    lock();
    THEME **link = &themes;
    while (*link && *link != handle) link = &(*link)->next;
    if (!*link) { unlock(); return E_HANDLE; }
    THEME *theme = *link;
    *link = theme->next;
    unlock();
    HeapFree(GetProcessHeap(), 0, theme);
    return S_OK;
}
UXTHEME_API BOOL IsThemeActive(void) { return TRUE; }
UXTHEME_API BOOL IsAppThemed(void) { return TRUE; }

static void fill(HDC dc, int x, int y, int w, int h, COLORREF color, const RECT *clip)
{
    if (clip) {
        int right = x + w < clip->right ? x + w : clip->right;
        int bottom = y + h < clip->bottom ? y + h : clip->bottom;
        if (x < clip->left) x = clip->left;
        if (y < clip->top) y = clip->top;
        w = right - x; h = bottom - y;
    }
    if (w <= 0 || h <= 0) return;
    HBRUSH brush = CreateSolidBrush(color);
    if (!brush) return;
    HGDIOBJ old = SelectObject(dc, brush);
    PatBlt(dc, x, y, w, h, PATCOPY);
    SelectObject(dc, old); DeleteObject(brush);
}
static void border(HDC dc, const RECT *r, COLORREF color, const RECT *clip)
{
    fill(dc, r->left, r->top, r->right-r->left, 1, color, clip);
    fill(dc, r->left, r->bottom-1, r->right-r->left, 1, color, clip);
    fill(dc, r->left, r->top, 1, r->bottom-r->top, color, clip);
    fill(dc, r->right-1, r->top, 1, r->bottom-r->top, color, clip);
}
UXTHEME_API HRESULT DrawThemeBackground(HTHEME theme, HDC dc, int part, int state,
                                        const RECT *rect, const RECT *clip)
{
    int id = kind(theme);
    if (!id) return E_HANDLE;
    if (!dc || !rect || rect->right < rect->left || rect->bottom < rect->top)
        return E_INVALIDARG;
    if (id == 1 && part == BP_RADIOBUTTON) {
        int checked=state>=5, interaction=(state-1)%4+1;
        COLORREF edge=interaction==4?RGB(188,188,188):
                      interaction==2||interaction==3?RGB(0,120,215):RGB(112,112,112);
        COLORREF interior=RGB(255,255,255);
        fill(dc,rect->left,rect->top,rect->right-rect->left,
             rect->bottom-rect->top,RGB(240,240,240),clip);
        HBRUSH brush=CreateSolidBrush(interior);HPEN pen=CreatePen(0,1,edge);
        HGDIOBJ old_brush=brush?SelectObject(dc,brush):NULL;
        HGDIOBJ old_pen=pen?SelectObject(dc,pen):NULL;
        Ellipse(dc,rect->left,rect->top,rect->right,rect->bottom);
        if(pen){SelectObject(dc,old_pen);DeleteObject(pen);}
        if(brush){SelectObject(dc,old_brush);DeleteObject(brush);}
        if(checked){
            COLORREF dot=interaction==4?RGB(131,131,131):RGB(0,120,215);
            int cx=(rect->left+rect->right)/2,cy=(rect->top+rect->bottom)/2;
            HBRUSH dot_brush=CreateSolidBrush(dot);HPEN dot_pen=CreatePen(0,1,dot);
            HGDIOBJ old_dot_brush=dot_brush?SelectObject(dc,dot_brush):NULL;
            HGDIOBJ old_dot_pen=dot_pen?SelectObject(dc,dot_pen):NULL;
            Ellipse(dc,cx-3,cy-3,cx+3,cy+3);
            if(dot_pen){SelectObject(dc,old_dot_pen);DeleteObject(dot_pen);}
            if(dot_brush){SelectObject(dc,old_dot_brush);DeleteObject(dot_brush);}
        }
        return S_OK;
    }
    COLORREF bg = RGB(255,255,255), edge = RGB(122,122,122);
    if (id == 1 && part == BP_PUSHBUTTON) {
        bg = RGB(225,225,225); edge = RGB(173,173,173);
        if (state == PBS_HOT) { bg = RGB(229,241,251); edge = RGB(0,120,215); }
        if (state == PBS_PRESSED) { bg = RGB(204,228,247); edge = RGB(0,84,153); }
        if (state == PBS_DISABLED) { bg = RGB(204,204,204); edge = RGB(191,191,191); }
        if (state == PBS_DEFAULTED) edge = RGB(0,120,215);
    } else if (id == 1 && part == BP_CHECKBOX) {
        int interaction = (state - 1) % 4 + 1;
        int checked = state >= 5;
        edge = interaction == 1 ? RGB(112,112,112) : RGB(0,120,215);
        if (interaction == 4) edge = RGB(188,188,188);
        if (checked) bg = interaction == 4 ? RGB(188,188,188) : RGB(0,120,215);
    } else if (id == 2 && (part == EP_EDITTEXT || part == EP_EDITBORDER_NOSCROLL)) {
        if (state == ETS_DISABLED) { bg = RGB(240,240,240); edge = RGB(204,204,204); }
        if (state == ETS_FOCUSED) edge = RGB(0,120,215);
        if (state == ETS_HOT) edge = RGB(23,23,23);
    } else if (id != 3) return E_NOTIMPL;
    fill(dc, rect->left, rect->top, rect->right-rect->left,
         rect->bottom-rect->top, bg, clip);
    if (id != 3) border(dc, rect, edge, clip);
    if (id == 1 && part == BP_CHECKBOX && state >= 5) {
        int x = rect->left + 3, y = rect->top + (rect->bottom-rect->top)/2;
        for (int i = 0; i < 3; ++i) fill(dc, x+i, y+i, 1, 2, RGB(255,255,255), clip);
        for (int i = 0; i < 6; ++i) fill(dc, x+3+i, y+2-i, 1, 2, RGB(255,255,255), clip);
    }
    return S_OK;
}
UXTHEME_API HRESULT GetThemeBackgroundContentRect(HTHEME theme, HDC dc,
                 int part, int state, const RECT *rect, RECT *content)
{
    (void)dc; (void)part; (void)state;
    if (!kind(theme)) return E_HANDLE;
    if (!rect || !content) return E_INVALIDARG;
    *content = *rect;
    content->left += 2; content->top += 2;
    content->right -= 2; content->bottom -= 2;
    if (content->right < content->left) content->right = content->left;
    if (content->bottom < content->top) content->bottom = content->top;
    return S_OK;
}
UXTHEME_API HRESULT GetThemeColor(HTHEME theme, int part, int state,
                                  int property, COLORREF *color)
{
    (void)part;
    int id = kind(theme);
    if (!id) return E_HANDLE;
    if (!color || property != TMT_TEXTCOLOR) return E_INVALIDARG;
    *color = (id == 1 && state == PBS_DISABLED) || (id == 2 && state == ETS_DISABLED)
             ? RGB(131,131,131) : RGB(0,0,0);
    return S_OK;
}
