# SVINDOWS architecture

SVINDOWS is organized to mirror the Windows NT executive. This keeps a clear path
toward NT binary compatibility: each NT subsystem has a home, and the public
native API (`Nt*` / `Zw*` system services) can eventually be implemented on top
of the same component boundaries the real kernel uses.

## Address space

64-bit canonical addressing, 4-level paging (PML4). The kernel lives in the
higher half:

| Region                                  | Purpose                                    |
| --------------------------------------- | ------------------------------------------ |
| `0x0000000000000000`–`0x00007FFFFFFFFFFF` | user space (ring 3 code/stack mapped here) |
| `0xFFFF800000000000`–…                   | direct map of all physical RAM (2 MiB pages) |
| `0xFFFFFFFF80000000`–`0xFFFFFFFFBFFFFFFF` | kernel image + kernel data (`-2 GiB`)     |
| `0xFFFFFFFFC0000000`–…                   | kernel pool heap (`-1 GiB`, grows on demand) |

The kernel is linked at virtual base `0xFFFFFFFF80000000` and loaded at physical
`0x100000` (1 MiB). Early boot maps the first 1 GiB both identity-mapped and at
the kernel's higher-half base so the trampoline can survive the jump into the
higher half. `Mm` then builds the kernel's own page tables — the kernel window
plus a direct map of all RAM — switches `CR3`, and drops the identity map. After
that, any physical page is reachable at `MM_DIRECT_MAP_BASE + phys` via
`MmPhysToVirt`, and `ExAllocatePool` hands out memory from the growable heap.

## Boot flow

```
BIOS/UEFI ──▶ Limine (Multiboot2) ──▶ _start (32-bit, arch/x86_64/Boot.asm)
   │
   │  1. save Multiboot2 info pointer + magic
   │  2. build boot page tables (identity + higher half, 2 MiB pages)
   │  3. enable PAE, set EFER.LME, enable paging  → IA-32e mode
   │  4. load GDT64, far-jump into 64-bit code
   │  5. jump to the higher-half virtual address
   ▼
KiSystemStartup (C, ke/KeMain.c)
   │  HAL console → GDT/TSS → IDT → syscall MSRs → Mm (memory)
   │  → Ob (objects) → Ke scheduler + threads (kernel + ring 3)
   │  → PIC/PIT → sti → idle thread
```

## System calls and ring 3

A user thread enters ring 3 by `iretq`. It calls back into the kernel with the
`syscall` instruction, whose entry (`arch/x86_64/SyscallEntry.asm`) does the
`swapgs` + kernel-stack switch through a per-CPU block (KPCR) reached via GS,
then dispatches through `KiServiceTable` in `ke/Syscall.c`. The user boundary
uses the Windows x64 ABI: the service number is in RAX, argument one is copied
from RCX to R10 by the ntdll stub, arguments two through four use RDX/R8/R9,
and later arguments live above the caller's shadow space. `SYSRET` returns to
ring 3. On every context switch the scheduler repoints TSS.RSP0 and the
KPCR's kernel stack at the incoming thread, so a syscall or interrupt taken from
ring 3 always lands on that thread's own kernel stack.

**The argument ABI.** The entry marshals the full Windows syscall argument set
into an array on the kernel stack — arguments 1-4 from `R10`/`RDX`/`R8`/`R9`,
and arguments 5+ from the user stack at `[user_rsp+0x28]` (past the ntdll stub's
return address and the four-slot home space), up to 16 — and passes its address
to `KiSystemServiceDispatch(number, args)`. That lets a service carry its true
NT signature: `NtCreateFile` reads a `POBJECT_ATTRIBUTES` (whose `UNICODE_STRING`
names the target) and reports via an `IO_STATUS_BLOCK`, `NtReadFile`/
`NtWriteFile` take the 9-argument form, and `NtAllocateVirtualMemory` the
6-argument form. The service table (`ke/Syscall.c`) is a sparse array indexed by
the **native x64 syscall numbers for the target ABI** — the same numbers the ntdll
stubs issue — so aligning to a real build's numbering is what would let a
genuine `ntdll` drive this kernel. SVINDOWS-only services (no Windows equivalent)
sit above the real range at `0xF0+`.

