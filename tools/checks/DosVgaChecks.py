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
prelude = r'''
#include <stdint.h>
#include <stdlib.h>
#include <assert.h>
typedef uint8_t U8;
typedef uint32_t U32;
#define TRUE 1
#define VGA_MEMORY_BASE 0xa0000u
typedef struct {
  U8 VideoDirty, VideoMode, VgaSequencer[5], VgaGraphics[9], VgaPlanes[4][65536];
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
  free(v->Memory); free(v);
  return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='sv-dos-vga-') as directory:
    cfile = Path(directory) / 'check.c'
    exe = Path(directory) / 'check'
    cfile.write_text(prelude + source[start:end] + checks)
    subprocess.run([os.environ.get('HOSTCC', 'cc'), '-std=c11', '-Wall', '-Wextra',
                    '-Werror', str(cfile), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
print('PASS DOS VGA plane masks and read-map addressing')
