# ============================================================================
# SVINDOWS top-level build
#
#   make / make all build the complete bootable system
#   make run-serial  boot cmd.exe in a terminal (serial input/output)
#   make run-gui     boot cmd.exe in a QEMU window (PS/2 keyboard + VGA)
#   make clean      remove build artifacts
# ============================================================================

CROSS   ?=
CC      := $(CROSS)gcc
LD      := $(CROSS)ld
NASM    := nasm
PYTHON  ?= python3

CONFIG ?= debug
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
KERNEL  := $(BUILD)/svindowskrnl.elf
ISO     := $(BUILD)/svindows.iso

.DEFAULT_GOAL := all

# The PE to start after boot. The command processor is the normal system shell;
# PROGRAM=testapp.exe and PROGRAM=hello.exe remain useful development boots.
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
          -DSVINDOWS_BOOT_COMMAND_LINE='"$(CMDLINE)"'

NASMFLAGS := -f elf64 -g -F dwarf

LDFLAGS := -n -nostdlib -static -no-pie -z noexecstack \
           -z max-page-size=0x1000 --no-warn-rwx-segments \
           -T kernel/arch/x86_64/linker.ld

C_SRC   := $(shell find kernel -name '*.c')
ASM_SRC := $(shell find kernel -name '*.asm')
OBJ     := $(patsubst %,$(BUILD)/%.o,$(C_SRC) $(ASM_SRC))
C_DEPS  := $(patsubst %.c,$(BUILD)/%.c.d,$(C_SRC))

-include $(C_DEPS)

