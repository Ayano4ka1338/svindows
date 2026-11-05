#include "Internal.h"

static U8 parity(U8 v);
static int condition(unsigned cc);
static int condition_flags(U32 tested_flags, unsigned cc);
static U32 pm_register(DOS_VM *vm, unsigned index, unsigned width);
static void pm_set_register(DOS_VM *vm, unsigned index, unsigned width, U32 value);
static U16 dpmi_make_descriptor(U32 base, U32 limit, U8 access, U8 desc_flags);
static U16 dpmi_copy_descriptor(U16 function);
static int dpmi_simulate_real_interrupt(U8 vector, U32 structure,
                                        int force_builtin_dos);
static __attribute__((always_inline)) inline int protected_linear(
                            DOS_VM *vm, unsigned index, U32 offset, U32 size,
                            int write, int execute, U32 *linear);
static int pm_write(DOS_VM *vm, unsigned s, U32 offset, unsigned width, U32 value);
static U32 pm_read(DOS_VM *vm, unsigned s, U32 offset, unsigned width);
static U32 pm_stack_pointer(DOS_VM *vm);

static U8 fetch8(void) {
  U8 v = rb(seg[CS], ip);
  ip++;
  return v;
}
static U16 fetch16(void) {
  U16 v = fetch8();
  return v | ((U16)fetch8() << 8);
}
static U32 fetch32(void) {
  U32 v = fetch16();
  return v | ((U32)fetch16() << 16);
}
static U32 read32(U16 s, U32 o) {
  return (U32)rw(s, o) | ((U32)rw(s, o + 2u) << 16);
}
static void write32(U16 s, U32 o, U32 v) {
  ww(s, o, (U16)v);
  ww(s, o + 2u, (U16)(v >> 16));
}
static int read_descriptor(U16 selector, DOS_DESCRIPTOR *out) {
  U32 index = selector & 0xfff8u;
  if (selector & 4u) {
    U32 slot = index >> 3;
    if (slot < 128 && current()->DpmiDescriptorUsed[slot]) {
      *out = current()->DpmiDescriptors[slot];
      return out->Valid;
    }
    if (!current()->Ldtr || !current()->LdtCache.Valid)
      return 0;
    if (index > current()->LdtCache.Limit ||
        current()->LdtCache.Limit - index < 7)
      return 0;
    if (current()->LdtCache.Base > 0xffffffffu - index)
      return 0;
    U32 at = current()->LdtCache.Base + index;
    if (at > MEM_SIZE - 8)
      return 0;
    const U8 *p = mem + at;
    U32 limit = (U32)p[0] | ((U32)p[1] << 8) | ((U32)p[6] & 0x0fu) << 16;
    out->Base = (U32)p[2] | ((U32)p[3] << 8) | ((U32)p[4] << 16) |
                ((U32)p[7] << 24);
    out->Flags = p[6] >> 4;
    out->Access = p[5];
    if (out->Flags & 8u)
      limit = (limit << 12) | 0xfffu;
    out->Limit = limit;
    out->Valid = !!(out->Access & 0x80u);
    return out->Valid;
  }
  DOS_TABLE_REGISTER *table = &current()->Gdtr;
  if (!table || index > table->Limit || table->Limit - index < 7)
    return 0;
  if (table->Base > 0xffffffffu - index)
    return 0;
  U32 at = table->Base + index;
  if (at > MEM_SIZE - 8)
    return 0;
  const U8 *p = mem + at;
  U32 limit = (U32)p[0] | ((U32)p[1] << 8) | ((U32)p[6] & 0x0fu) << 16;
  out->Base = (U32)p[2] | ((U32)p[3] << 8) | ((U32)p[4] << 16) |
              ((U32)p[7] << 24);
  out->Flags = p[6] >> 4;
  out->Access = p[5];
  if (out->Flags & 8u)
    limit = (limit << 12) | 0xfffu;
  out->Limit = limit;
  out->Valid = !!(out->Access & 0x80u);
  return out->Valid;
}
static void dpmi_result(U16 error) {
  DOS_VM *vm = current();
  if (error)
    KeLog("[dos] INT 31h AX=%04x failed error=%04x BX=%08x CX=%08x DX=%08x SI=%08x DI=%08x\n",
          (U16)r32[AX], error, r32[BX], r32[CX], r32[DX], r32[SI], r32[DI]);
  vm->Eflags = (vm->Eflags & ~CF) | (error ? CF : 0);
  vm->Flags = (U16)vm->Eflags;
  if (error)
    pm_set_register(vm, AX, 16, error);
}
static int dpmi_descriptor(U16 selector, DOS_DESCRIPTOR **out, U32 *slot_out) {
  U32 slot = (selector & 0xfff8u) >> 3;
  if (!(selector & 4u) || slot >= 128 ||
      !current()->DpmiDescriptorUsed[slot])
    return 0;
  *out = &current()->DpmiDescriptors[slot];
  if (slot_out) *slot_out = slot;
  return 1;
}
static void dpmi_interrupt(void) {
  DOS_VM *vm = current();
  U16 function = (U16)r32[AX], error = 0;
  U32 value;
  if (function == 0x0000) {
    U16 count = (U16)r32[CX];
    U32 first;
    if (!count || count > 128) { dpmi_result(0x8021); return; }
    for (first = 0; first + count <= 128; first++) {
      U32 i;
      for (i = 0; i < count && !vm->DpmiDescriptorUsed[first + i]; i++);
      if (i == count) break;
      first += i;
    }
    if (first + count > 128) { dpmi_result(0x8011); return; }
    for (U32 i = 0; i < count; i++) {
      vm->DpmiDescriptorUsed[first + i] = TRUE;
      vm->DpmiDescriptors[first + i] =
          (DOS_DESCRIPTOR){0, 0, 0xf2u, 0, TRUE};
    }
    pm_set_register(vm, AX, 16, (U16)((first << 3) | 7u));
  } else if (function == 0x0001) {
    U16 selector = (U16)r32[BX];
    DOS_DESCRIPTOR *d;
    U32 slot;
    if (!dpmi_descriptor(selector, &d, &slot)) error = 0x8022;
    else if (vm->DpmiDosDescriptor[slot]) error = 0x8022;
    else { vm->DpmiDescriptorUsed[slot] = FALSE; memset(d, 0, sizeof(*d)); }
  } else if (function == 0x0003) {
    pm_set_register(vm, AX, 16, 8);
  } else if (function >= 0x0006 && function <= 0x0009) {
    DOS_DESCRIPTOR *d = NULL;
    if (!dpmi_descriptor((U16)r32[BX], &d, NULL)) error = 0x8022;
    else if (function == 0x0006) {
      pm_set_register(vm, DX, 16, (U16)d->Base);
      pm_set_register(vm, CX, 16, (U16)(d->Base >> 16));
    } else if (function == 0x0007) {
      value = ((U32)(U16)r32[CX] << 16) | (U16)r32[DX];
      d->Base = value;
    } else if (function == 0x0008) {
      value = ((U32)(U16)r32[CX] << 16) | (U16)r32[DX];
      if (value <= 0xfffffu) { d->Limit = value; d->Flags &= ~8u; }
      else { d->Limit = value | 0xfffu; d->Flags |= 8u; }
    } else {
      value = (U16)r32[CX];
      d->Access = (U8)value;
      d->Flags = (U8)((d->Flags & 0xf0u) | ((value >> 12) & 0x0fu));
      d->Valid = !!(d->Access & 0x80u);
    }
    /* DPMI 1.0 reloads cached descriptors after base/limit/rights updates.
     * CX[15:12] contains G/D/B/AVL; CX[11:8] is reserved. */
    if (!error && d)
      for (U32 i = 0; i < 6; i++)
        if ((seg[i] & 0xfffcu) == ((U16)r32[BX] & 0xfffcu))
          vm->SegmentCache[i] = *d;
  } else if (function == 0x000a) {
    DOS_DESCRIPTOR *source;
    U32 slot;
    if (!dpmi_descriptor((U16)r32[BX], &source, NULL)) error = 0x8022;
    else {
      for (slot = 0; slot < 128 && vm->DpmiDescriptorUsed[slot]; slot++);
      if (slot == 128) error = 0x8011;
      else { vm->DpmiDescriptorUsed[slot] = TRUE; vm->DpmiDescriptors[slot] = *source;
        /* INT 31h/000Ah returns a writable data alias, including when
         * the source is executable (DOS/4G patches relocated code). */
        vm->DpmiDescriptors[slot].Access = (source->Access & 0xf0u) | 2u;
        pm_set_register(vm, AX, 16, (U16)((slot << 3) | 7u)); }
    }
  } else if (function == 0x000b || function == 0x000c) {
    error = dpmi_copy_descriptor(function);
  } else if (function == 0x0200 || function == 0x0201) {
    U8 vector = (U8)r32[BX];
    if (function == 0x0200) {
      pm_set_register(vm, CX, 16, rw(0, (U16)(vector * 4u + 2u)));
      pm_set_register(vm, DX, 16, rw(0, (U16)(vector * 4u)));
    } else {
      ww(0, (U16)(vector * 4u), (U16)r32[DX]);
      ww(0, (U16)(vector * 4u + 2u), (U16)r32[CX]);
    }
  } else if (function == 0x0204 || function == 0x0205) {
    U8 vector = (U8)r32[BX];
    if (function == 0x0204) {
      U16 selector = vm->DpmiPmVectorSelector[vector];
      pm_set_register(vm, CX, 16, selector ? selector : vm->DpmiHostSelector);
      pm_set_register(vm, DX, vm->DpmiClient32 ? 32 : 16,
                      selector ? vm->DpmiPmVectorOffset[vector]
                               : 0x0200u + (U32)vector * 2u);
    } else {
      U16 selector = (U16)r32[CX];
      DOS_DESCRIPTOR descriptor;
      /* DPMI permits clearing a protected-mode vector by setting CX:EDX
       * to zero. Extenders use this when restoring the host's vector. */
      if (!selector && !r32[DX]) {
        vm->DpmiPmVectorSelector[vector] = 0;
        vm->DpmiPmVectorOffset[vector] = 0;
      } else if (!read_descriptor(selector, &descriptor) ||
                 !(descriptor.Access & 8u)) error = 0x8022;
      else {
        vm->DpmiPmVectorSelector[vector] = selector;
        vm->DpmiPmVectorOffset[vector] = vm->DpmiClient32 ? r32[DX] : (U16)r32[DX];
      }
    }
  } else if (function == 0x0202 || function == 0x0203) {
    U8 vector = (U8)r32[BX];
    if (vector >= 32) error = 0x8021;
    else if (function == 0x0202) {
      U16 selector = vm->DpmiPmExceptionSelector[vector];
      pm_set_register(vm, CX, 16, selector ? selector : vm->DpmiHostSelector);
      pm_set_register(vm, DX, vm->DpmiClient32 ? 32 : 16,
                      selector ? vm->DpmiPmExceptionOffset[vector]
                               : 0x0400u + (U32)vector * 2u);
    } else {
      U16 selector = (U16)r32[CX];
      DOS_DESCRIPTOR descriptor;
      if (!read_descriptor(selector, &descriptor) ||
          !(descriptor.Access & 8u)) error = 0x8022;
      else {
        vm->DpmiPmExceptionSelector[vector] = selector;
        vm->DpmiPmExceptionOffset[vector] = vm->DpmiClient32 ? r32[DX] : (U16)r32[DX];
      }
    }
  } else if (function == 0x0400) {
    pm_set_register(vm, AX, 16, 0x0100); pm_set_register(vm, BX, 16, 0x0003);
    pm_set_register(vm, CX, 8, 3);
    pm_set_register(vm, DX, 16, 0x0870);
  } else if (function == 0x0500) {
    U32 structure = vm->DpmiClient32 ? r32[DI] : (U16)r32[DI];
    U32 available = MEM_SIZE - 0x00100000u;
    if (vm->XmsUsed >= available) available = 0;
    else available -= vm->XmsUsed;
    for (U32 i = 0; i < DOS_DPMI_MAX_BLOCKS; i++) if (vm->DpmiBlocks[i].Used) {
      if (vm->DpmiBlocks[i].Size >= available) available = 0;
      else available -= vm->DpmiBlocks[i].Size;
    }
    U32 pages = available / 4096u;
    U32 fields[9] = {available, pages, pages, MEM_SIZE / 4096u,
                     MEM_SIZE / 4096u, pages, MEM_SIZE / 4096u,
                     pages, 0xffffffffu};
    U32 linear;
    if (!protected_linear(vm, ES, structure, 48, 1, 0, &linear)) error = 0x8022;
    else {
      for (U32 i = 0; i < 9; i++)
        if (!pm_write(vm, ES, structure + i * 4, 32, fields[i])) { error = 0x8022; break; }
      for (U32 i = 0; !error && i < 3; i++)
        if (!pm_write(vm, ES, structure + 36 + i * 4, 32, 0xffffffffu)) error = 0x8022;
    }
  } else if (function == 0x0501) {
    U32 requested = ((U32)(U16)r32[BX] << 16) | (U16)r32[CX];
    U32 size, base = 0x00100000u;
    U32 slot;
    if (!requested || requested > 0xffffffffu - (4096u - 1u)) {
      dpmi_result(0x8021); return;
    }
    /* DPMI linear blocks are page-granular. Reserving complete pages prevents
     * two adjacent allocations from aliasing the same emulated page. */
    size = (requested + 4096u - 1u) & ~(4096u - 1u);
    for (slot = 0; slot < DOS_DPMI_MAX_BLOCKS && vm->DpmiBlocks[slot].Used; slot++);
    if (slot == DOS_DPMI_MAX_BLOCKS) { dpmi_result(0x8016); return; }
    /* First-fit in the flat client address space, with page-aligned bases. */
    for (;;) {
      U32 i, next = base;
      BOOLEAN collision = FALSE;
      if (base >= MEM_SIZE || size > MEM_SIZE - base) break;
      for (i = 0; i < DOS_DPMI_MAX_BLOCKS; i++) if (vm->DpmiBlocks[i].Used) {
        U32 b = vm->DpmiBlocks[i].Base, z = vm->DpmiBlocks[i].Size;
        if (base < b + z && b < base + size) {
          if (b > 0xffffffffu - z - (4096u - 1u)) {
            base = MEM_SIZE;
            collision = TRUE;
            break;
          }
          next = (b + z + 4096u - 1u) & ~(4096u - 1u);
          collision = TRUE;
          break;
        }
      }
      for (i = 0; !collision && i < DOS_XMS_MAX_HANDLES; i++)
        if (vm->XmsBlocks[i].Used) {
          U32 b = vm->XmsBlocks[i].Base, z = vm->XmsBlocks[i].Size;
          if (base < b + z && b < base + size) {
            next = b + z;
            collision = TRUE;
            break;
          }
        }
      if (!collision) break;
      if (next <= base) break;
      base = next;
    }
    if (base >= MEM_SIZE || size > MEM_SIZE - base) { dpmi_result(0x8012); return; }
    vm->DpmiBlocks[slot].Base = base;
    vm->DpmiBlocks[slot].Size = size;
    vm->DpmiBlocks[slot].Used = TRUE;
    pm_set_register(vm, BX, 16, (U16)(base >> 16));
    pm_set_register(vm, CX, 16, (U16)base);
    pm_set_register(vm, SI, 16, 0);
    pm_set_register(vm, DI, 16, (U16)(slot + 1));
  } else if (function == 0x0502) {
    U32 handle = ((U32)(U16)r32[SI] << 16) | (U16)r32[DI];
    if (!handle || handle > DOS_DPMI_MAX_BLOCKS || !vm->DpmiBlocks[handle - 1].Used)
      error = 0x8023;
    else memset(&vm->DpmiBlocks[handle - 1], 0, sizeof(DOS_DPMI_BLOCK));
  } else if (function == 0x0100) {
    U16 paragraphs = (U16)r32[BX];
    U16 segment = dos_alloc(paragraphs), selector = 0;
    if (!paragraphs || !segment) { dpmi_result(0x0008); return; }
    U32 block_slot;
    for (block_slot = 0; block_slot < DOS_MAX_BLOCKS &&
         (!vm->Blocks[block_slot].Used || vm->Blocks[block_slot].Segment != segment);
         block_slot++);
    if (block_slot == DOS_MAX_BLOCKS) { dpmi_result(0x0008); return; }
    selector = dpmi_make_descriptor((U32)segment << 4,
                                    (U32)paragraphs * 16u - 1u, 0xf2u, 0);
    if (!selector) {
      vm->Blocks[block_slot].Used = FALSE;
      dpmi_result(0x8011);
      return;
    }
    U32 desc_slot = (selector & 0xfff8u) >> 3;
    vm->DpmiDosDescriptor[desc_slot] = TRUE;
    pm_set_register(vm, AX, 16, segment);
    pm_set_register(vm, DX, 16, selector);
  } else if (function == 0x0101) {
    U16 selector = (U16)r32[DX];
    U32 slot = (selector & 0xfff8u) >> 3;
    if (!(selector & 4u) || slot >= 128 || !vm->DpmiDescriptorUsed[slot] ||
        !vm->DpmiDosDescriptor[slot]) error = 0x8022;
    else {
      U16 segment = (U16)(vm->DpmiDescriptors[slot].Base >> 4);
      for (U32 i = 0; i < DOS_MAX_BLOCKS; i++)
        if (vm->Blocks[i].Used && vm->Blocks[i].Segment == segment)
          vm->Blocks[i].Used = FALSE;
      vm->DpmiDosDescriptor[slot] = FALSE;
      vm->DpmiDescriptorUsed[slot] = FALSE;
      memset(&vm->DpmiDescriptors[slot], 0, sizeof(DOS_DESCRIPTOR));
      for (U32 i = 0; i < 6; i++) if (vm->Segments[i] == selector) {
        vm->Segments[i] = 0;
        memset(&vm->SegmentCache[i], 0, sizeof(DOS_DESCRIPTOR));
      }
    }
  } else if (function == 0x0300) {
    U8 vector = (U8)r32[BX];
    U32 structure = vm->DpmiClient32 ? r32[DI] : (U16)r32[DI];
    if ((U8)(r32[BX] >> 8) || (U16)r32[CX] ||
        (vector != 0x10 && vector != 0x16 && vector != 0x20 &&
         vector != 0x21 && vector != 0x29)) error = 0x8021;
    else if (!dpmi_simulate_real_interrupt(vector, structure, 0)) error = 0x8022;
  } else if (function == 0x0303) {
    U32 slot;
    DOS_DESCRIPTOR code, data;
    U16 callback_selector = seg[DS], registers_selector = seg[ES];
    U32 callback_offset = vm->DpmiClient32 ? r32[SI] : (U16)r32[SI];
    U32 registers_offset = vm->DpmiClient32 ? r32[DI] : (U16)r32[DI];
    for (slot = 0; slot < 16 && vm->DpmiCallbacks[slot].Used; slot++);
    if (slot == 16) error = 0x8015;
    else if (!read_descriptor(callback_selector, &code) ||
             !(code.Access & 8u) ||
             !read_descriptor(registers_selector, &data) ||
             (data.Access & 8u) ||
             !protected_linear(vm, ES, registers_offset, 0x32u, 1, 0, &value))
      error = 0x8022;
    else {
      vm->DpmiCallbacks[slot].Used = TRUE;
      vm->DpmiCallbacks[slot].Selector = callback_selector;
      vm->DpmiCallbacks[slot].Offset = callback_offset;
      vm->DpmiCallbacks[slot].RegistersSelector = registers_selector;
      vm->DpmiCallbacks[slot].RegistersOffset = registers_offset;
      pm_set_register(vm, CX, 16, 0xf000u);
      pm_set_register(vm, DX, 16, (U16)(0x0600u + slot * 4u));
    }
  } else if (function == 0x0304) {
    U16 callback_segment = (U16)r32[CX], callback_offset = (U16)r32[DX];
    U32 slot = callback_offset >= 0x0600u ?
               (callback_offset - 0x0600u) / 4u : 16u;
    if (callback_segment != 0xf000u || slot >= 16 ||
        callback_offset != 0x0600u + slot * 4u ||
        !vm->DpmiCallbacks[slot].Used) error = 0x8024;
    else vm->DpmiCallbacks[slot].Used = FALSE;
  } else if (function == 0x0301 || function == 0x0302) {
    U32 structure = vm->DpmiClient32 ? r32[DI] : (U16)r32[DI];
    U32 linear;
    if (!protected_linear(vm, ES, structure, 0x32u, 0, 0, &linear)) error = 0x8022;
    else {
      U16 target_cs = (U16)pm_read(vm, ES, structure + 0x2cu, 16);
      U16 target_ip = (U16)pm_read(vm, ES, structure + 0x2au, 16);
      if (target_cs == DOS_BUILTIN_SEGMENT && target_ip >= DOS_BUILTIN_OFFSET(0) &&
          target_ip <= DOS_BUILTIN_OFFSET(255) && !(target_ip & 3u)) {
        U8 vector = (U8)((target_ip - DOS_BUILTIN_OFFSET(0)) / 4u);
        if (!dpmi_simulate_real_interrupt(vector, structure, 1)) error = 0x8022;
      } else error = 0x8001;
    }
  } else if (function == 0x0305 || function == 0x0306) {
    pm_set_register(vm, BX, 16, 0xf000u);
    pm_set_register(vm, CX, 16, function == 0x0305 ? 0x0108u : 0x010au);
    pm_set_register(vm, SI, 16, vm->DpmiHostSelector);
    pm_set_register(vm, DI, vm->DpmiClient32 ? 32 : 16,
                    function == 0x0305 ? 0x0108u : 0x010cu);
    if (function == 0x0305) pm_set_register(vm, AX, 16, 0);
  } else if (function == 0x0604) {
    pm_set_register(vm, AX, 16, 4096);
  } else if (function == 0x0600 || function == 0x0601 ||
             function == 0x0702) {
    /* DOSVM keeps every emulated page resident. Lock/unlock and the paging
     * candidate hint therefore succeed without changing memory state. */
  } else {
    error = 0x8001;
  }
  dpmi_result(error);
}
static U16 dpmi_make_descriptor(U32 base, U32 limit, U8 access, U8 desc_flags) {
  DOS_VM *vm = current();
  U32 i;
  for (i = 0; i < 128 && vm->DpmiDescriptorUsed[i]; i++);
  if (i == 128) return 0;
  vm->DpmiDescriptorUsed[i] = TRUE;
  vm->DpmiDescriptors[i] = (DOS_DESCRIPTOR){base, limit, access, desc_flags, TRUE};
  return (U16)((i << 3) | 7u);
}
static void dpmi_initial_switch(void) {
  DOS_VM *vm = current();
  U16 sp = r[SP], return_ip, return_cs;
  U16 selectors[6] = {0, 0, 0, 0, 0, 0};
  U16 environment = rw(PSP, 0x2cu);
  int client32 = !!(r[AX] & 1u);
  KeLog("[dos] DPMI switch request AX=%04x BX=%04x CX=%04x SS:SP=%04x:%04x\n",
        r[AX], r[BX], r[CX], seg[SS], sp);
  /* BX is reserved by the initial switch call.  The host's 32-bit capability
   * is reported in BX by INT 2Fh/1687, but clients (including DOS/16M) pass
   * BX=0 here when requesting a 32-bit entry through AX=1. */
  if ((r[AX] & ~1u) ||
      (U32)seg[SS] * 16u + sp + 4u > DOS_REAL_MEMORY_SIZE) {
    return_ip = rw(seg[SS], sp);
    return_cs = rw(seg[SS], (U16)(sp + 2));
    r[SP] = (U16)(sp + 4);
    seg[CS] = return_cs;
    ip = return_ip;
    flags |= CF;
    r[AX] = client32 ? 0x8021 : 0x8021;
    return;
  }
  return_ip = rw(seg[SS], sp);
  return_cs = rw(seg[SS], (U16)(sp + 2));
  /* DPMI clients enter through a 16-bit extender stub even when AX=1.
   * Client bitness controls the data/stack B bit and INT 31h pointer ABI,
   * not the initial code segment's default instruction size. */
  selectors[0] = dpmi_make_descriptor((U32)return_cs << 4,
                                      0xffffu, 0xfau, 0u);
  selectors[1] = dpmi_make_descriptor((U32)seg[DS] << 4,
                                      0xffffu, 0xf2u, client32 ? 4u : 0u);
  selectors[2] = dpmi_make_descriptor((U32)seg[SS] << 4,
                                      0xffffu, 0xf2u, client32 ? 4u : 0u);
  selectors[3] = dpmi_make_descriptor((U32)PSP << 4,
                                      0xffu, 0xf2u, client32 ? 4u : 0u);
  selectors[4] = dpmi_make_descriptor(0xf0000u, 0xffffu, 0xfau,
                                      client32 ? 4u : 0u);
  if (environment)
    selectors[5] = dpmi_make_descriptor((U32)environment << 4,
                                        0xffffu, 0xf2u, client32 ? 4u : 0u);
  KeLog("[dos] DPMI initial %u-bit selectors CS=%04x DS=%04x SS=%04x ES=%04x host=%04x return=%04x:%04x\n",
        client32 ? 32u : 16u, selectors[0], selectors[1], selectors[2],
        selectors[3], selectors[4], return_cs, return_ip);
  if (!selectors[0] || !selectors[1] || !selectors[2] || !selectors[3] ||
      !selectors[4] || (environment && !selectors[5])) {
    for (unsigned i = 0; i < 6; i++) if (selectors[i]) {
      U32 slot = (selectors[i] & 0xfff8u) >> 3;
      vm->DpmiDescriptorUsed[slot] = FALSE;
      memset(&vm->DpmiDescriptors[slot], 0, sizeof(DOS_DESCRIPTOR));
    }
    r[SP] = (U16)(sp + 4);
    seg[CS] = return_cs;
    ip = return_ip;
    flags |= CF;
    r[AX] = 0x8011;
    return;
  }
  if (environment) ww(PSP, 0x2cu, selectors[5]);
  ww(seg[SS], (U16)(sp + 2), selectors[0]);
  /* Preserve general registers, including their high words. Only ESP is
   * zero-extended by the DPMI initial-switch contract (set below). */
  if (client32) {
    KeLog("[dos] DPMI 32-bit entry registers EAX=%08x ECX=%08x EDX=%08x EBX=%08x ESP=%08x EBP=%08x ESI=%08x EDI=%08x\n",
          vm->Registers32[AX], vm->Registers32[CX], vm->Registers32[DX],
          vm->Registers32[BX], vm->Registers32[SP], vm->Registers32[BP],
          vm->Registers32[SI], vm->Registers32[DI]);
  }
  vm->Segments[CS] = selectors[0];
  vm->Segments[DS] = selectors[1];
  vm->Segments[SS] = selectors[2];
  vm->Segments[ES] = selectors[3];
  vm->Segments[FS] = vm->Segments[GS] = 0;
  vm->SegmentCache[CS] = vm->DpmiDescriptors[(selectors[0] & 0xfff8u) >> 3];
  vm->SegmentCache[DS] = vm->DpmiDescriptors[(selectors[1] & 0xfff8u) >> 3];
  vm->SegmentCache[SS] = vm->DpmiDescriptors[(selectors[2] & 0xfff8u) >> 3];
  vm->SegmentCache[ES] = vm->DpmiDescriptors[(selectors[3] & 0xfff8u) >> 3];
  memset(&vm->SegmentCache[FS], 0, sizeof(DOS_DESCRIPTOR));
  memset(&vm->SegmentCache[GS], 0, sizeof(DOS_DESCRIPTOR));
  vm->ProtectedMode = TRUE;
  vm->DpmiClient32 = (BOOLEAN)client32;
  vm->DpmiHostSelector = selectors[4];
  vm->Cr0 |= 1u;
  vm->Eip = return_ip;
  vm->Eflags = (vm->Eflags & 0xffff0000u) | (flags & ~CF) | 2u;
  vm->Flags = (U16)vm->Eflags;
  r32[SP] = (U16)(sp + 4u);
  r[SP] = (U16)(sp + 4u);
}
static void dpmi_raw_to_real(void) {
  DOS_VM *vm = current();
  U16 new_cs = (U16)r32[SI], new_ds = (U16)r32[AX];
  U16 new_es = (U16)r32[CX], new_ss = (U16)r32[DX];
  U16 new_ip = (U16)r32[DI], new_sp = (U16)r32[BX];
  KeLog("[dos] DPMI raw switch to real mode %04x:%04x\n", new_cs, new_ip);
  vm->ProtectedMode = FALSE;
  vm->Cr0 &= ~1u;
  vm->Segments[CS] = new_cs; vm->Segments[DS] = new_ds;
  vm->Segments[ES] = new_es; vm->Segments[SS] = new_ss;
  vm->Segments[FS] = vm->Segments[GS] = 0;
  memset(vm->SegmentCache, 0, sizeof(vm->SegmentCache));
  vm->Ip = new_ip;
  vm->Registers[SP] = new_sp;
  vm->Registers32[SP] = new_sp;
  vm->Flags = (U16)vm->Eflags;
}
static void dpmi_raw_to_protected(void) {
  DOS_VM *vm = current();
  U16 selectors[4] = {r[SI], r[AX], r[CX], r[DX]};
  DOS_DESCRIPTOR descriptors[4];
  for (unsigned i = 0; i < 4; i++)
    if (!read_descriptor(selectors[i], &descriptors[i])) {
      fail("DOSVM: invalid DPMI raw mode selector\r\n");
      return;
    }
  if (!(descriptors[0].Access & 8u) ||
      (descriptors[3].Access & 8u) || !(descriptors[3].Access & 2u)) {
    fail("DOSVM: invalid DPMI raw mode segment type\r\n");
    return;
  }
  KeLog("[dos] DPMI raw switch to protected mode %04x:%08x\n",
        selectors[0], r32[DI]);
  vm->Segments[CS] = selectors[0]; vm->Segments[DS] = selectors[1];
  vm->Segments[ES] = selectors[2]; vm->Segments[SS] = selectors[3];
  vm->Segments[FS] = vm->Segments[GS] = 0;
  vm->SegmentCache[CS] = descriptors[0];
  vm->SegmentCache[DS] = descriptors[1];
  vm->SegmentCache[ES] = descriptors[2];
  vm->SegmentCache[SS] = descriptors[3];
  memset(&vm->SegmentCache[FS], 0, sizeof(DOS_DESCRIPTOR));
  memset(&vm->SegmentCache[GS], 0, sizeof(DOS_DESCRIPTOR));
  vm->Eip = r32[DI];
  vm->Eflags = (vm->Eflags & 0xffff0000u) | vm->Flags;
  vm->Registers32[SP] = r32[BX];
  vm->Registers[SP] = (U16)r32[BX];
  vm->ProtectedMode = TRUE;
  vm->Cr0 |= 1u;
}
static int load_segment(unsigned index, U16 selector) {
  if (index >= 6)
    return 0;
  DOS_DESCRIPTOR *cache = &current()->SegmentCache[index];
  if (!(selector & 0xfff8u) && !(selector & 4u)) {
    if (index == CS || index == SS)
      return 0;
    memset(cache, 0, sizeof(*cache));
    seg[index] = selector;
    return 1;
  }
  DOS_DESCRIPTOR descriptor;
  memset(&descriptor, 0, sizeof(descriptor));
  if (!read_descriptor(selector, &descriptor) || !(descriptor.Access & 0x10u))
    return 0;
  U8 type = descriptor.Access & 0x0fu;
  int executable = !!(type & 8u), readable = !!(type & 2u), writable = !!(type & 2u);
  U8 dpl = (descriptor.Access >> 5) & 3u;
  U8 cpl = seg[CS] & 3u, rpl = selector & 3u;
  if (index == CS) {
    int conforming = executable && !!(type & 4u);
    if (!executable || (conforming ? dpl > cpl : (dpl != cpl || rpl > cpl)))
      return 0;
  } else if (index == SS) {
    if (executable || !writable || dpl != cpl || rpl != cpl)
      return 0;
  } else {
    if (executable && !readable)
      return 0;
    if (executable && (type & 4u) ? dpl > cpl : ((dpl < cpl) || (dpl < rpl)))
      return 0;
  }
  *cache = descriptor;
  seg[index] = selector;
  return 1;
}
static int enter_protected_mode(U32 cr0) {
  DOS_VM *vm = current();
  if (cr0 & 0x80000000u) {
    fail("DOSVM: paging is not implemented in the DOS CPU\r\n");
    return 0;
  }
  if (!(vm->Cr0 & 1u) && (cr0 & 1u)) {
    for (unsigned i = 0; i < 6; i++) {
      vm->SegmentCache[i].Base = (U32)vm->Segments[i] << 4;
      vm->SegmentCache[i].Limit = 0xffffu;
      vm->SegmentCache[i].Access = i == CS ? 0x9bu : 0x93u;
      vm->SegmentCache[i].Flags = 0;
      vm->SegmentCache[i].Valid = TRUE;
    }
    vm->Eip = vm->Ip;
    vm->Eflags = (vm->Eflags & 0xffff0000u) | vm->Flags;
    vm->ProtectedMode = TRUE;
  } else if ((vm->Cr0 & 1u) && !(cr0 & 1u)) {
    vm->ProtectedMode = FALSE;
    vm->Ip = (U16)vm->Eip;
    memset(vm->SegmentCache, 0, sizeof(vm->SegmentCache));
  }
  vm->Cr0 = cr0;
  return 1;
}
static __attribute__((always_inline)) inline int protected_linear(
                            DOS_VM *vm, unsigned index, U32 offset, U32 size,
                            int write_access, int execute_access,
                            U32 *linear) {
  DOS_DESCRIPTOR *d;
  U32 last;
  if (index >= 6 || !size)
    return 0;
  d = &vm->SegmentCache[index];
  if (!d->Valid || !(d->Access & 0x80u) || offset > 0xffffffffu - (size - 1))
    return 0;
  last = offset + size - 1;
  U8 type = d->Access & 0x0fu;
  int executable = !!(type & 8u);
  if (execute_access ? !executable
                     : (write_access ? (executable || !(type & 2u))
                                     : (executable && !(type & 2u))))
    return 0;
  /* DOS/4GW uses flat 4-GiB code/data descriptors for almost all game
   * memory traffic.  Once access rights have been checked above, this common
   * case needs only the VM arena bounds check: avoid repeating limit,
   * expand-down and base-overflow arithmetic on every load/store. */
  if (d->Base == 0 && d->Limit == 0xffffffffu &&
      (executable || !(type & 4u))) {
    if (offset >= MEM_SIZE || size > MEM_SIZE - offset)
      return 0;
    *linear = offset;
    return 1;
  }
  if (type & 4u) {
    U32 lower = d->Limit + 1u;
    U32 upper = (d->Flags & 4u) ? 0xffffffffu : 0xffffu;
    if (offset < lower || last > upper)
      return 0;
  } else if (last > d->Limit)
    return 0;
  if (d->Base > 0xffffffffu - offset)
    return 0;
  *linear = d->Base + offset;
  if (*linear >= MEM_SIZE || size > MEM_SIZE - *linear)
    return 0;
  return 1;
}
static inline U32 dos_guest_load(const U8 *address, unsigned size) {
  U32 value = 0;
  switch (size) {
  case 1:
    value = address[0];
    break;
  case 2: {
    U16 word_value;
    __builtin_memcpy(&word_value, address, sizeof(word_value));
    value = word_value;
    break;
  }
  case 4:
    __builtin_memcpy(&value, address, sizeof(value));
    break;
  default:
    break;
  }
  return value;
}

