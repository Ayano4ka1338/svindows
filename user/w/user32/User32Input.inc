/* USER cursor handles, independent activation/focus, capture and queued key
 * state. win32k routes pointer input to child HWNDs without drawing a cursor. */

__declspec(dllimport) UINT64 NtUserSetCursor(HCURSOR cursor);
__declspec(dllimport) UINT64 NtUserSetFocus(HWND hwnd);
__declspec(dllimport) UINT64 NtUserGetFocus(void);
__declspec(dllimport) UINT64 NtUserSetCapture(HWND hwnd);
__declspec(dllimport) UINT64 NtUserReleaseCapture(void);

USER32_API HCURSOR LoadCursorW(HINSTANCE instance, const WCHAR *name)
{
    if (!name)
        return NULL;
    /* The kernel caches cursors per (module, resource) and hands the same
     * handle back for a repeated request, which is what a window class needs:
     * TRUE from LoadCursorW without a second cursor object. */
    SV_LARGE_STRING resource;
    resource.length = IS_INTRESOURCE(name) ? 0
                                           : (ULONG)(sv_wcslen(name) * sizeof(WCHAR));
    resource.flags = 0;
    resource.buffer = name;
    return (HCURSOR)(UINT_PTR)NtUserFindExistingCursorIcon(instance, &resource, NULL);
}

USER32_API HCURSOR SetCursor(HCURSOR cursor)
{
    (void)NtUserSetCursor(cursor);
    /* The window manager tracks this handle; cursor rendering is disabled. */
    return NULL;
}

USER32_API HWND SetFocus(HWND hwnd)
{
    /* NtUserSetFocus returns the previously focused window and makes the window
     * manager generate WM_KILLFOCUS/WM_SETFOCUS for the pair, which is the
     * documented SetFocus contract. */
    return (HWND)(UINT_PTR)NtUserSetFocus(hwnd);
}

USER32_API HWND GetFocus(void)
{
    return (HWND)(UINT_PTR)NtUserGetFocus();
}

__declspec(dllimport) UINT64 NtUserSetActiveWindow(HWND hwnd);
__declspec(dllimport) UINT64 NtUserGetForegroundWindow(void);
__declspec(dllimport) UINT64 NtUserGetKeyState(int key);
__declspec(dllimport) UINT64 NtUserTrackMouseEvent(TRACKMOUSEEVENT *event);
USER32_API HWND SetActiveWindow(HWND hwnd) { return (HWND)(UINT_PTR)NtUserSetActiveWindow(hwnd); }
__declspec(dllimport) UINT64 NtUserQueryWindow(HWND hwnd, UINT type);
__declspec(dllimport) DWORD GetCurrentThreadId(void);
USER32_API HWND GetActiveWindow(void)
{
    HWND hwnd = (HWND)(UINT_PTR)NtUserGetForegroundWindow();
    return hwnd && NtUserQueryWindow(hwnd, 0) == GetCurrentThreadId() ? hwnd : NULL;
}
USER32_API HWND GetForegroundWindow(void) { return (HWND)(UINT_PTR)NtUserGetForegroundWindow(); }
USER32_API BOOL TrackMouseEvent(TRACKMOUSEEVENT *event) { return NtUserTrackMouseEvent(event) != 0; }

USER32_API HWND SetCapture(HWND hwnd)
{
    /* The window manager records the capture window and routes mouse messages
     * to it; the return value is that previous window, or NULL when there was
     * none. */
    return (HWND)(UINT_PTR)NtUserSetCapture(hwnd);
}

USER32_API BOOL ReleaseCapture(void)
{
    /* TRUE when this thread's window actually held the capture, which is what
     * the previous-capture return value tells us. */
    return NtUserReleaseCapture() != 0;
}

USER32_API short GetKeyState(int key)
{
    return (short)NtUserGetKeyState(key);
}
