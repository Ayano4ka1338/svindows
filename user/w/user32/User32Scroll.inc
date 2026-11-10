/* Scroll ranges belong to the window. Standalone scroll bars receive their
 * standard control messages and keep their own state. */
typedef struct {
    UINT Size, Mask;
    int Min, Max;
    UINT Page;
    int Position, TrackPosition;
} SV_SCROLLINFO;

static SV_SCROLL_STATE *window_scroll(HWND hwnd, int bar)
{
    if (bar < 0 || bar > 1) return 0;
    RECT rect;
    if (!GetClientRect(hwnd, &rect)) return 0;
    SV_USER_WINDOW_STATE *window = sv_user_state(hwnd);
    if (!window) return 0;
    if (!(window->Style & (bar == 0 ? WS_HSCROLL : WS_VSCROLL))) return 0;
    return &window->Scroll[bar];
}

static int set_scroll_position(SV_SCROLL_STATE *scroll, int pos)
{
    INT64 max = scroll->Max;
    if (scroll->Page) max -= (INT64)scroll->Page - 1;
    if (pos < scroll->Min) pos = scroll->Min;
    if (pos > max) pos = (int)max;
    int previous = scroll->Position;
    scroll->Position = pos;
    return previous;
}

USER32_API int SetScrollPos(HWND hwnd, int bar, int pos, BOOL redraw)
{
    if (bar == 2) /* SB_CTL */
        return (int)SendMessageW(hwnd, 0x00e0 /* SBM_SETPOS */, pos, redraw);
    SV_SCROLL_STATE *scroll = window_scroll(hwnd, bar);
    if (!scroll) return 0;
    int previous = set_scroll_position(scroll, pos);
    if (redraw && scroll->Position != previous) InvalidateRect(hwnd, 0, FALSE);
    return previous;
}

USER32_API int SetScrollInfo(HWND hwnd, int bar, const SV_SCROLLINFO *info, BOOL redraw)
{
    if (!info || info->Size != sizeof(*info)) return 0;
    if (bar == 2)
        return (int)SendMessageW(hwnd, 0x00e9 /* SBM_SETSCROLLINFO */, redraw, (LPARAM)info);
    SV_SCROLL_STATE *scroll = window_scroll(hwnd, bar);
    if (!scroll) return 0;
    if (info->Mask & 1) { /* SIF_RANGE */
        scroll->Min = info->Min;
        scroll->Max = info->Max >= info->Min ? info->Max : info->Min;
    }
    if (info->Mask & 2) scroll->Page = info->Page; /* SIF_PAGE */
    UINT64 range = (UINT64)((INT64)scroll->Max - scroll->Min + 1);
    if (scroll->Page > range) scroll->Page = (UINT)range;
    set_scroll_position(scroll, info->Mask & 4 ? info->Position : scroll->Position);
    if (redraw) InvalidateRect(hwnd, 0, FALSE);
    return scroll->Position;
}