**GS and the TEB/PEB.** In ring 3 the GS base points at the thread's TEB, so
native code finds its environment at the usual offsets (`gs:[0x30]` = TEB self,
`gs:[0x60]` = PEB, whose `ImageBaseAddress` is at +0x10). In the kernel the GS
base is the per-CPU block (KPCR). The invariant "GS = KPCR while in ring 0" is
held by `swapgs`: the syscall stub swaps unconditionally, and interrupt entry/
exit swap only when the saved CS shows a ring-3 origin. `KERNEL_GS_BASE` (what
the next kernel-exit swapgs restores) is set to the incoming thread's TEB on
every context switch, so the model stays correct across preemption between user
and kernel threads. `Ps` (`ps/ProcessObject.c`) builds the PEB and TEB and
launches each process's main thread.

## File objects and FAT32

An open disk handle owns its seek position and requested/share access, while a
reference-counted vnode owns the cached contents, metadata and canonical path.
Consequently, two handles to the same file see the same bytes but advance their
positions independently.  The open-file table enforces the read/write/delete
share matrix; deleting an open file marks its vnode delete-pending and removes
the FAT entry when the final handle closes.

Directories are ordinary File objects. `NtQueryDirectoryFile` emits packed
`FileDirectoryInformation` or `FileNamesInformation` records, and kernel32's
`FindFirstFileExW`/`FindNextFileW` wrappers use that native interface. FAT32 is
responsible for short entries and validated VFAT chains; paths crossing the
NT boundary are converted between UTF-8 and UTF-16 without discarding Unicode.
`NtSetInformationFile` handles file position, end-of-file, rename and delete
disposition classes, so Win32 mutations retain handle/share semantics instead
of bypassing the I/O manager with pathname-only calls.

FAT32 accesses Limine's `initrd.fat` through a 64-sector write-back cache and
an in-memory block device. Dirty sectors are written into the initrd module on
eviction, when a disk/directory File object closes, or through
`NtFlushBuffersFile` (`FlushFileBuffers` in kernel32). These writes remain
visible for the rest of the boot session and are lost at reboot. File
replacement uses ordered copy-on-write phases:
write and flush unreferenced data, publish and flush its FAT chain, publish and
flush the directory entry, then release the old chain. Allocation exhaustion
is reported as `STATUS_DISK_FULL`. A build-time physical-sector fault injector
(`FAT_FAULT_AFTER`) exercises failed writes without requiring a test program.
Metadata mutations additionally use bounded sector-cache transactions. The
cache snapshots each sector before its first modification, flushes the changed
set on commit, and can rewrite the snapshots if a block write fails part-way
through. Directory allocation writes its contents before publishing the FAT
allocation; directory growth, create, rename and delete use transaction-backed
metadata phases. Rename publishes the destination before removing the source,
while deletion removes the directory entry before releasing
its cluster chain.

Mount performs a reachability pass beginning at the FAT32 root. It follows all
file chains, walks the directory tree without recursion, preserves bad/reserved
clusters, and transactionally frees allocated clusters which no directory
entry can reach. It then derives `FreeClusterCount` and `NextFreeCluster` from
the FAT and rewrites both primary and backup FSInfo sectors. Thus the benign
leaks possible when power is lost between durable metadata phases are repaired
before user processes can open the volume.

## Synchronization

Waitable kernel objects — events, semaphores, mutants, and threads — begin their
body with a `DISPATCHER_HEADER` (a signal state plus a wait list). A thread that
cannot immediately acquire an object links its wait block into that list, marks
itself `Waiting`, and reschedules; a signal wakes every waiter to re-test the
object. Because a syscall can block this way, the user `RSP` is saved on the
per-thread kernel stack (not the shared per-CPU slot) so it survives a context
switch to another thread mid-syscall. `NtCreateThreadEx`, `NtCreateEvent`,
`NtSetEvent`, and `NtWaitForSingleObject` (with alertable + timeout parameters)
expose this to ring 3.

