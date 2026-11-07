# 16-bit DOS programs

Svindows recognizes `.COM` files and DOS MZ `.EXE` files in the native process
creation path. A DOS program receives a normal process ID and process handle;
`cmd.exe` waits for it and reads its exit code in the same way as for a PE
program. PE32+ images continue through the PE loader. The implementation lives
in `kernel/dos/` and does not link any `16helloos` files.
`Dos.c` owns image loading and VM lifetime, `Cpu.c` executes instructions,
`Interrupt.c` handles DOS and BIOS interrupts, and `DosFile.c` manages DOS
paths, streams, and directory search. `Internal.h` holds their shared VM
state and low-level memory helpers.

The DOS process has a private 1 MiB real-mode memory arena. The kernel
backs a 128 MiB flat-memory address space per DOS VM and reports bounds from
that same limit to DPMI clients. Windows 3 standard mode
and DOS/4GW games use separate VM instances; allocations are released on exit.
The interpreter models 16-bit registers and segment:offset addressing, a PSP, the
COM load address (`PSP:0100`), and DOS MZ image layout and relocation records. It
handles common 8086 arithmetic, data movement, branches, stack, string and
shift instructions, plus selected 80186 instructions. The interpreter also
models 386 real-mode flags, extended registers, and selected 386 instructions.
When CR0.PE is set, it enters a protected-mode execution path with GDT/LDT
descriptor loading, segment base/limit checks, 16/32-bit operand and address
sizes, common data/branch/stack/string instructions, basic control-register
operations, and same-level IDT interrupt gates. Paging, privilege-level stack
switching, VCPI, and full exception delivery are not implemented. The DPMI
initialization handshake is available through INT 2Fh/1687h and a synthetic
real-to-protected entry point. For 32-bit clients, the initial CS descriptor
uses both the 32-bit default operand size and page granularity, producing a
4-GiB segment limit. At the synthetic 16-to-32-bit entry boundary, general
register values are zero-extended from the real-mode register frame so stale
upper halves cannot become invalid protected-mode pointers. The DOS and DPMI
allocation tables use bounded 128-entry slots. In protected mode the VM handles a subset of
DPMI INT 31h calls: descriptor allocation, get/set and base/limit/access setup,
DOS conventional memory allocation/free, simulated calls to supported real-mode
DOS/BIOS interrupt services, version and memory information queries, page-size
queries, and basic linear block allocation/free. General real-mode reflection,
callbacks, resize operations, and the remaining DPMI services are still missing.
DOS
`INT 20h` and
`INT 21h` provide termination, text and keyboard I/O (including nonblocking
`AH=06h` polling and `AH=0Bh` status), standard stream handles, file
open/create/read/write/seek/close, file timestamps, 8.3 directory search
through `FindFirst`/`FindNext` and the DOS DTA, zero-length write resizing,
simple directory operations and memory allocation. The kernel also supplies
DOS date, time, and FAT free-space calls for drive C:. `INT 21h/AH=4Bh` can
run a child `.COM` or DOS MZ `.EXE` image and `AH=4Dh` returns its exit code.
`INT 21h/AX=3800h` returns the default US country-format table, and
`INT 21h/AX=4400h` identifies character-device and C: file handles.
`INT 2Fh/AX=4300h` and `4310h` expose a virtual XMS 3.01 manager for DOS
programs such as Windows 3.1 startup. Its far-call interface supports A20
queries/toggles, XMS allocation/free/resize, block moves, locks, handle
information, and 16/32-bit free-memory queries, backed by a private 16 MiB
extended-memory arena per DOS VM. HMA allocation and UMBs are not available.
DOS file paths use the process's current C: directory. `C:NAME` is relative
to that directory, while `C:\\NAME` starts at the drive root. Unmounted drive
letters and paths too long for the DOS path buffer are rejected. Opened files
honor the read/write access mode supplied to `AH=3Dh`.
BIOS text output through `INT 10h/AH=0Eh` and keyboard input through
`INT 16h/AH=00h` are mapped to the Svindows console. BIOS video services now
include EGA palette register set/read calls (`INT 10h/AH=10h`, subfunctions
00h-03h and 07h-09h), VGA DAC single/block access, the EGA display-combination
query, and pixel read/write for the supported CGA graphics modes. CGA modes 04h,
05h, and 06h use the interleaved B8000h scanline layout; mode 06h is presented as
640x200 monochrome graphics through the Svindows framebuffer. Standard DOS
streams use the child process's native handles, so pipes and redirection work.

