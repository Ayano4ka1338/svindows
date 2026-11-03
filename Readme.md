# Svindows

<table>
  <tr>
    <td width="35%" align="center" valign="middle">
      <img src="other/Logo.png" alt="Svindows Logo" width="220">
    </td>
    <td width="65%" valign="middle">
      <h1>Svindows</h1>
      <p><strong>An experimental NT-compatible operating system for x86-64.</strong></p>
      <p>An independent OS built from scratch, featuring an NT-inspired kernel and compatibility layers for Windows, Linux and DOS applications.</p>
      <p>
        <img src="https://img.shields.io/github/stars/Ayano4ka1338/svindows?style=flat-square" alt="Stars">
        <img src="https://img.shields.io/github/issues/Ayano4ka1338/svindows?style=flat-square" alt="Issues">
        <img src="https://img.shields.io/github/last-commit/Ayano4ka1338/svindows?style=flat-square" alt="Last Commit">
        <img src="https://img.shields.io/github/repo-size/Ayano4ka1338/svindows?style=flat-square" alt="Repository Size">
        <img src="https://img.shields.io/github/languages/top/Ayano4ka1338/svindows?style=flat-square" alt="Language">
        <img src="https://img.shields.io/github/license/Ayano4ka1338/svindows?style=flat-square" alt="License">
      </p>
    </td>
  </tr>
</table>

<p align="center">
  <img src="https://github.com/user-attachments/assets/71df715c-e44f-4b46-964a-1b1937211be4" alt="Svindows Desktop" width="100%">
</p>

---

## About

Svindows is an experimental x86-64 operating system built from scratch around an NT-inspired kernel. It aims to run software from multiple operating system ecosystems through independent compatibility layers, without using Microsoft source code.

## Features

- **NT Kernel** — Limine Multiboot2 boot with a FAT32 `initrd.fat`, long mode, ring 0/3, virtual memory, processes, threads, scheduler, kernel objects and system calls.
- **Win32** — PE32+ EXE/DLL loading, PEB/TEB, native `Nt*` services, Win32 runtime, console, USER32 and GDI APIs.
- **DOS** — COM and MZ executables, DOS/BIOS interrupts, partial 386 protected mode and DPMI, virtual VGA, PIC, PIT and keyboard devices.
- **Linux ABI** — x86-64 ELF, musl interpreter, Linux-style syscalls, `mmap`, `futex`, `poll`/`epoll`, processes and AF_INET sockets.
- **Networking** — VirtIO-net, lwIP, IPv4, DHCP, DNS, ICMP, UDP, TCP and basic HTTP client.
- **Graphics & Storage** — FAT32/VFAT, framebuffer console, Win32 GDI and experimental Xorg/X11 desktop.
- **Userland** — `cmd.exe`, `curl`, `fastfetch`, Xorg utilities, `feh` and a DOS/4G DOOM compatibility target.

Compatibility is experimental and incomplete. DOS/DPMI gameplay, sound, full POSIX/Linux behavior, Linux signals, TLS/HTTPS and broad Windows API coverage are not guaranteed. X11 applications require a running X server. See [`docs/`](docs/) for details.

## Build & Run

**Requirements:** Linux, GCC/binutils, Clang/LLD, NASM, GNU Make, Python 3, xorriso, mtools and QEMU.

```bash
git clone https://github.com/Ayano4ka1338/svindows.git
cd svindows

make             # Build Limine ISO and FAT32 initrd module
make run         # Boot using QEMU serial console
make run-gui     # Boot graphical QEMU
make run-net     # Run network smoke test
make clean       # Remove generated files
```

Alpine APK packages (Xorg, `feh`, `curl` and Fastfetch) download in parallel
and are kept in `~/.cache/svindows/alpine-apk` between builds. `make clean` does
not delete this cache. Tune concurrency with `ALPINE_DOWNLOAD_JOBS=12` (default)
or select a nearby mirror with `ALPINE_MIRROR=https://.../alpine`.