Each user thread has a bounded FIFO APC queue. `NtQueueApcThread` wakes a
target blocked in an alertable wait; the wait returns `STATUS_USER_APC`, and
`ntdll` invokes the callback in that thread before returning to the caller.
`SleepEx` and the alertable Win32 wait wrappers use the same path. A separate
bounded special kernel APC queue delivers callbacks in the target thread at
APC_LEVEL and restarts interrupted waits. Normal kernel APCs, cross-process
queueing and I/O completion APCs remain future work.

**Validating user pointers.** A service must not trust the pointers ring 3 hands
it. `MmProbeForRead`/`MmProbeForWrite` (`mm/Vmm.c`) check that a buffer lies
wholly in user space (`< 0x0000800000000000`) with every page present, and
`MmCaptureUnicodeName` copies an object name out of a user `UNICODE_STRING` into
kernel memory so the service works on a stable copy. `NtCreateFile`, the registry
services, and the thread/event services probe their inputs and capture their
names up front, returning `STATUS_ACCESS_VIOLATION` on a bad pointer rather than
faulting the kernel. There is no kernel SEH yet, so these are range +
page-presence checks, not fault-safe probes. Ring-3 CPU faults are separate:
`Trap.c` converts them to `EXCEPTION_RECORD` plus the integer/control portion
of an AMD64 `CONTEXT` and redirects execution to ntdll's
`KiUserExceptionDispatcher`. Vectored handlers can edit the context and resume
through `NtContinue`; unhandled faults terminate only the affected process.
The kernel retains the original exception record and context across this
first-chance trip through ring 3. If no handler accepts the exception, the
termination path reports PID/TID, all integer registers, the nearest named PE
export (`module!symbol+offset`), and, for access violations, the faulting VM
region plus the live x86-64 PTE permissions. Stack diagnostics use the same
export lookup, so optimized release failures remain actionable without PDBs.
The ntdll dispatcher reads each loaded image's exception directory, searches
its `RUNTIME_FUNCTION` table and interprets the common AMD64 unwind opcodes.
C `__try/__except` scopes dispatch through `__C_specific_handler`; when one
accepts, `RtlDispatchException` runs a second pass that walks the frames again
with `EXCEPTION_UNWINDING` set so `__finally` handlers and (in C++ functions)
destructors run before control resumes at the handler.

**C++ exceptions.** `_CxxThrowException` raises status `0xE06D7363` with the
thrown object and its module `_ThrowInfo`; `RtlRaiseException` captures the
caller's `CONTEXT` with `RtlCaptureContext` and dispatches it exactly like a CPU
fault, so `RaiseException` needs no separate kernel path. `__CxxFrameHandler3`
reads the function's MSVC x64 `FuncInfo` (unwind map, try/catch map and
IP-to-state map) through the unwind-info handler data. A catch matches when the
handler has no type (a catch-all) or when any entry of the thrown object's
`CatchableTypeArray` names the handler's RTTI descriptor — so a `catch (Base&)`
matches a derived throw and receives the base subobject at its recorded offset.

