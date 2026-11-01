#!/usr/bin/env python3
"""Run native-host tests against the production DOS x86->x86-64 translator."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = r'''#include <assert.h>
#include <string.h>
void *malloc(size_t);
void free(void *);
#include <stdio.h>
#include <sys/mman.h>
#include "kernel/dos/Internal.h"

void *jit_test_allocations[256];
SIZE_T jit_test_allocation_sizes[256];
PVOID ExAllocatePoolWithTag(POOL_TYPE type, SIZE_T size, ULONG tag) {
  (void)type; (void)tag;
  SIZE_T mapped = (size + 4095u) & ~(SIZE_T)4095u;
  void *p = mmap(NULL, mapped, PROT_READ | PROT_WRITE | PROT_EXEC,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED) return NULL;
  memset(p, 0, mapped);
  /* Every test pool allocation is page-aligned and rounded to whole pages. */
  static unsigned next;
  jit_test_allocations[next % 256u] = p;
  jit_test_allocation_sizes[next % 256u] = mapped;
  next++;
  return p;
}
void ExFreePool(PVOID p) {
  /* The test fixture allocates exactly three pool objects and frees them in
   * arbitrary order; search the tiny mapping ledger. */
  extern void *jit_test_allocations[256];
  extern SIZE_T jit_test_allocation_sizes[256];
  for (unsigned i = 0; i < 256; ++i) if (jit_test_allocations[i] == p) {
    munmap(p, jit_test_allocation_sizes[i]);
    jit_test_allocations[i] = NULL;
    return;
  }
}
#include "kernel/dos/Jit.c"

static void init_vm(DOS_VM *vm) {
  memset(vm, 0, sizeof(*vm));
  vm->Memory = malloc(MEM_SIZE);
  assert(vm->Memory);
  memset(vm->Memory, 0, MEM_SIZE);
  assert(vm->Memory);
  vm->ProtectedMode = TRUE;
  vm->DpmiClient32 = TRUE;
  vm->Segments[CS] = 0x08;
  vm->Segments[DS] = 0x10;
  vm->Segments[SS] = 0x18;
  vm->Segments[ES] = 0x20;
  vm->SegmentCache[CS] = (DOS_DESCRIPTOR){0, MEM_SIZE - 1u, 0x9a, 4, TRUE};
  vm->SegmentCache[DS] = (DOS_DESCRIPTOR){0, MEM_SIZE - 1u, 0x92, 4, TRUE};
  vm->SegmentCache[SS] = (DOS_DESCRIPTOR){0, MEM_SIZE - 1u, 0x92, 4, TRUE};
  vm->SegmentCache[ES] = (DOS_DESCRIPTOR){0, MEM_SIZE - 1u, 0x92, 4, TRUE};
  vm->Eflags = 0x202;
  vm->HardwarePollCountdown = 127;
}
static void put(DOS_VM *vm, U32 at, const U8 *bytes, U32 count) {
  memcpy(vm->Memory + at, bytes, count);
}

int main(void) {
  DOS_VM *vm = malloc(sizeof(*vm));
  assert(vm);
  memset(vm, 0, sizeof(*vm));
  init_vm(vm);

  /* MOV EAX,5; MOV EBX,7; ADD EAX,EBX; JNZ +5. The generated host
   * instructions must update the DOS register image and take the guest branch. */
  const U8 arithmetic[] = {
    0xb8, 5, 0, 0, 0,  0xbb, 7, 0, 0, 0,
    0x01, 0xd8, 0x75, 0x05, 0xb8, 0, 0, 0, 0, 0x90
  };
  put(vm, 0x1000, arithmetic, sizeof(arithmetic));
  vm->Eip = 0x1000;
  U32 ran = DosJitRun(vm, 24);
  assert(ran == 5); /* the native conditional branch chains into the NOP target */
  assert(vm->Registers32[AX] == 12 && vm->Registers[AX] == 12);
  assert(vm->Eip == 0x1014);
  assert((vm->Eflags & (0x40u | 0x04u | 0x01u)) == 0x04u);
  U8 *cached = vm->JitCache->Entries[
      ((0x1000u >> 4) ^ (0x1000u >> 12) ^ 0x08u) % DOS_JIT_CACHE_ENTRIES].Code;
  assert(cached);

  /* A cache hit must run the same compiled block. */
  vm->Eip = 0x1000;
  vm->Registers32[AX] = vm->Registers32[BX] = 0;
  ran = DosJitRun(vm, 24);
  assert(ran == 5 && vm->Registers32[AX] == 12 && vm->Eip == 0x1014);

  /* 32-bit flat-RAM addressing: MOV EAX,[100h]; ADD EAX,3; MOV [104h],EAX. */
  const U8 memory_ops[] = {
    0x8b, 0x05, 0x00, 0x01, 0x00, 0x00,
    0x83, 0xc0, 0x03,
    0x89, 0x05, 0x04, 0x01, 0x00, 0x00
  };
  put(vm, 0x2000, memory_ops, sizeof(memory_ops));
  *(U32 *)(vm->Memory + 0x100) = 10;
  vm->Eip = 0x2000;
  ran = DosJitRun(vm, 24);
  assert(ran == 3 && *(U32 *)(vm->Memory + 0x104) == 13);
  assert(vm->Eip == 0x2000 + sizeof(memory_ops));

  /* SIB + scaled-index effective address (MOV EAX,[EBX+ESI*4+8]). */
  const U8 sib_ops[] = {0x8b, 0x44, 0xb3, 0x08};
  put(vm, 0x3000, sib_ops, sizeof(sib_ops));
  vm->Registers32[BX] = 0x400;
  vm->Registers32[SI] = 3;
  *(U32 *)(vm->Memory + 0x414) = 0xdecafbad;
  vm->Eip = 0x3000;
  ran = DosJitRun(vm, 8);
  assert(ran == 1 && vm->Registers32[AX] == 0xdecafbadu);

  /* An access into the VGA aperture must return to the checked interpreter,
   * not touch the backing memory through the JIT RAM shortcut. */
  const U8 vga_read[] = {0x8b, 0x05, 0x00, 0x00, 0x0a, 0x00};
  put(vm, 0x4000, vga_read, sizeof(vga_read));
  vm->Eip = 0x4000;
  ran = DosJitRun(vm, 8);
  assert(ran == 0 && vm->Eip == 0x4000);

  /* 32-bit stack operations must preserve the guest SP and low-word mirror. */
  const U8 stack_ops[] = {0x50, 0x5b}; /* push eax; pop ebx */
  put(vm, 0x6000, stack_ops, sizeof(stack_ops));
  vm->Eip = 0x6000;
  vm->Registers32[AX] = 0x12345678u;
  vm->Registers32[SP] = 0x3000u;
  vm->Registers[SP] = 0x3000u;
  ran = DosJitRun(vm, 8);
  assert(ran == 2 && vm->Registers32[BX] == 0x12345678u);
  assert(vm->Registers32[SP] == 0x3000u && vm->Registers[SP] == 0x3000u);
  assert(*(U32 *)(vm->Memory + 0x2ffcu) == 0x12345678u);

  /* A relative CALL and RET imm16 use the emulated guest stack, not the host stack. */
  const U8 direct_call[] = {0xe8, 0x00, 0x01, 0x00, 0x00};
  const U8 return_cleanup[] = {0xc2, 0x04, 0x00};
  put(vm, 0x8000, direct_call, sizeof(direct_call));
  put(vm, 0x8105, return_cleanup, sizeof(return_cleanup));
  vm->Eip = 0x8000;
  vm->Registers32[SP] = 0x4000u; vm->Registers[SP] = 0x4000u;
  ran = DosJitRun(vm, 1);
  assert(ran == 1 && vm->Eip == 0x8105 && vm->Registers32[SP] == 0x3ffcu);
  assert(*(U32 *)(vm->Memory + 0x3ffcu) == 0x8005u);
  ran = DosJitRun(vm, 1);
  assert(ran == 1 && vm->Eip == 0x8005 && vm->Registers32[SP] == 0x4004u);

  /* Indirect CALL/JMP and FF /6 PUSH dispatch through their native templates. */
  const U8 indirect_call[] = {0xff, 0xd0}; /* call eax */
  put(vm, 0x8200, indirect_call, sizeof(indirect_call));
  vm->Eip = 0x8200; vm->Registers32[AX] = 0x8300;
  vm->Registers32[SP] = 0x5000; vm->Registers[SP] = 0x5000;
  ran = DosJitRun(vm, 8);
  assert(ran == 1 && vm->Eip == 0x8300 && vm->Registers32[SP] == 0x4ffcu);
  assert(*(U32 *)(vm->Memory + 0x4ffcu) == 0x8202u);

  const U8 indirect_jump[] = {0xff, 0xe0}; /* jmp eax */
  put(vm, 0x8400, indirect_jump, sizeof(indirect_jump));
  vm->Eip = 0x8400; vm->Registers32[AX] = 0x8500;
  ran = DosJitRun(vm, 8);
  assert(ran == 1 && vm->Eip == 0x8500);

  const U8 push_operand[] = {0xff, 0xf0}; /* push eax */
  put(vm, 0x8600, push_operand, sizeof(push_operand));
  vm->Eip = 0x8600; vm->Registers32[AX] = 0xaabbccddu;
  vm->Registers32[SP] = 0x6000; vm->Registers[SP] = 0x6000;
  ran = DosJitRun(vm, 8);
  assert(ran == 1 && vm->Registers32[SP] == 0x5ffcu);
  assert(*(U32 *)(vm->Memory + 0x5ffcu) == 0xaabbccddu);

  /* A budget-truncated block is rebuilt when a larger poll window is available. */
  const U8 budget_block[] = {0xb8, 1, 0, 0, 0, 0xbb, 2, 0, 0, 0, 0x01, 0xd8};
  put(vm, 0x8800, budget_block, sizeof(budget_block));
  vm->Eip = 0x8800;
  assert(DosJitRun(vm, 1) == 1 && vm->Eip == 0x8805);
  vm->Eip = 0x8800;
  ran = DosJitRun(vm, 8);
  assert(ran == 3 && vm->Registers32[AX] == 3 && vm->Eip == 0x880c);

  /* Chained translated blocks never exceed the caller's timer/poll budget. */
  U8 nop_window[127];
  memset(nop_window, 0x90, sizeof(nop_window));
  put(vm, 0x9000, nop_window, sizeof(nop_window));
  vm->Eip = 0x9000;
  ran = DosJitRun(vm, 127);
  assert(ran == 127 && vm->Eip == 0x907fu);

  /* Shrinking the code descriptor limit must invalidate an old cached block. */
  const U8 limit_code[] = {0x90, 0x90, 0x90, 0x90};
  put(vm, 0x9100, limit_code, sizeof(limit_code));
  vm->SegmentCache[CS].Limit = 0x9fffu;
  vm->Eip = 0x9100;
  assert(DosJitRun(vm, 4) == 4);
  vm->SegmentCache[CS].Limit = 0x9101;
  vm->Eip = 0x9100;
  assert(DosJitRun(vm, 4) == 2 && vm->Eip == 0x9102);
  vm->SegmentCache[CS].Limit = MEM_SIZE - 1u;

  /* Common compiler arithmetic: signed IMUL and immediate/CL shift forms. */
  const U8 multiply_ops[] = {
    0xb8, 6, 0, 0, 0, 0xbb, 7, 0, 0, 0,
    0x0f, 0xaf, 0xc3,       /* imul eax,ebx */
    0x6b, 0xc0, 0xfb        /* imul eax,eax,-5 */
  };
  put(vm, 0x8700, multiply_ops, sizeof(multiply_ops));
  vm->Eip = 0x8700;
  ran = DosJitRun(vm, 8);
  assert(ran == 4 && vm->Registers32[AX] == (U32)-210);

  const U8 shift_ops[] = {
    0xb8, 3, 0, 0, 0, 0xc1, 0xe0, 3, 0xd1, 0xe8, 0xd3, 0xe0
  };
  put(vm, 0x8750, shift_ops, sizeof(shift_ops));
  vm->Eip = 0x8750;
  vm->Registers32[CX] = 2;
  ran = DosJitRun(vm, 8);
  assert(ran == 4 && vm->Registers32[AX] == 48);

  /* LEAVE follows the guest frame pointer and restores the emulated stack. */
  const U8 leave_op[] = {0xc9};
  put(vm, 0x8780, leave_op, sizeof(leave_op));
  *(U32 *)(vm->Memory + 0x7000) = 0x8888;
  vm->Eip = 0x8780;
  vm->Registers32[BP] = 0x7000; vm->Registers[BP] = 0x7000;
  vm->Registers32[SP] = 0x5000; vm->Registers[SP] = 0x5000;
  ran = DosJitRun(vm, 1);
  assert(ran == 1 && vm->Registers32[BP] == 0x8888 &&
         vm->Registers32[SP] == 0x7004 && vm->Registers[SP] == 0x7004);

  /* MOVZX/MOVSX byte/word loads and SETcc byte destinations are native too. */
  const U8 narrow_ops[] = {
    0x0f, 0xb6, 0xc4,                         /* movzx eax,ah */
    0x0f, 0xbe, 0x1d, 0x00, 0x18, 0x00, 0x00, /* movsx ebx,byte [1800h] */
    0x0f, 0xb7, 0x05, 0x02, 0x18, 0x00, 0x00, /* movzx eax,word [1802h] */
    0x0f, 0x94, 0xc4,                         /* setz ah */
    0x0f, 0x95, 0x05, 0x04, 0x18, 0x00, 0x00  /* setnz byte [1804h] */
  };
  put(vm, 0x8a00, narrow_ops, sizeof(narrow_ops));
  vm->Memory[0x1800] = 0x80;
  vm->Memory[0x1801] = 0x7f;
  *(U16 *)(vm->Memory + 0x1802) = 0xfedc;
  vm->Registers32[AX] = 0x12345678;
  vm->Registers[AX] = 0x5678;
  vm->Eflags |= 0x40u; /* ZF: SETZ=1 and SETNZ=0 */
  vm->Eip = 0x8a00;
  ran = DosJitRun(vm, 8);
  assert(ran == 5 && vm->Eip == 0x8a00 + sizeof(narrow_ops));
  assert(vm->Registers32[BX] == 0xffffff80u);
  assert(vm->Registers32[AX] == 0x000001dcu);
  assert(vm->Registers[AX] == 0x01dcu && vm->Memory[0x1804] == 0);
  assert((vm->Eflags & 0x40u) != 0 && ((vm->Registers32[AX] >> 8) & 0xffu) == 1);

  /* Byte MOV instructions use guarded RAM and preserve adjacent register bytes. */
  const U8 byte_ops[] = {
    0x8a, 0x25, 0x00, 0x19, 0x00, 0x00, /* mov ah,[1900h] */
    0x88, 0x25, 0x01, 0x19, 0x00, 0x00, /* mov [1901h],ah */
    0x8a, 0xdc                          /* mov bl,ah */
  };
  put(vm, 0x8a80, byte_ops, sizeof(byte_ops));
  vm->Memory[0x1900] = 0x91;
  vm->Registers32[AX] = 0x12345678;
  vm->Registers[AX] = 0x5678;
  vm->Registers32[BX] = 0x87654321;
  vm->Registers[BX] = 0x4321;
  vm->Eip = 0x8a80;
  ran = DosJitRun(vm, 8);
  assert(ran == 3 && vm->Eip == 0x8a80 + sizeof(byte_ops));
  assert(vm->Memory[0x1901] == 0x91);
  assert(vm->Registers32[AX] == 0x12349178 && vm->Registers[AX] == 0x9178);
  assert(vm->Registers32[BX] == 0x87654391 && vm->Registers[BX] == 0x4391);

  /* REP MOVSB/MOVSD executes as one checked native string operation. */
  const U8 rep_movsd[] = {0xf3, 0xa5};
  put(vm, 0x9200, rep_movsd, sizeof(rep_movsd));
  vm->Eip = 0x9200;
  vm->Registers32[SI] = 0x10000; vm->Registers[SI] = 0;
  vm->Registers32[DI] = 0x12000; vm->Registers[DI] = 0;
  vm->Registers32[CX] = 4; vm->Registers[CX] = 4;
  for (U32 i = 0; i < 4; ++i)
    *(U32 *)(vm->Memory + 0x10000 + i * 4) = 0xabc00000u + i;
  ran = DosJitRun(vm, 1);
  assert(ran == 1 && vm->Eip == 0x9202 && vm->Registers32[CX] == 0);
  assert(vm->Registers32[SI] == 0x10010 && vm->Registers32[DI] == 0x12010);
  assert(vm->Registers[SI] == 0x10 && vm->Registers[DI] == 0x2010);
  assert(memcmp(vm->Memory + 0x10000, vm->Memory + 0x12000, 16) == 0);

  /* Absolute moffs and C6 immediate-byte stores stay on the native path. */
  const U8 absolute_ops[] = {
    0xa1, 0x00, 0x1a, 0x00, 0x00,             /* mov eax,[1a00h] */
    0xa3, 0x04, 0x1a, 0x00, 0x00,             /* mov [1a04h],eax */
    0xa0, 0x08, 0x1a, 0x00, 0x00,             /* mov al,[1a08h] */
    0xa2, 0x09, 0x1a, 0x00, 0x00,             /* mov [1a09h],al */
    0xc6, 0x05, 0x0a, 0x1a, 0x00, 0x00, 0x7b, /* mov byte [1a0a],7b */
    0xc6, 0xc4, 0x55                          /* mov ah,55h */
  };
  *(U32 *)(vm->Memory + 0x1a00) = 0x89abcdefu;
  vm->Memory[0x1a08] = 0x42;
  put(vm, 0x8b00, absolute_ops, sizeof(absolute_ops));
  vm->Eip = 0x8b00;
  ran = DosJitRun(vm, 8);
  assert(ran == 6 && vm->Eip == 0x8b00 + sizeof(absolute_ops));
  assert(*(U32 *)(vm->Memory + 0x1a04) == 0x89abcdefu);
  assert(vm->Memory[0x1a09] == 0x42 && vm->Memory[0x1a0a] == 0x7b);
  assert(vm->Registers32[AX] == 0x89ab5542u && vm->Registers[AX] == 0x5542u);

  /* REP MOVSB copies byte buffers through the same guarded native path. */
  const U8 rep_movsb[] = {0xf3, 0xa4};
  put(vm, 0x9208, rep_movsb, sizeof(rep_movsb));
  vm->Eip = 0x9208;
  vm->Registers32[SI] = 0x14000; vm->Registers32[DI] = 0x15000;
  vm->Registers32[CX] = 257;
  for (U32 i = 0; i < 257; ++i) vm->Memory[0x14000 + i] = (U8)(i ^ 0xa5u);
  assert(DosJitRun(vm, 1) == 1 && vm->Eip == 0x920a);
  assert(vm->Registers32[SI] == 0x14101 && vm->Registers32[DI] == 0x15101 &&
         vm->Registers32[CX] == 0);
  assert(memcmp(vm->Memory + 0x14000, vm->Memory + 0x15000, 257) == 0);

  /* Large REP operations preserve the interpreter's 4096-element quantum. */
  put(vm, 0x9210, rep_movsd, sizeof(rep_movsd));
  vm->Eip = 0x9210;
  vm->Registers32[SI] = 0x20000; vm->Registers[SI] = 0;
  vm->Registers32[DI] = 0x30000; vm->Registers[DI] = 0;
  vm->Registers32[CX] = 4097; vm->Registers[CX] = 4097;
  for (U32 i = 0; i < 4097; ++i)
    *(U32 *)(vm->Memory + 0x20000 + i * 4) = 0x55000000u + i;
  assert(DosJitRun(vm, 1) == 1 && vm->Eip == 0x9210);
  assert(vm->Registers32[CX] == 1 && vm->Registers32[SI] == 0x24000 &&
         vm->Registers32[DI] == 0x34000);
  assert(memcmp(vm->Memory + 0x20000, vm->Memory + 0x30000, 4096u * 4u) == 0);
  assert(DosJitRun(vm, 1) == 1 && vm->Eip == 0x9212);
  assert(vm->Registers32[CX] == 0 &&
         *(U32 *)(vm->Memory + 0x34000) == 0x55001000u);

  /* REP STOSB/STOSD fills memory natively while preserving guest flags. */
  const U8 rep_stosd[] = {0xf3, 0xab};
  put(vm, 0x9240, rep_stosd, sizeof(rep_stosd));
  vm->Eip = 0x9240; vm->Registers32[AX] = 0xdeadbeefu;
  vm->Registers32[DI] = 0x40000; vm->Registers32[CX] = 11;
  vm->Eflags = (vm->Eflags & ~0x8d5u) | 0x845u;
  U32 saved_flags = vm->Eflags & 0x8d5u;
  assert(DosJitRun(vm, 1) == 1 && vm->Eip == 0x9242);
  assert(vm->Registers32[DI] == 0x4002cu && vm->Registers32[CX] == 0);
  for (U32 i = 0; i < 11; ++i)
    assert(*(U32 *)(vm->Memory + 0x40000 + i * 4) == 0xdeadbeefu);
  assert((vm->Eflags & 0x8d5u) == saved_flags);

  const U8 rep_stosb[] = {0xf3, 0xaa};
  put(vm, 0x9250, rep_stosb, sizeof(rep_stosb));
  vm->Eip = 0x9250; vm->Registers32[AX] = 0x1234567cu;
  vm->Registers32[DI] = 0x41000; vm->Registers32[CX] = 19;
  assert(DosJitRun(vm, 1) == 1 && vm->Eip == 0x9252);
  assert(vm->Registers32[DI] == 0x41013 && vm->Registers32[CX] == 0);
  for (U32 i = 0; i < 19; ++i) assert(vm->Memory[0x41000 + i] == 0x7c);

  /* Direction-flag and VGA ranges must side-exit to the checked interpreter. */
  put(vm, 0x9220, rep_movsd, sizeof(rep_movsd));
  vm->Eip = 0x9220; vm->Eflags |= 0x400u;
  vm->Registers32[SI] = 0x10000; vm->Registers32[DI] = 0x12000;
  vm->Registers32[CX] = 1;
  assert(DosJitRun(vm, 1) == 0 && vm->Eip == 0x9220);
  vm->Eflags &= ~0x400u;
  put(vm, 0x9230, rep_movsd, sizeof(rep_movsd));
  vm->Eip = 0x9230;
  vm->Registers32[SI] = 0xa0000; vm->Registers32[DI] = 0x12000;
  vm->Registers32[CX] = 1;
  assert(DosJitRun(vm, 1) == 0 && vm->Eip == 0x9230);

  /* Unsupported opcodes are negatively cached, but code writes still retranslate. */
  const U8 unsupported[] = {0xcc};
  put(vm, 0x9300, unsupported, sizeof(unsupported));
  vm->Eip = 0x9300;
  assert(DosJitRun(vm, 4) == 0);
  DOS_JIT_ENTRY *negative = &vm->JitCache->Entries[
      ((0x9300u >> 4) ^ (0x9300u >> 12) ^ vm->Segments[CS]) % DOS_JIT_CACHE_ENTRIES];
  assert(negative->NoJit && negative->SourceLength == 1);
  assert(DosJitRun(vm, 4) == 0);
  vm->Memory[0x9300] = 0x90;
  vm->Eip = 0x9300;
  assert(DosJitRun(vm, 4) == 1 && vm->Eip == 0x9301);

  /* The source-byte signature must invalidate translations after code writes. */
  const U8 self_mod[] = {0xb8, 1, 0, 0, 0};
  put(vm, 0x5000, self_mod, sizeof(self_mod));
  vm->Eip = 0x5000;
  assert(DosJitRun(vm, 4) == 1 && vm->Registers32[AX] == 1);
  vm->Memory[0x5001] = 9;
  vm->Eip = 0x5000;
  assert(DosJitRun(vm, 4) == 1 && vm->Registers32[AX] == 9);

  DosJitDestroy(vm);
  free(vm->Memory);
  free(vm);
  puts("PASS DOS native JIT arithmetic, IMUL, shifts, LEAVE, byte MOV, moffs/C6, MOVZX/MOVSX/SETcc, REP MOVSB/MOVSD/STOSB/STOSD, branch chaining, flat RAM, SIB, guarded VGA, stack/call/return, poll budget, negative-cache and self-modifying-code safety checks");
  return 0;
}
'''

with tempfile.TemporaryDirectory(prefix='sv-dos-jit-') as directory:
    cfile = Path(directory) / 'jit_check.c'
    exe = Path(directory) / 'jit_check'
    cfile.write_text(source)
    command = [os.environ.get('HOSTCC', 'cc'), '-std=gnu11', '-O2', '-Wall',
               '-Wextra', '-Werror', '-Wno-multichar', '-I', str(root), '-I', str(root / 'kernel/include'),
               str(cfile), '-o', str(exe)]
    subprocess.run(command, check=True)
    subprocess.run([str(exe)], check=True)
