/*
 * Small x86-64 threaded recompiler for flat 32-bit DOS/DPMI code.
 *
 * Each block is decoded once into bounded operations. The generated host code
 * calls the per-operation executor directly, keeping guest state and memory
 * checks explicit while removing instruction fetch/decode from hot blocks.
 * Control transfers end a block and DosJitRun chains the destination block.
 * Unsupported instructions and device-memory accesses side-exit to Cpu.c.
 */
#include <stddef.h>
#include "Internal.h"
#undef ip
#undef flags

#define JIT_MAX_OPS 128u
#define JIT_CODE_BYTES (JIT_MAX_OPS * 40u + 96u)
#define JIT_MEM_WRITE 1u
#define JIT_MEM_READ  0u

enum {
  JOP_NOP, JOP_MOV_IMM, JOP_MOV_RM, JOP_MOV_MOFFS, JOP_ADD,
  JOP_JCC, JOP_PUSH, JOP_POP, JOP_CALL_REL, JOP_RET,
  JOP_CALL_RM, JOP_JMP_RM, JOP_IMUL, JOP_IMUL_IMM, JOP_SHIFT,
  JOP_LEAVE, JOP_MOV_EXT, JOP_SETCC, JOP_STRING, JOP_MOV_IMM8
};

typedef struct _DOS_JIT_STORAGE {
  DOS_JIT_OP Ops[JIT_MAX_OPS];
  U8 CodeBytes[JIT_CODE_BYTES];
} DOS_JIT_STORAGE;

static U8 jit_parity(U8 value) {
  value ^= value >> 4;
  value &= 0x0fu;
  return (U8)((0x9669u >> value) & 1u);
}

static U8 jit_fetch(DOS_VM *vm, U32 ip, U32 base, U32 limit, int *ok) {
  if (!*ok || ip > limit || base > MEM_SIZE || ip >= MEM_SIZE - base) {
    *ok = 0;
    return 0;
  }
  return vm->Memory[base + ip];
}

static U32 jit_fetch32(DOS_VM *vm, U32 *ip, U32 base, U32 limit, int *ok) {
  U32 value = 0;
  for (unsigned i = 0; i < 4; ++i)
    value |= (U32)jit_fetch(vm, (*ip)++, base, limit, ok) << (i * 8u);
  return value;
}

static U32 jit_fetch16(DOS_VM *vm, U32 *ip, U32 base, U32 limit, int *ok) {
  U32 value = jit_fetch(vm, (*ip)++, base, limit, ok);
  value |= (U32)jit_fetch(vm, (*ip)++, base, limit, ok) << 8;
  return value;
}

static U32 jit_signature(DOS_VM *vm, U32 base, U32 ip, U32 length) {
  U32 hash = 2166136261u;
  if (!length || base > MEM_SIZE || ip > MEM_SIZE - base ||
      length > MEM_SIZE - base - ip)
    return 0;
  for (U32 i = 0; i < length; ++i)
    hash = (hash ^ vm->Memory[base + ip + i]) * 16777619u;
  return hash;
}

static U32 jit_cache_index(U32 ip, U16 selector) {
  return ((ip >> 4) ^ (ip >> 12) ^ selector) % DOS_JIT_CACHE_ENTRIES;
}

static void jit_release_entry(DOS_JIT_ENTRY *entry) {
  if (entry->Storage) ExFreePool(entry->Storage);
  memset(entry, 0, sizeof(*entry));
}

static int jit_is_vga(U32 address, U32 width) {
  if (address > MEM_SIZE || width > MEM_SIZE - address) return 1;
  return address < 0xc0000u && address + width > 0xa0000u;
}