The catch is entered in two steps, as MSVC guarantees. On the first pass the
personality only records the selected catch (funclet, frame, adjusted object and
the enclosing try block's entry state) and returns a private disposition.
`RtlDispatchException` then runs the termination pass from the raise site,
destroying intermediate frames and the catch frame's own try-block locals, and
only afterwards calls the catch funclet and resumes at the continuation it
returns. That is what makes a try-block destructor run before its catch body.
`ntdll.dll` must carry its own `.pdata`, and the compile flags include
`-funwind-tables`, so the unwinder can see through the
`RaiseException`/`_CxxThrowException` frames that live inside the native library.

**32-bit stack arguments.** The syscall entry marshals every stack argument as a
full 64-bit slot, but the upper half of a slot holding a 32-bit parameter is
undefined under the Win64 ABI. Services whose parameters sit at argument 5 or
later therefore truncate them to their declared width (for example
`NtWriteFile` reads `Length` as a `UINT32`) rather than trusting the full slot.

## Loading executables

`Ldr` (`ldr/Pe.c`) loads PE32+ images the way Windows does: it maps the image at
its preferred base, then **resolves imports** — for each imported DLL it loads
the module (from the mounted filesystem, on demand), looks each imported routine
up in that module's export directory, and patches the executable's Import
Address Table. Resolution recurses, so a full `app.exe -> kernel32.dll ->
ntdll.dll` chain links up automatically. An import the target module doesn't
export fails image loading with a DLL/entrypoint/ordinal diagnostic. Missing
contracts therefore stay visible instead of producing fabricated returns.

At the bottom, `ntdll.dll` is the native library: each `Nt*` export is a stub
(`mov r10, rcx; mov eax, <n>; syscall`) that crosses into the kernel.
`kernel32.dll` sits on top and implements the Win32 API (`WriteFile`,
`CreateThread`, `ExitProcess`, ...) by calling those native routines, so a
normal Win32 program links only against kernel32 and never issues a raw syscall.
`ntdll.dll` is `nasm`-assembled; `kernel32.dll` and the Win32 programs are C,
compiled with `clang --target=x86_64-pc-windows-msvc` and linked with
`lld-link` — ordinary PE files.

After loading, the kernel builds the process's **`PEB->Ldr` module list** —
a `PEB_LDR_DATA` with a `LDR_DATA_TABLE_ENTRY` (DllBase, EntryPoint,
SizeOfImage, a wide BaseDllName) per loaded module, threaded onto the
load/memory/init-order lists at the Windows x64 offsets — in a user-readable
region `PEB->Ldr` points at (`LdrBuildProcessModuleList`). That is what lets the
**dynamic runtime** work in ring 3 without the kernel's help: kernel32's
`GetModuleHandleA` walks the load-order list, `GetProcAddress` parses the target
module's PE export directory, and the process heap (`GetProcessHeap` /
`HeapAlloc` / `HeapFree`, a coalescing free list) runs over
`NtAllocateVirtualMemory`. `LoadLibraryA` loads a DLL from disk **at runtime**:
it calls the kernel loader through an `NtLoadLibrary` service, which maps the
image, resolves its imports, and appends a new `LDR_DATA_TABLE_ENTRY` to the
same `PEB->Ldr` region — after which `GetProcAddress` resolves the freshly
loaded module's exports, exactly as a plugin load works on Windows.

**Static thread-local storage.** A module that declares thread-local variables
carries an `IMAGE_TLS_DIRECTORY64` (DataDirectory 9) naming its `.tls` template,
the `_tls_index` DWORD the loader must fill, and an optional callback array.
`LdrpRegisterTls` (`ldr/pe/Tls.inc`) runs as each module is cached: it assigns
the module a TLS index and writes it to `AddressOfIndex`. Once every module is
known, `LdrApplyThreadTls` gives each thread a block vector at
`TEB.ThreadLocalStoragePointer` (`gs:[0x58]`) — one block per module, zero-filled
and copied from that module's template — from a bump region above the loader
init page. The index space starts at 0 for static TLS and `PEB->LdrpTlsNextIndex`
publishes the next free index, so kernel32's `TlsAlloc` continues after the
static indices instead of colliding with them. The callback arrays are collected
into the loader init block, and ntdll's ring-3 `LdrInitializeProcess` walks them
first — calling each with `DLL_PROCESS_ATTACH` before any `DllMain`.

SSE is enabled at boot (`CR0.MP`/`CR4.OSFXSR`), since compiler-emitted XMM use —
pervasive in real Windows binaries — would otherwise fault, and the context
switch preserves each thread's x87/SSE state with FXSAVE/FXRSTOR (`KiSchedule`
saves the outgoing thread's state and restores the incoming thread's, each from
a 16-byte-aligned per-thread area, so concurrent SSE users don't clobber one
another).

## Graphics

Limine is asked for a 32-bpp linear framebuffer through a Multiboot2 framebuffer
request tag (`arch/x86_64/Boot.asm`). At boot, `mm/Multiboot.c` reads the
framebuffer info tag — physical address, pitch, dimensions, and the RGB field
positions — into `GfxFramebuffer`. `GfxInitialize` (`hal/Framebuffer.c`) maps
the framebuffer (which lives above RAM) into the direct map, and the graphics
primitives — put-pixel, filled rectangle, screen clear, and a 9x20 antialiased
font (`hal/Font9x20.c`) — draw straight into it.

