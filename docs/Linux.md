# Linux ELF64 programs

## Boundary between the two program models

PE and ELF programs share the FAT-backed C: volume, but use separate process
interfaces. PE programs use NT handles and DOS paths; ELF programs use Linux
file descriptors and `/` paths. Linux path translation maps `/name` to
`C:\name` only for ordinary files. Linux PTYs, VTs, input devices, and the
framebuffer are kernel devices exposed through Linux file descriptors. A PE
process can inherit a PTY through an NT File handle that bridges to the same
device. The same handle is duplicated when an ELF process replaces itself
with a PE image, so failure does not close its original descriptor.

The process manager now carries a bounded environment across ELF-to-PE
`execve` and PE-to-ELF `CreateProcess`. Win32 exposes inherited entries through
`GetEnvironmentVariableW` and `GetEnvironmentStringsW`, and the PE C startup
passes the same entries to `main` as `envp`; `cmd.exe` passes its
current `set` entries to children. A DOS `PATH` becomes a `/`-based `PATH`
for ELF children. This carries `DISPLAY` from `startx` through xterm and
`cmd.exe` to Linux X11 clients such as `xeyes`.
The PE process-parameter command line accepts up to 767 characters; longer
requests fail instead of being silently truncated.

The path layer still maps ordinary files to FAT. A separate read-only `/proc`
provider exposes `/proc`, `/proc/self`, `/proc/version`,
`/proc/self/status`, `/proc/self/cmdline`, and the `exe` and `cwd` links.
Unknown `/proc` names do not fall through to `C:\proc`. This is a small
process view, not a complete procfs: numeric PID directories, `maps`, and
`fd` links are not yet present. `/proc/self/cmdline` currently contains only
the executable path followed by a NUL byte, rather than the full argument
vector. FAT also cannot represent Unix ownership,
symlinks, or permissions. The environment block is currently limited to
2048 bytes, and conversion of non-ASCII entries is lossy.
`link()` reports `EOPNOTSUPP` on FAT, since copying a file cannot provide
hard-link semantics.

The native `uname` syscall (63) returns the Linux UTS layout. The interactive `cmd.exe` installs a `UNAME.EXE` PE utility with the standard identity selection flags. `fastfetch` is the actual Alpine Linux v3.22 x86_64 ELF package (2.44.0-r0), staged from its APK together with the `hwdata-pci` data package and launched through the existing musl loader. The build reads the Alpine APKINDEX and verifies the APK data checksum before extracting the executable.

The disk also carries Alpine v3.22's statically linked `apk.static` as
`/sbin/apk` and `/usr/bin/apk`, plus the signed repository keys and default
`main`/`community` repository list. `apk --version` works without the musl
loader; `apk update` uses the existing HTTPS/network stack and stores indexes
under `/var/cache/apk`. APK package extraction supports FAT timestamps and
root ownership requests; FAT mode bits are advisory. Since FAT has no symlink
entry type, links to existing regular files are materialized as file copies.
`apk add tree` and running the installed `tree` command have been verified in
QEMU. Packages that need true symlink behavior, hard links, or unsupported
Linux syscalls can still fail.

Svindows has an initial, native x86-64 Linux ABI path in `kernel/linux/`.
The kernel recognizes ELF magic during process creation, validates `ET_EXEC`
and `ET_DYN` images, maps their `PT_LOAD` segments into a private user address
space, and applies page permissions. For an image with `PT_INTERP`, it loads
the named interpreter from the FAT-backed C: drive and enters that image. The
user-mode interpreter owns ELF relocations and shared-library loading. The
initial stack contains `argc`, `argv`, inherited environment entries, and auxiliary
entries including `AT_PHDR`, `AT_BASE`, `AT_ENTRY`, `AT_RANDOM`, and
`AT_EXECFN`. Linux programs use a separate syscall dispatcher; PE and DOS
programs retain their existing ABIs.

