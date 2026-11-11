#include "Control.h"
#define MARGIN 4

__declspec(dllimport) void *LocalAlloc(UINT flags, ULONG_PTR bytes);
__declspec(dllimport) void *LocalReAlloc(void *handle, ULONG_PTR bytes, UINT flags);
__declspec(dllimport) void *LocalLock(void *handle);
__declspec(dllimport) BOOL LocalUnlock(void *handle);
__declspec(dllimport) ULONG_PTR LocalSize(void *handle);
__declspec(dllimport) void *LocalFree(void *handle);

static int grow_local_buffer(EDIT_MODEL *model, int capacity)
{
    ULONG_PTR size = (ULONG_PTR)capacity * sizeof(WCHAR);
    void *handle = model->storage
        ? LocalReAlloc(model->storage, size, 0x40 /* LMEM_ZEROINIT */)
        : LocalAlloc(0x42 /* LMEM_MOVEABLE | LMEM_ZEROINIT */, size);
    if (!handle) return 0;
    model->storage = handle;
    model->text = LocalLock(handle);
    if (!model->text) return 0;
    LocalUnlock(handle);
    model->capacity = capacity;
    return 1;
}

static void free_local_buffer(EDIT_MODEL *model)
{
    LocalFree(model->storage);
    model->storage = 0;
}

static void *edit_get_handle(EDIT_MODEL *model)
{
    if (model->storage) return model->storage;
    WCHAR *previous = model->text;
    if (!grow_local_buffer(model, model->capacity)) {
        LocalFree(model->storage);
        model->storage = 0;
        model->text = previous;
        return 0;
    }
    for (int i = 0; i <= model->length; i++) model->text[i] = previous[i];
    SvControlFree(previous);
    model->grow_storage = grow_local_buffer;
    model->free_storage = free_local_buffer;
    return model->storage;
}

static void edit_set_handle(SV_CONTROL *control, void *handle)
{
    ULONG_PTR capacity = LocalSize(handle) / sizeof(WCHAR);
    if (!capacity || capacity > 0x7fffffffu) return;
    WCHAR *text = LocalLock(handle);
    if (!text) return;
    ULONG_PTR length = 0;
    while (length < capacity && text[length]) length++;
    LocalUnlock(handle);
    if (length == capacity) return;

    /* EM_SETHANDLE transfers the supplied block. The caller frees the
     * previous block if it supplies a different handle. */
    EDIT_MODEL *model = &control->model;
    if (!model->storage && model->text != text) SvControlFree(model->text);
    model->grow_storage = grow_local_buffer;
    model->free_storage = free_local_buffer;
    model->storage = handle;
    model->text = text;
    model->capacity = (int)capacity;
    model->length = (int)length;
    model->caret = model->anchor = 0;
    model->modified = 0;
    control->first_line = control->scroll_x = 0;
    SvControlInvalidate(control);
}

static int line_height(HDC dc)
{
    TEXTMETRICW tm;
    return GetTextMetricsW(dc, &tm) && tm.tmHeight > 0 ? tm.tmHeight : 15;
}
/* Tab stops use the selected font's measured space advance. Text remains
 * UTF-16 with literal tabs and CRLF; layout never rewrites the document. */
static int tab_width(HDC dc)
{
    SIZE s = {0}; GetTextExtentPoint32W(dc, L" ", 1, &s);
    return s.cx > 0 ? s.cx * 8 : 32;
}
static int advance(HDC dc, const WCHAR *text, int start, int end, int x)
{
    int tab = tab_width(dc);
    for (int at = start; at < end;) {
        if (text[at] == '\t') { x = (x / tab + 1) * tab; ++at; continue; }
        int next = at;
        while (next < end && text[next] != '\t') ++next;
        SIZE s = {0}; GetTextExtentPoint32W(dc, text + at, next - at, &s);
        x += s.cx; at = next;
    }
    return x;
}
static HDC measure(SV_CONTROL *c)
{
    HDC dc = GetDC(c->hwnd);
    if (dc) SelectObject(dc, c->font);
    return dc;
}

