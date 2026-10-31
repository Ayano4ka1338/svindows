# Win32 client layer and standard controls

Svindows implements its own Win32 client DLLs. Applications create controls as
child windows with the standard `CreateWindowExW` API and exchange `WM_*`,
`BM_*` and `EM_*` messages. This is an implemented subset, not full Windows 10
binary or visual compatibility.

## Ownership

| Component | Responsibility |
| --- | --- |
| `kernel/win32k/UserWindows.inc`, `UserMessages.inc` | HWND ownership/generations, class registration, thread queues, update regions |
| `UserCallbacks.inc` | synchronous window procedure callbacks, nested creation/messages/destruction |
| `UserScreen.inc`, `UserChildren.inc`, `ScreenRaster.inc` | screen geometry, child clipping, hit tests, activation/focus/capture, retained composition and cursor |
| `kernel/win32k/gdi/WindowSurfaces.inc` | independent drawing/completed client rasters and atomic paint publication |
| `user/w/ntdll/Ntdll.asm` | native syscall entry points |
| `user/w/user32/User32.c`, `User32*.inc` | public window, message, paint and input APIs |
| `user/w/user32/controls/Controls.c` | built-in classes, per-HWND state, text/font messages and reentrant lifetime |
| `controls/Button.c`, `Edit.c`, `Static.c` | control behavior and painting |
| `controls/EditModel.c` | UTF-16 storage, selection and editing, independent of HWND and rendering |
| `user/w/gdi32/` | DCs, GDI objects, text, fills and blits |
| `user/w/uxtheme/` | shared Windows 10-style control colors, borders and visual states |
| `user/w/comctl32/` | common-control class registration, progress, status, trackbar, tab, list and toolbar controls |
| `user/w/notepad/Notepad.c` | application layout and an ordinary child `EDIT` |

The kernel does not implement button or editor behavior. A control is a real
`WS_CHILD` HWND with a user-mode window procedure, not a rectangle drawn inside
an application's private widget tree. `hMenu` supplies the control ID. Mouse
coordinates are relative to that child's client area; keyboard input follows
focus, independently of mouse capture. Activation belongs to the root window.

Each child retains its own completed GDI raster. Composition clips it against
all ancestor clients. Moving an ancestor changes screen coordinates without
asking children to paint. Shadow pixels blend with the scene rather than
replacing it with opaque bands. Composition/publication are protected against
UP input-worker preemption.

## Supported controls

`BUTTON`: push/default push buttons, checkbox/autocheckbox, radio/autoradio,
and group boxes; mouse capture, Space activation, hover/leave, focused and disabled appearance, `BN_CLICKED`
through synchronous `WM_COMMAND`, `BM_GETCHECK`, `BM_SETCHECK`,
`BM_GETSTATE`, `BM_SETSTATE`, `BM_CLICK`.

`EDIT`: dynamically allocated UTF-16 text, single/multiline unwrapped layout,
CRLF and literal tabs, selection, click/drag positioning, Left/Right/Up/Down,
Home/End, Ctrl+Home/End, Shift selection, Ctrl+A, Backspace/Delete, horizontal
and vertical caret scrolling, read-only input, and focus/change notifications.
Deleting CRLF or a UTF-16 surrogate pair treats it as one editing unit.
Supported messages include `EM_GETSEL`, `EM_SETSEL`, `EM_REPLACESEL`,
`EM_GETMODIFY`, `EM_SETMODIFY`, `EM_GETLINECOUNT`, `EM_LINEINDEX`,
`EM_LINEFROMCHAR`, `EM_LINELENGTH`, `EM_GETFIRSTVISIBLELINE`,
`EM_LIMITTEXT`, `EM_GETLIMITTEXT`, `EM_SETREADONLY`.

`STATIC`: text labels with left/center/right alignment, simple and no-wrap
variants. The currently supported backgrounds use the default dialog palette.