# Large compatibility surfaces are grouped into included implementation parts.
# They remain one translation unit while private helpers are still shared, but
# listing them here keeps incremental builds correct when a part changes.
WIN32K_USER_PARTS := $(wildcard kernel/win32k/user_*.inc)
WIN32K_GDI_PARTS  := $(wildcard kernel/win32k/gdi/*.inc)
LDR_PARTS         := $(wildcard kernel/ldr/pe/*.inc)
KERNEL32_PARTS    := $(wildcard user/kernel32/*.inc)
NTDLLRTL_PARTS    := $(wildcard user/ntdllrtl/*.inc)
MSVCRT_PARTS      := $(wildcard user/msvcrt/*.inc)
CMD_PARTS         := $(wildcard user/cmd/*.inc)

# User-space images, built as real PE32+ files and placed on a FAT disk image
# the kernel reads at runtime. The dependency chain is
#   testapp.exe -> kernel32.dll -> ntdll.dll -> syscall
# ntdll (asm) exports the Nt* syscall stubs; kernel32 (C) implements Win32 on
# top of them; testapp (C) is a normal Win32 program.
TESTAPP    := $(BUILD)/testapp.exe
NTDLL      := $(BUILD)/ntdll.dll
NTDLLLIB   := $(BUILD)/ntdll.lib
KERNEL32   := $(BUILD)/kernel32.dll
KERNEL32LIB := $(BUILD)/kernel32.lib
ADVAPI32   := $(BUILD)/advapi32.dll
ADVAPI32LIB := $(BUILD)/advapi32.lib
MSVCRT     := $(BUILD)/msvcrt.dll
MSVCRTLIB  := $(BUILD)/msvcrt.lib
CRT0       := $(BUILD)/crt0.obj
HELLO      := $(BUILD)/hello.exe
FILETEST   := $(BUILD)/filetest.exe
CAT        := $(BUILD)/cat.exe
CONSOLETEST := $(BUILD)/consoletest.exe
ABITEST    := $(BUILD)/abitest.exe
SEHTEST    := $(BUILD)/sehtest.exe
SEHCRASH   := $(BUILD)/sehcrash.exe
SEHFRAME   := $(BUILD)/sehframe.exe
CPPTEST    := $(BUILD)/cpptest.exe
FETCH      := $(BUILD)/fetch.exe
CMD        := $(BUILD)/cmd.exe
EXTRA      := $(BUILD)/extra.dll
DISK       := $(BUILD)/disk.img
LLDLINK    := lld-link
LLVM_DLLTOOL := $(or $(shell command -v llvm-dlltool 2>/dev/null),\
                     $(shell command -v llvm-dlltool-19 2>/dev/null),\
                     llvm-dlltool)
CLANGWIN   := clang --target=x86_64-pc-windows-msvc -ffreestanding \
              -fno-stack-protector -fno-stack-check -funwind-tables $(USER_OPT)

QEMU        := qemu-system-x86_64
# -boot d forces booting from the CD-ROM (the ISO); the hard disk is data only.
QEMUFLAGS   := -m 1024M -no-reboot -no-shutdown -boot d
QEMUDISK    := -drive file=$(DISK),format=raw,if=ide,index=0,media=disk

.PHONY: all iso run run-serial run-gui test test-release test-exceptions test-fs-reboot clean FORCE

FORCE:

all: $(ISO) $(DISK)

$(BUILD)/%.c.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# PROGRAM is supplied through CFLAGS, which make does not otherwise track.
# Recompile just the boot selector so switching PROGRAM never reuses a kernel
# containing the previous executable name.
$(BUILD)/kernel/ke/ke_main.c.o: kernel/ke/ke_main.c FORCE
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/kernel/ke/syscall.c.o: $(WIN32K_USER_PARTS)
$(BUILD)/kernel/win32k/gdiobj.c.o: $(WIN32K_GDI_PARTS)
$(BUILD)/kernel/ldr/pe.c.o: $(LDR_PARTS)

$(BUILD)/%.asm.o: %.asm
	@mkdir -p $(dir $@)
	$(NASM) $(NASMFLAGS) $< -o $@

# ntdll.dll: the syscall-stub library, with an export table + import library.
# Each user DLL gets a distinct preferred base so the loader (which loads at the
# preferred base) never has two images collide. ntdllrtl.c provides the C side
# (Rtl heap, critical sections, string helpers) real binaries import.
$(NTDLL): user/ntdll.asm user/ntdll.def user/ntdllrtl.c $(NTDLLRTL_PARTS)
	@mkdir -p $(BUILD)
	$(NASM) -f win64 user/ntdll.asm -o $(BUILD)/ntdll.obj
	$(CLANGWIN) -c user/ntdllrtl.c -o $(BUILD)/ntdllrtl.obj
	$(LLDLINK) /dll /noentry /machine:x64 /nodefaultlib /base:0x180000000 \
	           /def:user/ntdll.def /out:$(NTDLL) /implib:$(NTDLLLIB) \
	           $(BUILD)/ntdll.obj $(BUILD)/ntdllrtl.obj
	@echo "  DLL   $(NTDLL)"

# kernel32.dll: the Win32 subsystem library, built from C over ntdll.
$(KERNEL32): user/kernel32.c user/kernel32_process.c $(KERNEL32_PARTS) $(NTDLL)
	@mkdir -p $(BUILD)
	$(CLANGWIN) -c user/kernel32.c -o $(BUILD)/kernel32.obj
	$(CLANGWIN) -c user/kernel32_process.c -o $(BUILD)/kernel32_process.obj
	$(LLDLINK) /dll /noentry /machine:x64 /nodefaultlib /base:0x1C0000000 \
	           /out:$(KERNEL32) /implib:$(KERNEL32LIB) \
	           $(BUILD)/kernel32.obj $(BUILD)/kernel32_process.obj $(NTDLLLIB)
	@echo "  DLL   $(KERNEL32)"

# advapi32.dll: the registry Reg* API (C), built over ntdll's Nt*Key services.
$(ADVAPI32): user/advapi32.c $(NTDLL)
	@mkdir -p $(BUILD)
	$(CLANGWIN) -c user/advapi32.c -o $(BUILD)/advapi32.obj
	$(LLDLINK) /dll /noentry /machine:x64 /nodefaultlib /base:0x1D0000000 \
	           /out:$(ADVAPI32) /implib:$(ADVAPI32LIB) \
	           $(BUILD)/advapi32.obj $(NTDLLLIB)
	@echo "  DLL   $(ADVAPI32)"

# msvcrt.dll: a tiny C runtime (printf/malloc/str*) over kernel32.
$(MSVCRT): user/msvcrt.c $(MSVCRT_PARTS) $(KERNEL32)
	@mkdir -p $(BUILD)
	$(CLANGWIN) -c user/msvcrt.c -o $(BUILD)/msvcrt.obj
	$(LLDLINK) /dll /noentry /machine:x64 /nodefaultlib /base:0x1E0000000 \
	           /out:$(MSVCRT) /implib:$(MSVCRTLIB) \
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
$(EXTRA): user/extra.c
	@mkdir -p $(BUILD)
	$(CLANGWIN) -c user/extra.c -o $(BUILD)/extra.obj
	$(LLDLINK) /dll /noentry /machine:x64 /nodefaultlib /base:0x200000000 \
	           /out:$(EXTRA) /implib:$(BUILD)/extra.lib $(BUILD)/extra.obj
	@echo "  DLL   $(EXTRA)"

# crt0.obj: the C runtime startup (mainCRTStartup). Linked into every program
# so a standard `int main()` is entered without a custom /entry.
$(CRT0): user/crt0.c
	@mkdir -p $(BUILD)
	$(CLANGWIN) -c user/crt0.c -o $(CRT0)

# stubtest.lib: an import library (built by llvm-dlltool from a .def) that
# promises a kernel32 export the real kernel32.dll does not provide, so testapp
# can exercise the loader's unimplemented-import stubbing.
STUBTESTLIB := $(BUILD)/stubtest.lib
$(STUBTESTLIB): user/stubtest.def
	@mkdir -p $(BUILD)
	$(LLVM_DLLTOOL) -m i386:x86-64 -d user/stubtest.def -l $(STUBTESTLIB) -D kernel32.dll

# testapp.exe: a Win32 program (C) entered through the CRT startup (int main),
# linked against kernel32 + advapi32 + msvcrt (+ the stubtest import lib).
$(TESTAPP): user/testapp.c $(CRT0) $(KERNEL32) $(ADVAPI32) $(MSVCRT) $(STUBTESTLIB)
	@mkdir -p $(BUILD)
	$(CLANGWIN) -c user/testapp.c -o $(BUILD)/testapp.obj
	$(LLDLINK) /subsystem:console /nodefaultlib /machine:x64 \
	           /out:$@ $(BUILD)/testapp.obj $(CRT0) $(KERNEL32LIB) \
	           $(ADVAPI32LIB) $(MSVCRTLIB) $(STUBTESTLIB)
	@echo "  PE    $@"

# hello.exe: a plain portable C program (int main, stdio/stdlib/string), built
# the ordinary way -- no custom entry, no OS-specific code.
$(HELLO): user/hello.c $(CRT0) $(KERNEL32) $(MSVCRT)
	@mkdir -p $(BUILD)
	$(CLANGWIN) -Iuser/include -c user/hello.c -o $(BUILD)/hello.obj
	$(LLDLINK) /subsystem:console /nodefaultlib /machine:x64 \
	           /out:$@ $(BUILD)/hello.obj $(CRT0) $(KERNEL32LIB) $(MSVCRTLIB)
	@echo "  PE    $@"

$(FILETEST): user/filetest.c $(CRT0) $(KERNEL32)
	@mkdir -p $(BUILD)
	$(CLANGWIN) -c user/filetest.c -o $(BUILD)/filetest.obj
	$(LLDLINK) /subsystem:console /nodefaultlib /machine:x64 \
	           /out:$@ $(BUILD)/filetest.obj $(CRT0) $(KERNEL32LIB)
	@echo "  PE    $@"

$(CAT): user/cat.c $(CRT0) $(KERNEL32)
	@mkdir -p $(BUILD)
	$(CLANGWIN) -c user/cat.c -o $(BUILD)/cat.obj
	$(LLDLINK) /subsystem:console /nodefaultlib /machine:x64 \
	           /out:$@ $(BUILD)/cat.obj $(CRT0) $(KERNEL32LIB)
	@echo "  PE    $@"

$(CONSOLETEST): user/consoletest.c $(CRT0) $(KERNEL32)
	@mkdir -p $(BUILD)
	$(CLANGWIN) -c user/consoletest.c -o $(BUILD)/consoletest.obj
	$(LLDLINK) /subsystem:console /nodefaultlib /machine:x64 \
	           /out:$@ $(BUILD)/consoletest.obj $(CRT0) $(KERNEL32LIB)
	@echo "  PE    $@"

$(ABITEST): user/abitest.c user/abi_helpers.asm $(CRT0) $(KERNEL32) $(NTDLL)
	@mkdir -p $(BUILD)
	$(CLANGWIN) -c user/abitest.c -o $(BUILD)/abitest.obj
	$(NASM) -f win64 user/abi_helpers.asm -o $(BUILD)/abi_helpers.obj
	$(LLDLINK) /subsystem:console /nodefaultlib /machine:x64 \
	           /out:$@ $(BUILD)/abitest.obj $(BUILD)/abi_helpers.obj \
	           $(CRT0) $(KERNEL32LIB) $(NTDLLLIB)
	@echo "  PE    $@"

$(SEHTEST): user/sehtest.c user/seh_helpers.asm $(CRT0) $(KERNEL32) $(NTDLL)
	@mkdir -p $(BUILD)
	$(CLANGWIN) -c user/sehtest.c -o $(BUILD)/sehtest.obj
	$(NASM) -f win64 user/seh_helpers.asm -o $(BUILD)/seh_helpers.obj
	$(LLDLINK) /subsystem:console /nodefaultlib /machine:x64 \
	           /out:$@ $(BUILD)/sehtest.obj $(BUILD)/seh_helpers.obj \
	           $(CRT0) $(KERNEL32LIB) $(NTDLLLIB)
	@echo "  PE    $@"

$(SEHCRASH): user/sehcrash.c user/seh_helpers.asm $(CRT0) $(KERNEL32)
	@mkdir -p $(BUILD)
	$(CLANGWIN) -c user/sehcrash.c -o $(BUILD)/sehcrash.obj
	$(NASM) -f win64 user/seh_helpers.asm -o $(BUILD)/seh_crash_helpers.obj
	$(LLDLINK) /subsystem:console /nodefaultlib /machine:x64 \
	           /out:$@ $(BUILD)/sehcrash.obj $(BUILD)/seh_crash_helpers.obj \
	           $(CRT0) $(KERNEL32LIB)
	@echo "  PE    $@"

$(SEHFRAME): user/sehframe.c $(CRT0) $(KERNEL32) $(NTDLL)
	@mkdir -p $(BUILD)
	$(CLANGWIN) -fasync-exceptions -funwind-tables -c user/sehframe.c -o $(BUILD)/sehframe.obj
	$(LLDLINK) /subsystem:console /nodefaultlib /machine:x64 \
	           /out:$@ $(BUILD)/sehframe.obj $(CRT0) \
	           $(KERNEL32LIB) $(NTDLLLIB)
	@echo "  PE    $@"

# cpptest.exe: a C++ program exercising the MSVC x64 C++ personality and
# _CxxThrowException, both provided by ntdll over the shared unwinder.
$(CPPTEST): user/cpptest.cpp $(CRT0) $(NTDLL)
	@mkdir -p $(BUILD)
	$(CLANGWIN) -x c++ -fno-rtti -fasync-exceptions -funwind-tables \
	           -c user/cpptest.cpp -o $(BUILD)/cpptest.obj
	$(LLDLINK) /subsystem:console /nodefaultlib /machine:x64 \
	           /out:$@ $(BUILD)/cpptest.obj $(CRT0) $(KERNEL32LIB) $(NTDLLLIB)
	@echo "  PE    $@"

$(FETCH): user/fetch.c user/fetch_cpu.asm $(CRT0) $(KERNEL32) $(NTDLL)
	@mkdir -p $(BUILD)
	$(CLANGWIN) -c user/fetch.c -o $(BUILD)/fetch.obj
	$(NASM) -f win64 user/fetch_cpu.asm -o $(BUILD)/fetch_cpu.obj
	$(LLDLINK) /subsystem:console /nodefaultlib /machine:x64 \
	           /out:$@ $(BUILD)/fetch.obj $(BUILD)/fetch_cpu.obj \
	           $(CRT0) $(KERNEL32LIB) $(NTDLLLIB)
	@echo "  PE    $@"

# cmd.exe is a normal user-mode PE. Like Windows cmd.exe, filesystem and shell
# syntax commands are built into the command processor rather than the kernel.
$(CMD): user/cmd.c $(CMD_PARTS) $(CRT0) $(KERNEL32) $(NTDLL)
	@mkdir -p $(BUILD)
	$(CLANGWIN) -c user/cmd.c -o $(BUILD)/cmd.obj
	$(LLDLINK) /subsystem:console /nodefaultlib /machine:x64 \
	           /out:$@ $(BUILD)/cmd.obj $(CRT0) $(KERNEL32LIB) $(NTDLLLIB)
	@echo "  PE    $@"

# FAT32 disk image holding the user-space executables, read by the kernel's
# ATA + FAT drivers at runtime.
$(DISK): Makefile $(TESTAPP) $(HELLO) $(FILETEST) $(CAT) $(CONSOLETEST) $(ABITEST) $(SEHTEST) $(SEHCRASH) $(SEHFRAME) $(CPPTEST) $(FETCH) $(CMD) $(KERNEL32) $(NTDLL) $(ADVAPI32) $(MSVCRT) $(EXTRA) user/message.txt
	@mkdir -p $(BUILD)
	dd if=/dev/zero of=$(DISK) bs=1M count=64 status=none
	mformat -i $(DISK) -F -v SVINDOWS ::
	mcopy -i $(DISK) $(TESTAPP) ::TESTAPP.EXE
	mcopy -i $(DISK) $(HELLO) ::HELLO.EXE
	mcopy -i $(DISK) $(FILETEST) ::FILETEST.EXE
	mcopy -i $(DISK) $(CAT) ::CAT.EXE
	mcopy -i $(DISK) $(CONSOLETEST) ::CONSOLE.EXE
	mcopy -i $(DISK) $(ABITEST) ::ABITEST.EXE
	mcopy -i $(DISK) $(SEHTEST) ::SEHTEST.EXE
	mcopy -i $(DISK) $(SEHCRASH) ::SEHCRASH.EXE
	mcopy -i $(DISK) $(SEHFRAME) ::SEHFRAME.EXE
	mcopy -i $(DISK) $(CPPTEST) ::CPPTEST.EXE
	mcopy -i $(DISK) $(FETCH) ::FETCH.EXE
	mcopy -i $(DISK) $(CMD) ::CMD.EXE
	mcopy -i $(DISK) $(KERNEL32) ::KERNEL32.DLL
	mcopy -i $(DISK) $(NTDLL) ::NTDLL.DLL
	mcopy -i $(DISK) $(ADVAPI32) ::ADVAPI32.DLL
	mcopy -i $(DISK) $(MSVCRT) ::MSVCRT.DLL
	mcopy -i $(DISK) $(EXTRA) ::EXTRA.DLL
	mcopy -i $(DISK) user/message.txt ::MESSAGE.TXT
	@echo "  DISK  $(DISK)"

$(KERNEL): $(OBJ) kernel/arch/x86_64/linker.ld
	@mkdir -p $(dir $@)
	$(LD) $(LDFLAGS) -o $@ $(OBJ)
	@echo "  LD    $@"

iso: $(ISO)

$(ISO): $(KERNEL) boot/grub.cfg
	@mkdir -p $(BUILD)/isodir/boot/grub
	@cp $(KERNEL) $(BUILD)/isodir/boot/svindowskrnl.elf
	@cp boot/grub.cfg $(BUILD)/isodir/boot/grub/grub.cfg
	grub2-mkrescue -o $@ $(BUILD)/isodir 2>/dev/null
	@echo "  ISO   $@"

run-serial: all
	$(QEMU) $(QEMUFLAGS) -cdrom $(ISO) $(QEMUDISK) -serial stdio -display none

run: run-serial

run-gui: all
	$(QEMU) $(QEMUFLAGS) -cdrom $(ISO) $(QEMUDISK) -serial stdio

test: all
	$(PYTHON) tools/qemu_test.py --name regression \
		--wait-for 'C:\>' \
		--send $$'set V=RECALL_OK\recho %V%: PASS\r\x1b[A\recho ARROW_EDIT: PASX\x1b[D\x1b[3~S\rdir\rabitest.exe\rsehtest.exe\rsehframe.exe\rcpptest.exe\rsehcrash.exe\recho UNHANDLED_TEST: PASS\rconsole.exe\rfiletest.exe\rfetch.exe\r' \
		--expect-count 'RECALL_OK: PASS=2' \
		--expect 'ARROW_EDIT: PASS' \
		--expect 'MESSAGE.TXT' \
		--expect 'ABI_TEST: PASS' \
		--expect 'MUTANT_TEST: PASS' \
		--expect 'VM_TEST: PASS' \
		--expect 'SEH_TEST: PASS' \
		--expect 'SEH_FRAME_TEST: PASS' \
		--expect 'SEH_FINALLY_TEST: PASS' \
		--expect 'CPP_EH_TEST: PASS' \
		--expect 'UNHANDLED_TEST: PASS' \
		--expect 'CONSOLE_TEST: PASS' \
		--expect 'FAT32_TEST: PASS' \
		--expect 'Svindows 0.6.0 x86_64'

test-release:
	$(MAKE) CONFIG=release all
	$(MAKE) CONFIG=release test

test-exceptions: all
	$(PYTHON) tools/qemu_test.py --name exceptions \
		--wait-for 'C:\>' \
		--send $$'sehtest.exe\rsehframe.exe\rcpptest.exe\rsehcrash.exe\recho UNHANDLED_TEST: PASS\r' \
		--expect 'SEH_TEST: PASS' --expect 'SEH_FRAME_TEST: PASS' \
		--expect 'SEH_FINALLY_TEST: PASS' \
		--expect 'CPP_EH_TEST: PASS' \
		--expect 'UNHANDLED_TEST: PASS'

test-fs-reboot: all
	$(PYTHON) tools/qemu_test.py --name fs-write \
		--wait-for 'C:\>' --send $$'filetest.exe\r' \
		--expect 'FAT32_TEST: PASS'
	$(PYTHON) tools/qemu_test.py --name fs-reboot \
		--wait-for 'C:\>' --send $$'type WRITE.TXT\r' \
		--expect 'FAT32 writes survive close and reopen.'

clean:
	rm -rf $(BUILD)
