#!/usr/bin/env python3
"""Check string-instruction bus accesses and time protected CPU checkpoints.

--cpu-source may select a saved Cpu.c for a comparable checkpoint microbenchmark.
This measures host checkpoint cost, not guest application FPS.
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--cpu-source', type=Path, default=ROOT/'kernel/dos/Cpu.c')
args = parser.parse_args()
source = args.cpu_source.read_text()
a = source.index('    U16 saved_regs[8], saved_segments[6];')
b = source.index('    U32 saved_ip', a)
checkpoint = source[a:b]
a = source.index('  if ((op >= 0xa4 && op <= 0xa7) || (op >= 0xaa && op <= 0xaf)) {')
b = source.index('  if (op >= 0x6c', a)
strings = source[a:b]
benchmark = r'''
#include <assert.h>
#include <stdio.h>
#include <time.h>
#include "kernel/dos/Internal.h"
#undef r
#undef seg
#undef ip
#undef flags
static __attribute__((noinline)) void checkpoint(DOS_VM *vm) {
''' + checkpoint + r'''
  /* Observe all saved bytes; prevent dead-store elimination across iterations. */
  __asm__ volatile("" : : "m"(saved_regs), "m"(saved_regs32),
                    "m"(saved_segments), "m"(saved_cache) : "memory");
  assert(!memcmp(saved_regs,vm->Registers,sizeof(saved_regs)));
  assert(!memcmp(saved_regs32,vm->Registers32,sizeof(saved_regs32)));
  assert(!memcmp(saved_segments,vm->Segments,sizeof(saved_segments)));
  assert(!memcmp(saved_cache,vm->SegmentCache,sizeof(saved_cache)));
}
int main(void) {
  static DOS_VM vm;
  for(unsigned i=0;i<sizeof(vm);i++) ((U8*)&vm)[i]=(U8)(i*37);
  checkpoint(&vm);
  struct timespec a,b;clock_gettime(CLOCK_MONOTONIC,&a);
  for(unsigned i=0;i<5000000;i++) checkpoint(&vm);
  clock_gettime(CLOCK_MONOTONIC,&b);
  long ns=(b.tv_sec-a.tv_sec)*1000000000L+b.tv_nsec-a.tv_nsec;
  printf("checkpoint: %ld.%02ld ns/instruction (5M saves, host microbenchmark)\n",
    ns/5000000L,(ns/50000L)%100);
}
'''
# A small bus spy executes the actual real-mode instruction body. Reads have
# side effects on VGA latches, so checking their absence is a correctness test.
harness = r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
void *memset(void *,int,size_t);
typedef uint8_t U8;typedef uint16_t U16;typedef uint32_t U32;typedef int32_t INT32;
enum {AX,CX,DX,BX,SP,BP,SI,DI};enum {ES,CS,SS,DS};
#define DF 0x400u
#define ZF 0x40u
static U16 r[8],seg[4],flags;
static U32 r32[8],reads,writes;
static U8 memory[0x200000];
static U8 rb(U16 s,U32 o) {reads++;return memory[((U32)s<<4)+o];}
static U16 rw(U16 s,U32 o) {U16 v=rb(s,o);return v|((U16)rb(s,o+1)<<8);}
static U32 read32(U16 s,U32 o) {U32 v=rw(s,o);return v|((U32)rw(s,o+2)<<16);}
static void wb(U16 s,U32 o,U8 v) {writes++;memory[((U32)s<<4)+o]=v;}
static void ww(U16 s,U32 o,U16 v) {wb(s,o,v);wb(s,(U16)(o+1),v>>8);}
static void write32(U16 s,U32 o,U32 v) {ww(s,o,v);ww(s,o+2,v>>16);}
static U8 get8(unsigned n) {return r[n];}
static void set8(unsigned n,U8 v) {r[n]=(r[n]&0xff00)|v;}
static U32 alu32(unsigned op,U32 a,U32 b) {(void)op;flags=(flags&~ZF)|(a==b?ZF:0);return a-b;}
static U16 alu(unsigned op,U16 a,U16 b,unsigned w) {(void)w;return alu32(op,a,b);}
static void execute(U8 op,int rep,int operand32,int address32) {int override=-1;
''' + strings + r'''
}
static void reset(void) {
 memset(r,0,sizeof(r));memset(r32,0,sizeof(r32));
 memset(memory,0,sizeof(memory));reads=writes=flags=0;
 seg[DS]=0x1000;seg[ES]=0xa000;
}
int main(void) {
 for(unsigned w=0;w<3;w++) {
  reset();r[AX]=0x3456;r32[AX]=0x12343456;r[SI]=7;r[DI]=4;r[CX]=3;
  unsigned bytes=1u<<w;
  execute(w?0xab:0xaa,0xf3,w==2,0);
  assert(reads==0 && writes==3*bytes && !r[CX] && r[SI]==7 && r[DI]==4+3*bytes);
  assert(memory[0xa0004]==0x56);
 }
 reset();r[CX]=3;execute(0xa4,0xf3,0,0);
 assert(reads==3 && writes==3 && r[SI]==3 && r[DI]==3);
 reset();r[CX]=3;execute(0xac,0xf3,0,0);
 assert(reads==3 && writes==0 && r[SI]==3 && r[DI]==0);
 reset();r[CX]=3;execute(0xae,0xf3,0,0);
 assert(reads==3 && writes==0 && r[SI]==0 && r[DI]==3);
 reset();r[CX]=3;execute(0xa6,0xf3,0,0);
 assert(reads==6 && !writes && r[SI]==3 && r[DI]==3);
 reset();r[CX]=3;r[AX]=1;execute(0xae,0xf3,0,0);
 assert(reads==1 && r[CX]==2 && r[DI]==1);
 reset();r[CX]=0;execute(0xab,0xf3,0,0);assert(!reads && !writes);
 reset();r[CX]=2;r[DI]=0xffff;execute(0xaa,0xf3,0,0);
 assert(!reads && writes==2 && r[DI]==1);
 reset();flags=DF;r[CX]=3;r[DI]=2;execute(0xaa,0xf3,0,0);
 assert(!reads && writes==3 && r[DI]==0xffff);
 reset();r32[CX]=2;r32[DI]=0x10000;r[AX]=1;execute(0xaa,0xf3,0,1);
 assert(!reads && writes==2 && r32[DI]==0x10002 && !r32[CX] && r[DI]==2);
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='sv-dos-perf-') as tmp:
    tmp = Path(tmp)
    common = [os.environ.get('HOSTCC','cc'),'-std=gnu11','-O2','-ffreestanding',
              '-mgeneral-regs-only','-Wall','-Wextra','-Werror','-Wno-multichar',
              '-I.','-Ikernel/include']
    (tmp/'benchmark.c').write_text(benchmark)
    subprocess.run(common+['-DNDEBUG',str(tmp/'benchmark.c'),'kernel/rtl/Memory.c',
                          '-o',str(tmp/'benchmark')],cwd=ROOT,check=True)
    subprocess.run([str(tmp/'benchmark')],check=True)
    # Baseline source can be timed even though it has the known spurious reads.
    if args.cpu_source.resolve() == (ROOT/'kernel/dos/Cpu.c').resolve():
        (tmp/'strings.c').write_text(harness)
        subprocess.run(common+[str(tmp/'strings.c'),'-o',str(tmp/'strings')],cwd=ROOT,check=True)
        subprocess.run([str(tmp/'strings')],check=True)
        print('PASS STOS/MOVS/LODS/CMPS/SCAS bus accesses, widths, DF, REP termination and wrap')
