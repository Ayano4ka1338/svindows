#!/usr/bin/env python3
"""Execute edge-case checks against the production DOS protected-mode shifter."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/dos/Cpu.c').read_text()
start = source.index('static U32 pm_shift(')
end = source.index('\nstatic U32 pm_stack_pointer(', start)
prelude = r'''
#include <stdint.h>
#include <assert.h>
typedef uint8_t U8;
typedef uint16_t U16;
typedef uint32_t U32;
#define CF 1u
#define ZF 0x40u
#define SF 0x80u
#define OF 0x800u
struct { U32 Eflags; U16 Flags; } cpu;
#define current() (&cpu)
static int parity(unsigned char v) { return !__builtin_parity((unsigned)v); }
'''
checks = r'''
int main(void) {
  /* Watcom's DOS-call wrapper encodes Carry as the sign of the return value:
   * RCL EAX,1; ROR EAX,1 must turn DOS error 2 into 80000002, not fd 2. */
  cpu.Eflags = CF | 2;
  U32 encoded = pm_shift(2, 2, 1, 32);
  assert(encoded == 5 && !(cpu.Eflags & CF));
  encoded = pm_shift(1, encoded, 1, 32);
  assert(encoded == 0x80000002u && (cpu.Eflags & CF));
  cpu.Eflags = ZF | 2;
  assert(pm_shift(1, 1, 1, 16) == 0x8000u);
  assert((cpu.Eflags & (ZF | CF)) == (ZF | CF));
  assert(pm_shift(1, 1, 1, 8) == 0x80u);
  cpu.Eflags = 2;
  assert(pm_shift(1, 0x12345678u, 4, 32) == 0x81234567u);
  cpu.Eflags = CF | ZF | 2;
  assert(pm_shift(1, 0x12345678u, 32, 32) == 0x12345678u);
  assert(cpu.Eflags == (CF | ZF | 2));
  return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='sv-dos-cpu-') as directory:
    cfile = Path(directory) / 'check.c'
    exe = Path(directory) / 'check'
    cfile.write_text(prelude + source[start:end] + checks)
    subprocess.run([os.environ.get('HOSTCC', 'cc'), '-std=c11', '-Wall', '-Wextra',
                    '-Werror', str(cfile), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
print('PASS DOS CPU rotate and DOS error-return regressions')
