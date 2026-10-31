Svindows

<p align="center">
  <img src="other/Logo.png" alt="Svindows Logo" width="160">
</p><h3 align="center">An experimental NT-compatible operating system for x86-64.</h3><p align="center">
  <img src="https://img.shields.io/github/stars/Ayano4ka1338/svindows?style=flat-square" alt="Stars">
  <img src="https://img.shields.io/github/issues/Ayano4ka1338/svindows?style=flat-square" alt="Issues">
  <img src="https://img.shields.io/github/last-commit/Ayano4ka1338/svindows?style=flat-square" alt="Last Commit">
  <img src="https://img.shields.io/github/repo-size/Ayano4ka1338/svindows?style=flat-square" alt="Repository Size">
  <img src="https://img.shields.io/github/languages/top/Ayano4ka1338/svindows?style=flat-square" alt="Language">
  <img src="https://img.shields.io/github/license/Ayano4ka1338/svindows?style=flat-square" alt="License">
</p><p align="center">
  <img src="https://github.com/user-attachments/assets/71df715c-e44f-4b46-964a-1b1937211be4" alt="Svindows Screenshot" width="100%">
</p>---

About

Svindows is an experimental x86-64 operating system built from scratch around an NT-inspired kernel. It aims to run software from multiple operating system ecosystems through independent compatibility layers, without using Microsoft source code.

Features

- NT Kernel: Multiboot2/GRUB boot, long mode, ring 0/3, virtual memory, processes, threads, scheduler, kernel objects and system calls.
- Win32: PE32+ EXE/DLL loading, PEB/TEB, native "Nt*" services, Win32 runtime, console, USER32 and GDI APIs.
- DOS: COM and MZ executables, DOS/BIOS interrupts, partial 386 protected mode and DPMI, with virtual VGA, PIC, PIT and keyboard devices.
- Linux ABI: x86-64 ELF and musl interpreter, Linux-style syscalls, file descriptors, "mmap", "futex", "poll"/"epoll", processes and AF_INET sockets.
- Networking: VirtIO-net, lwIP, IPv4, DHCP, DNS, ICMP, UDP and TCP; basic HTTP client support.
- Storage & Graphics: FAT32/VFAT, framebuffer console, Win32 GDI and experimental Xorg/X11 desktop support.
- Userland: "cmd.exe", Alpine-based "curl", "fastfetch", Xorg utilities, "feh" and a DOS/4G DOOM compatibility target.

Compatibility is incomplete. DOS/DPMI gameplay, sound, full POSIX/Linux behavior, Linux signals, TLS/HTTPS and broad Windows API coverage are not guaranteed. X11 applications require a running X server. See ""docs/"" (docs/) for implementation details and limitations.

Build & Run

Build on Linux with GCC/binutils, Clang/LLD, NASM, GNU Make, Python 3, GRUB tools, "xorriso", "mtools" and QEMU.

git clone https://github.com/Ayano4ka1338/svindows.git
cd svindows

make             # Build ISO and FAT disk image
make run         # Boot using QEMU serial console
make run-gui     # Boot in graphical QEMU
make run-net     # Run network smoke test
make clean       # Remove generated files

At the Svindows prompt, enter "help" to list available commands.

Run "startx.elf" from the graphical image to start the experimental X11 session.

Project Structure

Directory| Description
"kernel/"| NT-style kernel, DOS VM, Linux ABI, networking, FAT and Win32 graphics
"user/"| Native PE programs, Linux ELF launchers and DOS applications
"boot/"| GRUB configuration
"tools/"| Build, staging and validation scripts
"docs/"| Implementation details and limitations

License

MIT. Third-party components and Alpine packages retain their respective licenses.
