/*
 * user/w/user32/User32.h - the USER surface Svindows implements.
 *
 * user32.dll is a thin, real Win32 layer over the NtUser* services
 * (kernel/win32k/UserWindows.inc, UserMessages.inc, UserCallbacks.inc). Where
 * the kernel has no service for a Win32 call, the implementation returns the
 * documented neutral value and says why in a comment; nothing here fakes a
 * feature the kernel cannot back.
 */
#ifndef SVINDOWS_USER32_H_
#define SVINDOWS_USER32_H_

/* wingdi.h first: the base Win32 types (HANDLE, RECT, POINT, COLORREF, ...)
 * and HDC/HBRUSH live there, exactly as in the Windows SDK. */
#include <Gdi32.h>

typedef unsigned long long UINT_PTR;
typedef long long          INT_PTR;
typedef UINT_PTR           WPARAM;
typedef INT_PTR            LPARAM;
typedef INT_PTR            LRESULT;

typedef void              *HWND;
typedef void              *HINSTANCE;
typedef void              *HMODULE;
typedef void              *HMENU;
typedef void              *HICON;
typedef void              *HCURSOR;
typedef void              *HACCEL;
typedef unsigned short     ATOM;

#ifdef SV_USER32_BUILD
#define USER32_API __declspec(dllexport)
#else
#define USER32_API __declspec(dllimport)
#endif

#define LOWORD(v) ((WORD)((UINT_PTR)(v) & 0xFFFF))
#define HIWORD(v) ((WORD)(((UINT_PTR)(v) >> 16) & 0xFFFF))
#define MAKELONG(a, b) ((LONG)(((WORD)(a)) | (((DWORD)((WORD)(b))) << 16)))

/* MAKEINTRESOURCE-style names: the low 16 bits are the resource id. */
#define IS_INTRESOURCE(p) (((UINT_PTR)(p) >> 16) == 0)
#define MAKEINTRESOURCEA(i) ((const char *)(UINT_PTR)(WORD)(i))
#define MAKEINTRESOURCEW(i) ((const WCHAR *)(UINT_PTR)(WORD)(i))

typedef LRESULT (*WNDPROC)(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);

/* MSG is 48 bytes on x64 and is laid out exactly like the kernel's
 * USER_MSG_LOCAL (kernel/win32k/UserMessages.inc), so NtUserGetMessage and
 * NtUserPeekMessage fill a client MSG in place. */
typedef struct _MSG {
    HWND    hwnd;
    UINT    message;
    WPARAM  wParam;
    LPARAM  lParam;
    DWORD   time;
    POINT   pt;
    DWORD   lPrivate;
} MSG;

typedef struct _WNDCLASSEXW {
    UINT      cbSize;
    UINT      style;
    WNDPROC   lpfnWndProc;
    int       cbClsExtra;
    int       cbWndExtra;
    HINSTANCE hInstance;
    HICON     hIcon;
    HCURSOR   hCursor;
    HBRUSH    hbrBackground;
    const WCHAR *lpszMenuName;
    const WCHAR *lpszClassName;
    HICON     hIconSm;
} WNDCLASSEXW;

typedef struct _WNDCLASSW {
    UINT      style;
    WNDPROC   lpfnWndProc;
    int       cbClsExtra;
    int       cbWndExtra;
    HINSTANCE hInstance;
    HICON     hIcon;
    HCURSOR   hCursor;
    HBRUSH    hbrBackground;
    const WCHAR *lpszMenuName;
    const WCHAR *lpszClassName;
} WNDCLASSW;

typedef struct _CREATESTRUCTW {
    void *lpCreateParams;
    HINSTANCE hInstance;
    HMENU hMenu;
    HWND hwndParent;
    int cy, cx, y, x;
    LONG style;
    const WCHAR *lpszName;
    const WCHAR *lpszClass;
    DWORD dwExStyle;
} CREATESTRUCTW;