static inline void dos_guest_store(U8 *address, unsigned size, U32 value) {
  switch (size) {
  case 1:
    address[0] = (U8)value;
    break;
  case 2: {
    U16 word_value = (U16)value;
    __builtin_memcpy(address, &word_value, sizeof(word_value));
    break;
  }
  case 4:
    __builtin_memcpy(address, &value, sizeof(value));
    break;
  default:
    break;
  }
}

/* Validate a whole contiguous span for a protected-mode REP string fast path.
 * The per-element interpreter path remains the fallback for wrapped offsets,
 * VGA/device windows, overlapping MOVS ranges, and non-flat side effects. */
static int dos_string_span(DOS_VM *vm, unsigned segment, U32 cursor,
                           U32 count, unsigned element, int address32,
                           int backwards, int write_access, U32 *linear,
                           U32 *byte_count) {
  if (!count || count > 4096u ||
      (element != 1u && element != 2u && element != 4u))
    return 0;
  U64 bytes64 = (U64)count * element;
  U64 back = backwards ? (U64)(count - 1u) * element : 0;
  U64 low = (U64)cursor;
  U64 address_limit = address32 ? 0x100000000ULL : 0x10000ULL;
  if (backwards) {
    if (low < back) return 0;
    low -= back;
  }
  if (low + bytes64 > address_limit) return 0;
  U32 bytes = (U32)bytes64;
  if (!protected_linear(vm, segment, (U32)low, bytes, write_access, 0, linear))
    return 0;
  /* VGA/CGA memory has observable palette, plane, or scanout semantics and
   * must use the device-aware per-element path. */
  if (*linear < 0xb0000u && *linear + bytes > 0xa0000u)
    return 0;
  if (vm->VideoMode >= 4u && vm->VideoMode <= 6u &&
      *linear < 0xbc000u && *linear + bytes > 0xb8000u)
    return 0;
  *byte_count = bytes;
  return 1;
}

static U8 protected_read8(DOS_VM *vm, unsigned s, U32 o, int execute_access) {
  U32 a;
  if (!protected_linear(vm, s, o, 1, 0, execute_access, &a)) {
    fail(execute_access ? "DOSVM: protected instruction fetch fault\r\n"
                        : "DOSVM: protected memory read fault\r\n");
    return 0;
  }
  return vm->Memory[a];
}
static __attribute__((always_inline)) inline U32 protected_fetch(DOS_VM *vm, unsigned width) {
  U32 linear;
  int code32 = !!(vm->SegmentCache[CS].Flags & 4u);

  /* Most protected-mode instructions fetch immediates from a flat 32-bit
   * code segment.  Validate the complete immediate once and use one
   * unaligned-safe load instead of recursively doing a descriptor check for
   * every byte.  Retain the byte path for 16-bit IP wrap and fault boundaries. */
  if (width == 1 || width == 2 || width == 4) {
    const DOS_DESCRIPTOR *cs = &vm->SegmentCache[CS];
    /* DOS/4GW keeps a flat, present 4-GiB executable CS for practically all
     * game code. Fetch directly from the VM arena after checking the cached
     * descriptor once; retain the full descriptor path for unusual CS values
     * and any boundary/fault case. */
    if (cs->Valid && (cs->Access & 0x88u) == 0x88u &&
        cs->Base == 0 && cs->Limit == 0xffffffffu &&
        (code32 || vm->Eip + width <= 0x10000u) &&
        vm->Eip < MEM_SIZE && width <= MEM_SIZE - vm->Eip) {
      U32 value = dos_guest_load(vm->Memory + vm->Eip, width);
      vm->Eip += width;
      if (!code32) vm->Eip &= 0xffffu;
      return value;
    }
    if ((code32 || vm->Eip + width <= 0x10000u) &&
        protected_linear(vm, CS, vm->Eip, width, 0, 1, &linear)) {
      U32 value = dos_guest_load(vm->Memory + linear, width);
      vm->Eip += width;
      if (!code32) vm->Eip &= 0xffffu;
      return value;
    }
  }

  U32 value = protected_read8(vm, CS, vm->Eip, 1);
  if (!vm->Running) return 0;
  vm->Eip++;
  if (!code32) vm->Eip &= 0xffffu;
  for (unsigned i = 1; i < width; i++) {
    U32 byte = protected_read8(vm, CS, vm->Eip, 1);
    if (!vm->Running) return 0;
    value |= byte << (i * 8);
    vm->Eip++;
    if (!code32) vm->Eip &= 0xffffu;
  }
  return value;
}
static int load_ldt(U16 selector) {
  if (!(selector & 0xfff8u)) {
    current()->Ldtr = 0;
    memset(&current()->LdtCache, 0, sizeof(current()->LdtCache));
    return 1;
  }
  DOS_DESCRIPTOR d;
  if ((selector & 4u) || (selector & 3u) ||
      !read_descriptor(selector, &d) || (d.Access & 0x1fu) != 0x02u)
    return 0;
  current()->Ldtr = selector;
  current()->LdtCache = d;
  return 1;
}
typedef struct {
  U8 mod, reg, rm, is_reg, segment;
  U32 offset;
} PM_MOD;
static __attribute__((always_inline)) inline U32 pm_register(DOS_VM *vm, unsigned index, unsigned width) {
  if (width == 8)
    return index < 4 ? (U8)vm->Registers32[index] : (U8)(vm->Registers32[index - 4] >> 8);
  return width == 16 ? (U16)vm->Registers32[index] : vm->Registers32[index];
}
static __attribute__((always_inline)) inline void pm_set_register(DOS_VM *vm, unsigned index, unsigned width, U32 value) {
  if (width == 8) {
    unsigned reg = index & 3;
    if (index < 4)
      vm->Registers32[reg] = (vm->Registers32[reg] & 0xffffff00u) | (value & 0xffu);
    else
      vm->Registers32[reg] = (vm->Registers32[reg] & 0xffff00ffu) | ((value & 0xffu) << 8);
  } else if (width == 16)
    vm->Registers32[index] = (vm->Registers32[index] & 0xffff0000u) | (value & 0xffffu);
  else
    vm->Registers32[index] = value;
  unsigned low_index = width == 8 && index >= 4 ? index - 4 : index;
  vm->Registers[low_index] = (U16)vm->Registers32[low_index];
}
static U32 pm_read(DOS_VM *vm, unsigned s, U32 offset, unsigned width) {
  U32 linear;
  unsigned size = width / 8;
  if (!protected_linear(vm, s, offset, size, 0, 0, &linear)) {
    DOS_DESCRIPTOR *d = &vm->SegmentCache[s];
    KeLog("[dos] protected read fault CS=%04x EIP=%08x op=%02x at %08x seg=%04x off=%08x width=%u base=%08x limit=%08x access=%02x flags=%02x AX=%08x CX=%08x SI=%08x DI=%08x EFLAGS=%08x\n",
          vm->Segments[CS], vm->Eip, vm->CurrentOpcode,
          vm->CurrentInstructionIp, vm->Segments[s], offset, width,
          d->Base, d->Limit, d->Access, d->Flags,
          vm->Registers32[AX], vm->Registers32[CX], vm->Registers32[SI], vm->Registers32[DI], vm->Eflags);
    U32 code = vm->SegmentCache[CS].Base +
               vm->CurrentInstructionIp - 4u;
    if (code <= MEM_SIZE - 12u)
      KeLog("[dos] fault bytes %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
            vm->Memory[code], vm->Memory[code + 1], vm->Memory[code + 2], vm->Memory[code + 3],
            vm->Memory[code + 4], vm->Memory[code + 5], vm->Memory[code + 6], vm->Memory[code + 7],
            vm->Memory[code + 8], vm->Memory[code + 9], vm->Memory[code + 10], vm->Memory[code + 11]);
    fail("DOSVM: protected memory read fault\r\n");
    return 0;
  }
  /* VGA planar reads need the virtual latches/plane selection.  In the
   * normal chain-4 mode used by Doom, the guest byte array is the framebuffer
   * and ordinary unaligned loads are both correct and much cheaper. */
  if (linear + size <= 0xa0000u || linear >= 0xb0000u ||
      (linear >= 0xa0000u && linear + size <= 0xb0000u &&
       !(vm->VideoMode == 0x13u && !(vm->VgaSequencer[4] & 8u)))) {
    U32 value = 0;
    value = dos_guest_load(vm->Memory + linear, size);
    return value;
  }
  U32 value = 0;
  for (unsigned i = 0; i < size; i++) {
    U32 at = linear + i;
    U8 byte = at >= 0xa0000u && at < 0xb0000u
                  ? DosVideoReadMemory(vm, at) : vm->Memory[at];
    value |= (U32)byte << (i * 8);
  }
  return value;
}
static int pm_write(DOS_VM *vm, unsigned s, U32 offset, unsigned width, U32 value) {
  U32 linear;
  unsigned size = width / 8;
  if (!protected_linear(vm, s, offset, size, 1, 0, &linear)) {
    DOS_DESCRIPTOR *d = &vm->SegmentCache[s];
    KeLog("[dos] protected write fault CS=%04x EIP=%08x op=%02x at %08x seg=%04x off=%08x width=%u base=%08x limit=%08x access=%02x flags=%02x value=%08x\n",
          vm->Segments[CS], vm->Eip, vm->CurrentOpcode,
          vm->CurrentInstructionIp, vm->Segments[s], offset, width,
          d->Base, d->Limit, d->Access, d->Flags, value);
    fail("DOSVM: protected memory write fault\r\n");
    return 0;
  }
  if (linear + size <= 0xa0000u || linear >= 0xb0000u) {
    if (vm->VideoMode >= 4u && vm->VideoMode <= 6u &&
        linear < 0xbc000u && linear + size > 0xb8000u)
      vm->VideoDirty = TRUE;
    dos_guest_store(vm->Memory + linear, size, value);
  } else if (linear >= 0xa0000u && linear + size <= 0xb0000u &&
             vm->VideoMode == 0x13u && (vm->VgaSequencer[4] & 8u)) {
    /* Chain-4 mode maps the DOS framebuffer directly into VM memory.  Mark
     * the frame dirty once and store 8/16/32-bit values in one operation;
     * keep the plane-aware slow path for planar VGA modes. */
    vm->VideoDirty = TRUE;
    dos_guest_store(vm->Memory + linear, size, value);
  } else {
    for (unsigned i = 0; i < size; i++) {
      U32 at = linear + i;
      U8 byte = (U8)(value >> (i * 8));
      if (at >= 0xa0000u && at < 0xb0000u)
        DosVideoWriteMemory(vm, at, byte);
      else
        vm->Memory[at] = byte;
    }
  }
  return 1;
}
static U16 dpmi_copy_descriptor(U16 function) {
  DOS_VM *vm = current();
  U16 selector = (U16)r32[BX];
  U32 structure = vm->DpmiClient32 ? r32[DI] : (U16)r32[DI];
  DOS_DESCRIPTOR *d;
  if (!dpmi_descriptor(selector, &d, NULL)) return 0x8022;
  U32 linear;
  if (!protected_linear(vm, ES, structure, 8, function == 0x000b, 0, &linear))
    return 0x8022;
  if (function == 0x000b) {
    U32 limit = (d->Flags & 8u) ? (d->Limit >> 12) : d->Limit;
    U8 bytes[8] = {
      (U8)limit, (U8)(limit >> 8), (U8)d->Base, (U8)(d->Base >> 8),
      (U8)(d->Base >> 16), d->Access,
      (U8)((d->Flags << 4) | ((limit >> 16) & 0x0fu)),
      (U8)(d->Base >> 24)
    };
    for (U32 i = 0; i < 8; i++)
      if (!pm_write(vm, ES, structure + i, 8, bytes[i])) return 0x8022;
    return 0;
  }
  U8 bytes[8];
  for (U32 i = 0; i < 8; i++) bytes[i] = (U8)pm_read(vm, ES, structure + i, 8);
  U8 access = bytes[5], desc_flags = bytes[6] >> 4;
  U32 base = (U32)bytes[2] | ((U32)bytes[3] << 8) |
             ((U32)bytes[4] << 16) | ((U32)bytes[7] << 24);
  U32 limit = (U32)bytes[0] | ((U32)bytes[1] << 8) |
              ((U32)(bytes[6] & 0x0fu) << 16);
  if (!(access & 0x10u) || ((access >> 5) & 3u) != 3u) {
    KeLog("[dos] descriptor %04x rejected access=%02x flags=%02x base=%08x limit=%08x\n",
          selector, access, desc_flags, base, limit);
    return 0x8021;
  }
  if (desc_flags & 8u) limit = (limit << 12) | 0xfffu;
  /* A descriptor may span more than the emulated physical memory. Validate
   * each actual access in protected_linear instead of rejecting the LDT entry. */
  if (vm->DpmiDosDescriptor[(selector & 0xfff8u) >> 3]) return 0x8022;
  d->Base = base; d->Limit = limit; d->Access = access;
  d->Flags = desc_flags; d->Valid = !!(access & 0x80u);
  for (U32 i = 0; i < 6; i++)
    if (vm->Segments[i] == selector) vm->SegmentCache[i] = *d;
  return 0;
}
static int dpmi_simulate_real_interrupt(U8 vector, U32 structure,
                                        int force_builtin_dos) {
  DOS_VM *vm = current();
  U32 linear;
  if (!protected_linear(vm, ES, structure, 0x32u, 0, 0, &linear)) return 0;
  U32 values[8];
  static const U8 offsets[8] = {0x1c, 0x18, 0x14, 0x10, 0x2e, 0x08, 0x04, 0x00};
  for (U32 i = 0; i < 8; i++) values[i] = pm_read(vm, ES, structure + offsets[i], 32);
  U16 new_flags = (U16)pm_read(vm, ES, structure + 0x20, 16);
  U16 new_es = (U16)pm_read(vm, ES, structure + 0x22, 16);
  U16 new_ds = (U16)pm_read(vm, ES, structure + 0x24, 16);
  U16 new_fs = (U16)pm_read(vm, ES, structure + 0x26, 16);
  U16 new_gs = (U16)pm_read(vm, ES, structure + 0x28, 16);
  U16 new_sp = (U16)values[SP], new_ss = (U16)pm_read(vm, ES, structure + 0x30, 16);
  if (!new_ss || !new_sp) { new_ss = 0x0e00; new_sp = 0xfffe; }
  if ((U32)new_ss * 16u + new_sp >= DOS_REAL_MEMORY_SIZE) return 0;
  U16 saved_regs[8], saved_segments[6], saved_flags = vm->Flags, saved_ip = vm->Ip;
  U32 saved_regs32[8], saved_eflags = vm->Eflags, saved_eip = vm->Eip;
  U32 saved_cr0 = vm->Cr0;
  BOOLEAN saved_protected = vm->ProtectedMode;
  memcpy(saved_regs, vm->Registers, sizeof(saved_regs));
  memcpy(saved_regs32, vm->Registers32, sizeof(saved_regs32));
  memcpy(saved_segments, vm->Segments, sizeof(saved_segments));
  for (U32 i = 0; i < 8; i++) {
    vm->Registers32[i] = values[i];
    vm->Registers[i] = (U16)values[i];
  }
  vm->Registers[SP] = new_sp;
  vm->Registers32[SP] = new_sp;
  vm->Segments[ES] = new_es; vm->Segments[DS] = new_ds;
  vm->Segments[FS] = new_fs; vm->Segments[GS] = new_gs;
  vm->Segments[SS] = new_ss;
  vm->Flags = new_flags & 0x0fd5u;
  vm->Eflags = vm->Flags | 2u;
  vm->ProtectedMode = FALSE;
  vm->Cr0 &= ~1u;
  U16 saved_vector_off = 0, saved_vector_seg = 0;
  if (force_builtin_dos) {
    saved_vector_off = rw(0, (U16)(vector * 4u));
    saved_vector_seg = rw(0, (U16)(vector * 4u + 2u));
    ww(0, (U16)(vector * 4u), 0);
    ww(0, (U16)(vector * 4u + 2u), 0);
  }
  dos_int(vector);
  if (force_builtin_dos) {
    ww(0, (U16)(vector * 4u), saved_vector_off);
    ww(0, (U16)(vector * 4u + 2u), saved_vector_seg);
  }
  for (U32 i = 0; i < 8; i++)
    vm->Registers32[i] = (vm->Registers32[i] & 0xffff0000u) | vm->Registers[i];
  U32 result[8];
  for (U32 i = 0; i < 8; i++) result[i] = vm->Registers32[i];
  U16 result_flags = vm->Flags, result_es = vm->Segments[ES];
  U16 result_ds = vm->Segments[DS], result_fs = vm->Segments[FS];
  U16 result_gs = vm->Segments[GS], result_sp = vm->Registers[SP];
  U16 result_ss = vm->Segments[SS];
  memcpy(vm->Registers, saved_regs, sizeof(saved_regs));
  memcpy(vm->Registers32, saved_regs32, sizeof(saved_regs32));
  memcpy(vm->Segments, saved_segments, sizeof(saved_segments));
  vm->Flags = saved_flags; vm->Ip = saved_ip;
  vm->Eflags = saved_eflags; vm->Eip = saved_eip; vm->Cr0 = saved_cr0;
  vm->ProtectedMode = saved_protected;
  for (U32 i = 0; i < 8; i++)
    if (!pm_write(vm, ES, structure + offsets[i], 32, result[i])) return 0;
  return pm_write(vm, ES, structure + 0x20, 16, result_flags) &&
         pm_write(vm, ES, structure + 0x22, 16, result_es) &&
         pm_write(vm, ES, structure + 0x24, 16, result_ds) &&
         pm_write(vm, ES, structure + 0x26, 16, result_fs) &&
         pm_write(vm, ES, structure + 0x28, 16, result_gs) &&
         pm_write(vm, ES, structure + 0x2e, 16, result_sp) &&
         pm_write(vm, ES, structure + 0x30, 16, result_ss);
}
static PM_MOD pm_decode(DOS_VM *vm, int segment_override, int address32) {
  PM_MOD m = {0};
  U8 x = (U8)protected_fetch(vm, 1);
  m.mod = x >> 6;
  m.reg = (x >> 3) & 7;
  m.rm = x & 7;
  m.is_reg = m.mod == 3;
  m.segment = DS;
  if (m.is_reg)
    return m;
  if (address32) {
    U32 base = 0, index = 0;
    int has_base = 1, uses_stack = 0;
    if (m.rm == 4) {
      U8 sib = (U8)protected_fetch(vm, 1);
      unsigned scale = sib >> 6, idx = (sib >> 3) & 7, b = sib & 7;
      if (idx != 4)
        index = vm->Registers32[idx] << scale;
      if (m.mod == 0 && b == 5)
        has_base = 0;
      else {
        base = vm->Registers32[b];
        uses_stack = b == SP || b == BP;
      }
    } else if (m.mod == 0 && m.rm == 5)
      has_base = 0;
    else {
      base = vm->Registers32[m.rm];
      uses_stack = m.rm == BP || m.rm == SP;
    }
    if (!has_base)
      base = protected_fetch(vm, 4);
    if (m.mod == 1)
      base += (U32)(INT32)(signed char)protected_fetch(vm, 1);
    else if (m.mod == 2)
      base += protected_fetch(vm, 4);
    m.offset = base + index;
    if (uses_stack)
      m.segment = SS;
  } else {
    U16 base = 0;
    int uses_bp = 0;
    switch (m.rm) {
    case 0: base = vm->Registers[BX] + vm->Registers[SI]; break;
    case 1: base = vm->Registers[BX] + vm->Registers[DI]; break;
    case 2: base = vm->Registers[BP] + vm->Registers[SI]; uses_bp = 1; break;
    case 3: base = vm->Registers[BP] + vm->Registers[DI]; uses_bp = 1; break;
    case 4: base = vm->Registers[SI]; break;
    case 5: base = vm->Registers[DI]; break;
    case 6:
      if (m.mod == 0) base = (U16)protected_fetch(vm, 2);
      else { base = vm->Registers[BP]; uses_bp = 1; }
      break;
    default: base = vm->Registers[BX]; break;
    }
    if (m.mod == 1)
      base += (U16)(INT16)(signed char)protected_fetch(vm, 1);
    else if (m.mod == 2)
      base += (U16)protected_fetch(vm, 2);
    m.offset = base;
    if (uses_bp)
      m.segment = SS;
  }
  if (segment_override >= 0)
    m.segment = (U8)segment_override;
  return m;
}
/* Short inline path for the overwhelmingly common DOS/4GW operand: a
 * present flat 4-GiB segment addressing ordinary VM RAM. Keep descriptor
 * rights and arena bounds checks here; unusual segments and VGA still fall
 * through to the full checked accessors below. */
