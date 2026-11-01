#!/usr/bin/env python3
"""Run production FCB searches against a deterministic FAT directory."""
from pathlib import Path
import os
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
harness = r'''
#include <assert.h>
#include <stdlib.h>
#include <string.h>
void *calloc(size_t count, size_t size);
void free(void *pointer);
#include "Internal.h"
static KTHREAD thread;
static KPROCESS process;
PKTHREAD KeGetCurrentThread(void) { return &thread; }
U8 DosVideoReadMemory(DOS_VM *v, U32 a) { return v->Memory[a]; }
void DosVideoWriteMemory(DOS_VM *v, U32 a, U8 b) { v->Memory[a]=b; }
NTSTATUS FatEnumerateDirectory(const char *path, UINT32 index, FAT_FIND_DATA *out) {
  assert(!strcmp(path,"C:\\WINDOWS1"));
  static const char *names[]={"ONE.EXE","SECRET.EXE","SUBDIR","TWO.EXE","SVINDOWS"};
  static const U8 attrs[]={0x20,2,0x10,0x20,8};
  if (index>=5) return STATUS_NO_MORE_FILES;
  memset(out,0,sizeof(*out)); strcpy(out->ShortName,names[index]);
  out->Attributes=attrs[index];out->Size=0x12345;out->WriteTime=0x4567;out->WriteDate=0x2345;
  return STATUS_SUCCESS;
}
int main(void) {
  DOS_VM *v=calloc(1,sizeof(*v)); assert(v);
  v->Memory=calloc(1,DOS_REAL_MEMORY_SIZE);assert(v->Memory);
  thread.DosContext=v;thread.Process=&process;strcpy(process.CurrentDirectory,"C:\\WINDOWS1");
  v->DtaSegment=0x2000;v->DtaOffset=0x100;
  U16 f=0x300;
  for (unsigned i=0;i<8;i++) wb(0x1000,f+1+i,'?');
  wb(0x1000,f+9,'E');wb(0x1000,f+10,'X');wb(0x1000,f+11,'E');
  wb(0x2000,0x121,0xa5);
  assert(dos_find_fcb(0x1000,f,TRUE));
  assert(rb(0x2000,0x100)==3);
  assert(!memcmp(v->Memory+0x20101,"ONE     EXE",11));
  assert(rb(0x2000,0x10c)==0x20);
  assert(rw(0x2000,0x117)==0x4567 && rw(0x2000,0x119)==0x2345);
  assert(rw(0x2000,0x11d)==0x2345 && rw(0x2000,0x11f)==1);
  assert(rb(0x2000,0x121)==0xa5);
  v->DtaOffset=0x200;
  assert(dos_find_fcb(0x1000,f,FALSE));
  assert(!memcmp(v->Memory+0x20201,"TWO     EXE",11));
  assert(!dos_find_fcb(0x1000,f,FALSE));
  /* Extended searches include directories; attribute 8 finds only labels. */
  wb(0x1000,0x500,0xff);wb(0x1000,0x506,0x10);
  for (unsigned i=0;i<11;i++)wb(0x1000,0x508+i,'?');
  assert(dos_find_fcb(0x1000,0x500,TRUE));
  assert(rb(0x2000,0x200)==0xff && rb(0x2000,0x206)==0x10);
  assert(dos_find_fcb(0x1000,0x500,FALSE));
  assert(!memcmp(v->Memory+0x20208,"SUBDIR     ",11));
  wb(0x1000,0x506,8);
  assert(dos_find_fcb(0x1000,0x500,TRUE));
  assert(!memcmp(v->Memory+0x20208,"SVINDOWS   ",11));
  wb(0x1000,f,1);assert(!dos_find_fcb(0x1000,f,TRUE));
  free(v->Memory);free(v);return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='sv-dos-fcb-search-') as directory:
    cfile = Path(directory) / 'fcb.c'
    exe = Path(directory) / 'fcb'
    cfile.write_text(harness)
    subprocess.run([os.environ.get('HOSTCC', 'cc'), '-std=gnu11', '-O2',
                    '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
                    '-Wall', '-Wextra', '-Werror', '-Wno-multichar',
                    '-Ikernel/include', '-Ikernel/dos', str(cfile),
                    'kernel/dos/DosFile.c', '-o', str(exe)], cwd=root, check=True)
    subprocess.run([str(exe)], check=True)
print('PASS DOS FCB first/next, DTA layout, attributes, cursor and bounds')
