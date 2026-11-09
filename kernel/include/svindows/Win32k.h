/*
 * svindows/Win32k.h - the win32k side of the USER/GDI subsystem.
 *
 * Two pieces of shared memory and a GDI object table are what make the genuine
 * user32/gdi32 work against this kernel:
 *
 *  - SERVERINFO ("gpsi"): read-only, reached through SHAREDINFO.psi. USER32
 *    answers GetSystemMetrics and GetSysColor out of it with no syscall.
 *  - The GDI handle table at PEB->GdiSharedHandleTable: one 24-byte GDICELL64
 *    per object. GDI32 validates every handle against it before use.
 *  - Per-object attribute blocks (DC_ATTR): user-writable, pointed at by a
 *    cell's pUserInfo. GDI32 caches DC state there, so SetTextColor and
 *    SetBkColor never enter the kernel and win32k has to read the current
 *    colours back out of the block when it draws.
 */
#ifndef _SVINDOWS_WIN32K_H_
#define _SVINDOWS_WIN32K_H_

#include <nt/Ntdef.h>

/*
 * Build and publish the SERVERINFO page at PROCESS_SERVER_INFO_VA. Requires Mm
 * and the framebuffer (screen metrics come from it), and must run before the
 * user process starts, because USER32 reads the page during its process
 * attach.
 */
void W32kInitializeServerInfo(void);

/* Map the shared SERVERINFO page read-only into the process being created.
 * Called with the new address space active; without it GetSystemMetrics and
 * GetSysColor read a zeroed page and answer nonsense. */
BOOLEAN W32kMapServerInfo(void);
UINT64 NtUserGetSystemMetrics(UINT64 *args);
UINT64 NtUserGetSysColor(UINT64 *args);

/* Zero only the new process's mapped client table, with its CR3 active.
 * Global kernel object ownership is preserved. */
void W32kInitializeGdi(void);
struct _KPROCESS;
struct _KTHREAD;
void W32kCleanupProcessGdi(struct _KPROCESS *process);
void W32kCleanupProcessUser(struct _KPROCESS *process);
void W32kCleanupThreadUser(struct _KTHREAD *thread);
void W32kCleanupWindowDc(struct _KPROCESS *process, UINT64 hwnd);
BOOLEAN W32kClearSharedHandle(struct _KPROCESS *process, BOOLEAN gdi, UINT16 index);

/*
 * Create a device context over a retained window client raster, or the screen
 * for hwnd == 0. Window coordinates remain local across movement and drawing
 * into covered regions is retained for later composition.
 */
UINT64 W32kCreateWindowDc(UINT64 hwnd, INT32 x, INT32 y, INT32 width,
                          INT32 height);

/* Release a device context created for a window. */
void W32kReleaseDc(UINT64 hdc);

/* Hand out the cached device context for a window, creating it on first use and
 * refreshing its geometry on every call. */
UINT64 W32kAcquireWindowDc(UINT64 hwnd, INT32 x, INT32 y, INT32 width,
                           INT32 height);

/* Drop a window's cached device context (on window destruction). */
void W32kDestroyWindowDc(UINT64 hwnd);

/*
 * The window manager (win32k/UserScreen.inc).
 *
 * It owns the desktop backdrop, the z-order, the Windows 10 non-client frame,
 * the pointer and input routing. Window geometry lives there and nowhere else;
 * the services in UserWindows.inc forward geometry changes into it and ask it
 * for rectangles instead of keeping a second copy.
 */

/* Windows 10 frame metrics at 100% scaling, used by sizing and painting. */
#define W32K_CAPTION_HEIGHT 30
#define W32K_BUTTON_WIDTH   45
#define W32K_BORDER         1
#define W32K_TITLE_CHARS    80

#define W32K_KIND_NONE     0
#define W32K_KIND_TOPLEVEL 1
#define W32K_KIND_DESKTOP  2  /* WorkerW/Progman: the wallpaper layer      */
#define W32K_KIND_CHILD    4
#define W32K_KIND_TASKBAR  3  /* Shell_TrayWnd: an always-on-top strip     */

void W32kScreenCompose(void);
void W32kScreenDamage(INT32 x, INT32 y, INT32 width, INT32 height);
BOOLEAN W32kBeginWindowPaint(UINT64 hdc, UINT64 hwnd);
BOOLEAN W32kEndWindowPaint(UINT64 hdc, UINT64 hwnd);
void W32kCopyWindowClient(UINT16 slot, UINT32 *target, UINT32 stride,
                          INT32 x, INT32 y, INT32 width, INT32 height);
BOOLEAN W32kScreenPixelVisible(UINT64 hwnd, INT32 x, INT32 y);
BOOLEAN W32kScreenRectUnobscured(UINT64 hwnd, INT32 x, INT32 y,
                                 INT32 width, INT32 height);