static __attribute__((always_inline)) inline int pm_flat_operand_linear(
    DOS_VM *vm, unsigned segment, U32 offset, unsigned width, int write_access,
    U32 *linear) {
  if (segment >= 6 || (width != 8 && width != 16 && width != 32)) return 0;
  DOS_DESCRIPTOR *d = &vm->SegmentCache[segment];
  if (!d->Valid || !(d->Access & 0x80u) || d->Base != 0 ||
      d->Limit != 0xffffffffu)
    return 0;
  U8 type = d->Access & 0x0fu;
  int executable = !!(type & 8u);
  if ((type & 4u) ||
      (write_access ? (executable || !(type & 2u))
                    : (executable && !(type & 2u))))
    return 0;
  U32 size = width / 8u;
  if (offset >= MEM_SIZE || size > MEM_SIZE - offset) return 0;
  *linear = offset;
  return 1;
}
static inline U32 pm_read_rm(DOS_VM *vm, PM_MOD m, unsigned width) {
  if (m.is_reg) return pm_register(vm, m.rm, width);
  U32 linear, size = width / 8u;
  if (__builtin_expect(
          pm_flat_operand_linear(vm, m.segment, m.offset, width, 0, &linear), 1) &&
      __builtin_expect(
          (linear + size <= 0xa0000u || linear >= 0xb0000u ||
           (linear >= 0xa0000u && linear + size <= 0xb0000u &&
            vm->VideoMode == 0x13u && (vm->VgaSequencer[4] & 8u))), 1))
    return dos_guest_load(vm->Memory + linear, size);
  return pm_read(vm, m.segment, m.offset, width);
}
static inline int pm_write_rm(DOS_VM *vm, PM_MOD m, unsigned width, U32 value) {
  if (m.is_reg) {
    pm_set_register(vm, m.rm, width, value);
    return 1;
  }
  U32 linear, size = width / 8u;
  if (__builtin_expect(
          pm_flat_operand_linear(vm, m.segment, m.offset, width, 1, &linear), 1) &&
      __builtin_expect(
          (linear + size <= 0xa0000u || linear >= 0xb0000u ||
           (linear >= 0xa0000u && linear + size <= 0xb0000u &&
            vm->VideoMode == 0x13u && (vm->VgaSequencer[4] & 8u))), 1)) {
    if ((linear >= 0xa0000u && linear + size <= 0xb0000u) ||
        (vm->VideoMode >= 4u && vm->VideoMode <= 6u &&
         linear < 0xbc000u && linear + size > 0xb8000u))
      vm->VideoDirty = TRUE;
    dos_guest_store(vm->Memory + linear, size, value);
    return 1;
  }
  return pm_write(vm, m.segment, m.offset, width, value);
}
static int pm_load_segment(DOS_VM *vm, unsigned index, U16 selector) {
  if (!load_segment(index, selector)) {
    KeLog("[dos] protected segment load failed CS=%04x EIP=%08x op=%02x at %08x register=%u selector=%04x SP=%08x\n",
          vm->Segments[CS], vm->Eip, vm->CurrentOpcode,
          vm->CurrentInstructionIp, index, selector,
          pm_stack_pointer(vm));
    fail("DOSVM: protected segment selector fault\r\n");
    return 0;
  }
  if (index == SS) {
    /* MOV SS/POP SS/LSS block maskable interrupts through the next
     * instruction while the caller installs the matching stack pointer. */
    vm->InterruptShadow = 1;
  }
  return 1;
}
static U32 pm_alu(DOS_VM *vm, unsigned operation, U32 a, U32 b, unsigned width) {
  U32 mask = width == 8 ? 0xffu : width == 16 ? 0xffffu : 0xffffffffu;
  U32 sign = width == 8 ? 0x80u : width == 16 ? 0x8000u : 0x80000000u;
  U64 wide_result;
  U32 result, old_cf = vm->Eflags & CF;
  U32 f = vm->Eflags & ~(CF | ZF | SF | OF | AF | 4u);
  if (operation == 0 || operation == 2) {
    wide_result = (U64)(a & mask) + (b & mask) +
                  (operation == 2 ? !!old_cf : 0);
    result = (U32)wide_result & mask;
    if (wide_result > mask) f |= CF;
    if ((~(a ^ b) & (a ^ result) & sign)) f |= OF;
    if ((a ^ b ^ result) & 0x10u) f |= AF;
  } else if (operation == 5 || operation == 7 || operation == 3) {
    U32 borrow = operation == 3 ? !!old_cf : 0;
    result = (a - b - borrow) & mask;
    if ((U64)(a & mask) < (U64)(b & mask) + borrow) f |= CF;
    if (((a ^ b) & (a ^ result) & sign)) f |= OF;
    if ((a ^ b ^ result) & 0x10u) f |= AF;
  } else if (operation == 4)
    result = (a & b) & mask;
  else if (operation == 1)
    result = (a | b) & mask;
  else
    result = (a ^ b) & mask;
  if (!result) f |= ZF;
  if (result & sign) f |= SF;
  if (parity((U8)result)) f |= 4u;
  vm->Eflags = f | 2u;
  vm->Flags = (U16)vm->Eflags;
  return result;
}
static U32 pm_shift(DOS_VM *vm, unsigned kind, U32 value, U32 count, unsigned width) {
  U32 mask = width == 8 ? 0xffu : width == 16 ? 0xffffu : 0xffffffffu;
  U32 sign = width == 8 ? 0x80u : width == 16 ? 0x8000u : 0x80000000u;
  U32 original = value & mask, carry = 0;
  count &= 31u;
  if (!count) return original;
  for (U32 i = 0; i < count; i++) {
    if (kind == 0 || kind == 2) {
      carry = !!(value & sign);
      value = ((value << 1) | (kind == 2 ? !!(vm->Eflags & CF) : carry)) & mask;
    } else if (kind == 1 || kind == 3) {
      carry = value & 1u;
      U32 incoming = kind == 1 ? carry : !!(vm->Eflags & CF);
      value = (value >> 1) | (incoming ? sign : 0);
    } else if (kind == 4 || kind == 6) {
      carry = !!(value & sign);
      value = (value << 1) & mask;
    } else if (kind == 5) {
      carry = value & 1u;
      value >>= 1;
    } else {
      carry = value & 1u;
      value = (value >> 1) | (value & sign);
    }
    vm->Eflags = (vm->Eflags & ~CF) | (carry ? CF : 0);
  }
  if (kind >= 4) {
    vm->Eflags &= ~(ZF | SF | 4u);
    if (!(value & mask)) vm->Eflags |= ZF;
    if (value & sign) vm->Eflags |= SF;
    if (parity((U8)value)) vm->Eflags |= 4u;
  }
  if (count == 1) {
    vm->Eflags &= ~OF;
    if (kind == 0 || kind == 2 || kind == 4 || kind == 6)
      vm->Eflags |= (!!(value & sign) != !!carry) ? OF : 0;
    else if (kind == 5 && (original & sign))
      vm->Eflags |= OF;
    else if ((kind == 1 || kind == 3) &&
             (!!(value & sign) != !!(value & (sign >> 1))))
      vm->Eflags |= OF;
  }
  vm->Flags = (U16)vm->Eflags;
  return value & mask;
}
static U32 pm_stack_pointer(DOS_VM *vm) {
  return (vm->SegmentCache[SS].Flags & 4u)
             ? vm->Registers32[SP] : vm->Registers[SP];
}
static void pm_set_stack_pointer(DOS_VM *vm, U32 value) {
  if (vm->SegmentCache[SS].Flags & 4u)
    vm->Registers32[SP] = value;
  else
    vm->Registers32[SP] = (vm->Registers32[SP] & 0xffff0000u) | (U16)value;
  vm->Registers[SP] = (U16)value;
}
static int pm_push(DOS_VM *vm, U32 value, unsigned width) {
  U32 sp = pm_stack_pointer(vm) - width / 8;
  pm_set_stack_pointer(vm, sp);
  return pm_write(vm, SS, sp, width, value);
}
static U32 pm_pop(DOS_VM *vm, unsigned width) {
  U32 sp = pm_stack_pointer(vm);
  U32 value = pm_read(vm, SS, sp, width);
  if (!vm->Running) return 0;
  pm_set_stack_pointer(vm, sp + width / 8);
  return value;
}
static void pm_set_near_ip(DOS_VM *vm, U32 target, unsigned width) {
  vm->Eip = width == 16 ? (U16)target : target;
}
static int protected_interrupt(U8 vector, int software, unsigned width) {
  DOS_VM *vm = current();
  if (vector == 0x31 && software) {
    dpmi_interrupt();
    return 1;
  }
  /* Route the INT 33h reset/install check to the emulated DOS mouse driver
   * when the protected-mode client has no replacement vector. */
  if (software && vector == 0x33 && !vm->DpmiPmVectorSelector[vector]) {
    U16 function = (U16)r32[AX];
    if (function == 0 || function == 0x21) {
      DosMouseInitialize(vm);
      pm_set_register(vm, AX, 16, 0xffff);
      pm_set_register(vm, BX, 16, 3);
    }
    return 1;
  }
  if (vm->DpmiPmVectorSelector[vector]) {
    U16 selector = vm->DpmiPmVectorSelector[vector];
    U32 target = vm->DpmiPmVectorOffset[vector];
    U32 old_flags = vm->Eflags;
    U16 old_cs = seg[CS];
    U32 old_ip = vm->Eip;
    unsigned frame_width = vm->DpmiClient32 ? 32 : 16;
    if (!pm_push(vm, old_flags, frame_width) || !pm_push(vm, old_cs, frame_width) ||
        !pm_push(vm, old_ip, frame_width) || !pm_load_segment(vm, CS, selector))
      return 0;
    vm->Eip = target;
    vm->Eflags &= ~(0x100u | 0x200u | 0x4000u);
    vm->Flags = (U16)vm->Eflags;
    return 1;
  }
  U32 displacement = (U32)vector * 8u;
  if (vm->Idtr.Base > 0xffffffffu - displacement) {
    fail("DOSVM: protected interrupt vector address overflow\r\n");
    return 0;
  }
  U32 entry = vm->Idtr.Base + displacement;
  if (displacement + 7u > vm->Idtr.Limit ||
      entry > MEM_SIZE - 8) {
    fail("DOSVM: protected interrupt vector is outside IDT\r\n");
    return 0;
  }
  const U8 *gate = mem + entry;
  U8 access = gate[5];
  U8 type = access & 0x1fu;
  U8 dpl = (access >> 5) & 3u;
  if (!(access & 0x80u) || (type != 0x0eu && type != 0x0fu &&
                             type != 0x06u && type != 0x07u)) {
    fail("DOSVM: invalid protected interrupt gate\r\n");
    return 0;
  }
  if (software && (seg[CS] & 3u) > dpl) {
    fail("DOSVM: protected interrupt privilege fault\r\n");
    return 0;
  }
  U16 selector = (U16)gate[2] | ((U16)gate[3] << 8);
  U32 target = (U32)gate[0] | ((U32)gate[1] << 8);
  if (type == 0x0eu || type == 0x0fu)
    target |= (U32)gate[6] << 16 | (U32)gate[7] << 24;
  if ((selector & 3u) != (seg[CS] & 3u)) {
    fail("DOSVM: interrupt privilege stack switching requires a TSS\r\n");
    return 0;
  }
  U32 old_flags = vm->Eflags;
  U16 old_cs = seg[CS];
  U32 old_ip = vm->Eip;
  if (!pm_push(vm, old_flags, width) || !pm_push(vm, old_cs, width) ||
      !pm_push(vm, old_ip, width) || !pm_load_segment(vm, CS, selector))
    return 0;
  vm->Eip = target;
  vm->Eflags &= ~(0x100u | 0x4000u);
  if (type == 0x06u || type == 0x0eu)
    vm->Eflags &= ~0x200u;
  vm->Flags = (U16)vm->Eflags;
  return 1;
}
/*
 * Hardware state is polled at a guest-instruction deadline. Keeping the
 * deadline avoids a read/decrement/write of a countdown in the CPU hot loop.
 *
 * Interrupt shadows still expire instruction-by-instruction. When the shadow
 * ends, force a poll on the very next instruction boundary.
 */
static __attribute__((always_inline)) inline int dos_hardware_poll_due(DOS_VM *vm) {
  if (vm->InterruptShadow) {
    vm->InterruptShadow--;
    if (!vm->InterruptShadow)
      vm->HardwarePollDeadline = vm->Steps + 1u;
    return 0;
  }
  if ((INT32)(vm->Steps - vm->HardwarePollDeadline) < 0)
    return 0;
  vm->HardwarePollDeadline = vm->Steps + DOS_HW_POLL_INTERVAL;
  return 1;
}

static void dos_timer_update(DOS_VM *vm) {
  UINT32 tick = (UINT32)KeGetTickCount();
  if (!vm->LastTimerTick) {
    vm->LastTimerTick = tick ? tick : 1u;
    return;
  }
  UINT32 elapsed = tick - vm->LastTimerTick;
  if (!elapsed) return;
  vm->LastTimerTick = tick;
  U32 divisor = vm->PitReload ? vm->PitReload : 65536u;
  U32 period = divisor * 100u; /* host clock is 100 Hz */
  UINT64 phase = (UINT64)vm->DosTimerPhase + (UINT64)elapsed * 1193182u;
  /* The 8259 holds one pending edge while masked. Honour the client's
   * programmed PIT divisor (Doom installs a 35-Hz timer). */
  vm->DosTimerPhase = phase >= period ? period : (UINT32)phase;
}

static int dos_timer_irq_due(DOS_VM *vm) {
  U32 period = (vm->PitReload ? vm->PitReload : 65536u) * 100u;
  if (vm->DosTimerPhase < period) return 0;
  vm->DosTimerPhase -= period;
  return 1;
}

static int dos_dispatch_hardware(DOS_VM *vm) {
  if (!vm) return 0;
  dos_timer_update(vm);
  DosMousePoll(vm);
  DosSerialMousePoll(vm);
  if (DosMouseDispatchCallback(vm)) return 1;
  if (!(vm->Eflags & 0x200u) && vm->ProtectedMode)
    return 0;

  U8 timer_vector = (U8)(vm->PicMasterVectorBase + 0u);
  U8 keyboard_vector = (U8)(vm->PicMasterVectorBase + 1u);
  U8 serial2_vector = (U8)(vm->PicMasterVectorBase + 3u);
  U8 serial_vector = (U8)(vm->PicMasterVectorBase + 4u);
  if (vm->ProtectedMode) {
    if (vm->DpmiPmVectorSelector[timer_vector] &&
        !(vm->PicMasterMask & 0x01u) && dos_timer_irq_due(vm))
      return protected_interrupt(timer_vector, 0,
                                 vm->DpmiClient32 ? 32u : 16u);
    if (vm->DpmiPmVectorSelector[keyboard_vector] &&
        !(vm->PicMasterMask & 0x02u) && !vm->KeyboardDataReady) {
      UINT32 event;
      while (KbdReadMirroredInputEvent(&event)) {
        if (event & KBD_INPUT_CHARACTER) continue;
        vm->KeyboardData = (U8)(event & 0x7fu);
        if (event & 0x0100u) vm->KeyboardData |= 0x80u;
        vm->KeyboardDataReady = TRUE;
        return protected_interrupt(keyboard_vector, 0,
                                   vm->DpmiClient32 ? 32u : 16u);
      }
    }
    if (vm->DpmiPmVectorSelector[serial2_vector] &&
        !(vm->PicMasterMask & 0x08u) && DosSerialMouseIrqPending(vm, 0))
      return protected_interrupt(serial2_vector, 0,
                                 vm->DpmiClient32 ? 32u : 16u);
    if (vm->DpmiPmVectorSelector[serial_vector] &&
        !(vm->PicMasterMask & 0x10u) && DosSerialMouseIrqPending(vm, 1))
      return protected_interrupt(serial_vector, 0,
                                 vm->DpmiClient32 ? 32u : 16u);
  } else if (vm->Flags & 0x200u) {
    if (!(vm->PicMasterMask & 0x01u) &&
        (rw(0, timer_vector * 4u) || rw(0, timer_vector * 4u + 2u)) &&
        dos_timer_irq_due(vm)) {
      dos_int(timer_vector);
      return 1;
    }
    if (!(vm->PicMasterMask & 0x02u) &&
        (rw(0, keyboard_vector * 4u) || rw(0, keyboard_vector * 4u + 2u))) {
      UINT32 event;
      while (KbdReadMirroredInputEvent(&event)) {
        if (event & KBD_INPUT_CHARACTER) continue;
        vm->KeyboardData = (U8)(event & 0x7fu);
        if (event & 0x0100u) vm->KeyboardData |= 0x80u;
        vm->KeyboardDataReady = TRUE;
        dos_int(keyboard_vector);
        return 1;
      }
    }
    if (!(vm->PicMasterMask & 0x08u) && DosSerialMouseIrqPending(vm, 0) &&
        (rw(0, serial2_vector * 4u) || rw(0, serial2_vector * 4u + 2u))) {
      dos_int(serial2_vector);
      return 1;
    }
    if (!(vm->PicMasterMask & 0x10u) && DosSerialMouseIrqPending(vm, 1) &&
        (rw(0, serial_vector * 4u) || rw(0, serial_vector * 4u + 2u))) {
      dos_int(serial_vector);
      return 1;
    }
  }
  return 0;
}

