#!/usr/bin/env python3
"""Exercise production DOS serial ports and the guest BIOS timer handler."""
from pathlib import Path
import os
import re
import subprocess
import tempfile
import shutil

root = Path(__file__).resolve().parents[2]
harness = r'''
#include <assert.h>
#include <stdlib.h>
void *calloc(size_t count, size_t size);
void free(void *pointer);
#include "Internal.h"
#include <svindows/Input.h>
MOUSE_STATE MouseState;
static KTHREAD thread;
PKTHREAD KeGetCurrentThread(void) { return &thread; }
UINT64 KeGetTickCount(void) { return 100; }
int KeLog(const char *fmt, ...) { (void)fmt; return 0; }
BOOLEAN KbdMirroredInputEventAvailable(void) { return FALSE; }
BOOLEAN KbdReadMirroredInputEvent(UINT32 *event) { (void)event; return FALSE; }
static unsigned framebuffer_users;
BOOLEAN GfxAvailable(void) { return TRUE; }
UINT32 GfxColor(UINT8 red, UINT8 green, UINT8 blue) { return ((U32)red<<16)|((U32)green<<8)|blue; }
void GfxClear(UINT32 color) { (void)color; }
void HalConsoleAcquireFramebuffer(void) { framebuffer_users++; }
BOOLEAN HalConsoleReleaseFramebuffer(void) { assert(framebuffer_users); return !--framebuffer_users; }
void ConRedraw(void) {}
int main(void) {
  DOS_VM *vm = calloc(1, sizeof(*vm)); assert(vm);
  vm->Memory = calloc(1, DOS_REAL_MEMORY_SIZE); assert(vm->Memory);
  thread.DosContext = vm;
  vm->KeyboardData = 0x1c; vm->KeyboardDataReady = TRUE;
  assert(DosPortIn(vm,0x60,8)==0x1c && !vm->KeyboardDataReady);
  assert(DosPortIn(vm,0x60,8)==0x1c);
  ww(0x40,0x1a,0x1e); ww(0x40,0x1c,0x1e);
  vm->KeyboardData=0x50; dos_bios_keyboard();
  assert(rw(0x40,0x1e)==0x5000 && rw(0x40,0x1c)==0x20);
  vm->KeyboardData=0xd0; dos_bios_keyboard();
  assert(rw(0x40,0x1c)==0x20);
  vm->KeyboardData=0x1c; dos_bios_keyboard();
  assert(rw(0x40,0x20)==0x1c0d);
  vm->KeyboardData=0x2a; dos_bios_keyboard();
  assert(rb(0x40,0x17)==2);
  vm->KeyboardData=0x1e; dos_bios_keyboard();
  assert(rw(0x40,0x22)==0x1e41);
  vm->KeyboardData=0xaa; dos_bios_keyboard();
  assert(rb(0x40,0x17)==0);
  DosVideoSetMode(vm,0x10);
  assert(vm->Memory[0x449]==0x10 && vm->Memory[0x44a]==80);
  vm->VgaPlanes[2][123]=0x5a; vm->Memory[0xb8000]=0x42;
  DOS_VM *child=calloc(1,sizeof(*child));assert(child);
  child->Memory=calloc(1,DOS_REAL_MEMORY_SIZE);assert(child->Memory);
  DosVideoInherit(child,vm);
  assert(child->VideoMode==0x10 && child->Memory[0x449]==0x10);
  assert(child->VgaPlanes[2][123]==0x5a && child->Memory[0xb8000]==0x42);
  assert(framebuffer_users==2);
  DosVideoSetMode(child,3);DosVideoSetMode(vm,3);
  assert(!framebuffer_users);
  free(child->Memory);free(child);
  for (U16 port=0x2f8; port<=0x3f8; port+=0x100) {
    assert(DosPortIn(vm,port+5,8) == 0x60);
    assert(!(DosPortIn(vm,port+5,8) & 1));
    assert(DosPortIn(vm,port+2,8) == 1);
    assert(DosPortIn(vm,port+6,8) == (port == 0x3f8 ? 0xb0 : 0));
    DosPortOut(vm,port+3,0x80,8);
    DosPortOut(vm,port,0x60,8);
    assert(DosPortIn(vm,port,8) == 0x60);
    DosPortOut(vm,port+3,2,8);
    assert(DosPortIn(vm,port+3,8) == 2);
    assert(DosPortIn(vm,port,8) == 0);
    DosPortOut(vm,port+1,0,8);
    assert(DosPortIn(vm,port+1,8) == 0);
    DosPortOut(vm,port+7,0x5a,8);
    assert(DosPortIn(vm,port+7,8) == 0x5a);
  }
  free(vm->Memory); free(vm); return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='sv-dos-hardware-') as directory:
    cfile = Path(directory) / 'ports.c'
    exe = Path(directory) / 'ports'
    interrupt = (root / 'kernel/dos/Interrupt.c').read_text()
    start = interrupt.index('static void dos_bios_keyboard(void)')
    end = interrupt.index('static void dos_mouse_dimensions(', start)
    cfile.write_text(harness.replace('int main(void)', interrupt[start:end] + 'int main(void)'))
    subprocess.run([os.environ.get('HOSTCC', 'cc'), '-std=gnu11', '-O2',
                    '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
                    '-Wall', '-Wextra', '-Werror', '-Ikernel/include', '-Ikernel/dos',
                    str(cfile), 'kernel/dos/Video.c', '-o', str(exe)], cwd=root, check=True)
    subprocess.run([str(exe)], check=True)

# Decode the emitted IRQ0 stub with ndisasm to check instruction boundaries,
# the conditional carry into the high word, and the normal interrupt return.
source = (root / 'kernel/dos/Dos.c').read_text()
body = source.split('static const U8 bios_timer[] = {', 1)[1].split('};', 1)[0]
body = re.sub(r'/\*.*?\*/', '', body, flags=re.S)
code = bytes(int(v,16) for v in re.findall(r'0x[0-9a-fA-F]+',body))
if shutil.which('ndisasm'):
    with tempfile.TemporaryDirectory(prefix='sv-dos-timer-') as directory:
        binary = Path(directory) / 'timer.bin'
        binary.write_bytes(code)
        assembly = subprocess.check_output(['ndisasm','-b16',str(binary)],text=True)
        assert 'inc word [0x46c]' in assembly
        assert 'inc word [0x46e]' in assembly
        assert 'jnz 0x10' in assembly
        assert 'int 0x1c' in assembly and assembly.rstrip().endswith('iret')
else:
    print('SKIP BIOS IRQ0 disassembly check (ndisasm is not installed)')
print('PASS DOS keyboard/BDA, display inheritance, idle UARTs and BIOS timer checks')
