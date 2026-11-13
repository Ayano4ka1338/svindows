/* Public shell APIs backed by desktop-owned state in win32k. */
#include "../../../kernel/include/abi/WindowEnvironment.h"
__declspec(dllimport) UINT64 SvNtUserShellControl(UINT operation, UINT64 argument, UINT64 value);
__declspec(dllimport) UINT64 SvNtUserWindowLookup(UINT operation, UINT64 parent,
    UINT64 after, UINT64 class_name, UINT64 title);
__declspec(dllimport) UINT64 NtUserRegisterHotKey(HWND window, int id, UINT modifiers, UINT key);
__declspec(dllimport) UINT64 SvNtUserHotkeyControl(UINT operation, UINT64 window, INT64 id);

USER32_API BOOL RegisterHotKey(HWND window, int id, UINT modifiers, UINT key)
{ return NtUserRegisterHotKey(window, id, modifiers, key) != 0; }
USER32_API BOOL UnregisterHotKey(HWND window, int id)
{ return SvNtUserHotkeyControl(0, (UINT_PTR)window, id) != 0; }
USER32_API short GetAsyncKeyState(int key)
{ return (short)SvNtUserHotkeyControl(1, (UINT64)(UINT)key, 0); }

USER32_API HWND GetShellWindow(void)
{ return (HWND)(UINT_PTR)SvNtUserShellControl(SV_SHELL_GET_WINDOW, 0, 0); }
USER32_API BOOL SetShellWindow(HWND window)
{ return SvNtUserShellControl(SV_SHELL_SET_WINDOW, (UINT_PTR)window, 0) != 0; }
USER32_API BOOL RegisterShellHookWindow(HWND window)
{ return SvNtUserShellControl(SV_SHELL_REGISTER_HOOK, (UINT_PTR)window, 0) != 0; }
USER32_API BOOL DeregisterShellHookWindow(HWND window)
{ return SvNtUserShellControl(SV_SHELL_DEREGISTER_HOOK, (UINT_PTR)window, 0) != 0; }
USER32_API BOOL EnumWindows(BOOL (*callback)(HWND, LONG_PTR), LONG_PTR data)
{ return EnumDesktopWindows(NULL, callback, data); }
USER32_API HWND FindWindowExW(HWND parent, HWND after, const WCHAR *class_name, const WCHAR *title)
{
    return (HWND)(UINT_PTR)SvNtUserWindowLookup(0, (UINT_PTR)parent,
        (UINT_PTR)after, (UINT_PTR)class_name, (UINT_PTR)title);
}