/* Decode 32-bit ModRM/SIB addressing into an operation. */
static int jit_decode_rm(DOS_VM *vm, DOS_JIT_OP *op, U32 *cursor,
                         U32 base, U32 limit, int *ok) {
  U8 modrm = jit_fetch(vm, (*cursor)++, base, limit, ok);
  if (!*ok) return 0;
  op->ModRm = modrm;
  op->Reg = (modrm >> 3) & 7u;
  op->Reg2 = modrm & 7u;
  op->BaseReg = 0xffu;
  op->IndexReg = 0xffu;
  op->Scale = 1;
  U8 mod = modrm >> 6, rm = modrm & 7u;
  if (mod == 3) return 1;
  if (rm == 4) {
    U8 sib = jit_fetch(vm, (*cursor)++, base, limit, ok);
    if (!*ok) return 0;
    op->HasSib = 1;
    op->Scale = (U8)(1u << (sib >> 6));
    U8 index = (sib >> 3) & 7u;
    U8 breg = sib & 7u;
    if (index != 4) op->IndexReg = index;
    if (!(mod == 0 && breg == 5)) op->BaseReg = breg;
    if (mod == 0 && breg == 5)
      op->Displacement = jit_fetch32(vm, cursor, base, limit, ok);
  } else if (mod == 0 && rm == 5) {
    op->Displacement = jit_fetch32(vm, cursor, base, limit, ok);
  } else {
    op->BaseReg = rm;
  }
  if (mod == 1)
    op->Displacement += (U32)(INT32)(INT8)jit_fetch(vm, (*cursor)++, base, limit, ok);
  else if (mod == 2)
    op->Displacement += jit_fetch32(vm, cursor, base, limit, ok);
  return *ok;
}

static int jit_decode(DOS_VM *vm, U32 start, U32 base, U32 limit,
                      DOS_JIT_OP *op, U32 *length) {
  U32 cursor = start;
  int ok = 1, rep = 0;
  memset(op, 0, sizeof(*op));
  op->BaseReg = op->IndexReg = 0xffu;
  U8 byte = jit_fetch(vm, cursor++, base, limit, &ok);
  if (byte == 0xf3u) {
    rep = 1;
    byte = jit_fetch(vm, cursor++, base, limit, &ok);
  }
  op->Opcode = byte;
  if (!ok) return 0;
  if (rep && (byte == 0xa4u || byte == 0xa5u || byte == 0xaau || byte == 0xabu)) {
    op->Kind = JOP_STRING;
    op->Width = (byte == 0xa4u || byte == 0xaau) ? 1 : 4;
  } else if (rep) return 0;
  else if (byte == 0x90u) op->Kind = JOP_NOP;
  else if (byte >= 0xb8u && byte <= 0xbfu) {
    op->Kind = JOP_MOV_IMM; op->Reg = byte & 7u;
    op->Immediate = jit_fetch32(vm, &cursor, base, limit, &ok);
  } else if (byte >= 0x50u && byte <= 0x57u) {
    op->Kind = JOP_PUSH; op->Reg = byte & 7u;
  } else if (byte >= 0x58u && byte <= 0x5fu) {
    op->Kind = JOP_POP; op->Reg = byte & 7u;
  } else if (byte >= 0x70u && byte <= 0x7fu) {
    op->Kind = JOP_JCC; op->Condition = byte & 0x0fu;
    INT8 relative = (INT8)jit_fetch(vm, cursor++, base, limit, &ok);
    op->Target = cursor + (INT32)relative;
  } else if (byte == 0x01u || byte == 0x03u || byte == 0x8bu || byte == 0x89u ||
             byte == 0x8au || byte == 0x88u ||
             byte == 0x83u || byte == 0xffu || byte == 0x6bu || byte == 0xc6u) {
    if (!jit_decode_rm(vm, op, &cursor, base, limit, &ok)) return 0;
    if (byte == 0x01u || byte == 0x03u || byte == 0x83u) {
      if (byte == 0x83u && op->Reg != 0) return 0;
      op->Kind = JOP_ADD;
      op->Width = 4;
      if (byte == 0x83u)
        op->Immediate = (U32)(INT32)(INT8)jit_fetch(vm, cursor++, base, limit, &ok);
    } else if (byte == 0x8bu || byte == 0x89u || byte == 0x8au || byte == 0x88u) {
      op->Kind = JOP_MOV_RM;
      op->Width = (byte == 0x8au || byte == 0x88u) ? 1 : 4;
    } else if (byte == 0xffu) {
      if (op->Reg == 2) op->Kind = JOP_CALL_RM;
      else if (op->Reg == 4) op->Kind = JOP_JMP_RM;
      else if (op->Reg == 6) op->Kind = JOP_PUSH;
      else return 0;
      op->Width = 4;
    } else if (byte == 0x6bu) {
      op->Kind = JOP_IMUL_IMM; op->Width = 4;
      op->Immediate = (U32)(INT32)(INT8)jit_fetch(vm, cursor++, base, limit, &ok);
    } else {
      if (op->Reg != 0) return 0;
      op->Kind = JOP_MOV_IMM8; op->Width = 1;
      op->Immediate = jit_fetch(vm, cursor++, base, limit, &ok);
    }
  } else if (byte == 0xe8u) {
    op->Kind = JOP_CALL_REL;
    INT32 relative = (INT32)jit_fetch32(vm, &cursor, base, limit, &ok);
    op->Target = cursor + relative;
  } else if (byte == 0xc2u) {
    op->Kind = JOP_RET; op->Immediate = jit_fetch16(vm, &cursor, base, limit, &ok);
  } else if (byte == 0xc9u) op->Kind = JOP_LEAVE;
  else if (byte == 0xa0u || byte == 0xa1u || byte == 0xa2u || byte == 0xa3u) {
    op->Kind = JOP_MOV_MOFFS;
    op->Width = (byte == 0xa0u || byte == 0xa2u) ? 1 : 4;
    op->Displacement = jit_fetch32(vm, &cursor, base, limit, &ok);
  } else if (byte == 0x0fu) {
    U8 second = jit_fetch(vm, cursor++, base, limit, &ok);
    if (second == 0xafu || second == 0xb6u || second == 0xb7u ||
        second == 0xbeu || second == 0xbfu || second == 0x94u || second == 0x95u) {
      if (!jit_decode_rm(vm, op, &cursor, base, limit, &ok)) return 0;
      if (second == 0xafu) { op->Kind = JOP_IMUL; op->Width = 4; }
      else if (second == 0x94u || second == 0x95u) {
        op->Kind = JOP_SETCC; op->Condition = second == 0x94u ? 4 : 5;
        op->Width = 1;
      } else {
        op->Kind = JOP_MOV_EXT;
        op->Width = (second == 0xb6u || second == 0xbeu) ? 1 : 2;
        op->Condition = (second == 0xbeu || second == 0xbfu) ? 1 : 0;
      }
    } else return 0;
  } else if (byte == 0xc1u || byte == 0xd1u || byte == 0xd3u) {
    if (!jit_decode_rm(vm, op, &cursor, base, limit, &ok)) return 0;
    U8 group = op->Reg;
    if (group != 4 && group != 5 && group != 7) return 0;
    op->Kind = JOP_SHIFT; op->Width = 4;
    op->Condition = group;
    op->Immediate = byte == 0xc1u ? (U32)jit_fetch(vm, cursor++, base, limit, &ok) :
                    byte == 0xd1u ? 1u : 0u;
  } else return 0;
  if (!ok || cursor <= start || cursor - start > 15u || cursor - 1u > limit)
    return 0;
  op->NextIp = cursor;
  *length = cursor - start;
  return 1;
}