/* PAINTSTRUCT: 72 bytes on x64, the size NtUserBeginPaint clears and fills. */
typedef struct _PAINTSTRUCT {
    HDC   hdc;
    BOOL  fErase;
    RECT  rcPaint;
    BOOL  fRestore;
    BOOL  fIncUpdate;
    BYTE  rgbReserved[32];
} PAINTSTRUCT;

typedef struct _TRACKMOUSEEVENT {
    DWORD cbSize, dwFlags;
    HWND hwndTrack;
    DWORD dwHoverTime;
} TRACKMOUSEEVENT;
#define TME_LEAVE 0x00000002u
#define TME_QUERY 0x40000000u
#define TME_CANCEL 0x80000000u
USER32_API BOOL TrackMouseEvent(TRACKMOUSEEVENT *event);
__declspec(dllimport) int SvGdiDrawTextW(HDC dc, const WCHAR *text,
                                          int count, LPRECT rect, UINT format);
/* Window styles. */
#define WS_OVERLAPPED   0x00000000u
#define WS_POPUP        0x80000000u
#define WS_CHILD        0x40000000u
#define WS_MINIMIZE     0x20000000u
#define WS_VISIBLE      0x10000000u
#define WS_DISABLED     0x08000000u
#define WS_CLIPSIBLINGS 0x04000000u
#define WS_CLIPCHILDREN 0x02000000u
#define WS_MAXIMIZE     0x01000000u
#define WS_CAPTION      0x00C00000u
#define WS_BORDER       0x00800000u
#define WS_DLGFRAME     0x00400000u
#define WS_VSCROLL      0x00200000u
#define WS_HSCROLL      0x00100000u
#define WS_SYSMENU      0x00080000u
#define WS_THICKFRAME   0x00040000u
#define WS_GROUP        0x00020000u
#define WS_MINIMIZEBOX  0x00020000u
#define WS_TABSTOP      0x00010000u
#define WS_MAXIMIZEBOX  0x00010000u
#define WS_OVERLAPPEDWINDOW (WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | \
                             WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX)
#define WS_TILEDWINDOW  WS_OVERLAPPEDWINDOW

#define WS_EX_DLGMODALFRAME 0x00000001u
#define WS_EX_CLIENTEDGE    0x00000200u
#define WS_EX_OVERLAPPEDWINDOW 0x00000300u
#define WS_EX_APPWINDOW     0x00040000u

#define CW_USEDEFAULT ((int)0x80000000)

/* ShowWindow commands. */
#define SW_HIDE            0
#define SW_SHOWNORMAL      1
#define SW_NORMAL          1
#define SW_SHOWMINIMIZED   2
#define SW_SHOWMAXIMIZED   3
#define SW_SHOWNOACTIVATE  4
#define SW_SHOW            5
#define SW_MINIMIZE        6
#define SW_SHOWMINNOACTIVE 7
#define SW_SHOWNA          8
#define SW_RESTORE         9
#define SW_SHOWDEFAULT     10

/* SetWindowPos flags (the kernel interprets these directly). */
#define SWP_NOSIZE      0x0001u
#define SWP_NOMOVE      0x0002u
#define SWP_NOZORDER    0x0004u
#define SWP_NOREDRAW    0x0008u
#define SWP_NOACTIVATE  0x0010u
#define SWP_FRAMECHANGED 0x0020u
#define SWP_SHOWWINDOW  0x0040u
#define SWP_HIDEWINDOW  0x0080u

/* PeekMessage flags. */
#define PM_NOREMOVE 0x0000u
#define PM_REMOVE   0x0001u
#define PM_NOYIELD  0x0002u

