CROSS   ?=
CC      := $(CROSS)gcc
LD      := $(CROSS)ld
NASM    := nasm
HOSTCC  ?= clang

CONFIG ?= release
FAT_FAULT_AFTER ?= -1
ifeq ($(CONFIG),release)
KERNEL_OPT := -O2
USER_OPT   := -O2
else ifeq ($(CONFIG),debug)
KERNEL_OPT := -Og
USER_OPT   := -O0
else
$(error Unknown CONFIG '$(CONFIG)'; use CONFIG=debug or CONFIG=release)
endif

BUILD   := build
BUILD_CONFIG := $(BUILD)/BuildConfig
LWIP_MAKEFILE := $(BUILD)/lwip/sources.mk
LWIP_PORT_CHECK := $(BUILD)/checks/lwip-arch-port.ok
DOS_TESTS := $(BUILD)/dos-tests/.ready

# lwIP is pinned to an upstream release commit.  Staging the included makefile
# makes GNU make re-parse after the source has been fetched and version-checked.
$(LWIP_MAKEFILE): tools/StageLwip.py tools/lwip.lock.json
	python3 tools/StageLwip.py $(BUILD)/lwip

-include $(LWIP_MAKEFILE)
LWIP_C_SRC ?=
LWIP_ROOT ?= $(abspath $(BUILD)/lwip/src)
LWIP_OBJ := $(patsubst $(LWIP_ROOT)/%.c,$(BUILD)/lwip/%.o,$(LWIP_C_SRC))
KERNEL  := $(BUILD)/svindowskrnl.elf
ISO     := $(BUILD)/svindows.iso
INITRD  := $(BUILD)/initrd.fat
LIMINE_CLI := $(BUILD)/limine-cli
LIMINE_FILES := limine-bin/BOOTX64.EFI limine-bin/limine-bios-cd.bin \
                limine-bin/limine-uefi-cd.bin limine-bin/limine-bios.sys

.DEFAULT_GOAL := all

# The PE to start after boot: the command processor, the normal system shell.
PROGRAM ?= cmd.exe
CMDLINE ?= $(PROGRAM)

# Freestanding, higher-half (-mcmodel=kernel), no FP/SIMD in the kernel, no red
# zone (interrupts run on the same stack), position-dependent.
CFLAGS := -std=gnu11 -ffreestanding \
          -fno-stack-protector -fno-stack-clash-protection \
          -fno-pic -fno-pie -no-pie \
          -mno-red-zone -mcmodel=kernel -mgeneral-regs-only \
          -fno-asynchronous-unwind-tables -MMD -MP \
          -Wall -Wextra -Wno-unused-parameter -Wno-multichar \
          $(KERNEL_OPT) -g -Ikernel/include -DSVINDOWS_BOOT_PROGRAM=\"$(PROGRAM)\" \
          -DSVINDOWS_BOOT_COMMAND_LINE='"$(CMDLINE)"' \
          -DIO_FAULT_AFTER_WRITES=$(FAT_FAULT_AFTER)
CFLAGS += -Ikernel/net/lwip -I$(LWIP_ROOT)/include

NASMFLAGS := -f elf64 -g -F dwarf

LDFLAGS := -n -nostdlib -static -no-pie -z noexecstack \
           -z max-page-size=0x1000 --no-warn-rwx-segments \
           -T kernel/arch/x86_64/Linker.ld

# Keep object and link order deterministic across hosts and filesystems.
# DOS JIT is intentionally disabled in this recovery build. Exclude any
# stale kernel/dos/Jit.c left behind when extracting a ZIP over an older tree.
C_SRC   := $(sort $(shell find kernel -name '*.c' ! -path 'kernel/dos/Jit.c'))
ASM_SRC := $(sort $(shell find kernel -name '*.asm'))
OBJ     := $(patsubst %,$(BUILD)/%.o,$(C_SRC) $(ASM_SRC))
OBJ     += $(LWIP_OBJ)
C_DEPS  := $(patsubst %.c,$(BUILD)/%.c.d,$(C_SRC))

-include $(C_DEPS)
-include $(LWIP_OBJ:.o=.d)

