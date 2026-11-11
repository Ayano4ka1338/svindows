#include "Control.h"

enum {
    SBM_SETPOS = 0xe0, SBM_GETPOS, SBM_SETRANGE, SBM_GETRANGE,
    SBM_SETRANGEREDRAW = 0xe6, SBM_SETSCROLLINFO = 0xe9, SBM_GETSCROLLINFO = 0xeb
};
enum {
    SIF_RANGE = 1, SIF_PAGE = 2, SIF_POS = 4, SIF_TRACKPOS = 16
};
enum {
    SB_LINEUP, SB_LINEDOWN, SB_PAGEUP, SB_PAGEDOWN, SB_THUMBPOSITION,
    SB_THUMBTRACK, SB_TOP, SB_BOTTOM, SB_ENDSCROLL
};

typedef struct {
    int length, width, arrow, thumb, thumb_start;
} SCROLL_GEOMETRY;

static int scroll_max(const SV_SCROLLINFO *scroll)
{
    return (int)((long long)scroll->Max - (scroll->Page ? scroll->Page - 1 : 0));
}

static void scroll_clamp(SV_SCROLLINFO *scroll)
{
    unsigned long long range = (long long)scroll->Max - scroll->Min + 1;
    if (scroll->Page > range) scroll->Page = (UINT)range;
    int max = scroll_max(scroll);
    if (scroll->Position < scroll->Min) scroll->Position = scroll->Min;
    if (scroll->Position > max) scroll->Position = max;
}

static BOOL scroll_vertical(SV_CONTROL *control)
{
    return (control->style & 1) != 0;
}

static SCROLL_GEOMETRY scroll_geometry(SV_CONTROL *control)
{
    RECT client = {0};
    GetClientRect(control->hwnd, &client);
    BOOL vertical = scroll_vertical(control);
    SCROLL_GEOMETRY g = {
        .length = vertical ? client.bottom : client.right,
        .width = vertical ? client.right : client.bottom
    };
    g.arrow = g.width < g.length / 2 ? g.width : g.length / 2;
    int track = g.length - g.arrow * 2;
    SV_SCROLLINFO *scroll = &control->scroll;
    long long span = (long long)scroll_max(scroll) - scroll->Min;
    long long range = (long long)scroll->Max - scroll->Min + 1;
    g.thumb = scroll->Page ? (int)((long long)track * scroll->Page / range) : g.width;
    if (g.thumb < 8) g.thumb = 8;
    if (g.thumb > track || !span) g.thumb = track;
    g.thumb_start = g.arrow;
    if (span) {
        int position = control->dragging ? scroll->TrackPosition : scroll->Position;
        g.thumb_start += (int)(((long long)position - scroll->Min) * (track - g.thumb) / span);
    }
    return g;
}

static void scroll_fill(SV_CONTROL *control, HDC dc, int x, int y, int w, int h, COLORREF color)
{
    if (scroll_vertical(control)) SvControlFill(dc, y, x, h, w, color);
    else SvControlFill(dc, x, y, w, h, color);
}

static void scroll_paint(SV_CONTROL *control)
{
    PAINTSTRUCT paint;
    HDC dc = BeginPaint(control->hwnd, &paint);
    if (!dc) return;
    SCROLL_GEOMETRY g = scroll_geometry(control);
    COLORREF face = RGB(226,226,226), ink = RGB(83,83,83);
    scroll_fill(control,dc,0,0,g.length,g.width,RGB(244,244,244));
    scroll_fill(control,dc,0,0,g.arrow,g.width,face);
    scroll_fill(control,dc,g.length-g.arrow,0,g.arrow,g.width,face);
    scroll_fill(control,dc,g.thumb_start+1,1,g.thumb-2,g.width-2,RGB(187,187,187));
    if (g.arrow >= 8 && g.width >= 8) {
        int center = g.width / 2, tip = g.arrow / 2 - 1;
        for (int i = 0; i < 3; ++i) {
            scroll_fill(control,dc,tip+i,center-i,1,1+2*i,ink);
            scroll_fill(control,dc,g.length-1-tip-i,center-i,1,1+2*i,ink);
        }
    }
    EndPaint(control->hwnd, &paint);
}

static void scroll_notify(SV_CONTROL *control, UINT command, int position)
{
    if (!control->dead && control->parent)
        SendMessageW(control->parent, scroll_vertical(control) ? 0x115 : 0x114,
                     MAKELONG(command, position), (LPARAM)control->hwnd);
}

static int scroll_pointer(SV_CONTROL *control, LPARAM lp)
{
    return (short)(scroll_vertical(control) ? HIWORD(lp) : LOWORD(lp));
}

