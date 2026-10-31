#include "Control.h"
LRESULT SvStaticMessage(SV_CONTROL *c, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg != WM_PAINT) return DefWindowProcW(c->hwnd, msg, wp, lp);
    PAINTSTRUCT ps; HDC dc = BeginPaint(c->hwnd, &ps);
    if (!dc) return 0;
    RECT r; GetClientRect(c->hwnd, &r);
    SvControlFill(dc, 0, 0, r.right, r.bottom, SV_CONTROL_SURFACE);
    SelectObject(dc, c->font); SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, c->style & WS_DISABLED ? RGB(131,131,131) : RGB(0,0,0));
    UINT flags = (c->style & 31) == SS_CENTER ? DT_CENTER : (c->style & 31) == SS_RIGHT ? DT_RIGHT : DT_LEFT;
    flags |= (c->style & 31) == SS_LEFTNOWORDWRAP || (c->style & 31) == SS_SIMPLE ? DT_SINGLELINE : DT_WORDBREAK;
    if (c->style & SS_NOPREFIX) flags |= DT_NOPREFIX;
    DrawTextW(dc, c->model.text, c->model.length, &r, flags);
    EndPaint(c->hwnd, &ps); return 0;
}