/* Messages used by the SVINDOWS client layer. */
#define WM_NULL        0x0000u
#define WM_CREATE      0x0001u
#define WM_DESTROY     0x0002u
#define WM_MOVE        0x0003u
#define WM_ENABLE      0x000Au
#define WM_SETFONT     0x0030u
#define WM_GETFONT     0x0031u
#define WM_COMMAND     0x0111u
#define WM_MOUSELEAVE  0x02A3u
#define WM_CAPTURECHANGED 0x0215u
#define WM_SIZE        0x0005u
#define WM_ACTIVATE    0x0006u
#define WM_SETFOCUS    0x0007u
#define WM_KILLFOCUS   0x0008u
#define WM_SETTEXT     0x000Cu
#define WM_GETTEXT     0x000Du
#define WM_GETTEXTLENGTH 0x000Eu
#define WM_PAINT       0x000Fu
#define WM_CTLCOLORSTATIC 0x0138u
#define WM_CLOSE       0x0010u
#define WM_QUIT        0x0012u
#define WM_ERASEBKGND  0x0014u
#define WM_SHOWWINDOW  0x0018u
#define WM_SETCURSOR   0x0020u
#define WM_GETMINMAXINFO 0x0024u
#define WM_NCCREATE    0x0081u
#define WM_NCDESTROY   0x0082u
#define WM_NCCALCSIZE  0x0083u
#define WM_NCHITTEST   0x0084u
#define WM_KEYDOWN     0x0100u
#define WM_KEYUP       0x0101u
#define WM_CHAR        0x0102u
#define WM_SYSKEYDOWN  0x0104u
#define WM_SYSKEYUP    0x0105u
#define WM_SYSCOMMAND  0x0112u
#define SC_CLOSE       0xF060u
#define WM_TIMER       0x0113u
#define WM_MOUSEMOVE   0x0200u
#define WM_LBUTTONDOWN 0x0201u
#define WM_LBUTTONUP   0x0202u
#define WM_LBUTTONDBLCLK 0x0203u
#define WM_RBUTTONDOWN 0x0204u
#define WM_RBUTTONUP   0x0205u

#define SIZE_RESTORED  0
#define SIZE_MINIMIZED 1
#define SIZE_MAXIMIZED 2

/* Virtual key codes used by the client layer. */
#define VK_BACK   0x08
#define VK_TAB    0x09
#define VK_RETURN 0x0D
#define VK_SHIFT  0x10
#define VK_CONTROL 0x11
#define VK_MENU   0x12
#define VK_ESCAPE 0x1B
#define VK_SPACE  0x20
#define VK_HOME   0x24
#define VK_END    0x23
#define VK_LEFT   0x25
#define VK_UP     0x26
#define VK_RIGHT  0x27
#define VK_DOWN   0x28
#define VK_DELETE 0x2E

/* Standard controls. The application supplies content and notifications;
 * class procedures in user32 own input, focus and painting. */
