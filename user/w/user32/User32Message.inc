/*
 * user/w/user32/User32Message.inc - the message loop of user32.dll.
 *
 * Included by User32.c. The kernel owns the queues (kernel/win32k/
 * UserMessages.inc: one posted-message queue per thread, plus the window update
 * regions that turn into WM_PAINT) and runs the window procedure itself through
 * its kernel-to-user callback path (kernel/win32k/UserCallbacks.inc). This file
 * is therefore only the Win32-shaped spelling of those services.
 */

__declspec(dllimport) UINT64 NtUserGetMessage(MSG *message);
__declspec(dllimport) UINT64 NtUserPeekMessage(MSG *message, HWND hwnd, UINT first,
                                               UINT last, UINT remove);
__declspec(dllimport) UINT64 NtUserTranslateMessage(const MSG *message);
__declspec(dllimport) UINT64 NtUserDispatchMessage(const MSG *message);
__declspec(dllimport) UINT64 NtUserPostMessage(HWND hwnd, UINT message,
                                               WPARAM wparam, LPARAM lparam);
__declspec(dllimport) UINT64 NtUserMessageCall(HWND hwnd, UINT message,
                                               WPARAM wparam, LPARAM lparam,
                                               void *result_info, ULONG type,
                                               ULONG ansi);

USER32_API BOOL GetMessageW(MSG *message, HWND hwnd, UINT first, UINT last)
{
    if (!message)
        return (BOOL)-1;
    /* The window and message-range filters are accepted and not applied: the
     * kernel hands out the first message for the calling thread's queue
     * (NtUserGetMessage ignores its remaining arguments). A message loop that
     * relies on filtering therefore sees every message -- a superset, which a
     * loop that dispatches what it asked for handles correctly. */
    (void)hwnd;
    (void)first;
    (void)last;
    /* 1 = a message was stored, 0 = WM_QUIT, -1 = the caller's MSG was not
     * writable (the kernel's only failure). */
    return (BOOL)(INT64)NtUserGetMessage(message);
}

USER32_API BOOL PeekMessageW(MSG *message, HWND hwnd, UINT first, UINT last,
                             UINT remove)
{
    if (!message)
        return FALSE;
    /* PM_REMOVE is honoured by the kernel (bit 0 of the flags word). Note that
     * a PM_NOREMOVE peek on an empty queue still consumes one of the window's
     * bounded WM_PAINT redeliveries, because the kernel synthesizes the message
     * before it decides not to remove it. */
    return NtUserPeekMessage(message, hwnd, first, last, remove) != 0;
}

USER32_API BOOL TranslateMessage(const MSG *message)
{
    if (message)
        NtUserTranslateMessage(message);
    /* NtUserTranslateMessage is a stub that returns 0 without posting a
     * character: this kernel generates WM_CHAR in the input thread
     * (kernel/ke/KeMain.c -> KiUserQueueCharacter), not by translating
     * WM_KEYDOWN. FALSE is the honest answer -- no message was posted. */
    return FALSE;
}

USER32_API LRESULT DispatchMessageW(const MSG *message)
{
    if (!message)
        return 0;
    /* The kernel arms a kernel-to-user callback that runs the window procedure
     * and redirects this syscall's return to the procedure's LRESULT. Invalid targets or an exhausted callback stack return 0. */
    return (LRESULT)NtUserDispatchMessage(message);
}

USER32_API BOOL PostMessageW(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
{
    /* A posted message goes to the window owner thread's queue; hwnd == NULL is
     * the thread-message form, delivered to the posting thread's own queue. */
    return NtUserPostMessage(hwnd, message, wparam, lparam) != 0;
}

USER32_API LRESULT SendMessageW(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
{
    if (!hwnd)
        return 0;
    /* NtUserMessageCall runs the target window procedure through the same
     * callback path and returns its result to this caller. Nested calls use an independent callback frame up to the kernel nesting limit. */
    return (LRESULT)NtUserMessageCall(hwnd, message, wparam, lparam, NULL, 0, 0);
}

USER32_API void PostQuitMessage(int exit_code)
{
    /* WM_QUIT is a thread message: posted with hwnd == NULL, the kernel queues
     * it on the calling thread, which is what lets this work from inside a
     * window procedure without naming the thread. GetMessageW returns 0 for it. */
    NtUserPostMessage(NULL, WM_QUIT, (WPARAM)(INT_PTR)exit_code, 0);
}

USER32_API LRESULT DefWindowProcW(HWND hwnd, UINT message, WPARAM wparam,
                                  LPARAM lparam)
{
    if (message == WM_CLOSE) { DestroyWindow(hwnd); return 0; }
    if (message == WM_SYSCOMMAND && (wparam & 0xFFF0) == SC_CLOSE)
        return SendMessageW(hwnd, WM_CLOSE, 0, 0);
    if (message == WM_PAINT) {
        PAINTSTRUCT paint;
        if (BeginPaint(hwnd, &paint)) EndPaint(hwnd, &paint);
        return 0;
    }
    if (message == WM_SETTEXT) return NtUserSetWindowText(hwnd, (const WCHAR *)lparam);
    if (message == WM_GETTEXT) return NtUserGetWindowText(hwnd, (WCHAR *)lparam, (ULONG)wparam);
    if (message == WM_GETTEXTLENGTH) {
        WCHAR title[128];
        return NtUserGetWindowText(hwnd, title, 128);
    }
    return NtdllDefWindowProc_W(hwnd, message, wparam, lparam);
}

USER32_API LRESULT DefWindowProcA(HWND hwnd, UINT message, WPARAM wparam,
                                  LPARAM lparam)
{
    return NtdllDefWindowProc_A(hwnd, message, wparam, lparam);
}

USER32_API int MessageBoxW(HWND hwnd, const WCHAR *text, const WCHAR *caption,
                           UINT type)
{
    /*
     * MessageBox requires a user-mode dialog, controls and a modal loop.
     * The dialog manager is not implemented yet.
     *
     * 0 is the documented MessageBox failure result and is distinguishable from
     * every real button id (IDOK is 1), so a caller that checks for IDOK sees
     * that no dialog appeared instead of a fabricated answer.
     */
    (void)hwnd;
    (void)text;
    (void)caption;
    (void)type;
    return 0;
}