The character console owns a character/attribute screen buffer and renders user
output to serial plus the linear framebuffer. Kernel diagnostics use the early
serial/VGA path and therefore do not overwrite the user console screen.

## Input and the cursor

The 8042 PS/2 controller (`hal/Ps2.c`) multiplexes a keyboard (IRQ1) and the
mouse (IRQ12). The keyboard driver (`hal/Keyboard.c`) translates set-1
scancodes to ASCII, tracking shift/caps, into a ring buffer that `KbdReadChar`
drains. The 0xE0-prefixed navigation keys become the same ANSI escape
sequences a serial terminal emits (`ESC [ A` for Up, and so on), so the console
sees arrows identically from the PS/2 keyboard and COM1. The mouse driver
(`hal/Mouse.c`) reassembles the 3-byte movement packets into a screen-clamped
`MouseState` (position + buttons), bumping a sequence counter on each change.

`cmd.exe`'s line editor consumes those sequences: Up/Down walk the command
history (keeping the in-progress line as a draft), Left/Right, Home and End move
the caret, and Backspace/Delete edit mid-line, redrawing the prompt and buffer
in place.

The command processor keeps only stateful language primitives (`cd`, `set`,
`if`, `echo`, history, labels/comments and `exit`) plus parsing for batch files,
pipes and redirection. Stateless verbs are normal console PE images: `dir.exe`,
`type.exe`, `copy.exe`, the file-mutation commands, `cls.exe`, `ver.exe`,
`vol.exe`, `hostname.exe`, `pause.exe` and `help.exe`. Their aliases are also
installed as executable images. `CreateProcessA` passes the shell directory to
the private process-creation service, which publishes it as
`RTL_USER_PROCESS_PARAMETERS.CurrentDirectory`; `GetCurrentDirectoryW` and the
utilities therefore resolve relative paths correctly after `cd`.

A kernel **input thread** (`InputWorker` in `ke/KeMain.c`) drains serial and
PS/2 input into both the console event queue and USER message queues. The
console subsystem (`io/Console.c`) applies cooked/raw modes and owns the text
cursor; mouse events remain independent for USER/win32k consumers.

## Graphical session

The desktop is a Win32 session, not a full-screen application. Three layers:

**Kernel window manager** (`win32k/UserScreen.inc`, in the `ke/Syscall.c`
translation unit). It owns the screen: the desktop backdrop, the z-order list,
the Windows 10 non-client frame (30 pixel caption, 45x30 minimize/maximize/close
buttons, `#E81123` close hover), the pointer, and input routing. Window geometry
lives here and nowhere else — `ShowWindow`, `SetWindowPos`/`MoveWindow` and
`BeginPaint` ask it for rectangles instead of keeping their own copy. A window
is still a 0x200-byte record in the process's USER object arena, but the frame
style is cached in kernel state, because the compositor also runs on threads
(notably the input thread) that have no mapping for a client's address space.

Window DCs draw in local client coordinates into retained kernel rasters
(`win32k/gdi/WindowSurfaces.inc`). Each window has a drawing raster and a last
completed raster. `BeginPaint` starts a paint; `EndPaint` publishes only the
changed client rectangle. GDI operations outside that pair publish immediately.
Covered pixels are retained, and movement does not change a DC's origin.

The compositor assembles backdrop, window frames, completed client rasters and
the pointer in memory (`win32k/ScreenRaster.inc`) and copies only the finished
damage rectangle to the framebuffer. All chrome, including glyphs and titles,
is clipped to that rectangle. Moving, exposing or covering a window generates
no `WM_PAINT`; resizing and application invalidation do. The UP kernel masks
interrupts while publishing and composing so the input worker cannot mix window
geometry or completed rasters from different updates.

