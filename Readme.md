# Svindows

<img src="other/Logo123.png" width="90" align="right" alt="Svindows logo">

An experimental x86-64 operating system built from scratch around an NT-inspired kernel.
It runs software from several ecosystems through independent compatibility layers, without
Microsoft source code.

<p align="center">
  <img src="https://github.com/user-attachments/assets/71df715c-e44f-4b46-964a-1b1937211be4" alt="Svindows desktop" width="100%">
</p>

## Features

- **Kernel** — Limine boot, SMP, virtual memory and pools, objects, registry, processes,
  threads, a preemptive scheduler and call/import tracing.
- **Win32** — PE32+ loading, PEB/TEB, native `Nt*` calls, kernel32/user32/gdi32/comctl32,
  GDI+, COM/OLE, common dialogs and x64 exception handling. Original Windows 7 Notepad,
  Registry Editor and Calculator are bundled and usable.
- **DOS** — COM/MZ programs, BIOS interrupts, 386 protected mode/DPMI, a JIT CPU, VGA,
  and bundled Windows 1.0, 2.0 and 3.x applications.
- **Linux** — ELF64 with musl, Linux syscalls, `mmap`, `futex`, `poll`/`epoll`, `/proc`,
  AF_INET sockets, the Alpine `apk` client and Xorg applications.
- **Networking** — VirtIO-net, lwIP, IPv4, DHCP, DNS, ICMP, UDP and TCP.
- **Graphics** — framebuffer console, GDI/win32k, an Xorg desktop, and Win32 or Win16
  windows exported as native X11 clients.

Compatibility is experimental and incomplete. Details are in [`docs/`](docs/).

## Build & Run

Requires Linux, GCC/binutils, Clang/LLD, NASM, GNU Make, Python 3, xorriso, mtools and QEMU.

```bash
make          # build the ISO and FAT32 initrd
make run      # boot with a serial console
make run-gui  # boot the graphical desktop
make run-net  # boot and run the network smoke test
make clean    # remove generated files
```

Alpine packages (Xorg, `feh`, `curl`, fastfetch) are cached in
`~/.cache/svindows/alpine-apk`. Tune downloads with `ALPINE_DOWNLOAD_JOBS` and
`ALPINE_MIRROR`. Package installation is still experimental.

## License

See [`License`](License).
