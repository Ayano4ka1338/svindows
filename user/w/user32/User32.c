/* USER client API and standard controls. The kernel owns HWNDs, geometry,
 * queues and composition; control state and window procedures live in user
 * mode. Common rendering is shared through GDI32 and UXTHEME. */

typedef unsigned char  UINT8;
typedef unsigned short UINT16;
typedef unsigned int   UINT32;
typedef unsigned long long UINT64;
typedef long long      INT64;

#include "User32.h"
#include "controls/Control.h"
_Static_assert(sizeof(CREATESTRUCTW) == 80, "CREATESTRUCTW x64");

/* The kernel fills these structures by fixed byte offsets: MSG must match
 * USER_MSG_LOCAL (48 bytes) and PAINTSTRUCT is what NtUserBeginPaint clears and
 * writes (72 bytes). Fail the build rather than silently mis-marshalling. */
_Static_assert(sizeof(MSG) == 48, "MSG must match the kernel's USER_MSG_LOCAL");
_Static_assert(sizeof(PAINTSTRUCT) == 72, "PAINTSTRUCT is 72 bytes on x64");
_Static_assert(sizeof(WNDCLASSEXW) == 80, "WNDCLASSEXW is 80 bytes on x64");

/* ------------------------------------------------------------------ */
/* Native services (user/w/ntdll/Ntdll.asm -> kernel/win32k)              */
/* ------------------------------------------------------------------ */

/* NtUserCreateWindowEx(exStyle, class, classVersion, name, style, x, y, cx, cy,
 * parent, menu, instance, param, ?, ?): the kernel reads a[0]..a[12] and builds
 * the CREATESTRUCTW a WM_NCCREATE/WM_CREATE handler receives. */
__declspec(dllimport) UINT64 NtUserCreateWindowEx(ULONG ex_style, UINT64 class_name,
                                                  UINT64 class_version, UINT64 name,
                                                  ULONG style, int x, int y, int cx,
                                                  int cy, HWND parent, HMENU menu,
                                                  HINSTANCE instance, void *param,
                                                  UINT64 reserved1, UINT64 reserved2,
                                                  UINT64 reserved3);
__declspec(dllimport) UINT64 NtUserDestroyWindow(HWND hwnd);
__declspec(dllimport) UINT64 NtUserSetWindowText(HWND hwnd, const WCHAR *text);
__declspec(dllimport) UINT64 NtUserGetWindowText(HWND hwnd, WCHAR *buffer, ULONG count);
__declspec(dllimport) UINT64 NtUserWindowFromPoint(int x, int y);
__declspec(dllimport) UINT64 NtUserRegisterClassExWOW(const void *wndclass,
                                                      const void *class_name,
                                                      const void *menu_name,
                                                      ULONG fnid, ULONG flags,
                                                      USHORT wow);
__declspec(dllimport) UINT64 NtUserUnregisterClass(HINSTANCE instance,
                                                   const WCHAR **class_name,
                                                   const void *menu_name);
__declspec(dllimport) UINT64 NtUserShowWindow(HWND hwnd, int command);
__declspec(dllimport) UINT64 NtUserSetWindowPos(HWND hwnd, HWND insert_after, int x,
                                                int y, int cx, int cy, ULONG flags);
__declspec(dllimport) UINT64 NtUserMoveWindow(HWND hwnd, int x, int y, int cx, int cy,
                                              BOOL repaint);
__declspec(dllimport) UINT64 NtUserGetWindowRect(HWND hwnd, RECT *rect);
__declspec(dllimport) UINT64 NtUserGetClientRect(HWND hwnd, RECT *rect);
__declspec(dllimport) UINT64 NtUserFindExistingCursorIcon(HINSTANCE instance,
                                                          const void *resource,
                                                          const void *param);
/* ntdll's default window procedure: the real DefWindowProc lives there
 * (user/w/ntdll/Rtl/Compat.inc) because the per-message default results are part
 * of the native surface, not of this DLL. */
__declspec(dllimport) LRESULT NtdllDefWindowProc_W(HWND hwnd, UINT message,
                                                   WPARAM wparam, LPARAM lparam);
