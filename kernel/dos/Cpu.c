#include "Internal.h"

static U8 parity(U8 v);
static int condition(unsigned cc);
static U32 pm_register(unsigned index, unsigned width);
static void pm_set_register(unsigned index, unsigned width, U32 value);
static U16 dpmi_make_descriptor(U32 base, U32 limit, U8 access, U8 desc_flags);
static U16 dpmi_copy_descriptor(U16 function);
static int dpmi_simulate_real_interrupt(U8 vector, U32 structure,
                                        int force_builtin_dos);
static int protected_linear(unsigned index, U32 offset, U32 size,
                            int write, int execute, U32 *linear);
static int pm_write(unsigned s, U32 offset, unsigned width, U32 value);
static U32 pm_read(unsigned s, U32 offset, unsigned width);
static U32 pm_stack_pointer(void);

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
static U32 read32(U16 s, U16 o) {
  return (U32)rw(s, o) | ((U32)rw(s, (U16)(o + 2)) << 16);
}
static void write32(U16 s, U16 o, U32 v) {
  ww(s, o, (U16)v);
  ww(s, (U16)(o + 2), (U16)(v >> 16));
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
    pm_set_register(AX, 16, error);
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
    pm_set_register(AX, 16, (U16)((first << 3) | 7u));
  } else if (function == 0x0001) {
    U16 selector = (U16)r32[BX];
    DOS_DESCRIPTOR *d;
    U32 slot;
    if (!dpmi_descriptor(selector, &d, &slot)) error = 0x8022;
    else if (vm->DpmiDosDescriptor[slot]) error = 0x8022;
    else { vm->DpmiDescriptorUsed[slot] = FALSE; memset(d, 0, sizeof(*d)); }
  } else if (function == 0x0003) {
    pm_set_register(AX, 16, 8);
  } else if (function >= 0x0006 && function <= 0x0009) {
    DOS_DESCRIPTOR *d;
    if (!dpmi_descriptor((U16)r32[BX], &d, NULL)) error = 0x8022;
    else if (function == 0x0006) {
      pm_set_register(DX, 16, (U16)d->Base);
      pm_set_register(CX, 16, (U16)(d->Base >> 16));
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
      d->Flags = (U8)((d->Flags & 0xf0u) | ((value >> 8) & 0x0fu));
      d->Valid = !!(d->Access & 0x80u);
    }
  } else if (function == 0x000a) {
    DOS_DESCRIPTOR *source;
    U32 slot;
    if (!dpmi_descriptor((U16)r32[BX], &source, NULL)) error = 0x8022;
    else {
      for (slot = 0; slot < 128 && vm->DpmiDescriptorUsed[slot]; slot++);
      if (slot == DOS_MAX_BLOCKS) error = 0x8011;
      else { vm->DpmiDescriptorUsed[slot] = TRUE; vm->DpmiDescriptors[slot] = *source;
        pm_set_register(AX, 16, (U16)((slot << 3) | 7u)); }
    }
  } else if (function == 0x000b || function == 0x000c) {
    error = dpmi_copy_descriptor(function);
  } else if (function == 0x0200 || function == 0x0201) {
    U8 vector = (U8)r32[BX];
    if (function == 0x0200) {
      pm_set_register(CX, 16, rw(0, (U16)(vector * 4u + 2u)));
      pm_set_register(DX, 16, rw(0, (U16)(vector * 4u)));
    } else {
      ww(0, (U16)(vector * 4u), (U16)r32[DX]);
      ww(0, (U16)(vector * 4u + 2u), (U16)r32[CX]);
    }
  } else if (function == 0x0204 || function == 0x0205) {
    U8 vector = (U8)r32[BX];
    if (function == 0x0204) {
      U16 selector = vm->DpmiPmVectorSelector[vector];
      pm_set_register(CX, 16, selector ? selector : vm->DpmiHostSelector);
      pm_set_register(DX, vm->DpmiClient32 ? 32 : 16,
                      selector ? vm->DpmiPmVectorOffset[vector]
                               : 0x0200u + (U32)vector * 2u);
    } else {
      U16 selector = (U16)r32[CX];
      DOS_DESCRIPTOR descriptor;
      if (!read_descriptor(selector, &descriptor) ||
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
      pm_set_register(CX, 16, selector ? selector : vm->DpmiHostSelector);
      pm_set_register(DX, vm->DpmiClient32 ? 32 : 16,
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
    pm_set_register(AX, 16, 0x0100); pm_set_register(BX, 16, 0x0003);
    pm_set_register(CX, 8, 3);
    pm_set_register(DX, 16, 0x0870);
  } else if (function == 0x0500) {
    U32 structure = vm->DpmiClient32 ? r32[DI] : (U16)r32[DI];
    U32 available = MEM_SIZE - 0x00100000u;
    for (U32 i = 0; i < DOS_DPMI_MAX_BLOCKS; i++) if (vm->DpmiBlocks[i].Used) {
      if (vm->DpmiBlocks[i].Size >= available) available = 0;
      else available -= vm->DpmiBlocks[i].Size;
    }
    U32 pages = available / 4096u;
    U32 fields[9] = {available, pages, pages, MEM_SIZE / 4096u,
                     MEM_SIZE / 4096u, pages, MEM_SIZE / 4096u,
                     pages, 0xffffffffu};
    U32 linear;
    if (!protected_linear(ES, structure, 48, 1, 0, &linear)) error = 0x8022;
    else {
      for (U32 i = 0; i < 9; i++)
        if (!pm_write(ES, structure + i * 4, 32, fields[i])) { error = 0x8022; break; }
      for (U32 i = 0; !error && i < 3; i++)
        if (!pm_write(ES, structure + 36 + i * 4, 32, 0xffffffffu)) error = 0x8022;
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
      if (!collision) break;
      if (next <= base) break;
      base = next;
    }
    if (base >= MEM_SIZE || size > MEM_SIZE - base) { dpmi_result(0x8012); return; }
    vm->DpmiBlocks[slot].Base = base;
    vm->DpmiBlocks[slot].Size = size;
    vm->DpmiBlocks[slot].Used = TRUE;
    pm_set_register(BX, 16, (U16)(base >> 16));
    pm_set_register(CX, 16, (U16)base);
    pm_set_register(SI, 16, 0);
    pm_set_register(DI, 16, (U16)(slot + 1));
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
    pm_set_register(AX, 16, segment);
    pm_set_register(DX, 16, selector);
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
             !protected_linear(ES, registers_offset, 0x32u, 1, 0, &value))
      error = 0x8022;
    else {
      vm->DpmiCallbacks[slot].Used = TRUE;
      vm->DpmiCallbacks[slot].Selector = callback_selector;
      vm->DpmiCallbacks[slot].Offset = callback_offset;
      vm->DpmiCallbacks[slot].RegistersSelector = registers_selector;
      vm->DpmiCallbacks[slot].RegistersOffset = registers_offset;
      pm_set_register(CX, 16, 0xf000u);
      pm_set_register(DX, 16, (U16)(0x0600u + slot * 4u));
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
    if (!protected_linear(ES, structure, 0x32u, 0, 0, &linear)) error = 0x8022;
    else if (function == 0x0302 &&
             pm_read(ES, structure + 0x2cu, 16) == 0 &&
             pm_read(ES, structure + 0x2au, 16) == 0) {
      /* DOS16M uses a null real-mode procedure as its DPMI-to-DOS gateway:
       * the register block carries an INT 21h request. */
      if (!dpmi_simulate_real_interrupt(0x21, structure, 1)) error = 0x8022;
    } else error = 0x8001;
  } else if (function == 0x0305 || function == 0x0306) {
    pm_set_register(BX, 16, 0xf000u);
    pm_set_register(CX, 16, function == 0x0305 ? 0x0108u : 0x010au);
    pm_set_register(SI, 16, vm->DpmiHostSelector);
    pm_set_register(DI, vm->DpmiClient32 ? 32 : 16,
                    function == 0x0305 ? 0x0108u : 0x010cu);
    if (function == 0x0305) pm_set_register(AX, 16, 0);
  } else if (function == 0x0604) {
    pm_set_register(AX, 16, 4096);
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
  U16 selectors[5] = {0, 0, 0, 0, 0};
  int client32 = !!(r[AX] & 1u);
  KeLog("[dos] DPMI switch request AX=%04x BX=%04x CX=%04x SS:SP=%04x:%04x\n",
        r[AX], r[BX], r[CX], seg[SS], sp);
  if ((r[AX] & ~1u) || (client32 && !(r[BX] & 1u)) ||
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
  selectors[0] = dpmi_make_descriptor((U32)return_cs << 4,
                                      client32 ? 0xffffffffu : 0xffffu,
                                      0xfau, client32 ? 0x0cu : 0u);
  selectors[1] = dpmi_make_descriptor((U32)seg[DS] << 4,
                                      client32 ? 0xffffffffu : 0xffffu,
                                      0xf2u, client32 ? 0x0cu : 0u);
  selectors[2] = dpmi_make_descriptor((U32)seg[SS] << 4,
                                      client32 ? 0xffffffffu : 0xffffu,
                                      0xf2u, client32 ? 0x0cu : 0u);
  selectors[3] = dpmi_make_descriptor((U32)seg[ES] << 4,
                                      client32 ? 0xffffffffu : 0xffffu,
                                      0xf2u, client32 ? 0x0cu : 0u);
  selectors[4] = dpmi_make_descriptor(0xf0000u, 0xffffu, 0xfau,
                                      client32 ? 4u : 0u);
  KeLog("[dos] DPMI initial %u-bit selectors CS=%04x DS=%04x SS=%04x ES=%04x host=%04x return=%04x:%04x\n",
        client32 ? 32u : 16u, selectors[0], selectors[1], selectors[2],
        selectors[3], selectors[4], return_cs, return_ip);
  if (!selectors[0] || !selectors[1] || !selectors[2] || !selectors[3] ||
      !selectors[4]) {
    for (unsigned i = 0; i < 5; i++) if (selectors[i]) {
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
  ww(seg[SS], (U16)(sp + 2), selectors[0]);
  /* The synthetic DPMI entry is called from the 16-bit DOS extender stub.
   * The low halves are the documented real-mode register values; the high
   * halves may contain stale scratch values from 16-bit code and must not be
   * treated as flat 32-bit pointers by the newly-entered client.  Present a
   * deterministic zero-extended register frame at the 16->32-bit boundary. */
  if (client32) {
    for (U32 i = 0; i < 8; i++)
      vm->Registers32[i] = (U32)vm->Registers[i];
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
static int protected_linear(unsigned index, U32 offset, U32 size,
                            int write_access, int execute_access,
                            U32 *linear) {
  DOS_DESCRIPTOR *d;
  U32 last;
  if (index >= 6 || !size)
    return 0;
  d = &current()->SegmentCache[index];
  if (!d->Valid || !(d->Access & 0x80u) || offset > 0xffffffffu - (size - 1))
    return 0;
  last = offset + size - 1;
  U8 type = d->Access & 0x0fu;
  int executable = !!(type & 8u);
  if (execute_access ? !executable
                     : (write_access ? (executable || !(type & 2u))
                                     : (executable && !(type & 2u))))
    return 0;
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
static U8 protected_read8(unsigned s, U32 o, int execute_access) {
  U32 a;
  if (!protected_linear(s, o, 1, 0, execute_access, &a)) {
    fail(execute_access ? "DOSVM: protected instruction fetch fault\r\n"
                        : "DOSVM: protected memory read fault\r\n");
    return 0;
  }
  return mem[a];
}
static U32 protected_fetch(unsigned width) {
  DOS_VM *vm = current();
  U32 v = protected_read8(CS, vm->Eip, 1);
  if (!running)
    return 0;
  vm->Eip++;
  if (!(vm->SegmentCache[CS].Flags & 4u))
    vm->Eip &= 0xffffu;
  if (width > 1) {
    v |= (U32)protected_fetch(1) << 8;
    if (width > 2) {
      v |= (U32)protected_fetch(1) << 16;
      v |= (U32)protected_fetch(1) << 24;
    }
  }
  return v;
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
static U32 pm_register(unsigned index, unsigned width) {
  if (width == 8)
    return index < 4 ? (U8)r32[index] : (U8)(r32[index - 4] >> 8);
  return width == 16 ? (U16)r32[index] : r32[index];
}
static void pm_set_register(unsigned index, unsigned width, U32 value) {
  if (width == 8) {
    unsigned reg = index & 3;
    if (index < 4)
      r32[reg] = (r32[reg] & 0xffffff00u) | (value & 0xffu);
    else
      r32[reg] = (r32[reg] & 0xffff00ffu) | ((value & 0xffu) << 8);
  } else if (width == 16)
    r32[index] = (r32[index] & 0xffff0000u) | (value & 0xffffu);
  else
    r32[index] = value;
  unsigned low_index = width == 8 && index >= 4 ? index - 4 : index;
  r[low_index] = (U16)r32[low_index];
}
static U32 pm_read(unsigned s, U32 offset, unsigned width) {
  U32 linear;
  unsigned size = width / 8;
  if (!protected_linear(s, offset, size, 0, 0, &linear)) {
    DOS_DESCRIPTOR *d = &current()->SegmentCache[s];
    KeLog("[dos] protected read fault CS=%04x EIP=%08x op=%02x at %08x seg=%04x off=%08x width=%u base=%08x limit=%08x access=%02x flags=%02x AX=%08x CX=%08x SI=%08x DI=%08x EFLAGS=%08x\n",
          seg[CS], current()->Eip, current()->CurrentOpcode,
          current()->CurrentInstructionIp, seg[s], offset, width,
          d->Base, d->Limit, d->Access, d->Flags,
          r32[AX], r32[CX], r32[SI], r32[DI], current()->Eflags);
    U32 code = current()->SegmentCache[CS].Base +
               current()->CurrentInstructionIp - 4u;
    if (code <= MEM_SIZE - 12u)
      KeLog("[dos] fault bytes %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
            mem[code], mem[code + 1], mem[code + 2], mem[code + 3],
            mem[code + 4], mem[code + 5], mem[code + 6], mem[code + 7],
            mem[code + 8], mem[code + 9], mem[code + 10], mem[code + 11]);
    fail("DOSVM: protected memory read fault\r\n");
    return 0;
  }
  U32 value = mem[linear];
  if (size > 1)
    value |= (U32)mem[linear + 1] << 8;
  if (size > 2) {
    value |= (U32)mem[linear + 2] << 16;
    value |= (U32)mem[linear + 3] << 24;
  }
  return value;
}
static int pm_write(unsigned s, U32 offset, unsigned width, U32 value) {
  U32 linear;
  unsigned size = width / 8;
  if (!protected_linear(s, offset, size, 1, 0, &linear)) {
    fail("DOSVM: protected memory write fault\r\n");
    return 0;
  }
  for (unsigned i = 0; i < size; i++)
    mem[linear + i] = (U8)(value >> (i * 8));
  return 1;
}
static U16 dpmi_copy_descriptor(U16 function) {
  DOS_VM *vm = current();
  U16 selector = (U16)r32[BX];
  U32 structure = vm->DpmiClient32 ? r32[DI] : (U16)r32[DI];
  DOS_DESCRIPTOR *d;
  if (!dpmi_descriptor(selector, &d, NULL)) return 0x8022;
  U32 linear;
  if (!protected_linear(ES, structure, 8, function == 0x000c, 0, &linear))
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
      if (!pm_write(ES, structure + i, 8, bytes[i])) return 0x8022;
    return 0;
  }
  U8 bytes[8];
  for (U32 i = 0; i < 8; i++) bytes[i] = (U8)pm_read(ES, structure + i, 8);
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
  if (!protected_linear(ES, structure, 0x32u, 0, 0, &linear)) return 0;
  U32 values[8];
  static const U8 offsets[8] = {0x1c, 0x18, 0x14, 0x10, 0x2e, 0x08, 0x04, 0x00};
  for (U32 i = 0; i < 8; i++) values[i] = pm_read(ES, structure + offsets[i], 32);
  U16 new_flags = (U16)pm_read(ES, structure + 0x20, 16);
  U16 new_es = (U16)pm_read(ES, structure + 0x22, 16);
  U16 new_ds = (U16)pm_read(ES, structure + 0x24, 16);
  U16 new_fs = (U16)pm_read(ES, structure + 0x26, 16);
  U16 new_gs = (U16)pm_read(ES, structure + 0x28, 16);
  U16 new_sp = (U16)values[SP], new_ss = (U16)pm_read(ES, structure + 0x30, 16);
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
  if (force_builtin_dos && vector == 0x21) {
    saved_vector_off = rw(0, 0x84);
    saved_vector_seg = rw(0, 0x86);
    ww(0, 0x84, 0);
    ww(0, 0x86, 0);
  }
  dos_int(vector);
  if (force_builtin_dos && vector == 0x21) {
    ww(0, 0x84, saved_vector_off);
    ww(0, 0x86, saved_vector_seg);
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
    if (!pm_write(ES, structure + offsets[i], 32, result[i])) return 0;
  return pm_write(ES, structure + 0x20, 16, result_flags) &&
         pm_write(ES, structure + 0x22, 16, result_es) &&
         pm_write(ES, structure + 0x24, 16, result_ds) &&
         pm_write(ES, structure + 0x26, 16, result_fs) &&
         pm_write(ES, structure + 0x28, 16, result_gs) &&
         pm_write(ES, structure + 0x2e, 16, result_sp) &&
         pm_write(ES, structure + 0x30, 16, result_ss);
}
static PM_MOD pm_decode(int segment_override, int address32) {
  PM_MOD m = {0};
  U8 x = (U8)protected_fetch(1);
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
      U8 sib = (U8)protected_fetch(1);
      unsigned scale = sib >> 6, idx = (sib >> 3) & 7, b = sib & 7;
      if (idx != 4)
        index = r32[idx] << scale;
      if (m.mod == 0 && b == 5)
        has_base = 0;
      else {
        base = r32[b];
        uses_stack = b == SP || b == BP;
      }
    } else if (m.mod == 0 && m.rm == 5)
      has_base = 0;
    else {
      base = r32[m.rm];
      uses_stack = m.rm == BP || m.rm == SP;
    }
    if (!has_base)
      base = protected_fetch(4);
    if (m.mod == 1)
      base += (U32)(INT32)(signed char)protected_fetch(1);
    else if (m.mod == 2)
      base += protected_fetch(4);
    m.offset = base + index;
    if (uses_stack)
      m.segment = SS;
  } else {
    U16 base = 0;
    int uses_bp = 0;
    switch (m.rm) {
    case 0: base = r[BX] + r[SI]; break;
    case 1: base = r[BX] + r[DI]; break;
    case 2: base = r[BP] + r[SI]; uses_bp = 1; break;
    case 3: base = r[BP] + r[DI]; uses_bp = 1; break;
    case 4: base = r[SI]; break;
    case 5: base = r[DI]; break;
    case 6:
      if (m.mod == 0) base = (U16)protected_fetch(2);
      else { base = r[BP]; uses_bp = 1; }
      break;
    default: base = r[BX]; break;
    }
    if (m.mod == 1)
      base += (U16)(INT16)(signed char)protected_fetch(1);
    else if (m.mod == 2)
      base += (U16)protected_fetch(2);
    m.offset = base;
    if (uses_bp)
      m.segment = SS;
  }
  if (segment_override >= 0)
    m.segment = (U8)segment_override;
  return m;
}
static U32 pm_read_rm(PM_MOD m, unsigned width) {
  return m.is_reg ? pm_register(m.rm, width) : pm_read(m.segment, m.offset, width);
}
static int pm_write_rm(PM_MOD m, unsigned width, U32 value) {
  if (m.is_reg) {
    pm_set_register(m.rm, width, value);
    return 1;
  }
  return pm_write(m.segment, m.offset, width, value);
}
static int pm_load_segment(unsigned index, U16 selector) {
  if (!load_segment(index, selector)) {
    KeLog("[dos] protected segment load failed CS=%04x EIP=%08x op=%02x at %08x register=%u selector=%04x SP=%08x\n",
          seg[CS], current()->Eip, current()->CurrentOpcode,
          current()->CurrentInstructionIp, index, selector,
          pm_stack_pointer());
    fail("DOSVM: protected segment selector fault\r\n");
    return 0;
  }
  return 1;
}
static U32 pm_alu(unsigned operation, U32 a, U32 b, unsigned width) {
  U32 mask = width == 8 ? 0xffu : width == 16 ? 0xffffu : 0xffffffffu;
  U32 sign = width == 8 ? 0x80u : width == 16 ? 0x8000u : 0x80000000u;
  U64 wide_result;
  U32 result, old_cf = current()->Eflags & CF;
  U32 f = current()->Eflags & ~(CF | ZF | SF | OF | AF | 4u);
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
  current()->Eflags = f | 2u;
  current()->Flags = (U16)current()->Eflags;
  return result;
}
static U32 pm_shift(unsigned kind, U32 value, U32 count, unsigned width) {
  U32 mask = width == 8 ? 0xffu : width == 16 ? 0xffffu : 0xffffffffu;
  U32 sign = width == 8 ? 0x80u : width == 16 ? 0x8000u : 0x80000000u;
  U32 original = value & mask, carry = 0;
  count &= 31u;
  if (!count) return original;
  for (U32 i = 0; i < count; i++) {
    if (kind == 0 || kind == 2) {
      carry = !!(value & sign);
      value = ((value << 1) | (kind == 2 ? !!(current()->Eflags & CF) : carry)) & mask;
    } else if (kind == 1 || kind == 3) {
      carry = value & 1u;
      value = (value >> 1) | ((kind == 3 && (current()->Eflags & CF)) ? sign : 0);
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
    current()->Eflags = (current()->Eflags & ~CF) | (carry ? CF : 0);
  }
  if (kind >= 4) {
    current()->Eflags &= ~(ZF | SF | 4u);
    if (!(value & mask)) current()->Eflags |= ZF;
    if (value & sign) current()->Eflags |= SF;
    if (parity((U8)value)) current()->Eflags |= 4u;
  }
  if (count == 1) {
    current()->Eflags &= ~OF;
    if (kind == 0 || kind == 2 || kind == 4 || kind == 6)
      current()->Eflags |= (!!(value & sign) != !!carry) ? OF : 0;
    else if (kind == 5 && (original & sign))
      current()->Eflags |= OF;
    else if ((kind == 1 || kind == 3) &&
             (!!(value & sign) != !!(value & (sign >> 1))))
      current()->Eflags |= OF;
  }
  current()->Flags = (U16)current()->Eflags;
  return value & mask;
}
static U32 pm_stack_pointer(void) {
  return (current()->SegmentCache[SS].Flags & 4u) ? r32[SP] : r[SP];
}
static void pm_set_stack_pointer(U32 value) {
  if (current()->SegmentCache[SS].Flags & 4u)
    r32[SP] = value;
  else
    r32[SP] = (r32[SP] & 0xffff0000u) | (U16)value;
  r[SP] = (U16)value;
}
static int pm_push(U32 value, unsigned width) {
  U32 sp = pm_stack_pointer() - width / 8;
  pm_set_stack_pointer(sp);
  return pm_write(SS, sp, width, value);
}
static U32 pm_pop(unsigned width) {
  U32 sp = pm_stack_pointer();
  U32 value = pm_read(SS, sp, width);
  pm_set_stack_pointer(sp + width / 8);
  return value;
}
static void pm_set_near_ip(U32 target, unsigned width) {
  current()->Eip = width == 16 ? (U16)target : target;
}
static int protected_interrupt(U8 vector, int software, unsigned width) {
  DOS_VM *vm = current();
  if (vector == 0x31 && software) {
    dpmi_interrupt();
    return 1;
  }
  if (vm->DpmiPmVectorSelector[vector]) {
    U16 selector = vm->DpmiPmVectorSelector[vector];
    U32 target = vm->DpmiPmVectorOffset[vector];
    U32 old_flags = vm->Eflags;
    U16 old_cs = seg[CS];
    U32 old_ip = vm->Eip;
    unsigned frame_width = vm->DpmiClient32 ? 32 : 16;
    if (!pm_push(old_flags, frame_width) || !pm_push(old_cs, frame_width) ||
        !pm_push(old_ip, frame_width) || !pm_load_segment(CS, selector))
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
  if (!pm_push(old_flags, width) || !pm_push(old_cs, width) ||
      !pm_push(old_ip, width) || !pm_load_segment(CS, selector))
    return 0;
  vm->Eip = target;
  vm->Eflags &= ~(0x100u | 0x4000u);
  if (type == 0x06u || type == 0x0eu)
    vm->Eflags &= ~0x200u;
  vm->Flags = (U16)vm->Eflags;
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
  UINT64 phase = (UINT64)vm->DosTimerPhase + (UINT64)elapsed * 182u;
  /* The 8259 holds one pending edge while masked; it does not queue an
   * unbounded backlog of timer interrupts for a suspended DOS client. */
  vm->DosTimerPhase = phase >= 1000u ? 1000u : (UINT32)phase;
}

static int dos_timer_irq_due(DOS_VM *vm) {
  if (vm->DosTimerPhase < 1000u) return 0;
  vm->DosTimerPhase -= 1000u;
  return 1;
}

static int dos_dispatch_hardware(void) {
  DOS_VM *vm = current();
  dos_timer_update(vm);
  if (!(vm->Eflags & 0x200u) && vm->ProtectedMode)
    return 0;

  U8 timer_vector = (U8)(vm->PicMasterVectorBase + 0u);
  U8 keyboard_vector = (U8)(vm->PicMasterVectorBase + 1u);
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
  }
  return 0;
}

static void step_protected(void) {
  DOS_VM *vm = current();
  if (seg[CS] == vm->DpmiHostSelector && vm->Eip == 0x010cu) {
    dpmi_raw_to_real();
    return;
  }
  U8 cs_flags = vm->SegmentCache[CS].Flags;
  vm->CurrentInstructionIp = vm->Eip;
  int operand32 = !!(cs_flags & 4u), address32 = !!(cs_flags & 4u);
  int segment_override = -1, rep = 0;
  U8 op = (U8)protected_fetch(1);
  for (unsigned count = 0; count < 15 && running; count++) {
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
    op = (U8)protected_fetch(1);
  }
  if (!running) return;
  vm->CurrentOpcode = op;
  unsigned width = operand32 ? 32 : 16;

  /* DOS/4G clients use IN/OUT for VGA, PIT, PIC and the 8042. Never execute
   * host port instructions: route every guest access through the virtual PC. */
  if (op == 0xe4 || op == 0xe5 || op == 0xec || op == 0xed) {
    U16 port = (op == 0xe4 || op == 0xe5)
                   ? (U16)protected_fetch(1) : (U16)pm_register(DX, 16);
    unsigned io_width = (op == 0xe4 || op == 0xec) ? 8u : width;
    pm_set_register(AX, io_width, DosPortIn(vm, port, io_width));
    return;
  }
  if (op == 0xe6 || op == 0xe7 || op == 0xee || op == 0xef) {
    U16 port = (op == 0xe6 || op == 0xe7)
                   ? (U16)protected_fetch(1) : (U16)pm_register(DX, 16);
    unsigned io_width = (op == 0xe6 || op == 0xee) ? 8u : width;
    DosPortOut(vm, port, pm_register(AX, io_width), io_width);
    return;
  }
  if (op >= 0x6c && op <= 0x6f) {
    unsigned io_width = (op & 1u) ? width : 8u;
    unsigned bytes = io_width / 8u;
    unsigned address_width = address32 ? 32u : 16u;
    U32 count = rep ? pm_register(CX, address_width) : 1u;
    U32 si = pm_register(SI, address_width);
    U32 di = pm_register(DI, address_width);
    U16 port = (U16)pm_register(DX, 16);
    unsigned source_segment = segment_override >= 0 ?
                              (unsigned)segment_override : DS;
    if (count > 0x100000u) {
      fail("DOSVM: excessive protected string I/O count\r\n");
      return;
    }
    for (U32 i = 0; i < count && running; i++) {
      if (op == 0x6c || op == 0x6d) {
        U32 value = DosPortIn(vm, port, io_width);
        if (!pm_write(ES, di, io_width, value)) return;
      } else {
        U32 value = pm_read(source_segment, si, io_width);
        if (!running) return;
        DosPortOut(vm, port, value, io_width);
      }
      INT32 delta = (vm->Eflags & DF) ? -(INT32)bytes : (INT32)bytes;
      if (op == 0x6c || op == 0x6d)
        di = address_width == 32 ? di + (U32)delta : (U16)(di + delta);
      else
        si = address_width == 32 ? si + (U32)delta : (U16)(si + delta);
    }
    if (op == 0x6c || op == 0x6d)
      pm_set_register(DI, address_width, di);
    else
      pm_set_register(SI, address_width, si);
    if (rep) pm_set_register(CX, address_width, 0);
    return;
  }

  if (op == 0x06 || op == 0x0e || op == 0x16 || op == 0x1e) {
    unsigned index = op == 0x06 ? ES : op == 0x0e ? CS : op == 0x16 ? SS : DS;
    pm_push(seg[index], width);
    return;
  }
  if (op == 0x07 || op == 0x17 || op == 0x1f) {
    unsigned index = op == 0x07 ? ES : op == 0x17 ? SS : DS;
    pm_load_segment(index, (U16)pm_pop(width));
    return;
  }
  if (op >= 0xb0 && op <= 0xb7) {
    pm_set_register(op & 7, 8, protected_fetch(1));
    return;
  }
  if (op >= 0xb8 && op <= 0xbf) {
    pm_set_register(op & 7, width, protected_fetch(width / 8));
    return;
  }
  if (op <= 0x3d && ((op & 7u) == 4u || (op & 7u) == 5u)) {
    unsigned alu_width = (op & 1u) ? width : 8u;
    unsigned operation = (op >> 3) & 7u;
    U32 immediate = protected_fetch(alu_width / 8u);
    U32 result = pm_alu(operation, pm_register(AX, alu_width),
                        immediate, alu_width);
    if (operation != 7u) pm_set_register(AX, alu_width, result);
    return;
  }
  if (op >= 0x50 && op <= 0x57) {
    pm_push(pm_register(op & 7, width), width);
    return;
  }
  if (op >= 0x58 && op <= 0x5f) {
    pm_set_register(op & 7, width, pm_pop(width));
    return;
  }
  if (op >= 0x40 && op <= 0x4f) {
    unsigned reg = op & 7;
    U32 old_cf = vm->Eflags & CF;
    U32 result = pm_alu(op < 0x48 ? 0 : 5, pm_register(reg, width), 1, width);
    vm->Eflags = (vm->Eflags & ~CF) | old_cf;
    pm_set_register(reg, width, result);
    return;
  }
  if (op == 0xfe) {
    PM_MOD m = pm_decode(segment_override, address32);
    if (m.reg > 1u) { fail("DOSVM: invalid protected FE group\r\n"); return; }
    U32 old_cf = vm->Eflags & CF;
    U32 value = pm_alu(m.reg ? 5u : 0u, pm_read_rm(m, 8), 1, 8);
    vm->Eflags = (vm->Eflags & ~CF) | old_cf;
    pm_write_rm(m, 8, value);
    return;
  }
  if (op == 0xf6 || op == 0xf7) {
    PM_MOD m = pm_decode(segment_override, address32);
    unsigned operand_width = op == 0xf6 ? 8u : width;
    U32 value = pm_read_rm(m, operand_width);
    if (m.reg <= 1u) {
      pm_alu(4, value, protected_fetch(operand_width / 8u), operand_width);
    } else if (m.reg == 2u) {
      pm_write_rm(m, operand_width, ~value);
    } else if (m.reg == 3u) {
      pm_write_rm(m, operand_width, pm_alu(5, 0, value, operand_width));
    } else if (m.reg == 4u || m.reg == 5u) {
      U64 product;
      int overflow;
      if (m.reg == 4u) {
        product = (U64)pm_register(AX, operand_width) * value;
        overflow = !!(product >> operand_width);
      } else {
        INT64 left = operand_width == 8 ? (INT8)r[AX]
                   : operand_width == 16 ? (INT16)r[AX] : (INT32)r32[AX];
        INT64 right = operand_width == 8 ? (INT8)value
                    : operand_width == 16 ? (INT16)value : (INT32)value;
        INT64 signed_product = left * right;
        product = (U64)signed_product;
        overflow = operand_width == 8 ? signed_product < -128 || signed_product > 127
                 : operand_width == 16 ? signed_product < -32768 || signed_product > 32767
                 : signed_product < -2147483648LL || signed_product > 2147483647LL;
      }
      if (operand_width == 8)
        pm_set_register(AX, 16, (U16)product);
      else {
        pm_set_register(AX, operand_width, (U32)product);
        pm_set_register(DX, operand_width, (U32)(product >> operand_width));
      }
      vm->Eflags = (vm->Eflags & ~(CF | OF)) | (overflow ? CF | OF : 0);
      vm->Flags = (U16)vm->Eflags;
    } else {
      if (!value) { fail("DOSVM: protected divide by zero\r\n"); return; }
      U64 dividend = operand_width == 8 ? r[AX]
                   : operand_width == 16 ? ((U32)r[DX] << 16) | r[AX]
                   : ((U64)r32[DX] << 32) | r32[AX];
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
        pm_set_register(AX, 16, (U16)(((U8)remainder << 8) | (U8)quotient));
      else {
        pm_set_register(AX, operand_width, (U32)quotient);
        pm_set_register(DX, operand_width, (U32)remainder);
      }
    }
    return;
  }
  if (op >= 0x70 && op <= 0x7f) {
    INT32 displacement = (INT8)protected_fetch(1);
    if (condition(op & 15)) pm_set_near_ip(vm->Eip + displacement, width);
    return;
  }
  if (op == 0x90) return;
  if (op >= 0x91 && op <= 0x97) {
    U32 old_accumulator = pm_register(AX, width);
    U32 old_operand = pm_register(op & 7u, width);
    pm_set_register(AX, width, old_operand);
    pm_set_register(op & 7u, width, old_accumulator);
    return;
  }
  if (op == 0x98) {
    pm_set_register(AX, width, operand32 ? (U32)(INT32)(INT16)r[AX]
                                           : (U16)(INT16)(INT8)r[AX]);
    return;
  }
  if (op == 0x99) {
    pm_set_register(DX, width, operand32 ? (r32[AX] & 0x80000000u ? 0xffffffffu : 0u)
                                           : (r[AX] & 0x8000u ? 0xffffu : 0u));
    return;
  }
  /* The DOS4G stub and many 16-bit runtimes issue these legacy x87
   * control instructions during startup.  DOSVM has no hardware FPU state;
   * the no-op controls below have no observable effect for this VM. */
  if (op == 0xdb) {
    PM_MOD m = pm_decode(segment_override, address32);
    if (m.is_reg && m.reg == 4 && m.rm <= 4) return; /* FENI..FSETPM */
    KeLog("[dos] unsupported x87 DB /%u mod=%u rm=%u at CS=%04x EIP=%08x\n",
          m.reg, m.mod, m.rm, seg[CS], vm->Eip);
    fail("DOSVM: unsupported x87 instruction\r\n");
    return;
  }
  if (op == 0xdd) {
    PM_MOD m = pm_decode(segment_override, address32);
    if (!m.is_reg && m.reg == 7) { /* FNSTSW m16 */
      pm_write_rm(m, 16, 0);
      return;
    }
    KeLog("[dos] unsupported x87 DD /%u mod=%u rm=%u at CS=%04x EIP=%08x\n",
          m.reg, m.mod, m.rm, seg[CS], vm->Eip);
    fail("DOSVM: unsupported x87 instruction\r\n");
    return;
  }
  if (op == 0xd9) {
    PM_MOD m = pm_decode(segment_override, address32);
    U16 control = vm->FpuControl ? vm->FpuControl : 0x037fu;
    if (!m.is_reg && m.reg == 5) { /* FLDCW m16 */
      vm->FpuControl = (U16)pm_read_rm(m, 16);
      return;
    }
    if (!m.is_reg && m.reg == 7) { /* FNSTCW m16 */
      pm_write_rm(m, 16, control);
      return;
    }
    KeLog("[dos] unsupported x87 D9 /%u mod=%u rm=%u at CS=%04x EIP=%08x\n",
          m.reg, m.mod, m.rm, seg[CS], vm->Eip);
    fail("DOSVM: unsupported x87 instruction\r\n");
    return;
  }
  if (op == 0x68 || op == 0x6a) {
    U32 value = op == 0x68 ? protected_fetch(width / 8)
                           : (U32)(INT32)(INT8)protected_fetch(1);
    pm_push(value, width);
    return;
  }
  if (op == 0x60) {
    U32 original_sp = operand32 ? r32[SP] : r[SP];
    if (!pm_push(pm_register(AX, width), width) ||
        !pm_push(pm_register(CX, width), width) ||
        !pm_push(pm_register(DX, width), width) ||
        !pm_push(pm_register(BX, width), width) ||
        !pm_push(original_sp, width) ||
        !pm_push(pm_register(BP, width), width) ||
        !pm_push(pm_register(SI, width), width) ||
        !pm_push(pm_register(DI, width), width))
      return;
    return;
  }
  if (op == 0x61) {
    pm_set_register(DI, width, pm_pop(width)); pm_set_register(SI, width, pm_pop(width));
    pm_set_register(BP, width, pm_pop(width)); (void)pm_pop(width);
    pm_set_register(BX, width, pm_pop(width)); pm_set_register(DX, width, pm_pop(width));
    pm_set_register(CX, width, pm_pop(width)); pm_set_register(AX, width, pm_pop(width));
    return;
  }
  if (op == 0x9c) { pm_push(vm->Eflags | 2u, width); return; }
  if (op == 0x9d) {
    U32 value = pm_pop(width);
    vm->Eflags = width == 16 ? ((vm->Eflags & 0xffff0000u) | (value & 0xffffu))
                            : value;
    vm->Eflags |= 2u;
    vm->Flags = (U16)vm->Eflags;
    return;
  }
  if (op >= 0xa0 && op <= 0xa3) {
    U32 offset = protected_fetch(address32 ? 4 : 2);
    unsigned w = (op & 1u) ? width : 8;
    if (op == 0xa0 || op == 0xa1)
      pm_set_register(AX, w, pm_read(segment_override >= 0 ? segment_override : DS,
                                     offset, w));
    else
      pm_write(segment_override >= 0 ? segment_override : DS, offset, w,
               pm_register(AX, w));
    return;
  }
  if (op == 0xa8 || op == 0xa9) {
    unsigned w = op == 0xa8 ? 8 : width;
    pm_alu(4, pm_register(AX, w), protected_fetch(w / 8), w);
    return;
  }
  if (op >= 0xa4 && op <= 0xaf && op != 0xa8 && op != 0xa9) {
    unsigned element = (op & 1u) ? width / 8 : 1;
    U32 count = rep ? (address32 ? r32[CX] : r[CX]) : 1;
    if (count > 0x100000u) {
      fail("DOSVM: protected string instruction count is excessive\r\n");
      return;
    }
    U32 si = address32 ? r32[SI] : r[SI], di = address32 ? r32[DI] : r[DI];
    while (count-- && running) {
      unsigned source_segment = segment_override >= 0 ? segment_override : DS;
      U32 a = 0, b = 0;
      if (op <= 0xa7 || (op >= 0xac && op <= 0xad))
        a = pm_read(source_segment, si, element * 8);
      if (op == 0xa4 || op == 0xa5)
        pm_write(ES, di, element * 8, a);
      else if (op == 0xa6 || op == 0xa7) {
        b = pm_read(ES, di, element * 8);
        pm_alu(7, a, b, element * 8);
      } else if (op == 0xaa || op == 0xab)
        pm_write(ES, di, element * 8, pm_register(AX, element * 8));
      else if (op == 0xac || op == 0xad)
        pm_set_register(AX, element * 8, a);
      else if (op == 0xae || op == 0xaf) {
        b = pm_read(ES, di, element * 8);
        pm_alu(7, pm_register(AX, element * 8), b, element * 8);
      }
      INT32 delta = (vm->Eflags & DF) ? -(INT32)element : (INT32)element;
      if (op <= 0xa7 || (op >= 0xac && op <= 0xad))
        si += delta;
      if (op <= 0xa7 || (op >= 0xaa && op <= 0xab) ||
          (op >= 0xae && op <= 0xaf))
        di += delta;
      pm_set_register(SI, address32 ? 32 : 16, si);
      if (op <= 0xa7 || (op >= 0xaa && op <= 0xab) ||
          (op >= 0xae && op <= 0xaf))
        pm_set_register(DI, address32 ? 32 : 16, di);
      if (rep) {
        pm_set_register(CX, address32 ? 32 : 16,
                        (address32 ? r32[CX] : r[CX]) - 1);
        if ((op == 0xa6 || op == 0xa7 || op == 0xae || op == 0xaf) &&
            ((rep == 0xf3) != !!(vm->Eflags & ZF)))
          break;
      }
    }
    return;
  }
  if (op == 0x89 || op == 0x8b || op == 0x88 || op == 0x8a ||
      op == 0x84 || op == 0x85 || op == 0x86 || op == 0x87 ||
      op == 0x8d || op == 0xc6 || op == 0xc7 ||
      (op <= 0x3b && (op & 4u) == 0 && (op & 7u) <= 3) ||
      op == 0x80 || op == 0x81 || op == 0x82 || op == 0x83) {
    PM_MOD m = pm_decode(segment_override, address32);
    if (op == 0x8d) {
      if (m.is_reg) fail("DOSVM: invalid protected LEA operand\r\n");
      else pm_set_register(m.reg, width, m.offset);
      return;
    }
    unsigned w = width;
    if ((op <= 0x3b && !(op & 1u)) || op == 0x84 || op == 0x86 ||
        op == 0x88 || op == 0x8a || op == 0x80 || op == 0x82 || op == 0xc6)
      w = 8;
    U32 a = pm_read_rm(m, w), b = pm_register(m.reg, w), result;
    if (op == 0x89 || op == 0x88) pm_write_rm(m, w, b);
    else if (op == 0x8b || op == 0x8a) pm_set_register(m.reg, w, a);
    else if (op == 0x86 || op == 0x87) {
      pm_write_rm(m, w, b); pm_set_register(m.reg, w, a);
    } else if (op == 0xc6 || op == 0xc7) {
      if (m.reg) fail("DOSVM: invalid protected MOV group\r\n");
      else pm_write_rm(m, w, protected_fetch(w / 8));
    } else if (op >= 0x80 && op <= 0x83) {
      U32 immediate = op == 0x83 ? (U32)(INT32)(INT8)protected_fetch(1)
                       : (op == 0x80 || op == 0x82) ? protected_fetch(1)
                                                   : protected_fetch(width / 8);
      result = pm_alu(m.reg, a, immediate, w);
      if (m.reg != 7) pm_write_rm(m, w, result);
    } else if (op == 0x84 || op == 0x85) {
      pm_alu(4, a, b, w);
    } else {
      unsigned operation = (op >> 3) & 7u;
      int direction = (op & 2) != 0;
      result = pm_alu(operation, direction ? b : a, direction ? a : b, w);
      if (operation != 7) {
        if (direction) pm_set_register(m.reg, w, result);
        else pm_write_rm(m, w, result);
      }
    }
    return;
  }
  if (op == 0x8c || op == 0x8e) {
    PM_MOD m = pm_decode(segment_override, address32);
    if (m.reg >= 6 || (op == 0x8e && m.reg == CS)) {
      fail("DOSVM: invalid protected segment register\r\n");
      return;
    }
    if (op == 0x8c) pm_write_rm(m, 16, seg[m.reg]);
    else pm_load_segment(m.reg, (U16)pm_read_rm(m, 16));
    return;
  }
  if (op == 0x8f) {
    PM_MOD m = pm_decode(segment_override, address32);
    if (m.reg) fail("DOSVM: invalid protected POP operand\r\n");
    else pm_write_rm(m, width, pm_pop(width));
    return;
  }
  if (op == 0xc4 || op == 0xc5) {
    PM_MOD m = pm_decode(segment_override, address32);
    if (m.is_reg) {
      fail("DOSVM: far pointer load requires memory\r\n");
      return;
    }
    U32 offset = pm_read(m.segment, m.offset, width);
    U16 selector = (U16)pm_read(m.segment, m.offset + width / 8u, 16);
    if (pm_load_segment(op == 0xc4 ? ES : DS, selector))
      pm_set_register(m.reg, width, offset);
    return;
  }
  if (op == 0xd0 || op == 0xd1 || op == 0xd2 || op == 0xd3 ||
      op == 0xc0 || op == 0xc1) {
    PM_MOD m = pm_decode(segment_override, address32);
    unsigned w = (op & 1u) ? width : 8;
    U32 count = (op == 0xd0 || op == 0xd1) ? 1
                : (op == 0xd2 || op == 0xd3) ? (U8)r32[CX]
                                               : (U8)protected_fetch(1);
    pm_write_rm(m, w, pm_shift(m.reg, pm_read_rm(m, w), count, w));
    return;
  }
  if (op == 0xe8 || op == 0xe9) {
    INT32 displacement = width == 32 ? (INT32)protected_fetch(4)
                                     : (INT16)protected_fetch(2);
    if (op == 0xe8) pm_push(vm->Eip, width);
    pm_set_near_ip(vm->Eip + displacement, width);
    return;
  }
  if (op == 0xeb) {
    INT8 displacement = (INT8)protected_fetch(1);
    pm_set_near_ip(vm->Eip + displacement,
                   (cs_flags & 4u) ? 32 : 16);
    return;
  }
  if (op >= 0xe0 && op <= 0xe3) {
    INT8 displacement = (INT8)protected_fetch(1);
    U32 count = address32 ? r32[CX] : r[CX];
    int branch;
    if (op == 0xe3) branch = count == 0;
    else {
      count = address32 ? count - 1u : (U16)(count - 1u);
      pm_set_register(CX, address32 ? 32 : 16, count);
      branch = count != 0 && (op == 0xe2 ||
               (op == 0xe1 ? !!(vm->Eflags & ZF) : !(vm->Eflags & ZF)));
    }
    if (branch) pm_set_near_ip(vm->Eip + displacement,
                               (cs_flags & 4u) ? 32 : 16);
    return;
  }
  if (op == 0xc8) {
    U16 size = (U16)protected_fetch(2);
    U8 nesting = (U8)protected_fetch(1) & 31u;
    U32 old = pm_register(BP, width);
    if (!pm_push(old, width)) return;
    U32 frame = pm_stack_pointer();
    for (U8 i = 1; i < nesting; i++) {
      old -= width / 8u;
      if (!pm_push(pm_read(SS, old, width), width)) return;
    }
    if (nesting && !pm_push(frame, width)) return;
    pm_set_register(BP, width, frame);
    pm_set_stack_pointer(pm_stack_pointer() - size);
    return;
  }
  if (op == 0xc9) {
    pm_set_stack_pointer(pm_register(BP, width));
    pm_set_register(BP, width, pm_pop(width));
    return;
  }
  if (op == 0xc3 || op == 0xc2) {
    U32 target = pm_pop(width);
    if (op == 0xc2) pm_set_stack_pointer(pm_stack_pointer() + (U16)protected_fetch(2));
    vm->Eip = target;
    return;
  }
  if (op == 0xcb || op == 0xca) {
    U32 target = pm_pop(width);
    U16 selector = (U16)pm_pop(width);
    if (op == 0xca) pm_set_stack_pointer(pm_stack_pointer() + (U16)protected_fetch(2));
    if (pm_load_segment(CS, selector)) vm->Eip = target;
    return;
  }
  if (op == 0xcf) {
    U32 target = pm_pop(width);
    U16 selector = (U16)pm_pop(width);
    U32 new_flags = pm_pop(width);
    if (pm_load_segment(CS, selector)) {
      vm->Eip = target;
      vm->Eflags = width == 16 ? ((vm->Eflags & 0xffff0000u) | (new_flags & 0xffffu))
                              : new_flags;
      vm->Eflags |= 2u;
      vm->Flags = (U16)vm->Eflags;
    }
    return;
  }
  if (op == 0x9a) {
    U32 target = protected_fetch(width / 8);
    U16 selector = (U16)protected_fetch(2);
    if (pm_push(seg[CS], width) && pm_push(vm->Eip, width) &&
        pm_load_segment(CS, selector))
      vm->Eip = target;
    return;
  }
  if (op == 0xff) {
    PM_MOD m = pm_decode(segment_override, address32);
    U32 value = pm_read_rm(m, width);
    if (m.reg == 0 || m.reg == 1) {
      U32 old_cf = vm->Eflags & CF;
      U32 result = pm_alu(m.reg ? 5 : 0, value, 1, width);
      vm->Eflags = (vm->Eflags & ~CF) | old_cf;
      pm_write_rm(m, width, result);
    } else if (m.reg == 2 || m.reg == 4) {
      if (m.reg == 2 && !pm_push(vm->Eip, width)) return;
      vm->Eip = value;
    } else if ((m.reg == 3 || m.reg == 5) && !m.is_reg) {
      U16 selector = (U16)pm_read(m.segment, m.offset + width / 8, 16);
      if (m.reg == 3 &&
          (!pm_push(seg[CS], width) || !pm_push(vm->Eip, width)))
        return;
      if (pm_load_segment(CS, selector)) vm->Eip = value;
    } else if (m.reg == 6)
      pm_push(value, width);
    else
      fail("DOSVM: invalid protected FF group operation\r\n");
    return;
  }
  if (op == 0xea) {
    U32 target = protected_fetch(width / 8);
    U16 selector = (U16)protected_fetch(2);
    if (pm_load_segment(CS, selector)) vm->Eip = target;
    return;
  }
  if (op == 0xfa || op == 0xfb) {
    /* DPMI clients manipulate a virtual interrupt flag at ring 3. */
    if (op == 0xfa) vm->Eflags &= ~0x200u; else vm->Eflags |= 0x200u;
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
    U8 second = (U8)protected_fetch(1);
    if (second == 0x02 || second == 0x03) {
      PM_MOD m = pm_decode(segment_override, address32);
      U16 selector = (U16)pm_read_rm(m, 16);
      DOS_DESCRIPTOR descriptor;
      U8 cpl = seg[CS] & 3u, rpl = selector & 3u;
      if (read_descriptor(selector, &descriptor) &&
          (descriptor.Access & 0x10u) &&
          ((descriptor.Access >> 5) & 3u) >= (cpl > rpl ? cpl : rpl)) {
        U32 result = second == 0x02
                     ? ((U32)descriptor.Access << 8) |
                       ((U32)(descriptor.Flags & 0x0fu) << 20)
                     : descriptor.Limit;
        pm_set_register(m.reg, width, result);
        vm->Eflags |= ZF;
      } else vm->Eflags &= ~ZF;
      vm->Flags = (U16)vm->Eflags;
      return;
    }
    if (second >= 0x80 && second <= 0x8f) {
      INT32 displacement = width == 32 ? (INT32)protected_fetch(4)
                                       : (INT16)protected_fetch(2);
      if (condition(second & 15)) pm_set_near_ip(vm->Eip + displacement, width);
      return;
    }
    if (second == 0x31) { /* RDTSC: deterministic virtual instruction counter */
      U64 counter = (U64)vm->Steps * 1000ULL + vm->Eip;
      pm_set_register(AX, 32, (U32)counter);
      pm_set_register(DX, 32, (U32)(counter >> 32));
      return;
    }
    if (second >= 0x90 && second <= 0x9f) { /* SETcc r/m8 */
      PM_MOD m = pm_decode(segment_override, address32);
      pm_write_rm(m, 8, condition(second & 15u) ? 1u : 0u);
      return;
    }
    if (second == 0xa4 || second == 0xa5 ||
        second == 0xac || second == 0xad) { /* SHLD/SHRD */
      PM_MOD m = pm_decode(segment_override, address32);
      U32 destination = pm_read_rm(m, width);
      U32 source = pm_register(m.reg, width);
      U32 count = (second == 0xa4 || second == 0xac)
                    ? protected_fetch(1) : (U8)r32[CX];
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
      pm_write_rm(m, width, result);
      return;
    }
    if (second == 0xb0 || second == 0xb1) { /* CMPXCHG r/m,reg */
      PM_MOD m = pm_decode(segment_override, address32);
      unsigned w = second == 0xb0 ? 8u : width;
      U32 accumulator = pm_register(AX, w);
      U32 destination = pm_read_rm(m, w);
      (void)pm_alu(5, accumulator, destination, w);
      if (vm->Eflags & ZF)
        pm_write_rm(m, w, pm_register(m.reg, w));
      else
        pm_set_register(AX, w, destination);
      return;
    }
    if (second == 0xc0 || second == 0xc1) { /* XADD r/m,reg */
      PM_MOD m = pm_decode(segment_override, address32);
      unsigned w = second == 0xc0 ? 8u : width;
      U32 destination = pm_read_rm(m, w);
      U32 source = pm_register(m.reg, w);
      U32 result = pm_alu(0, destination, source, w);
      pm_write_rm(m, w, result);
      pm_set_register(m.reg, w, destination);
      return;
    }
    if (second >= 0xc8 && second <= 0xcf) { /* BSWAP r32 */
      unsigned reg = second & 7u;
      U32 value = r32[reg];
      value = ((value & 0x000000ffu) << 24) |
              ((value & 0x0000ff00u) << 8)  |
              ((value & 0x00ff0000u) >> 8)  |
              ((value & 0xff000000u) >> 24);
      r32[reg] = value;
      r[reg] = (U16)value;
      return;
    }
    if (second == 0xa2) {
      U32 leaf = r32[AX];
      r32[AX] = leaf ? 0 : 1;
      r32[BX] = 0x756e6547u; /* GenuineIntel */
      r32[DX] = 0x49656e69u;
      r32[CX] = 0x6c65746eu;
      if (!leaf) { r32[AX] = 1; r32[CX] = 0; r32[DX] = 0; }
      for (unsigned i = 0; i < 8; i++) r[i] = (U16)r32[i];
      return;
    }
    if (second == 0xb6 || second == 0xb7 || second == 0xbe || second == 0xbf) {
      PM_MOD m = pm_decode(segment_override, address32);
      unsigned source_width = (second == 0xb6 || second == 0xbe) ? 8 : 16;
      U32 value = pm_read_rm(m, source_width);
      if (second == 0xbe)
        value = (U32)(INT32)(INT8)value;
      else if (second == 0xbf)
        value = (U32)(INT32)(INT16)value;
      pm_set_register(m.reg, width, value);
      return;
    }
    if (second == 0xaf) {
      PM_MOD m = pm_decode(segment_override, address32);
      INT64 product = (width == 32
                           ? (INT64)(INT32)r32[m.reg] * (INT32)pm_read_rm(m, width)
                           : (INT32)(INT16)r32[m.reg] * (INT16)pm_read_rm(m, width));
      U32 result = (U32)product;
      pm_set_register(m.reg, width, result);
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
      PM_MOD m = pm_decode(segment_override, address32);
      INT32 bit_index = (INT32)pm_register(m.reg, width);
      U32 bit = (U32)bit_index & (width - 1), value;
      if (!m.is_reg) {
        INT32 bits_per_word = (INT32)width;
        INT32 word_index = bit_index / bits_per_word;
        if (bit_index < 0 && bit_index % bits_per_word) word_index--;
        m.offset += (U32)(word_index * (INT32)(width / 8));
        bit = (U32)(bit_index - word_index * bits_per_word);
      }
      value = pm_read_rm(m, width);
      vm->Eflags = (vm->Eflags & ~CF) | ((value & (1u << bit)) ? CF : 0);
      if (second != 0xa3) {
        if (second == 0xab) value |= 1u << bit;
        else if (second == 0xb3) value &= ~(1u << bit);
        else value ^= 1u << bit;
        pm_write_rm(m, width, value);
      }
      vm->Flags = (U16)vm->Eflags;
      return;
    }
    if (second == 0xba) {
      PM_MOD m = pm_decode(segment_override, address32);
      U8 operation = m.reg;
      U32 bit_index = protected_fetch(1) & (width - 1);
      U32 value = pm_read_rm(m, width);
      vm->Eflags = (vm->Eflags & ~CF) | ((value & (1u << bit_index)) ? CF : 0);
      if (operation >= 5) {
        if (operation == 5) value |= 1u << bit_index;
        else if (operation == 6) value &= ~(1u << bit_index);
        else value ^= 1u << bit_index;
        pm_write_rm(m, width, value);
      } else if (operation != 4)
        fail("DOSVM: invalid protected bit-test group\r\n");
      vm->Flags = (U16)vm->Eflags;
      return;
    }
    if (second == 0x01) {
      PM_MOD m = pm_decode(segment_override, address32);
      if ((seg[CS] & 3u) && m.reg >= 2) {
        fail("DOSVM: privileged descriptor instruction fault\r\n"); return;
      }
      if (m.reg == 0 || m.reg == 1) {
        DOS_TABLE_REGISTER *tr = m.reg ? &vm->Idtr : &vm->Gdtr;
        U8 bytes[6] = {(U8)tr->Limit, (U8)(tr->Limit >> 8),
                       (U8)tr->Base, (U8)(tr->Base >> 8),
                       (U8)(tr->Base >> 16), (U8)(tr->Base >> 24)};
        for (unsigned i = 0; i < 6; i++)
          if (!pm_write(m.segment, m.offset + i, 8, bytes[i])) return;
      } else if (m.reg == 2 || m.reg == 3) {
        DOS_TABLE_REGISTER *tr = m.reg == 2 ? &vm->Gdtr : &vm->Idtr;
        tr->Limit = (U16)pm_read(m.segment, m.offset, 16);
        tr->Base = pm_read(m.segment, m.offset + 2, 32);
      } else if (m.reg == 4) {
        pm_write_rm(m, 16, (U16)vm->Cr0);
      } else if (m.reg == 6) {
        U32 next_cr0 = vm->Cr0 | (pm_read_rm(m, 16) & 0x0fu);
        enter_protected_mode(next_cr0);
      } else fail("DOSVM: unsupported protected 0F 01 operation\r\n");
      return;
    }
    if (second == 0x00) {
      PM_MOD m = pm_decode(segment_override, address32);
      if (m.reg == 0) pm_write_rm(m, 16, vm->Ldtr);
      else if (m.reg == 1) pm_write_rm(m, 16, vm->Tr);
      else if (m.reg == 2) {
        if (seg[CS] & 3u) { fail("DOSVM: privileged LLDT fault\r\n"); return; }
        if (!load_ldt((U16)pm_read_rm(m, 16)))
          fail("DOSVM: invalid LDT selector\r\n");
      }
      else if (m.reg == 3) {
        if (seg[CS] & 3u) { fail("DOSVM: privileged LTR fault\r\n"); return; }
        U16 selector = (U16)pm_read_rm(m, 16);
        DOS_DESCRIPTOR d;
        if ((selector & 7u) || !read_descriptor(selector, &d) ||
            ((d.Access & 0x1fu) != 0x01u && (d.Access & 0x1fu) != 0x09u))
          fail("DOSVM: invalid task-register selector\r\n");
        else vm->Tr = selector;
      }
      else if (m.reg == 4 || m.reg == 5) {
        U16 selector = (U16)pm_read_rm(m, 16);
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
      U8 modrm = (U8)protected_fetch(1);
      unsigned cr = (modrm >> 3) & 7, reg = modrm & 7;
      if ((modrm >> 6) != 3 || cr != 0 || (seg[CS] & 3u)) {
        fail("DOSVM: invalid control-register operation\r\n"); return;
      }
      if (second == 0x20) pm_set_register(reg, 32, vm->Cr0);
      else enter_protected_mode(r32[reg]);
      return;
    }
    KeLog("[dos] unsupported protected 0F %02x at CS=%04x EIP=%08x\n",
          second, seg[CS], vm->Eip - 2u);
    fail("DOSVM: unsupported protected extended instruction\r\n");
    return;
  }
  if (op == 0xcd || op == 0xcc || op == 0xce) {
    U8 vector = op == 0xcc ? 3 : op == 0xce ? 4 : (U8)protected_fetch(1);
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
        op, seg[CS], vm->Eip - 1u, code_base, code_linear,
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
  U16 off, segment;
} MOD;
static MOD decode(int override) {
  MOD m;
  U8 x = fetch8();
  m.mod = x >> 6;
  m.reg = (x >> 3) & 7;
  m.rm = x & 7;
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

static int condition(unsigned cc) {
  int c = !!(flags & CF), z = !!(flags & ZF), s = !!(flags & SF),
      o = !!(flags & OF), p = !!(flags & 4);
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

void step(void) {
  DOS_VM *vm = current();
  if (dos_dispatch_hardware()) return;
  if ((vm->Steps & 0x3fffu) == 0) {
    UINT32 tick = (UINT32)KeGetTickCount();
    if (vm->VideoActive && (tick - vm->LastVideoTick) >= 2u) {
      vm->LastVideoTick = tick;
      DosVideoPresent(vm);
    }
  }
  if (vm->ProtectedMode) {
    step_protected();
    return;
  }
  int override = -1, rep = 0, operand32 = 0, address32 = 0;
  for (unsigned i = 0; i < 8; i++)
    r32[i] = (r32[i] & 0xffff0000u) | r[i];
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
    else if (op == 0xf2 || op == 0xf3)
      rep = op;
    else if (op == 0x66)
      operand32 = 1;
    else if (op == 0x67)
      address32 = 1;
    else
      break;
    op = fetch8();
  }
  if (address32) {
    fail("DOSVM: 32-bit address-size operations are not implemented\r\n");
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
    MOD m = decode(override);
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
    MOD m = decode(override);
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
    MOD m = decode(override);
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
    unsigned wide = op & 1, count = rep ? r[CX] : 1;
    if (count > 0xffff)
      count = 0xffff;
    while (count--) {
      U16 source = seg[override >= 0 ? override : DS], dest = seg[ES];
      U16 a = wide ? rw(source, r[SI]) : rb(source, r[SI]),
          b = wide ? rw(dest, r[DI]) : rb(dest, r[DI]);
      if (op <= 0xa5) {
        if (wide)
          ww(dest, r[DI], a);
        else
          wb(dest, r[DI], (U8)a);
      } else if (op <= 0xa7)
        alu(7, a, b, wide ? 16 : 8);
      else if (op <= 0xab) {
        if (wide)
          ww(dest, r[DI], r[AX]);
        else
          wb(dest, r[DI], (U8)r[AX]);
      } else if (op <= 0xad) {
        if (wide)
          r[AX] = a;
        else
          set8(0, (U8)a);
      } else
        alu(7, wide ? r[AX] : get8(0), b, wide ? 16 : 8);
      U16 delta = flags & DF ? (U16) - (wide ? 2 : 1) : (wide ? 2 : 1);
      if (op <= 0xa7 || (op <= 0xad && op >= 0xac))
        r[SI] += delta;
      if (op <= 0xab || op >= 0xae)
        r[DI] += delta;
      if (rep) {
        r[CX]--;
        if ((op == 0xa6 || op == 0xa7 || op == 0xae || op == 0xaf) &&
            ((rep == 0xf3) != !!(flags & ZF)))
          break;
      }
    }
    return;
  }
  if (op >= 0x6c && op <= 0x6f) {
    unsigned io_width = (op & 1u) ? (operand32 ? 32u : 16u) : 8u;
    unsigned bytes = io_width / 8u;
    U32 count = rep ? r[CX] : 1u;
    U16 port = r[DX];
    if (count > 0xffffu) count = 0xffffu;
    for (U32 i = 0; i < count && running; i++) {
      if (op == 0x6c || op == 0x6d) {
        U32 value = DosPortIn(current(), port, io_width);
        if (io_width == 8) wb(seg[ES], r[DI], (U8)value);
        else if (io_width == 16) ww(seg[ES], r[DI], (U16)value);
        else write32(seg[ES], r[DI], value);
      } else {
        U32 value = io_width == 8 ? rb(seg[override >= 0 ? override : DS], r[SI]) :
                    io_width == 16 ? rw(seg[override >= 0 ? override : DS], r[SI]) :
                    read32(seg[override >= 0 ? override : DS], r[SI]);
        DosPortOut(current(), port, value, io_width);
      }
      INT16 delta = (flags & DF) ? -(INT16)bytes : (INT16)bytes;
      if (op == 0x6c || op == 0x6d) r[DI] = (U16)(r[DI] + delta);
      else r[SI] = (U16)(r[SI] + delta);
    }
    if (rep) r[CX] = 0;
    return;
  }

  switch (op) {
  case 0x0f: {
    U8 extension = fetch8();
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
      (void)decode(override);
      return;
    }
    if (extension >= 0x90 && extension <= 0x9f) { /* SETcc r/m8 */
      MOD m = decode(override);
      write_rm(m, 0, (U16)condition(extension & 15));
      return;
    }
    if (extension == 0xb6 || extension == 0xb7 ||
        extension == 0xbe || extension == 0xbf) { /* MOVZX/MOVSX */
      MOD m = decode(override);
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
      MOD m = decode(override);
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
    if (extension == 0x31) { /* RDTSC: deterministic virtual instruction time */
      U64 ticks = current()->Steps;
      r32[AX] = (U32)ticks;
      r32[DX] = (U32)(ticks >> 32);
      r[AX] = (U16)r32[AX];
      r[DX] = (U16)r32[DX];
      return;
    }
    if (extension == 0xba) {
      MOD m = decode(override);
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
    if (extension == 0xa0 || extension == 0xa1 ||
        extension == 0xa8 || extension == 0xa9) {
      unsigned index = extension < 0xa8 ? FS : GS;
      if (extension == 0xa0 || extension == 0xa8) push(seg[index]);
      else seg[index] = pop();
      return;
    }
    if (extension == 0x01) {
      MOD m = decode(override);
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
    KeLog("[dos] unsupported 0F %02x at %04x:%04x AX=%04x\n",
          extension, instruction_cs, instruction_ip, r[AX]);
    fail("DOS: unsupported extended instruction\r\n");
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
    MOD m = decode(override);
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
    MOD m = decode(override);
    if (m.reg) {
      fail("DOS: invalid POP\r\n");
      return;
    }
    write_rm(m, 1, pop());
    return;
  }
  case 0x84:
  case 0x85: {
    MOD m = decode(override);
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
    MOD m = decode(override);
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
    MOD m = decode(override);
    if (m.reg > 5 || (op == 0x8e && m.reg == CS)) {
      fail("DOSVM: invalid segment register\r\n");
      return;
    }
    if (op == 0x8c)
      write_rm(m, 1, seg[m.reg]);
    else
      seg[m.reg] = read_rm(m, 1);
    return;
  }
  case 0x8d: {
    MOD m = decode(override);
    if (m.mod == 3) {
      fail("DOSVM: invalid LEA\r\n");
      return;
    }
    r[m.reg] = m.off;
    return;
  }
  case 0xc4:
  case 0xc5: {
    MOD m = decode(override);
    if (m.mod == 3) {
      fail("DOS: invalid far pointer\r\n");
      return;
    }
    r[m.reg] = rw(m.segment, m.off);
    seg[op == 0xc4 ? ES : DS] = rw(m.segment, (U16)(m.off + 2));
    return;
  }
  case 0x90:
    return;
  case 0xdb: {
    MOD m = decode(override);
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
    MOD m = decode(override);
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
    MOD m = decode(override);
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
  case 0x98:
    r[AX] = (U16)(signed char)r[AX];
    return;
  case 0x99:
    r[DX] = (r[AX] & 0x8000) ? 0xffff : 0;
    return;
  case 0x9a: {
    U16 o = fetch16(), s = fetch16();
    push(seg[CS]);
    push(ip);
    seg[CS] = s;
    ip = o;
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
    ip = pop();
    r[SP] += n;
    return;
  }
  case 0xc3:
    ip = pop();
    return;
  case 0xca: {
    U16 n = fetch16();
    ip = pop();
    seg[CS] = pop();
    r[SP] += n;
    return;
  }
  case 0xcb:
    ip = pop();
    seg[CS] = pop();
    return;
  case 0xc6:
  case 0xc7: {
    MOD m = decode(override);
    if (m.reg) {
      fail("DOSVM: invalid MOV\r\n");
      return;
    }
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
    dos_int(fetch8());
    return;
  case 0xcf:
    ip = pop();
    seg[CS] = pop();
    flags = (pop() & 0x7fffu) | 0x0002u;
    return;
  case 0xcc:
    dos_int(3);
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
    U16 d = fetch16();
    push(ip);
    ip += d;
    return;
  }
  case 0xe9:
    ip += fetch16();
    return;
  case 0xeb:
    ip += (signed char)fetch8();
    return;
  case 0xd0:
  case 0xd1:
  case 0xd2:
  case 0xd3: {
    MOD m = decode(override);
    int wide = op & 1;
    U8 count = op & 2 ? get8(1) : 1;
    write_rm(m, wide, shift_value(m.reg, read_rm(m, wide), count, wide));
    return;
  }
  case 0xd7: {
    U16 source = seg[override >= 0 ? override : DS];
    set8(0, rb(source, (U16)(r[BX] + get8(0))));
    return;
  }
  case 0xc0:
  case 0xc1: {
    MOD m = decode(override);
    int wide = op & 1;
    U8 count = fetch8();
    write_rm(m, wide, shift_value(m.reg, read_rm(m, wide), count, wide));
    return;
  }
  case 0xf6:
  case 0xf7: {
    MOD m = decode(override);
    int wide = op & 1;
    U16 v = read_rm(m, wide);
    U32 dividend, quotient;
    if (m.reg == 0) {
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
        fail("DOS: divide by zero\r\n");
        return;
      }
      dividend = wide ? ((U32)r[DX] << 16) | r[AX] : r[AX];
      if (m.reg == 6) {
        quotient = dividend / v;
        if (quotient > (wide ? 0xffffu : 0xffu)) {
          fail("DOS: divide overflow\r\n");
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
          fail("DOS: divide overflow\r\n");
          return;
        }
        INT32 q = signed_dividend / divisor;
        if (q < (wide ? -32768 : -128) || q > (wide ? 32767 : 127)) {
          fail("DOS: divide overflow\r\n");
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
    U16 o = fetch16(), s = fetch16();
    seg[CS] = s;
    ip = o;
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
    MOD m = decode(override);
    U16 v = read_rm(m, op == 0xff), cf = flags & CF;
    if (m.reg < 2) {
      v = alu(m.reg ? 5 : 0, v, 1, op == 0xff ? 16 : 8);
      flags = (flags & ~CF) | cf;
      write_rm(m, op == 0xff, v);
    } else if (op == 0xff && m.reg == 2) {
      push(ip);
      ip = v;
    } else if (op == 0xff && (m.reg == 3 || m.reg == 5) && m.mod != 3) {
      U16 target_segment = rw(m.segment, (U16)(m.off + 2));
      if (m.reg == 3) {
        push(seg[CS]);
        push(ip);
      }
      seg[CS] = target_segment;
      ip = v;
    } else if (op == 0xff && m.reg == 4)
      ip = v;
    else if (op == 0xff && m.reg == 6)
      push(v);
    else
      fail("DOSVM: unsupported FF group\r\n");
    return;
  }
  default:
    KeLog("[dos] unsupported opcode 0x%02x at %04x:%04x AX=0x%04x\n",
          op, instruction_cs, instruction_ip, r[AX]);
    fail("DOSVM: unsupported 8086 instruction\r\n");
    return;
  }
}

