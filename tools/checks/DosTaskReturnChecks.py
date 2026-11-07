#!/usr/bin/env python3
"""Exercise the actual DOSX INT 22h call-return trampoline and EXEC fallback."""
from pathlib import Path
import os
import subprocess
import tempfile
root=Path(__file__).resolve().parents[2]
source=(root/'kernel/dos/Interrupt.c').read_text()
a=source.index('static BOOLEAN dos_terminate_nested_psp(U8 code)')
b=source.index('\nstatic void dos_int_dispatch(',a)
harness=r'''
#include <assert.h>
#include "kernel/dos/Internal.h"
static KTHREAD thread;static KPRCB processor;
PKPRCB KeGetCurrentProcessorBlock(void) { processor.CurrentThread=&thread;return &processor; }
int KeLog(const char *fmt,...) { (void)fmt;return 0; }
void DosUpdateMemoryControlBlocks(DOS_VM *vm) { (void)vm; }
void dos_sync_system_data(void) {}
void DosVideoWriteMemory(DOS_VM *vm,U32 address,U8 value) { vm->Memory[address]=value; }
U8 DosVideoReadMemory(DOS_VM *vm,U32 address) { return vm->Memory[address]; }
'''+source[a:b]+r'''
int main(void) {
 static DOS_VM vm;static U8 memory[DOS_REAL_MEMORY_SIZE];vm.Memory=memory;thread.DosContext=&vm;
 vm.CurrentPsp=0x1200;ww(0x1200,0x16,PSP);ww(0x1200,0xa,0x1111);ww(0x1200,0xc,0x2400);
 ww(PSP,0x2e,0x400);ww(PSP,0x30,0x3000);
 seg[CS]=0x2000;ip=0x102;seg[SS]=0x3100;r[SP]=0x500;
 assert(dos_terminate_nested_psp(7));
 assert(seg[CS]==0x2400 && ip==0x1111 && seg[SS]==0x3000 && r[SP]==0x400);
 assert(vm.LastChildExitCode==7 && vm.ChildStatusAvailable && vm.CurrentPsp==PSP);
 /* Live DOSX frame: 0324:1894, FLAGS, then ES/DX/BX/AX saved by DOSX. */
 vm.CurrentPsp=0x1200;ww(0x1200,0xa,0x324);ww(0x1200,0xc,0x1894);
 seg[CS]=DOS_BUILTIN_SEGMENT;ip=DOS_BUILTIN_OFFSET(0x21);seg[SS]=0x1469;r[SP]=0x2332;
 ww(0x1469,0x2332,0x324);ww(0x1469,0x2334,0x1894);ww(0x1469,0x2336,0x322a);
 ww(0x1469,0x2338,0x4b);ww(0x1469,0x233a,0);ww(0x1469,0x233c,0x1a2f);ww(0x1469,0x233e,0);
 assert(dos_terminate_nested_psp(0));
 assert(seg[CS]==0x1894 && ip==0x324 && seg[SS]==0x1469 && r[SP]==0x2338);
 assert(rw(seg[SS],r[SP])==0x4b && (flags&0x200));
 /* A different INT 22h vector must still restore the parent's EXEC stack. */
 vm.CurrentPsp=0x1200;r[SP]=0x2332;ww(0x1200,0xa,0x325);
 seg[CS]=DOS_BUILTIN_SEGMENT;ip=DOS_BUILTIN_OFFSET(0x21);
 assert(dos_terminate_nested_psp(0));assert(seg[SS]==0x3000 && r[SP]==0x400);
 assert(!dos_terminate_nested_psp(0));
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='sv-dos-task-return-') as tmp:
    p=Path(tmp);(p/'test.c').write_text(harness)
    subprocess.run([os.environ.get('HOSTCC','cc'),'-std=gnu11','-O2','-Wall','-Wextra',
                    '-Werror','-Wno-multichar','-I.','-Ikernel/include',str(p/'test.c'),
                    '-o',str(p/'test')],cwd=root,check=True)
    subprocess.run([str(p/'test')],check=True)
print('PASS DOSX task call-return keeps saved ES/DX/BX/AX; nested EXEC still restores parent stack')
