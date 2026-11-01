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
typedef struct { U32 Eflags; U16 Flags; } DOS_VM;
static DOS_VM cpu;
static int parity(unsigned char v) { return !__builtin_parity((unsigned)v); }
'''
checks = r'''
int main(void) {
  /* Watcom's DOS-call wrapper encodes Carry as the sign of the return value:
   * RCL EAX,1; ROR EAX,1 must turn DOS error 2 into 80000002, not fd 2. */
  cpu.Eflags = CF | 2;
  U32 encoded = pm_shift(&cpu, 2, 2, 1, 32);
  assert(encoded == 5 && !(cpu.Eflags & CF));
  encoded = pm_shift(&cpu, 1, encoded, 1, 32);
  assert(encoded == 0x80000002u && (cpu.Eflags & CF));
  cpu.Eflags = ZF | 2;
  assert(pm_shift(&cpu, 1, 1, 1, 16) == 0x8000u);
  assert((cpu.Eflags & (ZF | CF)) == (ZF | CF));
  assert(pm_shift(&cpu, 1, 1, 1, 8) == 0x80u);
  cpu.Eflags = 2;
  assert(pm_shift(&cpu, 1, 0x12345678u, 4, 32) == 0x81234567u);
  cpu.Eflags = CF | ZF | 2;
  assert(pm_shift(&cpu, 1, 0x12345678u, 32, 32) == 0x12345678u);
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


# The production interpreter batches PIC/PIT/keyboard polling but preserves
# exact interrupt-shadow expiry. Compile the actual helper as a tiny host test.
poll_start = source.index('static __attribute__((always_inline)) inline int dos_hardware_poll_due(')
poll_end = source.index('\nstatic void dos_timer_update(', poll_start)
poll_source = source[poll_start:poll_end]
poll_prelude = r'''
#include <stdint.h>
#include <assert.h>
typedef uint8_t U8;
#define DOS_HW_POLL_INTERVAL 128u
typedef struct {
  U8 InterruptShadow;
  U8 HardwarePollCountdown;
} DOS_VM;
'''
poll_test = r'''
int main(void) {
  DOS_VM vm = {0};
  assert(dos_hardware_poll_due(&vm) == 1);
  assert(vm.HardwarePollCountdown == 127);
  for (unsigned i = 0; i < 127; ++i)
    assert(dos_hardware_poll_due(&vm) == 0);
  assert(vm.HardwarePollCountdown == 0);
  assert(dos_hardware_poll_due(&vm) == 1);

  /* A two-instruction shadow suppresses polling and forces the next boundary
   * to poll immediately after the protected instruction window expires. */
  vm.InterruptShadow = 2;
  assert(dos_hardware_poll_due(&vm) == 0);
  assert(vm.InterruptShadow == 1);
  assert(dos_hardware_poll_due(&vm) == 0);
  assert(vm.InterruptShadow == 0);
  assert(vm.HardwarePollCountdown == 0);
  assert(dos_hardware_poll_due(&vm) == 1);
  return 0;
}
'''

with tempfile.TemporaryDirectory(prefix='sv-dos-poll-') as directory:
    cfile = Path(directory) / 'poll.c'
    exe = Path(directory) / 'poll'
    cfile.write_text(poll_prelude + poll_source + '\n' + poll_test)
    subprocess.run([os.environ.get('HOSTCC', 'cc'), '-std=c11', '-Wall', '-Wextra',
                    '-Werror', str(cfile), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
print('PASS DOS hardware polling batch and interrupt-shadow regressions')

# Exercise the production flat-segment translation, batched protected fetch,
# and 8/16/32-bit guest-memory paths with a tiny host-side DOS VM fixture.
source = (root / 'kernel/dos/Cpu.c').read_text()
linear_start = source.rindex('static __attribute__((always_inline)) inline int protected_linear(')
linear_end = source.index('\nstatic int load_ldt(', linear_start)
linear_source = source[linear_start:linear_end]
reg_start = source.rindex('static __attribute__((always_inline)) inline U32 pm_register(DOS_VM *vm,')
reg_end = source.index('static U32 pm_read(', reg_start)
reg_source = source[reg_start:reg_end]
mem_start = source.rindex('static U32 pm_read(DOS_VM *vm,')
mem_end = source.index('\nstatic U16 dpmi_copy_descriptor(', mem_start)
mem_source = source[mem_start:mem_end]
rm_start = source.index('/* Short inline path for the overwhelmingly common DOS/4GW operand:')
rm_end = source.index('static int pm_load_segment(', rm_start)
rm_source = source[rm_start:rm_end]
fast_prelude = r'''
#include <stdint.h>
#include <assert.h>
#include <string.h>
#include <stdarg.h>
typedef uint8_t U8;
typedef uint16_t U16;
typedef uint32_t U32;
typedef uint64_t U64;
#define MEM_SIZE 0x200000u
#define CS 1
#define ES 0
#define SS 2
#define DS 3
#define AX 0
#define CX 1
#define SI 6
#define DI 7
#define TRUE 1
#define FALSE 0
typedef struct {
  U32 Base, Limit;
  U8 Access, Flags;
  int Valid;
} DOS_DESCRIPTOR;
typedef struct { U8 mod, reg, rm, is_reg, segment; U32 offset; } PM_MOD;
typedef struct {
  DOS_DESCRIPTOR SegmentCache[6];
  U16 Segments[6], Registers[8];
  U32 Registers32[8], Eip, Eflags, CurrentInstructionIp;
  U8 CurrentOpcode, Running, VideoMode, VgaSequencer[5];
  int VideoDirty;
  U8 *Memory;
} DOS_VM;
static DOS_VM cpu;
static U8 arena[MEM_SIZE];
static unsigned video_reads, video_writes;
#define current() (&cpu)
#define mem (current()->Memory)
#define running (current()->Running)
#define seg (current()->Segments)
#define r32 (current()->Registers32)
static void fail(const char *message) { (void)message; cpu.Running = 0; }
static void KeLog(const char *format, ...) { (void)format; }
static U8 DosVideoReadMemory(DOS_VM *vm, U32 linear) {
  video_reads++;
  return vm->Memory[linear];
}
static void DosVideoWriteMemory(DOS_VM *vm, U32 linear, U8 value) {
  video_writes++;
  vm->VideoDirty = 1;
  vm->Memory[linear] = value;
}
'''
fast_checks = r'''
int main(void) {
  U32 linear = 0;
  cpu.Memory = arena;
  cpu.Running = 1;

  /* Register helpers use the VM pointer directly and preserve x86 byte/word
   * register semantics without re-querying the current kernel thread. */
  pm_set_register(&cpu, 0, 32, 0x12345678u);
  assert(pm_register(&cpu, 0, 32) == 0x12345678u);
  pm_set_register(&cpu, 4, 8, 0xabu);
  assert(pm_register(&cpu, 4, 8) == 0xabu);
  assert(pm_register(&cpu, 0, 8) == 0x78u);
  pm_set_register(&cpu, 0, 16, 0xbeefu);
  assert(pm_register(&cpu, 0, 32) == 0x1234beefu);
  assert(cpu.Registers[0] == 0xbeefu);

  /* Flat 4-GiB descriptors use the direct VM-arena bounds path. */
  cpu.SegmentCache[DS] = (DOS_DESCRIPTOR){0, 0xffffffffu, 0x92, 4, 1};
  assert(protected_linear(&cpu, DS, 0x1234, 4, 1, 0, &linear));
  assert(linear == 0x1234);
  assert(!protected_linear(&cpu, DS, MEM_SIZE - 2, 4, 0, 0, &linear));
  assert(!protected_linear(&cpu, DS, 0, 1, 0, 1, &linear));
  U32 span = 0;
  assert(dos_string_span(&cpu, DS, 0x100, 4, 4, 1, 0, 0, &linear, &span));
  assert(linear == 0x100 && span == 16);
  assert(dos_string_span(&cpu, DS, 0x10c, 4, 4, 1, 1, 0, &linear, &span));
  assert(linear == 0x100 && span == 16);
  assert(!dos_string_span(&cpu, DS, 0xfff8, 4, 4, 0, 0, 0, &linear, &span));
  assert(!dos_string_span(&cpu, DS, 0x9fff8, 4, 4, 1, 0, 0, &linear, &span));

  /* Non-flat DPMI segments retain base/limit validation. */
  cpu.SegmentCache[DS] = (DOS_DESCRIPTOR){0x1000, 0x0fff, 0x92, 4, 1};
  assert(protected_linear(&cpu, DS, 0x20, 2, 1, 0, &linear));
  assert(linear == 0x1020);
  arena[0x1020] = 0x5a;
  assert(pm_read_rm(&cpu, (PM_MOD){0,0,0,0,DS,0x20}, 8) == 0x5a);
  assert(pm_write_rm(&cpu, (PM_MOD){0,0,0,0,DS,0x21}, 8, 0xa5));
  assert(arena[0x1021] == 0xa5);
  assert(!protected_linear(&cpu, DS, 0x0fff, 2, 0, 0, &linear));

  /* A 32-bit code fetch loads an unaligned immediate in one operation. */
  cpu.SegmentCache[CS] = (DOS_DESCRIPTOR){0, 0xffffffffu, 0x9a, 4, 1};
  arena[1] = 0x34; arena[2] = 0x12; arena[3] = 0x78; arena[4] = 0x56;
  cpu.Eip = 1;
  assert(protected_fetch(&cpu, 4) == 0x56781234u);
  assert(cpu.Eip == 5);

  /* A 16-bit CS still wraps EIP between bytes at the 64-KiB boundary. */
  cpu.SegmentCache[CS].Flags = 0;
  arena[0xffff] = 0x78; arena[0] = 0x56;
  cpu.Eip = 0xffff;
  assert(protected_fetch(&cpu, 2) == 0x5678u);
  assert(cpu.Eip == 1);

  /* Normal flat memory uses single-width loads/stores, including unaligned. */
  cpu.SegmentCache[DS] = (DOS_DESCRIPTOR){0, 0xffffffffu, 0x92, 4, 1};
  arena[0x2001] = 0x78; arena[0x2002] = 0x56;
  arena[0x2003] = 0x34; arena[0x2004] = 0x12;
  assert(pm_read(&cpu, DS, 0x2001, 32) == 0x12345678u);
  assert(pm_write(&cpu, DS, 0x2011, 32, 0xa1b2c3d4u));
  assert(arena[0x2011] == 0xd4 && arena[0x2012] == 0xc3 &&
         arena[0x2013] == 0xb2 && arena[0x2014] == 0xa1);
  PM_MOD operand = {0, 0, 0, 0, DS, 0x2001};
  assert(pm_read_rm(&cpu, operand, 32) == 0x12345678u);
  assert(pm_write_rm(&cpu, (PM_MOD){0,0,0,0,DS,0x2021}, 32, 0xdecafbad));
  assert(arena[0x2021] == 0xad && arena[0x2024] == 0xde);

  /* Chain-4 VGA accesses bypass per-byte plane helpers and dirty the frame. */
  cpu.VideoMode = 0x13; cpu.VgaSequencer[4] = 8; cpu.VideoDirty = 0;
  video_writes = 0;
  assert(pm_write(&cpu, DS, 0xa0001, 32, 0x78563412u));
  assert(cpu.VideoDirty && video_writes == 0);
  assert(arena[0xa0001] == 0x12 && arena[0xa0004] == 0x78);
  assert(pm_write_rm(&cpu, (PM_MOD){0,0,0,0,DS,0xa0010}, 16, 0x2211));
  assert(arena[0xa0010] == 0x11 && arena[0xa0011] == 0x22);

  /* Planar and boundary-crossing VGA accesses retain the byte-wise device
   * path rather than silently treating those bytes as linear RAM. */
  cpu.VgaSequencer[4] = 0; cpu.VideoDirty = 0; video_writes = 0;
  assert(pm_write_rm(&cpu, (PM_MOD){0,0,0,0,DS,0xa0008}, 16, 0xbeefu));
  assert(video_writes == 2 && cpu.VideoDirty);
  video_writes = 0;
  assert(pm_write(&cpu, DS, 0x9ffff, 16, 0x1234u));
  assert(arena[0x9ffff] == 0x34 && arena[0xa0000] == 0x12);
  assert(video_writes == 1);

  video_reads = 0;
  arena[0xa0010] = 0xaa; arena[0xa0011] = 0xbb;
  assert(pm_read_rm(&cpu, (PM_MOD){0,0,0,0,DS,0xa0010}, 16) == 0xbbaau);
  assert(video_reads == 2);
  return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='sv-dos-fastpath-') as directory:
    cfile = Path(directory) / 'fastpath.c'
    exe = Path(directory) / 'fastpath'
    cfile.write_text(fast_prelude + linear_source + '\n' + reg_source + '\n' + mem_source + '\n' + rm_source + '\n' + fast_checks)
    subprocess.run([os.environ.get('HOSTCC', 'cc'), '-std=gnu11', '-Wall', '-Wextra',
                    '-Werror', str(cfile), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
print('PASS DOS flat-segment, bulk-fetch, VM-context register, and fast-memory regressions')




# The main DOS runner carries its DOS_VM pointer through the instruction loop.
# In protected mode this avoids a current-thread/context lookup per instruction.
dos_source = (root / 'kernel/dos/Dos.c').read_text()
runner_start = dos_source.index('U8 run_vm(DOS_VM *vm) {')
runner_end = dos_source.index('\nvoid DosRun(', runner_start)
runner = dos_source[runner_start:runner_end]
assert 'while (vm->Running)' in runner
assert 'DosCpuRunBatch(vm, DOS_CPU_BATCH_SIZE)' in runner
assert 'KeGetCurrentThread' not in runner
assert 'step_vm(vm);' not in runner
step_start = source.index('static __attribute__((always_inline)) inline void step_vm_body(DOS_VM *vm) {')
step_end = source.index('\n/* Public one-step entry', step_start)
step_body = source[step_start:step_end]
assert 'DOS_VM *vm = current()' not in step_body
assert 'step_protected(vm);' in step_body
assert 'dos_dispatch_hardware(vm)' in step_body
batch_start = source.index('U32 DosCpuRunBatch(DOS_VM *vm, U32 max_steps) {')
batch_end = source.index('\n/* Compatibility entry point', batch_start)
batch_body = source[batch_start:batch_end]
assert 'vm->Steps++;' in batch_body
assert 'step_vm_body(vm);' in batch_body
assert 'executed < max_steps && vm->Running' in batch_body
print('PASS batched DOS VM-context hot instruction loop')

# Flat DOS/4GW instruction fetch takes the short linear-RAM path; unusual
# descriptors and address boundaries still use the checked translation path.
assert 'cs->Base == 0 && cs->Limit == 0xffffffffu' in source
assert 'vm->Eip < MEM_SIZE && width <= MEM_SIZE - vm->Eip' in source
# DOS/4GW's identical-image EXEC is a bootstrap transition, so the top-level
# stub returns the child's exit code instead of re-entering the launcher.
dos_source = (root / 'kernel/dos/Dos.c').read_text()
interrupt_source = (root / 'kernel/dos/Interrupt.c').read_text()
assert 'vm->Dos4gImage = DosImageIsDos4g(image, (U32)size);' in dos_source
assert 'parent->ExecDepth == 0' in interrupt_source
assert 'DosImageHash((const U8 *)image, (U32)size) == parent->ImageHash' in interrupt_source
assert 'parent->Running = FALSE;' in interrupt_source
print('PASS DOS/4GW self-exec termination and flat instruction-fetch fast path')

# Run the interpreter's relative JMP cases with the production fetch helpers.
# The displacement is relative to IP after fetching the entire instruction.
jump_start = source.index('  case 0xe9:', source.index('inline void step_vm_body'))
jump_end = source.index('  case 0xd0:', jump_start)
fetch_start = source.index('static U8 fetch8(void)')
fetch_end = source.index('static U32 read32(', fetch_start)
jump_prelude = r'''
#include <stdint.h>
#include <assert.h>
typedef uint8_t U8;
typedef uint16_t U16;
typedef uint32_t U32;
typedef int32_t INT32;
typedef int64_t INT64;
#define CS 1
static U8 memory[65536];
static U16 ip, seg[6];
static U8 rb(U16 s, U16 o) { (void)s; return memory[o]; }
static void fail(const char *s) { (void)s; assert(0); }
'''
jump_test = r'''
int main(void) {
  ip = 0x100; memory[0x100] = 0xe9; memory[0x101] = 5;
  jump(); assert(ip == 0x108);
  ip = 0x100; memory[0x101] = 0xfd; memory[0x102] = 0xff;
  jump(); assert(ip == 0x100);
  ip = 0x100; memory[0x100] = 0xeb; memory[0x101] = 5;
  jump(); assert(ip == 0x107);
  ip = 0x100; memory[0x101] = 0xfe;
  jump(); assert(ip == 0x100);
  ip = 0xfffe; memory[0xfffe] = 0xeb; memory[0xffff] = 2;
  jump(); assert(ip == 2);
  ip = 0xfffe; memory[0xfffe] = 0xe9; memory[0xffff] = 0xfd;
  memory[0] = 0xff;
  jump(); assert(ip == 0xfffe);
  return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='sv-dos-jmp-') as directory:
    cfile = Path(directory) / 'jump.c'
    exe = Path(directory) / 'jump'
    cfile.write_text(jump_prelude + source[fetch_start:fetch_end] +
                    'static void jump(void) { int operand32 = 0; '
                    'switch (fetch8()) {\n' + source[jump_start:jump_end] +
                    'default: assert(0); }}\n' + jump_test)
    for optimization in ('-Og', '-O2'):
        subprocess.run([os.environ.get('HOSTCC', 'cc'), '-std=c11', optimization,
                        '-Wall', '-Wextra', '-Werror', str(cfile), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
print('PASS DOS relative JMP forward, backward and IP wraparound')