static U32 jit_reg32(DOS_VM *vm, U8 reg) { return vm->Registers32[reg & 7u]; }
static void jit_set_reg32(DOS_VM *vm, U8 reg, U32 value) {
  vm->Registers32[reg & 7u] = value;
  vm->Registers[reg & 7u] = (U16)value;
}

static U32 jit_reg8(DOS_VM *vm, U8 reg) {
  if (reg < 4) return vm->Registers32[reg] & 0xffu;
  return (vm->Registers32[reg - 4u] >> 8) & 0xffu;
}
static void jit_set_reg8(DOS_VM *vm, U8 reg, U8 value) {
  U8 index = reg < 4 ? reg : (U8)(reg - 4u);
  U32 shift = reg < 4 ? 0u : 8u;
  U32 mask = 0xffu << shift;
  jit_set_reg32(vm, index, (vm->Registers32[index] & ~mask) | ((U32)value << shift));
}

static int jit_linear(DOS_VM *vm, U8 segment, U32 offset, U32 width,
                      U32 *linear) {
  DOS_DESCRIPTOR *d = &vm->SegmentCache[segment];
  if (!d->Valid || offset > d->Limit || width - 1u > d->Limit - offset ||
      d->Base > MEM_SIZE || offset > MEM_SIZE - d->Base ||
      width > MEM_SIZE - d->Base - offset)
    return 0;
  U32 address = d->Base + offset;
  if (jit_is_vga(address, width)) return 0;
  *linear = address;
  return 1;
}

