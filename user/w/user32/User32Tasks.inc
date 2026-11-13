/* Window actions and queue waits used by desktop management clients. */
__declspec(dllimport) HANDLE OpenProcess(DWORD access, BOOL inherit, DWORD pid);
__declspec(dllimport) BOOL TerminateProcess(HANDLE process, UINT code);
__declspec(dllimport) BOOL CloseHandle(HANDLE handle);
__declspec(dllimport) DWORD GetLastError(void);
__declspec(dllimport) DWORD WaitForMultipleObjects(DWORD count,
    const HANDLE *handles, BOOL all, DWORD timeout);
__declspec(dllimport) void Sleep(DWORD milliseconds);
__declspec(dllimport) UINT64 NtUserCallOneParam(UINT64 value, UINT64 routine);
USER32_API BOOL EndTask(HWND window, BOOL shutdown, BOOL force)
{
    (void)shutdown; /* Reserved by the Win32 contract. */
    DWORD pid;
    if (!GetWindowThreadProcessId(window, &pid)) return FALSE;
    if (!force) return PostMessageW(window, WM_CLOSE, 0, 0);
    HANDLE process = OpenProcess(1, FALSE, pid); /* PROCESS_TERMINATE */
    if (!process) return FALSE;
    BOOL result = TerminateProcess(process, 0);
    DWORD error = GetLastError();
    CloseHandle(process);
    SetLastError(error);
    return result;
}

USER32_API BOOL AllowSetForegroundWindow(DWORD pid)
{
    /* This single interactive session has no foreground lock. */
    if (pid == (DWORD)-1) return TRUE;
    HANDLE process = OpenProcess(0x1000, FALSE, pid);
    if (!process) return FALSE;
    CloseHandle(process);
    return TRUE;
}

USER32_API BOOL ShowWindowAsync(HWND window, int command)
{
    if (!IsWindow(window)) { SetLastError(1400); return FALSE; }
    if (GetWindowThreadProcessId(window, NULL) == GetCurrentThreadId())
        return ShowWindow(window, command);
    /* Restore/minimize/maximize are already routed to the owner queue. */
    UINT action = command == SW_RESTORE || command == SW_SHOW ? 0xf120 :
                  command == SW_MINIMIZE ? 0xf020 :
                  command == 3 /* SW_MAXIMIZE */ ? 0xf030 : 0;
    if (action) return PostMessageW(window, WM_SYSCOMMAND, action, 0);
    SetLastError(50);
    return FALSE;
}

USER32_API BOOL OpenIcon(HWND window)
{
    if (!IsWindow(window)) { SetLastError(1400); return FALSE; }
    return !IsIconic(window) || ShowWindowAsync(window, SW_RESTORE);
}

USER32_API void SwitchToThisWindow(HWND window, BOOL alt_tab)
{
    (void)alt_tab;
    if (!IsWindow(window)) { SetLastError(1400); return; }
    if (IsIconic(window)) ShowWindowAsync(window, SW_RESTORE);
    SetForegroundWindow(window);
}

USER32_API HWND GhostWindowFromHungWindow(HWND window)
{ (void)window; return NULL; /* No ghost-window service is active. */ }
USER32_API HWND HungWindowFromGhostWindow(HWND window)
{ (void)window; return NULL; }

USER32_API HANDLE MonitorFromPoint(POINT point, DWORD flags)
{
    BOOL inside = point.x >= 0 && point.y >= 0 &&
        point.x < GetSystemMetrics(0) && point.y < GetSystemMetrics(1);
    return inside || (flags & 3) ? (HANDLE)1 : NULL;
}

USER32_API HBRUSH GetSysColorBrush(int index)
{
    static HBRUSH brushes[SV_SYSCOLOR_COUNT];
    if (index < 0 || index >= SV_SYSCOLOR_COUNT) return NULL;
    if (!brushes[index]) brushes[index] = CreateSolidBrush(GetSysColor(index));
    return brushes[index];
}

USER32_API DWORD MsgWaitForMultipleObjects(DWORD count, const HANDLE *handles,
    BOOL all, DWORD timeout, DWORD mask)
{
    if (count > 63 || (count && !handles)) {
        SetLastError(87); return (DWORD)-1;
    }
    HANDLE wait[64];
    for (DWORD i = 0; i < count; ++i) wait[i] = handles[i];
    if (mask) {
        HANDLE input = (HANDLE)(UINT_PTR)NtUserCallOneParam(mask, 0x32);
        if (!input) { SetLastError(8); return (DWORD)-1; }
        wait[count++] = input;
    }
    if (!count) { Sleep(timeout); return 258; }
    return WaitForMultipleObjects(count, wait, all, timeout);
}

USER32_API WORD TileWindows(HWND parent, UINT flags, const RECT *rect,
    UINT count, const HWND *windows)
{
    (void)parent; (void)flags; (void)rect; (void)count; (void)windows;
    SetLastError(50); return 0; /* Cross-process layout is not implemented. */
}
USER32_API WORD CascadeWindows(HWND parent, UINT flags, const RECT *rect,
    UINT count, const HWND *windows)
{
    return TileWindows(parent, flags, rect, count, windows);
}
