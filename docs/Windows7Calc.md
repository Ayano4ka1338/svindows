# Windows 7 Calculator compatibility

The original calculator from `Win7Ultimate-Basic.vdi` is staged at
`C:\WINDOWS7\CALC.EXE`; its localized resource module is staged beside it as
`CALC.EXE.MUI`. The executable is PE32+ for AMD64 (Windows 6.1), so it uses
Svindows' existing 64-bit process and PE loader path.

Current status (2026-11-09): the original executable starts in Svindows under
QEMU, reaches an idle message loop, accepts mouse clicks, and visibly computes
`2 + 2 = 4`, `9 * 9 = 81`, and `8 / 2 = 4`. Start a visible VM with
`make run-gui`, then launch from the guest prompt with
`C:\WINDOWS7\CALC.EXE`. Screenshots are in `build/windows7-calc-result.png`,
`build/windows7-calc-multiply.png`, and `build/windows7-calc-divide.png`.
The executable and its MUI are original input files, not a replacement calculator.

The current Xorg-backed launch also paints the standard calculator buttons and
their Backspace and square-root glyphs. GDI+ image scaling now calls the
Win32 `StretchBlt` path, backed by `NtGdiStretchBlt`; missing UI glyphs are
rasterized from the bundled DejaVu Sans fallback font rather than handcrafted
pixel patterns. The calculator's standard-mode layout was checked in QEMU
after rebuilding the ISO. This check does not establish complete compatibility
for its other modes or every input path.

Compatibility remains partial: the UI lacks its original themed display,
dialog units use approximate scaling, and the new WIC,
GDI+ and COM support implements a subset. The scientific panel now opens and computes factorial; date calculation still
fails. Programmer/statistics modes and the complete API behavior are not verified.
Keyboard arithmetic and memory shortcuts have now been checked as described below. The notes below are
the bring-up history; earlier failure reports describe earlier builds.

Its imports include the existing Win32 surface (`KERNEL32`, `USER32`,
`GDI32`, `ADVAPI32`, `MSVCRT`, `UXTHEME`, `COMCTL32`) and additional system
libraries (`SHELL32`, `SHLWAPI`, `GDIPLUS`, `OLEAUT32`, `OLE32`, `RPCRT4`,
`WINMM`, `VERSION`, and `WINDOWSCODECS`). It also imports Windows 7 NTDLL
extensions and functions not yet known to be exported by Svindows' runtime.
The loader currently requires every static and delay import to resolve before
it starts the image, so the first launch milestone is resolving these module
and export dependencies. After that, runtime behavior will expose any missing
windowing, menu, resource, graphics, registry, or COM behavior.

The source image is user-provided. The staged calculator files were extracted
read-only; the VDI itself is not modified.

Runtime bring-up (2026-11-08): all static imports and the WindowsCodecs delay
import bind. The MUI now resolves beside the executable. The process enters UI
initialization, then terminates with an access violation at calc RVA 0x4337
(read from NULL + 0x10). A child CalcFrame receives WM_CLOSE before a subsequent
STATIC creation asks to use that now-invalid parent. The stale-parent fallback
was removed: accepting this invalid HWND concealed an earlier lifecycle issue
and did not prevent the access violation. Window-procedure replacement now
updates native dispatch; COMCTL32 has a synchronous subclass chain; class-long
access now reaches native class state. Builds succeed, but the calculator still
hits the same runtime failure; these API implementations alone do not prove its
cause. Next diagnosis should capture the caller that sends WM_CLOSE and the
original stack at RVA 0x4337, rather than infer causality from adjacent logs.

Further diagnosis: WM_CLOSE originates at calc RVA 0x2df7d, reached after
creation of dialog resource 0x2d5 fails. CreateDialogParamW was an unconditional
NULL stub. A modeless dialog implementation now parses standard/extended
resource templates, creates dialog/control HWNDs, and delivers WM_INITDIALOG.
Runtime now creates multiple dialog pages and BUTTON/STATIC controls instead
of hitting the previous access violation. It reaches repeated ShowWindow and
SetWindowPos calls with progressively decreasing dimensions, so usable UI is
still unverified. Dialog units currently use an approximate 2-pixel scale;
font-derived units, MapWindowPoints (currently a no-op), and nonclient geometry
need inspection before visual success can be claimed.

