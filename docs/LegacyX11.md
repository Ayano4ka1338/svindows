# DOS and Win16 windows in Xorg

Graphics DOS programs launched while Xorg owns the keyboard use a local X11
window on `:0`. VGA modes 04h–06h, 0Dh–12h and 13h are scanned out at the
guest resolution. Xorg retains the physical framebuffer. Text DOS output
continues through the launching console/terminal. Outside X, the fullscreen
backend is used.

Start `STARTX`, then enter `WIN3X` in its cmd terminal. The launcher runs
`WIN.COM /S`. The bundled `WINDOWS3/WIN.INI` loads `XWIN.EXE` through `load=`;
`WIN3` therefore also enables the bridge automatically inside X. Outside X,
the agent exits quietly and the normal Windows desktop stays fullscreen.
Program Manager, application windows, icons and dialogs become separate X
clients. The temporary DOS startup window disappears when the agent attaches.

Each top-level HWND owns its X window and an independent RGB backing image.
Moving the X window changes only its host position: a window can move beyond
the guest's 640×480 desktop without losing its image. Host focus changes are
forwarded to Win3 before keyboard/button input is released. Mouse motion is
coalesced while button transitions remain ordered. Native cursor rendering
avoids copying the guest pointer into each window's snapshot.

`WM_DELETE_WINDOW` returns a close command to the owning HWND. The program
still gets its normal close handling and can open a save dialog. Host resize
requests go to `SetWindowPos`, bounded by the guest screen dimensions. Nested
DOS EXEC contexts share the X connection and release its windows, cursor and
backing images when the final context exits.

## Rendering and current limits

This is an experimental Win16 agent and X11 bridge. Windows still runs in the
DOS VM; USER/GDI and existing application compatibility are retained. Every
100 ms the agent publishes visible top-level windows, repaints their frame
and children, and captures them into separate backing images. It restores
the guest stacking afterward and suspends host input during the capture.
First captures and size changes can temporarily activate a window to obtain
its complete image; mouse capture suppresses that activation.

Regions still covered by an owned popup retain their previous pixels instead
of copying the popup into its owner. Obscured regions can remain stale until
the guest reveals/repaints them. This is not a replacement offscreen Win16
display driver. Menus, modal dialogs and applications with custom drawing
need further interactive compatibility testing. The supplied Notepad also
requires `COMMDLG.DLL`, which is absent from this payload; the bridge does not
provide missing Windows components.

The current bounds are 64 visible top-level windows, 64 MiB of backing images,
and 2048 pixels per dimension at the kernel boundary. Win3 window sizes are
limited to its existing VGA screen; host positions are independent. Child
controls stay inside the parent X window. The original Win3 frame remains
inside the X window manager's frame. Host title encoding, custom guest cursor
shapes, clipboard sharing, XIM and guest display-resolution changes are not
integrated.

The backend targets the repository's local unauthenticated Xorg `:0` session,
24-bit RGB root visual, 32-bit little-endian ZPixmap storage, and bundled
evdev keycodes. A missing/unsupported X display never causes a fallback write
over Xorg's framebuffer.

## Building and verification

`user/d/xwin/XWIN.EXE` is included so an ordinary `make` needs no Win16 compiler.
Rebuild it with `WATCOM=/path/to/open-watcom make win16-agent`; the installation
needs `binl64`, Win16 headers and `lib286`. See the compiler's
[Windows 3.x target documentation](https://open-watcom.github.io/open-watcom-v2-wikidocs/c_readme.html).
Then run `make` to refresh the FAT image and ISO.

`make dos-x11-check` checks production code for stream short reads, X11 setup,
separate HWND resources, independent captures, popup clipping, exposure,
activation acknowledgements, mouse button ordering, close requests and shared
connection cleanup. It also checks the compatibility fixes needed by this
path: AF_UNIX edge generations for `epoll`, ring-3 `SMSW`, DOS extended-error
queries, and DOSX task termination without restoring a stale EXEC stack.

`python3 tools/Win3XVisualSmoke.py --kvm` builds an isolated test ISO from the
normal staged image, starts Calculator and Alarm Clock, and uses a separate
native X11 probe to find their distinct client IDs, move Calculator beyond
the VGA desktop, inject a key and close Clock while retaining Calculator.
The screenshot and serial transcript go to `build/checks`. Source files and
the normal ISO are not modified by the visual test. Without KVM, omit the
flag and allow a larger `--timeout` if needed.

The older aggregate `dos-vm-check` is not green on the unchanged repository
baseline: its Hardware, Cursor and Loader harnesses have stale structures,
and the VGA check expects an obsolete source fragment. Baseline failure logs
are recorded under `build/checks/baseline-*.log`; these are separate from the
new checks and successful QEMU smoke runs.

## Performance

Frame uploads use nonblocking writes and retain at most one pending X11 image
scanline (8216 bytes). A full socket returns control to the interpreter;
subsequent polls continue the exact partial request. Control requests follow
pending image bytes in stream order. Scanlines and clients rotate after
backpressure to avoid starving lower rows or other windows. The local socket
ring is 256 KiB instead of 8 KiB; it remains bounded.

The Win16 agent forces a complete repaint only for a new or resized backing
image. Ordinary refreshes flush existing guest damage without invalidating
all windows or changing their stacking. Obscured pixels retain the limitations
described above.

Protected-mode fault checkpoints use fixed-size compiler builtin copies,
retaining the complete register/segment rollback. Real-mode STOS no longer
reads source/destination memory; MOVS, LODS and SCAS also avoid irrelevant
reads, including reads with VGA latch side effects.

`make dos-performance-check` checks string bus accesses, operand widths,
REP termination, DF and address wrapping, and prints a host checkpoint
microbenchmark. `--cpu-source /path/to/Cpu.c` on the Python checker permits
comparison with an archived source version. On the development host, 5 million
checkpoint saves measured approximately 33 ns/save before and 2 ns/save after;
these are isolated host measurements, not application FPS. The DOS CPU still
uses a C interpreter, so these changes do not provide native x86 execution.
The X11 checker also simulates persistent EAGAIN, short writes within an image,
continuation before a control request, and continued execution without yields.
