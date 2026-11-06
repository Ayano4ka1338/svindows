#!/usr/bin/env python3
"""Exercise the DOS paragraph allocator's MCB-aware fit strategies."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
dos_source = (root / 'kernel/dos/Dos.c').read_text(encoding='utf-8')
start = dos_source.index('U16 free_paragraphs_from(U16 start) {')
end = dos_source.index('static int load(', start)
allocator = dos_source[start:end]
interrupt = (root / 'kernel/dos/Interrupt.c').read_text(encoding='utf-8')
system_data = (root / 'kernel/dos/DosSystemData.c').read_text(encoding='utf-8')

prelude = r'''
#include <assert.h>
#include <stdint.h>
typedef uint8_t U8;
typedef uint16_t U16;
typedef uint32_t U32;
typedef int BOOLEAN;
#define TRUE 1
#define DOS_MAX_BLOCKS 128u
#define PSP 0x1000u
typedef struct { U16 Segment, Paragraphs; BOOLEAN Used; U16 OwnerPsp; } DOS_BLOCK;
typedef struct {
  DOS_BLOCK Blocks[DOS_MAX_BLOCKS];
  U16 InitialParagraphs;
  U16 DosAllocationStrategy;
  U16 CurrentPsp;
} DOS_VM;
static DOS_VM test_vm;
static unsigned refresh_count;
static DOS_VM *current(void) { return &test_vm; }
void DosUpdateMemoryControlBlocks(DOS_VM *vm) {
  assert(vm == &test_vm);
  refresh_count++;
}
'''
checks = r'''
int main(void) {
  U16 a, b;
  test_vm.InitialParagraphs = 0x0100;
  test_vm.CurrentPsp = PSP;
  assert(dos_largest_free() == 0x8efe);
  /* First-fit allocations begin after, not on top of, the free MCB. */
  a = dos_alloc(0x20);
  assert(a == 0x1101);
  b = dos_alloc(0x20);
  assert(b == 0x1122);
  assert(refresh_count == 2);
  assert(dos_largest_free() == 0x8ebc);
  assert(free_paragraphs_from((U16)(b + 0x20 + 1)) > 0);

  /* Best-fit selects the smallest available gap that satisfies the request. */
  for (unsigned i = 0; i < DOS_MAX_BLOCKS; i++) test_vm.Blocks[i].Used = 0;
  test_vm.Blocks[0] = (DOS_BLOCK){0x1182, 0x10, 1, PSP};
  test_vm.Blocks[1] = (DOS_BLOCK){0x11b3, 0x10, 1, PSP};
  test_vm.DosAllocationStrategy = 1;
  a = dos_alloc(0x10);
  assert(a == 0x1193);

  /* Last-fit selects the highest fitting free range. */
  for (unsigned i = 0; i < DOS_MAX_BLOCKS; i++) test_vm.Blocks[i].Used = 0;
  test_vm.Blocks[0] = (DOS_BLOCK){0x1182, 0x10, 1, PSP};
  test_vm.DosAllocationStrategy = 2;
  a = dos_alloc(0x10);
  assert(a > 0x1182 + 0x10);
  assert(a + 0x10 < 0xa000);

  /* AH=58h exposes first/best/last fit and explicitly reports no UMB arena. */
  assert(interrupt_has_ah58);
  assert(system_data_has_dynamic_mcb_chain);
  return 0;
}
'''
# Keep the host unit focused on allocator mechanics; verify integration surfaces
# below in Python rather than injecting source strings into the C translation unit.
checks = checks.replace('  assert(interrupt_has_ah58);\n  assert(system_data_has_dynamic_mcb_chain);\n', '')
with tempfile.TemporaryDirectory(prefix='sv-dos-memory-') as directory:
    cfile = Path(directory) / 'check.c'
    exe = Path(directory) / 'check'
    cfile.write_text(prelude + allocator + checks, encoding='utf-8')
    subprocess.run([os.environ.get('HOSTCC', 'cc'), '-std=c11', '-Wall', '-Wextra',
                    '-Werror', str(cfile), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)

assert 'case 0x58:' in interrupt
for service in ('case 0x00: /* Get allocation strategy. */',
                'case 0x01: /* Set allocation strategy: first, best or last fit. */',
                'case 0x02: /* Get UMB link state;',
                'case 0x03: /* UMB linking'):
    assert service in interrupt, service
assert 'vm->DosAllocationStrategy' in dos_source
assert 'next->Segment - 1u' in system_data
assert 'vm->Blocks[i]' in system_data
assert 'DosUpdateMemoryControlBlocks(current());' in interrupt
print('PASS DOS MCB-aware allocation, first/best/last fit, INT 21h/AH=58h')
