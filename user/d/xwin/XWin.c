/* Win16 side of Svindows' seamless X11 bridge. Build with Open Watcom C/16. */
#include <windows.h>
#include <string.h>

#pragma pack(push, 1)
typedef struct {
    WORD op, hwnd;
    short x, y;
    WORD width, height;
    char title[64];
} PACKET;
#pragma pack(pop)

static void bridge(PACKET far *p);
#pragma aux bridge = "push ax"                                                                     \
                     "push bx"                                                                     \
                     "mov ax,5658h"                                                                \
                     "mov bx,5842h"                                                                \
                     "int 2fh"                                                                     \
                     "pop bx"                                                                      \
                     "pop ax" parm[es di] modify exact[];

static HWND windows[64];
static BOOL full_capture[64];
static PACKET packet;
static HWND helper;
static BOOL busy;
static void keep_on_guest_screen(HWND w, RECT far *rect) {
    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    int width = rect->right - rect->left, height = rect->bottom - rect->top;
    int x = rect->left, y = rect->top;
    if (width > sw)
        width = sw;
    if (height > sh)
        height = sh;
    if (x + width > sw)
        x = sw - width;
    if (y + height > sh)
        y = sh - height;
    if (x < 0)
        x = 0;
    if (y < 0)
        y = 0;
    if (x != rect->left || y != rect->top || width != rect->right - rect->left ||
        height != rect->bottom - rect->top) {
        SetWindowPos(w, NULL, x, y, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
        GetWindowRect(w, rect);
    }
}

static void capture_window(HWND w, BOOL full, HWND active) {
    HWND previous, above;
    unsigned clips = 0;
    RECT rect;
    if (!IsWindow(w) || !IsWindowVisible(w))
        return;
    previous = GetWindow(w, GW_HWNDPREV);
    /* Filling a newly allocated backing image once may require an activation
     * on Win3: its active window can remain above a NOACTIVATE raise. Avoid
     * doing this continuously, or while an application owns mouse capture. */
    if (full && !GetCapture() && !IsIconic(w)) {
        BringWindowToTop(w);
        SetActiveWindow(w);
    }
    if (full) {
        SetWindowPos(w, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        RedrawWindow(w, NULL, NULL,
                     RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN | RDW_UPDATENOW);
    } else {
        /* Flush existing damage without making every application repaint its
         * entire window at 10 Hz or changing the guest's window stacking. */
        RedrawWindow(w, NULL, NULL, RDW_ALLCHILDREN | RDW_UPDATENOW);
    }
    if (full && active != w)
        SendMessage(w, WM_NCACTIVATE, FALSE, 0L);
    packet.op = 10;
    packet.hwnd = (WORD)w;
    bridge(&packet);
    /* An owned modal popup may stay above its owner even after HWND_TOP.
     * Keep the owner's last pixels there instead of capturing the popup. */
    above = GetWindow(w, GW_HWNDPREV);
    while (above && clips++ < 64) {
        if (IsWindowVisible(above)) {
            GetWindowRect(above, &rect);
            packet.op = 9;
            packet.hwnd = (WORD)w;
            packet.x = rect.left;
            packet.y = rect.top;
            packet.width = rect.right - rect.left;
            packet.height = rect.bottom - rect.top;
            bridge(&packet);
        }
        above = GetWindow(above, GW_HWNDPREV);
    }
    packet.op = 7;
    packet.hwnd = (WORD)w;
    bridge(&packet);
    if (full && (!previous || IsWindow(previous)))
        SetWindowPos(w, previous ? previous : HWND_TOP, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

static void sync_windows(void) {
    HWND w, active;
    unsigned count = 0, i, total, cursor_hides = 0;
    BOOL restored = FALSE;
    if (busy)
        return;
    busy = TRUE;
    /* Each next bridge call acknowledges the preceding activation action. */
    for (i = 0; i < 64; i++) {
        packet.op = 4;
        bridge(&packet);
        if (!packet.op)
            break;
        w = (HWND)packet.hwnd;
        if (!IsWindow(w) || w == helper)
            continue;
        switch (packet.op) {
        case 1:
            if (GetActiveWindow() != w) {
                SetWindowPos(w, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
                SetActiveWindow(w);
            }
            break;
        case 2:
            BringWindowToTop(w);
            SetActiveWindow(w);
            PostMessage(w, WM_SYSCOMMAND, SC_CLOSE, 0L);
            break;
        case 3:
            if (packet.width && packet.height) {
                int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
                SetWindowPos(w, NULL, 0, 0, packet.width > sw ? sw : packet.width,
                             packet.height > sh ? sh : packet.height,
                             SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            }
            break;
        }
    }
    w = GetWindow(GetDesktopWindow(), GW_CHILD);
    while (w && count < 64) {
        if (w != helper && IsWindowVisible(w))
            windows[count++] = w;
        w = GetWindow(w, GW_HWNDNEXT);
    }
    total = count;
    packet.op = 1;
    bridge(&packet);
    /* Publish the real stacking first, before temporary paint-only raises. */
    while (count) {
        RECT rect;
        w = windows[--count];
        GetWindowRect(w, &rect);
        keep_on_guest_screen(w, &rect);
        memset(&packet, 0, sizeof(packet));
        packet.op = 2;
        packet.hwnd = (WORD)w;
        packet.x = rect.left;
        packet.y = rect.top;
        packet.width = rect.right - rect.left;
        packet.height = rect.bottom - rect.top;
        GetWindowText(w, packet.title, sizeof(packet.title));
        bridge(&packet);
        full_capture[count] = packet.op == 2;
    }
    active = GetActiveWindow();
    packet.op = 8;
    packet.hwnd = 1;
    bridge(&packet);
    do {
        cursor_hides++;
    } while (ShowCursor(FALSE) >= 0 && cursor_hides < 16);
    for (i = total; i; i--) {
        capture_window(windows[i - 1], full_capture[i - 1], active);
        if (full_capture[i - 1])
            restored = TRUE;
    }
    if (restored && active && IsWindow(active)) {
        BringWindowToTop(active);
        SetActiveWindow(active);
    }
    while (cursor_hides--)
        ShowCursor(TRUE);
    packet.op = 8;
    packet.hwnd = 0;
    bridge(&packet);
    packet.op = 3;
    bridge(&packet);
    busy = FALSE;
}

LONG FAR PASCAL __export WindowProc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(w, msg, wp, lp);
}

int PASCAL WinMain(HINSTANCE instance, HINSTANCE previous, LPSTR command, int show) {
    WNDCLASS wc;
    MSG msg;
    DWORD last = (DWORD)-100L;
    (void)command;
    (void)show;
    memset(&packet, 0, sizeof(packet));
    bridge(&packet);
    if (packet.op != 0x0100)
        return 0; /* Native fullscreen Win3 needs no agent. */
    if (!previous) {
        memset(&wc, 0, sizeof(wc));
        wc.lpfnWndProc = (WNDPROC)MakeProcInstance((FARPROC)WindowProc, instance);
        wc.hInstance = instance;
        wc.lpszClassName = "SvXWinAgent";
        if (!RegisterClass(&wc))
            return 1;
    }
    helper = CreateWindow("SvXWinAgent", "Svindows X11 agent", WS_OVERLAPPED, 0, 0, 0, 0, NULL,
                          NULL, instance, NULL);
    if (!helper)
        return 1;
    /* Program Manager remains an ordinary exported window and launches apps. */
    /* PeekMessage plus Yield keeps this background task cooperative even
     * while another startup task has a modal dialog. */
    for (;;) {
        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT)
                return (int)msg.wParam;
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        if ((DWORD)(GetTickCount() - last) >= 100L) {
            last = GetTickCount();
            sync_windows();
        }
        Yield();
    }
}
