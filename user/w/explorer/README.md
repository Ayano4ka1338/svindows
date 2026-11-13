# Native shell

`EXPLORER.EXE` is a Svindows user-mode PE application. It creates ordinary USER
windows and GDI surfaces; the kernel owns desktop registration, enumeration,
window lifetime, input, activation, work area and composition.

With no arguments the application creates `Progman` and `Shell_TrayWnd`, registers
the shell desktop, reserves the taskbar work area and tracks real top-level
windows through shell notifications and enumeration. Task buttons restore,
minimize and close their actual HWNDs, including windows created by external
applications. The Windows menu exposes all tracked windows when the taskbar is
too narrow to show every button.

Start discovers executables from the mounted filesystem: `C:\WINDOWS7`,
`C:\WINDOWS\SYSTEM32`, nested `C:\Program Files` directories and `C:\CMD.EXE`.
Search filters the discovered catalog. Launching uses actual executable paths
and the process API. Document opening uses the shared ShellExecute association
implementation. No replacement Notepad or Calculator is supplied by this app.

The desktop shows Computer, the real user Documents directory and entries in
`%USERPROFILE%\Desktop`. InternetShortcut `.url` files are parsed using their
documented `[InternetShortcut] URL` setting; file URLs decode percent-encoded
UTF-8, and link cycles are rejected. Namespace/file icons are ordinary child
windows painted by the shell with GDI. Their focus and keyboard navigation are
independent of their appearance.

`EXPLORER.EXE "C:\path"` or `EXPLORER.EXE /folder "C:\path"` opens an independent
file-browser process without claiming the shell desktop. The last browser window
ends that process. Browser windows created by the desktop share its process.

The browser enumerates actual directories into a report ListView, maintains
Back/Forward/Up history, accepts location input, and supports Open, Refresh,
New Folder, New File, Rename, Copy, Cut, Paste and permanent Delete. Copy/Cut use
real `CF_HDROP` data and `Preferred DropEffect` in the session clipboard. Paste
validates externally supplied clipboard bounds before capturing paths.

Recursive copies and deletions run on a worker thread. Enumeration uses a
dynamic work queue rather than recursive C calls, so a deep directory tree
cannot overflow the GUI stack. Cancellation, write errors, allocation errors and
partial operations have distinct outcomes. Closing a browser cancels and joins
its worker before releasing state. Moves use the native rename contract and do
not implicitly delete an existing destination.

The accompanying ListView implementation supports vertical/horizontal native
scrollbar controls, wheel accumulation, pixel scroll requests, Page Up/Down,
viewport-aware hit testing and selection. State remains referenced across
callbacks that may destroy the control or its parent.

Build the four C units with the existing Windows target include paths and link
as a GUI PE with `/entry:WinMainCRTStartup`. `StackProbe.asm` implements the
compiler's x64 page-by-page stack probe and supplies unwind metadata. Link
kernel32, user32, gdi32, comctl32, shell32 and msvcrt import libraries.

Host regressions are `tools/checks/ShellModelChecks.c` and
`tools/checks/ListViewScrollChecks.c`. They cover path boundaries, device names,
quoting, link cycles, launch cleanup, scrollbar/viewport consistency and
destruction from a notification. The integrated environment still requires QEMU
checks with the kernel/runtime session, clipboard, shell and console contracts.