static U32 jit_effective_address(DOS_VM *vm, const DOS_JIT_OP *op) {
  U32 address = op->Displacement;
  if (op->BaseReg != 0xffu) address += jit_reg32(vm, op->BaseReg);
  if (op->IndexReg != 0xffu)
    address += jit_reg32(vm, op->IndexReg) * op->Scale;
  return address;
}

static int jit_read_mem(DOS_VM *vm, U8 segment, U32 offset, U32 width, U32 *value) {
  U32 address;
  if (!jit_linear(vm, segment, offset, width, &address)) return 0;
  *value = 0;
  for (U32 i = 0; i < width; ++i) *value |= (U32)vm->Memory[address + i] << (i * 8u);
  return 1;
}
static int jit_write_mem(DOS_VM *vm, U8 segment, U32 offset, U32 width, U32 value) {
  U32 address;
  if (!jit_linear(vm, segment, offset, width, &address)) return 0;
  for (U32 i = 0; i < width; ++i) vm->Memory[address + i] = (U8)(value >> (i * 8u));
  return 1;
}

static int jit_read_rm(DOS_VM *vm, const DOS_JIT_OP *op, U32 width, U32 *value) {
  U8 mod = op->ModRm >> 6;
  if (mod == 3) {
    U8 reg = op->Reg2;
    *value = width == 1 ? jit_reg8(vm, reg) : width == 2 ?
             vm->Registers[reg] : jit_reg32(vm, reg);
    return 1;
  }
  U8 segment = (op->BaseReg == SP || op->BaseReg == BP) ? SS : DS;
  return jit_read_mem(vm, segment, jit_effective_address(vm, op), width, value);
}
static int jit_write_rm(DOS_VM *vm, const DOS_JIT_OP *op, U32 width, U32 value) {
  U8 mod = op->ModRm >> 6;
  if (mod == 3) {
    U8 reg = op->Reg2;
    if (width == 1) jit_set_reg8(vm, reg, (U8)value);
    else if (width == 2) {
      vm->Registers[reg] = (U16)value;
      vm->Registers32[reg] = (vm->Registers32[reg] & 0xffff0000u) | (U16)value;
    } else jit_set_reg32(vm, reg, value);
    return 1;
  }
  U8 segment = (op->BaseReg == SP || op->BaseReg == BP) ? SS : DS;
  return jit_write_mem(vm, segment, jit_effective_address(vm, op), width, value);
}

static U32 jit_condition(U32 flag_value, U8 cc) {
  U32 cf = flag_value & 1u, pf = (flag_value >> 2) & 1u;
  U32 zf = (flag_value >> 6) & 1u, sf = (flag_value >> 7) & 1u;
  U32 of = (flag_value >> 11) & 1u;
  switch (cc & 15u) {
  case 0: return of; case 1: return !of;
  case 2: return cf; case 3: return !cf;
  case 4: return zf; case 5: return !zf;
  case 6: return cf || zf; case 7: return !cf && !zf;
  case 8: return sf; case 9: return !sf;
  case 10: return pf; case 11: return !pf;
  case 12: return sf != of; case 13: return sf == of;
  case 14: return zf || sf != of; default: return !zf && sf == of;
  }
}

#define JIT_ARITH_FLAGS 0x8d5u
static U32 jit_add_flags(U32 old_flags, U32 a, U32 b, U32 result) {
  U32 f = old_flags & ~JIT_ARITH_FLAGS;
  if ((U64)a + b > 0xffffffffULL) f |= 1u;
  if (((a ^ b ^ result) & 0x10u)) f |= 0x10u;
  if (!result) f |= 0x40u;
  if (result & 0x80000000u) f |= 0x80u;
  if (jit_parity((U8)result)) f |= 4u;
  if ((~(a ^ b) & (a ^ result)) & 0x80000000u) f |= 0x800u;
  return f;
}