__declspec(dllimport) LRESULT NtdllDefWindowProc_A(HWND hwnd, UINT message,
                                                   WPARAM wparam, LPARAM lparam);

/* ------------------------------------------------------------------ */
/* Window classes                                                      */
/* ------------------------------------------------------------------ */

/* A LARGE_STRING as NtUserCreateWindowEx/NtUserFindExistingCursorIcon receive
 * it: { ULONG Length; ULONG Flags; PVOID Buffer }. The kernel tests Length for
 * the string form and Buffer's low 16 bits for the atom form. */
typedef struct _SV_LARGE_STRING {
    ULONG        length;
    ULONG        flags;
    const WCHAR *buffer;
} SV_LARGE_STRING;

static int sv_wcslen(const WCHAR *text)
{
    int count = 0;
    if (!text)
        return 0;
    while (text[count])
        count++;
    return count;
}

__declspec(dllimport) UINT64 NtUserGetAncestor(HWND hwnd, UINT flags);

static void sv_user_record_window(HWND hwnd, const WCHAR *class_name,
                                  const WCHAR *window_name, DWORD style,
                                  DWORD ex_style, HWND parent, HMENU menu,
                                  HINSTANCE instance);

static void sv_user_record_class(const WNDCLASSEXW*, ATOM);
static HMENU sv_user_default_menu(const WCHAR*,HINSTANCE);
static void sv_user_attach_menu(HWND);
static void sv_user_forget_window(HWND);
static BOOL sv_user_menu_command(HWND,WPARAM,LPARAM);
USER32_API BOOL DrawMenuBar(HWND);

USER32_API ATOM RegisterClassExW(const WNDCLASSEXW *wndclass)
{
    if (!wndclass || !wndclass->lpfnWndProc || !wndclass->lpszClassName)
        return 0;
    /* The kernel reads the class name from a captured UNICODE_STRING and falls
     * back to the WNDCLASSEXW's lpszClassName; it needs both to be valid. */
    struct { USHORT length, maximum_length; const WCHAR *buffer; } name;
    int chars = sv_wcslen(wndclass->lpszClassName);
    name.length = (USHORT)(chars * sizeof(WCHAR));
    name.maximum_length = (USHORT)((chars + 1) * sizeof(WCHAR));
    name.buffer = wndclass->lpszClassName;
    /* WOW-compiled classes do not exist in this build: fnId and flags are zero
     * and the caller's WNDCLASSEXW is the only source of the class data. */
    ATOM atom=(ATOM)NtUserRegisterClassExWOW(wndclass, &name, NULL, 0, 0, 0);
    if(atom)sv_user_record_class(wndclass,atom);
    return atom;
}

USER32_API ATOM RegisterClassW(const WNDCLASSW *wndclass)
{
    if (!wndclass)
        return 0;
    /* WNDCLASS is WNDCLASSEX without the cbSize and hIconSm fields; the kernel
     * only ever sees the extended form. */
    WNDCLASSEXW extended;
    UINT8 *raw = (UINT8 *)&extended;
    for (UINT i = 0; i < sizeof(extended); i++)
        raw[i] = 0;
    extended.cbSize = sizeof(WNDCLASSEXW);
    extended.style = wndclass->style;
    extended.lpfnWndProc = wndclass->lpfnWndProc;
    extended.cbClsExtra = wndclass->cbClsExtra;
    extended.cbWndExtra = wndclass->cbWndExtra;
    extended.hInstance = wndclass->hInstance;
    extended.hIcon = wndclass->hIcon;
    extended.hCursor = wndclass->hCursor;
    extended.hbrBackground = wndclass->hbrBackground;
    extended.lpszMenuName = wndclass->lpszMenuName;
    extended.lpszClassName = wndclass->lpszClassName;
    return RegisterClassExW(&extended);
}

USER32_API BOOL UnregisterClassW(const WCHAR *class_name, HINSTANCE instance)
{
    if (!class_name)
        return FALSE;
    /* NtUserUnregisterClass reads the name through a pointer to the name
     * pointer (a[1] is PUNICODE_STRING in win32k, a name pointer here). */
    const WCHAR *name = class_name;
    return NtUserUnregisterClass(instance, &name, NULL) != 0;
}

