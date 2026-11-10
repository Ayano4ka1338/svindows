# Original Windows 7 Notepad and Registry Editor

The user-provided `win7.vdi` was read without modifying the source image.
Original AMD64 executables and their MUI files are staged alongside Calculator
in `C:\WINDOWS7`. Registry Editor's original `ulib.dll` and `clb.dll` are
staged beside the executable; the PE loader searches the application directory
after the system directories. These files do not replace Svindows' system DLLs.

## Notepad: verified in the normal ISO

On 2026-11-10 the original `C:\WINDOWS7\NOTEPAD.EXE` was exercised in
QEMU using `build/svindows.iso`, with the normal strict import loader:

- The main window, menu and multiline editor appear.
- Keyboard input produces `svindows`.
- Ctrl+S opens the save-path dialog and saves `C:\TEMP\NOTE.TXT`.
- Ctrl+N clears the document; Ctrl+O reopens the saved eight-byte file and
  displays `svindows` again.

The latest evidence is in `build/diagnostics/win7-notepad-save-verify.log` and
`win7-notepad-{typed,save-dialog,saved,new-document,reopened}.ppm` in that
directory. PNG previews of the reopened document and save dialog are also
available. The common file dialogs currently accept a typed path; they do not
provide the full Windows Explorer file browser. Printing, font selection and
all encoding variants have not been verified.

The editor compatibility changes include movable LocalAlloc handles,
EM_GETHANDLE/EM_SETHANDLE storage, Unicode formatting, menu loading and common
file dialogs. This is a tested subset of Notepad functionality.

## Registry Editor: verified in the normal ISO

On 2026-11-10 the original `C:\WINDOWS7\REGEDIT.EXE` starts in the normal
`build/svindows.iso` with the strict production loader. All static and delay
imports of the executable and its private `ulib.dll`/`clb.dll` resolve.
`build/diagnostics/win7-regedit-strict-7.log` reaches the idle message queue
without an exception; its matching PNG shows the main window, Computer tree,
five root keys, menus, and Name/Type/Data list headers.

The final GUI build was also exercised under Xorg with Notepad running at the
same time. In Registry Editor, expanding HKEY_LOCAL_MACHINE displays Software;
Notepad accepts `svindows` after switching focus. Both windows retain their own
contents. Evidence: `build/diagnostics/win7-apps-xorg.log` and
`win7-apps-xorg-registry-expanded.png` in the same directory. This verifies
startup, simultaneous presentation and a read-only tree expansion. Registry
value editing, import/export, deletion and persistence have not been verified.

Compatibility remains partial. Stock Shell icons and image-list icon
replacement are unavailable; tree icon artwork is absent. Authorization checks,
ACL property pages, INF registration, device controls, serial devices, console
resizing and unsupported GDI modes return explicit failure. The console uses
its fixed OEM code page. OLE data-transfer media have ownership-aware cleanup;
file-copy callbacks support progress, cancellation, stop and quiet behavior for
basic copies, but extended copy flags and complete Windows metadata preservation
are not implemented. These limits must not be described as full Win32 support.

### Earlier diagnostic milestone

The earlier `win7-regedit-probe-16-trace` run used a separate diagnostic ISO
which bound unresolved imports to unique UD2 traps. Its UI screenshot alone
did not establish production compatibility. That loader is not part of the
normal build. The strict-loader and GUI evidence above supersedes the earlier
startup limitation.

Observed startup blockers fixed during bring-up include the executable stub
arena being overwritten by process-environment mapping, named PE resource
lookup, allocated FormatMessageW strings, MENUEX loading, absent SysTreeView32,
GetEffectiveClientRect, private-DLL lookup and missing import contracts.

## Local checks

`make tree-view-check client-rect-check common-controls-check user-menu-check
win32k-check storage-medium-check native-delete-check copy-file-ex-check
resource-range-check` exercises the implemented controls, reentrant destruction,
geometry, menus and native window lifecycle. `make all` builds the normal ISO.
The tree tests run under ASan/UBSan and cover notification veto, recursive
parent deletion from child callbacks, stale item handles and destruction
during selection. These checks supplement the QEMU runs; they do not replace
running the original applications.


Final verification: `make all` and the GUI build both succeeded. The combined
check log `build/diagnostics/win7-apps-release-check.log` contains 34 passing
checks, including new ASan/UBSan checks for OLE medium ownership, native-delete
handle cleanup, copy cancellation and packed large resource ranges. The latest
Notepad save/reopen transcript records the saved eight-byte file.