static int jit_push(DOS_VM *vm, U32 value) {
  U32 sp = vm->Registers32[SP] - 4u;
  if (!jit_write_mem(vm, SS, sp, 4, value)) return 0;
  jit_set_reg32(vm, SP, sp);
  return 1;
}
static int jit_pop(DOS_VM *vm, U32 *value) {
  U32 sp = vm->Registers32[SP];
  if (!jit_read_mem(vm, SS, sp, 4, value)) return 0;
  jit_set_reg32(vm, SP, sp + 4u);
  return 1;
}

static int jit_string(DOS_VM *vm, const DOS_JIT_OP *op) {
  if (vm->Eflags & 0x400u) return 0; /* DF set: checked interpreter path */
  U32 count = vm->Registers32[CX];
  U32 take = count > 4096u ? 4096u : count;
  U32 si = vm->Registers32[SI], di = vm->Registers32[DI];
  if (op->Opcode == 0xa4u || op->Opcode == 0xa5u) {
    for (U32 i = 0; i < take; ++i) {
      U32 value;
      if (!jit_read_mem(vm, DS, si + i * op->Width, op->Width, &value) ||
          !jit_write_mem(vm, ES, di + i * op->Width, op->Width, value))
        return 0;
    }
    jit_set_reg32(vm, SI, si + take * op->Width);
    jit_set_reg32(vm, DI, di + take * op->Width);
  } else {
    U32 value = op->Width == 1 ? jit_reg8(vm, AX) : jit_reg32(vm, AX);
    for (U32 i = 0; i < take; ++i)
      if (!jit_write_mem(vm, ES, di + i * op->Width, op->Width, value)) return 0;
    jit_set_reg32(vm, DI, di + take * op->Width);
  }
  jit_set_reg32(vm, CX, count - take);
  if (count <= take) vm->Eip = op->NextIp;
  return 1;
}