`comctl32.dll` exports `InitCommonControls[Ex]` and `CreateStatusWindowW`. It
registers `SysListView32`, `msctls_progress32`, `msctls_statusbar32`,
`msctls_trackbar32`, `SysTabControl32`, and `ToolbarWindow32` as child-window classes. ListView
supports report columns, text rows/subitems, single-row selection, keyboard
navigation, hit testing, and item/column notifications. Progress supports range,
position, step and step-advance messages. Status supports one text pane. Trackbar supports
horizontal/vertical range, position, mouse dragging, arrow/Home/End keys, and
scroll notifications. Tab supports text items, selection, click/arrow input,
and `TCN_SELCHANGE`. Toolbar supports indexed strings (`TB_ADDSTRINGW`) and
pointer-backed button text, separators, hover/pressed and checked/enabled state,
insertion/deletion/query messages, autosizing, and sends
standard `WM_COMMAND` notifications. Each retains state per HWND and paints through GDI.
Auto-radio currently groups by parent because dialog-order traversal and
`WS_GROUP` navigation are not implemented. Group boxes are static frames.
Only the ListView report view and single selection are implemented; icon/list
views, image lists, custom drawing, and its scroll bars remain unsupported.
Toolbar image lists, bitmap buttons, dropdowns and overflow, status pane layout,
trackbar custom tick arrays, tab images/custom draw, other
common-control families, and the broader COMCTL32 API remain future work.

`BUTTON`, `EDIT`, and `STATIC` implement `WM_SETTEXT`, `WM_GETTEXT`, `WM_GETTEXTLENGTH`,
`WM_SETFONT`, `WM_GETFONT`, and teardown through `WM_NCDESTROY`.
`SetWindowTextW`/`GetWindowTextW` use window messages so control text is not
truncated to the kernel's short caption buffer. An outstanding control callback
holds a reference: a parent may destroy the sender during `WM_COMMAND` without
freeing memory still used by that callback.

## Shared theme

`uxtheme.dll` exports `OpenThemeData`, `CloseThemeData`, `IsThemeActive`,
`IsAppThemed`, `DrawThemeBackground`, `GetThemeBackgroundContentRect` and
`GetThemeColor`. Theme handles are allocated and validated; unsupported classes
or parts do not claim success. Supported parts are push buttons, checkboxes,
edit backgrounds/borders and static backgrounds. Drawing uses GDI and the same
font atlas as non-client captions.

Applications can use the theme API themselves for custom controls. The current
theme is fixed; `.msstyles` loading, theme changes and the full UxTheme API are
not implemented. The Selawik atlas and independently drawn assets do not provide
pixel-for-pixel Windows 10 fidelity.

## Callbacks, lifecycle and input

Callbacks support eight nesting levels. Every level has a separate thunk and
two RW/NX stack pages separated from the next level by an unmapped guard.
Trampolines are client-read-only RX. Kernel continuation frames preserve the
outer creation/message/destruction operation. Resources are reused per thread
and released on termination. Destruction delivers `WM_DESTROY` before children
and `WM_NCDESTROY` immediately before release. A refused `WM_CREATE` also tears
down created children and returns NULL.

The PS/2 driver emits one ordered stream of key make, translated GUI character,
and break events. GUI Enter is CR; ANSI navigation sequences and console LF
stay in the console stream. `GetKeyState` reads the state advanced by removal
from the caller's message queue, not the hardware's later modifier state.
`TrackMouseEvent(TME_LEAVE)` is supported; hover timers are not.

The native syscall numbering remains Svindows-specific. Existing entries
largely follow the originally targeted Windows build, with private geometry
queries at `0x1031`–`0x1037`. `NtUserGetKeyState` and
`NtUserTrackMouseEvent` use private indices `0x1F00` and `0x1F01`; these are not
portable Windows syscall numbers.

## Limits and next layers

This milestone does not implement COMCTL32 status/progress/toolbars/list/tree
controls, the Windows Ribbon framework, menus/dialog management, clipboard and
undo, word wrapping, visible edit scrollbars, IME, general font selection/DPI,
or a blinking caret. Password and aligned EDIT variants and unsupported BUTTON
variants are rejected rather than displayed incorrectly. Standard controls do
not yet process the full set of color, owner-draw, accessibility or dialog
navigation messages. Cross-thread synchronous SendMessage is not implemented
with Windows' receiver-thread scheduling. `GetMessageW` filters remain limited.

`MessageBoxW` returns failure: a user-mode dialog manager and modal loop are
still required. A general-purpose Ribbon/COMCTL32 implementation remains future work.

The API split follows Microsoft's descriptions of
[controls as child windows](https://learn.microsoft.com/en-us/windows/win32/controls/window-controls),
[control IDs](https://learn.microsoft.com/en-us/windows/win32/winmsg/window-features),
and [EDIT notifications](https://learn.microsoft.com/en-us/windows/win32/controls/en-change).

## Build

`make run-gui` boots the default command processor in a graphical QEMU window.
Applications such as Notepad can be started from the command processor when
needed.
