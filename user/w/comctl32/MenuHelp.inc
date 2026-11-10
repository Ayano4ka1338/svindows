__declspec(dllimport) int LoadStringW(HINSTANCE, UINT, WCHAR *, int);

COMCTL32_API void MenuHelp(UINT message, WPARAM wp, LPARAM lp, HMENU main_menu,
    HINSTANCE instance, HWND status, UINT *ids)
{
    (void)main_menu;
    if (!status || message != 0x11f) return;
    if (HIWORD(wp) == 0xffff && !lp) {
        SendMessageW(status, WM_USER+9, FALSE, 0);
        return;
    }
    UINT resource = (HIWORD(wp) & 0x10) ? (ids ? ids[1] : 0) : LOWORD(wp);
    WCHAR help[256]; help[0] = 0;
    if (resource) LoadStringW(instance, resource, help, 256);
    SendMessageW(status, SB_SETTEXTW, 255 | SBT_NOBORDERS, (LPARAM)help);
    SendMessageW(status, WM_USER+9, TRUE, 0);
}