The implemented Linux syscalls are `read` (0), `write` (1), `open` (2),
`close` (3), `stat` (4), `fstat` (5), `lstat` (6), `lseek` (8),
`mmap` (9), `mprotect` (10), `munmap` (11), `brk` (12), `mremap` (25), `ioctl` (16,
fbdev, VT, evdev, and PTY requests), `pread64` (17), `readv` (19),
`writev` (20), `access` (21), `pipe` (22), `select` (23), `dup` (32),
`dup2` (33), `nanosleep` (35), `getpid` (39),
`socket` (41), `connect` (42), `accept` (43),
`sendto` (44), `recvfrom` (45), `sendmsg` (46), `recvmsg` (47),
`shutdown` (48), `bind` (49),
`listen` (50), `getsockname` (51), `getpeername` (52), `socketpair` (53),
`setsockopt` (54), `getsockopt` (55), `fork` (57), `execve` (59),
`exit` (60), `wait4` (61), `uname` (63), `fcntl` (72, descriptor and status flags,
including `F_DUPFD`),
`flock` (73), `getcwd` (79), `chdir` (80), `mkdir` (83), `unlink` (87),
`readlink` (89, framebuffer sysfs links), `chmod` (90), `fchmod` (91),
`chown` (92), `fchown` (93), `lchown` (94),
`getrlimit` (97, `RLIMIT_NOFILE`),
`getuid` (102), `getgid` (104), `setuid` (105), `setgid` (106),
`geteuid` (107), `getegid` (108), `setpgid` (109), `getppid` (110),
`getpgrp` (111), `setsid` (112), `setresuid` (117), `setresgid` (119),
`getpgid` (121), `getsid` (124), `umask` (166, advisory on FAT), `arch_prctl` (158), `gettid` (186), `futex` (202,
wait/wake), `epoll_create` (213), `getdents64` (217),
`set_tid_address` (218),
`clock_gettime` (228, realtime and monotonic), `clock_getres` (229),
`exit_group` (231), `epoll_wait` (232), `epoll_ctl` (233),
`openat` (257), `fchownat` (260), `futimesat` (261), `newfstatat` (262),
`unlinkat` (263), `renameat` (264), `symlinkat` (266, regular-file copies on FAT),
`readlinkat` (267), `fchmodat` (268), `pselect6` (270), `utimensat` (280),
`epoll_pwait` (281, without a signal mask),
`accept4` (288), `epoll_create1` (291), `dup3` (292), `pipe2` (293),
and read-only `prlimit64` (302, `RLIMIT_NOFILE`). Unknown calls
return `-ENOSYS`.

`clone` (56) supports threads sharing the process address space, file table,
and working directory. It supports TLS and parent/child TID pointers; Linux
namespace flags are not implemented. `fork` copies user pages and inherits
descriptors; `execve` replaces the image and honors close-on-exec.
`sched_yield` (24) is
also available. Thread exit clears the registered child TID and wakes one
futex waiter. Futex supports `WAIT` and `WAKE`, with or without the private
flag, and relative wait timeouts.
Linux file numbers and memory mappings live in a Linux-owned process context;
the common process structure keeps only a pointer to that context. The
process has 64 file numbers, including standard streams. Ordinary files
use the native FAT-backed file objects, so Linux-created files are visible to
native and DOS programs. `openat` supports relative paths through an open
directory descriptor or `AT_FDCWD`. The supported open flags are
access mode, `O_CREAT`, `O_EXCL`, `O_TRUNC`, `O_APPEND`, `O_LARGEFILE`, and
`O_CLOEXEC`,
plus `O_DIRECTORY` for directory opens. Directory descriptors support
`getdents64`, `fstat`, `lseek`, and `close`.
Console output and shell redirection use the process's inherited handles.
`pipe` and blocking `pipe2` use native kernel pipe objects. `pipe2` accepts
`O_CLOEXEC`; `fcntl(F_SETFL)` enables nonblocking reads on pipe descriptors.
`fcntl(F_DUPFD)` and `F_DUPFD_CLOEXEC` duplicate regular files and terminals
at or above a requested file number.
Creating a pipe with `O_NONBLOCK` remains unsupported. `dup`, `dup2`, and `dup3`
support native file and pipe descriptors. Other descriptor kinds still need
shared lifetime handling before they can be duplicated.
Linux `/dev/fb0` is a separate fbdev adapter over the boot framebuffer. It
supports `open`, `close`, `read`, `write`, `lseek`, `stat`, the fixed and variable
screen-info ioctls, fixed-mode `FBIOPUT_VSCREENINFO`, palette get/set,
`FBIOBLANK` for unblanking, and shared `mmap`. A narrow virtual sysfs adapter
exposes the links read by Xorg's fbdev probe. Framebuffer mappings alias device
memory and do not free physical pages when unmapped. While a Linux process
holds a framebuffer mapping, the text console stops painting over that display;
the console redraws after the final owner releases it. Mode setting, DRM/KMS,
virtual terminals, and general display arbitration are not implemented.

The `stat` family reports the x86-64 Linux 144-byte layout. File size, type,
read-only attribute, and FAT modification time come from the native file
layer; FAT does not provide Unix ownership, access time, change time, or
symbolic links. The synthesized UID/GID are zero. `lstat` reports the
same file as `stat` for FAT paths, and the link itself for virtual `/proc`
links. `newfstatat` accepts `AT_FDCWD` or an open directory
descriptor for relative paths and `AT_SYMLINK_NOFOLLOW`; `AT_EMPTY_PATH`
remains unsupported. Ownership is always reported as `0:0`; setting an
existing file or device to `0:0` succeeds, while other owners are refused.

