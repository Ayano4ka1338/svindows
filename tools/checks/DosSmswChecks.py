#!/usr/bin/env python3
"""Check the production 0F 01 privilege dispatch used by Win16 CRT startup."""
from pathlib import Path
import os
import subprocess
import tempfile
root=Path(__file__).resolve().parents[2]
source=(root/'kernel/dos/Cpu.c').read_text()
a=source.index('    if (second == 0x01) {')
b=source.index('    if (second == 0x00) {',a)
body=source[a:b]
body=body[body.index('\n')+1:body.rfind('    }')]
body=body.replace('      PM_MOD m = pm_decode(vm, segment_override, address32);','')
harness=r'''
#include <assert.h>
#include "kernel/dos/Internal.h"
typedef struct { unsigned reg,segment,offset; } PM_MOD;
static unsigned failures,writes,result;
void fail(const char *message) { (void)message;failures++; }
static int pm_write(DOS_VM *vm,unsigned segment,U32 offset,unsigned width,U32 value) {
 (void)vm;(void)segment;(void)offset;(void)width;(void)value;writes++;return 1;
}
static void pm_write_rm(DOS_VM *vm,PM_MOD m,unsigned width,U32 value) {
 (void)vm;(void)m;assert(width==16);result=value;
}
static U32 pm_read(DOS_VM *vm,unsigned segment,U32 offset,unsigned width) {
 (void)vm;(void)segment;(void)offset;(void)width;return 0;
}
static U32 pm_read_rm(DOS_VM *vm,PM_MOD m,unsigned width) {
 (void)vm;(void)m;(void)width;return 0;
}
static void enter_protected_mode(U32 value) { (void)value; }
static void run(DOS_VM *vm,PM_MOD m) { unsigned width=16;
'''+body+r'''
}
int main(void) {
 static DOS_VM vm;
 vm.Cpl=3;vm.Cr0=0x80000031;
 run(&vm,(PM_MOD){.reg=4});assert(!failures && result==0x31);
 run(&vm,(PM_MOD){.reg=0});assert(!failures && writes==6);
 run(&vm,(PM_MOD){.reg=1});assert(!failures && writes==12);
 run(&vm,(PM_MOD){.reg=2});run(&vm,(PM_MOD){.reg=3});run(&vm,(PM_MOD){.reg=6});
 assert(failures==3 && writes==12);
 vm.Cpl=0;run(&vm,(PM_MOD){.reg=2});assert(failures==3);
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='sv-smsw-') as tmp:
    p=Path(tmp);(p/'test.c').write_text(harness)
    subprocess.run([os.environ.get('HOSTCC','cc'),'-std=gnu11','-O2','-Wall','-Wextra',
                    '-Werror','-Wno-multichar','-I.','-Ikernel/include',str(p/'test.c'),
                    '-o',str(p/'test')],cwd=root,check=True)
    subprocess.run([str(p/'test')],check=True)
print('PASS CPL3 SMSW/SGDT/SIDT and privileged LGDT/LIDT/LMSW dispatch')
