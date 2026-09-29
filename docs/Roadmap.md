# Svindows roadmap

The goal is a clean-room, NT-compatible x86-64 operating system. Milestones are
kept here at subsystem level; experiments with individual proprietary Windows
programs do not define the architecture.

## Working foundation

- Multiboot2 boot, long mode, higher-half kernel, GDT/TSS and IDT.
- Physical/virtual memory managers and kernel pool.
- NT-style object manager, per-process handles, dispatcher objects and threads.
- Ring 3, `syscall`/`sysret`, PEB/TEB and PE32+ executable/DLL loading.
- ATA and writable FAT32 with subdirectories and persistent mutations.
- Native `ntdll.dll`, `kernel32.dll`, `advapi32.dll`, `msvcrt.dll` and a small CRT.
- Registry, virtual memory, timing, synchronization, TLS and dynamic DLL loading.
- PS/2 keyboard/mouse and a buffered character console rendered to serial and
  a 32-bpp framebuffer.
- User-mode `cmd.exe` with PATH, variables, batch, redirection, pipes, history,
  filesystem verbs and child-process execution.

## Completed command-line foundation

- Writable FAT32 create/truncate/write/rename/delete and subdirectory paths.
- Shared vnodes with independent per-handle offsets, directory File objects and
  `NtQueryDirectoryFile`-backed Win32 enumeration.
- A write-back sector cache with close-time and explicit
  `NtFlushBuffersFile`/`FlushFileBuffers` persistence.
- Per-process address spaces, handle tables, loader state and process objects.
- `CreateProcess`, waits, exit codes and inherited standard handles.
- Anonymous pipes, redirection, batch files and shell history.
- Console modes, character/attribute buffers, colors and cursor APIs.

## Next: hardening the command-line system

- Automate a sweep of every FAT physical-write boundary and reconcile
  divergent FAT mirrors after power loss during a mirrored update (orphaned
  allocations and FSInfo counters are now repaired at mount).
- A complete Windows `cmd.exe` grammar (arrow-key recall and in-line editing
  are in place).
- Multiple independently allocatable console instances and resize support.

## Compatibility work

- Load-config and security cookies (static PE TLS and `DLL_PROCESS_ATTACH` TLS
  callbacks are in place).
- Finish the x64 exception surface: chained unwind records, the remaining
  prolog/epilog edge cases, and C++ catch-object copying (copy constructors and
  virtual-base teardown). Second-pass unwind, `__finally`, base-class and
  catch-all matching through `__CxxFrameHandler3`/`_CxxThrowException`, C
  `__try/__except`, vectored handlers and `NtContinue` are in place.
- Broader NT and Win32 APIs driven by small, redistributable programs.
- Persistent registry hives and a device/driver/IRP I/O model.

## Graphics

- Keep USER/GDI and win32k interfaces independent of any particular desktop.
- Implement DCs, surfaces, text, blits, update regions and window composition.
- Build a native Svindows desktop shell once the underlying APIs are stable.

## Guiding rule

Implement operating-system contracts, not one-off binary-specific workarounds.
Every milestone should remain bootable, testable and understandable from the
source tree alone.
