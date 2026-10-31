<p align="center">
  <img src="other/Logo.png" alt="Svindows Logo" width="180">
</p>

<h1 align="center">Svindows</h1>

<p align="center">
  An NT-compatible operating system for x86-64
</p>

<p align="center">
  <img src="https://img.shields.io/github/stars/Ayano4ka1338/svindows?style=flat-square" alt="Stars">
  <img src="https://img.shields.io/github/issues/Ayano4ka1338/svindows?style=flat-square" alt="Issues">
  <img src="https://img.shields.io/github/last-commit/Ayano4ka1338/svindows?style=flat-square" alt="Last Commit">
  <img src="https://img.shields.io/github/repo-size/Ayano4ka1338/svindows?style=flat-square" alt="Repo Size">
  <img src="https://img.shields.io/github/languages/top/Ayano4ka1338/svindows?style=flat-square" alt="Top Language">
  <img src="https://img.shields.io/github/license/Ayano4ka1338/svindows?style=flat-square" alt="License">
</p>

---

## About

Svindows is an experimental x86-64 operating system built from scratch around an NT-inspired kernel. It aims to run several program formats through separate compatibility layers, without using Microsoft source code.

## Features

- **NT kernel:** Multiboot2/GRUB boot, long mode, ring 0/3, virtual memory, processes, threads, scheduler, kernel objects and system calls.
- **Win32:** PE32+ EXE/DLL loading, PEB/TEB, native `Nt*` services, Win32 runtime, console, USER32 and GDI APIs.
- **DOS:** COM and MZ executables, DOS/BIOS interrupts, partial 386 protected mode and DPMI, with virtual VGA, PIC, PIT and keyboard devices.
- **Linux ABI:** x86-64 ELF and musl interpreter, Linux-style syscalls, file descriptors, `mmap`, `futex`, `poll`/`epoll`, processes and AF_INET sockets.
- **Networking:** VirtIO-net, lwIP, IPv4, DHCP, DNS, ICMP, UDP and TCP; basic HTTP client support.
- **Storage & graphics:** FAT32/VFAT, framebuffer console, Win32 GDI, and experimental Xorg/X11 desktop support.
- **Userland:** `cmd.exe`, Alpine-based `curl`, `fastfetch`, Xorg utilities and `feh`; bundled DOS/4G DOOM compatibility target.

Compatibility is incomplete. DOS/DPMI gameplay, sound, full POSIX/Linux behavior, Linux signals, TLS/HTTPS and broad Windows API coverage are not guaranteed. X11 applications require a running X server. See `docs/` for implementation details and limitations.

## Build and run

Build on Linux with GCC/binutils, Clang/LLD, NASM, GNU Make, Python 3, GRUB tools, `xorriso`, `mtools` and QEMU. The first build may download pinned dependencies and Alpine packages.

```sh
make             # build the ISO and FAT disk image
make run         # boot in QEMU using the serial console
make run-gui     # boot in a graphical QEMU window
make run-net     # run the network smoke test
make clean       # remove generated files
```

At the Svindows prompt, enter `help` to list shell commands. Run `startx.elf` from the graphical image to start the experimental X11 session.

```

## Project layout

- `kernel/` — NT-style kernel, DOS VM, Linux ABI, networking, FAT and Win32 graphics.
- `user/` — native PE programs, Linux ELF launchers and DOS applications.
- `boot/` — GRUB configuration; `tools/` — build, staging and checks; `docs/` — subsystem notes.

## License

MIT. Third-party components and Alpine packages retain their respective licenses. See [License](License).

---

<p align="center">
  <sub>Svindows — an experimental NT-compatible operating system for x86-64.</sub>
</p>
