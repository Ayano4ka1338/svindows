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

__declspec(dllimport) UINT64 SvNtUserSetCursorPos(int x, int y);

USER32_API BOOL SetCursorPos(int x, int y)
{
    return SvNtUserSetCursorPos(x, y) != 0;
}

USER32_API int ShowCursor(BOOL show)
{
    /* Native pointer coordinates exist independently of cursor artwork. */
    static int display_count;
    return __atomic_add_fetch(&display_count, show ? 1 : -1, __ATOMIC_RELAXED);
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

__declspec(dllimport) UINT64 SvNtUserIsIconic(HWND hwnd);

USER32_API BOOL IsIconic(HWND hwnd)
{
    return SvNtUserIsIconic(hwnd) != 0;
}

USER32_API HANDLE GetKeyboardLayout(DWORD thread)
{
    (void)thread;
    /* The USER keyboard mapper has one installed layout, US English. */
    return (HANDLE)(UINT_PTR)0x04090409;
}

extern void SetLastError(DWORD error);

USER32_API HANDLE ActivateKeyboardLayout(HANDLE layout, UINT flags)
{
    UINT_PTR requested = (UINT_PTR)layout;
    /* PREV/NEXT both select the sole installed layout. Unsupported layouts
       must fail rather than claiming that the keyboard mapping changed. */
    if ((flags & ~(0x100u | 0x8u | 0x10u | 0x40000000u)) ||
        (requested != 0 && requested != 1 && requested != 0x04090409)) {
        SetLastError(87);
        return NULL;
    }
    return GetKeyboardLayout(0);
}

typedef void (*SV_WINEVENTPROC)(HANDLE, DWORD, HWND, LONG, LONG, DWORD, DWORD);
__declspec(dllimport) UINT64 NtUserSetWinEventHook(DWORD first, DWORD last,
    HINSTANCE module, const void *module_name, SV_WINEVENTPROC callback,
    DWORD process, DWORD thread, DWORD flags);
__declspec(dllimport) UINT64 NtUserUnhookWinEvent(HANDLE hook);
extern void SetLastError(DWORD error);

USER32_API HANDLE SetWinEventHook(DWORD first, DWORD last, HINSTANCE module,
    SV_WINEVENTPROC callback, DWORD process, DWORD thread, DWORD flags)
{
    if (!callback || first > last || (flags & ~7u)) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return NULL;
    }
    HANDLE hook = (HANDLE)(UINT_PTR)NtUserSetWinEventHook(first, last, module,
        NULL, callback, process, thread, flags);
    if (!hook) SetLastError(50); /* ERROR_NOT_SUPPORTED */
    return hook;
}

USER32_API BOOL UnhookWinEvent(HANDLE hook)
{
    if (hook && NtUserUnhookWinEvent(hook)) return TRUE;
    SetLastError(6); /* ERROR_INVALID_HANDLE */
    return FALSE;
}

USER32_API BOOL IsWinEventHookInstalled(DWORD event)
{
    (void)event;
    /* NtUserSetWinEventHook currently rejects every registration, so no
       event can have an installed hook. Keep this query consistent with it. */
    return FALSE;
}

__declspec(dllimport) UINT64 NtUserSetTimer(HWND window, UINT_PTR id, UINT timeout,
    void (*callback)(HWND, UINT, UINT_PTR, DWORD), UINT tolerance);
__declspec(dllimport) UINT64 NtUserKillTimer(HWND window, UINT_PTR id);

USER32_API UINT_PTR SetTimer(HWND window, UINT_PTR id, UINT timeout,
    void (*callback)(HWND, UINT, UINT_PTR, DWORD))
{
    UINT_PTR timer = NtUserSetTimer(window, id, timeout, callback, 0);
    if (!timer) SetLastError(50); /* ERROR_NOT_SUPPORTED */
    return timer;
}

USER32_API BOOL KillTimer(HWND window, UINT_PTR id)
{
    if (NtUserKillTimer(window, id)) return TRUE;
    SetLastError(87); /* ERROR_INVALID_PARAMETER */
    return FALSE;
}

__declspec(dllimport) UINT64 SvNtUserGetMessagePos(void);

USER32_API DWORD GetMessagePos(void)
{
    return (DWORD)SvNtUserGetMessagePos();
}

/* Carets are currently painted by the EDIT control itself. There is no
 * queue-owned native caret service for applications which manage their own. */
USER32_API BOOL CreateCaret(HWND window, HBITMAP bitmap, int width, int height)
{
    (void)bitmap; (void)window;
    if (width < 0 || height < 0) {
        SetLastError(87);
        return FALSE;
    }
    SetLastError(50);
    return FALSE;
}
USER32_API BOOL DestroyCaret(void)
{ SetLastError(50); return FALSE; }
USER32_API BOOL ShowCaret(HWND window)
{ (void)window; SetLastError(50); return FALSE; }
USER32_API BOOL HideCaret(HWND window)
{ (void)window; SetLastError(50); return FALSE; }
USER32_API BOOL SetCaretPos(int x, int y)
{ (void)x; (void)y; SetLastError(50); return FALSE; }