static U32 jit_execute_op(DOS_VM *vm, const DOS_JIT_OP *op) {
  U32 a, b, result;
  vm->CurrentInstructionIp = vm->Eip;
  vm->CurrentOpcode = op->Opcode;
  switch (op->Kind) {
  case JOP_NOP: vm->Eip = op->NextIp; break;
  case JOP_MOV_IMM: jit_set_reg32(vm, op->Reg, op->Immediate); vm->Eip = op->NextIp; break;
  case JOP_MOV_RM:
    if (op->Opcode == 0x8bu || op->Opcode == 0x8au) {
      if (!jit_read_rm(vm, op, op->Width, &a)) return 0;
      if (op->Width == 1) jit_set_reg8(vm, op->Reg, (U8)a);
      else jit_set_reg32(vm, op->Reg, a);
    } else {
      a = op->Width == 1 ? jit_reg8(vm, op->Reg) : jit_reg32(vm, op->Reg);
      if (!jit_write_rm(vm, op, op->Width, a)) return 0;
    }
    vm->Eip = op->NextIp; break;
  case JOP_MOV_MOFFS:
    if (op->Opcode == 0xa0u || op->Opcode == 0xa1u) {
      if (!jit_read_mem(vm, DS, op->Displacement, op->Width, &a)) return 0;
      if (op->Width == 1) jit_set_reg8(vm, AX, (U8)a); else jit_set_reg32(vm, AX, a);
    } else {
      a = op->Width == 1 ? jit_reg8(vm, AX) : jit_reg32(vm, AX);
      if (!jit_write_mem(vm, DS, op->Displacement, op->Width, a)) return 0;
    }
    vm->Eip = op->NextIp; break;
  case JOP_ADD:
    if (op->Opcode == 0x83u) {
      if (!jit_read_rm(vm, op, 4, &a)) return 0;
      b = op->Immediate;
      result = a + b;
      vm->Eflags = jit_add_flags(vm->Eflags, a, b, result);
      if (!jit_write_rm(vm, op, 4, result)) return 0;
    } else if (op->Opcode == 0x01u) {
      if (!jit_read_rm(vm, op, 4, &a)) return 0;
      b = jit_reg32(vm, op->Reg); result = a + b;
      vm->Eflags = jit_add_flags(vm->Eflags, a, b, result);
      if (!jit_write_rm(vm, op, 4, result)) return 0;
    } else {
      if (!jit_read_rm(vm, op, 4, &b)) return 0;
      a = jit_reg32(vm, op->Reg); result = a + b;
      vm->Eflags = jit_add_flags(vm->Eflags, a, b, result);
      jit_set_reg32(vm, op->Reg, result);
    }
    vm->Flags = (U16)vm->Eflags; vm->Eip = op->NextIp; break;
  case JOP_JCC:
    vm->Eip = jit_condition(vm->Eflags, op->Condition) ? op->Target : op->NextIp;
    vm->JitLastExecuted++; return 0;
  case JOP_PUSH:
    if (op->Opcode == 0xffu) {
      if (!jit_read_rm(vm, op, 4, &a)) return 0;
    } else a = jit_reg32(vm, op->Reg);
    if (!jit_push(vm, a)) return 0;
    vm->Eip = op->NextIp; break;
  case JOP_POP:
    if (!jit_pop(vm, &a)) return 0;
    jit_set_reg32(vm, op->Reg, a); vm->Eip = op->NextIp; break;
  case JOP_CALL_REL:
    if (!jit_push(vm, op->NextIp)) return 0;
    vm->Eip = op->Target; vm->JitLastExecuted++; return 0;
  case JOP_RET:
    if (!jit_pop(vm, &a)) return 0;
    jit_set_reg32(vm, SP, vm->Registers32[SP] + op->Immediate);
    vm->Eip = a; vm->JitLastExecuted++; return 0;
  case JOP_CALL_RM: case JOP_JMP_RM:
    if (!jit_read_rm(vm, op, 4, &a)) return 0;
    if (op->Kind == JOP_CALL_RM && !jit_push(vm, op->NextIp)) return 0;
    vm->Eip = a; vm->JitLastExecuted++; return 0;
  case JOP_IMUL:
    if (!jit_read_rm(vm, op, 4, &a)) return 0;
    result = (U32)((INT64)(INT32)jit_reg32(vm, op->Reg) * (INT32)a);
    jit_set_reg32(vm, op->Reg, result); vm->Eip = op->NextIp; break;
  case JOP_IMUL_IMM:
    if (!jit_read_rm(vm, op, 4, &a)) return 0;
    result = (U32)((INT64)(INT32)a * (INT32)op->Immediate);
    jit_set_reg32(vm, op->Reg, result); vm->Eip = op->NextIp; break;
  case JOP_SHIFT: {
    if (!jit_read_rm(vm, op, 4, &a)) return 0;
    U32 count = op->Immediate ? op->Immediate : (vm->Registers32[CX] & 0xffu);
    count &= 0x1fu;
    if (count) {
      if (op->Condition == 4) result = a << count;
      else if (op->Condition == 5) result = a >> count;
      else result = (U32)((INT32)a >> count);
      vm->Eflags = (vm->Eflags & ~0x8c5u) | (result == 0 ? 0x40u : 0) |
                   (result & 0x80000000u ? 0x80u : 0) |
                   (jit_parity((U8)result) ? 4u : 0);
      if (op->Condition == 4 ? ((a >> (32u - count)) & 1u) :
          ((a >> (count - 1u)) & 1u)) vm->Eflags |= 1u;
      vm->Flags = (U16)vm->Eflags;
      if (!jit_write_rm(vm, op, 4, result)) return 0;
    }
    vm->Eip = op->NextIp; break;
  }
  case JOP_LEAVE:
    jit_set_reg32(vm, SP, jit_reg32(vm, BP));
    if (!jit_pop(vm, &a)) return 0;
    jit_set_reg32(vm, BP, a); vm->Eip = op->NextIp; break;
  case JOP_MOV_EXT:
    if (!jit_read_rm(vm, op, op->Width, &a)) return 0;
    if (op->Condition && (a & (op->Width == 1 ? 0x80u : 0x8000u)))
      a |= op->Width == 1 ? 0xffffff00u : 0xffff0000u;
    jit_set_reg32(vm, op->Reg, a); vm->Eip = op->NextIp; break;
  case JOP_SETCC:
    if (!jit_write_rm(vm, op, 1, jit_condition(vm->Eflags, op->Condition))) return 0;
    vm->Eip = op->NextIp; break;
  case JOP_STRING:
    if (!jit_string(vm, op)) return 0;
    break;
  case JOP_MOV_IMM8:
    if (!jit_write_rm(vm, op, 1, op->Immediate)) return 0;
    vm->Eip = op->NextIp; break;
  default: return 0;
  }
  vm->Flags = (U16)vm->Eflags;
  vm->JitLastExecuted++;
  return 1;
}