#define BS_PUSHBUTTON    0x0000u
#define BS_DEFPUSHBUTTON 0x0001u
#define BS_CHECKBOX      0x0002u
#define BS_AUTOCHECKBOX  0x0003u
#define BS_RADIOBUTTON   0x0004u
#define BS_GROUPBOX      0x0007u
#define BS_AUTORADIOBUTTON 0x0009u
#define BS_OWNERDRAW     0x000Bu
#define BS_NOTIFY        0x4000u
#define BS_TYPEMASK      0x000Fu
#define BM_GETCHECK      0x00F0u
#define BM_SETCHECK      0x00F1u
#define BM_GETSTATE      0x00F2u
#define BM_SETSTATE      0x00F3u
#define BM_CLICK         0x00F5u
#define BST_UNCHECKED    0
#define BST_CHECKED      1
#define BST_PUSHED       4
#define BST_FOCUS        8
#define BN_CLICKED       0
#define BN_SETFOCUS      6
#define BN_KILLFOCUS     7
#define ES_LEFT          0x0000u
#define ES_CENTER        0x0001u
#define ES_RIGHT         0x0002u
#define ES_MULTILINE     0x0004u
#define ES_PASSWORD      0x0020u
#define ES_AUTOVSCROLL   0x0040u
#define ES_AUTOHSCROLL   0x0080u
#define ES_READONLY      0x0800u
#define ES_WANTRETURN    0x1000u
#define EM_GETSEL        0x00B0u
#define EM_SETSEL        0x00B1u
#define EM_GETMODIFY     0x00B8u
#define EM_SETMODIFY     0x00B9u
#define EM_GETLINECOUNT  0x00BAu
#define EM_LINEINDEX     0x00BBu
#define EM_SETHANDLE     0x00BCu
#define EM_GETHANDLE     0x00BDu
#define EM_LINELENGTH    0x00C1u
#define EM_REPLACESEL    0x00C2u
#define EM_LIMITTEXT     0x00C5u
#define EM_SETLIMITTEXT  EM_LIMITTEXT
#define EM_LINEFROMCHAR  0x00C9u
#define EM_GETFIRSTVISIBLELINE 0x00CEu
#define EM_SETREADONLY   0x00CFu
#define EM_GETLIMITTEXT  0x00D5u
#define EN_SETFOCUS      0x0100u
#define EN_KILLFOCUS     0x0200u
#define EN_CHANGE        0x0300u
#define EN_UPDATE        0x0400u
#define EN_MAXTEXT       0x0501u
#define SS_LEFT          0x0000u
#define SS_CENTER        0x0001u
#define SS_RIGHT         0x0002u
#define SS_SIMPLE        0x000Bu
#define SS_LEFTNOWORDWRAP 0x000Cu
#define SS_BITMAP         0x000Eu
#define SS_ENHMETAFILE    0x000Fu
#define SS_NOPREFIX      0x0080u

/* MessageBox types and results. */
#define MB_OK               0x00000000u
#define MB_ICONHAND         0x00000010u
#define MB_ICONERROR        0x00000010u
#define MB_ICONQUESTION     0x00000020u
#define MB_ICONEXCLAMATION  0x00000030u
#define MB_ICONWARNING      0x00000030u
#define MB_ICONASTERISK     0x00000040u
#define MB_ICONINFORMATION  0x00000040u
#define IDOK        1
#define IDCANCEL    2
#define IDABORT     3
#define IDRETRY     4
#define IDIGNORE    5
#define IDYES       6
#define IDNO        7

/* System metrics. The kernel fills the whole array (SERVERINFO + 0x758);
 * GetSystemMetrics rejects any index at or above SM_CMETRICS. */
#define SM_CXSCREEN   0
#define SM_CYSCREEN   1
#define SM_CXVSCROLL  2
#define SM_CYHSCROLL  3
#define SM_CYCAPTION  4
#define SM_CXBORDER   5
#define SM_CYBORDER   6
#define SM_CXDLGFRAME 7
#define SM_CYDLGFRAME 8
#define SM_CYVTHUMB   9
#define SM_CXHTHUMB   10
#define SM_CXICON     11
#define SM_CYICON     12
#define SM_CXCURSOR   13
#define SM_CYCURSOR   14
#define SM_CYMENU     15
#define SM_CXFULLSCREEN 16
#define SM_CYFULLSCREEN 17
#define SM_MOUSEPRESENT 19
#define SM_CYVSCROLL  20
#define SM_CXHSCROLL  21
#define SM_CXMIN      28
#define SM_CYMIN      29
#define SM_CXSIZE     30
#define SM_CYSIZE     31
#define SM_CXFRAME    32
#define SM_CYFRAME    33
#define SM_CXMINTRACK 34
#define SM_CYMINTRACK 35
#define SM_CMOUSEBUTTONS 43
#define SM_CXEDGE     45
#define SM_CYEDGE     46
#define SM_CXSMICON   49
#define SM_CYSMICON   50
#define SM_CYSMCAPTION 51
#define SM_CXMINIMIZED 57
#define SM_CYMINIMIZED 58
#define SM_CXMAXTRACK 59
#define SM_CYMAXTRACK 60
#define SM_CXMAXIMIZED 61
#define SM_CYMAXIMIZED 62
#define SM_CMONITORS  80
#define SM_CMETRICS   0x61

