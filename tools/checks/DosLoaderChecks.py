#!/usr/bin/env python3
"""Execute the production MZ loader, including Windows 1.01 /HIGH placement."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/dos/Dos.c').read_text()
loader = source[source.index('static int load('):source.index('\nBOOLEAN DosIsImage(')]
interrupt = (root / 'kernel/dos/Interrupt.c').read_text()
psp_start = interrupt.index('  case 0x50: /* Set current PSP')
psp_end = interrupt.index('\n  default:', psp_start)
psp_service = ('static void dos_sync_system_data(void) {}\n'
               'static void psp_service(U8 ah) { switch (ah) {\n' +
               interrupt[psp_start:psp_end] + '\n}}\n')
prelude = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
typedef uint8_t U8;
typedef uint16_t U16;
typedef uint32_t U32;
typedef int BOOLEAN;
#define PSP 0x1000u
#define ES 0
#define CS 1
#define SS 2
#define DS 3
#define SP 4
#define BX 3
#define DX 2
#define SI 6
static struct { U16 InitialParagraphs, DtaSegment, DtaOffset, CurrentPsp; } vm;
#define current() (&vm)
static U8 mem[0x100000];
static U16 seg[6], r[8], ip;
static U16 word(const U8 *p) { return p[0] | ((U16)p[1] << 8); }
static U32 addr(U16 s, U16 o) { return ((U32)s << 4) + o; }
static U8 rb(U16 s, U16 o) { return mem[addr(s,o)]; }
static void wb(U16 s, U16 o, U8 v) { mem[addr(s,o)] = v; }
static U16 rw(U16 s, U16 o) { return word(mem + addr(s,o)); }
static void ww(U16 s, U16 o, U16 v) { wb(s,o,v); wb(s,o+1,v>>8); }
static void put(U8 *p, U16 v) { p[0]=v; p[1]=v>>8; }
'''
test = r'''
int main(int argc, char **argv) {
  assert(argc == 2);
  FILE *f=fopen(argv[1], "rb"); assert(f);
  fseek(f,0,SEEK_END); long size=ftell(f); rewind(f);
  U8 *image=malloc(size); assert(image);
  assert(fread(image,1,size,f)==(size_t)size); fclose(f);
  assert(!word(image+10) && !word(image+12));
  assert(load(image,size,""));
  assert(vm.InitialParagraphs == 0x9000 && rw(PSP,2) == 0xa000);
  U32 bytes = word(image+4)*512u - (512u-word(image+2)) - word(image+8)*16u;
  U16 base=(U16)(0xa000u-(bytes+15u)/16u);
  assert(seg[CS] == base+word(image+22));
  assert(seg[SS] == base+word(image+14));
  assert(r[SP] == word(image+16) && ip == word(image+20));
  assert(seg[DS] == PSP && seg[ES] == PSP);
  assert(!memcmp(mem+addr(base,0),image+word(image+8)*16u,bytes));
  free(image);

  /* Ordinary MZ: grant maximum extra paragraphs, clamp to memory capacity,
   * preserve minimum guarantees, and relocate relative to the load segment. */
  U8 mz[64]={0}; mz[0]='M'; mz[1]='Z';
  put(mz+2,64); put(mz+4,1); put(mz+8,2);
  put(mz+10,4); put(mz+12,8);
  put(mz+6,1); put(mz+24,28); put(mz+28,2);
  put(mz+34,0x1234);
  memset(mem,0,sizeof(mem)); assert(load(mz,sizeof(mz)," /x"));
  assert(vm.InitialParagraphs == 0x10+2+8);
  assert(seg[CS] == PSP+0x10);
  assert(rw(seg[CS],2) == 0x1234+PSP+0x10);
  assert(mem[addr(PSP,0x80)] == 3);
  put(mz+12,0xffff); assert(load(mz,sizeof(mz),""));
  assert(vm.InitialParagraphs == 0x9000);
  put(mz+12,3); assert(!load(mz,sizeof(mz),""));
  vm.CurrentPsp = PSP;
  for (unsigned i=0;i<256;i++) wb(PSP,i,(U8)i);
  for (unsigned i=0x22;i<=0x24;i++) { ww(0,i*4,0x1234+i);ww(0,i*4+2,0xf000); }
  r[DX]=0x3000;r[SI]=0x4000;wb(0x3000,256,0xa5);
  psp_service(0x55);
  assert(vm.CurrentPsp==0x3000 && rw(0x3000,2)==0x4000);
  assert(rw(0x3000,0x16)==PSP);
  assert(rw(0x3000,0x34)==0x18 && rw(0x3000,0x36)==0x3000);
  assert(rw(0x3000,0x0a)==0x1256 && rw(0x3000,0x0c)==0xf000);
  assert(mem[addr(0x3000,256)]==0xa5);
  assert(mem[addr(0x3000,0x81)]==0x81);
  psp_service(0x51);assert(r[BX]==0x3000);
  r[BX]=PSP;psp_service(0x50);psp_service(0x62);assert(r[BX]==PSP);
  return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='sv-dos-loader-') as directory:
    cfile = Path(directory) / 'loader.c'
    exe = Path(directory) / 'loader'
    cfile.write_text(prelude + loader + psp_service + test)
    subprocess.run([os.environ.get('HOSTCC', 'cc'), '-std=c11', '-O2',
                    '-Wall', '-Wextra', '-Werror', str(cfile), '-o', str(exe)], check=True)
    subprocess.run([str(exe), str(root / 'user/d/windows1/WIN100.BIN')], check=True)
print('PASS DOS MZ /HIGH Windows image, maxalloc, relocation and PSP creation/switching')