# Large compatibility surfaces are grouped into included implementation parts.
# They remain one translation unit while private helpers are still shared, but
# listing them here keeps incremental builds correct when a part changes.
WIN32K_USER_PARTS := $(wildcard kernel/win32k/User*.inc)
WIN32K_GDI_PARTS  := $(wildcard kernel/win32k/gdi/*.inc)
LDR_PARTS         := $(wildcard kernel/ldr/pe/*.inc)
KERNEL32_PARTS    := $(wildcard user/w/kernel32/*.inc)
USER32_PARTS      := $(wildcard user/w/user32/*.inc)
CONTROL_SRC       := $(wildcard user/w/user32/controls/*.c)
CONTROL_OBJ       := $(patsubst user/w/user32/controls/%.c,$(BUILD)/control_%.obj,$(CONTROL_SRC))
CONTROL_HEADERS   := $(wildcard user/w/user32/controls/*.h)
GDI32_PARTS       := $(wildcard user/w/gdi32/*.inc)
NTDLLRTL_PARTS    := $(wildcard user/w/ntdll/Rtl/*.inc)
MSVCRT_PARTS      := $(wildcard user/w/msvcrt/*.inc)
CMD_PARTS         := $(wildcard user/w/cmd/*.inc)

# User-space images, built as real PE32+ files and placed on a FAT disk image
# the kernel reads at runtime. The dependency chain is
#   cmd.exe -> kernel32.dll -> ntdll.dll -> syscall
# ntdll (asm) exports the Nt* syscall stubs; kernel32 (C) implements Win32 on
# top of them; cmd (C) is a normal Win32 program.
NTDLL      := $(BUILD)/ntdll.dll
NTDLLLIB   := $(BUILD)/ntdll.lib
KERNEL32   := $(BUILD)/kernel32.dll
KERNEL32LIB := $(BUILD)/kernel32.lib
ADVAPI32   := $(BUILD)/advapi32.dll
ADVAPI32LIB := $(BUILD)/advapi32.lib
MSVCRT     := $(BUILD)/msvcrt.dll
MSVCRTLIB  := $(BUILD)/msvcrt.lib
CRT0       := $(BUILD)/crt0.obj
CMD        := $(BUILD)/cmd.exe
CMDUTIL    := $(BUILD)/cmdutil.exe
# USER/GDI client layer: user32.dll and gdi32.dll provide Win32 graphics APIs.
USER32     := $(BUILD)/user32.dll
USER32LIB  := $(BUILD)/user32.lib
GDI32      := $(BUILD)/gdi32.dll
GDI32LIB   := $(BUILD)/gdi32.lib
UXTHEME    := $(BUILD)/uxtheme.dll
UXTHEMELIB := $(BUILD)/uxtheme.lib
COMCTL32   := $(BUILD)/comctl32.dll
COMCTL32LIB := $(BUILD)/comctl32.lib
NOTEPAD    := $(BUILD)/notepad.exe
CALC       := $(BUILD)/calc.exe
NOTEPAD_INC := -Iuser/w/gdi32 -Iuser/w/user32 -Iuser/w/uxtheme
COMMAND_BINS := $(addprefix $(BUILD)/,DIR.EXE TYPE.EXE CLS.EXE VER.EXE HELP.EXE \
                VOL.EXE HOSTNAME.EXE PAUSE.EXE COPY.EXE DEL.EXE ERASE.EXE \
                MD.EXE MKDIR.EXE RD.EXE RMDIR.EXE REN.EXE RENAME.EXE \
                UNAME.EXE)
STARTX     := $(BUILD)/startx.elf
PING       := $(BUILD)/PING.ELF
CURL_HTTP  := $(BUILD)/CURLHTTP.ELF
NET_TOOLS  := $(PING) $(CURL_HTTP)
NET_BUILD_REV := 2026-10-31-dpmi-regframe-dns-fallback-r23
NET_BUILD_STAMP := $(BUILD)/.network-build-rev
NET_REBUILD_OBJECTS := $(addprefix $(BUILD)/,kernel/net/Pci.c.o \
    kernel/net/VirtioNet.c.o kernel/net/Network.c.o kernel/net/Lwip.c.o \
    kernel/net/LwipSysArch.c.o kernel/ke/KeMain.c.o kernel/linux/SocketInet.c.o \
    kernel/arch/x86_64/Trap.c.o kernel/arch/x86_64/Isr.asm.o \
    kernel/linux/Syscall.c.o kernel/linux/Time.c.o kernel/ps/Psobj.c.o \
    kernel/linux/File.c.o kernel/linux/Path.c.o kernel/ps/ProcessObject.c.o \
    kernel/dos/Cpu.c.o kernel/dos/Video.c.o kernel/dos/Dos.c.o \
    kernel/dos/Interrupt.c.o kernel/hal/Keyboard.c.o)
ALPINE_XORG := $(BUILD)/alpine-xorg/.ready
ALPINE_FASTFETCH := $(BUILD)/alpine-fastfetch/.ready
ALPINE_CURL := $(BUILD)/alpine-curl/.ready
ALPINE_FEH := $(BUILD)/alpine-feh/.ready
ALPINE_APK := $(BUILD)/alpine-apk/.ready
# Keep each legacy Windows release isolated; their WIN.COM and system modules
# share names but must never overwrite one another on the FAT image.
WINDOWS1_FILES := $(sort $(wildcard user/d/windows1/*))
WINDOWS1_NAMES := $(notdir $(WINDOWS1_FILES))
WINDOWS2_FILES := $(shell find user/d/windows2 -type f -print | sort)
WINDOWS2_DIRS := $(shell find user/d/windows2 -mindepth 1 -type d -print | sort)
WINDOWS2_NAMES := $(notdir $(WINDOWS2_FILES))
WINDOWS_NAMES := $(sort $(WINDOWS1_NAMES) $(WINDOWS2_NAMES))
# Filter the Windows names as well, so stale copies cannot leak under C:\DOOM.
DOOM_FILES  := $(filter-out $(addprefix user/d/doom/,$(WINDOWS_NAMES)),$(sort $(wildcard user/d/doom/*)))
LLDLINK    := lld-link
CLANGWIN   := clang --target=x86_64-pc-windows-msvc -ffreestanding \
              -fno-stack-protector -fno-stack-check -funwind-tables \
              -Iuser/w/include $(USER_OPT)

# QEMU is configurable from the make command line. Use the portable `pc`
# machine alias instead of the version-specific `pc-i440fx` name: older and
# vendor/custom QEMU builds commonly expose only the generic alias. Defaults
# use user-mode NAT and the transitional VirtIO-net PCI interface.
QEMU                 ?= qemu-system-x86_64
QEMU_MEMORY          ?= 1024M
QEMU_MACHINE         ?= pc
QEMU_CPU             ?= max
QEMU_NET_ID          ?= svnet0
QEMU_NETDEV          ?= user,id=$(QEMU_NET_ID),ipv6=off
# Keep the device on the legacy PCI root bus and expose its legacy I/O BARs.
# The in-tree driver currently speaks the VirtIO 0.9 PCI transport.
QEMU_NET_DEVICE      ?= virtio-net-pci,netdev=$(QEMU_NET_ID),disable-modern=on,bus=pci.0,addr=0x04
QEMU_EXTRA_ARGS      ?=
QEMU_SERIAL          ?= stdio
QEMUFLAGS            := -machine $(QEMU_MACHINE) -cpu $(QEMU_CPU) -m $(QEMU_MEMORY) -no-reboot -no-shutdown -boot d \
                        -netdev $(QEMU_NETDEV) -device $(QEMU_NET_DEVICE) \
                        $(QEMU_EXTRA_ARGS)
.PHONY: all clean iso run run-gui run-net dos-tests dos-vm-check lwip-port-check network-check feh-path-check feh-fs-check .FORCE

.FORCE:

all: $(ISO) $(INITRD)

$(STARTX): user/l/startx/StartX.c user/l/runtime/Memory.c
	@mkdir -p $(dir $@)
	$(CC) -std=gnu11 -ffreestanding -fno-stack-protector -fno-pie -no-pie \
	    -mgeneral-regs-only -mno-red-zone \
	    -nostdlib -static -O2 -Wall -Wextra -Werror -Wl,-e,_start \
	    -o $@ $^

# The small native Linux-ABI ping client remains a low-level network smoke test.
$(PING): user/l/net/ping.c user/l/net/netutil.h
	@mkdir -p $(dir $@)
	$(CC) -std=gnu11 -ffreestanding -fno-builtin -fno-stack-protector \
	    -fno-pie -no-pie -mno-red-zone -nostdlib -static -O2 \
	    -Wall -Wextra -Werror -Wl,-e,_start -Wl,--build-id=none \
	    -o $@ $<

$(CURL_HTTP): user/l/net/curl.c user/l/net/netutil.h
	@mkdir -p $(dir $@)
	$(CC) -std=gnu11 -ffreestanding -fno-builtin -fno-stack-protector \
	    -fno-pie -no-pie -mno-red-zone -nostdlib -static -O2 \
	    -Wall -Wextra -Werror -Wl,-e,_start -Wl,--build-id=none \
	    -o $@ $<

# A configuration change must invalidate existing objects even without clean.
# Preserve the stamp's mtime when settings have not changed.
$(BUILD_CONFIG): .FORCE
	@mkdir -p $(BUILD)
	@printf '%s\n' 'CONFIG=$(CONFIG)' 'KERNEL_OPT=$(KERNEL_OPT)' 'USER_OPT=$(USER_OPT)' > $@.tmp
	@cmp -s $@.tmp $@ && rm $@.tmp || mv $@.tmp $@

# ZIP extraction can preserve source mtimes older than an existing build/gui.
# A new network revision stamp deliberately rebuilds the affected kernel
# objects, network tools and FAT image once, preventing stale PING.ELF or a
# stale kernel from hiding source fixes after an in-place update.
$(NET_BUILD_STAMP): .FORCE
	@mkdir -p $(BUILD)
	@printf '%s\n' '$(NET_BUILD_REV)' > $@.tmp
	@cmp -s $@.tmp $@ && rm $@.tmp || mv $@.tmp $@

$(NET_REBUILD_OBJECTS) $(NET_TOOLS) $(INITRD) $(LWIP_OBJ): $(NET_BUILD_STAMP)

DOS_COMPAT_REV := 2026-11-02-int21-0c-input-flush-r1
DOS_COMPAT_STAMP := $(BUILD)/.dos-compat-rev
DOS_COMPAT_OBJECTS := $(addprefix $(BUILD)/,kernel/dos/Interrupt.c.o \
    kernel/io/Console.c.o kernel/io/File.c.o)
$(DOS_COMPAT_STAMP): .FORCE
	@mkdir -p $(BUILD)
	@printf '%s\n' '$(DOS_COMPAT_REV)' > $@.tmp
	@cmp -s $@.tmp $@ && rm $@.tmp || mv $@.tmp $@
$(DOS_COMPAT_OBJECTS) $(DOS_TESTS) $(INITRD): $(DOS_COMPAT_STAMP)

$(OBJ) $(NTDLL) $(KERNEL32) $(ADVAPI32) $(MSVCRT) $(CRT0) $(CMD) $(CMDUTIL) $(USER32) $(GDI32) $(UXTHEME) $(COMCTL32) $(CONTROL_OBJ) $(NOTEPAD) $(CALC): $(BUILD_CONFIG)

$(BUILD)/%.c.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@


ifneq ($(PROGRAM),cmd.exe)
IMAGE_PROGRAM := $(BUILD)/$(PROGRAM)
IMAGE_PROGRAM_INPUTS := $(IMAGE_PROGRAM)
endif

# PROGRAM is supplied through CFLAGS, which make does not otherwise track.
# Recompile just the boot selector so switching PROGRAM never reuses a kernel
# containing the previous executable name.
$(BUILD)/kernel/ke/KeMain.c.o: kernel/ke/KeMain.c .FORCE
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# The fault-injection threshold is supplied through CFLAGS as well.
$(BUILD)/kernel/io/BlockCache.c.o: kernel/io/BlockCache.c .FORCE
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/kernel/ke/Syscall.c.o: $(WIN32K_USER_PARTS)
$(BUILD)/kernel/win32k/Gdiobj.c.o: $(WIN32K_GDI_PARTS)
$(BUILD)/kernel/ldr/Pe.c.o: $(LDR_PARTS)

$(BUILD)/lwip/%.o: $(LWIP_ROOT)/%.c $(LWIP_MAKEFILE)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Wno-sign-compare -Wno-type-limits -Wno-missing-field-initializers -c $< -o $@

# Compile the Svindows lwIP architecture port against upstream arch.h before
# compiling any upstream translation unit. This catches missing format/limit
# macros at the port boundary instead of deep inside api_lib.c or other files.
$(LWIP_PORT_CHECK): $(LWIP_MAKEFILE) tools/checks/LwipArchCompile.c \
                    kernel/net/lwip/arch/cc.h kernel/net/lwip/lwipopts.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MF $(LWIP_PORT_CHECK).d -Werror -fsyntax-only tools/checks/LwipArchCompile.c
	@touch $@

$(LWIP_OBJ): | $(LWIP_PORT_CHECK)

lwip-port-check: $(LWIP_PORT_CHECK)

network-check:
	python3 tools/checks/LinuxInetChecks.py

feh-path-check:
	python3 tools/checks/FehPathChecks.py

feh-fs-check:
	python3 tools/checks/FehFsChecks.py

$(BUILD)/%.asm.o: %.asm
	@mkdir -p $(dir $@)
	$(NASM) $(NASMFLAGS) $< -o $@

# ntdll.dll: the syscall-stub library, with an export table + import library.
# Each user DLL gets a distinct preferred base so the loader (which loads at the
# preferred base) never has two images collide. Ntdllrtl.c provides the C side
# (Rtl heap, critical sections, string helpers) real binaries import.
$(NTDLL): user/w/ntdll/Ntdll.asm user/w/ntdll/Ntdll.def user/w/ntdll/Ntdllrtl.c $(NTDLLRTL_PARTS)
	@mkdir -p $(BUILD)
	$(NASM) -f win64 user/w/ntdll/Ntdll.asm -o $(BUILD)/ntdll.obj
	$(CLANGWIN) -c user/w/ntdll/Ntdllrtl.c -o $(BUILD)/ntdllrtl.obj
	$(LLDLINK) /dll /noentry /machine:x64 /nodefaultlib /base:0x180000000 \
	           /def:user/w/ntdll/Ntdll.def /out:$(NTDLL) /implib:$(NTDLLLIB) \
	           $(BUILD)/ntdll.obj $(BUILD)/ntdllrtl.obj
	@echo "  DLL   $(NTDLL)"

# kernel32.dll: the Win32 subsystem library, built from C over ntdll.
$(KERNEL32): user/w/kernel32/Kernel32.c user/w/kernel32/Kernel32Process.c $(KERNEL32_PARTS) $(NTDLL)
	@mkdir -p $(BUILD)
	$(CLANGWIN) -c user/w/kernel32/Kernel32.c -o $(BUILD)/kernel32.obj
	$(CLANGWIN) -c user/w/kernel32/Kernel32Process.c -o $(BUILD)/kernel32_process.obj
	$(LLDLINK) /dll /noentry /machine:x64 /nodefaultlib /base:0x1C0000000 \
	           /out:$(KERNEL32) /implib:$(KERNEL32LIB) \
	           /export:RtlCaptureContext=ntdll.RtlCaptureContext \
	           /export:RtlLookupFunctionEntry=ntdll.RtlLookupFunctionEntry \
	           /export:RtlVirtualUnwind=ntdll.RtlVirtualUnwind \
	           $(BUILD)/kernel32.obj $(BUILD)/kernel32_process.obj $(NTDLLLIB)
	@echo "  DLL   $(KERNEL32)"

# advapi32.dll: the registry Reg* API (C), built over ntdll's Nt*Key services.
$(ADVAPI32): user/w/advapi32/Advapi32.c user/w/advapi32/AdvapiSid.inc user/w/advapi32/AdvapiToken.inc user/w/advapi32/AdvapiRegistryEnum.inc user/w/advapi32/AdvapiService.inc user/w/advapi32/AdvapiSecurityDescriptor.inc user/w/advapi32/AdvapiLsa.inc user/w/advapi32/AdvapiCrypto.inc $(NTDLL) $(KERNEL32)
	@mkdir -p $(BUILD)
	$(CLANGWIN) -c user/w/advapi32/Advapi32.c -o $(BUILD)/advapi32.obj
	$(LLDLINK) /dll /noentry /machine:x64 /nodefaultlib /base:0x1D0000000 \
	           /out:$(ADVAPI32) /implib:$(ADVAPI32LIB) \
	           $(BUILD)/advapi32.obj $(NTDLLLIB) $(KERNEL32LIB)
	@echo "  DLL   $(ADVAPI32)"

# msvcrt.dll: a tiny C runtime (printf/malloc/str*) over kernel32.
$(MSVCRT): user/w/msvcrt/Msvcrt.c $(MSVCRT_PARTS) $(KERNEL32)
	@mkdir -p $(BUILD)
	$(CLANGWIN) -c user/w/msvcrt/Msvcrt.c -o $(BUILD)/msvcrt.obj
	$(LLDLINK) /dll /noentry /machine:x64 /nodefaultlib /base:0x1E0000000 \
	           /out:$(MSVCRT) /implib:$(MSVCRTLIB) \
	           /export:RtlEnterCriticalSection=ntdll.RtlEnterCriticalSection \
	           /export:RtlLeaveCriticalSection=ntdll.RtlLeaveCriticalSection \
	           /export:??0exception@@QEAA@XZ=msvcrt_exception_ctor0 \
	           "/export:??0exception@@QEAA@AEBQEBD@Z=msvcrt_exception_ctor_msg" \
	           "/export:??0exception@@QEAA@AEBQEBDH@Z=msvcrt_exception_ctor_msg" \
	           "/export:??0exception@@QEAA@AEBV0@@Z=msvcrt_exception_ctor_copy" \
	           /export:??1exception@@UEAA@XZ=msvcrt_exception_dtor \
	           "/export:?what@exception@@UEBAPEBDXZ=msvcrt_exception_what" \
	           "/export:?terminate@@YAXXZ=msvcrt_terminate" \
	           $(BUILD)/msvcrt.obj $(KERNEL32LIB)
	@echo "  DLL   $(MSVCRT)"

# crt0.obj: the C runtime startup (mainCRTStartup). Linked into every program
# so a standard `int main()` is entered without a custom /entry.
$(CRT0): user/w/crt/Crt0.c
	@mkdir -p $(BUILD)
	$(CLANGWIN) -c user/w/crt/Crt0.c -o $(CRT0)

# cmd.exe is a normal user-mode PE. Like Windows cmd.exe, filesystem and shell
# syntax commands are built into the command processor rather than the kernel.
$(CMD): user/w/cmd/Cmd.c $(CMD_PARTS) $(CRT0) $(KERNEL32) $(NTDLL)
	@mkdir -p $(BUILD)
	$(CLANGWIN) -c user/w/cmd/Cmd.c -o $(BUILD)/cmd.obj
	$(LLDLINK) /subsystem:console /nodefaultlib /machine:x64 \
	           /out:$@ $(BUILD)/cmd.obj $(CRT0) $(KERNEL32LIB) $(NTDLLLIB)
	@echo "  PE    $@"

# gdi32.dll: the GDI client layer over the NtGdi* services. It links ntdll only:
# the shared GDI handle table and the DC_ATTR arena are ordinary user memory the
# kernel publishes per process, so object bookkeeping costs no extra syscalls.
$(GDI32): user/w/gdi32/Gdi32.c user/w/gdi32/Gdi32.h $(GDI32_PARTS) $(NTDLL)
	@mkdir -p $(BUILD)
	$(CLANGWIN) -DSV_GDI32_BUILD -Iuser/w/gdi32 -c user/w/gdi32/Gdi32.c -o $(BUILD)/gdi32.obj
	$(LLDLINK) /dll /noentry /machine:x64 /nodefaultlib /base:0x230000000 \
	           /out:$@ /implib:$(GDI32LIB) $(BUILD)/gdi32.obj $(NTDLLLIB)
	@echo "  DLL   $@"

# Shared Windows 10 control theme; renders through GDI in user mode.
$(UXTHEME): user/w/uxtheme/Uxtheme.c user/w/uxtheme/Uxtheme.h $(GDI32) $(KERNEL32)
	@mkdir -p $(BUILD)
	$(CLANGWIN) -DSV_UXTHEME_BUILD $(NOTEPAD_INC) -c user/w/uxtheme/Uxtheme.c -o $(BUILD)/uxtheme.obj
	$(LLDLINK) /dll /noentry /machine:x64 /nodefaultlib /base:0x240000000 \
	           /out:$@ /implib:$(UXTHEMELIB) $(BUILD)/uxtheme.obj $(GDI32LIB) $(KERNEL32LIB)
	@echo "  DLL   $@"

# Shared common controls are a separate Win32 library layered on USER32/GDI32.
COMCTL32_SRC := user/w/comctl32/Comctl32.c user/w/comctl32/Progress.c user/w/comctl32/Status.c \
                user/w/comctl32/Trackbar.c user/w/comctl32/Tab.c user/w/comctl32/ListView.c user/w/comctl32/Toolbar.c
COMCTL32_HEADERS := user/w/comctl32/Comctl32.h user/w/comctl32/Internal.h
$(COMCTL32): $(COMCTL32_SRC) $(COMCTL32_HEADERS) $(USER32) $(GDI32) $(KERNEL32)
	@mkdir -p $(BUILD)
	$(CLANGWIN) -DSV_COMCTL32_BUILD $(NOTEPAD_INC) -Iuser/w/comctl32 -c user/w/comctl32/Comctl32.c -o $(BUILD)/comctl32.obj
	$(CLANGWIN) -DSV_COMCTL32_BUILD $(NOTEPAD_INC) -Iuser/w/comctl32 -c user/w/comctl32/Progress.c -o $(BUILD)/comctl32-progress.obj
	$(CLANGWIN) -DSV_COMCTL32_BUILD $(NOTEPAD_INC) -Iuser/w/comctl32 -c user/w/comctl32/Status.c -o $(BUILD)/comctl32-status.obj
	$(CLANGWIN) -DSV_COMCTL32_BUILD $(NOTEPAD_INC) -Iuser/w/comctl32 -c user/w/comctl32/Trackbar.c -o $(BUILD)/comctl32-trackbar.obj
	$(CLANGWIN) -DSV_COMCTL32_BUILD $(NOTEPAD_INC) -Iuser/w/comctl32 -c user/w/comctl32/Tab.c -o $(BUILD)/comctl32-tab.obj
	$(CLANGWIN) -DSV_COMCTL32_BUILD $(NOTEPAD_INC) -Iuser/w/comctl32 -c user/w/comctl32/ListView.c -o $(BUILD)/comctl32-listview.obj
	$(CLANGWIN) -DSV_COMCTL32_BUILD $(NOTEPAD_INC) -Iuser/w/comctl32 -c user/w/comctl32/Toolbar.c -o $(BUILD)/comctl32-toolbar.obj
	$(LLDLINK) /dll /noentry /machine:x64 /nodefaultlib /base:0x250000000 \
	           /out:$@ /implib:$(COMCTL32LIB) $(BUILD)/comctl32.obj \
	           $(BUILD)/comctl32-progress.obj $(BUILD)/comctl32-status.obj \
	           $(BUILD)/comctl32-trackbar.obj $(BUILD)/comctl32-tab.obj \
	           $(BUILD)/comctl32-listview.obj $(BUILD)/comctl32-toolbar.obj \
	           $(USER32LIB) $(GDI32LIB) $(KERNEL32LIB)
	@echo "  DLL   $@"

$(BUILD)/control_%.obj: user/w/user32/controls/%.c $(CONTROL_HEADERS) user/w/user32/User32.h user/w/gdi32/Gdi32.h user/w/uxtheme/Uxtheme.h | $(BUILD_CONFIG)
	@mkdir -p $(BUILD)
	$(CLANGWIN) -DSV_USER32_BUILD $(NOTEPAD_INC) -c $< -o $@

# user32.dll: the USER client layer over the NtUser* services. GetModuleHandleW
# and LoadStringW are exported as forwarders to kernel32, which owns the loader
# and resource metadata -- the same split Windows uses between user32 and
# kernelbase, and the reason no module list is duplicated here.
$(USER32): user/w/user32/User32.c user/w/user32/User32.h $(USER32_PARTS) $(CONTROL_SRC) $(CONTROL_HEADERS) $(CONTROL_OBJ) $(UXTHEME) $(GDI32) $(NTDLL) $(KERNEL32)
	@mkdir -p $(BUILD)
	$(CLANGWIN) -DSV_USER32_BUILD $(NOTEPAD_INC) -c user/w/user32/User32.c -o $(BUILD)/user32.obj
	$(LLDLINK) /dll /noentry /machine:x64 /nodefaultlib /base:0x220000000 \
	           /out:$@ /implib:$(USER32LIB) \
	           /export:GetModuleHandleW=kernel32.GetModuleHandleW \
	           /export:LoadStringW=kernel32.LoadStringW \
	           $(BUILD)/user32.obj $(CONTROL_OBJ) $(UXTHEMELIB) $(GDI32LIB) $(KERNEL32LIB) $(NTDLLLIB)
	@echo "  DLL   $@"

# Stateless console commands are separate programs. One implementation is
# linked once and installed under each command/alias name; it selects behavior
# from argv[0], like a compact multi-call toolset without shell-resident code.
$(CMDUTIL): user/w/cmdutils/Cmdutils.c $(CRT0) $(KERNEL32)
	@mkdir -p $(BUILD)
	$(CLANGWIN) -c user/w/cmdutils/Cmdutils.c -o $(BUILD)/cmdutils.obj
	$(LLDLINK) /subsystem:console /nodefaultlib /machine:x64 \
	           /out:$@ $(BUILD)/cmdutils.obj $(CRT0) $(KERNEL32LIB)
	@echo "  PE    $@"

$(COMMAND_BINS): $(CMDUTIL)
	cp $(CMDUTIL) $@

# FAT32 initrd holding the user-space executables, read through the kernel's
# memory-backed block device and FAT driver at runtime.
$(ALPINE_XORG): tools/StageAlpineXorg.py tools/alpine-xorg.lock.json tools/AlpineFetch.py
	python3 $< $(BUILD)/alpine-xorg

$(ALPINE_FASTFETCH): tools/StageAlpineFastfetch.py tools/alpine-fastfetch.lock.json tools/AlpineFetch.py
	python3 $< $(BUILD)/alpine-fastfetch

$(ALPINE_CURL): tools/StageAlpineCurl.py tools/alpine-curl.lock.json tools/AlpineFetch.py
	python3 $< $(BUILD)/alpine-curl

$(ALPINE_FEH): tools/StageAlpineFeh.py tools/alpine-feh.lock.json tools/AlpineFetch.py
	python3 $< $(BUILD)/alpine-feh

$(ALPINE_APK): tools/StageAlpineApk.py tools/alpine-apk.lock.json tools/AlpineFetch.py
	python3 $< $(BUILD)/alpine-apk

$(DOS_TESTS): tools/BuildDosTests.py
	python3 $< $(BUILD)/dos-tests

$(INITRD): Makefile etc/resolv.conf $(DOS_TESTS) $(NET_TOOLS) $(CMD) $(COMMAND_BINS) $(KERNEL32) $(NTDLL) $(ADVAPI32) $(MSVCRT) $(USER32) $(GDI32) $(UXTHEME) $(STARTX) $(ALPINE_XORG) $(ALPINE_FASTFETCH) $(ALPINE_CURL) $(ALPINE_FEH) $(ALPINE_APK) $(wildcard user/d/doom) $(DOOM_FILES) $(wildcard user/d/windows1) $(WINDOWS1_FILES) $(wildcard user/d/windows2) $(WINDOWS2_FILES) user/d/win.bat user/d/win2.bat user/d/win3.bat other/Logo.png $(IMAGE_PROGRAM_INPUTS)
	@mkdir -p $(BUILD)
	dd if=/dev/zero of=$(INITRD) bs=1M count=200 status=none
	mformat -i $(INITRD) -F -v SVINDOWS ::
	mcopy -i $(INITRD) $(CMD) ::CMD.EXE
	mcopy -i $(INITRD) $(KERNEL32) ::KERNEL32.DLL
	mcopy -i $(INITRD) $(NTDLL) ::NTDLL.DLL
	mcopy -i $(INITRD) $(ADVAPI32) ::ADVAPI32.DLL
	mcopy -i $(INITRD) $(MSVCRT) ::MSVCRT.DLL
	mcopy -i $(INITRD) $(USER32) ::USER32.DLL
	mcopy -i $(INITRD) $(GDI32) ::GDI32.DLL
	mcopy -i $(INITRD) $(UXTHEME) ::UXTHEME.DLL
	if [ -f "$(COMCTL32)" ]; then mcopy -i $(INITRD) $(COMCTL32) ::COMCTL32.DLL; fi
	mmd -i $(INITRD) ::WINDOWS ::WINDOWS/SYSTEM32
	mcopy -i $(INITRD) $(KERNEL32) ::WINDOWS/SYSTEM32/KERNEL32.DLL
	mcopy -i $(INITRD) $(NTDLL) ::WINDOWS/SYSTEM32/NTDLL.DLL
	mcopy -i $(INITRD) $(ADVAPI32) ::WINDOWS/SYSTEM32/ADVAPI32.DLL
	mcopy -i $(INITRD) $(MSVCRT) ::WINDOWS/SYSTEM32/MSVCRT.DLL
	mcopy -i $(INITRD) $(USER32) ::WINDOWS/SYSTEM32/USER32.DLL
	mcopy -i $(INITRD) $(GDI32) ::WINDOWS/SYSTEM32/GDI32.DLL
	mcopy -i $(INITRD) $(UXTHEME) ::WINDOWS/SYSTEM32/UXTHEME.DLL
	if [ -f "$(COMCTL32)" ]; then mcopy -i $(INITRD) $(COMCTL32) ::WINDOWS/SYSTEM32/COMCTL32.DLL; fi
	mcopy -i $(INITRD) $(COMMAND_BINS) ::
	# Keep each Windows release together; root batch files are launchers.
	mmd -i $(INITRD) ::WINDOWS1 ::WINDOWS2
	if [ -n "$(WINDOWS1_FILES)" ]; then mcopy -i $(INITRD) $(WINDOWS1_FILES) ::WINDOWS1/; fi
	for d in $(WINDOWS2_DIRS); do \
	    rel=$${d#user/d/windows2/}; \
	    mmd -i $(INITRD) "::WINDOWS2/$$rel"; \
	done
	for f in $(WINDOWS2_FILES); do \
	    rel=$${f#user/d/windows2/}; \
	    mcopy -i $(INITRD) "$$f" "::WINDOWS2/$$rel"; \
	done
	mcopy -i $(INITRD) user/d/win.bat ::WIN.BAT
	mcopy -i $(INITRD) user/d/win2.bat ::WIN2.BAT
	mcopy -i $(INITRD) user/d/win3.bat ::WIN3.BAT
	mcopy -i $(INITRD) $(STARTX) ::STARTX.ELF
	mmd -i $(INITRD) ::TESTS
	mcopy -i $(INITRD) $(BUILD)/dos-tests/COMSMOKE.COM ::TESTS/COMSMOKE.COM
	mcopy -i $(INITRD) $(BUILD)/dos-tests/MEMSMOKE.COM ::TESTS/MEMSMOKE.COM
	mcopy -i $(INITRD) $(BUILD)/dos-tests/DPMIINFO.COM ::TESTS/DPMIINFO.COM
	mcopy -i $(INITRD) $(BUILD)/dos-tests/FCBPARSE.COM ::TESTS/FCBPARSE.COM
	mcopy -i $(INITRD) $(BUILD)/dos-tests/INPFLUSH.COM ::TESTS/INPFLUSH.COM
	mcopy -i $(INITRD) $(BUILD)/dos-tests/FFGROUP.COM ::TESTS/FFGROUP.COM
	mcopy -i $(INITRD) $(BUILD)/dos-tests/MZSMOKE.EXE ::TESTS/MZSMOKE.EXE
	mcopy -i $(INITRD) $(BUILD)/dos-tests/DOSCHECK.BAT ::DOSCHECK.BAT
	mcopy -s -i $(INITRD) $(BUILD)/alpine-xorg/root/* ::
	mcopy -o -s -i $(INITRD) $(BUILD)/alpine-curl/root/* ::
	mcopy -o -s -i $(INITRD) $(BUILD)/alpine-feh/root/* ::
	mcopy -o -s -i $(INITRD) $(BUILD)/alpine-apk/root/* ::
	# mcopy -s may omit empty source directories; apk needs these parents when
	# it atomically renames newly installed package files into /usr/bin and
	# /usr/lib. Check first because other staged packages may already populate
	# either directory in the case-insensitive FAT image.
	for d in USR/BIN USR/LIB; do \
	    mdir -i $(INITRD) "::$$d" >/dev/null 2>&1 || mmd -i $(INITRD) "::$$d"; \
	done
	mcopy -i $(INITRD) $(BUILD)/UNAME.EXE ::USR/BIN/UNAME
	mcopy -i $(INITRD) $(BUILD)/alpine-fastfetch/usr/bin/fastfetch ::USR/BIN/FASTFETCH
	mmd -i $(INITRD) ::USR/SHARE/FASTFETCH ::USR/SHARE/HWDATA
	mcopy -s -i $(INITRD) $(BUILD)/alpine-fastfetch/usr/share/fastfetch/* ::USR/SHARE/FASTFETCH/
	mcopy -i $(INITRD) $(BUILD)/alpine-fastfetch/usr/share/hwdata/pci.ids ::USR/SHARE/HWDATA/PCI.IDS
	mcopy -i $(INITRD) $(BUILD)/alpine-fastfetch/etc/os-release ::ETC/OS-RELEASE
	mcopy -i $(INITRD) etc/resolv.conf ::ETC/RESOLV.CONF
	# The Linux-side name for the command processor: /usr/bin/cmd is the
	# same PE image as C:\CMD.EXE, launched through execve from xterm.
	# The extensionless copy lets the Linux path /usr/bin/cmd resolve.
	mcopy -i $(INITRD) $(CMD) ::USR/BIN/CMD
	mcopy -i $(INITRD) $(BUILD)/alpine-xorg/root/usr/bin/twm ::TWM.ELF
	mcopy -i $(INITRD) $(BUILD)/alpine-xorg/root/usr/bin/xterm ::XTERM.ELF
	# Also expose the upstream client under an explicit TLS-oriented name.
	mcopy -o -i $(INITRD) $(BUILD)/alpine-curl/root/usr/bin/curl ::USR/BIN/CURLTLS
	mcopy -o -i $(INITRD) $(BUILD)/alpine-curl/root/usr/bin/curl ::USR/BIN/CURLTLS.ELF
	# The packaged Alpine curl links against OpenSSL and the installed CA bundle.
	mcopy -o -i $(INITRD) $(BUILD)/alpine-curl/root/usr/bin/curl ::USR/BIN/CURL
	mcopy -o -i $(INITRD) $(BUILD)/alpine-curl/root/usr/bin/curl ::USR/BIN/CURL.ELF
	# Extensionless Linux command paths plus .ELF names for the Windows shell.
	mcopy -i $(INITRD) $(PING) ::USR/BIN/PING
	mcopy -i $(INITRD) $(PING) ::USR/BIN/PING.ELF
	mcopy -i $(INITRD) $(CURL_HTTP) ::USR/BIN/CURLHTTP.ELF
	mmd -i $(INITRD) ::DEV ::DEV/PTS
	mmd -i $(INITRD) ::TMP
	# Provide a known-good image for the first feh smoke test.
	mcopy -i $(INITRD) other/Logo.png ::LOGO.PNG
	# Give POSIX programs a real HOME rather than leaving HOME unset.
	mmd -i $(INITRD) ::HOME ::HOME/USER ::HOME/USER/.CONFIG ::HOME/USER/.CONFIG/FEH
	if [ -n "$(DOOM_FILES)" ]; then \
	    mmd -i $(INITRD) ::DOOM && \
	    mcopy -i $(INITRD) $(DOOM_FILES) ::DOOM/ && \
	    mcopy -i $(INITRD) user/d/doom/doom.bat ::DOOM.BAT; \
	fi
	# Xorg writes its default log to /var/log/Xorg.0.log; the Alpine package
	# owns /var/log, so the emulated C: root must provide it. FAT mirrors the
	# Linux path /var/log as C:\var\log.
	for d in VAR VAR/LOG VAR/CACHE VAR/CACHE/APK; do \
	    mdir -i $(INITRD) "::$$d" >/dev/null 2>&1 || mmd -i $(INITRD) "::$$d"; \
	done
	if [ -n "$(IMAGE_PROGRAM)" ]; then mcopy -i $(INITRD) $(IMAGE_PROGRAM) ::$(PROGRAM); fi

	@echo "  INITRD $(INITRD)"

$(KERNEL): $(OBJ) $(LWIP_MAKEFILE) kernel/arch/x86_64/Linker.ld
	@mkdir -p $(dir $@)
	$(LD) $(LDFLAGS) -o $@ $(OBJ)
	@echo "  LD    $@"

iso: $(ISO)

dos-tests: $(DOS_TESTS)

dos-vm-check:
	python3 tools/checks/DosInputChecks.py
	python3 tools/checks/DosMemoryChecks.py
	python3 tools/checks/DosMultiplexChecks.py
	python3 tools/checks/DosPspTerminationChecks.py
	python3 tools/checks/DosExecChecks.py user/d/windows1/WIN100.BIN user/d/windows2/WIN200.BIN
	python3 tools/checks/DosVmChecks.py
	python3 tools/checks/DosCpuChecks.py
	python3 tools/checks/DosLoaderChecks.py
	python3 tools/checks/DosHardwareChecks.py
	python3 tools/checks/DosFcbSearchChecks.py
	python3 tools/checks/DosVgaChecks.py
	python3 tools/checks/DosCursorChecks.py
	python3 tools/checks/DosDecimalChecks.py
	python3 tools/checks/DosSystemDataChecks.py
	python3 tools/checks/DosX87Checks.py

$(LIMINE_CLI): limine-bin/limine.c limine-bin/limine-bios-hdd.h
	@mkdir -p $(dir $@)
	$(HOSTCC) -O2 -std=c11 $< -o $@

$(ISO): Makefile $(KERNEL) $(INITRD) boot/limine.conf other/Logo.png $(LIMINE_FILES) $(LIMINE_CLI)
	@mkdir -p $(BUILD)/limine-isodir/boot/limine $(BUILD)/limine-isodir/EFI/BOOT
	@cp $(KERNEL) $(BUILD)/limine-isodir/boot/svindowskrnl.elf
	@cp $(INITRD) $(BUILD)/limine-isodir/boot/initrd.fat
	@cp boot/limine.conf $(BUILD)/limine-isodir/boot/limine/limine.conf
	@cp other/Logo.png $(BUILD)/limine-isodir/logo.png
	@cp limine-bin/limine-bios.sys limine-bin/limine-bios-cd.bin \
	    limine-bin/limine-uefi-cd.bin $(BUILD)/limine-isodir/boot/limine/
	@cp limine-bin/BOOTX64.EFI $(BUILD)/limine-isodir/EFI/BOOT/BOOTX64.EFI
	xorriso -as mkisofs -b boot/limine/limine-bios-cd.bin \
	    -no-emul-boot -boot-load-size 4 -boot-info-table \
	    -eltorito-alt-boot -e boot/limine/limine-uefi-cd.bin \
	    -no-emul-boot -isohybrid-gpt-basdat -o $@ $(BUILD)/limine-isodir
	$(LIMINE_CLI) bios-install $@
	@echo "  ISO   $@"

run: all
	$(QEMU) $(QEMUFLAGS) -cdrom $(ISO) -serial $(QEMU_SERIAL) -display none

run-gui:
	$(MAKE) BUILD=build/gui PROGRAM=cmd.exe CMDLINE=cmd.exe all
	$(QEMU) $(QEMUFLAGS) -cdrom build/gui/svindows.iso \
	    -serial $(QEMU_SERIAL)

# Explicit alias for testing DHCP/DNS/TCP/HTTP with the kernel's lwIP smoke
# test. The test result is printed to the serial console during boot.
run-net: run-gui

clean:
	rm -rf $(BUILD)