LRESULT SvScrollbarMessage(SV_CONTROL *control, UINT msg, WPARAM wp, LPARAM lp)
{
    SV_SCROLLINFO *scroll = &control->scroll;
    switch (msg) {
    case SBM_SETPOS: {
        int old = scroll->Position;
        scroll->Position = (int)wp;
        scroll_clamp(scroll);
        if (lp) SvControlInvalidate(control);
        return old;
    }
    case SBM_GETPOS:
        return scroll->Position;
    case SBM_SETRANGE:
    case SBM_SETRANGEREDRAW: {
        int old = scroll->Position;
        scroll->Min = (int)wp;
        scroll->Max = (int)lp < scroll->Min ? scroll->Min : (int)lp;
        scroll_clamp(scroll);
        if (msg == SBM_SETRANGEREDRAW) SvControlInvalidate(control);
        return old == scroll->Position ? 0 : old;
    }
    case SBM_GETRANGE:
        if (wp) *(int *)wp = scroll->Min;
        if (lp) *(int *)lp = scroll->Max;
        return 0;
    case SBM_SETSCROLLINFO: {
        const SV_SCROLLINFO *info = (const SV_SCROLLINFO *)lp;
        if (!info || info->Size != sizeof(*info)) return 0;
        if (info->Mask & SIF_RANGE) {
            scroll->Min = info->Min;
            scroll->Max = info->Max < info->Min ? info->Min : info->Max;
        }
        if (info->Mask & SIF_PAGE) scroll->Page = info->Page;
        if (info->Mask & SIF_POS) scroll->Position = info->Position;
        scroll_clamp(scroll);
        if (wp) SvControlInvalidate(control);
        return scroll->Position;
    }
    case SBM_GETSCROLLINFO: {
        SV_SCROLLINFO *info = (SV_SCROLLINFO *)lp;
        if (!info || info->Size != sizeof(*info)) return FALSE;
        if (info->Mask & SIF_RANGE) { info->Min = scroll->Min; info->Max = scroll->Max; }
        if (info->Mask & SIF_PAGE) info->Page = scroll->Page;
        if (info->Mask & SIF_POS) info->Position = scroll->Position;
        if (info->Mask & SIF_TRACKPOS)
            info->TrackPosition = control->dragging ? scroll->TrackPosition : scroll->Position;
        return TRUE;
    }
    case WM_PAINT:
        scroll_paint(control);
        return 0;
    case WM_LBUTTONDOWN: {
        if ((control->style & WS_DISABLED) || scroll_max(scroll) == scroll->Min) return 0;
        SCROLL_GEOMETRY g = scroll_geometry(control);
        int p = scroll_pointer(control, lp);
        SetFocus(control->hwnd);
        if (control->dead) return 0;
        if (p >= g.thumb_start && p < g.thumb_start + g.thumb) {
            control->dragging = TRUE;
            control->scroll_drag_offset = p - g.thumb_start;
            scroll->TrackPosition = scroll->Position;
        }
        control->pressed = TRUE;
        SetCapture(control->hwnd);
        if (!control->dragging) {
            UINT command = p < g.arrow ? SB_LINEUP : p >= g.length - g.arrow ? SB_LINEDOWN :
                           p < g.thumb_start ? SB_PAGEUP : SB_PAGEDOWN;
            scroll_notify(control, command, 0);
        }
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (!control->dragging) return 0;
        SCROLL_GEOMETRY g = scroll_geometry(control);
        int travel = g.length - 2 * g.arrow - g.thumb;
        if (travel <= 0) return 0;
        int p = scroll_pointer(control, lp) - g.arrow - control->scroll_drag_offset;
        if (p < 0) p = 0;
        if (p > travel) p = travel;
        long long span = (long long)scroll_max(scroll) - scroll->Min;
        int next = (int)(scroll->Min + (span * p + travel / 2) / travel);
        if (next != scroll->TrackPosition) {
            scroll->TrackPosition = next;
            SvControlInvalidate(control);
            scroll_notify(control, SB_THUMBTRACK, next);
        }
        return 0;
    }
    case WM_LBUTTONUP:
        if (control->pressed || control->dragging) {
            if (control->dragging) scroll_notify(control, SB_THUMBPOSITION, scroll->TrackPosition);
            control->dragging = control->pressed = FALSE;
            if (!control->dead) ReleaseCapture();
            SvControlInvalidate(control);
            scroll_notify(control, SB_ENDSCROLL, 0);
        }
        return 0;
    case WM_CAPTURECHANGED:
        control->dragging = control->pressed = FALSE;
        SvControlInvalidate(control);
        return 0;
    case WM_KEYDOWN: {
        if ((control->style & WS_DISABLED) || scroll_max(scroll) == scroll->Min) return 0;
        UINT command;
        switch (wp) {
        case VK_LEFT: case VK_UP: command = SB_LINEUP; break;
        case VK_RIGHT: case VK_DOWN: command = SB_LINEDOWN; break;
        case 0x21: command = SB_PAGEUP; break;
        case 0x22: command = SB_PAGEDOWN; break;
        case VK_HOME: command = SB_TOP; break;
        case VK_END: command = SB_BOTTOM; break;
        default: return DefWindowProcW(control->hwnd,msg,wp,lp);
        }
        scroll_notify(control, command, 0);
        return 0;
    }
    }
    return DefWindowProcW(control->hwnd,msg,wp,lp);
}