/* System colours (the SERVERINFO colour array index space). */
#define COLOR_SCROLLBAR     0
#define COLOR_BACKGROUND    1
#define COLOR_DESKTOP       COLOR_BACKGROUND
#define COLOR_ACTIVECAPTION 2
#define COLOR_INACTIVECAPTION 3
#define COLOR_MENU          4
#define COLOR_WINDOW        5
#define COLOR_WINDOWFRAME   6
#define COLOR_MENUTEXT      7
#define COLOR_WINDOWTEXT    8
#define COLOR_CAPTIONTEXT   9
#define COLOR_ACTIVEBORDER  10
#define COLOR_INACTIVEBORDER 11
#define COLOR_APPWORKSPACE  12
#define COLOR_HIGHLIGHT     13
#define COLOR_HIGHLIGHTTEXT 14
#define COLOR_BTNFACE       15
#define COLOR_3DFACE        COLOR_BTNFACE
#define COLOR_BTNSHADOW     16
#define COLOR_3DSHADOW      COLOR_BTNSHADOW
#define COLOR_GRAYTEXT      17
#define COLOR_BTNTEXT       18
#define COLOR_INACTIVECAPTIONTEXT 19
#define COLOR_BTNHIGHLIGHT  20
#define COLOR_3DHIGHLIGHT   COLOR_BTNHIGHLIGHT
#define COLOR_3DDKSHADOW    21
#define COLOR_3DLIGHT       22
#define GCLP_HBRBACKGROUND  (-10)
#define COLOR_INFOTEXT      23
#define COLOR_INFOBK        24
#define COLOR_HOTLIGHT      26

/* Stock cursors. */
#define IDC_ARROW       MAKEINTRESOURCEW(32512)
#define IDC_IBEAM       MAKEINTRESOURCEW(32513)
#define IDC_WAIT        MAKEINTRESOURCEW(32514)
#define IDC_CROSS       MAKEINTRESOURCEW(32515)
#define IDC_UPARROW     MAKEINTRESOURCEW(32516)
#define IDC_SIZENWSE    MAKEINTRESOURCEW(32642)
#define IDC_SIZENESW    MAKEINTRESOURCEW(32643)
#define IDC_SIZEWE      MAKEINTRESOURCEW(32644)
#define IDC_SIZENS      MAKEINTRESOURCEW(32645)
#define IDC_SIZEALL     MAKEINTRESOURCEW(32646)
#define IDC_NO          MAKEINTRESOURCEW(32648)
#define IDC_HAND        MAKEINTRESOURCEW(32649)
#define IDC_APPSTARTING MAKEINTRESOURCEW(32650)
#define IDC_HELP        MAKEINTRESOURCEW(32651)

/* Window classes and windows. */
USER32_API ATOM      RegisterClassExW(const WNDCLASSEXW *wndclass);
USER32_API ATOM      RegisterClassW(const WNDCLASSW *wndclass);
USER32_API BOOL      UnregisterClassW(const WCHAR *class_name, HINSTANCE instance);
USER32_API LONG_PTR  GetClassLongPtrW(HWND hwnd, int index);
USER32_API HWND      CreateWindowExW(DWORD ex_style, const WCHAR *class_name,
                                     const WCHAR *window_name, DWORD style,
                                     int x, int y, int width, int height,
                                     HWND parent, HMENU menu, HINSTANCE instance,
                                     void *param);
USER32_API HWND      CreateWindowW(const WCHAR *class_name, const WCHAR *window_name,
                                   DWORD style, int x, int y, int width, int height,
                                   HWND parent, HMENU menu, HINSTANCE instance,
                                   void *param);