static __attribute__((always_inline)) inline void step_protected(DOS_VM *vm) {
  if (vm->Segments[CS] == vm->DpmiHostSelector && vm->Eip == 0x010cu) {
    dpmi_raw_to_real();
    return;
  }
  U8 cs_flags = vm->SegmentCache[CS].Flags;
  vm->CurrentInstructionIp = vm->Eip;
  int operand32 = !!(cs_flags & 4u), address32 = !!(cs_flags & 4u);
  int segment_override = -1, rep = 0;
  U8 op = (U8)protected_fetch(vm, 1);
  for (unsigned count = 0; count < 15 && vm->Running; count++) {
    if (op == 0x26) segment_override = ES;
    else if (op == 0x2e) segment_override = CS;
    else if (op == 0x36) segment_override = SS;
    else if (op == 0x3e) segment_override = DS;
    else if (op == 0x64) segment_override = FS;
    else if (op == 0x65) segment_override = GS;
    else if (op == 0x66) operand32 = !operand32;
    else if (op == 0x67) address32 = !address32;
    else if (op == 0xf2 || op == 0xf3) rep = op;
    else if (op == 0xf0) { }
    else break;
    op = (U8)protected_fetch(vm, 1);
  }
  if (!vm->Running) return;
  vm->CurrentOpcode = op;
  unsigned width = operand32 ? 32 : 16;

  /* DOS/4G clients use IN/OUT for VGA, PIT, PIC and the 8042. Never execute
   * host port instructions: route every guest access through the virtual PC. */
  if (op == 0xe4 || op == 0xe5 || op == 0xec || op == 0xed) {
    U16 port = (op == 0xe4 || op == 0xe5)
                   ? (U16)protected_fetch(vm, 1) : (U16)pm_register(vm, DX, 16);
    unsigned io_width = (op == 0xe4 || op == 0xec) ? 8u : width;
    pm_set_register(vm, AX, io_width, DosPortIn(vm, port, io_width));
    return;
  }
  if (op == 0xe6 || op == 0xe7 || op == 0xee || op == 0xef) {
    U16 port = (op == 0xe6 || op == 0xe7)
                   ? (U16)protected_fetch(vm, 1) : (U16)pm_register(vm, DX, 16);
    unsigned io_width = (op == 0xe6 || op == 0xee) ? 8u : width;
    DosPortOut(vm, port, pm_register(vm, AX, io_width), io_width);
    return;
  }
  if (op >= 0x6c && op <= 0x6f) {
    unsigned io_width = (op & 1u) ? width : 8u;
    unsigned bytes = io_width / 8u;
    unsigned address_width = address32 ? 32u : 16u;
    U32 count = rep ? pm_register(vm, CX, address_width) : 1u;
    U32 si = pm_register(vm, SI, address_width);
    U32 di = pm_register(vm, DI, address_width);
    U16 port = (U16)pm_register(vm, DX, 16);
    unsigned source_segment = segment_override >= 0 ?
                              (unsigned)segment_override : DS;
    if (count > 0x100000u) {
      fail("DOSVM: excessive protected string I/O count\r\n");
      return;
    }
    for (U32 i = 0; i < count && vm->Running; i++) {
      if (op == 0x6c || op == 0x6d) {
        U32 value = DosPortIn(vm, port, io_width);
        if (!pm_write(vm, ES, di, io_width, value)) return;
      } else {
        U32 value = pm_read(vm, source_segment, si, io_width);
        if (!vm->Running) return;
        DosPortOut(vm, port, value, io_width);
      }
      INT32 delta = (vm->Eflags & DF) ? -(INT32)bytes : (INT32)bytes;
      if (op == 0x6c || op == 0x6d)
        di = address_width == 32 ? di + (U32)delta : (U16)(di + delta);
      else
        si = address_width == 32 ? si + (U32)delta : (U16)(si + delta);
    }
    if (op == 0x6c || op == 0x6d)
      pm_set_register(vm, DI, address_width, di);
    else
      pm_set_register(vm, SI, address_width, si);
    if (rep) pm_set_register(vm, CX, address_width, 0);
    return;
  }

  if (op == 0x06 || op == 0x0e || op == 0x16 || op == 0x1e) {
    unsigned index = op == 0x06 ? ES : op == 0x0e ? CS : op == 0x16 ? SS : DS;
    pm_push(vm, vm->Segments[index], width);
    return;
  }
  if (op == 0x07 || op == 0x17 || op == 0x1f) {
    unsigned index = op == 0x07 ? ES : op == 0x17 ? SS : DS;
    pm_load_segment(vm, index, (U16)pm_pop(vm, width));
    return;
  }
  if (op >= 0xb0 && op <= 0xb7) {
    pm_set_register(vm, op & 7, 8, protected_fetch(vm, 1));
    return;
  }
  if (op >= 0xb8 && op <= 0xbf) {
    U32 immediate = protected_fetch(vm, width / 8);
    pm_set_register(vm, op & 7, width, immediate);
    return;
  }
  if (op <= 0x3d && ((op & 7u) == 4u || (op & 7u) == 5u)) {
    unsigned alu_width = (op & 1u) ? width : 8u;
    unsigned operation = (op >> 3) & 7u;
    U32 immediate = protected_fetch(vm, alu_width / 8u);
    U32 result = pm_alu(vm, operation, pm_register(vm, AX, alu_width),
                        immediate, alu_width);
    if (operation != 7u) pm_set_register(vm, AX, alu_width, result);
    return;
  }
  if (op >= 0x50 && op <= 0x57) {
    pm_push(vm, pm_register(vm, op & 7, width), width);
    return;
  }
  if (op >= 0x58 && op <= 0x5f) {
    pm_set_register(vm, op & 7, width, pm_pop(vm, width));
    return;
  }
  if (op >= 0x40 && op <= 0x4f) {
    unsigned reg = op & 7;
    U32 old_cf = vm->Eflags & CF;
    U32 result = pm_alu(vm, op < 0x48 ? 0 : 5, pm_register(vm, reg, width), 1, width);
    vm->Eflags = (vm->Eflags & ~CF) | old_cf;
    pm_set_register(vm, reg, width, result);
    return;
  }
  if (op == 0x69 || op == 0x6b) {
    PM_MOD m = pm_decode(vm, segment_override, address32);
    U32 source = pm_read_rm(vm, m, width);
    INT64 immediate = op == 0x6b ? (INT8)protected_fetch(vm, 1)
                      : width == 32 ? (INT32)protected_fetch(vm, 4)
                                    : (INT16)protected_fetch(vm, 2);
    INT64 product = (width == 32 ? (INT64)(INT32)source
                                 : (INT64)(INT16)source) * immediate;
    pm_set_register(vm, m.reg, width, (U32)product);
    int overflow = width == 32 ? product != (INT64)(INT32)product
                               : product != (INT64)(INT16)product;
    vm->Eflags = (vm->Eflags & ~(CF | OF)) | (overflow ? CF | OF : 0);
    vm->Flags = (U16)vm->Eflags;
    return;
  }
  if (op == 0xfe) {
    PM_MOD m = pm_decode(vm, segment_override, address32);
    if (m.reg > 1u) { fail("DOSVM: invalid protected FE group\r\n"); return; }
    U32 old_cf = vm->Eflags & CF;
    U32 value = pm_alu(vm, m.reg ? 5u : 0u, pm_read_rm(vm, m, 8), 1, 8);
    vm->Eflags = (vm->Eflags & ~CF) | old_cf;
    pm_write_rm(vm, m, 8, value);
    return;
  }
  if (op == 0xf6 || op == 0xf7) {
    PM_MOD m = pm_decode(vm, segment_override, address32);
    unsigned operand_width = op == 0xf6 ? 8u : width;
    U32 value = pm_read_rm(vm, m, operand_width);
    if (m.reg <= 1u) {
      pm_alu(vm, 4, value, protected_fetch(vm, operand_width / 8u), operand_width);
    } else if (m.reg == 2u) {
      pm_write_rm(vm, m, operand_width, ~value);
    } else if (m.reg == 3u) {
      pm_write_rm(vm, m, operand_width, pm_alu(vm, 5, 0, value, operand_width));
    } else if (m.reg == 4u || m.reg == 5u) {
      U64 product;
      int overflow;
      if (m.reg == 4u) {
        product = (U64)pm_register(vm, AX, operand_width) * value;
        overflow = !!(product >> operand_width);
      } else {
        INT64 left = operand_width == 8 ? (INT8)vm->Registers[AX]
                   : operand_width == 16 ? (INT16)vm->Registers[AX] : (INT32)vm->Registers32[AX];
        INT64 right = operand_width == 8 ? (INT8)value
                    : operand_width == 16 ? (INT16)value : (INT32)value;
        INT64 signed_product = left * right;
        product = (U64)signed_product;
        overflow = operand_width == 8 ? signed_product < -128 || signed_product > 127
                 : operand_width == 16 ? signed_product < -32768 || signed_product > 32767
                 : signed_product < -2147483648LL || signed_product > 2147483647LL;
      }
      if (operand_width == 8)
        pm_set_register(vm, AX, 16, (U16)product);
      else {
        pm_set_register(vm, AX, operand_width, (U32)product);
        pm_set_register(vm, DX, operand_width, (U32)(product >> operand_width));
      }
      vm->Eflags = (vm->Eflags & ~(CF | OF)) | (overflow ? CF | OF : 0);
      vm->Flags = (U16)vm->Eflags;
    } else {
      if (!value) { fail("DOSVM: protected divide by zero\r\n"); return; }
      U64 dividend = operand_width == 8 ? vm->Registers[AX]
                   : operand_width == 16 ? ((U32)vm->Registers[DX] << 16) | vm->Registers[AX]
                   : ((U64)vm->Registers32[DX] << 32) | vm->Registers32[AX];
      U64 quotient, remainder;
      if (m.reg == 6u) {
        quotient = dividend / value;
        remainder = dividend % value;
        if (quotient > (operand_width == 8 ? 0xffu : operand_width == 16 ? 0xffffu : 0xffffffffu)) {
          fail("DOSVM: protected divide overflow\r\n"); return;
        }
      } else {
        INT64 numerator = operand_width == 8 ? (INT16)dividend
                        : operand_width == 16 ? (INT32)dividend : (INT64)dividend;
        INT64 divisor = operand_width == 8 ? (INT8)value
                      : operand_width == 16 ? (INT16)value : (INT32)value;
        if (numerator == (-9223372036854775807LL - 1LL) && divisor == -1) {
          fail("DOSVM: protected divide overflow\r\n"); return;
        }
        INT64 q = numerator / divisor, rem = numerator % divisor;
        if ((operand_width == 8 && (q < -128 || q > 127)) ||
            (operand_width == 16 && (q < -32768 || q > 32767)) ||
            (operand_width == 32 && (q < -2147483648LL || q > 2147483647LL))) {
          fail("DOSVM: protected divide overflow\r\n"); return;
        }
        quotient = (U64)q;
        remainder = (U64)rem;
      }
      if (operand_width == 8)
        pm_set_register(vm, AX, 16, (U16)(((U8)remainder << 8) | (U8)quotient));
      else {
        pm_set_register(vm, AX, operand_width, (U32)quotient);
        pm_set_register(vm, DX, operand_width, (U32)remainder);
      }
    }
    return;
  }
  if (op >= 0x70 && op <= 0x7f) {
    INT32 displacement = (INT8)protected_fetch(vm, 1);
    if (condition_flags(vm->Eflags, op & 15)) pm_set_near_ip(vm, vm->Eip + displacement, width);
    return;
  }
  if (op == 0x90) return;
  if (op >= 0x91 && op <= 0x97) {
    U32 old_accumulator = pm_register(vm, AX, width);
    U32 old_operand = pm_register(vm, op & 7u, width);
    pm_set_register(vm, AX, width, old_operand);
    pm_set_register(vm, op & 7u, width, old_accumulator);
    return;
  }
  if (op == 0x98) {
    pm_set_register(vm, AX, width, operand32 ? (U32)(INT32)(INT16)vm->Registers[AX]
                                           : (U16)(INT16)(INT8)vm->Registers[AX]);
    return;
  }
  if (op == 0x99) {
    pm_set_register(vm, DX, width, operand32 ? (vm->Registers32[AX] & 0x80000000u ? 0xffffffffu : 0u)
                                           : (vm->Registers[AX] & 0x8000u ? 0xffffu : 0u));
    return;
  }
  /* The DOS4G stub and many 16-bit runtimes issue these legacy x87
   * control instructions during startup.  DOSVM has no hardware FPU state;
   * the no-op controls below have no observable effect for this VM. */
  if (op == 0xdb) {
    PM_MOD m = pm_decode(vm, segment_override, address32);
    if (m.is_reg && m.reg == 4 && m.rm <= 4) return; /* FENI..FSETPM */
    KeLog("[dos] unsupported x87 DB /%u mod=%u rm=%u at CS=%04x EIP=%08x\n",
          m.reg, m.mod, m.rm, vm->Segments[CS], vm->Eip);
    fail("DOSVM: unsupported x87 instruction\r\n");
    return;
  }
  if (op == 0xdd) {
    PM_MOD m = pm_decode(vm, segment_override, address32);
    if (!m.is_reg && m.reg == 7) { /* FNSTSW m16 */
      pm_write_rm(vm, m, 16, 0);
      return;
    }
    KeLog("[dos] unsupported x87 DD /%u mod=%u rm=%u at CS=%04x EIP=%08x\n",
          m.reg, m.mod, m.rm, vm->Segments[CS], vm->Eip);
    fail("DOSVM: unsupported x87 instruction\r\n");
    return;
  }
  if (op == 0xd9) {
    PM_MOD m = pm_decode(vm, segment_override, address32);
    U16 control = vm->FpuControl ? vm->FpuControl : 0x037fu;
    if (!m.is_reg && m.reg == 5) { /* FLDCW m16 */
      vm->FpuControl = (U16)pm_read_rm(vm, m, 16);
      return;
    }
    if (!m.is_reg && m.reg == 7) { /* FNSTCW m16 */
      pm_write_rm(vm, m, 16, control);
      return;
    }
    KeLog("[dos] unsupported x87 D9 /%u mod=%u rm=%u at CS=%04x EIP=%08x\n",
          m.reg, m.mod, m.rm, vm->Segments[CS], vm->Eip);
    fail("DOSVM: unsupported x87 instruction\r\n");
    return;
  }
  if (op == 0x68 || op == 0x6a) {
    U32 value = op == 0x68 ? protected_fetch(vm, width / 8)
                           : (U32)(INT32)(INT8)protected_fetch(vm, 1);
    pm_push(vm, value, width);
    return;
  }
  if (op == 0x60) {
    U32 original_sp = operand32 ? vm->Registers32[SP] : vm->Registers[SP];
    if (!pm_push(vm, pm_register(vm, AX, width), width) ||
        !pm_push(vm, pm_register(vm, CX, width), width) ||
        !pm_push(vm, pm_register(vm, DX, width), width) ||
        !pm_push(vm, pm_register(vm, BX, width), width) ||
        !pm_push(vm, original_sp, width) ||
        !pm_push(vm, pm_register(vm, BP, width), width) ||
        !pm_push(vm, pm_register(vm, SI, width), width) ||
        !pm_push(vm, pm_register(vm, DI, width), width))
      return;
    return;
  }
  if (op == 0x61) {
    pm_set_register(vm, DI, width, pm_pop(vm, width)); pm_set_register(vm, SI, width, pm_pop(vm, width));
    pm_set_register(vm, BP, width, pm_pop(vm, width)); (void)pm_pop(vm, width);
    pm_set_register(vm, BX, width, pm_pop(vm, width)); pm_set_register(vm, DX, width, pm_pop(vm, width));
    pm_set_register(vm, CX, width, pm_pop(vm, width)); pm_set_register(vm, AX, width, pm_pop(vm, width));
    return;
  }
  if (op == 0x9c) { pm_push(vm, vm->Eflags | 2u, width); return; }
  if (op == 0x9d) {
    U32 value = pm_pop(vm, width);
    vm->Eflags = width == 16 ? ((vm->Eflags & 0xffff0000u) | (value & 0xffffu))
                            : value;
    vm->Eflags |= 2u;
    vm->Flags = (U16)vm->Eflags;
    return;
  }
  if (op >= 0xa0 && op <= 0xa3) {
    U32 offset = protected_fetch(vm, address32 ? 4 : 2);
    unsigned w = (op & 1u) ? width : 8;
    if (op == 0xa0 || op == 0xa1)
      pm_set_register(vm, AX, w, pm_read(vm, segment_override >= 0 ? segment_override : DS,
                                     offset, w));
    else
      pm_write(vm, segment_override >= 0 ? segment_override : DS, offset, w,
               pm_register(vm, AX, w));
    return;
  }
  if (op == 0xa8 || op == 0xa9) {
    unsigned w = op == 0xa8 ? 8 : width;
    pm_alu(vm, 4, pm_register(vm, AX, w), protected_fetch(vm, w / 8), w);
    return;
  }
  if (op >= 0xa4 && op <= 0xaf && op != 0xa8 && op != 0xa9) {
    unsigned element = (op & 1u) ? width / 8 : 1;
    U32 count = rep ? (address32 ? vm->Registers32[CX] : vm->Registers[CX]) : 1;
    U32 remaining = count > 4096u ? count - 4096u : 0;
    int comparison_stopped = 0;
    if (count > 4096u) count = 4096u;
    U32 si = address32 ? vm->Registers32[SI] : vm->Registers[SI], di = address32 ? vm->Registers32[DI] : vm->Registers[DI];
    if (rep && count && (op == 0xa4 || op == 0xa5 || op == 0xaa || op == 0xab)) {
      U32 source_linear = 0, destination_linear = 0, span_bytes = 0, destination_bytes = 0;
      int backwards = !!(vm->Eflags & DF);
      U32 chunk_count = count;
      if (chunk_count > 4096u) chunk_count = 4096u;
      int destination_ok = dos_string_span(vm, ES, di, chunk_count, element,
                                           address32, backwards, 1,
                                           &destination_linear, &destination_bytes);
      unsigned bulk_source_segment = segment_override >= 0 ?
                                     (unsigned)segment_override : DS;
      int source_ok = (op == 0xaa || op == 0xab) ||
                      dos_string_span(vm, bulk_source_segment, si, chunk_count, element,
                                      address32, backwards, 0,
                                      &source_linear, &span_bytes);
      if (destination_ok && source_ok &&
          ((op == 0xaa || op == 0xab) ||
           ((source_linear + span_bytes <= destination_linear ||
             destination_linear + destination_bytes <= source_linear) &&
            span_bytes == destination_bytes))) {
        if (op == 0xa4 || op == 0xa5)
          memcpy(vm->Memory + destination_linear, vm->Memory + source_linear,
                 span_bytes);
        else {
          U32 accumulator = pm_register(vm, AX, element * 8u);
          for (U32 offset = 0; offset < destination_bytes; offset += element)
            dos_guest_store(vm->Memory + destination_linear + offset, element,
                            accumulator);
        }
        INT32 delta = backwards ? -(INT32)(chunk_count * element)
                                : (INT32)(chunk_count * element);
        if (op == 0xa4 || op == 0xa5)
          pm_set_register(vm, SI, address32 ? 32 : 16, si + delta);
        pm_set_register(vm, DI, address32 ? 32 : 16, di + delta);
        pm_set_register(vm, CX, address32 ? 32 : 16, remaining);
        if (remaining) vm->Eip = vm->CurrentInstructionIp;
        return;
      }
    }
    while (count-- && vm->Running) {
      unsigned source_segment = segment_override >= 0 ? segment_override : DS;
      U32 a = 0, b = 0;
      if (op <= 0xa7 || (op >= 0xac && op <= 0xad))
        a = pm_read(vm, source_segment, si, element * 8);
      if (op == 0xa4 || op == 0xa5)
        pm_write(vm, ES, di, element * 8, a);
      else if (op == 0xa6 || op == 0xa7) {
        b = pm_read(vm, ES, di, element * 8);
        pm_alu(vm, 7, a, b, element * 8);
      } else if (op == 0xaa || op == 0xab)
        pm_write(vm, ES, di, element * 8, pm_register(vm, AX, element * 8));
      else if (op == 0xac || op == 0xad)
        pm_set_register(vm, AX, element * 8, a);
      else if (op == 0xae || op == 0xaf) {
        b = pm_read(vm, ES, di, element * 8);
        pm_alu(vm, 7, pm_register(vm, AX, element * 8), b, element * 8);
      }
      INT32 delta = (vm->Eflags & DF) ? -(INT32)element : (INT32)element;
      if (op <= 0xa7 || (op >= 0xac && op <= 0xad))
        si += delta;
      if (op <= 0xa7 || (op >= 0xaa && op <= 0xab) ||
          (op >= 0xae && op <= 0xaf))
        di += delta;
      pm_set_register(vm, SI, address32 ? 32 : 16, si);
      if (op <= 0xa7 || (op >= 0xaa && op <= 0xab) ||
          (op >= 0xae && op <= 0xaf))
        pm_set_register(vm, DI, address32 ? 32 : 16, di);
      if (rep) {
        pm_set_register(vm, CX, address32 ? 32 : 16,
                        (address32 ? vm->Registers32[CX] : vm->Registers[CX]) - 1);
        if ((op == 0xa6 || op == 0xa7 || op == 0xae || op == 0xaf) &&
            ((rep == 0xf3) != !!(vm->Eflags & ZF))) {
          comparison_stopped = 1;
          break;
        }
      }
      if (!address32) { si = (U16)si; di = (U16)di; }
    }
    if (vm->Running && rep && remaining && !comparison_stopped)
      vm->Eip = vm->CurrentInstructionIp;
    return;
  }
  if (op == 0x89 || op == 0x8b || op == 0x88 || op == 0x8a ||
      op == 0x84 || op == 0x85 || op == 0x86 || op == 0x87 ||
      op == 0x8d || op == 0xc6 || op == 0xc7 ||
      (op <= 0x3b && (op & 4u) == 0 && (op & 7u) <= 3) ||
      op == 0x80 || op == 0x81 || op == 0x82 || op == 0x83) {
    PM_MOD m = pm_decode(vm, segment_override, address32);
    if (op == 0x8d) {
      if (m.is_reg) fail("DOSVM: invalid protected LEA operand\r\n");
      else pm_set_register(vm, m.reg, width, m.offset);
      return;
    }
    unsigned w = width;
    if ((op <= 0x3b && !(op & 1u)) || op == 0x84 || op == 0x86 ||
        op == 0x88 || op == 0x8a || op == 0x80 || op == 0x82 || op == 0xc6)
      w = 8;
    U32 a = pm_read_rm(vm, m, w), b = pm_register(vm, m.reg, w), result;
    if (op == 0x89 || op == 0x88) pm_write_rm(vm, m, w, b);
    else if (op == 0x8b || op == 0x8a) pm_set_register(vm, m.reg, w, a);
    else if (op == 0x86 || op == 0x87) {
      pm_write_rm(vm, m, w, b); pm_set_register(vm, m.reg, w, a);
    } else if (op == 0xc6 || op == 0xc7) {
      if (m.reg) fail("DOSVM: invalid protected MOV group\r\n");
      else pm_write_rm(vm, m, w, protected_fetch(vm, w / 8));
    } else if (op >= 0x80 && op <= 0x83) {
      U32 immediate = op == 0x83 ? (U32)(INT32)(INT8)protected_fetch(vm, 1)
                       : (op == 0x80 || op == 0x82) ? protected_fetch(vm, 1)
                                                   : protected_fetch(vm, width / 8);
      result = pm_alu(vm, m.reg, a, immediate, w);
      if (m.reg != 7) pm_write_rm(vm, m, w, result);
    } else if (op == 0x84 || op == 0x85) {
      pm_alu(vm, 4, a, b, w);
    } else {
      unsigned operation = (op >> 3) & 7u;
      int direction = (op & 2) != 0;
      result = pm_alu(vm, operation, direction ? b : a, direction ? a : b, w);
      if (operation != 7) {
        if (direction) pm_set_register(vm, m.reg, w, result);
        else pm_write_rm(vm, m, w, result);
      }
    }
    return;
  }
  if (op == 0x8c || op == 0x8e) {
    PM_MOD m = pm_decode(vm, segment_override, address32);
    if (m.reg >= 6 || (op == 0x8e && m.reg == CS)) {
      fail("DOSVM: invalid protected segment register\r\n");
      return;
    }
    if (op == 0x8c) pm_write_rm(vm, m, 16, vm->Segments[m.reg]);
    else {
      U16 value = (U16)pm_read_rm(vm, m, 16);
      pm_load_segment(vm, m.reg, value);
    }
    return;
  }
  if (op == 0x8f) {
    PM_MOD m = pm_decode(vm, segment_override, address32);
    if (m.reg) fail("DOSVM: invalid protected POP operand\r\n");
    else pm_write_rm(vm, m, width, pm_pop(vm, width));
    return;
  }
  if (op == 0xc4 || op == 0xc5) {
    PM_MOD m = pm_decode(vm, segment_override, address32);
    if (m.is_reg) {
      fail("DOSVM: far pointer load requires memory\r\n");
      return;
    }
    U32 offset = pm_read(vm, m.segment, m.offset, width);
    U16 selector = (U16)pm_read(vm, m.segment, m.offset + width / 8u, 16);
    if (pm_load_segment(vm, op == 0xc4 ? ES : DS, selector))
      pm_set_register(vm, m.reg, width, offset);
    return;
  }
  if (op == 0xd0 || op == 0xd1 || op == 0xd2 || op == 0xd3 ||
      op == 0xc0 || op == 0xc1) {
    PM_MOD m = pm_decode(vm, segment_override, address32);
    unsigned w = (op & 1u) ? width : 8;
    U32 count = (op == 0xd0 || op == 0xd1) ? 1
                : (op == 0xd2 || op == 0xd3) ? (U8)vm->Registers32[CX]
                                               : (U8)protected_fetch(vm, 1);
    pm_write_rm(vm, m, w, pm_shift(vm, m.reg, pm_read_rm(vm, m, w), count, w));
    return;
  }
  if (op == 0xe8 || op == 0xe9) {
    INT32 displacement = width == 32 ? (INT32)protected_fetch(vm, 4)
                                     : (INT16)protected_fetch(vm, 2);
    if (op == 0xe8) pm_push(vm, vm->Eip, width);
    pm_set_near_ip(vm, vm->Eip + displacement, width);
    return;
  }
  if (op == 0xeb) {
    INT8 displacement = (INT8)protected_fetch(vm, 1);
    pm_set_near_ip(vm, vm->Eip + displacement,
                   (cs_flags & 4u) ? 32 : 16);
    return;
  }
  if (op >= 0xe0 && op <= 0xe3) {
    INT8 displacement = (INT8)protected_fetch(vm, 1);
    U32 count = address32 ? vm->Registers32[CX] : vm->Registers[CX];
    int branch;
    if (op == 0xe3) branch = count == 0;
    else {
      count = address32 ? count - 1u : (U16)(count - 1u);
      pm_set_register(vm, CX, address32 ? 32 : 16, count);
      branch = count != 0 && (op == 0xe2 ||
               (op == 0xe1 ? !!(vm->Eflags & ZF) : !(vm->Eflags & ZF)));
    }
    if (branch) pm_set_near_ip(vm, vm->Eip + displacement,
                               (cs_flags & 4u) ? 32 : 16);
    return;
  }
  if (op == 0xc8) {
    U16 size = (U16)protected_fetch(vm, 2);
    U8 nesting = (U8)protected_fetch(vm, 1) & 31u;
    U32 old = pm_register(vm, BP, width);
    if (!pm_push(vm, old, width)) return;
    U32 frame = pm_stack_pointer(vm);
    for (U8 i = 1; i < nesting; i++) {
      old -= width / 8u;
      if (!pm_push(vm, pm_read(vm, SS, old, width), width)) return;
    }
    if (nesting && !pm_push(vm, frame, width)) return;
    pm_set_register(vm, BP, width, frame);
    pm_set_stack_pointer(vm, pm_stack_pointer(vm) - size);
    return;
  }
  if (op == 0xc9) {
    pm_set_stack_pointer(vm, pm_register(vm, BP, width));
    pm_set_register(vm, BP, width, pm_pop(vm, width));
    return;
  }
  if (op == 0xc3 || op == 0xc2) {
    U32 target = pm_pop(vm, width);
    if (op == 0xc2) pm_set_stack_pointer(vm, pm_stack_pointer(vm) + (U16)protected_fetch(vm, 2));
    vm->Eip = target;
    return;
  }
  if (op == 0xcb || op == 0xca) {
    U32 target = pm_pop(vm, width);
    U16 selector = (U16)pm_pop(vm, width);
    if (op == 0xca) pm_set_stack_pointer(vm, pm_stack_pointer(vm) + (U16)protected_fetch(vm, 2));
    if (pm_load_segment(vm, CS, selector)) vm->Eip = target;
    return;
  }
  if (op == 0xcf) {
    U32 target = pm_pop(vm, width);
    U16 selector = (U16)pm_pop(vm, width);
    U32 new_flags = pm_pop(vm, width);
    U8 old_cpl = vm->Segments[CS] & 3u, new_cpl = selector & 3u;
    U32 new_sp = 0;
    U16 new_ss = 0;
    /* IRET to an outer privilege level has a five-field frame. DOS extenders
     * use this when returning from their ring-0 exception/interrupt stubs. */
    if (new_cpl > old_cpl) {
      new_sp = pm_pop(vm, width);
      new_ss = (U16)pm_pop(vm, width);
    }
    DOS_DESCRIPTOR code = {0};
    int code_valid = read_descriptor(selector, &code);
    if (!vm->Running) return;
    if (!code_valid || new_cpl < old_cpl || !(code.Access & 0x10u) || !(code.Access & 8u) ||
        (((code.Access & 4u) ? ((code.Access >> 5) & 3u) > new_cpl
                              : ((code.Access >> 5) & 3u) != new_cpl)) ||
        !(code.Access & 0x80u)) {
      KeLog("[dos] IRET reject selector=%04x target=%08x valid=%d access=%02x flags=%02x oldcpl=%u newcpl=%u\n",
            selector, target, code_valid, code.Access, code.Flags, old_cpl, new_cpl);
      fail("DOSVM: invalid protected IRET code selector\r\n");
      return;
    }
    /* Validate both descriptors before committing a privilege return. A
     * normal MOV CS validator checks against the old CPL and cannot do this. */
    if (new_cpl > old_cpl) {
      DOS_DESCRIPTOR stack = {0};
      if (!read_descriptor(new_ss, &stack) || !(stack.Access & 0x10u) ||
          (stack.Access & 8u) || !(stack.Access & 2u) ||
          ((stack.Access >> 5) & 3u) != new_cpl ||
          (new_ss & 3u) != new_cpl || !(stack.Access & 0x80u)) {
        fail("DOSVM: invalid protected IRET stack selector\r\n");
        return;
      }
      vm->Segments[CS] = selector; vm->SegmentCache[CS] = code;
      vm->Segments[SS] = new_ss; vm->SegmentCache[SS] = stack;
      pm_set_stack_pointer(vm, new_sp);
    } else if (!pm_load_segment(vm, CS, selector)) return;
    vm->Eip = target;
    vm->Eflags = width == 16 ? ((vm->Eflags & 0xffff0000u) | (new_flags & 0xffffu))
                            : new_flags;
    vm->Eflags |= 2u;
    vm->Flags = (U16)vm->Eflags;
    return;
  }
  if (op == 0x9a) {
    U32 target = protected_fetch(vm, width / 8);
    U16 selector = (U16)protected_fetch(vm, 2);
    if (pm_push(vm, vm->Segments[CS], width) && pm_push(vm, vm->Eip, width) &&
        pm_load_segment(vm, CS, selector))
      vm->Eip = target;
    return;
  }
  if (op == 0xff) {
    PM_MOD m = pm_decode(vm, segment_override, address32);
    U32 value = pm_read_rm(vm, m, width);
    if (m.reg == 0 || m.reg == 1) {
      U32 old_cf = vm->Eflags & CF;
      U32 result = pm_alu(vm, m.reg ? 5 : 0, value, 1, width);
      vm->Eflags = (vm->Eflags & ~CF) | old_cf;
      pm_write_rm(vm, m, width, result);
    } else if (m.reg == 2 || m.reg == 4) {
      if (m.reg == 2 && !pm_push(vm, vm->Eip, width)) return;
      vm->Eip = value;
    } else if ((m.reg == 3 || m.reg == 5) && !m.is_reg) {
      U16 selector = (U16)pm_read(vm, m.segment, m.offset + width / 8, 16);
      if (m.reg == 3 &&
          (!pm_push(vm, vm->Segments[CS], width) || !pm_push(vm, vm->Eip, width)))
        return;
      if (pm_load_segment(vm, CS, selector)) vm->Eip = value;
    } else if (m.reg == 6)
      pm_push(vm, value, width);
    else
      fail("DOSVM: invalid protected FF group operation\r\n");
    return;
  }
  if (op == 0xea) {
    U32 target = protected_fetch(vm, width / 8);
    U16 selector = (U16)protected_fetch(vm, 2);
    if (pm_load_segment(vm, CS, selector)) vm->Eip = target;
    return;
  }
  if (op == 0xfa || op == 0xfb) {
    /* DPMI clients manipulate a virtual interrupt flag at ring 3. */
    if (op == 0xfa) vm->Eflags &= ~0x200u;
    else {
      if (!(vm->Eflags & 0x200u)) vm->InterruptShadow = 1;
      vm->Eflags |= 0x200u;
    }
    vm->Flags = (U16)vm->Eflags;
    return;
  }
  if (op == 0xf8 || op == 0xf9 || op == 0xf5 || op == 0xfc || op == 0xfd) {
    if (op == 0xf8) vm->Eflags &= ~CF;
    else if (op == 0xf9) vm->Eflags |= CF;
    else if (op == 0xf5) vm->Eflags ^= CF;
    else if (op == 0xfc) vm->Eflags &= ~DF;
    else vm->Eflags |= DF;
    vm->Flags = (U16)vm->Eflags;
    return;
  }
  if (op == 0x0f) {
    U8 second = (U8)protected_fetch(vm, 1);
    if (second == 0xa0 || second == 0xa1 ||
        second == 0xa8 || second == 0xa9) {
      unsigned segment = (second & 8u) ? GS : FS;
      if (second & 1u) {
        U16 selector = (U16)pm_pop(vm, width);
        if (vm->Running) pm_load_segment(vm, segment, selector);
      } else pm_push(vm, vm->Segments[segment], width);
      return;
    }
    if (second == 0xb2 || second == 0xb4 || second == 0xb5) {
      PM_MOD m = pm_decode(vm, segment_override, address32);
      if (m.is_reg) {
        fail("DOSVM: far segment load requires memory\r\n");
        return;
      }
      U32 offset = pm_read(vm, m.segment, m.offset, width);
      U16 selector = (U16)pm_read(vm, m.segment, m.offset + width / 8u, 16);
      unsigned index = second == 0xb2 ? SS : second == 0xb4 ? FS : GS;
      if (pm_load_segment(vm, index, selector)) {
        pm_set_register(vm, m.reg, width, offset);
        /* LSS suppresses maskable interrupts through the following
         * instruction; the virtual IRQ dispatcher observes IF separately. */
      }
      return;
    }
    if (second == 0x02 || second == 0x03) {
      PM_MOD m = pm_decode(vm, segment_override, address32);
      U16 selector = (U16)pm_read_rm(vm, m, 16);
      DOS_DESCRIPTOR descriptor;
      U8 cpl = vm->Segments[CS] & 3u, rpl = selector & 3u;
      if (read_descriptor(selector, &descriptor) &&
          (descriptor.Access & 0x10u) &&
          ((descriptor.Access >> 5) & 3u) >= (cpl > rpl ? cpl : rpl)) {
        U32 result = second == 0x02
                     ? ((U32)descriptor.Access << 8) |
                       ((U32)(descriptor.Flags & 0x0fu) << 20)
                     : descriptor.Limit;
        pm_set_register(vm, m.reg, width, result);
        vm->Eflags |= ZF;
      } else vm->Eflags &= ~ZF;
      vm->Flags = (U16)vm->Eflags;
      return;
    }
    if (second >= 0x80 && second <= 0x8f) {
      INT32 displacement = width == 32 ? (INT32)protected_fetch(vm, 4)
                                       : (INT16)protected_fetch(vm, 2);
      if (condition_flags(vm->Eflags, second & 15)) pm_set_near_ip(vm, vm->Eip + displacement, width);
      return;
    }
    if (second == 0x31) { /* RDTSC: deterministic virtual instruction counter */
      U64 counter = (U64)vm->Steps * 1000ULL + vm->Eip;
      pm_set_register(vm, AX, 32, (U32)counter);
      pm_set_register(vm, DX, 32, (U32)(counter >> 32));
      return;
    }
    if (second >= 0x90 && second <= 0x9f) { /* SETcc r/m8 */
      PM_MOD m = pm_decode(vm, segment_override, address32);
      pm_write_rm(vm, m, 8, condition_flags(vm->Eflags, second & 15u) ? 1u : 0u);
      return;
    }
    if (second == 0xa4 || second == 0xa5 ||
        second == 0xac || second == 0xad) { /* SHLD/SHRD */
      PM_MOD m = pm_decode(vm, segment_override, address32);
      U32 destination = pm_read_rm(vm, m, width);
      U32 source = pm_register(vm, m.reg, width);
      U32 count = (second == 0xa4 || second == 0xac)
                    ? protected_fetch(vm, 1) : (U8)vm->Registers32[CX];
      count &= 0x1fu;
      if (!count) return;
      if (count > width) count = width; /* Intel leaves larger counts undefined. */
      U32 mask = width == 16 ? 0xffffu : 0xffffffffu;
      U32 sign = width == 16 ? 0x8000u : 0x80000000u;
      U32 result;
      BOOLEAN carry;
      if (second == 0xa4 || second == 0xa5) {
        carry = !!(destination & (1u << (width - count)));
        result = ((destination << count) |
                  (source >> (width - count))) & mask;
      } else {
        carry = !!(destination & (1u << (count - 1u)));
        result = (destination >> count) |
                  ((source << (width - count)) & mask);
      }
      vm->Eflags &= ~(CF | ZF | SF | 4u | OF);
      if (carry) vm->Eflags |= CF;
      if (!result) vm->Eflags |= ZF;
      if (result & sign) vm->Eflags |= SF;
      if (parity((U8)result)) vm->Eflags |= 4u;
      if (count == 1u) {
        BOOLEAN old_sign = !!(destination & sign);
        BOOLEAN new_sign = !!(result & sign);
        if ((second == 0xa4 || second == 0xa5) ? (new_sign != carry)
                                                : (old_sign != new_sign))
          vm->Eflags |= OF;
      }
      vm->Flags = (U16)vm->Eflags;
      pm_write_rm(vm, m, width, result);
      return;
    }
    if (second == 0xb0 || second == 0xb1) { /* CMPXCHG r/m,reg */
      PM_MOD m = pm_decode(vm, segment_override, address32);
      unsigned w = second == 0xb0 ? 8u : width;
      U32 accumulator = pm_register(vm, AX, w);
      U32 destination = pm_read_rm(vm, m, w);
      (void)pm_alu(vm, 5, accumulator, destination, w);
      if (vm->Eflags & ZF)
        pm_write_rm(vm, m, w, pm_register(vm, m.reg, w));
      else
        pm_set_register(vm, AX, w, destination);
      return;
    }
    if (second == 0xc0 || second == 0xc1) { /* XADD r/m,reg */
      PM_MOD m = pm_decode(vm, segment_override, address32);
      unsigned w = second == 0xc0 ? 8u : width;
      U32 destination = pm_read_rm(vm, m, w);
      U32 source = pm_register(vm, m.reg, w);
      U32 result = pm_alu(vm, 0, destination, source, w);
      pm_write_rm(vm, m, w, result);
      pm_set_register(vm, m.reg, w, destination);
      return;
    }
    if (second >= 0xc8 && second <= 0xcf) { /* BSWAP r32 */
      unsigned reg = second & 7u;
      U32 value = vm->Registers32[reg];
      value = ((value & 0x000000ffu) << 24) |
              ((value & 0x0000ff00u) << 8)  |
              ((value & 0x00ff0000u) >> 8)  |
              ((value & 0xff000000u) >> 24);
      vm->Registers32[reg] = value;
      vm->Registers[reg] = (U16)value;
      return;
    }
    if (second == 0xa2) {
      U32 leaf = vm->Registers32[AX];
      vm->Registers32[AX] = leaf ? 0 : 1;
      vm->Registers32[BX] = 0x756e6547u; /* GenuineIntel */
      vm->Registers32[DX] = 0x49656e69u;
      vm->Registers32[CX] = 0x6c65746eu;
      if (!leaf) { vm->Registers32[AX] = 1; vm->Registers32[CX] = 0; vm->Registers32[DX] = 0; }
      for (unsigned i = 0; i < 8; i++) vm->Registers[i] = (U16)vm->Registers32[i];
      return;
    }
    if (second == 0xb6 || second == 0xb7 || second == 0xbe || second == 0xbf) {
      PM_MOD m = pm_decode(vm, segment_override, address32);
      unsigned source_width = (second == 0xb6 || second == 0xbe) ? 8 : 16;
      U32 value = pm_read_rm(vm, m, source_width);
      if (second == 0xbe)
        value = (U32)(INT32)(INT8)value;
      else if (second == 0xbf)
        value = (U32)(INT32)(INT16)value;
      pm_set_register(vm, m.reg, width, value);
      return;
    }
    if (second == 0xaf) {
      PM_MOD m = pm_decode(vm, segment_override, address32);
      INT64 product = (width == 32
                           ? (INT64)(INT32)vm->Registers32[m.reg] * (INT32)pm_read_rm(vm, m, width)
                           : (INT32)(INT16)vm->Registers32[m.reg] * (INT16)pm_read_rm(vm, m, width));
      U32 result = (U32)product;
      pm_set_register(vm, m.reg, width, result);
      U32 upper = width == 32 ? (U32)(product >> 32)
                              : (U16)(product >> 16);
      U32 sign_extension = (result & (width == 32 ? 0x80000000u : 0x8000u))
                               ? (width == 32 ? 0xffffffffu : 0xffffu) : 0;
      vm->Eflags &= ~(CF | OF);
      if (upper != sign_extension) vm->Eflags |= CF | OF;
      vm->Flags = (U16)vm->Eflags;
      return;
    }
    if (second == 0xa3 || second == 0xab || second == 0xb3 || second == 0xbb) {
      PM_MOD m = pm_decode(vm, segment_override, address32);
      INT32 bit_index = (INT32)pm_register(vm, m.reg, width);
      U32 bit = (U32)bit_index & (width - 1), value;
      if (!m.is_reg) {
        INT32 bits_per_word = (INT32)width;
        INT32 word_index = bit_index / bits_per_word;
        if (bit_index < 0 && bit_index % bits_per_word) word_index--;
        m.offset += (U32)(word_index * (INT32)(width / 8));
        bit = (U32)(bit_index - word_index * bits_per_word);
      }
      value = pm_read_rm(vm, m, width);
      vm->Eflags = (vm->Eflags & ~CF) | ((value & (1u << bit)) ? CF : 0);
      if (second != 0xa3) {
        if (second == 0xab) value |= 1u << bit;
        else if (second == 0xb3) value &= ~(1u << bit);
        else value ^= 1u << bit;
        pm_write_rm(vm, m, width, value);
      }
      vm->Flags = (U16)vm->Eflags;
      return;
    }
    if (second == 0xba) {
      PM_MOD m = pm_decode(vm, segment_override, address32);
      U8 operation = m.reg;
      U32 bit_index = protected_fetch(vm, 1) & (width - 1);
      U32 value = pm_read_rm(vm, m, width);
      vm->Eflags = (vm->Eflags & ~CF) | ((value & (1u << bit_index)) ? CF : 0);
      if (operation >= 5) {
        if (operation == 5) value |= 1u << bit_index;
        else if (operation == 6) value &= ~(1u << bit_index);
        else value ^= 1u << bit_index;
        pm_write_rm(vm, m, width, value);
      } else if (operation != 4)
        fail("DOSVM: invalid protected bit-test group\r\n");
      vm->Flags = (U16)vm->Eflags;
      return;
    }
    if (second == 0x01) {
      PM_MOD m = pm_decode(vm, segment_override, address32);
      if ((vm->Segments[CS] & 3u) && m.reg >= 2) {
        fail("DOSVM: privileged descriptor instruction fault\r\n"); return;
      }
      if (m.reg == 0 || m.reg == 1) {
        DOS_TABLE_REGISTER *tr = m.reg ? &vm->Idtr : &vm->Gdtr;
        U8 bytes[6] = {(U8)tr->Limit, (U8)(tr->Limit >> 8),
                       (U8)tr->Base, (U8)(tr->Base >> 8),
                       (U8)(tr->Base >> 16), (U8)(tr->Base >> 24)};
        for (unsigned i = 0; i < 6; i++)
          if (!pm_write(vm, m.segment, m.offset + i, 8, bytes[i])) return;
      } else if (m.reg == 2 || m.reg == 3) {
        DOS_TABLE_REGISTER *tr = m.reg == 2 ? &vm->Gdtr : &vm->Idtr;
        tr->Limit = (U16)pm_read(vm, m.segment, m.offset, 16);
        tr->Base = pm_read(vm, m.segment, m.offset + 2, 32);
      } else if (m.reg == 4) {
        pm_write_rm(vm, m, 16, (U16)vm->Cr0);
      } else if (m.reg == 6) {
        U32 next_cr0 = vm->Cr0 | (pm_read_rm(vm, m, 16) & 0x0fu);
        enter_protected_mode(next_cr0);
      } else fail("DOSVM: unsupported protected 0F 01 operation\r\n");
      return;
    }
    if (second == 0x00) {
      PM_MOD m = pm_decode(vm, segment_override, address32);
      if (m.reg == 0) pm_write_rm(vm, m, 16, vm->Ldtr);
      else if (m.reg == 1) pm_write_rm(vm, m, 16, vm->Tr);
      else if (m.reg == 2) {
        if (vm->Segments[CS] & 3u) { fail("DOSVM: privileged LLDT fault\r\n"); return; }
        if (!load_ldt((U16)pm_read_rm(vm, m, 16)))
          fail("DOSVM: invalid LDT selector\r\n");
      }
      else if (m.reg == 3) {
        if (vm->Segments[CS] & 3u) { fail("DOSVM: privileged LTR fault\r\n"); return; }
        U16 selector = (U16)pm_read_rm(vm, m, 16);
        DOS_DESCRIPTOR d;
        if ((selector & 7u) || !read_descriptor(selector, &d) ||
            ((d.Access & 0x1fu) != 0x01u && (d.Access & 0x1fu) != 0x09u))
          fail("DOSVM: invalid task-register selector\r\n");
        else vm->Tr = selector;
      }
      else if (m.reg == 4 || m.reg == 5) {
        U16 selector = (U16)pm_read_rm(vm, m, 16);
        DOS_DESCRIPTOR d;
        int valid = read_descriptor(selector, &d) && (d.Access & 0x10u) &&
                    (m.reg == 4 ? (d.Access & 2u) :
                     ((d.Access & 8u) && (d.Access & 2u)));
        if (m.reg == 4) vm->Eflags = (vm->Eflags & ~ZF) | (valid ? ZF : 0);
        else vm->Eflags = (vm->Eflags & ~ZF) | (valid ? ZF : 0);
        vm->Flags = (U16)vm->Eflags;
      }
      else fail("DOSVM: unsupported 0F 00 operation\r\n");
      return;
    }
    if (second == 0x20 || second == 0x22) {
      U8 modrm = (U8)protected_fetch(vm, 1);
      unsigned cr = (modrm >> 3) & 7, reg = modrm & 7;
      if ((modrm >> 6) != 3 || cr != 0 || (vm->Segments[CS] & 3u)) {
        fail("DOSVM: invalid control-register operation\r\n"); return;
      }
      if (second == 0x20) pm_set_register(vm, reg, 32, vm->Cr0);
      else enter_protected_mode(vm->Registers32[reg]);
      return;
    }
    KeLog("[dos] unsupported protected 0F %02x at CS=%04x EIP=%08x\n",
          second, vm->Segments[CS], vm->Eip - 2u);
    fail("DOSVM: unsupported protected extended instruction\r\n");
    return;
  }
  if (op == 0xcd || op == 0xcc || op == 0xce) {
    U8 vector = op == 0xcc ? 3 : op == 0xce ? 4 : (U8)protected_fetch(vm, 1);
    if (op != 0xce || (vm->Eflags & OF))
      protected_interrupt(vector, 1, width);
    return;
  }
  U32 code_base = vm->SegmentCache[CS].Base;
  U32 code_offset = vm->Eip ? vm->Eip - 1u : 0;
  U32 code_linear = code_base <= 0xffffffffu - code_offset ?
                    code_base + code_offset : 0xffffffffu;
  U8 b0 = 0xffu, b1 = 0xffu, b2 = 0xffu, b3 = 0xffu;
  if (code_linear <= MEM_SIZE - 4u) {
    b0 = vm->Memory[code_linear];
    b1 = vm->Memory[code_linear + 1u];
    b2 = vm->Memory[code_linear + 2u];
    b3 = vm->Memory[code_linear + 3u];
  }
  KeLog("[dos] unsupported protected opcode 0x%02x at CS=%04x EIP=%08x BASE=%08x LINEAR=%08x next=%02x %02x %02x %02x\n",
        op, vm->Segments[CS], vm->Eip - 1u, code_base, code_linear,
        b0, b1, b2, b3);
  fail("DOSVM: unsupported protected-mode instruction\r\n");
}
static void push(U16 v) {
  r[SP] -= 2;
  ww(seg[SS], r[SP], v);
}
static void push32(U32 v) {
  r[SP] -= 4;
  write32(seg[SS], r[SP], v);
}
static U32 pop32(void) {
  U32 v = read32(seg[SS], r[SP]);
  r[SP] += 4;
  return v;
}
static U16 pop(void) {
  U16 v = rw(seg[SS], r[SP]);
  r[SP] += 2;
  return v;
}
static U8 parity(U8 v) {
  v ^= v >> 4;
  v ^= v >> 2;
  v ^= v >> 1;
  return !(v & 1);
}
/* 8086 decimal-adjust instructions used by older Windows/DOS runtimes.
 * DAA/DAS update the arithmetic status flags from the adjusted AL value;
 * AAA/AAS only define AF and CF and leave the other flags unchanged. */
