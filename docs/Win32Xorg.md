# Win32 windows in Xorg

The Xorg mirror exports one retained Win32 window tree per X11 client. It must
not crop the final Win32 desktop composite: native and TWM placement/stacking
can differ, and another application's pixels in that composite would otherwise
be copied into the covered client's X11 window.

`W32kComposeWindow` snapshots a root and its descendants into a separate raster
while native composition state is protected. The desktop composition target
and damage clip are restored before any socket operation. Overlap and exposure
do not require either application to repaint its completed client content.

Captionless WS_POPUP windows use X11 override-redirect, so menus are not
reparented or decorated by TWM. Their positions are translated through the
owner's current X11 client origin. Managed titles come from the window's
current Unicode title and are updated as UTF-8 WM_NAME/_NET_WM_NAME properties.
ConfigureWindow values are 32-bit protocol fields; the exported menu bar is
accounted for when converting a managed resize back to Win32 outer geometry.

X11 pointer events name a destination window. Dispatch uses that root and its
children rather than hit-testing unrelated overlapping native roots. Capture
still takes precedence. Focus events synchronize the native active window and
restore the previously focused child when its handle is still valid.

## Verification

`make win32k-check` covers independent retained exports, popup attributes,
UTF-8 title bytes, owner-relative placement, resize request fields and explicit
pointer destinations, along with the existing paint and window-lifecycle tests.

Build and run with `make run-gui`. From the guest shell launch
`C:\WINDOWS7\CALC.EXE &`, then `C:\WINDOWS7\NOTEPAD.EXE &`.
Check that Calculator retains its controls when Notepad opens and when raised
again. Open Notepad's Format menu: it should appear below the menu bar without
an application caption. Move the owner and repeat. Alternate keyboard input
between both applications and verify it reaches the focused application.

The 2026-11-10 QEMU captures are in `build/diagnostics/xorg-*.png`; serial
transcripts and build/check logs are in the same directory. The user's
already-running VM is left alone; rebuilt images take effect after a reboot.

Verified on the rebuilt GUI ISO: Calculator remains intact after Notepad
opens and is raised again; mouse input computes 2+2=4. After focusing Notepad,
keyboard input adds `svindows` to its editor. Moving Notepad by its TWM caption
and reopening Format leaves the menu anchored below Format without a TWM
caption or an extra application frame. Final captures:
`xorg-calc-four-final.png`, `xorg-notepad-typed-final.png`, and
`xorg-moved-menu-final.png` in `build/diagnostics`.

Rechecked on 2026-11-11 after rebuilding the release GUI ISO from current
sources (`make BUILD=build/gui PROGRAM=cmd.exe CMDLINE=cmd.exe all`).
`make win32k-check` passes. A fresh QEMU boot with Calculator followed by
Notepad preserves Calculator's controls when it is raised above Notepad;
Notepad's Format menu has no TWM caption and stays below its own menu bar.
The application captions are `Calculator` and `Untitled - Notepad`.
Current captures are `xorg-current-two.png`, `xorg-current-format.png`, and
`xorg-current-raised.png`; the serial transcript is
`build/diagnostics/xorg-current-runtime.log`.
An existing VM keeps its loaded kernel and initrd: boot the rebuilt
`build/gui/svindows.iso` to use these fixes.
