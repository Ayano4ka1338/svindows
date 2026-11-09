/*
 * user/w/user32/User32Paint.inc - update regions, painting and window DCs.
 *
 * Included by User32.c. The update region lives in win32k, not here: a window
 * carries a bounded "WM_PAINT still owed" count, GetMessage synthesizes the
 * message for as long as it is non-zero, and BeginPaint/ValidateRect clear it
 * (kernel/win32k/UserMessages.inc). These are the Win32 spellings of that.
 */

__declspec(dllimport) UINT64 NtUserBeginPaint(HWND hwnd, PAINTSTRUCT *paint);
__declspec(dllimport) UINT64 NtUserEndPaint(HWND hwnd, const PAINTSTRUCT *paint);
__declspec(dllimport) UINT64 NtUserInvalidateRect(HWND hwnd, const RECT *rect,
                                                  BOOL erase);
__declspec(dllimport) UINT64 NtUserValidateRect(HWND hwnd, const RECT *rect);
__declspec(dllimport) UINT64 NtUserUpdateWindow(HWND hwnd);
__declspec(dllimport) UINT64 NtUserGetDC(HWND hwnd);

USER32_API HDC BeginPaint(HWND hwnd, PAINTSTRUCT *paint)
{
    if (!paint)
        return NULL;
    /* The kernel fills the whole 72-byte PAINTSTRUCT, validates the window (the
     * update region is whole-window, so rcPaint is the entire client area) and
     * hands back the window's cached device context over its client raster.
     * EndPaint publishes the completed paint to the compositor. */
    HDC dc = (HDC)(UINT_PTR)NtUserBeginPaint(hwnd, paint);
    /* BeginPaint opens a retained window-surface transaction. Erase through
     * the class/application procedure now, before the caller draws its paint
     * content; EndPaint will publish both together. */
    if (dc && paint->fErase)
        (void)SendMessageW(hwnd, WM_ERASEBKGND, (WPARAM)dc, 0);
    return dc;
}

USER32_API BOOL EndPaint(HWND hwnd, const PAINTSTRUCT *paint)
{
    /* Publish the completed client raster and compose the changed screen
     * region. The previous paint remains visible until this call. */
    if (!paint)
        return FALSE;
    return NtUserEndPaint(hwnd, paint) != 0;
}

USER32_API BOOL InvalidateRect(HWND hwnd, const RECT *rect, BOOL erase)
{
    /* The update region is "the whole window" in this kernel (the region
     * argument is accepted and not narrowed), and erase only decides whether a
     * client is expected to erase: begin-paint always reports fErase = 1. */
    return NtUserInvalidateRect(hwnd, rect, erase) != 0;
}

USER32_API BOOL ValidateRect(HWND hwnd, const RECT *rect)
{
    return NtUserValidateRect(hwnd, rect) != 0;
}

USER32_API BOOL UpdateWindow(HWND hwnd)
{
    /* Synchronous WM_PAINT, including calls from another window procedure. */
    return NtUserUpdateWindow(hwnd) != 0;
}

USER32_API HDC GetDC(HWND hwnd)
{
    /* GetDC(NULL) is the whole screen; GetDC(hwnd) is that window's client
     * area. Window DCs retain their pixels independently of the framebuffer;
     * drawing outside a BeginPaint/EndPaint pair publishes each GDI operation.
     * Repeated calls reuse the cached DC. */
    return (HDC)(UINT_PTR)NtUserGetDC(hwnd);
}

USER32_API int ReleaseDC(HWND hwnd, HDC hdc)
{
    /*
     * There is no NtUserReleaseDC service. The kernel caches one window DC per
     * window and returns the same context to every GetDC/BeginPaint until the
     * window is destroyed (W32kAcquireWindowDc -> the g_window_dc cache), so
     * there is nothing for the client to release and the cache exists precisely
     * so that a GetDC/ReleaseDC pair does not burn a handle per repaint.
     *
     * 1 is the documented success result; the DC stays valid until the window
     * is destroyed, which is why a client must not keep drawing through it
     * after ReleaseDC.
     */
    (void)hwnd;
    (void)hdc;
    return 1;
}