static U16 decimal_adjust(U8 opcode, U16 value, U16 *flags_ptr) {
  U8 old_al = (U8)value;
  U8 al = old_al;
  U16 f = *flags_ptr;
  int adjust_low = ((old_al & 0x0fu) > 9u) || !!(f & AF);
  int adjust_high = (old_al > 0x99u) || !!(f & CF);

  if (opcode == 0x27u || opcode == 0x2fu) { /* DAA / DAS */
    if (opcode == 0x27u) {
      if (adjust_low) al = (U8)(al + 0x06u);
      if (adjust_high) al = (U8)(al + 0x60u);
    } else {
      if (adjust_low) al = (U8)(al - 0x06u);
      if (adjust_high) al = (U8)(al - 0x60u);
    }
    f &= (U16)~(CF | AF | ZF | SF | 4u);
    if (adjust_low) f |= AF;
    if (adjust_high) f |= CF;
    if (!al) f |= ZF;
    if (al & 0x80u) f |= SF;
    if (parity(al)) f |= 4u;
    *flags_ptr = f;
    return (U16)((value & 0xff00u) | al);
  }

  /* AAA/AAS adjust the unpacked BCD digit in AX.  Intel leaves SZP and OF
   * undefined, so deliberately preserve them instead of inventing values. */
  if (adjust_low) {
    value = opcode == 0x37u ? (U16)(value + 0x0106u)
                             : (U16)(value - 0x0106u);
    f |= CF | AF;
  } else {
    f &= (U16)~(CF | AF);
  }
  value = (U16)((value & 0xff00u) | ((U8)value & 0x0fu));
  *flags_ptr = f;
  return value;
}

static U16 alu(unsigned op, U16 a, U16 b, unsigned width) {
  U32 mask = width == 8 ? 0xffu : 0xffffu, sign = width == 8 ? 0x80u : 0x8000u;
  U32 result = 0, carry = 0;
  U16 f = flags & ~(CF | ZF | SF | OF | AF | 4u);
  if (op == 0 || op == 2) {
    carry = (op == 2 && (flags & CF)) ? 1 : 0;
    result = (a & mask) + (b & mask) + carry;
    if ((~(a ^ b) & (a ^ result) & sign))
      f |= OF;
  } else if (op == 5 || op == 7 || op == 3) {
    carry = (op == 3 && (flags & CF)) ? 1 : 0;
    result = (a & mask) - (b & mask) - carry;
    if (((a ^ b) & (a ^ result) & sign))
      f |= OF;
  } else if (op == 4)
    result = (a & mask) & (b & mask);
  else if (op == 1)
    result = (a & mask) | (b & mask);
  else if (op == 6)
    result = (a & mask) ^ (b & mask);
  if ((op == 0 || op == 2 || op == 3 || op == 5 || op == 7) && (result & ~mask))
    f |= CF;
  if ((op == 0 || op == 2 || op == 3 || op == 5 || op == 7) &&
      ((a ^ b ^ result) & 0x10))
    f |= AF;
  result &= mask;
  if (!result)
    f |= ZF;
  if (result & sign)
    f |= SF;
  if (parity((U8)result))
    f |= 4;
  flags = f;
  return (U16)result;
}
static U32 alu32(unsigned op, U32 a, U32 b) {
  U64 wide = 0;
  U32 result = 0, f = flags & ~(CF | ZF | SF | OF | AF | 4u);
  if (op == 0 || op == 2) {
    wide = (U64)a + b + (op == 2 && (flags & CF) ? 1u : 0u);
    result = (U32)wide;
    if (wide >> 32) f |= CF;
    if ((~(a ^ b) & (a ^ result) & 0x80000000u)) f |= OF;
    if ((a ^ b ^ result) & 0x10u) f |= AF;
  } else if (op == 5 || op == 7 || op == 3) {
    U32 borrow = op == 3 && (flags & CF) ? 1u : 0u;
    result = a - b - borrow;
    if ((U64)a < (U64)b + borrow) f |= CF;
    if (((a ^ b) & (a ^ result) & 0x80000000u)) f |= OF;
    if ((a ^ b ^ result) & 0x10u) f |= AF;
  } else if (op == 4) result = a & b;
  else if (op == 1) result = a | b;
  else result = a ^ b;
  if (!result) f |= ZF;
  if (result & 0x80000000u) f |= SF;
  if (parity((U8)result)) f |= 4u;
  flags = f;
  return result;
}

typedef struct {
  U8 mod, reg, rm;
  U32 off;
  U16 segment;
} MOD;
static MOD decode(int override, int address32) {
  MOD m;
  U8 x = fetch8();
  m.mod = x >> 6;
  m.reg = (x >> 3) & 7;
  m.rm = x & 7;
  if (address32) {
    if (m.mod == 3) {
      m.off = 0;
      m.segment = seg[override >= 0 ? override : DS];
      return m;
    }
    U32 base = 0, index = 0;
    int has_base = 1, uses_stack = 0;
    if (m.rm == 4) {
      U8 sib = fetch8();
      unsigned scale = sib >> 6, index_reg = (sib >> 3) & 7u;
      unsigned base_reg = sib & 7u;
      if (index_reg != 4u) index = r32[index_reg] << scale;
      if (m.mod == 0 && base_reg == 5u) has_base = 0;
      else {
        base = r32[base_reg];
        uses_stack = base_reg == SP || base_reg == BP;
      }
    } else if (m.mod == 0 && m.rm == 5u) {
      has_base = 0;
    } else {
      base = r32[m.rm];
      uses_stack = m.rm == SP || m.rm == BP;
    }
    if (!has_base) base = fetch32();
    if (m.mod == 1) base += (U32)(INT32)(signed char)fetch8();
    else if (m.mod == 2) base += fetch32();
    m.off = base + index;
    m.segment = seg[override >= 0 ? override : (uses_stack ? SS : DS)];
    return m;
  }
  U16 base = 0;
  int uses_bp = 0;
  switch (m.rm) {
  case 0:
    base = r[BX] + r[SI];
    break;
  case 1:
    base = r[BX] + r[DI];
    break;
  case 2:
    base = r[BP] + r[SI];
    uses_bp = 1;
    break;
  case 3:
    base = r[BP] + r[DI];
    uses_bp = 1;
    break;
  case 4:
    base = r[SI];
    break;
  case 5:
    base = r[DI];
    break;
  case 6:
    if (!m.mod)
      base = fetch16();
    else {
      base = r[BP];
      uses_bp = 1;
    }
    break;
  default:
    base = r[BX];
    break;
  }
  if (m.mod == 1)
    base += (U16)(signed char)fetch8();
  if (m.mod == 2)
    base += fetch16();
  m.off = base;
  m.segment = seg[override >= 0 ? override : (uses_bp ? SS : DS)];
  return m;
}
static U16 read_rm(MOD m, int wide) {
  return m.mod == 3 ? (wide ? r[m.rm] : get8(m.rm))
                    : (wide ? rw(m.segment, m.off) : rb(m.segment, m.off));
}
static void write_rm(MOD m, int wide, U16 v) {
  if (m.mod == 3) {
    if (wide)
      r[m.rm] = v;
    else
      set8(m.rm, (U8)v);
  } else if (wide)
    ww(m.segment, m.off, v);
  else
    wb(m.segment, m.off, (U8)v);
}
/*
 * Windows 1.x uses the 8087 instruction set even on machines where the host
 * kernel is built with -mgeneral-regs-only.  The kernel scheduler already
 * preserves the current thread's x87 state with FXSAVE/FXRSTOR, so execute
 * guest x87 operations on the host FPU while translating all guest memory
 * operands through the DOS segment helpers below.  Guest exception masks are
 * retained in FpuControl, but host exceptions are always masked so a guest
 * #MF can never escape into the kernel.
 */
static void x87_apply_control(DOS_VM *vm) {
  U16 host_control = (U16)(vm->FpuControl | 0x003fu);
  __asm__ volatile("fldcw %0" : : "m"(host_control) : "memory");
}

static void x87_capture_status(DOS_VM *vm) {
  __asm__ volatile("fnstsw %0" : "=m"(vm->FpuStatus) : : "memory");
}

static void x87_initialize(DOS_VM *vm) {
  __asm__ volatile("fninit" : : : "memory");
  vm->FpuControl = 0x037fu;
  vm->FpuStatus = 0;
  vm->FpuInitialized = TRUE;
  x87_apply_control(vm);
}

static MOD decode_x87(int override, int address32) {
  if (!address32) return decode(override, 0);

  MOD m = {0};
  U8 modrm = fetch8();
  m.mod = modrm >> 6;
  m.reg = (modrm >> 3) & 7u;
  m.rm = modrm & 7u;
  if (m.mod == 3) return m;

  U32 base = 0, index = 0;
  int has_base = 1, uses_bp = 0;
  if (m.rm == 4) {
    U8 sib = fetch8();
    unsigned scale = sib >> 6;
    unsigned index_reg = (sib >> 3) & 7u;
    unsigned base_reg = sib & 7u;
    if (index_reg != 4u) index = r32[index_reg] << scale;
    if (m.mod == 0 && base_reg == 5u) has_base = 0;
    else {
      base = r32[base_reg];
      uses_bp = base_reg == BP || base_reg == SP;
    }
  } else if (m.mod == 0 && m.rm == 5) {
    has_base = 0;
  } else {
    base = r32[m.rm];
    uses_bp = m.rm == BP || m.rm == SP;
  }

  if (!has_base) base = fetch32();
  if (m.mod == 1) base += (U32)(INT32)(INT8)fetch8();
  else if (m.mod == 2) base += fetch32();
  U32 offset = base + index;
  m.off = offset;
  m.segment = seg[override >= 0 ? override : (uses_bp ? SS : DS)];
  return m;
}

static void x87_read_bytes(MOD m, U8 *out, U32 size) {
  for (U32 i = 0; i < size; ++i)
    out[i] = rb(m.segment, (U16)(m.off + i));
}

static void x87_write_bytes(MOD m, const U8 *in, U32 size) {
  for (U32 i = 0; i < size; ++i)
    wb(m.segment, (U16)(m.off + i), in[i]);
}