The default build uses `CONFIG=release`, compiling the kernel and DOS
interpreter with `-O2` for gameplay. Use `make CONFIG=debug` explicitly when
source-level debugging with `-Og` is needed.

To run a DOS program, put its `.COM` or MZ `.EXE` file on `build/initrd.fat`
(or copy it to the mounted C: drive) and enter its name in `cmd.exe`. When
`user/d/doom/` is present, the build copies the bundled DOS/4G Doom distribution
and any additional files in that directory to `C:\DOOM`, and adds a root
`DOOM.BAT` launcher. Windows 1.01 system files from `user/d/windows1/` are
copied into `C:\WINDOWS1`. The root `C:\WIN.BAT` launcher changes into that
directory before running `WIN.COM`, then restores the root directory when
Windows exits. Start Windows with `C:\>WIN`; alternatively run `CD \WINDOWS1`
and `WIN.COM`. `C:\DOSCHECK.BAT` runs COM,
MZ, conventional-memory and DPMI host-discovery probes from `C:\TESTS`.
`python3 tools/QemuSmoke.py --doom-probe` now checks both the initial 32-bit
DPMI transition and the first virtual PC I/O access. This is a useful startup
smoke test, not proof that the entire game is playable.

The supplied Russian Windows 3.1 files are staged under `C:\WINDOWS3` for
backward compatibility with the existing payload directory. Start them with
`C:\>WIN2` or `C:\>WIN3`; both launch standard 286 mode (`WIN.COM /S`) because
the payload contains `KRNL286.EXE` and does not include the enhanced-mode
`WIN386.EXE`.
The DOS environment includes `C:\WINDOWS3\SYSTEM` in `PATH`, which lets
`WIN.COM` locate `DOSX.EXE`; DOSX's real-mode `0F 00` system-selector probes
are also modeled. For separate top-level Win16 windows inside Xorg, use
`WIN3X` from the X session; see [the legacy X11 backend](LegacyX11.md) for its
rendering model, build instructions and current limitations.
The DOS X11 backend retains unchanged-row detection and sends modified
scanlines individually to the X server.

The DOS CPU uses a batched interpreter with a native x86-64 JIT for hot,
flat 32-bit DPMI blocks. The JIT caches translations by CS:EIP, validates a
source-byte signature on cache hits, and recompiles self-modifying code.
Supported blocks include common register and RAM moves, arithmetic, branches,
stack/call/return, shifts, multiply, narrow loads, and REP MOVS/STOS. VGA,
non-flat addressing, unsupported opcodes, and interrupt-boundary instructions
return to the checked interpreter. Cold blocks stay interpreted until they
have been encountered repeatedly. The bundled DOS/4G Doom image stays on the
optimized interpreter because measurements in QEMU TCG showed the helper-based
JIT was slower for that workload.

The DOS VM also virtualizes the PC I/O instructions `IN`/`OUT` and their
string forms. Its virtual devices currently cover the VGA DAC and mode-13h
linear framebuffer (`A000:0000`), CGA mode 04h-06h scanout (`B800:0000`),
basic VGA status/register ports, the 8042
keyboard data/status ports, a Sound Blaster 2.0 DSP interface at 220h
(reset handshake, version query, common playback command arguments, and
acknowledged pause/resume controls), a coarse PIT counter, and PIC mask/EOI registers, including the common ICW1–ICW4 sequence used to
remap the master/slave interrupt vector bases. Mode 13h is presented through
the Svindows graphics framebuffer. DOS keyboard input is received through an
independent mirrored event queue; timer and keyboard interrupts are reflected
to the currently programmed real-mode or DPMI protected-mode IRQ vectors. The
interpreter samples virtual hardware at 256-instruction boundaries instead of
checking the PIT, PIC, and keyboard queue on every guest instruction;
interrupt-shadow instructions still expire at instruction boundaries. This
keeps timer and input latency bounded while reducing hot-loop overhead. This
is enough infrastructure for DOS/4G graphics and input experiments, but it is
not a complete PC chipset or VGA implementation.

