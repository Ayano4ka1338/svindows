#!/usr/bin/env python3
"""Host tests for DOS/4GW image detection and its self-EXEC exit guard."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/dos/Dos.c').read_text()
start = source.index('static BOOLEAN dos_command_is_win100_bin(')
end = source.index('\nU16 free_paragraphs_from(', start)
helper_source = source[start:end]
image_start = source.index('BOOLEAN DosIsImage(')
image_end = source.index('\nNTSTATUS DosCreateVm(', image_start)
helper_source += source[image_start:image_end]
prelude = r'''
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
typedef uint8_t U8;
typedef uint8_t UINT8;
typedef uint32_t U32;
typedef size_t SIZE_T;
typedef int BOOLEAN;
#define TRUE 1
#define FALSE 0
'''
test = r'''
int main(int argc, char **argv) {
  static const U8 doom_marker[] = {0x4d,0x5a,0x00,0x00,'D','O','S','/','4','G','W'};
  static const U8 other_image[] = {0x4d,0x5a,0x00,0x00,'D','O','S','/','4','G','X'};
  assert(DosImageIsDos4g(doom_marker, sizeof(doom_marker)) == TRUE);
  assert(DosImageIsDos4g(other_image, sizeof(other_image)) == FALSE);
  assert(DosImageIsDos4g(doom_marker, 6) == FALSE);
  assert(DosImageIsDos4g(0, 0) == FALSE);
  assert(DosImageHash(doom_marker, sizeof(doom_marker)) == 0x3f59e251u);
  assert(DosImageHash(doom_marker, sizeof(doom_marker)) !=
         DosImageHash(other_image, sizeof(other_image)));
  assert(dos_command_is_win100_bin("C:\\WINDOWS1\\WIN100.BIN"));
  assert(dos_command_is_win100_bin("\"c:\\windows1\\win100.bin\" /n"));
  assert(!dos_command_is_win100_bin("C:\\WINDOWS1\\WIN.COM"));
  assert(!dos_command_is_win100_bin("C:\\DOOM\\DOOM.EXE"));

  /* Windows 1.x MZ/NE files carry a DOS entry stub and must be EXEC-able. */
  U8 win_ne[0x500] = {0};
  win_ne[0] = 'M'; win_ne[1] = 'Z';
  win_ne[0x3c] = 0x00; win_ne[0x3d] = 0x04;
  win_ne[0x400] = 'N'; win_ne[0x401] = 'E';
  assert(DosIsImage("C:\\WINDOWS1\\WIN100.BIN", win_ne, sizeof(win_ne)));
  U8 win_pe[0x500] = {0};
  win_pe[0] = 'M'; win_pe[1] = 'Z';
  win_pe[0x3c] = 0x00; win_pe[0x3d] = 0x04;
  win_pe[0x400] = 'P'; win_pe[0x401] = 'E';
  assert(!DosIsImage("C:\\WINDOWS1\\MODERN.EXE", win_pe, sizeof(win_pe)));
  U8 win_le[0x500] = {0};
  win_le[0] = 'M'; win_le[1] = 'Z';
  win_le[0x3c] = 0x00; win_le[0x3d] = 0x04;
  win_le[0x400] = 'L'; win_le[0x401] = 'E';
  assert(!DosIsImage("C:\\WINDOWS1\\DOS4GW.EXE", win_le, sizeof(win_le)));
  /* Exercise the real Windows 1.01 MZ/NE kernel, not only a synthetic header. */
  assert(argc == 2);
  FILE *win_file = fopen(argv[1], "rb");
  assert(win_file);
  assert(fseek(win_file, 0, SEEK_END) == 0);
  long win_size = ftell(win_file);
  assert(win_size > 0);
  rewind(win_file);
  U8 *win_image = malloc((size_t)win_size);
  assert(win_image);
  assert(fread(win_image, 1, (size_t)win_size, win_file) == (size_t)win_size);
  fclose(win_file);
  assert(win_image[0] == 'M' && win_image[1] == 'Z');
  U32 win_ne_offset = (U32)win_image[0x3c] |
      ((U32)win_image[0x3d] << 8) | ((U32)win_image[0x3e] << 16) |
      ((U32)win_image[0x3f] << 24);
  assert(win_ne_offset + 2 <= (U32)win_size);
  assert(win_image[win_ne_offset] == 'N' && win_image[win_ne_offset + 1] == 'E');
  assert(DosIsImage("C:\\WINDOWS1\\WIN100.BIN", win_image, (SIZE_T)win_size));
  free(win_image);
  return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='sv-dos-exec-') as directory:
    cfile = Path(directory) / 'exec.c'
    exe = Path(directory) / 'exec'
    cfile.write_text(prelude + helper_source + test)
    subprocess.run([os.environ.get('HOSTCC', 'cc'), '-std=c11', '-Wall', '-Wextra',
                    '-Werror', str(cfile), '-o', str(exe)], check=True)
    subprocess.run([str(exe), str(root / 'user/d/windows1/WIN100.BIN')], check=True)

interrupt = (root / 'kernel/dos/Interrupt.c').read_text()
assert 'parent->Dos4gImage && parent->ExecDepth == 0' in interrupt
assert 'size == parent->ImageSize' in interrupt
assert 'DosImageHash((const U8 *)image, (U32)size) == parent->ImageHash' in interrupt
assert 'parent->ExitCode = child_status;' in interrupt
assert 'parent->Running = FALSE;' in interrupt
assert 'vm->DosVersion = dos_command_is_win100_bin(command) ? 0x2203u : 0x0005u;' in source
win_com = (root / 'user/d/windows1/WIN.COM').read_bytes()
assert b'\xB8\x00\x4B\xCD\x21' in win_com  # WIN.COM EXECs WIN100.BIN via INT 21h/4B00h
assert 'r[AX] = current()->DosVersion;' in interrupt
assert 'image[pe + 1] == ' in source or "image[pe + 1] == 'E'" in source

doom = (root / 'user/d/doom/doom.exe').read_bytes()
assert b'DOS/4GW' in doom
assert len(doom) == 709905
doom_hash = 2166136261
for byte in doom:
    doom_hash = ((doom_hash ^ byte) * 16777619) & 0xffffffff
assert doom_hash == 0x4a434833
print('PASS DOS/4GW marker, image identity, and one-shot bootstrap termination checks')