static U32 jit_execute_block(DOS_VM *vm, const DOS_JIT_OP *ops, U32 count) {
  vm->JitLastExecuted = 0;
  for (U32 i = 0; i < count; ++i)
    if (!jit_execute_op(vm, &ops[i])) break;
  return vm->JitLastExecuted;
}

static void jit_emit_u32(U8 **p, U32 value) {
  for (unsigned i = 0; i < 4; ++i) *(*p)++ = (U8)(value >> (i * 8u));
}
static void jit_emit_u64(U8 **p, U64 value) {
  for (unsigned i = 0; i < 8; ++i) *(*p)++ = (U8)(value >> (i * 8u));
}

static U8 *jit_emit_block(DOS_JIT_STORAGE *storage, U32 count) {
  U8 *code = storage->CodeBytes, *p = code;
  /* A per-instruction call stub made code execution dominated by host call
   * overhead. Emit one native entry point per decoded block and keep the
   * operation dispatch inside a straight C loop instead. */
  *p++ = 0x48; *p++ = 0xbe; /* movabs rsi,&ops */
  jit_emit_u64(&p, (U64)(ULONG_PTR)storage->Ops);
  *p++ = 0xba; /* mov edx,count */
  jit_emit_u32(&p, count);
  *p++ = 0x48; *p++ = 0xb8; /* movabs rax,&block executor */
  jit_emit_u64(&p, (U64)(ULONG_PTR)jit_execute_block);
  *p++ = 0xff; *p++ = 0xe0; /* jmp rax (tail-call, preserve ABI alignment) */
  return code;
}

