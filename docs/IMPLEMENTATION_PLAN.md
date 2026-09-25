# SVINDOWS implementation plan

This checklist defines the current development order.  Each stage ends with a
clean build and a QEMU smoke test before work proceeds to the next stage.

## 1. Process model and lifetime

- [x] Give every `KPROCESS` its own handle table.
- [x] Move loader module/cache state from globals into a process-owned context.
- [x] Move user virtual-allocation and thread-stack cursors into `KPROCESS`.
- [x] Track parent PID, exit status, image name and standard handles.
- [x] Implement process termination: terminate remaining threads, signal the
      process object, close its handles and retain the exit status for waiters.
- [x] Reap terminated threads and release kernel stacks/KTHREAD objects.
- [x] Walk and destroy a process's user page tables and physical pages.
- [x] Represent the initial command processor as a normal user process rather
      than as a thread owned by the System process.
- [x] Add regression coverage for process isolation, standard-handle isolation, exit
      codes and repeated create/wait/close cycles.

## 2. Writable FAT32 and Win32 filesystem surface

- [x] Add sector writes and a buffered block-write path to the ATA layer.
- [x] Implement FAT32 cluster allocation/freeing and mirrored FAT updates.
- [x] Implement directory entry creation, update, deletion and subdirectories.
- [x] Add file offsets, writable file objects and create dispositions.
- [x] Implement `ReadFile`, `WriteFile`, seek, truncate, delete, rename and
      directory creation/removal through NT syscalls and Kernel32 wrappers.
- [x] Add path normalization for drive letters, `.`/`..`, separators and the
      current directory.
- [x] Add create/write/reopen and restart-persistence regression coverage.

## 3. Command processor

- [x] Replace whitespace splitting with a quoted/escaped command-line parser.
- [x] Resolve executable names using current directory, `.exe` completion and
      `PATH`/`PATHEXT`-style searching.
- [x] Implement environment expansion and core `set`, `cd`, `type`, `copy`,
      `del`, `mkdir`, `rmdir`, `rename` and `echo` behavior.
- [x] Add `<`, `>`, `>>`, `2>` redirection backed by inheritable standard
      handles, then anonymous pipes and pipelines.
- [x] Add `.bat` execution, `%ERRORLEVEL%`, conditionals needed by scripts and
      command history/editing.
- [x] Add end-to-end shell regression programs and QEMU scenarios.

## 4. Console subsystem

- [x] Separate kernel diagnostics from user console output.
- [x] Introduce a console subsystem with a character/attribute screen buffer.
- [x] Implement cursor position/shape, colors, scrolling and clear/fill APIs.
- [x] Add cooked/raw input modes and an input-event queue.
- [x] Attach multiple client processes to a console and route their standard
      handles through console file objects.
- [x] Make serial and framebuffer backends consume the same console output.
- [x] Add console API and multi-process output regression tests.

## Follow-up hardening (outside stages 1-4)

- [x] Deliver ring-3 CPU exceptions through `KiUserExceptionDispatcher`, with
      vectored handlers, editable AMD64 `CONTEXT`, `NtContinue`, and divide/
      access-violation regression coverage.
- [x] Walk x64 `.pdata`/`UNWIND_INFO`, restore common AMD64 frame layouts and
      dispatch C `__try/__except` scopes with constant or callable filters.
- [x] Add second-pass unwind for `__finally` termination handlers.
- [x] Dispatch C++ exceptions: `_CxxThrowException` raises 0xE06D7363 and
      `__CxxFrameHandler3` matches typed and catch-all handlers from the MSVC
      x64 `FuncInfo` maps, running destructors on the way.
- [ ] Cover chained unwind records and remaining prolog/epilog edge cases.
- [ ] Enter C++ catches only after unwinding (defer the catch funclet through a
      trampoline) so try-block destruction order matches MSVC.
- [ ] Match C++ catches through the full `CatchableTypeArray` so base and
      polymorphic types (not just exact names) are caught.
- [ ] Enforce the complete Windows file-sharing matrix across concurrent opens.
- [ ] Add FAT32 fault-injection/out-of-space tests and crash-consistent updates.
- [ ] Add FAT long-name creation (existing VFAT long names are readable).
- [ ] Add arrow-key recall and richer in-line command editing.
- [ ] Support multiple independently allocated console instances and resize.
