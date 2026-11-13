# Native console host

`conhost.exe` is a Win32 renderer for executive-owned consoles. The process
factory attaches console programs to a real console object, installs separate
input/output File endpoints, and queues the host broker. The host is launched
as a separate GUI process with `--attach <application PID>`; process handles
returned by `CreateProcess` continue to identify the application.

The console owns Unicode cells, active screen buffers, viewport/cursor/modes,
codepages, input records, line editing and process attachments. `conhost` reads
coherent retained snapshots, draws positioned GDI glyph runs, and writes
keyboard/mouse records into the console input queue. Resize, scrolling,
QuickEdit selection and shared clipboard copy/paste act on actual data.

`ConWaitForProcessConsoleHost` waits for the authorized host's HWND and status.
The host publishes readiness only after registering its own window. Console
startup failure wakes waiters with its real status. Kernel thread rundown
releases pinned console objects and pending console-read File references, so
forced process termination does not leak references on abandoned stacks.

Screen-buffer security attributes are captured through the executive Se
subsystem; requested access and handle inheritance use Ob. Reopening
`CONOUT$` checks the active buffer's descriptor. Ctrl handlers run on real
threads in the target process, with its stack/TEB/TLS and address space.

The existing boot/serial console remains a separate real device for bootstrap
and explicitly requested headless command sessions. Ordinary console programs
launched from the native GUI use the new console objects and host.

Build `Conhost.c` as PE32+ GUI with `WinMainCRTStartup`, linking kernel32,
user32, gdi32, shell32 and msvcrt import libraries. The shared Win64 stack probe
used by Explorer may be linked when the compiler requests it. Private native
transport entries are `SvNtConsoleManage` (`0x1F60`) and `SvNtConsoleHost`
(`0x1F61`); their structures are defined in `abi/ConsoleHost.h`.

`tools/checks/ConsoleHostChecks.c` executes the actual console and Ob/Se code
under ASan/UBSan using deterministic scheduler/File fixtures. It checks console
isolation, Unicode and multibyte boundaries, input modes/records, allocation
rollback, clipping, descriptors/access/inheritance, readiness, invalidated
handles and exact allocation balance. Guest QEMU checks validate the renderer,
input dispatch, child startup and close path.