static DOS_JIT_ENTRY *jit_compile(DOS_VM *vm, U32 start, U32 budget,
                                  int hot_only) {
  DOS_JIT_CACHE *cache = vm->JitCache;
  U16 selector = vm->Segments[CS];
  U32 slot = jit_cache_index(start, selector);
  DOS_JIT_ENTRY *entry = &cache->Entries[slot];
  DOS_DESCRIPTOR *cs = &vm->SegmentCache[CS];
  int warmed = 0;
  if (!cs->Valid || cs->Base != 0 || !(cs->Flags & 4u) ||
      start > cs->Limit || start >= MEM_SIZE)
    return NULL;
  U32 signature = 0;
  if (entry->Selector == selector && entry->StartIp == start &&
      entry->CodeLimit == cs->Limit && entry->CodeBase == cs->Base &&
      entry->SourceLength) {
    signature = jit_signature(vm, cs->Base, start, entry->SourceLength);
    if (signature == entry->Signature) {
      if (entry->Warmup) {
        if (entry->WarmCount < 8u) entry->WarmCount++;
        if (entry->WarmCount < 8u) return entry;
        warmed = 1;
      } else if (!(entry->BudgetLimited && budget > entry->InstructionCount)) {
        return entry;
      }
    }
    jit_release_entry(entry);
  } else if (entry->Storage || entry->NoJit) jit_release_entry(entry);

  if (hot_only && !warmed) {
    entry->Selector = selector;
    entry->StartIp = start;
    entry->CodeBase = cs->Base;
    entry->CodeLimit = cs->Limit;
    entry->SourceLength = 1;
    entry->Signature = jit_signature(vm, cs->Base, start, 1);
    entry->Warmup = TRUE;
    entry->WarmCount = 1;
    return entry;
  }

  if (!vm->JitCache) return NULL;
  DOS_JIT_STORAGE *storage = ExAllocatePoolWithTag(NonPagedPool,
                                                   sizeof(*storage), 'tiJD');
  if (!storage) return NULL;
  U32 count = 0, cursor = start, source_length = 0;
  U32 cap = budget < JIT_MAX_OPS ? budget : JIT_MAX_OPS;
  while (count < cap) {
    U32 length = 0;
    DOS_JIT_OP op;
    if (!jit_decode(vm, cursor, cs->Base, cs->Limit, &op, &length)) break;
    /* Static absolute operands into VGA are always interpreted through Video.c. */
    if ((op.Kind == JOP_MOV_MOFFS ||
         ((op.Kind == JOP_MOV_RM || op.Kind == JOP_ADD || op.Kind == JOP_MOV_IMM8 ||
           op.Kind == JOP_MOV_EXT || op.Kind == JOP_SETCC || op.Kind == JOP_SHIFT ||
           op.Kind == JOP_IMUL || op.Kind == JOP_IMUL_IMM || op.Kind == JOP_CALL_RM ||
           op.Kind == JOP_JMP_RM || (op.Kind == JOP_PUSH && op.Opcode == 0xffu)) &&
          (op.ModRm >> 6) != 3 && op.BaseReg == 0xffu && op.IndexReg == 0xffu)) &&
        jit_is_vga(op.Displacement, op.Width ? op.Width : 4u))
      break;
    storage->Ops[count++] = op;
    cursor = op.NextIp;
    source_length = cursor - start;
    /* Keep basic blocks short; loop headers and branch targets are re-looked up. */
    if (op.Kind == JOP_JCC || op.Kind == JOP_CALL_REL || op.Kind == JOP_RET ||
        op.Kind == JOP_CALL_RM || op.Kind == JOP_JMP_RM) break;
  }
  if (!count) {
    ExFreePool(storage);
    if (cursor == start) {
      /* Cache one-byte unsupported instructions to avoid repeated decoding. */
      source_length = start <= cs->Limit ? 1u : 0u;
      if (source_length) {
        entry->NoJit = TRUE;
        entry->Selector = selector;
        entry->StartIp = start;
        entry->CodeBase = cs->Base;
        entry->CodeLimit = cs->Limit;
        entry->SourceLength = source_length;
        entry->Signature = jit_signature(vm, cs->Base, start, source_length);
      }
    }
    return NULL;
  }
  jit_release_entry(entry);
  entry->Storage = storage;
  entry->Code = jit_emit_block(storage, count);
  entry->Selector = selector;
  entry->StartIp = start;
  entry->CodeBase = cs->Base;
  entry->CodeLimit = cs->Limit;
  entry->SourceLength = source_length;
  entry->InstructionCount = count;
  entry->Signature = jit_signature(vm, cs->Base, start, source_length);
  entry->BudgetLimited = count == cap && cap < JIT_MAX_OPS;
  return entry;
}

static U32 jit_run(DOS_VM *vm, U32 instruction_budget, int hot_only) {
  if (!vm || !instruction_budget || !vm->ProtectedMode || !vm->DpmiClient32)
    return 0;
  if (!vm->JitCache) {
    vm->JitCache = ExAllocatePoolWithTag(NonPagedPool, sizeof(*vm->JitCache), 'caJD');
    if (!vm->JitCache) return 0;
    memset(vm->JitCache, 0, sizeof(*vm->JitCache));
  }
  U32 total = 0;
  while (total < instruction_budget) {
    DOS_JIT_ENTRY *entry = jit_compile(vm, vm->Eip,
                                       instruction_budget - total, hot_only);
    if (!entry || entry->NoJit || !entry->Code) break;
    U32 (*code)(DOS_VM *) = (U32 (*)(DOS_VM *))(ULONG_PTR)entry->Code;
    U32 ran = code(vm);
    if (!ran) break;
    total += ran;
  }
  return total;
}

U32 DosJitRun(DOS_VM *vm, U32 instruction_budget) {
  return jit_run(vm, instruction_budget, 0);
}

U32 DosJitRunHot(DOS_VM *vm, U32 instruction_budget) {
  return jit_run(vm, instruction_budget, 1);
}

void DosJitDestroy(DOS_VM *vm) {
  if (!vm || !vm->JitCache) return;
  for (U32 i = 0; i < DOS_JIT_CACHE_ENTRIES; ++i)
    jit_release_entry(&vm->JitCache->Entries[i]);
  ExFreePool(vm->JitCache);
  vm->JitCache = NULL;
}