`brk` begins after the highest ELF load segment. `mmap` allocates private
anonymous pages or copies a readable file into private pages; changes to a
file mapping do not write back to FAT. Mappings are currently limited to
64 MiB per call and a dedicated 4 GiB virtual address arena. `mprotect`
supports mapped pages, ELF image pages, and allocated `brk` pages, including
`PROT_NONE`; `munmap` supports partial unmapping of mmap-owned ranges.
`MAP_FIXED_NOREPLACE` and `MAP_FIXED` are supported within the arena for
Linux-owned mappings; `MAP_FIXED` can replace overlapping owned mappings.
`MAP_SHARED` is supported for `/dev/fb0`. General file-backed `MAP_SHARED`
and fixed mappings over ELF or other non-owned regions are not supported.
Pages are allocated eagerly, and access beyond a
file's end is zero-filled instead of raising Linux `SIGBUS`.

The loader rejects malformed headers, invalid ranges, overlapping load pages,
and entry points outside executable segments. Broad glibc and musl application
compatibility remains unverified. The Alpine Xorg server
creates its abstract and pathname local listeners, registers them with
`epoll`, loads the fbdev driver, maps the boot framebuffer, and initializes
the video screen and X extensions. Xorg forks a shell to run Alpine
`xkbcomp`, then loads the compiled XKB map and enters its event loop.
General shared-memory mappings, signal delivery, Linux input devices, and
virtual terminals are not implemented. The current path translation maps Linux `/` to drive C: and
resolves relative paths against
the native process working directory.

The disk includes the real Xorg 21.1.19-r0 ELF from Alpine Linux v3.22
and its direct musl-linked library closure. `tools/StageAlpineXorg.py` uses
`tools/alpine-xorg.lock.json` to download exact APK versions from the
[Alpine repositories](https://dl-cdn.alpinelinux.org/alpine/v3.22/), verify
SHA-256 digests, and stage the ELF, libraries, Xorg modules, `xkbcomp`,
standalone `dash` as `/bin/sh`, and XKB rules. The build installs the executable as `C:\XORG`, the musl loader and shared libraries
under `/lib`, and modules under `/usr/lib/xorg/modules`. A direct server probe with
`xorg :0 -nolisten tcp -nolock -logfile /tmp/Xorg.log` creates both local
listeners, uses fbdev at 1280x800, compiles its XKB map, and enters the main
loop. `-nolock` avoids Xorg's hard-link lock-file operation,
which FAT cannot represent. `AF_UNIX` currently supports `SOCK_STREAM` only;
its queues, names, and data buffers are kernel-owned. `epoll` supports
level-triggered readiness for sockets and native pipes, including registrations with
`EPOLLET`. `sendmsg` and `recvmsg` handle data vectors but not ancillary data
or descriptor passing; datagram sockets remain unsupported. FAT
cannot preserve the requested mode for `/tmp/.X11-unix`, so Xorg warns that
its mode should be 1777.

For a desktop session, run `make run-gui`, then enter `startx.elf` at the
`C:\>` prompt. The launcher connects to an existing Xorg server or starts one
on Linux VT 2, waits for a successful X11 setup reply, sets a gray mesh root
pattern with `xsetroot`, and starts `xterm` and Alpine `twm`. TWM uses
`RandomPlacement` so xterm appears without a placement click. The launcher
holds one X11 connection across setup so the server does not reset the root
window between short-lived clients. `/dev/console`, `/dev/tty`, and
`/dev/tty0` through `/dev/tty12` expose the VT/KD ioctls Xorg needs.
`/dev/ptmx` and
`/dev/pts/N` provide Unix98 pseudo terminals for xterm's shell, with basic
line discipline, termios, window-size ioctls, and poll/select readiness.
The TWM binary is installed at `/usr/bin/twm` for Linux `execve`.
`python3 tools/StartXVisualSmoke.py` captures the desktop in QEMU.
The Alpine `xeyes` client is installed at `/usr/bin/xeyes`; run `xeyes` from
the xterm shell to check X11 client startup, drawing, and pointer events.
Entering `xorg` alone starts only the X server: it has no desktop, window
manager, or client to draw, so a blank screen is expected while the server
waits for connections. The missing `udev` warning does not stop the configured
fbdev and evdev drivers from initializing.

The native Linux thread ABI now accepts the
historical `CLONE_DETACHED` bit that musl uses, and `epoll` can monitor native
pipes used by Xorg's input thread. Kernel evdev adapters expose the PS/2
keyboard as `/dev/input/event0` and mouse as `/dev/input/event1`; Xorg's
evdev driver registers them as separate XInput devices. The adapter currently
has no key repeat, LED control, or hotplug. VT_PROCESS release/acquire signals
and separate per-VT text buffers are not implemented, so switching away from
an Xorg-owned VT from another process is currently refused.

Xterm starts `/usr/bin/cmd`, a copy of the native `CMD.EXE` image. Its Linux
`execve` converts the process to the PE personality while retaining the PTY
stream through the NT File bridge. `cmd` searches C: and `C:\usr\bin`, so it
can launch both `.exe` and `.elf` programs by name. Linux GUI clients launched
from this shell inherit `DISPLAY=:0`.