/* ------------------------------------------------------------------ */
/* Windows                                                             */
/* ------------------------------------------------------------------ */

USER32_API HWND CreateWindowExW(DWORD ex_style, const WCHAR *class_name,
                                const WCHAR *window_name, DWORD style, int x, int y,
                                int width, int height, HWND parent, HMENU menu,
                                HINSTANCE instance, void *param)
{
    if (!class_name || !SvRegisterControl(class_name))
        return NULL;

    /* Preserve CW_USEDEFAULT for win32k. Its window manager resolves default
     * geometry and cascades successive top-level windows, keeping a newly
     * launched application from covering the one that was already open. */

    if(!menu&&!(style&(WS_CHILD|WS_POPUP)))menu=sv_user_default_menu(class_name,instance);

    SV_LARGE_STRING named_class;
    UINT64 class_arg;
    if (IS_INTRESOURCE(class_name)) {
        /* Atom form: the kernel reads the atom straight out of the argument. */
        class_arg = (UINT64)(UINT_PTR)class_name;
    } else {
        int chars = sv_wcslen(class_name);
        named_class.length = (ULONG)(chars * sizeof(WCHAR));
        named_class.flags = 0;
        named_class.buffer = class_name;
        class_arg = (UINT64)(UINT_PTR)&named_class;
    }

    /* The kernel runs WM_NCCREATE and WM_CREATE through a kernel-to-user
     * callback before this call returns, and answers with the HWND (or NULL
     * when the class could not be resolved or the callback was refused). The
     * window manager also owns the frame from here on: it decides the client
     * rectangle and sends WM_SIZE when the window is created or resized. */
    HWND hwnd = (HWND)(UINT_PTR)NtUserCreateWindowEx(
        ex_style, class_arg, 0, (UINT64)(UINT_PTR)window_name, style,
        x, y, width, height, parent, menu, instance, param, 0, 0, 0);
    if (hwnd)
        sv_user_record_window(hwnd, class_name, window_name, style, ex_style,
                              parent, menu, instance);
    if(hwnd&&menu&&!(style&WS_CHILD))sv_user_attach_menu(hwnd);
    return hwnd;
}

USER32_API HWND CreateWindowW(const WCHAR *class_name, const WCHAR *window_name,
                              DWORD style, int x, int y, int width, int height,
                              HWND parent, HMENU menu, HINSTANCE instance,
                              void *param)
{
    return CreateWindowExW(0, class_name, window_name, style, x, y, width, height,
                           parent, menu, instance, param);
}

USER32_API BOOL ShowWindow(HWND hwnd, int command)
{
    /* NtUserShowWindow flips WS_VISIBLE, invalidates the window (which is what
     * produces its first WM_PAINT) and repaints. It always answers 1: the
     * kernel does not remember the previous visibility, so the documented
     * "was previously visible" return value cannot be reported. */
    return NtUserShowWindow(hwnd, command) != 0;
}

USER32_API BOOL DestroyWindow(HWND hwnd)
{
    if (!hwnd)
        return FALSE;
    /* win32k delivers both teardown messages and destroys descendants. */
    BOOL destroyed=NtUserDestroyWindow(hwnd)!=0;
    if(destroyed)sv_user_forget_window(hwnd);
    return destroyed;
}

USER32_API BOOL SetWindowPos(HWND hwnd, HWND insert_after, int x, int y, int width,
                             int height, UINT flags)
{
    if (!hwnd)
        return FALSE;
    /* The window manager resizes the frame, moves the client rectangle and
     * generates WM_SIZE/WM_MOVE itself, so this layer only translates the call:
     * synthesizing WM_SIZE here as well would deliver it twice. */
    return NtUserSetWindowPos(hwnd, insert_after, x, y, width, height, flags) != 0;
}