Geometry fixes: GetWindowRect now converts stored width/height into right/bottom
coordinates; MapWindowPoints maps through native client origins; SetWindowPos
SHOW/HIDE flags now update compositor visibility. Runtime reaches GetMessage's
empty-queue wait after painting. A QEMU screendump (build/windows7-calc.png)
shows the Calculator frame and button grid. Most button labels and the display
are blank, so this is not a verified usable calculator. Next rendering work
includes calculator button image lists (BCM_SETIMAGELIST 0x1602), whose current
control implementation returns zero without rendering the supplied images.

Image-list storage now clones added bitmaps, buttons accept BCM_SETIMAGELIST,
and bitmap STATIC controls handle STM_SETIMAGE/STM_GETIMAGE and paint their
bitmap. A runtime screenshot remains visually unchanged. Diagnostic output
shows the first three BCM_SETIMAGELIST calls supply valid but empty lists at
assignment time. This does not yet establish whether ImageList_Add later
fails, is never called, or produces blank pixels; trace the image generation
and add path next. Calculator still reaches an idle message loop.

Bitmap-generation diagnosis: LoadImageW now decodes uncompressed RT_BITMAP
resources (1/4/8-bit palette and 24/32-bit BGR) into a 32-bit DIB. Runtime logs
confirm ImageList_Add receives a NULL bitmap. GdipCreateBitmapFromScan0 receives
136x27 and succeeds. No GdipCreateHBITMAPFromBitmap call is reached in that
path. Disassembly at calc RVA 0x12d3a..0x12e04 shows a four-state cloning/drawing
loop via GdipCloneBitmapAreaI before conversion, and accumulated nonzero GDI+
status bypasses conversion. Next inspect the clone source and status, rather
than treating scan allocation as failed. Full serial log is captured using
QEMU chardev logfile=/tmp/svcalc-serial.log.

Confirmed source-image dependency: calc's helper at RVA 0x7dc0 calls
WICCreateImagingFactory_Proxy, which currently returns E_NOTIMPL. Resource
inventory finds 69 named-type IMAGE entries containing PNG streams; the only
ordinary RT_BITMAP entry is ID 51209 (100x55, 32-bpp BI_RGB). The normal label
path falls back from LoadImageW to WIC, then supplies NULL to
GdipCreateBitmapFromHBITMAP. Runtime explicitly confirms NULL HBITMAP, successful
scan allocation, successful graphics context, invalid clone source and invalid
draw image. Implement the WIC factory/stream/PNG decoder/frame/converter subset
used by calc instead of changing GDI+ success codes or fabricating label pixels.

WIC bring-up: implemented a memory-stream PNG decoder using stb_image pinned
to f58f558c120e9b32c217290b80bad1a0729fbb2c (license retained in header),
factory/stream/decoder/frame COM vtables, bitmap source size/pixel format,
resolution/CopyPixels, format conversion and scaling. Runtime now successfully
adds resource pixels to image lists and paints original Win7 button gradients.
Screenshot: build/windows7-calc-wic.png. Glyphs and display are still missing;
calculation is unverified. Relevant follow-up: DrawTextW refuses DT_HIDEPREFIX,
and current DIB synchronization uploads client memory before source BitBlt,
which may overwrite native GDI drawing when the DIB memory is stale. Neither
hypothesis has yet been verified as the cause of the remaining blank text.
This WIC subset is not a complete codec or COM implementation.

Text path: added DIB readback after ExtTextOutW/BitBlt and accepted
DT_HIDEPREFIX. That alone left the UI unchanged. Implementing BUTTON
NM_CUSTOMDRAW prepaint/postpaint notifications and dialog DWLP_MSGRESULT
storage/return then made the original digit/operator/memory labels visible.
Screenshot: build/windows7-calc-labels.png. Display and equals glyph remain
blank; arithmetic is not verified. A first monitor mouse attempt reached the
large background EDIT HWND rather than the target digit, so input hit-testing
or injected pointer coordinates also require inspection. No computation result
has been claimed from that attempt.

Display bring-up: DrawTextW multiline rendering no longer drops a line solely
because its full font height exceeds the rectangle. It now renders the visible
portion and applies RIGHT/CENTER horizontal alignment. Runtime screenshot
build/windows7-calc-display.png shows initial 0, digit labels and equals glyph.
Small relative monitor mouse movements (each <=100 pixels) reliably reach the
digit/operator controls; earlier huge moves were discarded as PS/2 overflow.
Clicks on 2,+,2,= delivered WM_COMMAND through the dialog to CalcFrame, but the
visible result remained 0. Arithmetic is still unverified; inspect command IDs,
frame state and result formatting on that exact sequence next.

