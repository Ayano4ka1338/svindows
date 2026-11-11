CROSS   ?=
CC      := $(CROSS)gcc
LD      := $(CROSS)ld
NASM    := nasm
HOSTCC  ?= clang

CONFIG ?= release
FAT_FAULT_AFTER ?= -1
ifeq ($(CONFIG),release)
KERNEL_OPT := -O2 -DSVINDOWS_RELEASE
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
C_SRC   := $(sort $(shell find kernel -name '*.c'))
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
WINSPOOL   := $(BUILD)/winspool.drv
COMDLG32   := $(BUILD)/comdlg32.dll
UXTHEME    := $(BUILD)/uxtheme.dll
UXTHEMELIB := $(BUILD)/uxtheme.lib
COMCTL32   := $(BUILD)/comctl32.dll
COMCTL32LIB := $(BUILD)/comctl32.lib
SHELL32    := $(BUILD)/shell32.dll
SHELL32LIB := $(BUILD)/shell32.lib
ADVPACK    := $(BUILD)/advpack.dll
ACLUI      := $(BUILD)/aclui.dll
AUTHZ      := $(BUILD)/authz.dll
SHLWAPI    := $(BUILD)/shlwapi.dll
GDIPLUS    := $(BUILD)/gdiplus.dll
OLEAUT32   := $(BUILD)/oleaut32.dll
RPCRT4     := $(BUILD)/rpcrt4.dll
WINMM      := $(BUILD)/winmm.dll
WINDOWSCODECS := $(BUILD)/windowscodecs.dll
IPHLPAPI := $(BUILD)/iphlpapi.dll
TASK_SERVICES := $(addprefix $(BUILD)/,pcwum.dll secur32.dll wevtapi.dll credui.dll)
TASK_SERVICE_BASE_pcwum := 0x330000000
TASK_SERVICE_BASE_secur32 := 0x340000000
TASK_SERVICE_BASE_wevtapi := 0x350000000
TASK_SERVICE_BASE_credui := 0x360000000
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
WINDOWS3_FILES := $(shell find user/d/windows3 -type f -print | sort)
WINDOWS3_DIRS := $(shell find user/d/windows3 -mindepth 1 -type d -print | sort)
WINDOWS3_NAMES := $(notdir $(WINDOWS3_FILES))
WINDOWS7_FILES := $(sort $(wildcard user/w/windows7/*))
WINDOWS_FILES := $(sort $(wildcard user/w/windows/*))
WINDOWS_NAMES := $(sort $(WINDOWS1_NAMES) $(WINDOWS3_NAMES))
# Filter the Windows names as well, so stale copies cannot leak under C:\DOOM.
DOOM_FILES  := $(filter-out $(addprefix user/d/doom/,$(WINDOWS_NAMES)),$(sort $(wildcard user/d/doom/*)))
DOOM_NESTED_FILES := $(shell find user/d/doom -mindepth 2 -type f -print | sort)
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
QEMU_ACCEL           ?= $(shell if test -r /dev/kvm -a -w /dev/kvm; then echo kvm; else echo tcg; fi)
QEMU_CPU             ?= max
QEMU_NET_ID          ?= svnet0
QEMU_NETDEV          ?= user,id=$(QEMU_NET_ID),ipv6=off
# Keep the device on the legacy PCI root bus and expose its legacy I/O BARs.
# The in-tree driver currently speaks the VirtIO 0.9 PCI transport.
QEMU_NET_DEVICE      ?= virtio-net-pci,netdev=$(QEMU_NET_ID),disable-modern=on,bus=pci.0,addr=0x04
QEMU_AUDIO_BACKEND   ?= pa
QEMU_EXTRA_ARGS      ?=
QEMU_SERIAL          ?= stdio
QEMUFLAGS            := -machine $(QEMU_MACHINE),accel=$(QEMU_ACCEL) -cpu $(QEMU_CPU) -m $(QEMU_MEMORY) -no-reboot -no-shutdown -boot d \
                        -netdev $(QEMU_NETDEV) -device $(QEMU_NET_DEVICE) \
                        -audiodev $(QEMU_AUDIO_BACKEND),id=svaudio0 \
                        -device sb16,audiodev=svaudio0,irq=5,dma=1,dma16=5 \
                        $(QEMU_EXTRA_ARGS)
.PHONY: all clean iso run run-gui run-net dos-tests dos-vm-check dos-x11-check dos-performance-check win16-agent lwip-port-check network-check feh-path-check feh-fs-check ui-font .FORCE

.FORCE:

all: $(ISO) $(INITRD)

$(BUILD)/GenerateUiFont: tools/GenerateUiFont.c tools/third_party/stb_truetype.h tools/third_party/DejaVuSans.ttf
	@mkdir -p $(dir $@)
	$(HOSTCC) -O2 -Wall -Wextra -Itools/third_party -o $@ $< -lm

build/fontcache/selawk.ttf: tools/FetchUiFont.py
	python3 tools/FetchUiFont.py $@

ui-font: $(BUILD)/GenerateUiFont build/fontcache/selawk.ttf
	$< build/fontcache/selawk.ttf kernel/hal/UiFontData.c tools/third_party/DejaVuSans.ttf

$(STARTX): user/l/startx/StartX.c user/l/runtime/Memory.c
	@mkdir -p $(dir $@)
	$(CC) -std=gnu11 -ffreestanding -fno-stack-protector -fno-pie -no-pie \
	    -mgeneral-regs-only -mno-red-zone \
	    -nostdlib -static -O2 -Wall -Wextra -Werror -Wl,-e,_start \
	    -o $@ $^

# The small native Linux-ABI ping client remains a low-level network smoke test.
$(PING): user/l/net/ping.c user/l/net/netutil.h user/l/runtime/Memory.c
	@mkdir -p $(dir $@)
	$(CC) -std=gnu11 -ffreestanding -fno-builtin -fno-stack-protector \
	    -fno-pie -no-pie -mno-red-zone -nostdlib -static -O2 \
	    -Wall -Wextra -Werror -Wl,-e,_start -Wl,--build-id=none \
	    -o $@ user/l/net/ping.c user/l/runtime/Memory.c

$(CURL_HTTP): user/l/net/curl.c user/l/net/netutil.h user/l/runtime/Memory.c
	@mkdir -p $(dir $@)
	$(CC) -std=gnu11 -ffreestanding -fno-builtin -fno-stack-protector \
	    -fno-pie -no-pie -mno-red-zone -nostdlib -static -O2 \
	    -Wall -Wextra -Werror -Wl,-e,_start -Wl,--build-id=none \
	    -o $@ user/l/net/curl.c user/l/runtime/Memory.c

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

$(OBJ) $(NTDLL) $(KERNEL32) $(ADVAPI32) $(MSVCRT) $(CRT0) $(CMD) $(CMDUTIL) $(USER32) $(GDI32) $(WINSPOOL) $(COMDLG32) $(UXTHEME) $(COMCTL32) $(SHELL32) $(ADVPACK) $(ACLUI) $(AUTHZ) $(SHLWAPI) $(GDIPLUS) $(OLEAUT32) $(RPCRT4) $(WINMM) $(WINDOWSCODECS) $(IPHLPAPI) $(TASK_SERVICES) $(CONTROL_OBJ) $(NOTEPAD) $(CALC): $(BUILD_CONFIG)

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
	           /export:RegOpenKeyExA=advapi32.RegOpenKeyExA \
	           /export:RegQueryValueExA=advapi32.RegQueryValueExA \
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
	           "/export:??1type_info@@UEAA@XZ=msvcrt_type_info_dtor" \
	           "/export:??2@YAPEAX_K@Z=msvcrt_operator_new" \
	           "/export:??3@YAXPEAX@Z=msvcrt_operator_delete" \
	           /export:_CxxThrowException=__CxxThrowException \
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

$(WINSPOOL): user/w/winspool/Winspool.c $(NTDLL)
	@mkdir -p $(BUILD)
	$(CLANGWIN) -c $< -o $(BUILD)/winspool.obj
	$(LLDLINK) /dll /noentry /machine:x64 /nodefaultlib /base:0x2D0000000 \
	           /out:$@ $(BUILD)/winspool.obj $(NTDLLLIB)
	@echo "  DLL   $@"

$(COMDLG32): user/w/comdlg32/Comdlg32.c $(USER32) $(KERNEL32) $(MSVCRT)
	@mkdir -p $(BUILD)
	$(CLANGWIN) $(NOTEPAD_INC) -c $< -o $(BUILD)/comdlg32.obj
	$(LLDLINK) /dll /noentry /machine:x64 /nodefaultlib /base:0x2E0000000 \
	           /out:$@ $(BUILD)/comdlg32.obj $(USER32LIB) $(KERNEL32LIB) $(MSVCRTLIB)
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
                user/w/comctl32/Trackbar.c user/w/comctl32/Tab.c user/w/comctl32/ListView.c user/w/comctl32/Toolbar.c user/w/comctl32/DateTime.c user/w/comctl32/UpDown.c user/w/comctl32/TaskDialog.c user/w/comctl32/TreeView.c user/w/comctl32/ClientRect.inc user/w/comctl32/MenuHelp.inc
COMCTL32_HEADERS := user/w/comctl32/Comctl32.h user/w/comctl32/Internal.h user/w/comctl32/PointerArray.inc
$(COMCTL32): $(COMCTL32_SRC) $(COMCTL32_HEADERS) user/w/comctl32/Comctl32.def $(USER32) $(GDI32) $(KERNEL32)
	@mkdir -p $(BUILD)
	$(CLANGWIN) -DSV_COMCTL32_BUILD $(NOTEPAD_INC) -Iuser/w/comctl32 -c user/w/comctl32/Comctl32.c -o $(BUILD)/comctl32.obj
	$(CLANGWIN) -DSV_COMCTL32_BUILD $(NOTEPAD_INC) -Iuser/w/comctl32 -c user/w/comctl32/Progress.c -o $(BUILD)/comctl32-progress.obj
	$(CLANGWIN) -DSV_COMCTL32_BUILD $(NOTEPAD_INC) -Iuser/w/comctl32 -c user/w/comctl32/Status.c -o $(BUILD)/comctl32-status.obj
	$(CLANGWIN) -DSV_COMCTL32_BUILD $(NOTEPAD_INC) -Iuser/w/comctl32 -c user/w/comctl32/Trackbar.c -o $(BUILD)/comctl32-trackbar.obj
	$(CLANGWIN) -DSV_COMCTL32_BUILD $(NOTEPAD_INC) -Iuser/w/comctl32 -c user/w/comctl32/Tab.c -o $(BUILD)/comctl32-tab.obj
	$(CLANGWIN) -DSV_COMCTL32_BUILD $(NOTEPAD_INC) -Iuser/w/comctl32 -c user/w/comctl32/ListView.c -o $(BUILD)/comctl32-listview.obj
	$(CLANGWIN) -DSV_COMCTL32_BUILD $(NOTEPAD_INC) -Iuser/w/comctl32 -c user/w/comctl32/Toolbar.c -o $(BUILD)/comctl32-toolbar.obj
	$(CLANGWIN) -DSV_COMCTL32_BUILD $(NOTEPAD_INC) -Iuser/w/comctl32 -c user/w/comctl32/DateTime.c -o $(BUILD)/comctl32-datetime.obj
	$(CLANGWIN) -DSV_COMCTL32_BUILD $(NOTEPAD_INC) -Iuser/w/comctl32 -c user/w/comctl32/UpDown.c -o $(BUILD)/comctl32-updown.obj
	$(CLANGWIN) -DSV_COMCTL32_BUILD $(NOTEPAD_INC) -Iuser/w/comctl32 -c user/w/comctl32/TreeView.c -o $(BUILD)/comctl32-treeview.obj
	$(CLANGWIN) -DSV_COMCTL32_BUILD $(NOTEPAD_INC) -Iuser/w/comctl32 -c user/w/comctl32/TaskDialog.c -o $(BUILD)/comctl32-taskdialog.obj
	$(LLDLINK) /dll /noentry /machine:x64 /nodefaultlib /base:0x250000000 \
	           /def:user/w/comctl32/Comctl32.def \
	           /out:$@ /implib:$(COMCTL32LIB) $(BUILD)/comctl32.obj \
	           $(BUILD)/comctl32-progress.obj $(BUILD)/comctl32-status.obj \
	           $(BUILD)/comctl32-trackbar.obj $(BUILD)/comctl32-tab.obj \
	           $(BUILD)/comctl32-listview.obj $(BUILD)/comctl32-toolbar.obj $(BUILD)/comctl32-datetime.obj $(BUILD)/comctl32-updown.obj $(BUILD)/comctl32-taskdialog.obj $(BUILD)/comctl32-treeview.obj \
	           $(USER32LIB) $(GDI32LIB) $(KERNEL32LIB)
	@echo "  DLL   $@"

$(SHELL32): user/w/shell32/Shell32.c user/w/shell32/Shell32.def $(wildcard user/w/shell32/*.inc) $(KERNEL32) $(USER32)
	@mkdir -p $(BUILD)
	$(CLANGWIN) -c user/w/shell32/Shell32.c -o $(BUILD)/shell32.obj
	$(LLDLINK) /dll /noentry /machine:x64 /nodefaultlib /base:0x260000000 \
	           /def:user/w/shell32/Shell32.def /out:$@ /implib:$(SHELL32LIB) \
	           $(BUILD)/shell32.obj $(KERNEL32LIB) $(USER32LIB)
	@echo "  DLL   $@"

$(ADVPACK): user/w/advpack/Advpack.c
	@mkdir -p $(BUILD)
	$(CLANGWIN) -c user/w/advpack/Advpack.c -o $(BUILD)/advpack.obj
	$(LLDLINK) /dll /noentry /machine:x64 /nodefaultlib /base:0x310000000 /out:$@ $(BUILD)/advpack.obj

$(ACLUI): user/w/aclui/Aclui.c user/w/aclui/Aclui.def $(KERNEL32)
	@mkdir -p $(BUILD)
	$(CLANGWIN) -c user/w/aclui/Aclui.c -o $(BUILD)/aclui.obj
	$(LLDLINK) /dll /noentry /machine:x64 /nodefaultlib /base:0x300000000 /def:user/w/aclui/Aclui.def /out:$@ $(BUILD)/aclui.obj $(KERNEL32LIB)

$(AUTHZ): user/w/authz/Authz.c $(KERNEL32)
	@mkdir -p $(BUILD)
	$(CLANGWIN) -c user/w/authz/Authz.c -o $(BUILD)/authz.obj
	$(LLDLINK) /dll /noentry /machine:x64 /nodefaultlib /base:0x2F0000000 /out:$@ $(BUILD)/authz.obj $(KERNEL32LIB)

$(SHLWAPI): user/w/shlwapi/Shlwapi.c user/w/shlwapi/Shlwapi.def $(wildcard user/w/shlwapi/*.inc) $(KERNEL32) $(USER32)
	@mkdir -p $(BUILD)
	$(CLANGWIN) -c user/w/shlwapi/Shlwapi.c -o $(BUILD)/shlwapi.obj
	$(LLDLINK) /dll /noentry /machine:x64 /nodefaultlib /base:0x270000000 \
	           /def:user/w/shlwapi/Shlwapi.def /out:$@ $(BUILD)/shlwapi.obj $(KERNEL32LIB) $(USER32LIB)
	@echo "  DLL   $@"

$(GDIPLUS): user/w/gdiplus/Gdiplus.c user/w/gdiplus/Gdiplus.def $(GDI32) $(KERNEL32) $(MSVCRT)
	@mkdir -p $(BUILD)
	$(CLANGWIN) -Iuser/w/gdi32 -c user/w/gdiplus/Gdiplus.c -o $(BUILD)/gdiplus.obj
	$(LLDLINK) /dll /noentry /machine:x64 /nodefaultlib /base:0x280000000 \
	           /def:user/w/gdiplus/Gdiplus.def /out:$@ /implib:$(BUILD)/gdiplus.lib \
	           $(BUILD)/gdiplus.obj $(GDI32LIB) $(KERNEL32LIB) $(MSVCRTLIB)
	@echo "  DLL   $@"

$(OLEAUT32): user/w/oleaut32/Oleaut32.c user/w/oleaut32/Oleaut32.def $(KERNEL32)
	@mkdir -p $(BUILD)
	$(CLANGWIN) -c user/w/oleaut32/Oleaut32.c -o $(BUILD)/oleaut32.obj
	$(LLDLINK) /dll /noentry /machine:x64 /nodefaultlib /base:0x290000000 \
	           /def:user/w/oleaut32/Oleaut32.def /out:$@ /implib:$(BUILD)/oleaut32.lib \
	           $(BUILD)/oleaut32.obj $(KERNEL32LIB)
	@echo "  DLL   $@"

$(RPCRT4): user/w/rpcrt4/Rpcrt4.c user/w/rpcrt4/Rpcrt4.def $(KERNEL32)
	@mkdir -p $(BUILD)
	$(CLANGWIN) -c user/w/rpcrt4/Rpcrt4.c -o $(BUILD)/rpcrt4.obj
	$(LLDLINK) /dll /noentry /machine:x64 /nodefaultlib /base:0x2A0000000 \
	           /def:user/w/rpcrt4/Rpcrt4.def /out:$@ /implib:$(BUILD)/rpcrt4.lib \
	           $(BUILD)/rpcrt4.obj $(KERNEL32LIB)
	@echo "  DLL   $@"

$(WINMM): user/w/winmm/Winmm.c user/w/winmm/Winmm.def $(KERNEL32)
	$(CLANGWIN) -c user/w/winmm/Winmm.c -o $(BUILD)/winmm.obj
	$(LLDLINK) /dll /noentry /machine:x64 /nodefaultlib /base:0x2B0000000 \
	           /def:user/w/winmm/Winmm.def /out:$@ /implib:$(BUILD)/winmm.lib \
	           $(BUILD)/winmm.obj $(KERNEL32LIB)
	@echo "  DLL   $@"

$(IPHLPAPI): Makefile user/w/iphlpapi/Iphlpapi.c user/w/iphlpapi/IpHelper.h kernel/include/abi/NetQuery.h $(NTDLL)
	@mkdir -p $(BUILD)
	$(CLANGWIN) -c user/w/iphlpapi/Iphlpapi.c -o $(BUILD)/iphlpapi.obj
	$(LLDLINK) /dll /noentry /machine:x64 /nodefaultlib /base:0x320000000 /out:$@ $(BUILD)/iphlpapi.obj $(NTDLLLIB)

$(TASK_SERVICES): $(BUILD)/%.dll: Makefile user/w/taskservices/%.c $(KERNEL32)
	@mkdir -p $(BUILD)
	$(CLANGWIN) -c user/w/taskservices/$*.c -o $(BUILD)/$*.obj
	$(LLDLINK) /dll /noentry /machine:x64 /nodefaultlib /base:$(TASK_SERVICE_BASE_$*) /out:$@ $(BUILD)/$*.obj $(KERNEL32LIB)

$(WINDOWSCODECS): user/w/windowscodecs/WindowsCodecs.c user/w/windowscodecs/WindowsCodecs.def user/w/windowscodecs/third_party/stb_image.h $(wildcard user/w/windowscodecs/include/*.h) $(KERNEL32)
	@mkdir -p $(BUILD)
	$(CLANGWIN) -mno-stack-arg-probe -Iuser/w/windowscodecs/include -c user/w/windowscodecs/WindowsCodecs.c -o $(BUILD)/windowscodecs.obj
	$(LLDLINK) /dll /noentry /machine:x64 /nodefaultlib /base:0x2C0000000 \
	           /def:user/w/windowscodecs/WindowsCodecs.def /out:$@ \
	           $(BUILD)/windowscodecs.obj $(KERNEL32LIB)
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
	           /export:RegisterWindowMessageW=kernel32.RegisterWindowMessageW \
	           /export:RegisterClipboardFormatW=kernel32.RegisterWindowMessageW \
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

# FAT32 initrd holding user-space executables and Mesa's software GL runtime.
# LLVM's llvmpipe library needs the enlarged image; default QEMU RAM remains
# sufficient to boot and run the demo.
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

$(INITRD): Makefile etc/resolv.conf $(DOS_TESTS) $(NET_TOOLS) $(CMD) $(COMMAND_BINS) $(KERNEL32) $(NTDLL) $(ADVAPI32) $(MSVCRT) $(USER32) $(GDI32) $(WINSPOOL) $(COMDLG32) $(UXTHEME) $(COMCTL32) $(SHELL32) $(ADVPACK) $(ACLUI) $(AUTHZ) $(SHLWAPI) $(GDIPLUS) $(OLEAUT32) $(RPCRT4) $(WINMM) $(WINDOWSCODECS) $(IPHLPAPI) $(TASK_SERVICES) $(STARTX) $(ALPINE_XORG) $(ALPINE_FASTFETCH) $(ALPINE_CURL) $(ALPINE_FEH) $(ALPINE_APK) $(wildcard user/d/doom) $(DOOM_FILES) $(DOOM_NESTED_FILES) $(wildcard user/d/windows1) $(wildcard user/d/windows3) $(WINDOWS1_FILES) $(WINDOWS3_FILES) $(WINDOWS7_FILES) $(WINDOWS_FILES) user/d/win.bat user/d/win2.bat user/d/win3.bat user/d/win3x.bat user/d/win7x.bat user/d/xwin/XWIN.EXE other/Logo_small.png $(IMAGE_PROGRAM_INPUTS)
	@mkdir -p $(BUILD)
	dd if=/dev/zero of=$(INITRD) bs=1M count=450 status=none
	mformat -i $(INITRD) -F -v SVINDOWS ::
	mcopy -i $(INITRD) $(CMD) ::CMD.EXE
	mmd -i $(INITRD) ::WINDOWS ::WINDOWS/SYSTEM32
	if [ -n "$(WINDOWS_FILES)" ]; then mcopy -i $(INITRD) $(WINDOWS_FILES) ::WINDOWS/; fi
	mcopy -i $(INITRD) $(KERNEL32) ::WINDOWS/SYSTEM32/KERNEL32.DLL
	mcopy -i $(INITRD) $(NTDLL) ::WINDOWS/SYSTEM32/NTDLL.DLL
	mcopy -i $(INITRD) $(ADVAPI32) ::WINDOWS/SYSTEM32/ADVAPI32.DLL
	mcopy -i $(INITRD) $(MSVCRT) ::WINDOWS/SYSTEM32/MSVCRT.DLL
	mcopy -i $(INITRD) $(USER32) ::WINDOWS/SYSTEM32/USER32.DLL
	mcopy -i $(INITRD) $(GDI32) ::WINDOWS/SYSTEM32/GDI32.DLL
	mcopy -i $(INITRD) $(WINSPOOL) ::WINDOWS/SYSTEM32/WINSPOOL.DRV
	mcopy -i $(INITRD) $(COMDLG32) ::WINDOWS/SYSTEM32/COMDLG32.DLL
	mcopy -i $(INITRD) $(UXTHEME) ::WINDOWS/SYSTEM32/UXTHEME.DLL
	mcopy -i $(INITRD) $(COMCTL32) ::WINDOWS/SYSTEM32/COMCTL32.DLL
	mcopy -i $(INITRD) $(SHELL32) ::WINDOWS/SYSTEM32/SHELL32.DLL
	mcopy -i $(INITRD) $(ADVPACK) ::WINDOWS/SYSTEM32/ADVPACK.DLL
	mcopy -i $(INITRD) $(ACLUI) ::WINDOWS/SYSTEM32/ACLUI.DLL
	mcopy -i $(INITRD) $(AUTHZ) ::WINDOWS/SYSTEM32/AUTHZ.DLL
	mcopy -i $(INITRD) $(SHLWAPI) ::WINDOWS/SYSTEM32/SHLWAPI.DLL
	mcopy -i $(INITRD) $(GDIPLUS) ::WINDOWS/SYSTEM32/GDIPLUS.DLL
	mcopy -i $(INITRD) $(OLEAUT32) ::WINDOWS/SYSTEM32/OLEAUT32.DLL
	mcopy -i $(INITRD) $(RPCRT4) ::WINDOWS/SYSTEM32/RPCRT4.DLL
	mcopy -i $(INITRD) $(WINMM) ::WINDOWS/SYSTEM32/WINMM.DLL
	mcopy -i $(INITRD) $(WINDOWSCODECS) ::WINDOWS/SYSTEM32/WINDOWSCODECS.DLL
	mcopy -i $(INITRD) $(IPHLPAPI) ::WINDOWS/SYSTEM32/IPHLPAPI.DLL
	for dll in $(TASK_SERVICES); do mcopy -i $(INITRD) "$$dll" ::WINDOWS/SYSTEM32/; done
	mcopy -i $(INITRD) $(COMMAND_BINS) ::WINDOWS/SYSTEM32/
	# Keep each Windows release together; root batch files are launchers.
	mmd -i $(INITRD) ::WINDOWS1 ::WINDOWS3 ::WINDOWS7
	if [ -n "$(WINDOWS1_FILES)" ]; then mcopy -i $(INITRD) $(WINDOWS1_FILES) ::WINDOWS1/; fi
	for d in $(WINDOWS3_DIRS); do \
	    rel=$${d#user/d/windows3/}; \
	    mmd -i $(INITRD) "::WINDOWS3/$$rel"; \
	done
	for f in $(WINDOWS3_FILES); do \
	    rel=$${f#user/d/windows3/}; \
	    mcopy -i $(INITRD) "$$f" "::WINDOWS3/$$rel"; \
	done
	if [ -n "$(WINDOWS7_FILES)" ]; then mcopy -i $(INITRD) $(WINDOWS7_FILES) ::WINDOWS7/; fi
	mmd -i $(INITRD) ::DOS
	mcopy -i $(INITRD) user/d/win.bat ::DOS/WIN.BAT
	mcopy -i $(INITRD) user/d/win2.bat ::DOS/WIN2.BAT
	mcopy -i $(INITRD) user/d/win3.bat ::DOS/WIN3.BAT
	mcopy -i $(INITRD) user/d/win3x.bat ::DOS/WIN3X.BAT
	mcopy -i $(INITRD) user/d/win7x.bat ::DOS/WIN7X.BAT
	mcopy -i $(INITRD) user/d/xwin/XWIN.EXE ::WINDOWS3/XWIN.EXE
	mmd -i $(INITRD) ::TESTS
	mcopy -i $(INITRD) $(BUILD)/dos-tests/COMSMOKE.COM ::TESTS/COMSMOKE.COM
	mcopy -i $(INITRD) $(BUILD)/dos-tests/MEMSMOKE.COM ::TESTS/MEMSMOKE.COM
	mcopy -i $(INITRD) $(BUILD)/dos-tests/DPMIINFO.COM ::TESTS/DPMIINFO.COM
	mcopy -i $(INITRD) $(BUILD)/dos-tests/FCBPARSE.COM ::TESTS/FCBPARSE.COM
	mcopy -i $(INITRD) $(BUILD)/dos-tests/INPFLUSH.COM ::TESTS/INPFLUSH.COM
	mcopy -i $(INITRD) $(BUILD)/dos-tests/FFGROUP.COM ::TESTS/FFGROUP.COM
	mcopy -i $(INITRD) $(BUILD)/dos-tests/MZSMOKE.EXE ::TESTS/MZSMOKE.EXE
	mcopy -i $(INITRD) $(BUILD)/dos-tests/DOSCHECK.BAT ::TESTS/DOSCHECK.BAT
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
	mcopy -i $(INITRD) $(STARTX) ::USR/BIN/STARTX.ELF
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
	mmd -i $(INITRD) ::dev ::dev/pts
	mmd -i $(INITRD) ::tmp ::temp
	# Give POSIX programs a real HOME rather than leaving HOME unset.
	mmd -i $(INITRD) ::home ::home/user ::home/user/.config ::home/user/.config/feh
	# Keep the feh smoke-test image in the user's home directory.
	mcopy -i $(INITRD) other/Logo_small.png ::home/user/LOGO.PNG
	if [ -n "$(DOOM_FILES)" ]; then \
	    mmd -i $(INITRD) ::DOOM && \
	    mcopy -i $(INITRD) $(DOOM_FILES) ::DOOM/ && \
	    mcopy -i $(INITRD) user/d/doom/doom.bat ::DOS/DOOM.BAT; \
	fi
	# Xorg writes its default log to /var/log/Xorg.0.log; the Alpine package
	# owns /var/log, so the emulated C: root must provide it. FAT mirrors the
	# Linux path /var/log as C:\var\log.
	for d in var var/log var/cache var/cache/apk; do \
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

dos-x11-check:
	python3 tools/checks/EpollSocketEdgeChecks.py
	python3 tools/checks/DosSmswChecks.py
	python3 tools/checks/DosExtendedErrorChecks.py
	python3 tools/checks/DosTaskReturnChecks.py
	python3 tools/checks/DosX11Checks.py

dos-performance-check:
	python3 tools/checks/DosPerformanceChecks.py

win16-agent:
	python3 tools/BuildWin16Agent.py

dos-vm-check:
	python3 tools/checks/DosInputChecks.py
	python3 tools/checks/DosMemoryChecks.py
	python3 tools/checks/DosMultiplexChecks.py
	python3 tools/checks/DosPspTerminationChecks.py
	python3 tools/checks/DosExecChecks.py user/d/windows1/WIN100.BIN user/d/windows3/WIN200.BIN
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

# Host checks execute the actual USER menu model and native USER/GDI code.
.PHONY: user-menu-check win32k-check crt-wide-conversion-check shell-path-check ip-helper-check elapsed-time-check shell-command-line-check pe-loader-check thread-exit-check process-terminate-check icon-dib-check
icon-dib-check:
	@mkdir -p $(BUILD)
	clang -O1 -g -fsanitize=address,undefined tools/checks/IconDibChecks.c -o $(BUILD)/IconDibChecks
	$(BUILD)/IconDibChecks

process-terminate-check:
	@mkdir -p $(BUILD)
	clang -O1 -g -fsanitize=address,undefined -idirafter kernel/include tools/checks/ProcessTerminateChecks.c -o $(BUILD)/ProcessTerminateChecks
	$(BUILD)/ProcessTerminateChecks

thread-exit-check:
	@mkdir -p $(BUILD)
	clang -O1 -g -fsanitize=address,undefined -idirafter kernel/include tools/checks/ThreadExitChecks.c -o $(BUILD)/ThreadExitChecks
	$(BUILD)/ThreadExitChecks

pe-loader-check:
	@mkdir -p $(BUILD)
	cc -O2 -g -fno-tree-slp-vectorize -ffunction-sections -fdata-sections -Wno-multichar -idirafter kernel/include tools/checks/LoaderChecks.c -Wl,--gc-sections -o $(BUILD)/LoaderChecks
	$(BUILD)/LoaderChecks

shell-command-line-check:
	@mkdir -p $(BUILD)
	clang -O1 -g -fshort-wchar -fsanitize=address,undefined tools/checks/ShellCommandLineChecks.c -o $(BUILD)/ShellCommandLineChecks
	$(BUILD)/ShellCommandLineChecks

elapsed-time-check:
	@mkdir -p $(BUILD)
	clang -O1 -g -fsanitize=address,undefined tools/checks/ElapsedTimeChecks.c -o $(BUILD)/ElapsedTimeChecks
	$(BUILD)/ElapsedTimeChecks

ip-helper-check:
	@mkdir -p $(BUILD)
	clang -O1 -g -fshort-wchar -fsanitize=address,undefined tools/checks/IpHelperChecks.c -o $(BUILD)/IpHelperChecks
	$(BUILD)/IpHelperChecks

shell-path-check:
	@mkdir -p $(BUILD)
	clang -O1 -g -fshort-wchar -fsanitize=address,undefined tools/checks/ShellPathChecks.c -o $(BUILD)/ShellPathChecks
	$(BUILD)/ShellPathChecks

crt-wide-conversion-check:
	@mkdir -p $(BUILD)
	clang -O1 -g -fshort-wchar -fsanitize=address,undefined tools/checks/CrtWideConversionChecks.c -o $(BUILD)/CrtWideConversionChecks
	$(BUILD)/CrtWideConversionChecks

user-menu-check:
	mkdir -p $(BUILD)
	clang -fdeclspec -fshort-wchar -Wno-ignored-attributes -ffunction-sections -fdata-sections $(NOTEPAD_INC) tools/checks/UserMenuChecks.c -Wl,--gc-sections -o $(BUILD)/UserMenuChecks
	$(BUILD)/UserMenuChecks

# Keep GCC from packing unrelated syscall function pointers into shared
# vector constants, which prevents dead-section removal in this host harness.
win32k-check:
	mkdir -p $(BUILD)
	cc -O2 -g -fno-tree-slp-vectorize -ffunction-sections -fdata-sections -Wno-multichar -idirafter kernel/include -Ikernel/net/lwip -I$(LWIP_ROOT)/include tools/checks/Win32kChecks.c -Wl,--gc-sections -o $(BUILD)/Win32kChecks
	$(BUILD)/Win32kChecks

.PHONY: common-controls-check
common-controls-check:
	mkdir -p $(BUILD)
	clang -O1 -g -fshort-wchar -ffunction-sections -fdata-sections -fsanitize=address,undefined $(NOTEPAD_INC) tools/checks/DateTimeChecks.c -Wl,--gc-sections -o $(BUILD)/DateTimeChecks
	$(BUILD)/DateTimeChecks
	clang -O1 -g -fshort-wchar -ffunction-sections -fdata-sections -fsanitize=address,undefined $(NOTEPAD_INC) tools/checks/ComboChecks.c -Wl,--gc-sections -o $(BUILD)/ComboChecks
	$(BUILD)/ComboChecks
	clang -O1 -g -fshort-wchar -ffunction-sections -fdata-sections -fsanitize=address,undefined $(NOTEPAD_INC) tools/checks/EditChecks.c -Wl,--gc-sections -o $(BUILD)/EditChecks
	$(BUILD)/EditChecks

.PHONY: tree-view-check
tree-view-check:
	mkdir -p $(BUILD)
	clang -O1 -g -fshort-wchar -ffunction-sections -fdata-sections -fsanitize=address,undefined $(NOTEPAD_INC) tools/checks/TreeViewChecks.c -Wl,--gc-sections -o $(BUILD)/TreeViewChecks
	$(BUILD)/TreeViewChecks

.PHONY: client-rect-check
client-rect-check:
	mkdir -p $(BUILD)
	clang -O1 -g -fshort-wchar -fsanitize=address,undefined $(NOTEPAD_INC) tools/checks/ClientRectChecks.c -o $(BUILD)/ClientRectChecks
	$(BUILD)/ClientRectChecks

.PHONY: storage-medium-check
storage-medium-check:
	mkdir -p $(BUILD)
	clang -O1 -g -fsanitize=address,undefined tools/checks/StorageMediumChecks.c -o $(BUILD)/StorageMediumChecks
	$(BUILD)/StorageMediumChecks

.PHONY: native-delete-check
native-delete-check:
	mkdir -p $(BUILD)
	clang -O1 -g -fsanitize=address,undefined tools/checks/NativeDeleteChecks.c -o $(BUILD)/NativeDeleteChecks
	$(BUILD)/NativeDeleteChecks

.PHONY: copy-file-ex-check
copy-file-ex-check:
	mkdir -p $(BUILD)
	clang -O1 -g -fsanitize=address,undefined tools/checks/CopyFileExChecks.c -o $(BUILD)/CopyFileExChecks
	$(BUILD)/CopyFileExChecks

.PHONY: resource-range-check
resource-range-check:
	mkdir -p $(BUILD)
	clang -O1 -g -fsanitize=address,undefined tools/checks/ResourceRangeChecks.c -o $(BUILD)/ResourceRangeChecks
	$(BUILD)/ResourceRangeChecks

.PHONY: status-control-check
status-control-check:
	mkdir -p $(BUILD)
	clang -O1 -g -fshort-wchar -ffunction-sections -fdata-sections -fsanitize=address,undefined $(NOTEPAD_INC) tools/checks/StatusChecks.c -Wl,--gc-sections -o $(BUILD)/StatusChecks
	$(BUILD)/StatusChecks

.PHONY: tab-layout-check
tab-layout-check:
	mkdir -p $(BUILD)
	clang -O1 -g -fshort-wchar -ffunction-sections -fdata-sections -fsanitize=address,undefined $(NOTEPAD_INC) tools/checks/TabLayoutChecks.c -Wl,--gc-sections -o $(BUILD)/TabLayoutChecks
	$(BUILD)/TabLayoutChecks

.PHONY: cpu-accounting-check
cpu-accounting-check:
	mkdir -p $(BUILD)
	clang -O1 -g -fsanitize=address,undefined -idirafter kernel/include tools/checks/CpuAccountingChecks.c -o $(BUILD)/CpuAccountingChecks
	$(BUILD)/CpuAccountingChecks

.PHONY: system-cpu-check
system-cpu-check:
	mkdir -p $(BUILD)
	clang -O1 -g -fsanitize=address,undefined -idirafter kernel/include tools/checks/SystemCpuChecks.c -o $(BUILD)/SystemCpuChecks
	$(BUILD)/SystemCpuChecks
