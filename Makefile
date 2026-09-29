CROSS   ?=
CC      := $(CROSS)gcc
LD      := $(CROSS)ld
NASM    := nasm
HOSTCC  ?= clang

CONFIG ?= debug
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
KERNEL  := $(BUILD)/svindowskrnl.elf
ISO     := $(BUILD)/svindows.iso

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

NASMFLAGS := -f elf64 -g -F dwarf

LDFLAGS := -n -nostdlib -static -no-pie -z noexecstack \
           -z max-page-size=0x1000 --no-warn-rwx-segments \
           -T kernel/arch/x86_64/Linker.ld

# Keep object and link order deterministic across hosts and filesystems.
C_SRC   := $(sort $(shell find kernel -name '*.c'))
ASM_SRC := $(sort $(shell find kernel -name '*.asm'))
OBJ     := $(patsubst %,$(BUILD)/%.o,$(C_SRC) $(ASM_SRC))
C_DEPS  := $(patsubst %.c,$(BUILD)/%.c.d,$(C_SRC))

-include $(C_DEPS)

# Large compatibility surfaces are grouped into included implementation parts.
# They remain one translation unit while private helpers are still shared, but
# listing them here keeps incremental builds correct when a part changes.
WIN32K_USER_PARTS := $(wildcard kernel/win32k/User*.inc)
WIN32K_GDI_PARTS  := $(wildcard kernel/win32k/gdi/*.inc)
LDR_PARTS         := $(wildcard kernel/ldr/pe/*.inc)
KERNEL32_PARTS    := $(wildcard user/kernel32/*.inc)
NTDLLRTL_PARTS    := $(wildcard user/ntdll/Rtl/*.inc)
MSVCRT_PARTS      := $(wildcard user/msvcrt/*.inc)
CMD_PARTS         := $(wildcard user/cmd/*.inc)

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
COMMAND_BINS := $(addprefix $(BUILD)/,DIR.EXE TYPE.EXE CLS.EXE VER.EXE HELP.EXE \
                VOL.EXE HOSTNAME.EXE PAUSE.EXE COPY.EXE DEL.EXE ERASE.EXE \
                MD.EXE MKDIR.EXE RD.EXE RMDIR.EXE REN.EXE RENAME.EXE)
EXTRA      := $(BUILD)/extra.dll
DISK       := $(BUILD)/disk.img
LLDLINK    := lld-link
CLANGWIN   := clang --target=x86_64-pc-windows-msvc -ffreestanding \
              -fno-stack-protector -fno-stack-check -funwind-tables \
              -Iuser/include $(USER_OPT)

QEMU        := qemu-system-x86_64
# -boot d forces booting from the CD-ROM (the ISO); the hard disk is data only.
QEMUFLAGS   := -m 1024M -no-reboot -no-shutdown -boot d
QEMUDISK    := -drive file=$(DISK),format=raw,if=ide,index=0,media=disk

.PHONY: all iso run run-serial run-gui clean check audit test test-release FORCE

FORCE:

all: $(ISO) $(DISK)

# A configuration change must invalidate existing objects even without clean.
# Preserve the stamp's mtime when settings have not changed.
$(BUILD_CONFIG): FORCE
	@mkdir -p $(BUILD)
	@printf '%s\n' 'CONFIG=$(CONFIG)' 'KERNEL_OPT=$(KERNEL_OPT)' 'USER_OPT=$(USER_OPT)' > $@.tmp
	@cmp -s $@.tmp $@ && rm $@.tmp || mv $@.tmp $@

$(OBJ) $(NTDLL) $(KERNEL32) $(ADVAPI32) $(MSVCRT) $(CRT0) $(CMD) $(CMDUTIL) $(EXTRA): $(BUILD_CONFIG)

$(BUILD)/%.c.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# PROGRAM is supplied through CFLAGS, which make does not otherwise track.
# Recompile just the boot selector so switching PROGRAM never reuses a kernel
# containing the previous executable name.
$(BUILD)/kernel/ke/KeMain.c.o: kernel/ke/KeMain.c FORCE
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# The fault-injection threshold is supplied through CFLAGS as well.
$(BUILD)/kernel/io/BlockCache.c.o: kernel/io/BlockCache.c FORCE
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/kernel/ke/Syscall.c.o: $(WIN32K_USER_PARTS)
$(BUILD)/kernel/win32k/Gdiobj.c.o: $(WIN32K_GDI_PARTS)
$(BUILD)/kernel/ldr/Pe.c.o: $(LDR_PARTS)

$(BUILD)/%.asm.o: %.asm
	@mkdir -p $(dir $@)
	$(NASM) $(NASMFLAGS) $< -o $@

# ntdll.dll: the syscall-stub library, with an export table + import library.
# Each user DLL gets a distinct preferred base so the loader (which loads at the
# preferred base) never has two images collide. Ntdllrtl.c provides the C side
# (Rtl heap, critical sections, string helpers) real binaries import.
$(NTDLL): user/ntdll/Ntdll.asm user/ntdll/Ntdll.def user/ntdll/Ntdllrtl.c $(NTDLLRTL_PARTS)
	@mkdir -p $(BUILD)
	$(NASM) -f win64 user/ntdll/Ntdll.asm -o $(BUILD)/ntdll.obj
	$(CLANGWIN) -c user/ntdll/Ntdllrtl.c -o $(BUILD)/ntdllrtl.obj
	$(LLDLINK) /dll /noentry /machine:x64 /nodefaultlib /base:0x180000000 \
	           /def:user/ntdll/Ntdll.def /out:$(NTDLL) /implib:$(NTDLLLIB) \
	           $(BUILD)/ntdll.obj $(BUILD)/ntdllrtl.obj
	@echo "  DLL   $(NTDLL)"

# kernel32.dll: the Win32 subsystem library, built from C over ntdll.
$(KERNEL32): user/kernel32/Kernel32.c user/kernel32/Kernel32Process.c $(KERNEL32_PARTS) $(NTDLL)
	@mkdir -p $(BUILD)
	$(CLANGWIN) -c user/kernel32/Kernel32.c -o $(BUILD)/kernel32.obj
	$(CLANGWIN) -c user/kernel32/Kernel32Process.c -o $(BUILD)/kernel32_process.obj
	$(LLDLINK) /dll /noentry /machine:x64 /nodefaultlib /base:0x1C0000000 \
	           /out:$(KERNEL32) /implib:$(KERNEL32LIB) \
	           $(BUILD)/kernel32.obj $(BUILD)/kernel32_process.obj $(NTDLLLIB)
	@echo "  DLL   $(KERNEL32)"

# advapi32.dll: the registry Reg* API (C), built over ntdll's Nt*Key services.
$(ADVAPI32): user/advapi32/Advapi32.c $(NTDLL)
	@mkdir -p $(BUILD)
	$(CLANGWIN) -c user/advapi32/Advapi32.c -o $(BUILD)/advapi32.obj
	$(LLDLINK) /dll /noentry /machine:x64 /nodefaultlib /base:0x1D0000000 \
	           /out:$(ADVAPI32) /implib:$(ADVAPI32LIB) \
	           $(BUILD)/advapi32.obj $(NTDLLLIB)
	@echo "  DLL   $(ADVAPI32)"

# msvcrt.dll: a tiny C runtime (printf/malloc/str*) over kernel32.
$(MSVCRT): user/msvcrt/Msvcrt.c $(MSVCRT_PARTS) $(KERNEL32)
	@mkdir -p $(BUILD)
	$(CLANGWIN) -c user/msvcrt/Msvcrt.c -o $(BUILD)/msvcrt.obj
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

# extra.dll: a standalone DLL loaded at runtime via LoadLibraryA (not linked
# into the app's import chain). Distinct preferred base so it never collides.
$(EXTRA): user/extra/Extra.c
	@mkdir -p $(BUILD)
	$(CLANGWIN) -c user/extra/Extra.c -o $(BUILD)/extra.obj
	$(LLDLINK) /dll /noentry /machine:x64 /nodefaultlib /base:0x200000000 \
	           /out:$(EXTRA) /implib:$(BUILD)/extra.lib $(BUILD)/extra.obj
	@echo "  DLL   $(EXTRA)"

# crt0.obj: the C runtime startup (mainCRTStartup). Linked into every program
# so a standard `int main()` is entered without a custom /entry.
$(CRT0): user/crt/Crt0.c
	@mkdir -p $(BUILD)
	$(CLANGWIN) -c user/crt/Crt0.c -o $(CRT0)

# cmd.exe is a normal user-mode PE. Like Windows cmd.exe, filesystem and shell
# syntax commands are built into the command processor rather than the kernel.
$(CMD): user/cmd/Cmd.c $(CMD_PARTS) $(CRT0) $(KERNEL32) $(NTDLL)
	@mkdir -p $(BUILD)
	$(CLANGWIN) -c user/cmd/Cmd.c -o $(BUILD)/cmd.obj
	$(LLDLINK) /subsystem:console /nodefaultlib /machine:x64 \
	           /out:$@ $(BUILD)/cmd.obj $(CRT0) $(KERNEL32LIB) $(NTDLLLIB)
	@echo "  PE    $@"

# Stateless console commands are separate programs. One implementation is
# linked once and installed under each command/alias name; it selects behavior
# from argv[0], like a compact multi-call toolset without shell-resident code.
$(CMDUTIL): user/cmdutils/Cmdutils.c $(CRT0) $(KERNEL32)
	@mkdir -p $(BUILD)
	$(CLANGWIN) -c user/cmdutils/Cmdutils.c -o $(BUILD)/cmdutils.obj
	$(LLDLINK) /subsystem:console /nodefaultlib /machine:x64 \
	           /out:$@ $(BUILD)/cmdutils.obj $(CRT0) $(KERNEL32LIB)
	@echo "  PE    $@"

$(COMMAND_BINS): $(CMDUTIL)
	cp $(CMDUTIL) $@

# FAT32 disk image holding the user-space executables, read by the kernel's
# ATA + FAT drivers at runtime.
$(DISK): Makefile $(CMD) $(COMMAND_BINS) $(KERNEL32) $(NTDLL) $(ADVAPI32) $(MSVCRT) $(EXTRA)
	@mkdir -p $(BUILD)
	dd if=/dev/zero of=$(DISK) bs=1M count=64 status=none
	mformat -i $(DISK) -F -v SVINDOWS ::
	mcopy -i $(DISK) $(CMD) ::CMD.EXE
	mcopy -i $(DISK) $(KERNEL32) ::KERNEL32.DLL
	mcopy -i $(DISK) $(NTDLL) ::NTDLL.DLL
	mcopy -i $(DISK) $(ADVAPI32) ::ADVAPI32.DLL
	mcopy -i $(DISK) $(MSVCRT) ::MSVCRT.DLL
	mcopy -i $(DISK) $(EXTRA) ::EXTRA.DLL
	mcopy -i $(DISK) $(COMMAND_BINS) ::

	@echo "  DISK  $(DISK)"

$(KERNEL): $(OBJ) kernel/arch/x86_64/Linker.ld
	@mkdir -p $(dir $@)
	$(LD) $(LDFLAGS) -o $@ $(OBJ)
	@echo "  LD    $@"

iso: $(ISO)

$(ISO): $(KERNEL) boot/Grub.cfg
	@mkdir -p $(BUILD)/isodir/boot/grub
	@cp $(KERNEL) $(BUILD)/isodir/boot/svindowskrnl.elf
	@cp boot/Grub.cfg $(BUILD)/isodir/boot/grub/grub.cfg
	grub2-mkrescue -o $@ $(BUILD)/isodir 2>/dev/null
	@echo "  ISO   $@"

run-serial: all
	$(QEMU) $(QEMUFLAGS) -cdrom $(ISO) $(QEMUDISK) -serial stdio -display none

run: run-serial

run-gui: all
	$(QEMU) $(QEMUFLAGS) -cdrom $(ISO) $(QEMUDISK) -serial stdio

# Host checks exercise the actual kernel implementations with fake hardware.
# No regression executable is installed in the guest image.
check:
	@mkdir -p $(BUILD)/checks
	$(HOSTCC) -std=gnu11 -O1 -g -ffunction-sections -fdata-sections \
	    -fsanitize=address,undefined -fno-omit-frame-pointer -Ikernel/include \
	    -Wl,--gc-sections tools/checks/ExecutionChecks.c -o $(BUILD)/checks/execution-checks
	UBSAN_OPTIONS=halt_on_error=1 $(BUILD)/checks/execution-checks
	$(HOSTCC) -std=gnu11 -O1 -g -ffunction-sections -fdata-sections \
	    -fsanitize=address,undefined -fno-omit-frame-pointer -Ikernel/include \
	    -Wno-multichar -Wl,--gc-sections tools/checks/TimerChecks.c -o $(BUILD)/checks/timer-checks
	UBSAN_OPTIONS=halt_on_error=1 $(BUILD)/checks/timer-checks
	$(HOSTCC) -std=gnu11 -O1 -g -ffunction-sections -fdata-sections \
	    -fsanitize=address,undefined -fno-omit-frame-pointer -Ikernel/include \
	    -Wl,--gc-sections tools/checks/KernelChecks.c kernel/mm/Vmm.c \
	    kernel/mm/Multiboot.c kernel/hal/Framebuffer.c kernel/hal/Font9x20.c kernel/hal/Console.c \
	    kernel/io/BlockCache.c kernel/rtl/Print.c kernel/win32k/SharedHandles.c -o $(BUILD)/checks/kernel-checks
	UBSAN_OPTIONS=halt_on_error=1 $(BUILD)/checks/kernel-checks
	$(HOSTCC) -std=gnu11 -O1 -g -pthread -fsanitize=address,undefined \
	    -fno-omit-frame-pointer -Ikernel/include tools/checks/RuntimeChecks.c \
	    -o $(BUILD)/checks/runtime-checks
	UBSAN_OPTIONS=halt_on_error=1 $(BUILD)/checks/runtime-checks
	$(HOSTCC) -std=gnu11 -O1 -g -ffunction-sections -fdata-sections \
	    -fsanitize=address,undefined -fno-omit-frame-pointer -Ikernel/include \
	    -Wl,--gc-sections tools/checks/ConsoleChecks.c kernel/io/Console.c \
	    -o $(BUILD)/checks/console-checks
	UBSAN_OPTIONS=halt_on_error=1 $(BUILD)/checks/console-checks
	$(HOSTCC) -std=gnu11 -O1 -g -ffunction-sections -fdata-sections \
	    -fsanitize=address,undefined -fno-omit-frame-pointer -Ikernel/include \
	    -Wl,--gc-sections tools/checks/LoaderChecks.c -o $(BUILD)/checks/loader-checks
	UBSAN_OPTIONS=halt_on_error=1 $(BUILD)/checks/loader-checks
	$(HOSTCC) -std=gnu11 -O1 -g -ffunction-sections -fdata-sections \
	    -fsanitize=address,undefined -fno-omit-frame-pointer -Ikernel/include \
	    -Wl,--gc-sections tools/checks/Win32kChecks.c -o $(BUILD)/checks/win32k-checks
	UBSAN_OPTIONS=halt_on_error=1 $(BUILD)/checks/win32k-checks

test: all check
	python3 tools/QemuSmoke.py

audit:
	python3 tools/SourceAudit.py

test-release:
	$(MAKE) clean
	$(MAKE) CONFIG=release test

clean:
	rm -rf $(BUILD)