USER32_API BOOL      DestroyWindow(HWND hwnd);
USER32_API BOOL      ShowWindow(HWND hwnd, int command);
USER32_API BOOL      UpdateWindow(HWND hwnd);
USER32_API BOOL      SetWindowTextW(HWND hwnd, const WCHAR *text);
USER32_API int       GetWindowTextW(HWND hwnd, WCHAR *buffer, int capacity);
USER32_API HWND      WindowFromPoint(POINT point);
USER32_API HWND      GetParent(HWND hwnd);
USER32_API HWND      GetWindow(HWND hwnd, UINT command);
USER32_API int       GetWindowTextLengthW(HWND hwnd);
USER32_API HWND      GetForegroundWindow(void);
USER32_API LRESULT   DefWindowProcW(HWND hwnd, UINT message, WPARAM wparam,
                                    LPARAM lparam);
USER32_API LRESULT   DefWindowProcA(HWND hwnd, UINT message, WPARAM wparam,
                                    LPARAM lparam);

/* Messages. */
USER32_API BOOL      GetMessageW(MSG *message, HWND hwnd, UINT first, UINT last);
USER32_API BOOL      PeekMessageW(MSG *message, HWND hwnd, UINT first, UINT last,
                                  UINT remove);
USER32_API BOOL      TranslateMessage(const MSG *message);
USER32_API LRESULT   DispatchMessageW(const MSG *message);
USER32_API BOOL      PostMessageW(HWND hwnd, UINT message, WPARAM wparam,
                                  LPARAM lparam);
USER32_API LRESULT   SendMessageW(HWND hwnd, UINT message, WPARAM wparam,
                                  LPARAM lparam);
USER32_API void      PostQuitMessage(int exit_code);

/* Painting and geometry. */
USER32_API HDC       BeginPaint(HWND hwnd, PAINTSTRUCT *paint);
USER32_API BOOL      EndPaint(HWND hwnd, const PAINTSTRUCT *paint);
USER32_API BOOL      InvalidateRect(HWND hwnd, const RECT *rect, BOOL erase);
USER32_API BOOL      ValidateRect(HWND hwnd, const RECT *rect);
USER32_API HDC       GetDC(HWND hwnd);
USER32_API int       ReleaseDC(HWND hwnd, HDC hdc);
USER32_API BOOL      GetClientRect(HWND hwnd, LPRECT rect);
USER32_API BOOL      GetWindowRect(HWND hwnd, LPRECT rect);
USER32_API BOOL      SetWindowPos(HWND hwnd, HWND insert_after, int x, int y,
                                  int width, int height, UINT flags);
USER32_API BOOL      MoveWindow(HWND hwnd, int x, int y, int width, int height,
                                BOOL repaint);
USER32_API int       GetSystemMetrics(int index);
USER32_API DWORD     GetSysColor(int index);

/* Input, cursor and focus. */
USER32_API HCURSOR   LoadCursorW(HINSTANCE instance, const WCHAR *name);
USER32_API HCURSOR   SetCursor(HCURSOR cursor);
USER32_API HWND      SetFocus(HWND hwnd);
USER32_API HWND      GetFocus(void);
USER32_API HWND      SetActiveWindow(HWND hwnd);
USER32_API HWND      GetActiveWindow(void);
USER32_API HWND      SetCapture(HWND hwnd);
USER32_API BOOL      ReleaseCapture(void);
USER32_API short     GetKeyState(int key);

USER32_API int       MessageBoxW(HWND hwnd, const WCHAR *text, const WCHAR *caption,
                                 UINT type);

/* Forwarders to kernel32, which owns the loader and resource metadata. */
USER32_API HMODULE   GetModuleHandleW(const WCHAR *name);
USER32_API int       LoadStringW(HINSTANCE instance, UINT id, WCHAR *buffer,
                                 int capacity);

#endif /* SVINDOWS_USER32_H_ */