QEMU runs attach an SB16 device and an audio backend. When the DOS guest resets
its DSP, the VM probes that device; 8-bit single-cycle DMA playback is copied
from guest RAM into an aligned below-16-MiB bounce buffer and sent to the SB16.
This path depends on the QEMU device being present. The virtual DSP still does
not implement full SB16 behavior: 16-bit playback, auto-init DMA, mixer/FM
music, and virtual sound IRQ completion are not implemented. This remains a partial DOS and
386 protected-mode subset. Paging, privilege
level stack switching, full exception delivery, many DPMI services, arbitrary
VGA register semantics, MPU-401 audio, EMS, TSRs, and DOS
`EXEC` modes other than `AL=00h` are not implemented. Some protected-mode
opcodes and extender behaviours may still be unsupported; the bundled Doom
image is a compatibility target, not yet a guaranteed gameplay smoke test.
The DOS VM runs until the program exits; the normal kernel timer interrupt
preempts its thread. DOS MZ and COM programs do not execute directly on the
x86-64 CPU: long mode cannot run 16-bit real-mode code in an ordinary 64-bit
process.

The protected-mode hot path now recognizes flat DOS/4GW descriptors, validates
32-bit instruction immediates as a single range, and uses width-sized RAM
loads/stores instead of assembling values byte by byte. Mode-13h chain-4 VGA
writes are also bulk-stored, and the interpreter no longer invokes the full
scheduler every 1M guest instructions. The protected decoder and register
helpers also reuse the DOS VM pointer already obtained by the run loop, avoiding
repeated current-thread lookups for each protected-mode register operation.
Hardware polling remains batched at 256 instructions with interrupt-shadow
handling preserved. Flat protected-mode DOS now runs short instruction blocks
directly, avoiding the general real-mode/callback/render checks on every
instruction; blocks stop before the next IRQ or render boundary. Exception
rollback snapshots the register file each step and copies segment state only
for instructions that can change it. Framebuffer conversion is capped near Doom's 35-Hz game
tick rather than repeatedly converting continuously dirty VGA memory at 100 Hz;
cached VGA scaling coordinates also avoid per-pixel division. Unsupported
guest instructions continue through the interpreter. Validated flat protected
mode `REP MOVS` spans use SSE2 bulk copies, and `REP STOSD` spans use SSE2
vector stores; device-backed VGA spans retain the checked per-element path.
Mode 13h scanout caches its nearest-neighbour scale maps, converts each
palette color once per frame, and uses SSE2 copies for repeated output rows.
These raster optimizations run on the BSP. APs remain parked because shared
thread scheduling is not yet safe for SMP, so DOS work is not distributed
across processors.
The Doom startup probe
checks WAD loading and VGA mode entry, but it does not measure in-game frame
rate or establish a 30-FPS target.

`make run` selects QEMU KVM when `/dev/kvm` is readable and writable, and
otherwise uses TCG; set `QEMU_ACCEL=kvm` or `QEMU_ACCEL=tcg` to override the
choice. The DOS renderer logs `[dos-fps]` once per second. In the current
container, which has no accessible `/dev/kvm`, Doom presents roughly 15–19
frames per second under TCG after the current changes, so 30 FPS has not yet
been reached. Performance under
KVM depends on the host and has not been measured here.

Child DOS images currently run within the parent's native process; they have
private DOS memory and inherit standard streams and DOS file handles. Their
PSP points to a DOS environment block; `EXEC` accepts a caller supplied
environment segment or inherits the parent's block.


The VM records the DOS image size and FNV-1a identity to identify the embedded
DOS/4GW launch stub's same-image `EXEC`. For that one top-level bootstrap
transition, the parent stub exits with the protected child image's exit code
instead of remaining active and potentially re-launching the game. Other DOS
`EXEC` parent/child behavior is unchanged. The protected instruction-fetch,
register-access, and hardware-poll helpers are inlined in the interpreter path.
The JIT runs only up to the next hardware-poll boundary, preserving virtual
IRQ delivery timing.
