<p align="center">
  <img src="other/logo.png" alt="Svindows Logo" width="180">
</p>

<h1 align="center">Svindows</h1>

<p align="center">
  An NT-compatible operating system for x86-64
</p>

<p align="center">
  <img src="https://img.shields.io/github/stars/uriyovsiannikov/svindows?style=flat-square" alt="Stars">
  <img src="https://img.shields.io/github/issues/uriyovsiannikov/svindows?style=flat-square" alt="Issues">
  <img src="https://img.shields.io/github/last-commit/uriyovsiannikov/svindows?style=flat-square" alt="Last Commit">
  <img src="https://img.shields.io/github/repo-size/uriyovsiannikov/svindows?style=flat-square" alt="Repo Size">
  <img src="https://img.shields.io/github/languages/top/uriyovsiannikov/svindows?style=flat-square" alt="Top Language">
  <img src="https://img.shields.io/github/license/uriyovsiannikov/svindows?style=flat-square" alt="License">
</p>

---

## About

Svindows is a from-scratch `x86_64` operating system aiming for **binary compatibility with the Windows NT family**.

The goal is to load and execute native NT `PE/COFF` executables (`.exe` / `.dll`) without using Microsoft's code.

The project is built bottom-up around NT-style components:

`Ke` · `Mm` · `Ob` · `Ps` · `Io` · `Ex` · `Rtl` · `Hal`

Svindows is intended for educational and research purposes.

> Svindows is not affiliated with Microsoft and contains no Microsoft code.

## Current Status

- Boots via **Multiboot2/GRUB** and enters 64-bit long mode
- GDT/TSS and IDT
- CPU exception handling
- Serial and VGA console
- Higher-half kernel
- Physical-frame allocator
- Page tables and direct map
- Kernel pool
- NT-style object manager
- Per-process handles and namespaces
- Kernel threads
- Preemptive scheduler
- Ring-3 processes through `syscall`/`sysret`
- NT-style `Nt*` service table
- PE32+ executable and DLL loader
- Writable FAT32 disk image support with subdirectories
- Import resolution
- Windows-compatible PEB/TEB structures
- NT file I/O
- Events, semaphores, threads and waits
- Registry APIs
- User-pointer validation
- Ring-3 exception dispatch: vectored handlers, C `__try/__except` and
  `__finally`, and C++ `throw`/`catch` through the MSVC x64 personality
- `kernel32.dll`
- `advapi32.dll`
- `msvcrt.dll`
- Small CRT for C/Win32 programs
- Win32 TLS
- Process/thread identity
- Virtual memory APIs
- Timing and synchronization
- Dynamic DLL loading
- 32-bpp framebuffer
- PS/2 keyboard and mouse input
- User-mode `cmd.exe` with PATH lookup, variables, batch files, redirection,
  pipelines, history, arrow-key recall, in-line editing and filesystem commands
- Character console subsystem with screen buffers, colors, cursor and modes

### Compatibility

Svindows boots its own PE32+ command processor by default. The shell can launch
child PE processes in isolated address spaces, search `PATH`, expand variables,
run `.bat` files, connect processes with anonymous pipes, redirect standard
handles, and mutate files and directories on the writable FAT32 volume.

## Roadmap

See [`docs/ROADMAP.md`](docs/ROADMAP.md) for the planned development roadmap.

## Building

You need a Linux host with:

- `gcc` or `clang`
- `ld`
- `nasm`
- `grub-mkrescue`
- `xorriso`
- `mtools`
- `qemu-system-x86_64`

Build the complete system (kernel, user-mode runtime, disk and ISO):

```bash
make
# or, equivalently:
make all
```

Remove all generated files:

```bash
make clean
```

Run in the current terminal (serial input/output):

```bash
make run-serial
```

Run with a graphical QEMU window:

```bash
make run-gui
```

Both run targets boot the same user-mode `cmd.exe`; `run-gui` opens QEMU's VGA
window while `run-serial` stays entirely in the terminal.

User-space executables are stored on the FAT32 disk image:

```text
build/disk.img
```

## Project Structure

```text
boot/            GRUB configuration

kernel/
  arch/x86_64/   Architecture-specific boot & CPU code
  include/       Public kernel headers
    nt/          NT ABI types and status codes
    svindows/    Executive component interfaces
  ke/            Kernel core, threads, scheduler
  hal/            HAL, serial, VGA, framebuffer, PS/2, PIC, PIT
  rtl/            Runtime Library
  mm/             Memory Manager
  ex/             Executive support
  ob/             Object Manager
  ldr/            PE/COFF image loader
  ps/             Process manager, PEB/TEB
  io/             ATA/FAT32 I/O
  cm/             Configuration Manager / registry

user/             User-space DLLs, CRT, and test programs
docs/             Architecture notes and roadmap
scripts/          Helper scripts
```

## Architecture

Svindows follows an NT-inspired architecture with separate kernel subsystems for:

- **Ke** — kernel core, threads and scheduler
- **Mm** — memory management
- **Ob** — object manager
- **Ps** — process and thread management
- **Io** — I/O subsystem
- **Ex** — executive support
- **Rtl** — runtime library
- **Hal** — hardware abstraction layer

The kernel is designed from the bottom up rather than being based on the Windows source code.

## License

See [`LICENSE`](LICENSE).

---

<p align="center">
  <sub>Svindows — an experimental NT-compatible operating system for x86-64.</sub>
</p>