void W32kSetFocus(UINT64 hwnd);

/* The window manager classifies a window from its class name and style:
 * WorkerW/Progman become the wallpaper layer, Shell_TrayWnd a topmost strip. */
UINT8 W32kClassifyWindow(const UINT16 *class_name, UINT32 style);
void W32kScreenWindowCreated(UINT16 slot, const UINT16 *title, UINT32 style,
                             INT32 x, INT32 y, UINT32 w, UINT32 h, UINT8 kind);
void W32kScreenWindowDestroyed(UINT16 slot);
void W32kScreenSetParent(UINT16 slot, UINT16 parent);
void W32kScreenSetStatic(UINT16 slot, BOOLEAN is_static);
UINT16 W32kWindowParent(UINT16 slot);
UINT16 W32kRootSlot(UINT16 slot);
void W32kScreenSetTitle(UINT16 slot, const UINT16 *title);
void W32kSetWindowText(UINT64 hwnd, const UINT16 *text);
void W32kShowWindow(UINT64 hwnd, UINT32 command);
void W32kMoveWindow(UINT64 hwnd, INT32 x, INT32 y, UINT32 width, UINT32 height);
void W32kRaiseSlot(UINT16 slot);
void W32kSetMenuBar(UINT16 slot, const UINT16 *labels, UINT32 count, BOOLEAN present);
void W32kSetZOrder(UINT16 slot, UINT64 after);
void W32kInvalidateFrame(UINT16 slot);

BOOLEAN W32kWindowExists(UINT16 slot);
BOOLEAN W32kWindowRect(UINT16 slot, INT32 *x, INT32 *y, UINT32 *width,
                       UINT32 *height);
void W32kClientOrigin(UINT16 slot, INT32 *x, INT32 *y, INT32 *width,
                      INT32 *height);
UINT16 W32kActiveSlot(void);
BOOLEAN W32kGetRect(UINT64 hwnd, INT32 *rect, BOOLEAN client);

/* Input arrives from the HAL through the kernel input worker. */
void W32kInputMouse(INT32 x, INT32 y, UINT8 buttons);
void W32kInputKey(UINT16 event, UINT16 character);
void W32kXorgWorker(PVOID context);
void W32kPostCharacter(UINT16 character);
/* TRUE when the focused window manager should receive keyboard events. */
BOOLEAN W32kKeyboardOwner(void);
BOOLEAN W32kXorgInputActive(void);
BOOLEAN W32kXorgConnected(void);

/* The win32k system services, dispatched from KiServiceTable. */
UINT64 NtGdiGetEntry(UINT64 *args);
UINT64 NtGdiCreateSolidBrush(UINT64 *args);
UINT64 NtGdiSelectBrush(UINT64 *args);
UINT64 NtGdiSelectFont(UINT64 *args);
UINT64 NtGdiSelectPen(UINT64 *args);
UINT64 NtGdiSelectBitmap(UINT64 *args);
UINT64 NtGdiDeleteObjectApp(UINT64 *args);
UINT64 NtGdiPatBlt(UINT64 *args);
UINT64 NtGdiBitBlt(UINT64 *args);
UINT64 NtGdiBlitUserBuffer(UINT64 *args);
UINT64 NtGdiReadUserBuffer(UINT64 *args);
UINT64 NtGdiCreatePatternBrush(UINT64 *args);
UINT64 NtGdiExtTextOutW(UINT64 *args);
UINT64 NtGdiGetTextMetricsW(UINT64 *args);
UINT64 NtGdiGetTextExtent(UINT64 *args);
UINT64 NtGdiGetTextExtentExW(UINT64 *args);
UINT64 NtGdiSetPixel(UINT64 *args);
UINT64 NtGdiRectangle(UINT64 *args);
UINT64 NtGdiMoveTo(UINT64 *args);
UINT64 NtGdiLineTo(UINT64 *args);
UINT64 NtGdiCreateCompatibleDC(UINT64 *args);
UINT64 NtGdiCreateCompatibleBitmap(UINT64 *args);
UINT64 NtGdiCreateBitmap(UINT64 *args);
UINT64 NtGdiGetDCObject(UINT64 *args);
UINT64 NtGdiFlush(UINT64 *args);
UINT64 NtGdiHfontCreate(UINT64 *args);
UINT64 NtGdiStretchBlt(UINT64 *args);
UINT64 NtGdiCreateRectRgn(UINT64 *args);
UINT64 NtGdiCreatePen(UINT64 *args);
UINT64 NtGdiGetDeviceCaps(UINT64 *args);
UINT64 NtGdiGetCurrentDpiInfo(UINT64 *args);

void W32kActivateWindow(UINT64 hwnd);

#endif /* _SVINDOWS_WIN32K_H_ */