static int line_origin(SV_CONTROL *c, HDC dc, int start, int end, int width)
{
    DWORD alignment = c->style & (ES_CENTER | ES_RIGHT);
    if (!alignment) return MARGIN;
    int spare = width - 2 * MARGIN - advance(dc, c->model.text, start, end, 0);
    if (spare <= 0) return MARGIN;
    return MARGIN + (alignment & ES_RIGHT ? spare : spare / 2);
}

static int top_margin(SV_CONTROL *c, int height, int line_height)
{
    if (c->style & ES_MULTILINE) return MARGIN;
    return height > line_height ? (height - line_height) / 2 : 0;
}

void SvEditEnsureVisible(SV_CONTROL *c)
{
    RECT r; GetClientRect(c->hwnd, &r);
    HDC dc = measure(c); if (!dc) return;
    int lh = line_height(dc), line = SvEditLineFromOffset(&c->model, -1);
    int rows = (r.bottom - MARGIN * 2) / lh;
    if (rows < 1) rows = 1;
    if (line < c->first_line) c->first_line = line;
    if (line >= c->first_line + rows) c->first_line = line - rows + 1;
    int start = SvEditLineStart(&c->model, line);
    int x = advance(dc, c->model.text, start, c->model.caret, 0);
    int width = r.right - MARGIN * 2;
    if (width < 1) width = 1;
    if (x < c->scroll_x) c->scroll_x = x;
    if (x >= c->scroll_x + width) c->scroll_x = x - width + 1;
    ReleaseDC(c->hwnd, dc);
}
static int offset_at(SV_CONTROL *c, HDC dc, int line, int x, int width)
{
    int start = SvEditLineStart(&c->model, line);
    if (start < 0) return c->model.length;
    int end = SvEditLineEnd(&c->model, start), at = start, px = 0;
    x -= line_origin(c, dc, start, end, width) - c->scroll_x;
    while (at < end) {
        int next = at + 1;
        if (c->model.text[at] >= 0xD800 && c->model.text[at] <= 0xDBFF && next < end &&
            c->model.text[next] >= 0xDC00 && c->model.text[next] <= 0xDFFF) ++next;
        int nx = advance(dc, c->model.text, at, next, px);
        if (x < px + (nx - px)/2) break;
        px = nx; at = next;
    }
    return at;
}
static void paint(SV_CONTROL *c)
{
    PAINTSTRUCT ps; HDC dc = BeginPaint(c->hwnd, &ps); if (!dc) return;
    RECT r; GetClientRect(c->hwnd, &r);
    int state = c->style & WS_DISABLED ? ETS_DISABLED : c->focused ? ETS_FOCUSED : ETS_NORMAL;
    if ((c->style & WS_BORDER) || (c->ex_style & WS_EX_CLIENTEDGE))
        DrawThemeBackground(c->theme, dc, EP_EDITTEXT, state, &r, NULL);
    else SvControlFill(dc, 0, 0, r.right, r.bottom, c->style & WS_DISABLED ? RGB(240,240,240) : RGB(255,255,255));
    SelectObject(dc, c->font); SetBkMode(dc, TRANSPARENT);
    int lh = line_height(dc);
    int top = top_margin(c, r.bottom, lh);
    RECT clip = {MARGIN, top, r.right-MARGIN, r.bottom-top};
    int lo = c->model.anchor < c->model.caret ? c->model.anchor : c->model.caret;
    int hi = c->model.anchor > c->model.caret ? c->model.anchor : c->model.caret;
    for (int line = c->first_line, y = top; y < clip.bottom; ++line, y += lh) {
        int start = SvEditLineStart(&c->model, line);
        if (start < 0) break;
        int end = SvEditLineEnd(&c->model, start), px = 0;
        int origin = line_origin(c, dc, start, end, r.right) - c->scroll_x;
        for (int at = start; at < end;) {
            int next = at + 1, selected = at >= lo && at < hi;
            if (c->model.text[at] != '\t') {
                while (next < end && c->model.text[next] != '\t' &&
                       (next >= lo && next < hi) == selected) ++next;
            }
            int nx = advance(dc, c->model.text, at, next, px);
            int sx = origin + px;
            int left = sx > clip.left ? sx : clip.left;
            int right = origin + nx < clip.right ? origin + nx : clip.right;
            if (selected) SvControlFill(dc, left, y, right-left,
                         y+lh < clip.bottom ? lh : clip.bottom-y,
                         c->focused ? RGB(0,120,215) : RGB(229,229,229));
            SetTextColor(dc, selected && c->focused ? RGB(255,255,255) :
                         c->style & WS_DISABLED ? RGB(131,131,131) : RGB(0,0,0));
            if (c->model.text[at] != '\t')
                ExtTextOutW(dc, sx, y, ETO_CLIPPED, &clip, c->model.text+at, (UINT)(next-at), NULL);
            px = nx; at = next;
        }
        if (c->focused && c->model.caret >= start && c->model.caret <= end) {
            int x = origin + advance(dc, c->model.text, start, c->model.caret, 0);
            if (x >= clip.left && x < clip.right)
                SvControlFill(dc, x, y, 1, y+lh < clip.bottom ? lh : clip.bottom-y, RGB(0,0,0));
        }
    }
    EndPaint(c->hwnd, &ps);
}
static void changed(SV_CONTROL *c, int success)
{
    if (!success) return;
    SvEditEnsureVisible(c); SvControlInvalidate(c); SvControlNotify(c, EN_CHANGE);
}
static void move_caret(SV_CONTROL *c, int offset, int extend)
{
    SvEditSelect(&c->model, extend ? c->model.anchor : offset, offset);
    SvEditEnsureVisible(c); SvControlInvalidate(c);
}
static int horizontal(const EDIT_MODEL *m, int direction)
{
    int p = m->caret;
    if (direction < 0 && p) {
        --p;
        if (p && ((m->text[p] == '\n' && m->text[p-1] == '\r') ||
            (m->text[p] >= 0xDC00 && m->text[p] <= 0xDFFF && m->text[p-1] >= 0xD800 && m->text[p-1] <= 0xDBFF))) --p;
    } else if (direction > 0 && p < m->length) {
        ++p;
        if (p < m->length && ((m->text[p-1] == '\r' && m->text[p] == '\n') ||
            (m->text[p-1] >= 0xD800 && m->text[p-1] <= 0xDBFF && m->text[p] >= 0xDC00 && m->text[p] <= 0xDFFF))) ++p;
    }
    return p;
}
LRESULT SvEditMessage(SV_CONTROL *c, UINT msg, WPARAM wp, LPARAM lp)
{
    EDIT_MODEL *m = &c->model;
    int readonly = !!(c->style & (ES_READONLY|WS_DISABLED));
    switch (msg) {
    case WM_PAINT: paint(c); return 0;
    case WM_SIZE: SvEditEnsureVisible(c); SvControlInvalidate(c); return 0;
    case EM_GETSEL: {
        unsigned lo = (unsigned)(m->anchor < m->caret ? m->anchor : m->caret);
        unsigned hi = (unsigned)(m->anchor > m->caret ? m->anchor : m->caret);
        if (wp) *(DWORD *)wp = lo;
        if (lp) *(DWORD *)lp = hi;
        return lo > 65535 || hi > 65535 ? -1 : MAKELONG(lo, hi);
    }
    case EM_SETSEL: SvEditSelect(m, (int)wp, (int)lp); SvEditEnsureVisible(c); SvControlInvalidate(c); return 0;
    case EM_GETHANDLE:
        return (c->style & ES_MULTILINE) ? (LRESULT)edit_get_handle(m) : 0;
    case EM_SETHANDLE:
        if (c->style & ES_MULTILINE) edit_set_handle(c, (void *)wp);
        return 0;
    case EM_GETMODIFY: return m->modified;
    case EM_SETMODIFY: m->modified = !!wp; return 0;
    case EM_GETLINECOUNT: return SvEditLineFromOffset(m, m->length) + 1;
    case EM_LINEFROMCHAR: return SvEditLineFromOffset(m, (int)wp);
    case EM_LINEINDEX: return SvEditLineStart(m, (int)wp < 0 ? SvEditLineFromOffset(m, -1) : (int)wp);
    case EM_LINELENGTH: {
        int p = (int)wp < 0 ? m->caret : (int)wp;
        int start = SvEditLineStart(m, SvEditLineFromOffset(m, p));
        return SvEditLineEnd(m, start) - start;
    }
    case EM_GETFIRSTVISIBLELINE: return c->first_line;
    case EM_GETLIMITTEXT: return m->limit;
    case EM_LIMITTEXT: m->limit = wp ? (wp < 0x7FFFFFFE ? (unsigned)wp : 0x7FFFFFFE) : 0x7FFFFFFE; return 0;
    case EM_SETREADONLY:
        if (wp) c->style |= ES_READONLY; else c->style &= ~ES_READONLY;
        SvControlInvalidate(c); return TRUE;
    case EM_REPLACESEL: changed(c, SvEditReplace(m, (const WCHAR *)lp, -1)); return 0;
    case WM_CHAR:
        if (readonly) return 0;
        if (wp == '\b') changed(c, SvEditDelete(m, 1));
        else if (wp == '\r' && (c->style & ES_MULTILINE)) changed(c, SvEditReplace(m, L"\r\n", 2));
        else if (wp == '\t' && (c->style & ES_MULTILINE)) { WCHAR ch = '\t'; changed(c, SvEditReplace(m, &ch, 1)); }
        else if (wp >= 32 && wp <= 0xFFFF && wp != 127) { WCHAR ch = (WCHAR)wp; changed(c, SvEditReplace(m, &ch, 1)); }
        return 0;
    case WM_KEYDOWN: {
        int shift = GetKeyState(VK_SHIFT) < 0, ctrl = GetKeyState(VK_CONTROL) < 0;
        int line = SvEditLineFromOffset(m, -1), start = SvEditLineStart(m, line);
        if (ctrl && wp == 'A') { move_caret(c, m->length, 0); m->anchor = 0; SvControlInvalidate(c); }
        else if (wp == VK_DELETE && !readonly) changed(c, SvEditDelete(m, 0));
        else if (wp == VK_LEFT || wp == VK_RIGHT) {
            int p;
            if (!shift && m->caret != m->anchor) p = wp == VK_LEFT ? (m->caret < m->anchor ? m->caret : m->anchor) : (m->caret > m->anchor ? m->caret : m->anchor);
            else p = horizontal(m, wp == VK_LEFT ? -1 : 1);
            move_caret(c, p, shift);
        } else if (wp == VK_HOME) move_caret(c, ctrl ? 0 : start, shift);
        else if (wp == VK_END) move_caret(c, ctrl ? m->length : SvEditLineEnd(m, start), shift);
        else if (wp == VK_UP || wp == VK_DOWN) {
            HDC dc = measure(c); if (!dc) return 0;
            RECT client = {0}; GetClientRect(c->hwnd, &client);
            int origin = line_origin(c, dc, start, SvEditLineEnd(m, start), client.right);
            int x = origin + advance(dc, m->text, start, m->caret, 0) - c->scroll_x;
            int next = line + (wp == VK_UP ? -1 : 1);
            if (next >= 0) move_caret(c, offset_at(c, dc, next, x, client.right), shift);
            ReleaseDC(c->hwnd, dc);
        }
        return 0;
    }
    case WM_LBUTTONDOWN:
        SetFocus(c->hwnd); SetCapture(c->hwnd); c->dragging = 1;
        /* fall through */
    case WM_MOUSEMOVE:
        if (msg == WM_LBUTTONDOWN || c->dragging) {
            HDC dc = measure(c); if (!dc) return 0;
            RECT client = {0}; GetClientRect(c->hwnd, &client);
            int height = line_height(dc);
            int top = top_margin(c, client.bottom, height);
            int line = ((short)HIWORD(lp) - top) / height + c->first_line;
            if (line < 0) line = 0;
            int offset = offset_at(c, dc, line, (short)LOWORD(lp), client.right);
            move_caret(c, offset, msg == WM_MOUSEMOVE || GetKeyState(VK_SHIFT) < 0);
            ReleaseDC(c->hwnd, dc);
        }
        return 0;
    case WM_LBUTTONUP: c->dragging = 0; ReleaseCapture(); return 0;
    case WM_CAPTURECHANGED: c->dragging = 0; return 0;
    }
    return DefWindowProcW(c->hwnd, msg, wp, lp);
}
