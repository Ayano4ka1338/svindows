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

## Registry Editor: diagnostic UI milestone only

The normal image still cannot resolve every Registry Editor dependency.
Do not describe Registry Editor as working in the release ISO.

A separate diagnostic ISO binds each unresolved import to a unique UD2 trap
and traces import addresses and native window callbacks. Calling any missing
API therefore stops at a specific address rather than returning fake success.
Changes enabling this diagnostic loader are temporary and are restored before
building the normal ISO; they are not part of the production loader.

The diagnostic run `build/diagnostics/win7-regedit-probe-16-trace.log` reaches
an idle message queue without an exception. Its matching `.ppm` and `.png`
show the original Registry Editor window, Computer tree with the five root
keys, menus, and the Name/Type/Data list header. Selecting, editing, importing,
exporting and deleting registry data have not been verified.

Observed and fixed startup blockers include the executable stub arena being
overwritten by process environment mapping, named PE resource lookup,
allocated FormatMessageW strings, MENUEX loading, absent SysTreeView32 class,
and GetEffectiveClientRect. Stock Shell icons and image-list icon replacement
currently report their defined failure results; tree icon artwork is absent.

Remaining normal-loader gaps include optional authorization/ACL dialogs,
SHLWAPI ordinal helpers, CRT routines and native/console/filesystem APIs
imported by the original private DLLs. Registry Editor also delay-imports
ADVPACK. Resolve these contracts and rerun with the strict loader before
claiming normal launch compatibility. A diagnostic UI screenshot alone does
not establish this.

## Local checks

`make tree-view-check client-rect-check common-controls-check user-menu-check
win32k-check` exercises the implemented controls, reentrant destruction,
geometry, menus and native window lifecycle. `make all` builds the normal ISO.
The tree tests run under ASan/UBSan and cover notification veto, recursive
parent deletion from child callbacks, stale item handles and destruction
during selection. These checks supplement the QEMU runs; they do not replace
running the original applications.