Arithmetic evidence: with pointer movement bounded to <=100 pixels per packet
and 400ms button-down/800ms inter-click pauses, C,2,+,2,= delivers command IDs
0x51,0x84,0x5d,0x84,0x79. WM_SETTEXT tracing confirms the original calculator
formats intermediate "2 +" and final "4". Serial evidence is saved at
build/diagnostics/win7-calc-arithmetic.log. The final screenshot still does not
show the number, so visible result presentation remains incomplete. Earlier
"remains 0" observations were insufficient evidence of an arithmetic failure;
fast injected clicks had omitted the + command. Added per-control ID text
tracing for the next run to distinguish overlapping result/history controls.

Result control identity: ID 0x0096 receives final "4"; history ID 0x0194 is
cleared separately, so those last blank WM_SETTEXT calls do not erase the result
model. Native GWL_STYLE updates now reach the compositor including WS_VISIBLE,
but the repeated C,2,+,2,= screenshot remains blank in the result area. Thus the
style fix alone does not explain the rendering loss. Result HWND 0x10008 paints
at screen client origin 180,198 with size 167x15. Saved per-control trace at
build/diagnostics/win7-calc-display.log. Inspect its presented raster and sibling
composition/clipping next; do not replace the original calculator's result with
an externally generated display.

Raster evidence: temporary EndPaint inspection counted 43 nonwhite pixels for
initial 0, 36 for digit 2, and 32 for final 4 in result HWND 0x10008's 167x15
PresentedSurface. Thus native rendering and retained presentation both contain
the result; its loss occurs during composition. Removed that slot-specific
raster diagnostic after measurement. New-child insertion now places children
below existing siblings; repeated visual verification still shows blank output,
so creation order alone is insufficient. Inspect subsequent Z-order changes
and the actual W32kCopyWindowClient composition sequence for overlapping slots
8 and 9 next. Latest runtime log: build/diagnostics/win7-calc-raster.log.


Visible-result fix: composition was copying an allocated but never painted
STATIC client raster over the result and history controls. A retained window
DC now records whether any dirty pixels have actually been presented; untouched
buffers do not participate in composition. This is independent of the pixel
color, so an intentionally painted white surface is still opaque. Native zero
window dimensions are also preserved (only CW_USEDEFAULT requests default
sizes), and zero-sized top-level windows are not framed. Temporary per-control
text/pixel diagnostics were removed after verification. Composition evidence is
saved in `build/diagnostics/win7-calc-composition.log`.

SetWindowPos also accepts HWND_BOTTOM and a sibling insertion handle. The
calculator's covering STATIC explicitly requests HWND_TOP, so this Z-order
extension alone did not resolve the lost result. The untouched-raster fix did.

Verification: rebuilt the boot ISO, launched the original calculator under
QEMU/KVM, and clicked C before each arithmetic sequence. The final results were
checked in actual guest screenshots, alongside the original WM_SETTEXT trace.
No changes were made to the source VDI. This establishes an initial working
launch with mouse arithmetic, not full Windows 7 application compatibility.

Staged input SHA-256:

- calc.exe: `c6a91cba00bf87cdb064c49adaac82255cbec6fdd48fd21f9b3b96abf019916b`
- calc.exe.mui: `d35db127daf07ac79397e984c1998df177cffce8a22ebcb9b1fae550834aecad`

Final check after removing the temporary STATIC/composition diagnostics: a fresh
ISO boot and calculator launch again visibly produced `4` for `C,2,+,2,=`.
`build/windows7-calc-result.png` is from this final build; its zero-sized helper
window no longer leaves a stray caption line above Calculator. Final serial
capture: `build/diagnostics/win7-calc-final.log`. `git diff --check` passes.


Keyboard compatibility continuation (2026-11-08): LoadAcceleratorsW now reads
RT_ACCELERATOR resource tables (8-byte resource records, including the final
entry marker) from the original MUI. TranslateAcceleratorW matches character
or virtual-key entries, checks Shift/Ctrl/Alt for virtual keys, and sends
WM_COMMAND with the accelerator notification in its high word. Tables are
cached per module/resource; arbitrary pointers are not accepted as table handles.

The hardware make/character pair is now delivered as a key message with private
layout translation. TranslateMessage posts WM_CHAR/WM_SYSCHAR only when the
application requests translation. Consuming Enter as an accelerator therefore
no longer also executes the character-13 memory-store accelerator. Translated
characters precede pending raw keys, preventing fast Enter from overtaking
unprocessed digit characters. OEM punctuation virtual keys and the extended
scan-code bit are preserved; queued key state also handles system-key messages.
Serial character injection keeps its direct character path.

Runtime evidence in the original calculator:

- Before the change, keyboard `2,+,2,Enter` left the display at 0.
- Keyboard addition now gives 4; multiplication gives 81; division gives 4.
- A rapid `Esc,1,2,Shift+=,3,4,Enter` sequence (20 ms key holds, 60 ms between
  sends) finishes at 46, with exactly one equals WM_COMMAND (0x10079) and no
  spurious memory-store command.
- `Ctrl+M,Esc,Ctrl+R` stores and recalls 46, with the original M indicator.

Screenshots: `build/windows7-calc-keyboard.png`,
`build/windows7-calc-keyboard-multiply.png`,
`build/windows7-calc-keyboard-divide.png`,
`build/windows7-calc-keyboard-fast.png`, and
`build/windows7-calc-keyboard-memory.png`. Runtime log:
`build/diagnostics/win7-calc-keyboard.log`. The ISO builds successfully and
`git diff --check` passes. This verifies the listed shortcuts, not arbitrary
keyboard layouts, IME, full synthetic-key translation, or dialog navigation.

Next compatibility work: menu storage, default class menus, submenu lookup,
checked/disabled state and popup dispatch. The current menu APIs mostly retain
counts or return fixed values; no usable native calculator menu is verified.


Menu compatibility continuation (2026-11-08): registered class menu resources
are loaded separately for each overlapped window. The original MENU resource
108 is parsed recursively, including popup records, terminators and separators.
GetMenu now queries the native window before WM_CREATE has completed. Menu
objects retain labels, commands, submenus, item data, radio/check/disabled state;
lookup, insertion, updates and removal operate on actual items. Invalid menu
handles and malformed/truncated resource streams are rejected.

The window manager draws a 19-pixel nonclient menu bar, consistent with
SM_CYMENU, reserves it in client/child geometry, and routes menu clicks through
WM_SYSCOMMAND. Popup windows run a modal USER loop with mouse capture, keyboard
selection, Escape and mnemonics. Commands are posted to the owner as WM_COMMAND;
checked and disabled items are drawn from stored state. This remains a subset:
owner-draw/bitmap menus, automatic hover switching, full menu notification
semantics, MENUEX resources and complete Alt/dialog navigation are not verified.

Verified in the original calculator: View/Edit/Help bar, View popup with checked
Standard/Basic entries, and Digit grouping selection reaching calc's UseSep
setting. Selecting Scientific initially failed in WindowsCodecs at RVA 0x1b78:
the PNG decoder's local frame crossed a callback stack guard after UI creation.
Callback stacks had only 8 KiB per level. They now provide 64 KiB per level,
retaining eight independently guarded RW/NX stacks, an RX trampoline, rollback,
per-process isolation and thread cleanup. The scientific panel then opens and
visibly computes fact(3) = 6. Opening View again and selecting Standard using
Down/Enter successfully restores the ordinary panel. Client metadata is also
reclaimed for reused HWND generations, including descendants destroyed natively.

Screenshots: `build/windows7-calc-view-menu.png`,
`build/windows7-calc-scientific.png`,
`build/windows7-calc-scientific-factorial.png`, and
`build/windows7-calc-menu-standard-return.png`.

Validation commands: `make all user-menu-check win32k-check`. The menu checks
execute the actual USER implementation and cover nested resources, state,
radio groups, insertion/removal, invalid submenus, truncation and HWND reuse.
Native checks cover menu snapshots and geometry, 64 KiB mapped stack ranges
and guard pages at all eight callback levels, 1000 reused callbacks, concurrent
thread slots, process isolation, failure rollback and the retained GDI suite.
All pass; `git diff --check` passes.

Additional mode coverage exposed a remaining failure in Date calculation:
DTM_SETSYSTEMTIME (0x1002) reaches calc's subclass at RVA 0x508a0 and then calls
a NULL procedure (execute fault at RIP 0). Saved evidence:
`build/diagnostics/win7-calc-date-controls-failure.log`. This is not a verified
working mode. Inspect date/time controls and their original class procedure;
do not paper over the failure by claiming a successful control message.

Final menu regression: the rebuilt ISO displays the Worksheets submenu
(Mortgage, Vehicle lease and both fuel-economy entries). Escape closes each
nested popup; normal keyboard `2,+,2,Enter` then still produces 4. Screenshots:
`build/windows7-calc-submenu.png` and
`build/windows7-calc-menu-regression.png`. Final capture:
`build/diagnostics/win7-calc-menu-final.log`.
The date-panel failure log also records unresolved control classes with NULL
native window procedures. InitCommonControlsEx currently does not register
date/time classes; inspect registration and reject unknown classes correctly
while implementing the actual control behavior.