USER32_API BOOL MoveWindow(HWND hwnd, int x, int y, int width, int height,
                           BOOL repaint)
{
    if (!hwnd)
        return FALSE;
    return NtUserMoveWindow(hwnd, x, y, width, height, repaint) != 0;
}

USER32_API BOOL SetWindowTextW(HWND hwnd, const WCHAR *text)
{
    if (!hwnd)
        return FALSE;
    /* The title is kernel-side state the frame painter draws, so the empty
     * string that SetWindowText(NULL) would mean is passed through as such. */
    return SendMessageW(hwnd, WM_SETTEXT, 0, (LPARAM)(text ? text : L"")) != 0;
}

USER32_API int GetWindowTextW(HWND hwnd, WCHAR *buffer, int capacity)
{
    if (!hwnd || !buffer || capacity <= 0)
        return 0;
    int copied = (int)SendMessageW(hwnd, WM_GETTEXT, (WPARAM)capacity, (LPARAM)buffer);
    /* Win32 guarantees a terminated buffer even when the title was truncated. */
    if (copied >= capacity)
        copied = capacity - 1;
    if (copied < 0)
        copied = 0;
    buffer[copied] = 0;
    return copied;
}

__declspec(dllimport) UINT64 SvNtUserGetDesktopWindow(void);

USER32_API HWND GetDesktopWindow(void)
{
    return (HWND)(UINT_PTR)SvNtUserGetDesktopWindow();
}

USER32_API HWND WindowFromPoint(POINT point)
{
    /* Screen coordinates, answered by the window manager's hit test. */
    return (HWND)(UINT_PTR)NtUserWindowFromPoint((int)point.x, (int)point.y);
}

/* ------------------------------------------------------------------ */
/* Geometry and metrics                                                */
/* ------------------------------------------------------------------ */

USER32_API BOOL GetClientRect(HWND hwnd, LPRECT rect)
{
    if (!rect)
        return FALSE;
    rect->left = 0;
    rect->top = 0;
    rect->right = 0;
    rect->bottom = 0;
    /* The window manager owns the frame, so it is the only component that knows
     * where the client area starts: this is its answer, in client coordinates. */
    return NtUserGetClientRect(hwnd, rect) != 0;
}

USER32_API BOOL GetWindowRect(HWND hwnd, LPRECT rect)
{
    if (!rect)
        return FALSE;
    rect->left = 0;
    rect->top = 0;
    rect->right = 0;
    rect->bottom = 0;
    /* Screen coordinates of the whole window, frame included. */
    return NtUserGetWindowRect(hwnd, rect) != 0;
}

/* win32k reads both queries from its shared SERVERINFO pages. The direct
 * user mapping still needs validation, so these calls use native services
 * rather than inventing fallback dimensions or colours in user32. */
__declspec(dllimport) UINT64 NtUserGetSystemMetrics(int index);
__declspec(dllimport) UINT64 NtUserGetSysColor(int index);
#define SV_SM_COUNT       0x61       /* GetSystemMetrics rejects >= 0x60 */
#define SV_SYSCOLOR_COUNT 31

USER32_API int GetSystemMetrics(int index)
{
    if (index < 0 || index >= SV_SM_COUNT)
        return 0;
    return (int)NtUserGetSystemMetrics(index);
}

USER32_API DWORD GetSysColor(int index)
{
    if (index < 0 || index >= SV_SYSCOLOR_COUNT)
        return 0;
    return (DWORD)NtUserGetSysColor(index);
}


#include "User32Message.inc"
#include "User32Input.inc"
#include "User32Paint.inc"
#include "User32Compat.inc"

USER32_API int GetWindowTextLengthW(HWND hwnd)
{
    return (int)SendMessageW(hwnd, WM_GETTEXTLENGTH, 0, 0);
}
USER32_API HWND GetParent(HWND hwnd)
{
    return (HWND)(UINT_PTR)NtUserGetAncestor(hwnd, 1);
}

__declspec(dllimport) UINT64 NtUserWindowRelation(HWND,UINT);
USER32_API HWND GetWindow(HWND hwnd,UINT command){return (HWND)(UINT_PTR)NtUserWindowRelation(hwnd,command);}