Input is routed by the window manager rather than by a global "active window":
hit testing distinguishes client, caption, caption buttons and border; a click
activates and raises its window; a caption drag moves it; the buttons post
`WM_SYSCOMMAND` (`SC_MINIMIZE`/`SC_MAXIMIZE`/`SC_RESTORE`/`SC_CLOSE`, with the
action applied by win32k) and `WM_CLOSE`; the client receives
`WM_MOUSEMOVE`/`WM_LBUTTONDOWN`/`WM_LBUTTONUP` in its own coordinates, plus
`WM_KEYDOWN`/`WM_KEYUP` with virtual keys and `WM_CHAR` from the keyboard's
translated character path. `SetCapture`/`ReleaseCapture` and
`SetFocus`/`GetFocus` are real services.

**Client layer**: `user32.dll` (classes, windows, message loop,
`BeginPaint`/`EndPaint`, `DefWindowProc`) over the `NtUser*` services and
`gdi32.dll` (DCs, brushes, pens, `Rectangle`, `LineTo`, `PatBlt`, `BitBlt`,
`TextOutW`/`ExtTextOutW`, text metrics and extents) over the `NtGdi*` services.
GDI32 imports ntdll; USER32 also uses kernel32 for allocations, GDI32 for
control drawing and UXTHEME for the shared Windows 10-style theme. COMCTL32 is
a separate client library over USER32/GDI32; progress, status, trackbar and tab
controls are ordinary child HWNDs with user-mode procedures and per-window state.
`BUTTON`, `EDIT` and `STATIC` are user-mode window procedures on real child
HWNDs, with per-instance state and synchronous parent notifications. Notepad
hosts a standard multiline EDIT instead of implementing its own text widget.
Each child has a retained raster clipped against ancestor clients; moving a
parent does not repaint or copy a child's document. Activation and keyboard
focus are separate. Nested USER callbacks have independent guarded stacks and
continuations, including destruction of child trees. See [User32.md](User32.md)
for the supported subset and the remaining dialog/COMCTL32/Ribbon work.

**Text**: win32k's GDI and the client toolkit rasterize one atlas.
`kernel/hal/UiFontData.c` is generated by `tools/GenerateUiFont.c` from Selawik
(SIL OFL 1.1, metric-compatible with Segoe UI) and compiled into the kernel for
window captions and every GDI text call. Its advances
match Segoe UI 9pt to within a pixel per string, so layout can be written in the
same pixel numbers Windows uses.

The address map a client sees is fixed and shared between the two handle tables,
the read-only SERVERINFO page and the per-process arenas (`Ps.h`):

```text
0x1000000  GDI shared handle table
0x1180000  USER shared handle table (SHAREDINFO.aheList)
0x1300000  SERVERINFO (SHAREDINFO.psi): screen metrics, system colours
0x1304000  USER object arena: window records
0x1350000  GDI attribute blocks: per-DC user-writable state
```

## The registry

The Configuration Manager (`Cm`, `cm/Registry.c`) keeps a hierarchical
key/value store in memory: keys form a tree under an anonymous root whose child
`Registry` anchors absolute paths (`\Registry\Machine\...`), and each key holds
a list of named, typed values (`REG_SZ`, `REG_DWORD`, ...). Keys are handed to
ring 3 as **`Key` objects** — `NtCreateKey`/`NtOpenKey` wrap the persistent
tree node in an Ob object and return a handle, so `NtClose` releases a key like
any other handle while the tree itself persists. The services carry their real
NT signatures: `NtOpenKey`/`NtCreateKey` name the key through an
`OBJECT_ATTRIBUTES` (whose `RootDirectory` anchors a relative name), and
`NtQueryValueKey` returns a `KEY_VALUE_PARTIAL_INFORMATION`. `advapi32.dll`
layers the classic `Reg*` API on
top: predefined roots (`HKEY_LOCAL_MACHINE`, ...) map to absolute paths opened
from the root, and a real key handle passes through as the parent for a relative
open — so `app.exe → advapi32 → ntdll → syscall → Cm` is the full path a
registry call takes.

## The user-mode runtime

