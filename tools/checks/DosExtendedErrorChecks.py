#!/usr/bin/env python3
"""Exercise production DOS AH=59h and the error-recording INT 21h wrapper."""
from pathlib import Path
import os
import subprocess
import tempfile
root=Path(__file__).resolve().parents[2]
source=(root/'kernel/dos/Interrupt.c').read_text()
a=source.index('  case 0x59: {');b=source.index('  case 0x58: {',a)
query=source[a:b]
wrapper=source[source.index('void dos_int(U8 n) {'):]
harness=r'''
#include <assert.h>
#include "kernel/dos/Internal.h"
void DosVideoWriteMemory(DOS_VM *vm,U32 address,U8 value) { vm->Memory[address]=value; }
U8 DosVideoReadMemory(DOS_VM *vm,U32 address) { return vm->Memory[address]; }
static KTHREAD thread;
static KPRCB processor;
PKPRCB KeGetCurrentProcessorBlock(void) { processor.CurrentThread=&thread;return &processor; }
PKTHREAD KeGetCurrentThread(void) { return &thread; }
static void query_error(void) {
 flags &= ~CF;
 switch(0x59) {
'''+query+r'''
 }
}
static void dos_int_dispatch(U8 n) {
 if(n==0x21 && get8(4)==0x59)query_error();
 else if(n==0x21 && get8(4)==0x3d) {r[AX]=2;flags|=CF;}
 else flags &= ~CF;
}
'''+wrapper+r'''
int main(void) {
 static DOS_VM vm;static U8 memory[DOS_REAL_MEMORY_SIZE];vm.Memory=memory;
 thread.DosContext=&vm;vm.CurrentPsp=0x1000;seg[SS]=0x2345;r[SP]=0x6789;
 r[AX]=0x3d00;dos_int(0x21);assert(vm.LastDosError==2);
 r[AX]=0x5906;r[BX]=0;dos_int(0x21);
 assert(r[AX]==2 && r[BX]==0x0804 && r[CX]==0x0200 && !(flags&CF));
 assert(vm.LastDosError==2 && !vm.InDosCount);
 r[AX]=0x5900;r[BX]=1;dos_int(0x21);assert((flags&CF) && r[AX]==1 && vm.LastDosError==2);
 r[AX]=0x3000;dos_int(0x21);assert(vm.LastDosError==2);
 vm.LastDosError=0;r[AX]=0x5900;r[BX]=0;dos_int(0x21);
 assert(!r[AX] && !r[BX] && !r[CX] && !(flags&CF));
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='sv-dos-error-') as tmp:
    p=Path(tmp);(p/'test.c').write_text(harness)
    subprocess.run([os.environ.get('HOSTCC','cc'),'-std=gnu11','-O2','-Wall','-Wextra',
                    '-Werror','-Wno-multichar','-I.','-Ikernel/include',str(p/'test.c'),
                    '-o',str(p/'test')],cwd=root,check=True)
    subprocess.run([str(p/'test')],check=True)
print('PASS DOS extended error recording, reserved AL, class/locus and query preservation')
