#!/usr/bin/env python3
"""Check production VGA plane masks and read-map addressing on the host."""
from pathlib import Path
import os
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/dos/Video.c').read_text()
start = source.index('U8 DosVideoReadMemory(')
end = source.index('void DosVideoSetMode(', start)
palette_start = source.index('void DosVideoSetPaletteRegister(')
palette_end = source.index('static U32 dos_cga_scanline_offset(', palette_start)
palette_source = source[palette_start:palette_end]
scan_start = source.index('static U32 dos_cga_scanline_offset(')
scan_end = source.index('U8 DosVideoReadMemory(', scan_start)
scan_source = source[scan_start:scan_end]
prelude = r'''
#include <stdint.h>
#include <stdlib.h>
#include <assert.h>
typedef uint8_t U8;
typedef uint16_t U16;
typedef uint32_t U32;
#define TRUE 1
#define VGA_MEMORY_BASE 0xa0000u
typedef struct {
  U8 VideoDirty, VideoMode, VgaSequencer[5], VgaGraphics[9], VgaPlanes[4][65536];
  U8 VgaAttribute[0x15];
  U8 VgaLatches[4];
  U8 *Memory;
} DOS_VM;
'''
checks = r'''
int main(void) {
  DOS_VM *v = calloc(1, sizeof(*v));
  assert(v);
  v->Memory = calloc(1, 0x100000u);
  assert(v->Memory);
  v->VideoMode = 0x13;
  /* Unchained writes select planes, not adjacent linear pixels. */
  v->VgaSequencer[2] = 1;
  DosVideoWriteMemory(v, 0xa4000, 17);
  assert(v->VgaPlanes[0][0x4000] == 17);
  /* EGA mode 10h: map masks, latches, set/reset, bit masks and raster ops. */
  v->VideoMode = 0x10;
  v->VgaSequencer[2] = 0x0f;
  v->VgaGraphics[5] = 0;
  v->VgaGraphics[8] = 0xff;
  DosVideoWriteMemory(v, 0xa0000, 0x3c);
  for (unsigned p=0;p<4;p++) assert(v->VgaPlanes[p][0] == 0x3c);
  v->VgaGraphics[4] = 2;
  assert(DosVideoReadMemory(v,0xa0000) == 0x3c);
  v->VgaGraphics[1] = 0x0f; v->VgaGraphics[0] = 5;
  v->VgaGraphics[8] = 0xf0;
  DosVideoWriteMemory(v,0xa0000,0);
  assert(v->VgaPlanes[0][0] == 0xfc && v->VgaPlanes[1][0] == 0x0c);
  v->VgaGraphics[1] = 0; v->VgaGraphics[8] = 0xff;
  v->VgaGraphics[5] = 2;
  DosVideoWriteMemory(v,0xa0001,6);
  assert(v->VgaPlanes[0][1] == 0 && v->VgaPlanes[1][1] == 0xff);
  DosVideoReadMemory(v,0xa0001);
  v->VgaGraphics[5] = 1;
  DosVideoWriteMemory(v,0xa0002,0);
  for (unsigned p=0;p<4;p++) assert(v->VgaPlanes[p][2] == v->VgaPlanes[p][1]);
  v->VgaGraphics[5] = 8; v->VgaGraphics[7] = 0xf; v->VgaGraphics[2] = 6;
  assert(DosVideoReadMemory(v,0xa0001) == 0xff);
  v->VgaGraphics[2] = 5;
  assert(DosVideoReadMemory(v,0xa0001) == 0);
  v->VgaSequencer[2] = 1;
  v->VgaPlanes[0][3] = 0xaa;
  v->VgaGraphics[5] = 0; v->VgaGraphics[4] = 0;
  DosVideoReadMemory(v,0xa0003);
  v->VgaGraphics[3] = 0x18; /* XOR with the latched destination. */
  DosVideoWriteMemory(v,0xa0003,0x0f);
  assert(v->VgaPlanes[0][3] == 0xa5);
  DosVideoReadMemory(v,0xa0003);
  v->VgaGraphics[5] = 3; v->VgaGraphics[3] = 0; v->VgaGraphics[0] = 1;
  DosVideoWriteMemory(v,0xa0003,0xf0);
  assert(v->VgaPlanes[0][3] == 0xf5);
  v->VgaGraphics[5] = 0; v->VgaGraphics[3] = 1;
  DosVideoWriteMemory(v,0xa0003,1);
  assert(v->VgaPlanes[0][3] == 0x80);
  v->VideoMode = 0x13;
  v->VgaGraphics[5] = 0;
  assert(v->VgaPlanes[1][0x4000] == 0);
  v->VgaSequencer[2] = 10;
  DosVideoWriteMemory(v, 0xa4000, 31);
  assert(v->VgaPlanes[0][0x4000] == 17);
  assert(v->VgaPlanes[1][0x4000] == 31);
  assert(v->VgaPlanes[2][0x4000] == 0);
  assert(v->VgaPlanes[3][0x4000] == 31);
  v->VgaGraphics[4] = 3;
  assert(DosVideoReadMemory(v, 0xa4000) == 31);
  v->VgaGraphics[4] = 0;
  assert(DosVideoReadMemory(v, 0xa4000) == 17);
  /* Chained mode 13h keeps normal linear byte addressing. */
  v->VgaSequencer[4] = 8;
  DosVideoWriteMemory(v, 0xa4000, 99);
  assert(v->Memory[0xa4000] == 99);
  assert(DosVideoReadMemory(v, 0xa4000) == 99);
  assert(v->VgaPlanes[0][0x4000] == 17);
  /* BIOS INT 10h palette services update the same EGA registers. */
  DosVideoSetPaletteRegister(v, 0, 1);
  assert(v->VgaAttribute[0] == 1 && v->VideoDirty);
  DosVideoSetPaletteRegister(v, 0x11, 0x2a);
  assert(DosVideoGetPaletteRegister(v, 0x11) == 0x2a);
  DosVideoSetPaletteRegister(v, 20, 7);
  assert(DosVideoGetPaletteRegister(v, 20) == 0xff);
  /* CGA graphics scanlines are interleaved in two 8-KiB banks. */
  assert(dos_cga_scanline_offset(0) == 0);
  assert(dos_cga_scanline_offset(1) == 0x2000);
  assert(dos_cga_scanline_offset(2) == 80);
  assert(dos_cga_scanline_offset(3) == 0x2050);
  v->VideoMode = 0x06;
  DosVideoWritePixel(v, 0, 0, 1);
  DosVideoWritePixel(v, 7, 0, 1);
  DosVideoWritePixel(v, 8, 0, 1);
  DosVideoWritePixel(v, 1, 1, 1);
  assert(v->Memory[0xb8000] == 0x81);
  assert(v->Memory[0xb8001] == 0x80);
  assert(v->Memory[0xba000] == 0x40);
  assert(DosVideoReadPixel(v, 0, 0) == 1);
  assert(DosVideoReadPixel(v, 7, 0) == 1);
  assert(DosVideoReadPixel(v, 8, 0) == 1);
  assert(DosVideoReadPixel(v, 1, 1) == 1);
  DosVideoWritePixel(v, 7, 0, 0);
  assert(DosVideoReadPixel(v, 7, 0) == 0);
  v->VideoMode = 0x04;
  DosVideoWritePixel(v, 3, 1, 2);
  assert(DosVideoReadPixel(v, 3, 1) == 2);
  free(v->Memory); free(v);
  return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='sv-dos-vga-') as directory:
    cfile = Path(directory) / 'check.c'
    exe = Path(directory) / 'check'
    cfile.write_text(prelude + palette_source + scan_source + source[start:end] + checks)
    subprocess.run([os.environ.get('HOSTCC', 'cc'), '-std=c11', '-Wall', '-Wextra',
                    '-Werror', str(cfile), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
interrupt = (root / 'kernel/dos/Interrupt.c').read_text()
assert 'if (n == 0x10 && ah == 0x10)' in interrupt
assert 'case 0x00: /* Set one EGA attribute/palette register. */' in interrupt
assert 'DosVideoSetPaletteRegister(vm, palette_index, value);' in interrupt
assert 'case 0x09: /* Copy 16 palette registers and overscan to ES:DX. */' in interrupt
assert 'ah == 0x12 && get8(3) == 0x10u' in interrupt
assert 'if (n == 0x11)' in interrupt and 'if (n == 0x12)' in interrupt
assert 'U8 operation = get8(7)' in interrupt  # INT 10h/AH=0Bh uses BH
assert 'if (n == 0x10 && ah == 0x0b)' in interrupt
assert 'r[AX] = rw(0x40u, 0x10u);' in interrupt  # INT 11h returns BIOS BDA equipment word
# Windows 1.01 system files must be staged together under C:\WINDOWS1.
windows_files = [
    'ABOUT.TXT', 'CALC.EXE', 'CALENDAR.EXE', 'CARDFILE.EXE', 'CLIPBRD.EXE',
    'CLOCK.EXE', 'CONTROL.EXE', 'COURA.FON', 'COURB.FON', 'HELVA.FON',
    'HELVB.FON', 'HIFONTS.FON', 'HPLASER.DRV', 'LOFONTS.FON', 'MODERN.FON',
    'MOUSE.COM', 'MOUSE.INI', 'MOUSE.SYS', 'MSDOS.EXE', 'MSDOSD.EXE',
    'NOTEPAD.EXE', 'PAINT.EXE', 'REVERSI.EXE', 'ROMAN.FON', 'SCRIPT.FON',
    'SPOOLER.EXE', 'TERMINAL.EXE', 'TMSRA.FON', 'TMSRB.FON', 'WIN.CNF',
    'WIN.COM', 'WIN.INI', 'WIN100.BIN', 'WIN100.OVL', 'WINOLDAP.GRB',
    'WINOLDAP.MOD', 'WRITE.DAT', 'WRITE.EXE',
]
windows_dir = root / 'user/d/windows1'
actual_windows_files = {p.name for p in windows_dir.iterdir() if p.is_file()}
assert set(windows_files).issubset(actual_windows_files)
assert actual_windows_files.issubset(set(windows_files) | {'MOUSE.DRV'})
for name in windows_files:
    assert (windows_dir / name).is_file(), name
# Local extraction leftovers may exist in DOOM; staging must filter them out.
makefile = (root / 'Makefile').read_text()
assert 'DOOM_FILES  := $(filter-out $(addprefix user/d/doom/,$(WINDOWS_NAMES))' in makefile
makefile = (root / 'Makefile').read_text()
assert 'DOOM_FILES  := $(filter-out $(addprefix user/d/doom/,$(WINDOWS_NAMES)),$(sort $(wildcard user/d/doom/*)))' in makefile
assert 'WINDOWS1_FILES := $(sort $(wildcard user/d/windows1/*))' in makefile
assert 'mcopy -i $(INITRD) $(DOOM_FILES) ::DOOM/' in makefile
assert 'mcopy -i $(INITRD) $(WINDOWS1_FILES) ::WINDOWS1/' in makefile
assert 'mcopy -i $(INITRD) $(WINDOWS2_FILES) ::WINDOWS2/' in makefile
assert 'mcopy -i $(INITRD) user/d/win2.bat ::WIN2.BAT' in makefile
assert 'mcopy -i $(INITRD) user/d/win.bat ::WIN.BAT' in makefile
overlay = (windows_dir / 'WIN100.OVL').read_bytes()
assert overlay.startswith(b'C:\\WINDOWS1\\WIN100.OVL\0')
assert b'C:\\WINTEST\\WIN100.OVL' not in overlay
assert 'vm->IsWindows100 = dos_command_is_win100_bin(command);' in (root / 'kernel/dos/Dos.c').read_text()
assert 'vm->DosVersion = vm->IsWindows100 ? 0x2203u :' in (root / 'kernel/dos/Dos.c').read_text()
assert '(vm->IsWindows200 ? 0x0004u : 0x0005u)' in (root / 'kernel/dos/Dos.c').read_text()
assert 'r[AX] = current()->DosVersion;' in (root / 'kernel/dos/Interrupt.c').read_text()
assert 'r[BX] = 0x0005' in (root / 'kernel/dos/Interrupt.c').read_text()
assert (root / 'user/d/win.bat').read_bytes().replace(b'\r\n', b'\n').decode().splitlines() == ['@echo off', 'cd \\windows1', 'win.com', 'cd \\']
assert 'user/d/windows101' not in makefile
windows2 = root / 'user/d/windows2'
assert {'WIN.COM', 'WIN200.BIN', 'WIN200.OVL'}.issubset({p.name.upper() for p in windows2.iterdir() if p.is_file()})
assert not (windows2 / 'CTMOUSE.EXE').exists()
assert 'CTMOUSE' not in (windows2 / 'AUTOEXEC.BAT').read_text(encoding='ascii').upper()
assert 'CTMOUSE' not in (root / 'user/d/win2.bat').read_text(encoding='ascii').upper()
assert 'WINDOWS2_FILES := $(sort $(wildcard user/d/windows2/*))' in makefile
print('PASS DOS VGA planes, CGA scanline banks, BIOS EGA palette, Windows 1.x/2.x staging, cursor path, and SETVER versions')