Above `ntdll`, the DLLs a Windows program expects are reproduced in miniature.
`kernel32.dll` carries the process runtime (module/symbol lookup, the heap,
threads, files) plus time and string helpers; `advapi32.dll` the registry
`Reg*` API; and `msvcrt.dll` a small C runtime (`printf`, `malloc`/`free`,
`str*`/`mem*`) layered on the Win32 API. Time reads avoid a system call the way
Windows does: the kernel maps a read-only **`KUSER_SHARED_DATA`** page at the
fixed user address `0x7FFE0000` and refreshes its tick count / system time on
every clock tick, so `GetTickCount` just reads and scales the shared value. The
command line lives in a `RTL_USER_PROCESS_PARAMETERS` block that `PEB->
ProcessParameters` points at, where `GetCommandLine` finds it.

kernel32 covers a widening slice of the API real programs import:
`GetCurrentProcessId`/`GetCurrentThreadId`/`GetLastError`/`SetLastError` read
straight from the TEB via GS (`ClientId` at 0x40/0x48, `LastErrorValue` at 0x68);
`VirtualAlloc`/`VirtualProtect` run over `NtAllocateVirtualMemory` and a
`NtProtectVirtualMemory` service that re-applies page protection (`MmProtectRange`
keeps each frame, flips its writable bit); `QueryPerformanceCounter` reads the
shared-data clock; and `Interlocked*` plus recursive critical sections give the
atomics and locking a threaded program expects.

## Running a standard program

A program does not need SVINDOWS-specific glue to run. The linker's default console
entry is the CRT startup `mainCRTStartup` (`user/w/crt/Crt0.c`), which parses
`argc`/`argv` from the command line, calls `int main(argc, argv, envp)`, and
calls `ExitProcess` with its return value. So a plain, portable C program —
`int main()`, `<Stdio.h>`/`<Stdlib.h>`/`<String.h>` (minimal headers under
`user/w/include/` that declare the `msvcrt` exports), no OS-specific code —
compiles the ordinary way and runs unchanged. This is the foundation for
eventually loading ready-made Windows binaries: grow the reimplemented DLLs and
the loader (load-config, security cookies, the remaining unwind edge cases, ...)
until real programs' imports and startup are all satisfied.

## Executive components

The directory names match NT's internal prefixes, so a symbol like
`MmAllocatePages` lives in `mm/`, `ObCreateObject` in `ob/`, and so on.

| Prefix | Directory | Responsibility                                             | State |
| ------ | --------- | ---------------------------------------------------------- | ----- |
| `Ke`   | `ke/`     | CPU control, interrupts/traps, scheduling, synchronization | working |
| `Hal`  | `hal/`    | port I/O, serial, VGA, framebuffer, PIC, PIT, IRQ dispatch  | working |
| `Mm`   | `mm/`     | physical & virtual memory, direct map, page tables         | working |
| `Ob`   | `ob/`     | object manager: object types, handles, namespace           | working |
| `Ps`   | `ps/`     | processes and threads (PEB/TEB, user process creation)     | early |
| `Io`   | `io/`     | ATA PIO, FAT32, File objects, handle-based file services    | early |
| `Cm`   | `cm/`     | Configuration Manager: the registry (keys, values, hive)    | early |
| `Ex`   | `ex/`     | executive support: pool allocator, sync primitives         | early |
| `Ldr`  | `ldr/`    | PE/COFF image loader                                       | early |
| `Rtl`  | `rtl/`    | runtime library: strings, memory, lists, formatting        | early |

## Calling conventions and the NT ABI

Internally the kernel is compiled with the System V AMD64 ABI (the toolchain
default). The boundary that must match Windows exactly — the system-service
interface consumed by native PE binaries — uses the Microsoft x64 calling
convention. `NTAPI` is defined as `__attribute__((ms_abi))` for exactly those
functions so that, when the PE loader and `ntdll` shim arrive, native binaries
see a Windows-compatible ABI.

## Design principles

- **Freestanding C.** No host libc. `-ffreestanding`, no red zone, kernel code
  model, general registers only (no implicit SSE/FP in the kernel).
- **NT-faithful naming.** Types, status codes, and structures follow the public
  NT conventions so the eventual native API is a natural fit, not a rewrite.
- **Testable at every step.** Every milestone boots in QEMU and prints evidence
  of what it did over the serial port.