#define X87_ST2(mnemonic, n) \
  __asm__ volatile(mnemonic " %%st(" #n "), %%st(0)" : : : "memory")
#define X87_ST1(mnemonic, n) \
  __asm__ volatile(mnemonic " %%st(" #n ")" : : : "memory")
#define X87_ST_LOAD(n) \
  __asm__ volatile("fld %%st(" #n ")" : : : "memory")
#define X87_ST_EXCHANGE(n) \
  __asm__ volatile("fxch %%st(" #n ")" : : : "memory")

static void x87_d8_register(U8 operation, U8 index) {
#define X87_REG_CASE(mnemonic, n) case n: X87_ST2(mnemonic, n); break
#define X87_CMP_CASE(mnemonic, n) case n: X87_ST1(mnemonic, n); break
  switch (operation) {
  case 0:
    switch (index) {
      X87_REG_CASE("fadd", 0); X87_REG_CASE("fadd", 1);
      X87_REG_CASE("fadd", 2); X87_REG_CASE("fadd", 3);
      X87_REG_CASE("fadd", 4); X87_REG_CASE("fadd", 5);
      X87_REG_CASE("fadd", 6); X87_REG_CASE("fadd", 7);
    }
    break;
  case 1:
    switch (index) {
      X87_REG_CASE("fmul", 0); X87_REG_CASE("fmul", 1);
      X87_REG_CASE("fmul", 2); X87_REG_CASE("fmul", 3);
      X87_REG_CASE("fmul", 4); X87_REG_CASE("fmul", 5);
      X87_REG_CASE("fmul", 6); X87_REG_CASE("fmul", 7);
    }
    break;
  case 2:
    switch (index) {
      X87_CMP_CASE("fcom", 0); X87_CMP_CASE("fcom", 1);
      X87_CMP_CASE("fcom", 2); X87_CMP_CASE("fcom", 3);
      X87_CMP_CASE("fcom", 4); X87_CMP_CASE("fcom", 5);
      X87_CMP_CASE("fcom", 6); X87_CMP_CASE("fcom", 7);
    }
    break;
  case 3:
    switch (index) {
      X87_CMP_CASE("fcomp", 0); X87_CMP_CASE("fcomp", 1);
      X87_CMP_CASE("fcomp", 2); X87_CMP_CASE("fcomp", 3);
      X87_CMP_CASE("fcomp", 4); X87_CMP_CASE("fcomp", 5);
      X87_CMP_CASE("fcomp", 6); X87_CMP_CASE("fcomp", 7);
    }
    break;
  case 4:
    switch (index) {
      X87_REG_CASE("fsub", 0); X87_REG_CASE("fsub", 1);
      X87_REG_CASE("fsub", 2); X87_REG_CASE("fsub", 3);
      X87_REG_CASE("fsub", 4); X87_REG_CASE("fsub", 5);
      X87_REG_CASE("fsub", 6); X87_REG_CASE("fsub", 7);
    }
    break;
  case 5:
    switch (index) {
      X87_REG_CASE("fsubr", 0); X87_REG_CASE("fsubr", 1);
      X87_REG_CASE("fsubr", 2); X87_REG_CASE("fsubr", 3);
      X87_REG_CASE("fsubr", 4); X87_REG_CASE("fsubr", 5);
      X87_REG_CASE("fsubr", 6); X87_REG_CASE("fsubr", 7);
    }
    break;
  case 6:
    switch (index) {
      X87_REG_CASE("fdiv", 0); X87_REG_CASE("fdiv", 1);
      X87_REG_CASE("fdiv", 2); X87_REG_CASE("fdiv", 3);
      X87_REG_CASE("fdiv", 4); X87_REG_CASE("fdiv", 5);
      X87_REG_CASE("fdiv", 6); X87_REG_CASE("fdiv", 7);
    }
    break;
  case 7:
    switch (index) {
      X87_REG_CASE("fdivr", 0); X87_REG_CASE("fdivr", 1);
      X87_REG_CASE("fdivr", 2); X87_REG_CASE("fdivr", 3);
      X87_REG_CASE("fdivr", 4); X87_REG_CASE("fdivr", 5);
      X87_REG_CASE("fdivr", 6); X87_REG_CASE("fdivr", 7);
    }
    break;
  }
#undef X87_REG_CASE
#undef X87_CMP_CASE
}

static void x87_integer_arithmetic(U8 opcode, U8 operation, U32 raw,
                                   unsigned width) {
  if (opcode == 0xda) {
    switch (operation) {
    case 0: __asm__ volatile("fiaddl %0" : : "m"(raw) : "memory"); break;
    case 1: __asm__ volatile("fimull %0" : : "m"(raw) : "memory"); break;
    case 2: __asm__ volatile("ficoml %0" : : "m"(raw) : "memory"); break;
    case 3: __asm__ volatile("ficompl %0" : : "m"(raw) : "memory"); break;
    case 4: __asm__ volatile("fisubl %0" : : "m"(raw) : "memory"); break;
    case 5: __asm__ volatile("fisubrl %0" : : "m"(raw) : "memory"); break;
    case 6: __asm__ volatile("fidivl %0" : : "m"(raw) : "memory"); break;
    case 7: __asm__ volatile("fidivrl %0" : : "m"(raw) : "memory"); break;
    }
  } else {
    U16 raw16 = (U16)raw;
    switch (operation) {
    case 0: __asm__ volatile("fiadds %0" : : "m"(raw16) : "memory"); break;
    case 1: __asm__ volatile("fimuls %0" : : "m"(raw16) : "memory"); break;
    case 2: __asm__ volatile("ficoms %0" : : "m"(raw16) : "memory"); break;
    case 3: __asm__ volatile("ficomps %0" : : "m"(raw16) : "memory"); break;
    case 4: __asm__ volatile("fisubs %0" : : "m"(raw16) : "memory"); break;
    case 5: __asm__ volatile("fisubrs %0" : : "m"(raw16) : "memory"); break;
    case 6: __asm__ volatile("fidivs %0" : : "m"(raw16) : "memory"); break;
    case 7: __asm__ volatile("fidivrs %0" : : "m"(raw16) : "memory"); break;
    }
  }
  (void)width;
}

/*
 * The guest is a 16-bit DOS program. In 16-bit operand-size mode its x87
 * environment is 14 bytes and its FSAVE image is 94 bytes. The 64-bit host
 * stores the corresponding legacy environment in 28 bytes and FSAVE image
 * in 108 bytes. Translate the headers explicitly; the 80-byte x87 register
 * stack has the same packed 10-byte-per-register representation.
 */
static void x87_env16_to_host28(const U8 *guest, U8 *host) {
  memset(host, 0, 28);
  host[0] = guest[0]; host[1] = guest[1];
  host[4] = guest[2]; host[5] = guest[3];
  host[8] = guest[4]; host[9] = guest[5];
  host[12] = guest[6]; host[13] = guest[7];
  host[16] = guest[8]; host[17] = guest[9];
  host[20] = guest[10]; host[21] = guest[11];
  host[24] = guest[12]; host[25] = guest[13];
}

static void x87_env28_to_guest16(const U8 *host, U8 *guest) {
  guest[0] = host[0]; guest[1] = host[1];
  guest[2] = host[4]; guest[3] = host[5];
  guest[4] = host[8]; guest[5] = host[9];
  guest[6] = host[12]; guest[7] = host[13];
  guest[8] = host[16]; guest[9] = host[17];
  guest[10] = host[20]; guest[11] = host[21];
  guest[12] = host[24]; guest[13] = host[25];
}

static void x87_state16_to_host108(const U8 *guest, U8 *host) {
  x87_env16_to_host28(guest, host);
  memcpy(host + 28, guest + 14, 80);
}

static void x87_state108_to_guest94(const U8 *host, U8 *guest) {
  x87_env28_to_guest16(host, guest);
  memcpy(guest + 14, host + 28, 80);
}

static int x87_real(DOS_VM *vm, U8 opcode, int override, int address32,
                    int operand32, U16 instruction_cs, U16 instruction_ip) {
  if (!vm->FpuInitialized) x87_initialize(vm);
  x87_apply_control(vm);
  MOD m = decode_x87(override, address32);
  if (!vm->Running) return 1;
  if (!vm->FpuTraceLogged) {
    KeLog("[dos-x87] first x87 op=%02x modrm=%02x at %04x:%04x SS:SP=%04x:%04x BP=%04x AX=%04x flags=%04x\n",
          opcode, (U8)((m.mod << 6) | (m.reg << 3) | m.rm),
          instruction_cs, instruction_ip, vm->Segments[SS],
          vm->Registers[SP], vm->Registers[BP], vm->Registers[AX], vm->Flags);
    vm->FpuTraceLogged = TRUE;
  }
  U32 raw32 = 0;
  U64 raw64 = 0;
  U16 raw16 = 0;
  U8 temp[108];
  switch (opcode) {
  case 0xd8: /* Real arithmetic and compare, m32real or ST(i). */
    if (m.mod == 3) {
      x87_d8_register(m.reg, m.rm);
    } else {
      raw32 = read32(m.segment, m.off);
      switch (m.reg) {
      case 0: __asm__ volatile("fadds %0" : : "m"(raw32) : "memory"); break;
      case 1: __asm__ volatile("fmuls %0" : : "m"(raw32) : "memory"); break;
      case 2: __asm__ volatile("fcoms %0" : : "m"(raw32) : "memory"); break;
      case 3: __asm__ volatile("fcomps %0" : : "m"(raw32) : "memory"); break;
      case 4: __asm__ volatile("fsubs %0" : : "m"(raw32) : "memory"); break;
      case 5: __asm__ volatile("fsubrs %0" : : "m"(raw32) : "memory"); break;
      case 6: __asm__ volatile("fdivs %0" : : "m"(raw32) : "memory"); break;
      case 7: __asm__ volatile("fdivrs %0" : : "m"(raw32) : "memory"); break;
      }
    }
    break;

  case 0xd9:
    if (m.mod == 3) {
      if (m.reg == 0) {
        switch (m.rm) {
        case 0: X87_ST_LOAD(0); break; case 1: X87_ST_LOAD(1); break;
        case 2: X87_ST_LOAD(2); break; case 3: X87_ST_LOAD(3); break;
        case 4: X87_ST_LOAD(4); break; case 5: X87_ST_LOAD(5); break;
        case 6: X87_ST_LOAD(6); break; case 7: X87_ST_LOAD(7); break;
        }
      } else if (m.reg == 1) {
        switch (m.rm) {
        case 0: X87_ST_EXCHANGE(0); break; case 1: X87_ST_EXCHANGE(1); break;
        case 2: X87_ST_EXCHANGE(2); break; case 3: X87_ST_EXCHANGE(3); break;
        case 4: X87_ST_EXCHANGE(4); break; case 5: X87_ST_EXCHANGE(5); break;
        case 6: X87_ST_EXCHANGE(6); break; case 7: X87_ST_EXCHANGE(7); break;
        }
      } else if (m.reg == 2 && m.rm == 0) {
        __asm__ volatile("fnop" : : : "memory");
      } else if (m.reg == 4) {
        switch (m.rm) {
        case 0: __asm__ volatile("fchs" : : : "memory"); break;
        case 1: __asm__ volatile("fabs" : : : "memory"); break;
        case 4: __asm__ volatile("ftst" : : : "memory"); break;
        case 5: __asm__ volatile("fxam" : : : "memory"); break;
        case 8: __asm__ volatile("fld1" : : : "memory"); break;
        default: break;
        }
      } else if (m.reg == 5) {
        switch (m.rm) {
        case 0: __asm__ volatile("fld1" : : : "memory"); break;
        case 1: __asm__ volatile("fldl2t" : : : "memory"); break;
        case 2: __asm__ volatile("fldl2e" : : : "memory"); break;
        case 3: __asm__ volatile("fldpi" : : : "memory"); break;
        case 4: __asm__ volatile("fldlg2" : : : "memory"); break;
        case 5: __asm__ volatile("fldln2" : : : "memory"); break;
        case 6: __asm__ volatile("fldz" : : : "memory"); break;
        default: break;
        }
      } else if (m.reg == 6) {
        switch (m.rm) {
        case 0: __asm__ volatile("f2xm1" : : : "memory"); break;
        case 1: __asm__ volatile("fyl2x" : : : "memory"); break;
        case 2: __asm__ volatile("fptan" : : : "memory"); break;
        case 3: __asm__ volatile("fpatan" : : : "memory"); break;
        case 4: __asm__ volatile("fxtract" : : : "memory"); break;
        case 5: __asm__ volatile("fprem1" : : : "memory"); break;
        case 6: __asm__ volatile("fdecstp" : : : "memory"); break;
        case 7: __asm__ volatile("fincstp" : : : "memory"); break;
        }
      } else if (m.reg == 7) {
        switch (m.rm) {
        case 0: __asm__ volatile("fprem" : : : "memory"); break;
        case 1: __asm__ volatile("fyl2xp1" : : : "memory"); break;
        case 2: __asm__ volatile("fsqrt" : : : "memory"); break;
        case 3: __asm__ volatile("fsincos" : : : "memory"); break;
        case 4: __asm__ volatile("frndint" : : : "memory"); break;
        case 5: __asm__ volatile("fscale" : : : "memory"); break;
        case 6: __asm__ volatile("fsin" : : : "memory"); break;
        case 7: __asm__ volatile("fcos" : : : "memory"); break;
        }
      } else {
        KeLog("[dos] unsupported x87 D9 /%u rm=%u at %04x:%04x\n",
              m.reg, m.rm, instruction_cs, instruction_ip);
        fail("DOSVM: unsupported x87 D9 instruction\r\n");
      }
    } else if (m.reg == 0) {
      raw32 = read32(m.segment, m.off);
      __asm__ volatile("flds %0" : : "m"(raw32) : "memory");
    } else if (m.reg == 2 || m.reg == 3) {
      __asm__ volatile("fsts %0" : "=m"(raw32) : : "memory");
      write32(m.segment, m.off, raw32);
      if (m.reg == 3) __asm__ volatile("fstp %%st(0)" : : : "memory");
    } else if (m.reg == 4 || m.reg == 6) {
      U32 env_size = operand32 ? 28u : 14u;
      if (m.reg == 4) {
        if (operand32) {
          x87_read_bytes(m, temp, 28);
        } else {
          U8 guest_env[14];
          x87_read_bytes(m, guest_env, 14);
          x87_env16_to_host28(guest_env, temp);
        }
        vm->FpuControl = word(temp);
        temp[0] = (U8)(vm->FpuControl | 0x3fu);
        temp[1] = (U8)((vm->FpuControl | 0x3fu) >> 8);
        __asm__ volatile("fldenv %0" : : "m"(*(U8 (*)[28])temp) : "memory");
        x87_apply_control(vm);
      } else {
        __asm__ volatile("fnstenv %0" : "=m"(*(U8 (*)[28])temp) : : "memory");
        if (operand32) {
          temp[0] = (U8)vm->FpuControl;
          temp[1] = (U8)(vm->FpuControl >> 8);
          x87_write_bytes(m, temp, env_size);
        } else {
          U8 guest_env[14];
          temp[0] = (U8)vm->FpuControl;
          temp[1] = (U8)(vm->FpuControl >> 8);
          x87_env28_to_guest16(temp, guest_env);
          x87_write_bytes(m, guest_env, 14);
        }
        x87_apply_control(vm);
      }
    } else if (m.reg == 1) {
      /* DOSBox's FPU_ESC1_EA leaves this reserved memory group inert. */
    } else if (m.reg == 5) {
      vm->FpuControl = rw(m.segment, m.off);
      x87_apply_control(vm);
    } else if (m.reg == 7) {
      raw16 = vm->FpuControl;
      ww(m.segment, m.off, raw16);
    } else {
      KeLog("[dos] unsupported x87 D9 /%u mod=%u rm=%u at %04x:%04x\n",
            m.reg, m.mod, m.rm, instruction_cs, instruction_ip);
      fail("DOSVM: unsupported x87 D9 instruction\r\n");
    }
    break;

  case 0xda:
  case 0xde:
    if (m.mod == 3) {
      /* FCMOV variants use the guest's arithmetic flags. Copy ST(i) to ST0
       * only when the condition is true; FLD + FSTP keeps the stack depth. */
      int move = 0;
      if (opcode == 0xda) {
        if (m.reg == 0) move = !!(vm->Flags & CF);
        else if (m.reg == 1) move = !!(vm->Flags & ZF);
        else if (m.reg == 2) move = !!(vm->Flags & (CF | ZF));
        else if (m.reg == 3) move = !!(vm->Flags & 4u);
        else if (m.reg == 4) move = !(vm->Flags & CF);
        else if (m.reg == 5) move = !(vm->Flags & ZF);
        else if (m.reg == 6) move = !(vm->Flags & (CF | ZF));
        else move = !(vm->Flags & 4u);
        if (move) {
          switch (m.rm) {
          case 0: __asm__ volatile("fld %%st(0); fstp %%st(1)" : : : "memory"); break;
          case 1: __asm__ volatile("fld %%st(1); fstp %%st(1)" : : : "memory"); break;
          case 2: __asm__ volatile("fld %%st(2); fstp %%st(1)" : : : "memory"); break;
          case 3: __asm__ volatile("fld %%st(3); fstp %%st(1)" : : : "memory"); break;
          case 4: __asm__ volatile("fld %%st(4); fstp %%st(1)" : : : "memory"); break;
          case 5: __asm__ volatile("fld %%st(5); fstp %%st(1)" : : : "memory"); break;
          case 6: __asm__ volatile("fld %%st(6); fstp %%st(1)" : : : "memory"); break;
          case 7: __asm__ volatile("fld %%st(7); fstp %%st(1)" : : : "memory"); break;
          }
        }
      } else {
        /* DE D9 (ModR/M /3,rm=1) is the special FCOMPP encoding.
         * DE C9 is FMULP ST(1),ST(0), handled by /1 below. */
        if (m.reg == 3 && m.rm == 1) __asm__ volatile("fcompp" : : : "memory");
        else {
          switch (m.reg) {
          case 0: switch (m.rm) {
            case 0: __asm__ volatile("faddp %%st(0), %%st(0)" : : : "memory"); break;
            case 1: __asm__ volatile("faddp %%st(0), %%st(1)" : : : "memory"); break;
            case 2: __asm__ volatile("faddp %%st(0), %%st(2)" : : : "memory"); break;
            case 3: __asm__ volatile("faddp %%st(0), %%st(3)" : : : "memory"); break;
            case 4: __asm__ volatile("faddp %%st(0), %%st(4)" : : : "memory"); break;
            case 5: __asm__ volatile("faddp %%st(0), %%st(5)" : : : "memory"); break;
            case 6: __asm__ volatile("faddp %%st(0), %%st(6)" : : : "memory"); break;
            case 7: __asm__ volatile("faddp %%st(0), %%st(7)" : : : "memory"); break;
          } break;
          case 1: switch (m.rm) {
            case 0: __asm__ volatile("fmulp %%st(0), %%st(0)" : : : "memory"); break;
            case 1: __asm__ volatile("fmulp %%st(0), %%st(1)" : : : "memory"); break;
            case 2: __asm__ volatile("fmulp %%st(0), %%st(2)" : : : "memory"); break;
            case 3: __asm__ volatile("fmulp %%st(0), %%st(3)" : : : "memory"); break;
            case 4: __asm__ volatile("fmulp %%st(0), %%st(4)" : : : "memory"); break;
            case 5: __asm__ volatile("fmulp %%st(0), %%st(5)" : : : "memory"); break;
            case 6: __asm__ volatile("fmulp %%st(0), %%st(6)" : : : "memory"); break;
            case 7: __asm__ volatile("fmulp %%st(0), %%st(7)" : : : "memory"); break;
          } break;
          case 4: case 5: case 6: case 7:
            /* Common DE pop arithmetic is used by Windows' FPU dispatch. */
            if (m.reg == 4) {
              switch (m.rm) {
              case 0: __asm__ volatile("fsubrp %%st(0), %%st(0)" : : : "memory"); break;
              case 1: __asm__ volatile("fsubrp %%st(0), %%st(1)" : : : "memory"); break;
              case 2: __asm__ volatile("fsubrp %%st(0), %%st(2)" : : : "memory"); break;
              case 3: __asm__ volatile("fsubrp %%st(0), %%st(3)" : : : "memory"); break;
              case 4: __asm__ volatile("fsubrp %%st(0), %%st(4)" : : : "memory"); break;
              case 5: __asm__ volatile("fsubrp %%st(0), %%st(5)" : : : "memory"); break;
              case 6: __asm__ volatile("fsubrp %%st(0), %%st(6)" : : : "memory"); break;
              case 7: __asm__ volatile("fsubrp %%st(0), %%st(7)" : : : "memory"); break;
              }
            } else if (m.reg == 5) {
              switch (m.rm) {
              case 0: __asm__ volatile("fsubp %%st(0), %%st(0)" : : : "memory"); break;
              case 1: __asm__ volatile("fsubp %%st(0), %%st(1)" : : : "memory"); break;
              case 2: __asm__ volatile("fsubp %%st(0), %%st(2)" : : : "memory"); break;
              case 3: __asm__ volatile("fsubp %%st(0), %%st(3)" : : : "memory"); break;
              case 4: __asm__ volatile("fsubp %%st(0), %%st(4)" : : : "memory"); break;
              case 5: __asm__ volatile("fsubp %%st(0), %%st(5)" : : : "memory"); break;
              case 6: __asm__ volatile("fsubp %%st(0), %%st(6)" : : : "memory"); break;
              case 7: __asm__ volatile("fsubp %%st(0), %%st(7)" : : : "memory"); break;
              }
            } else if (m.reg == 6) {
              switch (m.rm) {
              case 0: __asm__ volatile("fdivrp %%st(0), %%st(0)" : : : "memory"); break;
              case 1: __asm__ volatile("fdivrp %%st(0), %%st(1)" : : : "memory"); break;
              case 2: __asm__ volatile("fdivrp %%st(0), %%st(2)" : : : "memory"); break;
              case 3: __asm__ volatile("fdivrp %%st(0), %%st(3)" : : : "memory"); break;
              case 4: __asm__ volatile("fdivrp %%st(0), %%st(4)" : : : "memory"); break;
              case 5: __asm__ volatile("fdivrp %%st(0), %%st(5)" : : : "memory"); break;
              case 6: __asm__ volatile("fdivrp %%st(0), %%st(6)" : : : "memory"); break;
              case 7: __asm__ volatile("fdivrp %%st(0), %%st(7)" : : : "memory"); break;
              }
            } else {
              switch (m.rm) {
              case 0: __asm__ volatile("fdivp %%st(0), %%st(0)" : : : "memory"); break;
              case 1: __asm__ volatile("fdivp %%st(0), %%st(1)" : : : "memory"); break;
              case 2: __asm__ volatile("fdivp %%st(0), %%st(2)" : : : "memory"); break;
              case 3: __asm__ volatile("fdivp %%st(0), %%st(3)" : : : "memory"); break;
              case 4: __asm__ volatile("fdivp %%st(0), %%st(4)" : : : "memory"); break;
              case 5: __asm__ volatile("fdivp %%st(0), %%st(5)" : : : "memory"); break;
              case 6: __asm__ volatile("fdivp %%st(0), %%st(6)" : : : "memory"); break;
              case 7: __asm__ volatile("fdivp %%st(0), %%st(7)" : : : "memory"); break;
              }
            }
            break;
          default:
            KeLog("[dos] unsupported x87 DE register /%u rm=%u at %04x:%04x\n",
                  m.reg, m.rm, instruction_cs, instruction_ip);
            fail("DOSVM: unsupported x87 DE register instruction\r\n");
            break;
          }
        }
      }
    } else {
      if (opcode == 0xda) raw32 = read32(m.segment, m.off);
      else raw16 = rw(m.segment, m.off);
      x87_integer_arithmetic(opcode, m.reg, opcode == 0xda ? raw32 : raw16,
                             opcode == 0xda ? 32u : 16u);
    }
    break;

  case 0xdb:
    if (m.mod == 3) {
      if (m.reg <= 3) { /* FCMOVNB/NE/NBE/NU ST0,ST(i). */
        int move = m.reg == 0 ? !(vm->Flags & CF) :
                   m.reg == 1 ? !(vm->Flags & ZF) :
                   m.reg == 2 ? !(vm->Flags & (CF | ZF)) :
                                !(vm->Flags & 4u);
        if (move) {
          switch (m.rm) {
          case 0: __asm__ volatile("fld %%st(0); fstp %%st(1)" : : : "memory"); break;
          case 1: __asm__ volatile("fld %%st(1); fstp %%st(1)" : : : "memory"); break;
          case 2: __asm__ volatile("fld %%st(2); fstp %%st(1)" : : : "memory"); break;
          case 3: __asm__ volatile("fld %%st(3); fstp %%st(1)" : : : "memory"); break;
          case 4: __asm__ volatile("fld %%st(4); fstp %%st(1)" : : : "memory"); break;
          case 5: __asm__ volatile("fld %%st(5); fstp %%st(1)" : : : "memory"); break;
          case 6: __asm__ volatile("fld %%st(6); fstp %%st(1)" : : : "memory"); break;
          case 7: __asm__ volatile("fld %%st(7); fstp %%st(1)" : : : "memory"); break;
          }
        }
      } else if (m.reg == 4 && m.rm == 2) { /* FNCLEX */
        __asm__ volatile("fnclex" : : : "memory");
      } else if (m.reg == 4 && m.rm == 3) { /* FNINIT */
        __asm__ volatile("fninit" : : : "memory");
        vm->FpuControl = 0x037fu;
      } else if (m.reg == 4 && (m.rm == 0 || m.rm == 1 || m.rm == 4)) {
        /* FENI, FDISI and FSETPM are obsolete mode controls on 80387+. */
      } else if (m.reg == 5 || m.reg == 6) {
        /* FUCOMI/FCOMI ST(0),ST(i): DB E8+i / DB F0+i (Pentium Pro).
         * These write the integer condition flags directly, unlike FUCOM.
         * DOSBox's FPU interpreter implements the same status-to-EFLAGS
         * mapping; use the host x87 and merge only CF, PF and ZF. */
        U64 host_flags;
        switch (m.rm) {
        case 0: __asm__ volatile("fucomi %%st(0), %%st(0); pushfq; popq %0" : "=r"(host_flags) : : "cc", "memory"); break;
        case 1: __asm__ volatile("fucomi %%st(1), %%st(0); pushfq; popq %0" : "=r"(host_flags) : : "cc", "memory"); break;
        case 2: __asm__ volatile("fucomi %%st(2), %%st(0); pushfq; popq %0" : "=r"(host_flags) : : "cc", "memory"); break;
        case 3: __asm__ volatile("fucomi %%st(3), %%st(0); pushfq; popq %0" : "=r"(host_flags) : : "cc", "memory"); break;
        case 4: __asm__ volatile("fucomi %%st(4), %%st(0); pushfq; popq %0" : "=r"(host_flags) : : "cc", "memory"); break;
        case 5: __asm__ volatile("fucomi %%st(5), %%st(0); pushfq; popq %0" : "=r"(host_flags) : : "cc", "memory"); break;
        case 6: __asm__ volatile("fucomi %%st(6), %%st(0); pushfq; popq %0" : "=r"(host_flags) : : "cc", "memory"); break;
        default: __asm__ volatile("fucomi %%st(7), %%st(0); pushfq; popq %0" : "=r"(host_flags) : : "cc", "memory"); break;
        }
        if (m.reg == 6) {
          /* FCOMI raises the invalid-operation exception for any NaN. */
          /* The emulator's default masked-exception mode leaves the same
           * unordered condition codes as FUCOMI. */
        }
        vm->Flags = (vm->Flags & ~(CF | 0x04u | ZF)) |
                    ((U32)host_flags & (CF | 0x04u | ZF));
      } else {
        KeLog("[dos] unsupported x87 DB /%u rm=%u at %04x:%04x\n",
              m.reg, m.rm, instruction_cs, instruction_ip);
        fail("DOSVM: unsupported x87 DB instruction\r\n");
      }
    } else if (m.reg == 0 || m.reg == 1 || m.reg == 2 || m.reg == 3) {
      if (m.reg == 0) {
        raw32 = read32(m.segment, m.off);
        __asm__ volatile("fildl %0" : : "m"(raw32) : "memory");
      } else if (m.reg == 1) {
        U16 old = (U16)(vm->FpuControl | 0x003fu);
        U16 trunc = (U16)((old & ~0x0c00u) | 0x0c00u);
        __asm__ volatile("fldcw %0" : : "m"(trunc) : "memory");
        __asm__ volatile("fistpl %0" : "=m"(raw32) : : "memory");
        __asm__ volatile("fldcw %0" : : "m"(old) : "memory");
        write32(m.segment, m.off, raw32);
      } else if (m.reg == 2) {
        __asm__ volatile("fistl %0" : "=m"(raw32) : : "memory");
        write32(m.segment, m.off, raw32);
      } else {
        __asm__ volatile("fistpl %0" : "=m"(raw32) : : "memory");
        write32(m.segment, m.off, raw32);
      }
    } else if (m.reg == 5 || m.reg == 7) {
      if (m.reg == 5) {
        x87_read_bytes(m, temp, 10);
        __asm__ volatile("fldt %0" : : "m"(*(U8 (*)[10])temp) : "memory");
      } else {
        __asm__ volatile("fstpt %0" : "=m"(*(U8 (*)[10])temp) : : "memory");
        x87_write_bytes(m, temp, 10);
      }
    } else if (m.reg == 4 || m.reg == 6) {
      if (m.reg == 4) {
        x87_read_bytes(m, temp, 10);
        __asm__ volatile("fbld %0" : : "m"(*(U8 (*)[10])temp) : "memory");
      } else {
        __asm__ volatile("fbstp %0" : "=m"(*(U8 (*)[10])temp) : : "memory");
        x87_write_bytes(m, temp, 10);
      }
    }
    break;

  case 0xdc:
    if (m.mod == 3) {
      /* DC register forms write ST(i), unlike the D8 forms which write ST0. */
      switch (m.reg) {
#define X87_DC_CASE(mn, n) case n: __asm__ volatile(mn " %%st(0), %%st(" #n ")" : : : "memory"); break
      case 0: switch (m.rm) { X87_DC_CASE("fadd",0); X87_DC_CASE("fadd",1); X87_DC_CASE("fadd",2); X87_DC_CASE("fadd",3); X87_DC_CASE("fadd",4); X87_DC_CASE("fadd",5); X87_DC_CASE("fadd",6); X87_DC_CASE("fadd",7); } break;
      case 1: switch (m.rm) { X87_DC_CASE("fmul",0); X87_DC_CASE("fmul",1); X87_DC_CASE("fmul",2); X87_DC_CASE("fmul",3); X87_DC_CASE("fmul",4); X87_DC_CASE("fmul",5); X87_DC_CASE("fmul",6); X87_DC_CASE("fmul",7); } break;
      case 4: switch (m.rm) { X87_DC_CASE("fsubr",0); X87_DC_CASE("fsubr",1); X87_DC_CASE("fsubr",2); X87_DC_CASE("fsubr",3); X87_DC_CASE("fsubr",4); X87_DC_CASE("fsubr",5); X87_DC_CASE("fsubr",6); X87_DC_CASE("fsubr",7); } break;
      case 5: switch (m.rm) { X87_DC_CASE("fsub",0); X87_DC_CASE("fsub",1); X87_DC_CASE("fsub",2); X87_DC_CASE("fsub",3); X87_DC_CASE("fsub",4); X87_DC_CASE("fsub",5); X87_DC_CASE("fsub",6); X87_DC_CASE("fsub",7); } break;
      case 6: switch (m.rm) { X87_DC_CASE("fdivr",0); X87_DC_CASE("fdivr",1); X87_DC_CASE("fdivr",2); X87_DC_CASE("fdivr",3); X87_DC_CASE("fdivr",4); X87_DC_CASE("fdivr",5); X87_DC_CASE("fdivr",6); X87_DC_CASE("fdivr",7); } break;
      case 7: switch (m.rm) { X87_DC_CASE("fdiv",0); X87_DC_CASE("fdiv",1); X87_DC_CASE("fdiv",2); X87_DC_CASE("fdiv",3); X87_DC_CASE("fdiv",4); X87_DC_CASE("fdiv",5); X87_DC_CASE("fdiv",6); X87_DC_CASE("fdiv",7); } break;
      default:
        KeLog("[dos] unsupported x87 DC register /%u rm=%u at %04x:%04x\n", m.reg, m.rm, instruction_cs, instruction_ip);
        fail("DOSVM: unsupported x87 DC register instruction\r\n");
        break;
      }
#undef X87_DC_CASE
    } else {
      raw64 = 0;
      for (U32 i = 0; i < 8; ++i) raw64 |= (U64)rb(m.segment, (U16)(m.off + i)) << (i * 8u);
      switch (m.reg) {
      case 0: __asm__ volatile("faddl %0" : : "m"(raw64) : "memory"); break;
      case 1: __asm__ volatile("fmull %0" : : "m"(raw64) : "memory"); break;
      case 2: __asm__ volatile("fcoml %0" : : "m"(raw64) : "memory"); break;
      case 3: __asm__ volatile("fcompl %0" : : "m"(raw64) : "memory"); break;
      case 4: __asm__ volatile("fsubl %0" : : "m"(raw64) : "memory"); break;
      case 5: __asm__ volatile("fsubrl %0" : : "m"(raw64) : "memory"); break;
      case 6: __asm__ volatile("fdivl %0" : : "m"(raw64) : "memory"); break;
      case 7: __asm__ volatile("fdivrl %0" : : "m"(raw64) : "memory"); break;
      }
    }
    break;

  case 0xdd:
    if (m.mod == 3) {
      switch (m.reg) {
      case 0:
        switch (m.rm) {
        case 0: __asm__ volatile("ffree %%st(0)" : : : "memory"); break; case 1: __asm__ volatile("ffree %%st(1)" : : : "memory"); break;
        case 2: __asm__ volatile("ffree %%st(2)" : : : "memory"); break; case 3: __asm__ volatile("ffree %%st(3)" : : : "memory"); break;
        case 4: __asm__ volatile("ffree %%st(4)" : : : "memory"); break; case 5: __asm__ volatile("ffree %%st(5)" : : : "memory"); break;
        case 6: __asm__ volatile("ffree %%st(6)" : : : "memory"); break; case 7: __asm__ volatile("ffree %%st(7)" : : : "memory"); break;
        } break;
      case 2: case 3:
        switch (m.rm) {
        case 0: __asm__ volatile("fst %%st(0)" : : : "memory"); break; case 1: __asm__ volatile("fst %%st(1)" : : : "memory"); break;
        case 2: __asm__ volatile("fst %%st(2)" : : : "memory"); break; case 3: __asm__ volatile("fst %%st(3)" : : : "memory"); break;
        case 4: __asm__ volatile("fst %%st(4)" : : : "memory"); break; case 5: __asm__ volatile("fst %%st(5)" : : : "memory"); break;
        case 6: __asm__ volatile("fst %%st(6)" : : : "memory"); break; case 7: __asm__ volatile("fst %%st(7)" : : : "memory"); break;
        }
        if (m.reg == 3) __asm__ volatile("fstp %%st(0)" : : : "memory");
        break;
      case 4: case 5:
        switch (m.rm) {
        case 0: if (m.reg == 4) __asm__ volatile("fucom %%st(0)" : : : "memory"); else __asm__ volatile("fucomp %%st(0)" : : : "memory"); break;
        case 1: if (m.reg == 4) __asm__ volatile("fucom %%st(1)" : : : "memory"); else __asm__ volatile("fucomp %%st(1)" : : : "memory"); break;
        case 2: if (m.reg == 4) __asm__ volatile("fucom %%st(2)" : : : "memory"); else __asm__ volatile("fucomp %%st(2)" : : : "memory"); break;
        case 3: if (m.reg == 4) __asm__ volatile("fucom %%st(3)" : : : "memory"); else __asm__ volatile("fucomp %%st(3)" : : : "memory"); break;
        case 4: if (m.reg == 4) __asm__ volatile("fucom %%st(4)" : : : "memory"); else __asm__ volatile("fucomp %%st(4)" : : : "memory"); break;
        case 5: if (m.reg == 4) __asm__ volatile("fucom %%st(5)" : : : "memory"); else __asm__ volatile("fucomp %%st(5)" : : : "memory"); break;
        case 6: if (m.reg == 4) __asm__ volatile("fucom %%st(6)" : : : "memory"); else __asm__ volatile("fucomp %%st(6)" : : : "memory"); break;
        case 7: if (m.reg == 4) __asm__ volatile("fucom %%st(7)" : : : "memory"); else __asm__ volatile("fucomp %%st(7)" : : : "memory"); break;
        }
        break;
      default:
        /* Reserved register forms raise #UD. DOSX probes these encodings
         * while selecting an x87-compatible execution path. */
        if (rw(0, 24u) || rw(0, 26u)) {
          ip = instruction_ip;
          dos_int(6);
          return vm->Running;
        }
        break;
      }
    } else if (m.reg == 0) {
      raw64 = 0;
      for (U32 i = 0; i < 8; ++i) raw64 |= (U64)rb(m.segment, (U16)(m.off + i)) << (i * 8u);
      __asm__ volatile("fldl %0" : : "m"(raw64) : "memory");
    } else if (m.reg == 1) { /* FISTTP m64int (truncate, independent of RC). */
      U16 old = (U16)(vm->FpuControl | 0x003fu);
      U16 trunc = (U16)((old & ~0x0c00u) | 0x0c00u);
      __asm__ volatile("fldcw %0" : : "m"(trunc) : "memory");
      __asm__ volatile("fistpll %0" : "=m"(raw64) : : "memory");
      __asm__ volatile("fldcw %0" : : "m"(old) : "memory");
      for (U32 i = 0; i < 8; ++i)
        wb(m.segment, (U16)(m.off + i), (U8)(raw64 >> (i * 8u)));
    } else if (m.reg == 2 || m.reg == 3) {
      __asm__ volatile("fstl %0" : "=m"(raw64) : : "memory");
      for (U32 i = 0; i < 8; ++i) wb(m.segment, (U16)(m.off + i), (U8)(raw64 >> (i * 8u)));
      if (m.reg == 3) __asm__ volatile("fstp %%st(0)" : : : "memory");
    } else if (m.reg == 4 || m.reg == 6) {
      if (m.reg == 4) {
        if (operand32) {
          x87_read_bytes(m, temp, 108);
          vm->FpuControl = word(temp);
        } else {
          U8 guest_state[94];
          x87_read_bytes(m, guest_state, 94);
          vm->FpuControl = word(guest_state);
          x87_state16_to_host108(guest_state, temp);
        }
        temp[0] = (U8)(vm->FpuControl | 0x3fu);
        temp[1] = (U8)((vm->FpuControl | 0x3fu) >> 8);
        __asm__ volatile("frstor %0" : : "m"(*(U8 (*)[108])temp) : "memory");
        x87_apply_control(vm);
      } else {
        __asm__ volatile("fnsave %0" : "=m"(*(U8 (*)[108])temp) : : "memory");
        if (operand32) {
          temp[0] = (U8)vm->FpuControl;
          temp[1] = (U8)(vm->FpuControl >> 8);
          x87_write_bytes(m, temp, 108);
        } else {
          U8 guest_state[94];
          temp[0] = (U8)vm->FpuControl;
          temp[1] = (U8)(vm->FpuControl >> 8);
          x87_state108_to_guest94(temp, guest_state);
          x87_write_bytes(m, guest_state, 94);
        }
        x87_apply_control(vm);
      }
    } else if (m.reg == 7) {
      x87_capture_status(vm);
      ww(m.segment, m.off, vm->FpuStatus);
    } else {
      /* Reserved x87 memory forms are ignored by the DOS compatibility FPU.
       * DOSX uses DD /5 as a probe before choosing its protected-mode path. */
    }
    break;

  case 0xdf:
    if (m.mod == 3 && m.reg == 0) { /* FFREEP ST(i): free then pop ST0. */
      switch (m.rm) {
      case 0: __asm__ volatile("ffree %%st(0); fstp %%st(0)" : : : "memory"); break;
      case 1: __asm__ volatile("ffree %%st(1); fstp %%st(0)" : : : "memory"); break;
      case 2: __asm__ volatile("ffree %%st(2); fstp %%st(0)" : : : "memory"); break;
      case 3: __asm__ volatile("ffree %%st(3); fstp %%st(0)" : : : "memory"); break;
      case 4: __asm__ volatile("ffree %%st(4); fstp %%st(0)" : : : "memory"); break;
      case 5: __asm__ volatile("ffree %%st(5); fstp %%st(0)" : : : "memory"); break;
      case 6: __asm__ volatile("ffree %%st(6); fstp %%st(0)" : : : "memory"); break;
      case 7: __asm__ volatile("ffree %%st(7); fstp %%st(0)" : : : "memory"); break;
      }
    } else if (m.mod == 3 && m.reg == 1) { /* FXCH ST(i). */
      switch (m.rm) {
      case 0: X87_ST_EXCHANGE(0); break; case 1: X87_ST_EXCHANGE(1); break;
      case 2: X87_ST_EXCHANGE(2); break; case 3: X87_ST_EXCHANGE(3); break;
      case 4: X87_ST_EXCHANGE(4); break; case 5: X87_ST_EXCHANGE(5); break;
      case 6: X87_ST_EXCHANGE(6); break; case 7: X87_ST_EXCHANGE(7); break;
      }
    } else if (m.mod == 3 && m.reg == 4 && m.rm == 0) {
      x87_capture_status(vm);
      vm->Registers[AX] = vm->FpuStatus;
      vm->Registers32[AX] = (vm->Registers32[AX] & 0xffff0000u) | vm->FpuStatus;
    } else if (m.mod == 3 && (m.reg == 5 || m.reg == 6)) {
      U64 host_flags = 0;
      switch (m.rm) {
      case 0: if (m.reg == 5) __asm__ volatile("fucomip %%st(0), %%st(0); pushfq; popq %0" : "=r"(host_flags) : : "cc", "memory"); else __asm__ volatile("fcomip %%st(0), %%st(0); pushfq; popq %0" : "=r"(host_flags) : : "cc", "memory"); break;
      case 1: if (m.reg == 5) __asm__ volatile("fucomip %%st(1), %%st(0); pushfq; popq %0" : "=r"(host_flags) : : "cc", "memory"); else __asm__ volatile("fcomip %%st(1), %%st(0); pushfq; popq %0" : "=r"(host_flags) : : "cc", "memory"); break;
      case 2: if (m.reg == 5) __asm__ volatile("fucomip %%st(2), %%st(0); pushfq; popq %0" : "=r"(host_flags) : : "cc", "memory"); else __asm__ volatile("fcomip %%st(2), %%st(0); pushfq; popq %0" : "=r"(host_flags) : : "cc", "memory"); break;
      case 3: if (m.reg == 5) __asm__ volatile("fucomip %%st(3), %%st(0); pushfq; popq %0" : "=r"(host_flags) : : "cc", "memory"); else __asm__ volatile("fcomip %%st(3), %%st(0); pushfq; popq %0" : "=r"(host_flags) : : "cc", "memory"); break;
      case 4: if (m.reg == 5) __asm__ volatile("fucomip %%st(4), %%st(0); pushfq; popq %0" : "=r"(host_flags) : : "cc", "memory"); else __asm__ volatile("fcomip %%st(4), %%st(0); pushfq; popq %0" : "=r"(host_flags) : : "cc", "memory"); break;
      case 5: if (m.reg == 5) __asm__ volatile("fucomip %%st(5), %%st(0); pushfq; popq %0" : "=r"(host_flags) : : "cc", "memory"); else __asm__ volatile("fcomip %%st(5), %%st(0); pushfq; popq %0" : "=r"(host_flags) : : "cc", "memory"); break;
      case 6: if (m.reg == 5) __asm__ volatile("fucomip %%st(6), %%st(0); pushfq; popq %0" : "=r"(host_flags) : : "cc", "memory"); else __asm__ volatile("fcomip %%st(6), %%st(0); pushfq; popq %0" : "=r"(host_flags) : : "cc", "memory"); break;
      case 7: if (m.reg == 5) __asm__ volatile("fucomip %%st(7), %%st(0); pushfq; popq %0" : "=r"(host_flags) : : "cc", "memory"); else __asm__ volatile("fcomip %%st(7), %%st(0); pushfq; popq %0" : "=r"(host_flags) : : "cc", "memory"); break;
      }
      vm->Flags = (U16)((vm->Flags & ~(CF | ZF | 4u)) |
                        (host_flags & (CF | ZF | 4u)) | 2u);
    } else if (m.mod != 3 && (m.reg == 0 || m.reg == 1 || m.reg == 2 || m.reg == 3)) {
      if (m.reg == 0) {
        raw16 = rw(m.segment, m.off);
        __asm__ volatile("filds %0" : : "m"(raw16) : "memory");
      } else if (m.reg == 1) {
        U16 old = (U16)(vm->FpuControl | 0x003fu);
        U16 trunc = (U16)((old & ~0x0c00u) | 0x0c00u);
        __asm__ volatile("fldcw %0" : : "m"(trunc) : "memory");
        __asm__ volatile("fistps %0" : "=m"(raw16) : : "memory");
        __asm__ volatile("fldcw %0" : : "m"(old) : "memory");
        ww(m.segment, m.off, raw16);
      } else if (m.reg == 2) {
        __asm__ volatile("fists %0" : "=m"(raw16) : : "memory");
        ww(m.segment, m.off, raw16);
      } else {
        __asm__ volatile("fistps %0" : "=m"(raw16) : : "memory");
        ww(m.segment, m.off, raw16);
      }
    } else if (m.mod != 3 && (m.reg == 4 || m.reg == 5 ||
                              m.reg == 6 || m.reg == 7)) {
      if (m.reg == 4) { /* FBLD m80bcd */
        for (U32 i = 0; i < 10; i++) temp[i] = rb(m.segment, (U16)(m.off + i));
        __asm__ volatile("fbld %0" : : "m"(*(U8 (*)[10])temp) : "memory");
      } else if (m.reg == 5) {
        raw64 = 0;
        for (U32 i = 0; i < 8; ++i)
          raw64 |= (U64)rb(m.segment, (U16)(m.off + i)) << (i * 8u);
        __asm__ volatile("fildll %0" : : "m"(raw64) : "memory");
      } else if (m.reg == 6) { /* FBSTP m80bcd */
        __asm__ volatile("fbstp %0" : "=m"(*(U8 (*)[10])temp) : : "memory");
        for (U32 i = 0; i < 10; i++) wb(m.segment, (U16)(m.off + i), temp[i]);
      } else {
        __asm__ volatile("fistpll %0" : "=m"(raw64) : : "memory");
        for (U32 i = 0; i < 8; ++i)
          wb(m.segment, (U16)(m.off + i), (U8)(raw64 >> (i * 8u)));
      }
    } else {
      KeLog("[dos] unsupported x87 DF /%u mod=%u rm=%u at %04x:%04x\n", m.reg, m.mod, m.rm, instruction_cs, instruction_ip);
      fail("DOSVM: unsupported x87 DF instruction\r\n");
    }
    break;

  default:
    return 0;
  }

  x87_capture_status(vm);
  return vm->Running;
}

#undef X87_ST2
#undef X87_ST1
#undef X87_ST_LOAD
#undef X87_ST_EXCHANGE

void fail(const char *s) {
  if (!fault)
    say(s);
  fault = 1;
  running = 0;
  exit_code = 255;
}

static U16 shift_value(U8 kind, U16 value, U8 count, int wide) {
  U16 mask = wide ? 0xffff : 0xff, sign = wide ? 0x8000 : 0x80;
  U16 original = value & mask;
  count &= 31;
  if (!count)
    return original;
  U16 v = original, carry = 0, rotate_carry = !!(flags & CF);
  for (U8 i = 0; i < count; i++) {
    if (kind == 0) {
      carry = !!(v & sign);
      v = ((v << 1) | carry) & mask;
    } else if (kind == 1) {
      carry = v & 1;
      v = (v >> 1) | (carry ? sign : 0);
    } else if (kind == 2) { /* RCL: rotate through the carry flag */
      carry = !!(v & sign);
      v = ((v << 1) | rotate_carry) & mask;
      rotate_carry = carry;
    } else if (kind == 3) { /* RCR: rotate through the carry flag */
      carry = v & 1;
      v = (v >> 1) | (rotate_carry ? sign : 0);
      rotate_carry = carry;
    } else if (kind == 4 || kind == 6) {
      carry = !!(v & sign);
      v = (v << 1) & mask;
    } else if (kind == 5) {
      carry = v & 1;
      v >>= 1;
    } else if (kind == 7) {
      carry = v & 1;
      v = (v >> 1) | (v & sign);
    } else {
      fail("DOS: unsupported rotate\r\n");
      return original;
    }
  }
  flags = (flags & ~CF) | (carry ? CF : 0);
  if (kind >= 4) {
    flags &= ~(ZF | SF | 4u);
    if (!v)
      flags |= ZF;
    if (v & sign)
      flags |= SF;
    if (parity((U8)v))
      flags |= 4;
  }
  if (count == 1) {
    flags &= ~OF;
    if (kind == 4 || kind == 6 || kind == 0 || kind == 2) {
      if (!!(v & sign) != carry)
        flags |= OF;
    } else if (kind == 5) {
      if (original & sign)
        flags |= OF;
    } else if (kind == 1 || kind == 3) {
      if ((v & sign) != (v & (sign >> 1)) << 1)
        flags |= OF;
    }
  }
  return v;
}

static int condition_flags(U32 tested_flags, unsigned cc) {
  int c = !!(tested_flags & CF), z = !!(tested_flags & ZF), s = !!(tested_flags & SF),
      o = !!(tested_flags & OF), p = !!(tested_flags & 4);
  switch (cc >> 1) {
  case 0:
    return o ^ (cc & 1);
  case 1:
    return c ^ (cc & 1);
  case 2:
    return z ^ (cc & 1);
  case 3:
    return (c || z) ^ (cc & 1);
  case 4:
    return s ^ (cc & 1);
  case 5:
    return p ^ (cc & 1);
  case 6:
    return (s != o) ^ (cc & 1);
  default:
    return (z || (s != o)) ^ (cc & 1);
  }
}

static int condition(unsigned cc) {
  DOS_VM *vm = current();
  return condition_flags(vm->ProtectedMode ? vm->Eflags : vm->Flags, cc);
}

static __attribute__((always_inline)) inline void step_vm_body(DOS_VM *vm) {
  if (!vm || !vm->Running) return;
  if (vm->MouseCallbackActive && DosMouseCallbackReturn(vm)) return;
  if (dos_hardware_poll_due(vm) && dos_dispatch_hardware(vm)) return;
  if ((vm->Steps & 0x3fffu) == 0) {
    UINT32 tick = (UINT32)KeGetTickCount();
    if (vm->VideoActive && (tick - vm->LastVideoTick) >= 2u) {
      vm->LastVideoTick = tick;
      DosVideoPresent(vm);
    }
  }
  if (vm->ProtectedMode) {
    step_protected(vm);
    return;
  }
  int override = -1, rep = 0, operand32 = 0, address32 = 0;
  for (unsigned i = 0; i < 8; i++)
    r32[i] = (r32[i] & 0xffff0000u) | r[i];
  /* A saved BIOS/DOS vector can also be chained with PUSHF/CALL FAR.
   * Execute the built-in service, then consume its real-mode IRET frame. */
  if (seg[CS] == DOS_BUILTIN_SEGMENT && ip >= DOS_BUILTIN_OFFSET(0) &&
      ip <= DOS_BUILTIN_OFFSET(255) && !(ip & 3u)) {
    U8 vector = (U8)((ip - DOS_BUILTIN_OFFSET(0)) / 4u);
    U16 off = rw(0, (U16)(vector * 4u));
    U16 cs = rw(0, (U16)(vector * 4u + 2u));
    ww(0, (U16)(vector * 4u), 0);
    ww(0, (U16)(vector * 4u + 2u), 0);
    dos_int(vector);
    ww(0, (U16)(vector * 4u), off);
    ww(0, (U16)(vector * 4u + 2u), cs);
    if (running) {
      U16 return_ip = pop(), return_cs = pop(), return_flags = pop();
      ip = return_ip; seg[CS] = return_cs;
      flags = (flags & 0x0cd5u) | (return_flags & ~0x0cd5u) | 2u;
      vm->Eflags = (vm->Eflags & 0xffff0000u) | flags;
    }
    return;
  }
  if (seg[CS] == 0xf000u && ip == 0x0100u) {
    dpmi_initial_switch();
    return;
  }
  if (seg[CS] == 0xf000u && ip == 0x010au) {
    dpmi_raw_to_protected();
    return;
  }
  U16 instruction_cs = seg[CS], instruction_ip = ip;
  U8 op = fetch8();
  for (unsigned i = 0; i < 8; i++) {
    if (op == 0x26)
      override = ES;
    else if (op == 0x2e)
      override = CS;
    else if (op == 0x36)
      override = SS;
    else if (op == 0x3e)
      override = DS;
    else if (op == 0x64)
      override = FS;
    else if (op == 0x65)
      override = GS;
    else if (op == 0xf2 || op == 0xf3)
      rep = op;
    else if (op == 0xf0)
      ; /* LOCK is redundant in this single-CPU DOS VM. */
    else if (op == 0x66)
      operand32 = 1;
    else if (op == 0x67)
      address32 = 1;
    else
      break;
    op = fetch8();
  }
  if (op >= 0xd8 && op <= 0xdf) {
    (void)x87_real(vm, op, override, address32, operand32, instruction_cs, instruction_ip);
    return;
  }
  if (op >= 0x70 && op <= 0x7f) {
    signed char d = (signed char)fetch8();
    if (condition(op & 15))
      ip += d;
    return;
  }
  if (op >= 0xb8 && op <= 0xbf) {
    if (operand32) {
      r32[op & 7] = fetch32();
      r[op & 7] = (U16)r32[op & 7];
    } else
      r[op & 7] = fetch16();
    return;
  }
  if (op >= 0xb0 && op <= 0xb7) {
    set8(op & 7, fetch8());
    return;
  }
  if (op >= 0x50 && op <= 0x57) {
    if (operand32)
      push32(r32[op & 7]);
    else
      push(r[op & 7]);
    return;
  }
  if (op >= 0x58 && op <= 0x5f) {
    if (operand32) {
      r32[op & 7] = pop32();
      r[op & 7] = (U16)r32[op & 7];
    } else
      r[op & 7] = pop();
    return;
  }
  if (op >= 0x40 && op <= 0x4f) {
    unsigned i = op & 7;
    U16 cf = flags & CF;
    if (operand32) {
      U32 value = r32[i], result = op < 0x48 ? value + 1u : value - 1u;
      U32 f = flags & ~(ZF | SF | OF | AF | 4u);
      if (!result) f |= ZF;
      if (result & 0x80000000u) f |= SF;
      if (op < 0x48 ? value == 0x7fffffffu : value == 0x80000000u) f |= OF;
      if ((value ^ result) & 0x10u) f |= AF;
      flags = f | cf;
      r32[i] = result;
      r[i] = (U16)result;
    } else
      r[i] = alu(op < 0x48 ? 0 : 5, r[i], 1, 16);
    flags = (flags & ~CF) | cf;
    return;
  }
  if (op >= 0x91 && op <= 0x97) {
    if (operand32) {
      U32 t = r32[AX];
      r32[AX] = r32[op & 7];
      r32[op & 7] = t;
      r[AX] = (U16)r32[AX];
      r[op & 7] = (U16)r32[op & 7];
    } else {
      U16 t = r[AX];
      r[AX] = r[op & 7];
      r[op & 7] = t;
    }
    return;
  }
  if (op <= 0x3d && (op & 7) >= 4 && (op & 7) <= 5) {
    unsigned operation = op >> 3, wide = op & 1;
    if (wide && operand32) {
      U32 immediate = fetch32();
      U32 result = alu32(operation, r32[AX], immediate);
      if (operation != 7) {
        r32[AX] = result;
        r[AX] = (U16)result;
      }
      return;
    }
    U16 immediate = wide ? fetch16() : fetch8();
    U16 result =
        alu(operation, wide ? r[AX] : get8(0), immediate, wide ? 16 : 8);
    if (operation != 7) {
      if (wide)
        r[AX] = result;
      else
        set8(0, (U8)result);
    }
    return;
  }
  if (op <= 0x3b && (op & 7) <= 3) {
    unsigned operation = op >> 3, wide = op & 1, direction = op & 2;
    MOD m = decode(override, address32);
    if (wide && operand32) {
      U32 rm_value = m.mod == 3 ? r32[m.rm] : read32(m.segment, m.off);
      U32 reg_value = r32[m.reg];
      U32 a = direction ? reg_value : rm_value;
      U32 b = direction ? rm_value : reg_value;
      U32 result = alu32(operation, a, b);
      if (operation != 7) {
        if (direction) { r32[m.reg] = result; r[m.reg] = (U16)result; }
        else if (m.mod == 3) { r32[m.rm] = result; r[m.rm] = (U16)result; }
        else write32(m.segment, m.off, result);
      }
      return;
    }
    U16 a = direction ? (wide ? r[m.reg] : get8(m.reg)) : read_rm(m, wide);
    U16 b = direction ? read_rm(m, wide) : (wide ? r[m.reg] : get8(m.reg));
    U16 result = alu(operation, a, b, wide ? 16 : 8);
    if (operation != 7) {
      if (direction) {
        if (wide)
          r[m.reg] = result;
        else
          set8(m.reg, (U8)result);
      } else
        write_rm(m, wide, result);
    }
    return;
  }
  if (op >= 0x88 && op <= 0x8b) {
    MOD m = decode(override, address32);
    unsigned wide = op & 1;
    if (wide && operand32) {
      if (op & 2) {
        U32 value = m.mod == 3 ? r32[m.rm] : read32(m.segment, m.off);
        r32[m.reg] = value;
        r[m.reg] = (U16)value;
      } else if (m.mod == 3) {
        r32[m.rm] = r32[m.reg];
        r[m.rm] = (U16)r32[m.rm];
      } else
        write32(m.segment, m.off, r32[m.reg]);
      return;
    }
    if (op & 2) {
      U16 v = read_rm(m, wide);
      if (wide)
        r[m.reg] = v;
      else
        set8(m.reg, (U8)v);
    } else
      write_rm(m, wide, wide ? r[m.reg] : get8(m.reg));
    return;
  }
  if (op >= 0x80 && op <= 0x83) {
    MOD m = decode(override, address32);
    int wide = op == 0x81 || op == 0x83;
    if (operand32 && wide) {
      U32 value = m.mod == 3 ? r32[m.rm] : read32(m.segment, m.off);
      U32 immediate = op == 0x83 ? (U32)(INT32)(signed char)fetch8() : fetch32();
      U32 result = alu32(m.reg, value, immediate);
      if (m.reg != 7) {
        if (m.mod == 3) { r32[m.rm] = result; r[m.rm] = (U16)result; }
        else write32(m.segment, m.off, result);
      }
      return;
    }
    U16 imm = op == 0x81   ? fetch16()
              : op == 0x83 ? (U16)(signed char)fetch8()
                           : fetch8();
    U16 v = alu(m.reg, read_rm(m, wide), imm, wide ? 16 : 8);
    if (m.reg != 7)
      write_rm(m, wide, v);
    return;
  }
  if ((op >= 0xa4 && op <= 0xa7) || (op >= 0xaa && op <= 0xaf)) {
    unsigned width = (op & 1u) ? (operand32 ? 32u : 16u) : 8u;
    unsigned bytes = width / 8u;
    U32 count = rep ? (address32 ? r32[CX] : r[CX]) : 1u;
    U32 si = address32 ? r32[SI] : r[SI];
    U32 di = address32 ? r32[DI] : r[DI];
    while (count--) {
      U16 source = seg[override >= 0 ? override : DS], dest = seg[ES];
      U32 a = width == 32u ? read32(source, si) :
              width == 16u ? rw(source, si) : rb(source, si);
      U32 b = width == 32u ? read32(dest, di) :
              width == 16u ? rw(dest, di) : rb(dest, di);
      if (op <= 0xa5) {
        if (width == 32u)
          write32(dest, di, a);
        else if (width == 16u)
          ww(dest, di, (U16)a);
        else
          wb(dest, di, (U8)a);
      } else if (op <= 0xa7) {
        if (width == 32u) alu32(7, a, b);
        else (void)alu(7, (U16)a, (U16)b, width);
      } else if (op <= 0xab) {
        U32 accumulator = operand32 ? r32[AX] : r[AX];
        if (width == 32u)
          write32(dest, di, accumulator);
        else if (width == 16u)
          ww(dest, di, r[AX]);
        else
          wb(dest, di, (U8)r[AX]);
      } else if (op <= 0xad) {
        if (width == 32u) { r32[AX] = a; r[AX] = (U16)a; }
        else if (width == 16u)
          r[AX] = (U16)a;
        else
          set8(0, (U8)a);
      } else if (width == 32u)
        alu32(7, operand32 ? r32[AX] : r[AX], b);
      else
        (void)alu(7, width == 16u ? r[AX] : get8(0), (U16)b, width);
      INT32 delta = flags & DF ? -(INT32)bytes : (INT32)bytes;
      if (op <= 0xa7 || (op <= 0xad && op >= 0xac))
        si = address32 ? si + (U32)delta : (U16)(si + delta);
      if (op <= 0xab || op >= 0xae)
        di = address32 ? di + (U32)delta : (U16)(di + delta);
      if (rep) {
        if (address32) r32[CX] = count;
        else r[CX] = (U16)count;
        if ((op == 0xa6 || op == 0xa7 || op == 0xae || op == 0xaf) &&
            ((rep == 0xf3) != !!(flags & ZF)))
          break;
      }
    }
    if (address32) {
      r32[SI] = si; r32[DI] = di;
      r[SI] = (U16)si; r[DI] = (U16)di;
      if (rep) r[CX] = (U16)r32[CX];
    } else {
      r[SI] = (U16)si; r[DI] = (U16)di;
    }
    return;
  }
  if (op >= 0x6c && op <= 0x6f) {
    unsigned io_width = (op & 1u) ? (operand32 ? 32u : 16u) : 8u;
    unsigned bytes = io_width / 8u;
    U32 count = rep ? (address32 ? r32[CX] : r[CX]) : 1u;
    U32 si = address32 ? r32[SI] : r[SI];
    U32 di = address32 ? r32[DI] : r[DI];
    U16 port = r[DX];
    if (count > 0xffffu) count = 0xffffu;
    for (U32 i = 0; i < count && running; i++) {
      if (op == 0x6c || op == 0x6d) {
        U32 value = DosPortIn(current(), port, io_width);
        if (io_width == 8) wb(seg[ES], di, (U8)value);
        else if (io_width == 16) ww(seg[ES], di, (U16)value);
        else write32(seg[ES], di, value);
      } else {
        U32 value = io_width == 8 ? rb(seg[override >= 0 ? override : DS], si) :
                    io_width == 16 ? rw(seg[override >= 0 ? override : DS], si) :
                    read32(seg[override >= 0 ? override : DS], si);
        DosPortOut(current(), port, value, io_width);
      }
      INT16 delta = (flags & DF) ? -(INT16)bytes : (INT16)bytes;
      if (op == 0x6c || op == 0x6d)
        di = address32 ? di + (U32)delta : (U16)(di + delta);
      else si = address32 ? si + (U32)delta : (U16)(si + delta);
    }
    if (address32) {
      r32[SI] = si; r32[DI] = di;
      r[SI] = (U16)si; r[DI] = (U16)di;
      if (rep) { r32[CX] = 0; r[CX] = 0; }
    } else {
      r[SI] = (U16)si; r[DI] = (U16)di;
      if (rep) r[CX] = 0;
    }
    return;
  }

  switch (op) {
  case 0x62: { /* BOUND r16/r32, m16&16/m32&32 */
    MOD m = decode(override, address32);
    if (m.mod == 3) {
      if (rw(0, 24u) || rw(0, 26u)) dos_int(6);
      return;
    }
    if (operand32) {
      INT32 value = (INT32)r32[m.reg];
      INT32 lower = (INT32)read32(m.segment, m.off);
      INT32 upper = (INT32)read32(m.segment, (U16)(m.off + 4u));
      if (value < lower || value > upper) {
        if (rw(0, 20u) || rw(0, 22u)) dos_int(5);
        /* A missing real-mode vector means there is no guest handler for
         * this CPU exception. Keep the DOS VM alive so early CPU probes can
         * run before an extender installs its exception vectors. */
      }
    } else {
      INT16 value = (INT16)r[m.reg];
      INT16 lower = (INT16)rw(m.segment, m.off);
      INT16 upper = (INT16)rw(m.segment, (U16)(m.off + 2u));
      if (value < lower || value > upper) {
        if (rw(0, 20u) || rw(0, 22u)) dos_int(5);
      }
    }
    return;
  }
  case 0x63: { /* ARPL r/m16, r16 (DOSX CPU probe). */
    MOD m = decode(override, address32);
    U16 destination = read_rm(m, 1);
    U16 source = r[m.reg];
    if ((destination & 3u) < (source & 3u)) {
      destination = (U16)((destination & ~3u) | (source & 3u));
      write_rm(m, 1, destination);
      flags |= ZF;
    } else flags &= ~ZF;
    return;
  }
  case 0x0f: {
    U8 extension = fetch8();
    if (extension == 0x0a) { /* LAR r16/r32, r/m16. */
      MOD m = decode(override, address32);
      U16 selector = read_rm(m, 1);
      DOS_DESCRIPTOR descriptor;
      if (read_descriptor(selector, &descriptor)) {
        U32 rights = ((U32)descriptor.Access << 8) |
                     ((U32)(descriptor.Flags & 0x0fu) << 20);
        if (operand32) r32[m.reg] = rights;
        else r[m.reg] = (U16)rights;
        flags |= ZF;
      } else {
        flags &= ~ZF;
      }
      return;
    }
    if (extension == 0x06) { /* CLTS: no task-switched FPU in this VM. */
      current()->Cr0 &= ~(1u << 3);
      return;
    }
    if (extension == 0x77) { /* EMMS: the VM does not track MMX tag aliases. */
      return;
    }
    if (extension == 0xdb || extension == 0xdf ||
        extension == 0xeb || extension == 0xef ||
        extension == 0x64 || extension == 0x65 || extension == 0x66 ||
        extension == 0x74 || extension == 0x75 || extension == 0x76 ||
        extension == 0xec ||
        extension == 0x6e || extension == 0x6f ||
        extension == 0x7e || extension == 0x7f) {
      MOD m = decode(override, address32);
      U64 source;
      if (extension == 0x6f || extension == 0x7f) {
        if (extension == 0x6f)
          vm->MmxRegisters[m.reg] = m.mod == 3 ? vm->MmxRegisters[m.rm] :
              ((U64)read32(m.segment, m.off) |
               ((U64)read32(m.segment, (U16)(m.off + 4u)) << 32));
        else if (m.mod == 3) vm->MmxRegisters[m.rm] = vm->MmxRegisters[m.reg];
        else {
          write32(m.segment, m.off, (U32)vm->MmxRegisters[m.reg]);
          write32(m.segment, (U16)(m.off + 4u), (U32)(vm->MmxRegisters[m.reg] >> 32));
        }
        return;
      }
      if (extension == 0x6e || extension == 0x7e) {
        if (extension == 0x6e)
          vm->MmxRegisters[m.reg] = m.mod == 3 ? r32[m.rm] : read32(m.segment, m.off);
        else if (m.mod == 3) {
          r32[m.rm] = (U32)vm->MmxRegisters[m.reg];
          r[m.rm] = (U16)r32[m.rm];
        } else write32(m.segment, m.off, (U32)vm->MmxRegisters[m.reg]);
        return;
      }
      if (m.mod == 3) source = vm->MmxRegisters[m.rm];
      else {
        source = (U64)rw(m.segment, m.off) |
                 ((U64)rw(m.segment, (U16)(m.off + 2u)) << 16) |
                 ((U64)rw(m.segment, (U16)(m.off + 4u)) << 32) |
                 ((U64)rw(m.segment, (U16)(m.off + 6u)) << 48);
      }
      if (extension == 0xdb) vm->MmxRegisters[m.reg] &= source;      /* PAND */
      else if (extension == 0xdf) vm->MmxRegisters[m.reg] =
          ~vm->MmxRegisters[m.reg] & source;                         /* PANDN */
      else if (extension == 0xeb) vm->MmxRegisters[m.reg] |= source; /* POR */
      else if (extension == 0xef) vm->MmxRegisters[m.reg] ^= source; /* PXOR */
      else if (extension == 0xec) { /* PADDSB: signed saturating byte add. */
        U64 result = 0;
        for (unsigned lane = 0; lane < 8; lane++) {
          INT16 sum = (INT8)(vm->MmxRegisters[m.reg] >> (lane * 8u)) +
                      (INT8)(source >> (lane * 8u));
          if (sum > 127) sum = 127;
          if (sum < -128) sum = -128;
          result |= (U64)(U8)(INT8)sum << (lane * 8u);
        }
        vm->MmxRegisters[m.reg] = result;
      }
      else {
        unsigned lane_bits = (extension == 0x64 || extension == 0x74) ? 8u :
                             (extension == 0x65 || extension == 0x75) ? 16u : 32u;
        U64 result = 0;
        for (unsigned shift = 0; shift < 64; shift += lane_bits) {
          U64 mask = lane_bits == 32u ? 0xffffffffULL : (1ULL << lane_bits) - 1u;
          U64 left = (vm->MmxRegisters[m.reg] >> shift) & mask;
          U64 right = (source >> shift) & mask;
          int equal = left == right;
          int greater = lane_bits == 8u ? (INT8)left > (INT8)right :
                        lane_bits == 16u ? (INT16)left > (INT16)right :
                                           (INT32)left > (INT32)right;
          int compare_equal = extension >= 0x74u;
          result |= (compare_equal ? (unsigned int)equal : greater ? 1u : 0u)
                    ? mask << shift : 0;
        }
        vm->MmxRegisters[m.reg] = result;
      }
      return;
    }
    if (extension >= 0x80 && extension <= 0x8f) {
      if (operand32) {
        INT32 displacement = (INT32)fetch32();
        if (condition(extension & 15)) {
          INT32 target = (INT32)ip + displacement;
          if (target < 0 || target > 0xffff) {
            fail("DOSVM: 32-bit branch exceeds real-mode code segment\r\n");
            return;
          }
          ip = (U16)target;
        }
      } else {
        U16 displacement = fetch16();
        if (condition(extension & 15))
          ip += displacement;
      }
      return;
    }
    if (extension == 0xa2) {
      U32 leaf = r32[AX];
      r32[AX] = leaf ? 0 : 1;
      r32[BX] = 0x756e6547u;
      r32[DX] = 0x49656e69u;
      r32[CX] = leaf ? 0 : 0x6c65746eu;
      for (unsigned i = 0; i < 8; i++) r[i] = (U16)r32[i];
      return;
    }
    if (extension == 0x1f) { /* multi-byte NOP */
      (void)decode(override, address32);
      return;
    }
    if (extension >= 0x40 && extension <= 0x4f) { /* CMOVcc r16/r32,r/m */
      MOD m = decode(override, address32);
      if (condition(extension & 15u)) {
        if (operand32) {
          U32 value = m.mod == 3 ? r32[m.rm] : read32(m.segment, m.off);
          r32[m.reg] = value;
          r[m.reg] = (U16)value;
        } else {
          r[m.reg] = m.mod == 3 ? r[m.rm] : read_rm(m, 1);
        }
      }
      return;
    }
    if (extension >= 0x90 && extension <= 0x9f) { /* SETcc r/m8 */
      MOD m = decode(override, address32);
      write_rm(m, 0, (U16)condition(extension & 15));
      return;
    }
    if (extension == 0xb6 || extension == 0xb7 ||
        extension == 0xbe || extension == 0xbf) { /* MOVZX/MOVSX */
      MOD m = decode(override, address32);
      unsigned source_width = (extension == 0xb6 || extension == 0xbe) ? 8 : 16;
      U32 value = read_rm(m, source_width == 16);
      if (extension == 0xbe) value = (U32)(INT32)(INT8)value;
      else if (extension == 0xbf) value = (U32)(INT32)(INT16)value;
      if (operand32) {
        r32[m.reg] = value;
        r[m.reg] = (U16)value;
      } else r[m.reg] = (U16)value;
      return;
    }
    if (extension == 0xaf) { /* two-operand IMUL */
      MOD m = decode(override, address32);
      if (operand32) {
        INT64 product = (INT64)(INT32)r32[m.reg] * (INT32)(m.mod == 3
            ? r32[m.rm] : read32(m.segment, m.off));
        U32 result = (U32)product;
        r32[m.reg] = result;
        r[m.reg] = (U16)result;
        U32 sign_extension = (result & 0x80000000u) ? 0xffffffffu : 0;
        flags = (flags & ~(CF | OF)) | ((U32)(product >> 32) != sign_extension
                                             ? CF | OF : 0);
      } else {
        INT32 product = (INT32)(INT16)r[m.reg] * (INT16)read_rm(m, 1);
        r[m.reg] = (U16)product;
        flags = (flags & ~(CF | OF)) |
                (product < -32768 || product > 32767 ? CF | OF : 0);
      }
      return;
    }
    if (extension == 0xc0 || extension == 0xc1) { /* XADD r/m,reg */
      MOD m = decode(override, address32);
      if (extension == 0xc0) {
        U8 destination = (U8)read_rm(m, 0);
        U8 source = get8(m.reg);
        U8 result = (U8)alu(0, destination, source, 8);
        write_rm(m, 0, result);
        set8(m.reg, destination);
      } else if (operand32) {
        U32 destination = m.mod == 3 ? r32[m.rm] : read32(m.segment, m.off);
        U32 source = r32[m.reg];
        U32 result = alu32(0, destination, source);
        if (m.mod == 3) {
          r32[m.rm] = result;
          r[m.rm] = (U16)result;
        } else write32(m.segment, m.off, result);
        r32[m.reg] = destination;
        r[m.reg] = (U16)destination;
      } else {
        U16 destination = read_rm(m, 1);
        U16 source = r[m.reg];
        U16 result = alu(0, destination, source, 16);
        write_rm(m, 1, result);
        r[m.reg] = destination;
      }
      return;
    }
    if (extension == 0x31) { /* RDTSC: deterministic virtual instruction time */
      U64 ticks = current()->Steps;
      r32[AX] = (U32)ticks;
      r32[DX] = (U32)(ticks >> 32);
      r[AX] = (U16)r32[AX];
      r[DX] = (U16)r32[DX];
      return;
    }
    if (extension == 0xba) {
      MOD m = decode(override, address32);
      unsigned width = operand32 ? 32 : 16;
      U32 value = m.mod == 3 ? (width == 32 ? r32[m.rm] : r[m.rm])
                             : (width == 32 ? read32(m.segment, m.off)
                                            : read_rm(m, 1));
      U8 bit = fetch8() & (width - 1);
      flags = (flags & ~CF) | ((value & (1u << bit)) ? CF : 0);
      if (m.reg >= 5) {
        if (m.reg == 5) value |= 1u << bit;
        else if (m.reg == 6) value &= ~(1u << bit);
        else value ^= 1u << bit;
        if (m.mod == 3) {
          if (width == 32) { r32[m.rm] = value; r[m.rm] = (U16)value; }
          else r[m.rm] = (U16)value;
        } else if (width == 32)
          write32(m.segment, m.off, value);
        else
          write_rm(m, 1, (U16)value);
      } else if (m.reg != 4) {
        fail("DOSVM: invalid bit-test instruction group\r\n");
      }
      return;
    }
    if (extension == 0xa6 || extension == 0xa7) { /* 386 XBTS / IBTS */
      MOD m = decode(override, address32);
      unsigned width = operand32 ? 32u : 16u;
      unsigned count = r[CX] & 0x1fu;
      U32 bit_offset = operand32 ? r32[AX] : r[AX];
      if (count) {
        U32 value;
        unsigned shift = bit_offset & (width - 1u);
        if (m.mod == 3) value = operand32 ? r32[m.rm] : r[m.rm];
        else {
          U32 byte_offset = (bit_offset / width) * (width / 8u);
          value = width == 32 ? read32(m.segment, (U16)(m.off + byte_offset))
                              : (U32)rw(m.segment, (U16)(m.off + byte_offset)) |
                                ((U32)rw(m.segment, (U16)(m.off + byte_offset + 2u)) << 16);
        }
        U32 mask = count >= width ? (width == 32 ? 0xffffffffu : 0xffffu)
                                  : ((1u << count) - 1u);
        if (extension == 0xa6) {
          U32 result = (shift + count > width && m.mod != 3 && width == 16)
                         ? ((value >> shift) | (value << (32u - shift)))
                         : (value >> shift);
          if (operand32) { r32[m.reg] = result & mask; r[m.reg] = (U16)r32[m.reg]; }
          else r[m.reg] = (U16)(result & mask);
        } else {
          U32 insert = (operand32 ? r32[m.reg] : r[m.reg]) & mask;
          U32 shifted_mask = mask << shift;
          value = (value & ~shifted_mask) | (insert << shift);
          if (m.mod == 3) {
            if (operand32) { r32[m.rm] = value; r[m.rm] = (U16)value; }
            else r[m.rm] = (U16)value;
          } else if (width == 32) write32(m.segment, (U16)(m.off + (bit_offset / width) * 4u), value);
          else {
            U16 off = (U16)(m.off + (bit_offset / width) * 2u);
            ww(m.segment, off, (U16)value);
            if (shift + count > width) {
              U32 remaining_mask = mask >> (width - shift);
              U16 next = rw(m.segment, (U16)(off + 2u));
              next = (U16)((next & ~remaining_mask) | (insert >> (width - shift)));
              ww(m.segment, (U16)(off + 2u), next);
            }
          }
        }
      }
      return;
    }
    if (extension == 0xa3 || extension == 0xab ||
        extension == 0xb3 || extension == 0xbb) { /* BT/BTS/BTR/BTC r/m,reg */
      MOD m = decode(override, address32);
      unsigned width = operand32 ? 32u : 16u;
      INT32 index = operand32 ? (INT32)r32[m.reg] : (INT16)r[m.reg];
      INT32 word = 0;
      unsigned bit = (U32)index & (width - 1u);
      if (m.mod != 3) {
        word = index / (INT32)width;
        if (index < 0 && index % (INT32)width) word--;
        m.off = (U16)(m.off + word * (INT32)(width / 8u));
      }
      U32 value = m.mod == 3 ? (width == 32 ? r32[m.rm] : r[m.rm])
                             : (width == 32 ? read32(m.segment, m.off)
                                            : read_rm(m, 1));
      flags = (flags & ~CF) | ((value & (1u << bit)) ? CF : 0);
      if (extension != 0xa3) {
        if (extension == 0xab) value |= 1u << bit;
        else if (extension == 0xb3) value &= ~(1u << bit);
        else value ^= 1u << bit;
        if (m.mod == 3) {
          if (width == 32) { r32[m.rm] = value; r[m.rm] = (U16)value; }
          else r[m.rm] = (U16)value;
        } else if (width == 32) write32(m.segment, m.off, value);
        else write_rm(m, 1, (U16)value);
      }
      return;
    }
    if (extension == 0xa0 || extension == 0xa1 ||
        extension == 0xa8 || extension == 0xa9) {
      unsigned index = extension < 0xa8 ? FS : GS;
      if (extension == 0xa0 || extension == 0xa8) push(seg[index]);
      else seg[index] = pop();
      return;
    }
    if (extension == 0x01) {
      MOD m = decode(override, address32);
      if (m.mod == 3 && m.reg != 4 && m.reg != 6) {
        fail("DOSVM: invalid descriptor-table operand\r\n");
        return;
      }
      if (m.reg <= 3 || m.reg == 4 || m.reg == 6) {
        /* SGDT/SIDT are /0 and /1; LGDT/LIDT are /2 and /3. */
        if (m.reg <= 1) {
          DOS_TABLE_REGISTER *tr = m.reg ? &current()->Idtr : &current()->Gdtr;
          ww(m.segment, m.off, (U16)tr->Limit);
          write32(m.segment, (U16)(m.off + 2), tr->Base);
        } else if (m.reg == 2 || m.reg == 3) {
          DOS_TABLE_REGISTER *tr = m.reg == 2 ? &current()->Gdtr : &current()->Idtr;
          tr->Limit = rw(m.segment, m.off);
          tr->Base = read32(m.segment, (U16)(m.off + 2));
        } else if (m.reg == 4) {
          write_rm(m, 1, (U16)current()->Cr0);
        } else if (m.reg == 6) {
          U32 next_cr0 = current()->Cr0 | (read_rm(m, 1) & 0x0fu);
          enter_protected_mode(next_cr0);
        } else {
          fail("DOSVM: unsupported 0F 01 group operation\r\n");
          return;
        }
        return;
      }
      fail("DOSVM: invalid 0F 01 group operation\r\n");
      return;
    }
    if (extension == 0x00) {
      /* DOSX probes the 386 system-selector instructions before selecting
       * its protected-mode startup path.  Model the selector-table state
       * maintained by this VM, including the non-faulting VERR/VERW tests. */
      MOD m = decode(override, address32);
      U16 selector = (U16)read_rm(m, 1);
      if (m.reg == 0) write_rm(m, 1, vm->Ldtr);       /* SLDT */
      else if (m.reg == 1) write_rm(m, 1, vm->Tr);   /* STR */
      else if (m.reg == 2) {                         /* LLDT */
        if (!load_ldt(selector))
          fail("DOSVM: invalid LDT selector\r\n");
      } else if (m.reg == 3) {                       /* LTR */
        DOS_DESCRIPTOR descriptor;
        if ((selector & 7u) || !read_descriptor(selector, &descriptor) ||
            ((descriptor.Access & 0x1fu) != 0x01u &&
             (descriptor.Access & 0x1fu) != 0x09u))
          fail("DOSVM: invalid task-register selector\r\n");
        else vm->Tr = selector;
      } else if (m.reg == 4 || m.reg == 5) {         /* VERR / VERW */
        DOS_DESCRIPTOR descriptor;
        int valid = read_descriptor(selector, &descriptor) &&
                    (descriptor.Access & 0x10u) &&
                    (m.reg == 4 ? (descriptor.Access & 2u) :
                     ((descriptor.Access & 8u) &&
                      (descriptor.Access & 2u)));
        flags = (flags & ~ZF) | (valid ? ZF : 0);
      } else {
        fail("DOSVM: invalid 0F 00 operation\r\n");
      }
      return;
    }
    if (extension == 0x20 || extension == 0x22) {
      U8 modrm = fetch8();
      unsigned cr = (modrm >> 3) & 7, reg = modrm & 7;
      if ((modrm >> 6) != 3 || cr != 0) {
        fail("DOSVM: unsupported control-register operation\r\n");
        return;
      }
      if (extension == 0x20) {
        r[reg] = (U16)current()->Cr0;
        r32[reg] = (r32[reg] & 0xffff0000u) | r[reg];
      } else {
        U32 next_cr0 = r32[reg];
        enter_protected_mode(next_cr0);
      }
      return;
    }
    /* Unimplemented 0F opcodes fault as #UD on the emulated 386. Give an
     * installed guest exception handler the original instruction address. */
    if (rw(0, 24u) || rw(0, 26u)) {
      ip = instruction_ip;
      dos_int(6);
    } else {
      KeLog("[dos] unsupported 0F %02x rep=%02x modrm=%02x at %04x:%04x AX=%04x BX=%04x CX=%04x DX=%04x DS:SI=%04x:%04x ES:DI=%04x:%04x steps=%08x\n",
            extension, rep, rb(seg[CS], ip), instruction_cs, instruction_ip,
            r[AX], r[BX], r[CX], r[DX], seg[DS], r[SI], seg[ES], r[DI],
            vm->Steps);
      fail("DOS: unsupported extended instruction\r\n");
    }
    return;
  }
  case 0x06:
    push(seg[ES]);
    return;
  case 0x07:
    seg[ES] = pop();
    return;
  case 0x0e:
    push(seg[CS]);
    return;
  case 0x16:
    push(seg[SS]);
    return;
  case 0x17:
    seg[SS] = pop();
    vm->InterruptShadow = 1;
    return;
  case 0x1e:
    push(seg[DS]);
    return;
  case 0x1f:
    seg[DS] = pop();
    return;
  case 0x68:
    if (operand32)
      push32(fetch32());
    else
      push(fetch16());
    return;
  case 0x6a:
    if (operand32)
      push32((U32)(INT32)(signed char)fetch8());
    else
      push((U16)(signed char)fetch8());
    return;
  case 0x60: {
    U16 old_sp = r[SP];
    if (operand32) {
      U32 old_esp = r32[SP];
      push32(r32[AX]); push32(r32[CX]); push32(r32[DX]); push32(r32[BX]);
      push32(old_esp); push32(r32[BP]); push32(r32[SI]); push32(r32[DI]);
    } else {
      push(r[AX]); push(r[CX]); push(r[DX]); push(r[BX]);
      push(old_sp); push(r[BP]); push(r[SI]); push(r[DI]);
    }
    return;
  }
  case 0x61:
    if (operand32) {
      r32[DI] = pop32(); r32[SI] = pop32(); r32[BP] = pop32();
      (void)pop32(); r32[BX] = pop32(); r32[DX] = pop32();
      r32[CX] = pop32(); r32[AX] = pop32();
      for (unsigned i = 0; i < 8; i++) r[i] = (U16)r32[i];
    } else {
      r[DI] = pop(); r[SI] = pop(); r[BP] = pop(); (void)pop();
      r[BX] = pop(); r[DX] = pop(); r[CX] = pop(); r[AX] = pop();
    }
    return;
  case 0x69:
  case 0x6b: {
    MOD m = decode(override, address32);
    INT32 product =
        (INT16)read_rm(m, 1) *
        (op == 0x69 ? (INT16)fetch16() : (INT32)(signed char)fetch8());
    r[m.reg] = (U16)product;
    flags &= ~(CF | OF);
    if (product < -32768 || product > 32767)
      flags |= CF | OF;
    return;
  }
  case 0x8f: {
    MOD m = decode(override, address32);
    if (m.reg) {
      /* 8F /1-/7 are reserved. DOSX probes reserved encodings while
       * selecting its CPU path; let a guest #UD handler inspect the original
       * instruction instead of terminating the whole DOS VM. */
      if (rw(0, 24u) || rw(0, 26u)) {
        ip = instruction_ip;
        dos_int(6);
      }
      return;
    }
    if (operand32) {
      U32 value = pop32();
      if (m.mod == 3) {
        r32[m.rm] = value;
        r[m.rm] = (U16)value;
      } else write32(m.segment, m.off, value);
    } else
      write_rm(m, 1, pop());
    return;
  }
  case 0x84:
  case 0x85: {
    MOD m = decode(override, address32);
    if (op == 0x85 && operand32) {
      U32 a = m.mod == 3 ? r32[m.rm] : read32(m.segment, m.off);
      alu32(4, a, r32[m.reg]);
      return;
    }
    alu(4, read_rm(m, op & 1), op & 1 ? r[m.reg] : get8(m.reg),
        op & 1 ? 16 : 8);
    return;
  }
  case 0x86:
  case 0x87: {
    MOD m = decode(override, address32);
    if (op == 0x87 && operand32) {
      U32 a = m.mod == 3 ? r32[m.rm] : read32(m.segment, m.off);
      U32 b = r32[m.reg];
      if (m.mod == 3) { r32[m.rm] = b; r[m.rm] = (U16)b; }
      else write32(m.segment, m.off, b);
      r32[m.reg] = a;
      r[m.reg] = (U16)a;
      return;
    }
    U16 v = read_rm(m, op & 1), q = op & 1 ? r[m.reg] : get8(m.reg);
    write_rm(m, op & 1, q);
    if (op & 1)
      r[m.reg] = v;
    else
      set8(m.reg, (U8)v);
    return;
  }
  case 0x8c:
  case 0x8e: {
    MOD m = decode(override, address32);
    if (m.reg > 5 || (op == 0x8e && m.reg == CS)) {
      /* Invalid MOV Sreg encodings raise #UD; extenders probe these forms. */
      if (rw(0, 24u) || rw(0, 26u)) {
        ip = instruction_ip;
        dos_int(6);
      } /* No #UD vector is installed yet; ignore this CPU probe encoding. */
      return;
    }
    if (op == 0x8c)
      write_rm(m, 1, seg[m.reg]);
    else {
      seg[m.reg] = read_rm(m, 1);
      if (m.reg == SS) vm->InterruptShadow = 1;
    }
    return;
  }
  case 0x8d: {
    MOD m = decode(override, address32);
    if (m.mod == 3) {
      fail("DOSVM: invalid LEA\r\n");
      return;
    }
    r[m.reg] = m.off;
    return;
  }
  case 0xc4:
  case 0xc5: {
    MOD m = decode(override, address32);
    if (m.mod == 3) {
      /* LES/LDS require a memory operand; register forms raise #UD. DOSX
       * probes the CPU with these encodings, so deliver the guest exception
       * when a real-mode handler is installed. */
      if (rw(0, 24u) || rw(0, 26u)) {
        ip = instruction_ip;
        dos_int(6);
      } else {
        KeLog("[dos] invalid LES/LDS modrm=%02x at %04x:%04x AX=%04x BX=%04x CX=%04x DX=%04x SI=%04x DI=%04x SS:SP=%04x:%04x\n",
              (U8)((m.mod << 6) | (m.reg << 3) | m.rm), instruction_cs,
              instruction_ip, r[AX], r[BX], r[CX], r[DX], r[SI], r[DI],
              seg[SS], r[SP]);
        fail("DOS: invalid far pointer\r\n");
      }
      return;
    }
    r[m.reg] = rw(m.segment, m.off);
    seg[op == 0xc4 ? ES : DS] = rw(m.segment, (U16)(m.off + 2));
    return;
  }
  case 0x90:
    return;
  case 0xdb: {
    MOD m = decode(override, address32);
    /* FENI, FDISI, FNCLEX, FNINIT and the legacy FSETPM are control
     * operations with no externally visible state in this no-FPU VM. */
    if (m.mod == 3 && m.reg == 4 && m.rm <= 4)
      return;
    KeLog("[dos] unsupported x87 DB /%u mod=%u rm=%u at %04x:%04x\n",
          m.reg, m.mod, m.rm, instruction_cs, instruction_ip);
    fail("DOSVM: unsupported x87 instruction\r\n");
    return;
  }
  case 0xdd: {
    MOD m = decode(override, address32);
    /* DOS4G uses FNSTSW as an FPU-presence/status probe.  The VM exposes a
     * cleared status word because it does not model a physical x87 unit. */
    if (m.mod != 3 && m.reg == 7) {
      ww(m.segment, m.off, 0);
      return;
    }
    KeLog("[dos] unsupported x87 DD /%u mod=%u rm=%u at %04x:%04x\n",
          m.reg, m.mod, m.rm, instruction_cs, instruction_ip);
    fail("DOSVM: unsupported x87 instruction\r\n");
    return;
  }
  case 0xd9: {
    MOD m = decode(override, address32);
    U16 control = current()->FpuControl ? current()->FpuControl : 0x037fu;
    if (m.mod != 3 && m.reg == 5) { /* FLDCW m16 */
      current()->FpuControl = rw(m.segment, m.off);
      return;
    }
    if (m.mod != 3 && m.reg == 7) { /* FNSTCW m16 */
      ww(m.segment, m.off, control);
      return;
    }
    KeLog("[dos] unsupported x87 D9 /%u mod=%u rm=%u at %04x:%04x\n",
          m.reg, m.mod, m.rm, instruction_cs, instruction_ip);
    fail("DOSVM: unsupported x87 instruction\r\n");
    return;
  }
  case 0x9b: /* WAIT has no external coprocessor in this personality. */
    return;
  case 0x27: /* DAA */
  case 0x2f: /* DAS */
  case 0x37: /* AAA */
  case 0x3f: /* AAS */
    r[AX] = decimal_adjust(op, r[AX], &flags);
    return;
  case 0x98:
    r[AX] = (U16)(signed char)r[AX];
    return;
  case 0x99:
    r[DX] = (r[AX] & 0x8000) ? 0xffff : 0;
    return;
  case 0x9a: {
    U32 target = operand32 ? fetch32() : fetch16();
    U16 selector = fetch16();
    if (target > 0xffffu) {
      fail("DOSVM: far CALL target exceeds real-mode CS limit\r\n");
      return;
    }
    if (operand32) {
      push32(seg[CS]);
      push32(ip);
    } else {
      push(seg[CS]);
      push(ip);
    }
    seg[CS] = selector;
    ip = (U16)target;
    return;
  }
  case 0x9c:
    if (operand32) {
      current()->Eflags = (current()->Eflags & 0xffff0000u) | flags;
      push32(current()->Eflags | 2u);
    } else
      push(flags);
    return;
  case 0x9d:
    /* 386 real mode: POPFD has a 32-bit stack slot, while reserved upper
     * EFLAGS bits remain clear. */
    if (operand32) {
      current()->Eflags = (pop32() & 0x00007fffu) | 2u;
      flags = (U16)current()->Eflags;
    } else
      flags = (pop() & 0x7fffu) | 0x0002u;
    return;
  case 0x9e:
    flags = (flags & ~0xd5u) | (get8(4) & 0xd5u);
    return;
  case 0x9f:
    set8(4, (U8)flags);
    return;
  case 0xa0:
  case 0xa1: {
    U16 o = fetch16();
    U16 s = seg[override >= 0 ? override : DS];
    if ((op & 1) && operand32) {
      r32[AX] = read32(s, o);
      r[AX] = (U16)r32[AX];
    } else if (op & 1)
      r[AX] = rw(s, o);
    else
      set8(0, rb(s, o));
    return;
  }
  case 0xa2:
  case 0xa3: {
    U16 o = fetch16();
    U16 s = seg[override >= 0 ? override : DS];
    if ((op & 1) && operand32)
      write32(s, o, r32[AX]);
    else if (op & 1)
      ww(s, o, r[AX]);
    else
      wb(s, o, (U8)r[AX]);
    return;
  }
  case 0xa8:
    alu(4, get8(0), fetch8(), 8);
    return;
  case 0xa9:
    alu(4, r[AX], fetch16(), 16);
    return;
  case 0xc2: {
    U16 n = fetch16();
    U32 target = operand32 ? pop32() : pop();
    if (target > 0xffffu) {
      fail("DOSVM: RET target exceeds real-mode CS limit\r\n");
      return;
    }
    ip = (U16)target;
    r[SP] = (U16)(r[SP] + n);
    return;
  }
  case 0xc3: {
    U32 target = operand32 ? pop32() : pop();
    if (target > 0xffffu) {
      fail("DOSVM: RET target exceeds real-mode CS limit\r\n");
      return;
    }
    ip = (U16)target;
    return;
  }
  case 0xca: {
    U16 n = fetch16();
    U32 target = operand32 ? pop32() : pop();
    U32 selector = operand32 ? pop32() : pop();
    if (target > 0xffffu) {
      fail("DOSVM: RETF target exceeds real-mode CS limit\r\n");
      return;
    }
    ip = (U16)target;
    seg[CS] = (U16)selector;
    r[SP] = (U16)(r[SP] + n);
    return;
  }
  case 0xcb: {
    U32 target = operand32 ? pop32() : pop();
    U32 selector = operand32 ? pop32() : pop();
    if (target > 0xffffu) {
      fail("DOSVM: RETF target exceeds real-mode CS limit\r\n");
      return;
    }
    ip = (U16)target;
    seg[CS] = (U16)selector;
    return;
  }
  case 0xc6:
  case 0xc7: {
    MOD m = decode(override, address32);
    if (m.reg) {
      /* C6/C7 reserve ModRM.reg=0; DOS extenders use the invalid form when
       * probing the processor and expect the #UD handler to decide. */
      if (rw(0, 24u) || rw(0, 26u)) {
        ip = instruction_ip;
        dos_int(6);
      }
      return;
    }
    if (op == 0xc7 && operand32) {
      U32 immediate = fetch32();
      if (m.mod == 3) {
        r32[m.rm] = immediate;
        r[m.rm] = (U16)immediate;
      } else write32(m.segment, m.off, immediate);
    } else
      write_rm(m, op & 1, op & 1 ? fetch16() : fetch8());
    return;
  }
  case 0xc8: {
    U16 size = fetch16();
    U8 nesting = fetch8() & 31;
    U16 old = r[BP];
    push(old);
    U16 frame = r[SP];
    for (U8 i = 1; i < nesting; i++) {
      old -= 2;
      push(rw(seg[SS], old));
    }
    if (nesting)
      push(frame);
    r[BP] = frame;
    r[SP] -= size;
    return;
  }
  case 0xc9:
    r[SP] = r[BP];
    r[BP] = pop();
    return;
  case 0xcd:
    {
      U8 vector = fetch8();
      /* The default INT 31 vector is our DPMI entry. Once a DOS extender or
       * client installs its own real-mode handler, preserve normal IVT
       * dispatch so private INT 31 services reach that handler. */
      if (vector == 0x31 &&
          rw(0, 0x31u * 4u) == DOS_BUILTIN_OFFSET(0x31) &&
          rw(0, 0x31u * 4u + 2u) == DOS_BUILTIN_SEGMENT) {
        KeLog("[dos] real INT 31h AX=%04x at %04x:%04x SS:SP=%04x:%04x\n",
              r[AX], seg[CS], ip, seg[SS], r[SP]);
        dpmi_interrupt();
      } else
        dos_int(vector);
    }
    return;
  case 0xcf:
    if (operand32) {
      U32 target = pop32();
      U32 selector = pop32();
      U32 new_flags = pop32();
      if (target > 0xffffu) {
        fail("DOSVM: IRETD target exceeds real-mode CS limit\r\n");
        return;
      }
      ip = (U16)target;
      seg[CS] = (U16)selector;
      current()->Eflags = (new_flags & 0x003f7fd7u) | 2u;
      flags = (U16)current()->Eflags;
    } else {
      ip = pop();
      seg[CS] = pop();
      flags = (pop() & 0x7fffu) | 0x0002u;
      current()->Eflags = (current()->Eflags & 0xffff0000u) | flags;
    }
    return;
  case 0xcc:
    /* A DOS program may use INT3 as an optional debugger probe. */
    if (rw(0, 12) || rw(0, 14)) dos_int(3);
    return;
  case 0xce: /* INTO: interrupt on overflow. */
    if (flags & OF) dos_int(4);
    return;
  case 0xf1: /* ICEBP / INT1: deliver a guest debug trap if installed. */
    if (rw(0, 4u) || rw(0, 6u)) dos_int(1);
    return;
  case 0xf4: /* HLT: let the host scheduler advance virtual interrupt time. */
    if (flags & 0x0200u) KeYield();
    return;
  case 0xe4: case 0xe5: case 0xec: case 0xed: {
    U16 port = (op == 0xe4 || op == 0xe5) ? fetch8() : r[DX];
    unsigned io_width = (op == 0xe4 || op == 0xec) ? 8u :
                        (operand32 ? 32u : 16u);
    U32 value = DosPortIn(current(), port, io_width);
    if (io_width == 8) set8(0, (U8)value);
    else if (io_width == 16) r[AX] = (U16)value;
    else { r32[AX] = value; r[AX] = (U16)value; }
    return;
  }
  case 0xe6: case 0xe7: case 0xee: case 0xef: {
    U16 port = (op == 0xe6 || op == 0xe7) ? fetch8() : r[DX];
    unsigned io_width = (op == 0xe6 || op == 0xee) ? 8u :
                        (operand32 ? 32u : 16u);
    U32 value = io_width == 8 ? get8(0) :
                io_width == 16 ? r[AX] : r32[AX];
    DosPortOut(current(), port, value, io_width);
    return;
  }
  case 0xe0:
  case 0xe1:
  case 0xe2:
  case 0xe3: {
    signed char d = (signed char)fetch8();
    BOOLEAN take;
    if (op == 0xe3)
      take = r[CX] == 0;
    else {
      r[CX]--;
      take = r[CX] != 0 &&
             (op == 0xe2 || (op == 0xe1 ? !!(flags & ZF) : !(flags & ZF)));
    }
    if (take)
      ip += d;
    return;
  }
  case 0xe8: {
    if (operand32) {
      INT32 displacement = (INT32)fetch32();
      INT64 target = (INT64)ip + displacement;
      if (target < 0 || target > 0xffff) {
        fail("DOSVM: 32-bit CALL target exceeds real-mode CS limit\r\n");
        return;
      }
      push32(ip);
      ip = (U16)target;
    } else {
      U16 displacement = fetch16();
      push(ip);
      ip = (U16)(ip + displacement);
    }
    return;
  }
  case 0xe9:
    if (operand32) {
      INT32 displacement = (INT32)fetch32();
      INT64 target = (INT64)ip + displacement;
      if (target < 0 || target > 0xffff) {
        fail("DOSVM: 32-bit JMP target exceeds real-mode CS limit\r\n");
        return;
      }
      ip = (U16)target;
    } else {
      U16 displacement = fetch16();
      ip = (U16)(ip + displacement);
    }
    return;
  case 0xeb: {
    signed char displacement = (signed char)fetch8();
    ip = (U16)(ip + displacement);
    return;
  }
  case 0xd0:
  case 0xd1:
  case 0xd2:
  case 0xd3: {
    MOD m = decode(override, address32);
    int wide = op & 1;
    U8 count = op & 2 ? get8(1) : 1;
    write_rm(m, wide, shift_value(m.reg, read_rm(m, wide), count, wide));
    return;
  }
  case 0xd6: /* SALC: undocumented 8086 instruction used by DOSX. */
    set8(0, (flags & CF) ? 0xffu : 0x00u);
    return;
  case 0xd7: {
    U16 source = seg[override >= 0 ? override : DS];
    set8(0, rb(source, (U16)(r[BX] + get8(0))));
    return;
  }
  case 0xd4: /* AAM imm8 */
  case 0xd5: { /* AAD imm8 */
    U8 radix = fetch8();
    U8 al = get8(0);
    if (op == 0xd4) {
      if (!radix) {
        if (rw(0, 0) || rw(0, 2)) dos_int(0);
        else fail("DOS: divide by zero\r\n");
        return;
      }
      set8(4, (U8)(al / radix));
      al = (U8)(al % radix);
      set8(0, al);
    } else {
      al = (U8)((U16)get8(4) * radix + al);
      set8(0, al);
      set8(4, 0);
    }
    flags &= ~(SF | ZF | 4u);
    if (!al) flags |= ZF;
    if (al & 0x80u) flags |= SF;
    if (parity(al)) flags |= 4u;
    return;
  }
  case 0xc0:
  case 0xc1: {
    MOD m = decode(override, address32);
    int wide = op & 1;
    U8 count = fetch8();
    write_rm(m, wide, shift_value(m.reg, read_rm(m, wide), count, wide));
    return;
  }
  case 0xf6:
  case 0xf7: {
    MOD m = decode(override, address32);
    int wide = op & 1;
    U16 v = read_rm(m, wide);
    U32 dividend, quotient;
    /* Several 16-bit programs use the 8086-compatible undocumented /1 alias
     * of TEST. Treat it like /0 (the immediate TEST operation) instead of
     * aborting the DOS VM. */
    if (m.reg == 0 || m.reg == 1) {
      alu(4, v, wide ? fetch16() : fetch8(), wide ? 16 : 8);
      return;
    }
    if (m.reg == 2) {
      write_rm(m, wide, ~v);
      return;
    }
    if (m.reg == 3) {
      write_rm(m, wide, alu(5, 0, v, wide ? 16 : 8));
      return;
    }
    if (m.reg == 4 || m.reg == 5) {
      if (wide) {
        INT32 product = m.reg == 4 ? (INT32)((U32)r[AX] * v)
                                   : (INT32)((INT16)r[AX] * (INT16)v);
        r[AX] = (U16)product;
        r[DX] = (U16)(product >> 16);
        flags &= ~(CF | OF);
        if (m.reg == 4 ? r[DX] != 0 : r[DX] != (r[AX] & 0x8000 ? 0xffff : 0))
          flags |= CF | OF;
      } else {
        INT16 product = m.reg == 4
                            ? (U16)((U8)r[AX] * (U8)v)
                            : (INT16)((signed char)r[AX] * (signed char)v);
        r[AX] = (U16)product;
        flags &= ~(CF | OF);
        if (m.reg == 4 ? (r[AX] >> 8) != 0
                       : (INT16)(signed char)r[AX] != product)
          flags |= CF | OF;
      }
      return;
    }
    if (m.reg == 6 || m.reg == 7) {
      if (!v) {
        if (rw(0, 0) || rw(0, 2)) {
          ip = instruction_ip;
          dos_int(0);
        } else {
          fail("DOS: divide by zero\r\n");
        }
        return;
      }
      dividend = wide ? ((U32)r[DX] << 16) | r[AX] : r[AX];
      if (m.reg == 6) {
        quotient = dividend / v;
        if (quotient > (wide ? 0xffffu : 0xffu)) {
          if (rw(0, 0) || rw(0, 2)) {
            ip = instruction_ip;
            dos_int(0);
          } else {
            fail("DOS: divide overflow\r\n");
          }
          return;
        }
        if (wide) {
          r[AX] = (U16)quotient;
          r[DX] = (U16)(dividend % v);
        } else
          r[AX] = (U16)((dividend % v) << 8) | quotient;
      } else {
        INT32 signed_dividend = wide ? (INT32)dividend : (INT16)dividend;
        INT32 divisor = wide ? (INT16)v : (signed char)v;
        if (signed_dividend == (INT32)0x80000000u && divisor == -1) {
          if (rw(0, 0) || rw(0, 2)) {
            ip = instruction_ip;
            dos_int(0);
          } else {
            fail("DOS: divide overflow\r\n");
          }
          return;
        }
        INT32 q = signed_dividend / divisor;
        if (q < (wide ? -32768 : -128) || q > (wide ? 32767 : 127)) {
          if (rw(0, 0) || rw(0, 2)) {
            ip = instruction_ip;
            dos_int(0);
          } else {
            fail("DOS: divide overflow\r\n");
          }
          return;
        }
        if (wide) {
          r[AX] = (U16)q;
          r[DX] = (U16)(signed_dividend % divisor);
        } else
          r[AX] = (U16)((U8)(signed_dividend % divisor) << 8) | (U8)q;
      }
      return;
    }
    fail("DOS: unsupported F6/F7 group\r\n");
    return;
  }
  case 0xea: {
    U32 target = operand32 ? fetch32() : fetch16();
    U16 selector = fetch16();
    if (target > 0xffffu) {
      fail("DOSVM: far JMP target exceeds real-mode CS limit\r\n");
      return;
    }
    seg[CS] = selector;
    ip = (U16)target;
    return;
  }
  case 0xf8:
    flags &= ~CF;
    return;
  case 0xf9:
    flags |= CF;
    return;
  case 0xf5:
    flags ^= CF;
    return;
  case 0xfc:
    flags &= ~DF;
    return;
  case 0xfd:
    flags |= DF;
    return;
  case 0xfa:
    flags &= ~0x200u;
    return;
  case 0xfb:
    flags |= 0x200u;
    return;
  case 0xfe:
  case 0xff: {
    MOD m = decode(override, address32);
    U16 cf = flags & CF;

    /* FE is the byte-sized INC/DEC group and only /0 and /1 are defined. */
    if (op == 0xfe) {
      if (m.reg > 1) {
        /* Reserved group selectors raise #UD; DOS extenders install a guest
         * handler while probing CPU behavior. On older 8086-compatible
         * clients without one, preserve their historical undefined no-op. */
        if (rw(0, 24u) || rw(0, 26u)) dos_int(6);
        return;
      }
      U16 value = read_rm(m, 0);
      U16 result = alu(m.reg ? 5u : 0u, value, 1, 8);
      flags = (flags & ~CF) | cf;
      write_rm(m, 0, result);
      return;
    }

    /* FF uses the current operand size.  Windows 1.x mixes 16-bit code with
     * 32-bit operand-size overrides for indirect calls, jumps and stack
     * arguments, so truncating these operands to a word corrupts control flow. */
    U32 value = operand32
                    ? (m.mod == 3 ? r32[m.rm] : read32(m.segment, m.off))
                    : read_rm(m, 1);
    switch (m.reg) {
    case 0: /* INC r/m16|32 */
    case 1: { /* DEC r/m16|32 */
      if (operand32) {
        U32 result = alu32(m.reg ? 5u : 0u, value, 1u);
        flags = (flags & ~CF) | cf;
        if (m.mod == 3) {
          r32[m.rm] = result;
          r[m.rm] = (U16)result;
        } else write32(m.segment, m.off, result);
      } else {
        U16 result = alu(m.reg ? 5u : 0u, (U16)value, 1, 16);
        flags = (flags & ~CF) | cf;
        write_rm(m, 1, result);
      }
      return;
    }
    case 2: { /* near indirect CALL */
      if (value > 0xffffu) {
        fail("DOSVM: 32-bit near CALL target exceeds real-mode CS limit\r\n");
        return;
      }
      if (operand32) push32(ip); else push(ip);
      ip = (U16)value;
      return;
    }
    case 3: /* far indirect CALL m16:16|32 */
    case 5: { /* far indirect JMP m16:16|32 */
      if (m.mod == 3) {
        /* Invalid FF register encodings raise #UD on x86. DOSX probes these
         * encodings during CPU detection; dispatch a guest handler if set. */
        if (rw(0, 24u) || rw(0, 26u)) dos_int(6);
        return;
      }
      U16 target_segment = rw(m.segment, (U16)(m.off + (operand32 ? 4u : 2u)));
      if (operand32 && value > 0xffffu) {
        fail("DOSVM: 32-bit far target exceeds real-mode CS limit\r\n");
        return;
      }
      if (m.reg == 3) {
        if (operand32) {
          push32(seg[CS]);
          push32(ip);
        } else {
          push(seg[CS]);
          push(ip);
        }
      }
      seg[CS] = target_segment;
      ip = (U16)value;
      return;
    }
    case 4: /* near indirect JMP */
      if (value > 0xffffu) {
        fail("DOSVM: 32-bit near JMP target exceeds real-mode CS limit\r\n");
        return;
      }
      ip = (U16)value;
      return;
    case 6: /* PUSH r/m16|32 */
      if (operand32) push32(value); else push((U16)value);
      return;
    default: /* /7 is reserved and raises #UD. */
      if (rw(0, 24u) || rw(0, 26u)) dos_int(6);
      return;
    }
  }
  default:
    KeLog("[dos] unsupported opcode 0x%02x at %04x:%04x AX=%04x SS:SP=%04x:%04x BP=%04x DS=%04x ES=%04x flags=%04x next=%02x %02x %02x %02x\n",
          op, instruction_cs, instruction_ip, r[AX], seg[SS], r[SP], r[BP],
          seg[DS], seg[ES], flags, rb(seg[CS], ip), rb(seg[CS], (U16)(ip + 1u)),
          rb(seg[CS], (U16)(ip + 2u)), rb(seg[CS], (U16)(ip + 3u)));
    fail("DOSVM: unsupported 8086 instruction\r\n");
    return;
  }
}


/* Public one-step entry for diagnostics and compatibility callers. */
void step_vm(DOS_VM *vm) { step_vm_body(vm); }

/* A short instruction batch keeps the interpreter's tight loop in this
 * translation unit. step_vm_body is forced inline here so the protected-mode
 * hot path does not pay an external C call for every emulated instruction. */
U32 DosCpuRunBatch(DOS_VM *vm, U32 max_steps) {
  if (!vm || !max_steps) return 0;
  U32 executed = 0;
  while (executed < max_steps && vm->Running) {
    vm->Steps++;
    step_vm_body(vm);
    executed++;
  }
  return executed;
}

/* Compatibility entry point for callers that run the current DOS thread. */
void step(void) { step_vm(current()); }
