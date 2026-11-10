__declspec(dllimport) HWND GetDlgItem(HWND, int);
__declspec(dllimport) LONG_PTR GetWindowLongPtrW(HWND, int);
__declspec(dllimport) int MapWindowPoints(HWND, HWND, POINT *, UINT);

COMCTL32_API void GetEffectiveClientRect(HWND window, RECT *rect, const int *controls)
{
    if (!rect) return;
    *rect = (RECT){0};
    if (!GetClientRect(window, rect) || !controls) return;
    /* The first pair describes the menu, not a child window. */
    for (controls += 2; controls[0]; controls += 2) {
        HWND child = GetDlgItem(window, controls[1]);
        RECT occupied;
        if (!child || !(GetWindowLongPtrW(child, -16) & WS_VISIBLE) ||
            !GetWindowRect(child, &occupied)) continue;
        MapWindowPoints(0, window, (POINT *)&occupied, 2);
        if (occupied.left < rect->left) occupied.left = rect->left;
        if (occupied.top < rect->top) occupied.top = rect->top;
        if (occupied.right > rect->right) occupied.right = rect->right;
        if (occupied.bottom > rect->bottom) occupied.bottom = rect->bottom;
        if (occupied.left >= occupied.right || occupied.top >= occupied.bottom) continue;
        RECT choices[4] = {*rect, *rect, *rect, *rect};
        choices[0].right = occupied.left;
        choices[1].left = occupied.right;
        choices[2].bottom = occupied.top;
        choices[3].top = occupied.bottom;
        unsigned long long largest = 0;
        RECT result = {rect->left, rect->top, rect->left, rect->top};
        for (UINT i = 0; i < 4; i++) {
            unsigned long long area = (unsigned long long)(choices[i].right - choices[i].left) *
                                      (choices[i].bottom - choices[i].top);
            if (area > largest) { largest = area; result = choices[i]; }
        }
        *rect = result;
    }
}
