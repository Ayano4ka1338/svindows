#include <svindows/Gfx.h>
#include "Internal.h"
#include <svindows/Input.h>

typedef struct _DOS_DATE_TIME {
  U16 Year;
  U8 Month, Day, Weekday, Hour, Minute, Second, Hundredths;
} DOS_DATE_TIME;

typedef struct __attribute__((packed)) _DOS_MOUSE_STATE {
  U16 X, Y, MinX, MaxX, MinY, MaxY, LastX, LastY, HostX, HostY;
  U16 MotionX, MotionY, PressX[3], PressY[3], ReleaseX[3], ReleaseY[3];
  U16 PressCount[3], ReleaseCount[3];
  U16 CallbackOffset, CallbackSegment, CallbackMask;
  struct __attribute__((packed)) { U16 Mask, Offset, Segment; } Alt[3];
  U16 ConditionalLeft, ConditionalTop, ConditionalRight, ConditionalBottom;
  U16 HotX, HotY, SensitivityX, SensitivityY, DoubleSpeed, RatioX, RatioY;
  U16 TextScreenMask, TextCursorMask;
  U16 AndMask[64][8], XorMask[64][8];
  U8 CursorWidthWords, CursorHeight, Page;
  U8 TextCursorType, TextCursorStart, TextCursorEnd, ConditionalOff;
  U8 HideCount;
  U8 Disabled;
} DOS_MOUSE_STATE;

static U16 dos_mouse_clip(U16 value, U16 minimum, U16 maximum);
static void dos_int_dispatch(U8 n);

static void dos_xms_free_stats(DOS_VM *vm, U32 *total_out, U32 *largest_out) {
  U32 total = 0, largest = 0, cursor = DOS_XMS_BASE;
  const U32 limit = DOS_XMS_BASE + DOS_XMS_CAPACITY;
  while (cursor < limit) {
    U32 next = limit, occupied_end = cursor;
    for (U32 i = 0; i < DOS_XMS_MAX_HANDLES; i++) {
      DOS_XMS_BLOCK *b = &vm->XmsBlocks[i];
      if (!b->Used || b->Base + b->Size <= cursor) continue;
      if (b->Base <= cursor) {
        if (b->Base + b->Size > occupied_end) occupied_end = b->Base + b->Size;
      } else if (b->Base < next) next = b->Base;
    }
    for (U32 i = 0; i < DOS_DPMI_MAX_BLOCKS; i++) {
      DOS_DPMI_BLOCK *b = &vm->DpmiBlocks[i];
      if (!b->Used || b->Base + b->Size <= cursor) continue;
      if (b->Base <= cursor) {
        if (b->Base + b->Size > occupied_end) occupied_end = b->Base + b->Size;
      } else if (b->Base < next) next = b->Base;
    }
    if (occupied_end > cursor) {
      cursor = occupied_end > limit ? limit : occupied_end;
      continue;
    }
    U32 gap = next - cursor;
    total += gap;
    if (gap > largest) largest = gap;
    if (next == limit) break;
    cursor = next;
  }
  *total_out = total;
  *largest_out = largest;
}

static U32 dos_xms_free_bytes(DOS_VM *vm) {
  U32 total, largest;
  dos_xms_free_stats(vm, &total, &largest);
  return total;
}

static U32 dos_xms_largest_free(DOS_VM *vm) {
  U32 total, largest;
  dos_xms_free_stats(vm, &total, &largest);
  return largest;
}

static U8 dos_xms_allocate(DOS_VM *vm, U32 bytes, U16 *handle) {
  U32 slot, base = DOS_XMS_BASE;
  const U32 limit = DOS_XMS_BASE + DOS_XMS_CAPACITY;
  for (slot = 0; slot < DOS_XMS_MAX_HANDLES && vm->XmsBlocks[slot].Used; slot++);
  if (slot == DOS_XMS_MAX_HANDLES) return 0xa1;
  if (bytes > dos_xms_free_bytes(vm)) return 0xa0;
  for (;;) {
    U32 next = limit, end = base;
    for (U32 i = 0; i < DOS_XMS_MAX_HANDLES; i++) {
      DOS_XMS_BLOCK *b = &vm->XmsBlocks[i];
      if (b->Used && b->Size && b->Base >= base && b->Base < next) {
        next = b->Base;
        end = b->Base + b->Size;
      }
    }
    for (U32 i = 0; i < DOS_DPMI_MAX_BLOCKS; i++) {
      DOS_DPMI_BLOCK *b = &vm->DpmiBlocks[i];
      if (b->Used && base < b->Base + b->Size && b->Base < base + bytes) {
        end = b->Base + b->Size;
        next = base;
        break;
      }
    }
    if (bytes <= next - base) break;
    if (next == limit || end <= base) return 0xa0;
    base = end;
  }
  vm->XmsBlocks[slot] = (DOS_XMS_BLOCK){base, bytes, TRUE, FALSE};
  vm->XmsUsed += bytes;
  *handle = (U16)(slot + 1u);
  return 0;
}

static BOOLEAN dos_xms_range_free(DOS_VM *vm, U32 base, U32 size,
                                  U32 ignored_slot) {
  const U32 limit = DOS_XMS_BASE + DOS_XMS_CAPACITY;
  if (base < DOS_XMS_BASE || base > limit || size > limit - base) return FALSE;
  for (U32 i = 0; i < DOS_XMS_MAX_HANDLES; i++) {
    DOS_XMS_BLOCK *b = &vm->XmsBlocks[i];
    if (i != ignored_slot && b->Used && base < b->Base + b->Size &&
        b->Base < base + size) return FALSE;
  }
  for (U32 i = 0; i < DOS_DPMI_MAX_BLOCKS; i++) {
    DOS_DPMI_BLOCK *b = &vm->DpmiBlocks[i];
    if (b->Used && base < b->Base + b->Size && b->Base < base + size)
      return FALSE;
  }
  return TRUE;
}

static U8 dos_xms_resize(DOS_VM *vm, U16 handle, U32 size) {
  if (!handle || handle > DOS_XMS_MAX_HANDLES ||
      !vm->XmsBlocks[handle - 1u].Used) return 0xa2;
  U32 slot = handle - 1u;
  DOS_XMS_BLOCK *block = &vm->XmsBlocks[slot];
  if (block->Locked) return 0xabu;
  U32 old_size = block->Size, old_base = block->Base;
  U32 base = old_base;
  if (!dos_xms_range_free(vm, base, size, slot)) {
    base = DOS_XMS_BASE;
    for (;;) {
      U32 next = DOS_XMS_BASE + DOS_XMS_CAPACITY;
      U32 after = base;
      for (U32 i = 0; i < DOS_XMS_MAX_HANDLES; i++) {
        DOS_XMS_BLOCK *b = &vm->XmsBlocks[i];
        if (i != slot && b->Used && b->Size && b->Base >= base && b->Base < next) {
          next = b->Base;
          after = b->Base + b->Size;
        }
      }
      for (U32 i = 0; i < DOS_DPMI_MAX_BLOCKS; i++) {
        DOS_DPMI_BLOCK *b = &vm->DpmiBlocks[i];
        if (b->Used && base < b->Base + b->Size && b->Base < base + size) {
          next = base;
          after = b->Base + b->Size;
          break;
        }
      }
      if (size <= next - base) break;
      if (next == DOS_XMS_BASE + DOS_XMS_CAPACITY || after <= base)
        return 0xa0;
      base = after;
    }
    if (!dos_xms_range_free(vm, base, size, slot)) return 0xa0;
  }
  if (base != old_base) memmove(vm->Memory + base, vm->Memory + old_base, old_size);
  block->Base = base;
  block->Size = size;
  vm->XmsUsed = vm->XmsUsed - old_size + size;
  return 0;
}

static void dos_xms_result(U8 error) {
  r[AX] = error ? 0 : 1;
  set8(3, error); /* BL is the XMS error code. */
}

static void dos_xms_call(DOS_VM *vm) {
  U8 function = (U8)(r[AX] >> 8);
  switch (function) {
  case 0x00: /* Get XMS version. */
    r[AX] = 0x0300;
    r[BX] = 0x0301;
    r[DX] = 1; /* HMA supported. */
    return;
  case 0x01: /* Request HMA. */
    if (vm->XmsHmaOwned) {
      dos_xms_result(0x91); return;
    }
    vm->XmsHmaOwned = TRUE;
    dos_xms_result(0); return;
  case 0x02: /* Release HMA. */
    if (!vm->XmsHmaOwned) { dos_xms_result(0x93); return; }
    vm->XmsHmaOwned = FALSE;
    dos_xms_result(0); return;
  case 0x03: case 0x05: /* Global/local A20 enable. */
    if (function == 0x03 && vm->XmsA20Global != 0xffffu)
      vm->XmsA20Global++;
    if (function == 0x05 && vm->XmsA20Local != 0xffffu)
      vm->XmsA20Local++;
    dos_xms_result(0); return;
  case 0x04: case 0x06: /* Global/local A20 disable. */
    if (function == 0x04 && vm->XmsA20Global) vm->XmsA20Global--;
    if (function == 0x06 && vm->XmsA20Local) vm->XmsA20Local--;
    dos_xms_result(0); return;
  case 0x07: /* Query A20 state. */
    r[AX] = (vm->XmsA20Global || vm->XmsA20Local) ? 1 : 0;
    set8(3, 0);
    return;
  case 0x08: { /* Query free extended memory in KiB. */
    U32 largest = dos_xms_largest_free(vm) / 1024u;
    U32 total = dos_xms_free_bytes(vm) / 1024u;
    r[AX] = (U16)(largest > 0xffffu ? 0xffffu : largest);
    r[DX] = (U16)(total > 0xffffu ? 0xffffu : total);
    set8(3, 0);
    return;
  }
  case 0x09: case 0x89: { /* Allocate EMB in KiB or byte units. */
    U32 bytes = function == 0x09 ? (U32)r[DX] * 1024u : r32[DX];
    U16 handle = 0;
    U8 error = dos_xms_allocate(vm, bytes, &handle);
    if (error) { dos_xms_result(error); return; }
    r[DX] = handle;
    r32[DX] = handle;
    dos_xms_result(0); return;
  }
  case 0x0a: { /* Free extended-memory block. */
    U16 handle = r[DX];
    if (!handle || handle > DOS_XMS_MAX_HANDLES || !vm->XmsBlocks[handle - 1u].Used) {
      dos_xms_result(0xa2); return;
    }
    DOS_XMS_BLOCK *b = &vm->XmsBlocks[handle - 1u];
    if (b->Locked) { dos_xms_result(0xab); return; }
    vm->XmsUsed -= b->Size;
    memset(b, 0, sizeof(*b));
    dos_xms_result(0); return;
  }
  case 0x0b: { /* Move between conventional and extended memory. */
    U16 ds = seg[DS], si = r[SI];
    U32 length = (U32)rw(ds, si) | ((U32)rw(ds, (U16)(si + 2u)) << 16);
    U16 sh = rw(ds, (U16)(si + 4u));
    U32 so = (U32)rw(ds, (U16)(si + 6u)) | ((U32)rw(ds, (U16)(si + 8u)) << 16);
    U16 dh = rw(ds, (U16)(si + 10u));
    U32 dest = (U32)rw(ds, (U16)(si + 12u)) | ((U32)rw(ds, (U16)(si + 14u)) << 16);
    U32 sb = 0, db = 0;
    if (sh) {
      if (sh > DOS_XMS_MAX_HANDLES || !vm->XmsBlocks[sh - 1u].Used ||
          so > vm->XmsBlocks[sh - 1u].Size || length > vm->XmsBlocks[sh - 1u].Size - so) {
        dos_xms_result(0xa7); return;
      }
      sb = vm->XmsBlocks[sh - 1u].Base + so;
    } else sb = (so >> 16) * 16u + (so & 0xffffu);
    if (dh) {
      if (dh > DOS_XMS_MAX_HANDLES || !vm->XmsBlocks[dh - 1u].Used ||
          dest > vm->XmsBlocks[dh - 1u].Size || length > vm->XmsBlocks[dh - 1u].Size - dest) {
        dos_xms_result(0xa7); return;
      }
      db = vm->XmsBlocks[dh - 1u].Base + dest;
    } else db = (dest >> 16) * 16u + (dest & 0xffffu);
    if ((!sh && (sb > DOS_REAL_MEMORY_SIZE || length > DOS_REAL_MEMORY_SIZE - sb)) ||
        (!dh && (db > DOS_REAL_MEMORY_SIZE || length > DOS_REAL_MEMORY_SIZE - db))) {
      dos_xms_result(0xa7); return;
    }
    U8 *src = vm->Memory + sb;
    U8 *dst = vm->Memory + db;
    memmove(dst, src, length);
    dos_xms_result(0); return;
  }
  case 0x0c: case 0x0d: { /* Lock/unlock a block. */
    U16 handle = r[DX];
    if (!handle || handle > DOS_XMS_MAX_HANDLES || !vm->XmsBlocks[handle - 1u].Used) {
      dos_xms_result(0xa2); return;
    }
    DOS_XMS_BLOCK *b = &vm->XmsBlocks[handle - 1u];
    b->Locked = function == 0x0cu;
    if (function == 0x0cu) { r[BX] = (U16)b->Base; r[DX] = (U16)(b->Base >> 16); }
    dos_xms_result(0); return;
  }
  case 0x0e: case 0x8e: { /* Get EMB handle information. */
    U16 handle = r[DX];
    if (!handle || handle > DOS_XMS_MAX_HANDLES ||
        !vm->XmsBlocks[handle - 1u].Used) {
      dos_xms_result(0xa2); return;
    }
    U8 free_handles = 0;
    for (U32 i = 0; i < DOS_XMS_MAX_HANDLES; i++)
      if (!vm->XmsBlocks[i].Used) free_handles++;
    set8(7, vm->XmsBlocks[handle - 1u].Locked ? 1u : 0u); /* BH */
    U32 bytes = vm->XmsBlocks[handle - 1u].Size;
    if (function == 0x0e) {
      set8(3, free_handles); /* BL */
      r[DX] = (U16)(bytes / 1024u);
      r32[DX] = (r32[DX] & 0xffff0000u) | r[DX];
    } else {
      r[CX] = free_handles;
      r32[DX] = bytes;
      r[DX] = (U16)bytes;
    }
    r[AX] = 1;
    return;
  }
  case 0x0f: case 0x8f: { /* Resize EMB in KiB or byte units. */
    U32 bytes = function == 0x0f ? (U32)r[BX] * 1024u : r32[BX];
    U8 error = dos_xms_resize(vm, r[DX], bytes);
    dos_xms_result(error);
    return;
  }
  case 0x88: { /* Query any free extended memory, XMS 3.0 byte units. */
    r32[AX] = dos_xms_largest_free(vm);
    r32[DX] = dos_xms_free_bytes(vm);
    r32[CX] = MEM_SIZE - 1u;
    r[AX] = (U16)r32[AX]; r[BX] = (U16)r32[BX];
    r[CX] = (U16)r32[CX]; r[DX] = (U16)r32[DX];
    set8(3, 0);
    return;
  }
  default:
    dos_xms_result(0x80); return;
  }
}

static void dos_mouse_state_transfer(DOS_VM *vm, U16 segment, U16 offset,
                                     BOOLEAN restore) {
  DOS_MOUSE_STATE state;
  U8 *bytes = (U8 *)&state;
  if (restore) {
    for (U32 i = 0; i < sizeof(state); i++)
      bytes[i] = rb(segment, (U16)(offset + i));
    vm->MouseMinX = state.MinX; vm->MouseMaxX = state.MaxX;
    vm->MouseMinY = state.MinY; vm->MouseMaxY = state.MaxY;
    if (vm->MouseMaxX < vm->MouseMinX) vm->MouseMaxX = vm->MouseMinX;
    if (vm->MouseMaxY < vm->MouseMinY) vm->MouseMaxY = vm->MouseMinY;
    vm->MouseX = dos_mouse_clip(state.X, vm->MouseMinX, vm->MouseMaxX);
    vm->MouseY = dos_mouse_clip(state.Y, vm->MouseMinY, vm->MouseMaxY);
    vm->MouseLastX = state.LastX; vm->MouseLastY = state.LastY;
    vm->MouseHostX = state.HostX; vm->MouseHostY = state.HostY;
    vm->MouseMotionX = state.MotionX; vm->MouseMotionY = state.MotionY;
    memcpy(vm->MousePressX, state.PressX, sizeof(state.PressX));
    memcpy(vm->MousePressY, state.PressY, sizeof(state.PressY));
    memcpy(vm->MouseReleaseX, state.ReleaseX, sizeof(state.ReleaseX));
    memcpy(vm->MouseReleaseY, state.ReleaseY, sizeof(state.ReleaseY));
    memcpy(vm->MousePressCount, state.PressCount, sizeof(state.PressCount));
    memcpy(vm->MouseReleaseCount, state.ReleaseCount, sizeof(state.ReleaseCount));
    vm->MouseCallbackOffset = state.CallbackOffset;
    vm->MouseCallbackSegment = state.CallbackSegment;
    vm->MouseCallbackMask = state.CallbackMask;
    memcpy(vm->MouseAltCallbacks, state.Alt, sizeof(state.Alt));
    vm->MouseConditionalLeft = state.ConditionalLeft;
    vm->MouseConditionalTop = state.ConditionalTop;
    vm->MouseConditionalRight = state.ConditionalRight;
    vm->MouseConditionalBottom = state.ConditionalBottom;
    vm->MouseHotX = (INT16)state.HotX; vm->MouseHotY = (INT16)state.HotY;
    vm->MouseSensitivityX = state.SensitivityX;
    vm->MouseSensitivityY = state.SensitivityY;
    vm->MouseDoubleSpeedThreshold = state.DoubleSpeed;
    vm->MouseRatioX = state.RatioX; vm->MouseRatioY = state.RatioY;
    vm->MouseTextScreenMask = state.TextScreenMask;
    vm->MouseTextCursorMask = state.TextCursorMask;
    memcpy(vm->MouseAndMask, state.AndMask, sizeof(state.AndMask));
    memcpy(vm->MouseXorMask, state.XorMask, sizeof(state.XorMask));
    vm->MouseCursorWidthWords = state.CursorWidthWords;
    vm->MouseCursorHeight = state.CursorHeight;
    vm->MousePage = state.Page; vm->MouseHideCount = state.HideCount;
    vm->MouseTextCursorType = state.TextCursorType;
    vm->MouseTextCursorStart = state.TextCursorStart;
    vm->MouseTextCursorEnd = state.TextCursorEnd;
    vm->MouseConditionalOff = state.ConditionalOff;
    vm->MouseDisabled = state.Disabled;
    vm->VideoDirty = TRUE;
    return;
  }
  memset(&state, 0, sizeof(state));
  state.X = vm->MouseX; state.Y = vm->MouseY;
  state.MinX = vm->MouseMinX; state.MaxX = vm->MouseMaxX;
  state.MinY = vm->MouseMinY; state.MaxY = vm->MouseMaxY;
  state.LastX = vm->MouseLastX; state.LastY = vm->MouseLastY;
  state.HostX = vm->MouseHostX; state.HostY = vm->MouseHostY;
  state.MotionX = vm->MouseMotionX; state.MotionY = vm->MouseMotionY;
  memcpy(state.PressX, vm->MousePressX, sizeof(state.PressX));
  memcpy(state.PressY, vm->MousePressY, sizeof(state.PressY));
  memcpy(state.ReleaseX, vm->MouseReleaseX, sizeof(state.ReleaseX));
  memcpy(state.ReleaseY, vm->MouseReleaseY, sizeof(state.ReleaseY));
  memcpy(state.PressCount, vm->MousePressCount, sizeof(state.PressCount));
  memcpy(state.ReleaseCount, vm->MouseReleaseCount, sizeof(state.ReleaseCount));
  state.CallbackOffset = vm->MouseCallbackOffset;
  state.CallbackSegment = vm->MouseCallbackSegment;
  state.CallbackMask = vm->MouseCallbackMask;
  memcpy(state.Alt, vm->MouseAltCallbacks, sizeof(state.Alt));
  state.ConditionalLeft = vm->MouseConditionalLeft;
  state.ConditionalTop = vm->MouseConditionalTop;
  state.ConditionalRight = vm->MouseConditionalRight;
  state.ConditionalBottom = vm->MouseConditionalBottom;
  state.HotX = (U16)vm->MouseHotX; state.HotY = (U16)vm->MouseHotY;
  state.SensitivityX = vm->MouseSensitivityX;
  state.SensitivityY = vm->MouseSensitivityY;
  state.DoubleSpeed = vm->MouseDoubleSpeedThreshold;
  state.RatioX = vm->MouseRatioX; state.RatioY = vm->MouseRatioY;
  state.TextScreenMask = vm->MouseTextScreenMask;
  state.TextCursorMask = vm->MouseTextCursorMask;
  memcpy(state.AndMask, vm->MouseAndMask, sizeof(state.AndMask));
  memcpy(state.XorMask, vm->MouseXorMask, sizeof(state.XorMask));
  state.CursorWidthWords = vm->MouseCursorWidthWords;
  state.CursorHeight = vm->MouseCursorHeight;
  state.Page = vm->MousePage; state.HideCount = vm->MouseHideCount;
  state.TextCursorType = vm->MouseTextCursorType;
  state.TextCursorStart = vm->MouseTextCursorStart;
  state.TextCursorEnd = vm->MouseTextCursorEnd;
  state.ConditionalOff = vm->MouseConditionalOff;
  state.Disabled = vm->MouseDisabled;
  for (U32 i = 0; i < sizeof(state); i++) wb(segment, (U16)(offset + i), bytes[i]);
}

static U8 dos_cga_palette_to_ega(U8 color) {
  U8 rgb = color & 7u;
  if (color & 0x18u) {
    if (rgb & 1u) color |= 0x08u;
    if (rgb & 2u) color |= 0x10u;
    if (rgb & 4u) color |= 0x20u;
  }
  return (U8)((rgb | (color & 0x38u)) & 0x3fu);
}

/* DOS INT 21h/AH=29h: parse a filespec into an unopened FCB.
 * The guest pointer is deliberately advanced only past the parsed filespec;
 * separators after it remain for the caller, as on MS-DOS. */
static BOOLEAN dos_fcb_terminator(U8 ch) {
  return ch < 0x20u || ch == 0x7fu || ch == ' ' || ch == '\t' ||
         ch == ';' || ch == ',' || ch == '=' || ch == '+' || ch == '<' ||
         ch == '>' || ch == ':' || ch == '/' || ch == '\\' ||
         ch == '"' || ch == '[' || ch == ']' || ch == '|';
}

static BOOLEAN dos_fcb_leading_separator(U8 ch) {
  return ch == ' ' || ch == '\t' || ch == '.' || ch == ';' ||
         ch == ',' || ch == '=' || ch == '+';
}

static U8 dos_fcb_upper(U8 ch) {
  return ch >= 'a' && ch <= 'z' ? (U8)(ch - ('a' - 'A')) : ch;
}

static U8 dos_parse_fcb(void) {
  U8 options = get8(0);
  U16 source = r[SI];
  U16 fcb = r[DI];
  U8 name[8], extension[3];
  U8 name_length = 0, extension_length = 0;
  BOOLEAN have_name = FALSE, have_extension = FALSE, wildcard = FALSE;
  BOOLEAN extended = rb(seg[ES], fcb) == 0xffu;
  if (extended)
    fcb = (U16)(fcb + 7u);

  if (options & 0x01u) {
    while (dos_fcb_leading_separator(rb(seg[DS], source)))
      source++;
  }

  /* A drive specifier is meaningful only at the beginning of the filespec. */
  U8 first = dos_fcb_upper(rb(seg[DS], source));
  U8 second = rb(seg[DS], (U16)(source + 1u));
  BOOLEAN has_drive = second == ':';
  if (has_drive) {
    /* This VM advertises A:, B: and C: as the DOS drive range. */
    if (first < 'A' || first > 'C') {
      r[SI] = source;
      return 0xffu;
    }
    wb(seg[ES], fcb, (U8)(first - 'A' + 1u));
    source = (U16)(source + 2u);
  } else if (!(options & 0x02u)) {
    wb(seg[ES], fcb, 0);
  }

  memset(name, ' ', sizeof(name));
  memset(extension, ' ', sizeof(extension));

  U8 ch = rb(seg[DS], source);
  while (ch && ch != '.' && !dos_fcb_terminator(ch)) {
    have_name = TRUE;
    if (ch == '*') {
      wildcard = TRUE;
      while (name_length < sizeof(name))
        name[name_length++] = '?';
      source++;
      /* '*' consumes the rest of this field, but not its extension. */
      while ((ch = rb(seg[DS], source)) && ch != '.' &&
             !dos_fcb_terminator(ch))
        source++;
      break;
    }
    if (name_length < sizeof(name)) {
      if (ch == '?') wildcard = TRUE;
      name[name_length++] = dos_fcb_upper(ch);
    }
    source++;
    ch = rb(seg[DS], source);
  }

  if (ch == '.') {
    have_extension = TRUE;
    source++;
    ch = rb(seg[DS], source);
    while (ch && !dos_fcb_terminator(ch) && ch != '.') {
      if (ch == '*') {
        wildcard = TRUE;
        while (extension_length < sizeof(extension))
          extension[extension_length++] = '?';
        source++;
        while ((ch = rb(seg[DS], source)) && ch != '.' &&
               !dos_fcb_terminator(ch))
          source++;
        break;
      }
      if (extension_length < sizeof(extension)) {
        if (ch == '?') wildcard = TRUE;
        extension[extension_length++] = dos_fcb_upper(ch);
      }
      source++;
      ch = rb(seg[DS], source);
    }
  }

  /* AL bits 2/3 preserve existing name/extension when absent. */
  if (have_name || !(options & 0x04u))
    for (U8 i = 0; i < sizeof(name); i++)
      wb(seg[ES], (U16)(fcb + 1u + i), name[i]);
  if (have_extension || !(options & 0x08u))
    for (U8 i = 0; i < sizeof(extension); i++)
      wb(seg[ES], (U16)(fcb + 9u + i), extension[i]);

  /* The PSP's default FCB slots at 005Ch and 006Ch are only 16 bytes
   * long and sit immediately before the command tail at 0080h. Clear only
   * the current-block and record-size fields; touching the rest of a full
   * 37-byte FCB here would corrupt the PSP command tail. */
  for (U8 i = 12; i < 16; i++)
    wb(seg[ES], (U16)(fcb + i), 0);

  r[SI] = source;
  return wildcard ? 1u : 0u;
}

static void dos_bios_keyboard(void) {
  U8 scan = current()->KeyboardData;
  current()->KeyboardDataReady = FALSE;
  U8 key = scan & 0x7fu, state = rb(0x40u, 0x17u), modifier = 0;
  if (key == 0x36u) modifier = 1u;
  if (key == 0x2au) modifier = 2u;
  if (key == 0x1du) modifier = 4u;
  if (key == 0x38u) modifier = 8u;
  if (modifier) {
    state = scan & 0x80u ? state & (U8)~modifier : state | modifier;
    wb(0x40u, 0x17u, state);
    return;
  }
  if (scan & 0x80u) return;
  if (key == 0x3au || key == 0x45u || key == 0x46u) {
    state ^= key == 0x3au ? 0x40u : key == 0x45u ? 0x20u : 0x10u;
    wb(0x40u, 0x17u, state);
    return;
  }
  static const U8 plain[] =
      "\0\0331234567890-=\b\tqwertyuiop[]\r\0asdfghjkl;'`\0\\zxcvbnm,./\0*\0 \0";
  static const U8 shifted[] =
      "\0\033!@#$%^&*()_+\b\tQWERTYUIOP{}\r\0ASDFGHJKL:\"~\0|ZXCVBNM<>?\0*\0 \0";
  U8 ch = key < sizeof(plain) - 1u ? ((state & 3u) ? shifted[key] : plain[key]) : 0;
  if (state & 0x40u) {
    if (ch >= 'a' && ch <= 'z') ch -= 'a' - 'A';
    else if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
  }
  if ((state & 4u) && ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z')))
    ch &= 0x1fu;
  if (state & 8u) ch = 0;
  U16 head = rw(0x40u, 0x1au), tail = rw(0x40u, 0x1cu);
  if (head < 0x1eu || head >= 0x3eu || (head & 1u)) head = 0x1eu;
  if (tail < 0x1eu || tail >= 0x3eu || (tail & 1u)) tail = 0x1eu;
  U16 next = tail + 2u < 0x3eu ? tail + 2u : 0x1eu;
  if (next == head) return;
  ww(0x40u, tail, ((U16)key << 8) | ch);
  ww(0x40u, 0x1au, head);
  ww(0x40u, 0x1cu, next);
}

static void dos_mouse_dimensions(U16 *width, U16 *height) {
  DOS_VM *vm = current();
  *width = 640;
  *height = 200;
  switch (vm->VideoMode) {
  case 0x0d: *width = 320; break;
  case 0x10: *height = 350; break;
  case 0x11: case 0x12: *height = 480; break;
  default: break;
  }
}

static U16 dos_mouse_clip(U16 value, U16 minimum, U16 maximum) {
  return value < minimum ? minimum : value > maximum ? maximum : value;
}

/* Poll the host PS/2 pointer from both guest code and the virtual hardware
 * clock. This keeps callbacks and button counters live even when a program
 * spends long periods outside INT 33h. */
static void dos_mouse_poll(DOS_VM *vm) {
  U16 width, height;
  dos_mouse_dimensions(&width, &height);
  UINT32 host_width = GfxFramebuffer.Width ? GfxFramebuffer.Width : 1024u;
  UINT32 host_height = GfxFramebuffer.Height ? GfxFramebuffer.Height : 768u;
  INT32 hx = MouseState.X, hy = MouseState.Y;
  if (hx < 0) hx = 0;
  if (hy < 0) hy = 0;
  if (hx >= (INT32)host_width) hx = (INT32)host_width - 1;
  if (hy >= (INT32)host_height) hy = (INT32)host_height - 1;
  U16 x = (U16)((UINT32)hx * (width - 1u) / (host_width - 1u ? host_width - 1u : 1u));
  U16 y = (U16)((UINT32)hy * (height - 1u) / (host_height - 1u ? host_height - 1u : 1u));
  if (vm->VideoMode <= 3u || vm->VideoMode == 7u) { x &= 0xfff8u; y &= 0xfff8u; }
  U8 buttons = MouseState.Buttons & 7u;

  if (!vm->MouseInitialized) {
    vm->MouseInitialized = TRUE;
    vm->MouseMinX = vm->MouseMinY = 0;
    vm->MouseMaxX = width - 1u;
    vm->MouseMaxY = height - 1u;
    vm->MouseX = vm->MouseLastX = x;
    vm->MouseY = vm->MouseLastY = y;
    vm->MouseHostX = x; vm->MouseHostY = y;
    vm->MouseButtons = vm->MouseLastButtons = buttons;
    vm->MouseHostSeq = MouseState.Seq;
    vm->MouseVideoWidth = width; vm->MouseVideoHeight = height;
    vm->MouseSensitivityX = vm->MouseSensitivityY = 50;
    vm->MouseDoubleSpeedThreshold = 64;
    vm->MouseRatioX = 8; vm->MouseRatioY = 16;
    vm->MouseTextScreenMask = 0xffff; vm->MouseTextCursorMask = 0x7700;
    return;
  }
  if (vm->MouseVideoWidth != width || vm->MouseVideoHeight != height) {
    U16 old_width = vm->MouseVideoWidth, old_height = vm->MouseVideoHeight;
    if (vm->MouseMinX == 0 && vm->MouseMaxX == old_width - 1u)
      vm->MouseMaxX = width - 1u;
    if (vm->MouseMinY == 0 && vm->MouseMaxY == old_height - 1u)
      vm->MouseMaxY = height - 1u;
    vm->MouseVideoWidth = width; vm->MouseVideoHeight = height;
    vm->MouseHostX = x; vm->MouseHostY = y;
    if (old_width > 1) vm->MouseX = (U16)((UINT32)vm->MouseX * (width - 1u) / (old_width - 1u));
    if (old_height > 1) vm->MouseY = (U16)((UINT32)vm->MouseY * (height - 1u) / (old_height - 1u));
    vm->MouseX = dos_mouse_clip(vm->MouseX, vm->MouseMinX, vm->MouseMaxX);
    vm->MouseY = dos_mouse_clip(vm->MouseY, vm->MouseMinY, vm->MouseMaxY);
    vm->VideoDirty = TRUE;
  }
  U32 seq = MouseState.Seq;
  if (seq == vm->MouseHostSeq) return;
  vm->MouseHostSeq = seq;
  INT32 delta_x = (INT32)x - vm->MouseHostX;
  INT32 delta_y = (INT32)y - vm->MouseHostY;
  vm->MouseHostX = x; vm->MouseHostY = y;
  INT32 mickey_x = delta_x * vm->MouseRatioX / 8;
  INT32 mickey_y = delta_y * vm->MouseRatioY / 8;
  U32 ratio_x = vm->MouseRatioX ? vm->MouseRatioX : 8u;
  U32 ratio_y = vm->MouseRatioY ? vm->MouseRatioY : 16u;
  INT32 guest_dx = delta_x * 8 * vm->MouseSensitivityX / (INT32)(ratio_x * 50u);
  INT32 guest_dy = delta_y * 16 * vm->MouseSensitivityY / (INT32)(ratio_y * 50u);
  INT32 speed = (mickey_x < 0 ? -mickey_x : mickey_x) +
                (mickey_y < 0 ? -mickey_y : mickey_y);
  if (vm->MouseDoubleSpeedThreshold && speed * 100 > vm->MouseDoubleSpeedThreshold) {
    guest_dx *= 2; guest_dy *= 2;
  }
  vm->MouseLastX = vm->MouseX;
  vm->MouseLastY = vm->MouseY;
  vm->MouseLastButtons = vm->MouseButtons;
  INT32 guest_x = (INT32)vm->MouseX + guest_dx;
  INT32 guest_y = (INT32)vm->MouseY + guest_dy;
  vm->MouseX = (U16)(guest_x < vm->MouseMinX ? vm->MouseMinX :
                     guest_x > vm->MouseMaxX ? vm->MouseMaxX : guest_x);
  vm->MouseY = (U16)(guest_y < vm->MouseMinY ? vm->MouseMinY :
                     guest_y > vm->MouseMaxY ? vm->MouseMaxY : guest_y);
  vm->MouseButtons = buttons;
  if (vm->MouseX != vm->MouseLastX || vm->MouseY != vm->MouseLastY ||
      vm->MouseButtons != vm->MouseLastButtons)
    vm->VideoDirty = TRUE;
  vm->MouseMotionX = (U16)(vm->MouseMotionX + (U16)(INT16)mickey_x);
  vm->MouseMotionY = (U16)(vm->MouseMotionY + (U16)(INT16)mickey_y);
  U8 events = (vm->MouseX != vm->MouseLastX ||
               vm->MouseY != vm->MouseLastY) ? 1u : 0u;
  for (U8 i = 0; i < 3; i++) {
    U8 bit = (U8)(1u << i);
    if (!(vm->MouseLastButtons & bit) && (buttons & bit)) {
      if (vm->MousePressCount[i] != 0xffffu) vm->MousePressCount[i]++;
      vm->MousePressX[i] = vm->MouseX; vm->MousePressY[i] = vm->MouseY;
      events |= (U8)(2u << (i * 2u));
    }
    if ((vm->MouseLastButtons & bit) && !(buttons & bit)) {
      if (vm->MouseReleaseCount[i] != 0xffffu) vm->MouseReleaseCount[i]++;
      vm->MouseReleaseX[i] = vm->MouseX; vm->MouseReleaseY[i] = vm->MouseY;
      events |= (U8)(4u << (i * 2u));
    }
  }
  if (events) {
    vm->VideoDirty = TRUE;
    vm->MousePendingEvents |= events;
    U8 key_flags = rb(0x40u, 0x17u);
    U8 modifiers = (U8)(((key_flags & 3u) ? 0x20u : 0u) |
                        ((key_flags & 4u) ? 0x40u : 0u) |
                        ((key_flags & 8u) ? 0x80u : 0u));
    for (U8 i = 0; i < 3; i++) {
      U16 alt_mask = vm->MouseAltCallbacks[i].Mask;
      U8 required = (U8)(alt_mask & 0xe0u);
      if (alt_mask && required && (modifiers & required) == required)
        vm->MouseAltPendingEvents[i] |= events & (U8)alt_mask & 0x1fu;
    }
    if (vm->MouseConditionalOff && !vm->MouseHideCount &&
        x >= vm->MouseConditionalLeft && x <= vm->MouseConditionalRight &&
        y >= vm->MouseConditionalTop && y <= vm->MouseConditionalBottom)
      vm->MouseHideCount = 1;
  }
}

void DosMousePoll(DOS_VM *vm) { dos_mouse_poll(vm); }

/* INT 33h/AX=0000h reset: initialize the emulated DOS mouse interface and
 * report a three-button device. Also used when a protected-mode client invokes
 * INT 33h without installing its own protected-mode vector. */
void DosMouseInitialize(DOS_VM *vm) {
  if (!vm) return;
  U16 width, height;
  dos_mouse_dimensions(&width, &height);
  dos_mouse_poll(vm);
  vm->MouseInitialized = TRUE;
  vm->MouseMinX = vm->MouseMinY = 0;
  vm->MouseMaxX = width - 1u; vm->MouseMaxY = height - 1u;
  /* A reset leaves the driver installed with its cursor hidden. The guest
   * owns cursor rendering and may reveal it with function 01h. */
  vm->MouseHideCount = 1;
  vm->MouseDisabled = FALSE;
  DosMouseSetDefaultCursor(vm);
  vm->MouseMotionX = vm->MouseMotionY = 0;
  vm->MousePendingEvents = 0;
  vm->MouseCallbackMask = vm->MouseCallbackOffset = vm->MouseCallbackSegment = 0;
  memset(vm->MouseAltCallbacks, 0, sizeof(vm->MouseAltCallbacks));
  memset(vm->MouseAltPendingEvents, 0, sizeof(vm->MouseAltPendingEvents));
  memset(vm->MousePressCount, 0, sizeof(vm->MousePressCount));
  memset(vm->MouseReleaseCount, 0, sizeof(vm->MouseReleaseCount));
  vm->MouseRatioX = 8; vm->MouseRatioY = 16;
  vm->MouseSensitivityX = vm->MouseSensitivityY = 50;
  vm->MouseDoubleSpeedThreshold = 64;
  vm->MouseLastX = vm->MouseX; vm->MouseLastY = vm->MouseY;
  vm->VideoDirty = TRUE;
}

static void dos_int33(void) {
  DOS_VM *vm = current();
  U16 function = r[AX];
  U16 width, height;
  dos_mouse_dimensions(&width, &height);
  dos_mouse_poll(vm);
  U16 x = vm->MouseX, y = vm->MouseY;
  U8 buttons = vm->MouseButtons;
  switch (function) {
  case 0x00: /* reset / installation check */
    DosMouseInitialize(vm);
    r[AX] = 0xffff; r[BX] = 3;
    break;
  case 0x01: /* show cursor */
    if (vm->MouseHideCount) vm->MouseHideCount--;
    vm->VideoDirty = TRUE;
    break;
  case 0x02: /* hide cursor */
    if (vm->MouseHideCount != 0xffu) vm->MouseHideCount++;
    vm->VideoDirty = TRUE;
    break;
  case 0x03: /* position and button status */
    r[BX] = buttons; r[CX] = x; r[DX] = y;
    break;
  case 0x04: /* set guest pointer position */
    vm->MouseX = dos_mouse_clip((vm->VideoMode <= 3u || vm->VideoMode == 7u) ? (r[CX] & 0xfff8u) : r[CX], vm->MouseMinX, vm->MouseMaxX);
    vm->MouseY = dos_mouse_clip((vm->VideoMode <= 3u || vm->VideoMode == 7u) ? (r[DX] & 0xfff8u) : r[DX], vm->MouseMinY, vm->MouseMaxY);
    vm->MouseLastX = vm->MouseX; vm->MouseLastY = vm->MouseY;
    vm->VideoDirty = TRUE;
    break;
  case 0x05: case 0x06: { /* button press/release counters */
    U8 button = (U8)r[BX];
    if (button > 2) { r[AX] = r[BX] = 0; break; }
    U16 count = function == 5 ? vm->MousePressCount[button] :
                                vm->MouseReleaseCount[button];
    r[AX] = buttons; r[BX] = count;
    if (function == 5) {
      r[CX] = vm->MousePressX[button]; r[DX] = vm->MousePressY[button];
      vm->MousePressCount[button] = 0;
    } else {
      r[CX] = vm->MouseReleaseX[button]; r[DX] = vm->MouseReleaseY[button];
      vm->MouseReleaseCount[button] = 0;
    }
    break;
  }
  case 0x07: /* horizontal range */
    vm->MouseMinX = (vm->VideoMode <= 3u || vm->VideoMode == 7u) ? (r[CX] & 0xfff8u) : r[CX];
    vm->MouseMaxX = (vm->VideoMode <= 3u || vm->VideoMode == 7u) ? (r[DX] & 0xfff8u) : r[DX];
    if (vm->MouseMaxX < vm->MouseMinX) vm->MouseMaxX = vm->MouseMinX;
    vm->MouseX = dos_mouse_clip(vm->MouseX, vm->MouseMinX, vm->MouseMaxX);
    vm->MouseLastX = vm->MouseX;
    vm->VideoDirty = TRUE;
    break;
  case 0x08: /* vertical range */
    vm->MouseMinY = (vm->VideoMode <= 3u || vm->VideoMode == 7u) ? (r[CX] & 0xfff8u) : r[CX];
    vm->MouseMaxY = (vm->VideoMode <= 3u || vm->VideoMode == 7u) ? (r[DX] & 0xfff8u) : r[DX];
    if (vm->MouseMaxY < vm->MouseMinY) vm->MouseMaxY = vm->MouseMinY;
    vm->MouseY = dos_mouse_clip(vm->MouseY, vm->MouseMinY, vm->MouseMaxY);
    vm->MouseLastY = vm->MouseY;
    vm->VideoDirty = TRUE;
    break;
  case 0x0b: /* motion counters */
    r[CX] = vm->MouseMotionX; r[DX] = vm->MouseMotionY;
    vm->MouseMotionX = vm->MouseMotionY = 0;
    break;
  case 0x0d: case 0x0e: /* light-pen emulation toggles are harmless no-ops */
    break;
  case 0x0f: /* mickey-to-pixel ratios */
    vm->MouseRatioX = r[CX];
    vm->MouseRatioY = r[DX];
    break;
  case 0x13: /* set double-speed threshold in mickeys per second */
    vm->MouseDoubleSpeedThreshold = r[DX] ? r[DX] : 64;
    break;
  case 0x09: /* define graphics cursor mask and hotspot */
    vm->MouseHotX = (INT16)r[BX]; vm->MouseHotY = (INT16)r[CX];
    vm->MouseCursorWidthWords = 1; vm->MouseCursorHeight = 16;
    for (U8 row = 0; row < 16; row++) {
      vm->MouseAndMask[row][0] = rw(seg[ES], (U16)(r[DX] + row * 2u));
      vm->MouseXorMask[row][0] = rw(seg[ES], (U16)(r[DX] + 32u + row * 2u));
    }
    vm->VideoDirty = TRUE;
    break;
  case 0x12: { /* define large graphics cursor (up to 128x64) */
    U8 words = (U8)(r[BX] >> 8), rows = (U8)(r[CX] >> 8);
    if (!words || words > 8 || !rows || rows > 64) {
      r[AX] = 0;
      break;
    }
    vm->MouseCursorWidthWords = words; vm->MouseCursorHeight = rows;
    vm->MouseHotX = (INT8)(U8)r[BX]; vm->MouseHotY = (INT8)(U8)r[CX];
    U16 plane_bytes = (U16)(words * rows * 2u);
    for (U8 row = 0; row < rows; row++)
      for (U8 col = 0; col < words; col++) {
        U16 index = (U16)(r[DX] + (row * words + col) * 2u);
        vm->MouseAndMask[row][col] = rw(seg[ES], index);
        vm->MouseXorMask[row][col] = rw(seg[ES], (U16)(index + plane_bytes));
      }
    vm->VideoDirty = TRUE;
    r[AX] = 0xffff;
    break;
  }
  case 0x0a: /* define text cursor */
    vm->MouseTextCursorType = (U8)r[BX];
    if (!r[BX]) {
      vm->MouseTextScreenMask = r[CX];
      vm->MouseTextCursorMask = r[DX];
    } else {
      vm->MouseTextCursorStart = (U8)(r[CX] & 0x1fu);
      vm->MouseTextCursorEnd = (U8)(r[DX] & 0x1fu);
    }
    vm->VideoDirty = TRUE;
    break;
  case 0x0c: /* install event callback */
    vm->MouseCallbackMask = r[CX];
    vm->MouseCallbackOffset = r[DX];
    vm->MouseCallbackSegment = seg[ES];
    if (r[CX] & 1u) {
      vm->MousePendingEvents |= 1u; /* publish the current pointer position */
      vm->VideoDirty = TRUE;
    }
    break;
  case 0x10: /* conditional cursor-off rectangle */
    vm->MouseConditionalOff = TRUE;
    vm->MouseConditionalLeft = r[CX]; vm->MouseConditionalTop = r[DX];
    vm->MouseConditionalRight = r[SI]; vm->MouseConditionalBottom = r[DI];
    if (x >= vm->MouseConditionalLeft && x <= vm->MouseConditionalRight &&
        y >= vm->MouseConditionalTop && y <= vm->MouseConditionalBottom) {
      if (!vm->MouseHideCount) vm->MouseHideCount = 1;
      vm->VideoDirty = TRUE;
    }
    break;
  case 0x14: { /* exchange callback */
    U16 old_mask = vm->MouseCallbackMask;
    U16 old_offset = vm->MouseCallbackOffset;
    U16 old_segment = vm->MouseCallbackSegment;
    /* Windows 1.x probes INT 33h by installing and immediately restoring a
     * zero-mask handler. Its MOUSE module enables the DOS callback path only
     * when the following version query identifies an integrated host. Keep
     * the normal SETVER result for every other DOS-version check. */
    if (!old_mask && (old_offset || old_segment) && !r[CX] &&
        !r[DX] && !seg[ES])
      vm->MouseWindowsVersionProbe = TRUE;
    vm->MouseCallbackMask = r[CX]; vm->MouseCallbackOffset = r[DX];
    vm->MouseCallbackSegment = seg[ES];
    r[CX] = old_mask; r[DX] = old_offset; seg[ES] = old_segment;
    if (vm->MouseCallbackMask & 1u) {
      vm->MousePendingEvents |= 1u; /* initial move lets Windows paint its cursor */
      vm->VideoDirty = TRUE;
    }
    break;
  }
  case 0x18: { /* alternate callback, selected by modifier state */
    U8 free_slot = 0xff;
    for (U8 i = 0; i < 3; i++) {
      if (vm->MouseAltCallbacks[i].Mask == r[CX] && r[CX]) {
        free_slot = i; break;
      }
      if (!vm->MouseAltCallbacks[i].Mask && free_slot == 0xff) free_slot = i;
    }
    if (!r[CX]) {
      memset(vm->MouseAltCallbacks, 0, sizeof(vm->MouseAltCallbacks));
      memset(vm->MouseAltPendingEvents, 0, sizeof(vm->MouseAltPendingEvents));
      r[AX] = 0x0018;
    } else if (free_slot == 0xff) {
      r[AX] = 0xffff;
    } else {
      vm->MouseAltCallbacks[free_slot].Mask = r[CX];
      vm->MouseAltCallbacks[free_slot].Offset = r[DX];
      vm->MouseAltCallbacks[free_slot].Segment = seg[ES];
      vm->MouseAltPendingEvents[free_slot] = 0;
      r[AX] = 0x0018;
    }
    break;
  }
  case 0x19: { /* return matching alternate callback vector */
    U16 requested = r[CX];
    r[BX] = 0; r[DX] = 0; r[CX] = 0;
    for (U8 i = 0; i < 3; i++)
      if (vm->MouseAltCallbacks[i].Mask == requested) {
        r[BX] = vm->MouseAltCallbacks[i].Segment;
        r[DX] = vm->MouseAltCallbacks[i].Offset;
        r[CX] = vm->MouseAltCallbacks[i].Mask;
        break;
      }
    break;
  }
  case 0x15: /* state buffer size */
    r[BX] = (U16)sizeof(DOS_MOUSE_STATE);
    break;
  case 0x16: /* save mouse-driver state */
  case 0x17: { /* restore mouse-driver state */
    U16 needed = (U16)sizeof(DOS_MOUSE_STATE);
    if (r[BX] < needed) { r[AX] = 0xffff; break; }
    dos_mouse_state_transfer(vm, seg[ES], r[DX], function == 0x17);
    break;
  }
  case 0x1a: /* set sensitivity and double-click speed */
    vm->MouseSensitivityX = r[BX]; vm->MouseSensitivityY = r[CX];
    vm->MouseDoubleSpeedThreshold = r[DX];
    break;
  case 0x1b: /* get sensitivity and double-click speed */
    r[BX] = vm->MouseSensitivityX; r[CX] = vm->MouseSensitivityY;
    r[DX] = vm->MouseDoubleSpeedThreshold;
    break;
  case 0x1c: /* PS/2 has no programmable InPort interrupt rate */
    break;
  case 0x1d: vm->MousePage = (U8)r[BX]; break;
  case 0x1e: r[BX] = vm->MousePage; break;
  case 0x1f:
    vm->MouseDisabled = TRUE;
    vm->MousePendingEvents = 0; vm->VideoDirty = TRUE;
    r[AX] = 0x001f;
    break;
  case 0x20:
    vm->MouseDisabled = FALSE; vm->MouseHostSeq = MouseState.Seq;
    vm->VideoDirty = TRUE;
    r[AX] = 0x0020;
    break;
  case 0x21: /* software reset */
    DosMouseInitialize(vm);
    r[AX] = 0xffff; r[BX] = 3;
    break;
  case 0x22: break; /* language setting ignored, like the US driver */
  case 0x23: r[BX] = 0; break;
  case 0x24: /* driver version and device type */
    r[BX] = 0x0600; r[CX] = 0x0400; r[DX] = 0;
    break;
  case 0x25: /* general driver information */
    r[AX] = vm->MouseTextCursorType ? 0x2000 : 0;
    r[BX] = r[CX] = r[DX] = 0;
    break;
  case 0x26: /* get maximum virtual coordinates */
    r[BX] = vm->MouseDisabled ? 1 : 0;
    r[CX] = width - 1u; r[DX] = height - 1u;
    break;
  case 0x27: /* get masks and motion counters */
    r[AX] = vm->MouseTextScreenMask; r[BX] = vm->MouseTextCursorMask;
    r[CX] = vm->MouseMotionX; r[DX] = vm->MouseMotionY;
    vm->MouseMotionX = vm->MouseMotionY = 0;
    break;
  default:
    r[AX] = 0;
    break;
  }
  if (vm->MouseDisabled && function != 0x00 && function != 0x1f && function != 0x20 &&
      function != 0x21 && function != 0x24) {
    if (function == 0x03) r[BX] = r[CX] = r[DX] = 0;
    else if (function == 0x05 || function == 0x06) r[AX] = r[BX] = 0;
    else r[AX] = 0;
  }
}

/* Called by the CPU when a host input event is pending. The callback uses the
 * documented far-call convention: the guest returns with RETF. */
BOOLEAN DosMouseDispatchCallback(DOS_VM *vm) {
  if (!vm || vm->ProtectedMode || vm->MouseDisabled || vm->MouseCallbackActive ||
      !(vm->Flags & 0x0200u))
    return FALSE;
  if (!vm->MousePendingEvents && !vm->MouseAltPendingEvents[0] &&
      !vm->MouseAltPendingEvents[1] && !vm->MouseAltPendingEvents[2])
    return FALSE;
  U16 mask = vm->MouseCallbackMask;
  U16 callback_offset = vm->MouseCallbackOffset;
  U16 callback_segment = vm->MouseCallbackSegment;
  U8 events = vm->MousePendingEvents & (U8)mask;
  int alt_index = -1;
  if (!events) {
    for (U8 i = 0; i < 3; i++) {
      U8 alt_events = vm->MouseAltPendingEvents[i];
      if (alt_events) {
        alt_index = i; mask = vm->MouseAltCallbacks[i].Mask;
        events = alt_events;
        callback_offset = vm->MouseAltCallbacks[i].Offset;
        callback_segment = vm->MouseAltCallbacks[i].Segment;
        break;
      }
    }
  }
  if (!events || (!callback_segment && !callback_offset)) {
    vm->MousePendingEvents = 0;
    return FALSE;
  }
  if (alt_index >= 0) vm->MouseAltPendingEvents[alt_index] &= (U8)~events;
  else vm->MousePendingEvents &= (U8)~events;
  memcpy(vm->MouseCallbackSavedRegisters, r, sizeof(vm->MouseCallbackSavedRegisters));
  memcpy(vm->MouseCallbackSavedSegments, seg, sizeof(vm->MouseCallbackSavedSegments));
  vm->MouseCallbackSavedFlags = flags;
  vm->MouseCallbackReturnCs = seg[CS]; vm->MouseCallbackReturnIp = ip;
  vm->MouseCallbackActive = TRUE;
  r[SP] -= 2; ww(seg[SS], r[SP], DOS_BUILTIN_SEGMENT);
  r[SP] -= 2; ww(seg[SS], r[SP], 0x0f00u);
  r[AX] = events; r[BX] = vm->MouseButtons;
  r[CX] = vm->MouseX; r[DX] = vm->MouseY;
  r[SI] = vm->MouseMotionX; r[DI] = vm->MouseMotionY;
  /* Do not consume the mickey counters here. Windows 1.x uses SI/DI to
   * notice the event and then calls function 0Bh from inside its callback to
   * fetch and clear the same accumulated motion. */
  seg[CS] = callback_segment;
  ip = callback_offset;
  return TRUE;
}

BOOLEAN DosMouseCallbackReturn(DOS_VM *vm) {
  if (!vm || !vm->MouseCallbackActive || seg[CS] != DOS_BUILTIN_SEGMENT ||
      ip != 0x0f00u)
    return FALSE;
  memcpy(r, vm->MouseCallbackSavedRegisters, sizeof(vm->MouseCallbackSavedRegisters));
  memcpy(seg, vm->MouseCallbackSavedSegments, sizeof(vm->MouseCallbackSavedSegments));
  flags = vm->MouseCallbackSavedFlags;
  ip = vm->MouseCallbackReturnIp; seg[CS] = vm->MouseCallbackReturnCs;
  vm->MouseCallbackActive = FALSE;
  return TRUE;
}

static DOS_DATE_TIME dos_now(void) {
  DOS_DATE_TIME result = {1980, 1, 1, 2, 0, 0, 0, 0};
  UINT64 time = KeQuerySystemTimeFileTime();
  if (!time)
    return result;
  UINT64 days = time / 864000000000ULL;
  UINT64 day_time = time % 864000000000ULL;
  result.Weekday = (U8)((days + 1) % 7); /* 1601-01-01 was Monday. */
  result.Year = 1601;
  while (result.Year < 9999) {
    BOOLEAN leap = result.Year % 4 == 0 &&
                   (result.Year % 100 != 0 || result.Year % 400 == 0);
    U16 length = leap ? 366 : 365;
    if (days < length)
      break;
    days -= length;
    result.Year++;
  }
  static const U8 month_days[12] = {31, 28, 31, 30, 31, 30,
                                     31, 31, 30, 31, 30, 31};
  result.Month = 1;
  for (unsigned month = 0; month < 12; month++) {
    U16 length = month_days[month];
    if (month == 1 && result.Year % 4 == 0 &&
        (result.Year % 100 != 0 || result.Year % 400 == 0))
      length++;
    if (days < length)
      break;
    days -= length;
    result.Month++;
  }
  result.Day = (U8)days + 1;
  UINT64 seconds = day_time / 10000000ULL;
  result.Hour = (U8)(seconds / 3600);
  result.Minute = (U8)((seconds / 60) % 60);
  result.Second = (U8)(seconds % 60);
  result.Hundredths = (U8)((day_time % 10000000ULL) / 100000ULL);
  return result;
}

/* Load-only EXEC keeps DOSX in the caller's address space. */
static U16 dos_exec_shared(BOOLEAN execute) {
  char path[260];
  if (!guest_path(seg[DS], r[DX], path)) return 3;
  void *image = NULL;
  SIZE_T size = 0;
  if (!NT_SUCCESS(FatLoadFile(path, &image, &size, NULL))) return 2;
  U8 *h = image;
  if (size < 28 || h[0] != 'M' || h[1] != 'Z') { ExFreePool(image); return 11; }
  U32 length = (U32)word(h + 4) * 512u;
  if (word(h + 2)) length -= 512u - word(h + 2);
  U32 header = (U32)word(h + 8) * 16u;
  if (length > size || header < 28 || header > length ||
      (U32)word(h + 24) + (U32)word(h + 6) * 4u > header) {
    ExFreePool(image); return 11;
  }
  U32 bytes = length - header;
  U32 image_paras = (bytes + 15u) / 16u;
  U32 minimum = 16u + image_paras + word(h + 10);
  if (word(h + 12) < word(h + 10)) { ExFreePool(image); return 11; }
  BOOLEAN high = !word(h + 10) && !word(h + 12);
  for (U32 i = 0; i < word(h + 6); i++) {
    U8 *rel = h + word(h + 24) + i * 4u;
    U32 pos = (U32)word(rel + 2) * 16u + word(rel);
    if (pos + 2 > (high ? image_paras * 16u : minimum * 16u - 256u)) { ExFreePool(image); return 11; }
  }
  U16 block = r[BX], bs = seg[ES];
  U16 source_env = rw(bs, block);
  if (!source_env) source_env = rw(current()->CurrentPsp, 0x2c);
  U8 environment[DOS_ENV_SIZE];
  U32 env_bytes = 0;
  if (source_env) {
    while (env_bytes + 1u < DOS_ENV_SIZE) {
      environment[env_bytes] = rb(source_env, (U16)env_bytes);
      if (env_bytes && !environment[env_bytes] && !environment[env_bytes - 1u]) { env_bytes++; break; }
      env_bytes++;
    }
  } else { environment[0] = environment[1] = 0; env_bytes = 2; }
  U32 path_size = 0; while (path[path_size]) path_size++;
  if (env_bytes + 2u + path_size + 1u > DOS_ENV_SIZE) { ExFreePool(image); return 10; }
  environment[env_bytes++] = 1; environment[env_bytes++] = 0;
  for (U32 i = 0; i <= path_size; i++) environment[env_bytes++] = (U8)path[i];
  U16 env = dos_alloc((U16)((env_bytes + 15u) / 16u));
  if (!env) { ExFreePool(image); return 8; }
  U32 available = dos_largest_free();
  U32 paras = high ? available : 16u + image_paras + word(h + 12);
  if (paras > available) paras = available;
  U16 psp = minimum <= available ? dos_alloc((U16)paras) : 0;
  if (!psp) {
    for (U32 i = 0; i < DOS_MAX_BLOCKS; i++)
      if (current()->Blocks[i].Used && current()->Blocks[i].Segment == env) current()->Blocks[i].Used = FALSE;
    DosUpdateMemoryControlBlocks(current());
    ExFreePool(image); return 8;
  }
  memcpy(mem + ((U32)env << 4), environment, env_bytes);
  KeLog("[load] psp=%04x paras=%04x cs=%04x ip=%04x\n",psp,paras,psp+16+word(h+22),word(h+20));
  U16 base = high ? (U32)psp + paras - image_paras : psp + 16u;
  memcpy(mem + ((U32)base << 4), h + header, bytes);
  for (U32 i = 0; i < word(h + 6); i++) {
    U8 *rel = h + word(h + 24) + i * 4u;
    U32 pos = (U32)word(rel + 2) * 16u + word(rel);
    U32 linear = ((U32)base << 4) + pos;
    U16 relocated = word(mem + linear) + base;
    mem[linear] = (U8)relocated;
    mem[linear + 1] = (U8)(relocated >> 8);
  }
  memcpy(mem + ((U32)psp << 4), mem + ((U32)current()->CurrentPsp << 4), 256);
  /* The inherited inline JFT moves with its PSP. */
  if (rw(psp, 0x34) == 0x18) ww(psp, 0x36, psp);
  ww(psp, 2, psp + (U16)paras);
  ww(psp, 0x16, current()->CurrentPsp);
  ww(psp, 0x2c, env);
  for (U32 i = 0; i < DOS_MAX_BLOCKS; i++)
    if (current()->Blocks[i].Used && (current()->Blocks[i].Segment == psp || current()->Blocks[i].Segment == env))
      current()->Blocks[i].OwnerPsp = psp;
  DosUpdateMemoryControlBlocks(current());
  U16 ts = rw(bs, block + 4), to = rw(bs, block + 2);
  U8 n = rb(ts, to); if (n > 126) n = 126;
  for (U16 i = 0; i < n + 2u; i++) wb(psp, 0x80u + i, rb(ts, to + i));
  U16 entry_sp = word(h + 0x10), entry_ss = base + word(h + 0x0e);
  U16 entry_ip = word(h + 0x14), entry_cs = base + word(h + 0x16);
  if (execute) {
    U16 parent = current()->CurrentPsp;
    U16 return_ip = ip, return_cs = seg[CS], return_sp = r[SP];
    if (return_cs == DOS_BUILTIN_SEGMENT && return_ip == DOS_BUILTIN_OFFSET(0x21)) {
      return_ip = rw(seg[SS], return_sp);
      return_cs = rw(seg[SS], (U16)(return_sp + 2u));
      return_sp += 6u;
    }
    ww(parent, 0x2e, return_sp); ww(parent, 0x30, seg[SS]);
    ww(psp, 0x0a, return_ip); ww(psp, 0x0c, return_cs);
    for (U32 i = 0; i < DOS_MAX_BLOCKS; i++)
      if (current()->Blocks[i].Used && current()->Blocks[i].Segment == psp)
        current()->Blocks[i].OwnerPsp = psp;
    DosUpdateMemoryControlBlocks(current());
    seg[CS] = entry_cs; ip = entry_ip;
    seg[SS] = entry_ss; r[SP] = entry_sp;
    seg[DS] = seg[ES] = psp;
  }
  ww(bs, block + 0x0e, word(h + 0x10));
  ww(bs, block + 0x10, base + word(h + 0x0e));
  ww(bs, block + 0x12, word(h + 0x14));
  ww(bs, block + 0x14, base + word(h + 0x16));
  current()->CurrentPsp = psp;
  current()->DtaSegment = psp; current()->DtaOffset = 0x80;
  ExFreePool(image);
  return 0;
}

static U16 dos_exec(void) {
  DOS_VM *parent = current();
  if (parent->ExecDepth >= 4)
    return 8;
  char path[260];
  if (!guest_path(seg[DS], r[DX], path))
    return 3;
  void *image = NULL;
  SIZE_T size = 0;
  NTSTATUS status = FatLoadFile(path, &image, &size, NULL);
  if (!NT_SUCCESS(status)) {
    unsigned n = 0;
    while (path[n]) n++;
    BOOLEAN has_extension = FALSE;
    for (unsigned i = n; i > 0 && path[i - 1] != '\\'; i--)
      if (path[i - 1] == '.') has_extension = TRUE;
    if (has_extension || n + 4 >= sizeof(path))
      return 2;
    const char *extensions[2] = {".COM", ".EXE"};
    for (unsigned i = 0; i < 2 && !NT_SUCCESS(status); i++) {
      for (unsigned j = 0; j < 5; j++)
        path[n + j] = extensions[i][j];
      status = FatLoadFile(path, &image, &size, NULL);
    }
    if (!NT_SUCCESS(status)) {
      return 2;
    }
  }
  if (!DosIsImage(path, image, size)) {
    ExFreePool(image);
    return 11;
  }
  /* The embedded DOS/4GW stub starts the protected-mode image by EXECing an
   * identical copy of itself. That is a one-way bootstrap, not a shell-like
   * child process. When its protected child exits, terminate the dormant stub
   * too; otherwise some DOS/4GW builds re-enter the bootstrap and restart the
   * game. Ordinary DOS self-EXEC remains unchanged. */
  BOOLEAN dos4g_bootstrap = parent->Dos4gImage && parent->ExecDepth == 0 &&
      size == parent->ImageSize &&
      DosImageHash((const U8 *)image, (U32)size) == parent->ImageHash;
  U16 block = r[BX], block_segment = seg[ES];
  U16 tail_offset = rw(block_segment, (U16)(block + 2));
  U16 tail_segment = rw(block_segment, (U16)(block + 4));
  U8 tail_length = tail_segment || tail_offset ? rb(tail_segment, tail_offset) : 0;
  if (tail_length > 126) {
    ExFreePool(image);
    return 1;
  }
  char command[390];
  unsigned n = 0;
  BOOLEAN quote = FALSE;
  for (unsigned i = 0; path[i]; i++)
    if (path[i] == ' ' || path[i] == '\t') quote = TRUE;
  if (quote) command[n++] = '"';
  unsigned path_index = 0;
  while (path[path_index] && path_index < 259)
    command[n++] = path[path_index++];
  if (quote) command[n++] = '"';
  if (tail_length && rb(tail_segment, (U16)(tail_offset + 1)) != ' ' &&
      rb(tail_segment, (U16)(tail_offset + 1)) != '\t')
    command[n++] = ' ';
  for (unsigned i = 0; i < tail_length; i++)
    command[n++] = (char)rb(tail_segment, (U16)(tail_offset + 1 + i));
  command[n] = 0;
  PVOID context = NULL;
  status = DosCreateVm(image, size, command, &context);
  ExFreePool(image);
  if (!NT_SUCCESS(status))
    return status == STATUS_NO_MEMORY ? 8 : 11;
  DOS_VM *child = (DOS_VM *)context;
  child->ExecDepth = parent->ExecDepth + 1;
  /* Preserve the explicit SETVER override selected for WIN100.BIN (3.34).
   * Other EXEC children inherit their parent's compatibility version, which
   * lets Windows 3.1 startup helpers keep the DOS 3.30 setting. */
  if (child->DosVersion != 0x2203u)
    child->DosVersion = parent->DosVersion;
  /* EXEC shares the display hardware and BIOS data with its caller. */
  DosVideoInherit(child, parent);
  memcpy(child->Memory + 0x400u, parent->Memory + 0x400u, 0x100u);
  U16 source_environment = rw(block_segment, block);
  if (!source_environment)
    source_environment = rw(PSP, 0x2c);
  U32 source_address = (U32)source_environment << 4;
  if (source_environment && source_address + DOS_ENV_SIZE <= MEM_SIZE)
    memcpy(child->Memory + ((U32)DOS_ENV_SEGMENT << 4),
           parent->Memory + source_address, DOS_ENV_SIZE);
  else if (source_environment) {
    DosDestroyVm(child);
    return 10;
  }
  for (unsigned i = 5; i < 20; i++)
    if (parent->Files[i]) {
      child->Files[i] = parent->Files[i];
      child->Files[i]->References++;
    }
  KeGetCurrentThread()->DosContext = child;
  U8 child_status = run_vm(child);
  U8 termination_type = child->Fault ? 2 : 0;
  KeGetCurrentThread()->DosContext = parent;
  DosVideoInherit(parent, child);
  DosDestroyVm(child);
  parent->LastChildExitCode = child_status;
  parent->LastChildTerminationType = termination_type;
  parent->ChildStatusAvailable = TRUE;
  if (dos4g_bootstrap) {
    parent->ExitCode = child_status;
    parent->Running = FALSE;
  }
  return 0;
}

static void dos_sync_system_data(void) {
  PKPROCESS process = KeGetCurrentThread()->Process;
  const char *cwd = process ? process->CurrentDirectory : "C:\\";
  DosSyncSystemData(current(), cwd);
}

static BOOLEAN dos_terminate_nested_psp(U8 code) {
  DOS_VM *vm = current();
  U16 psp = vm->CurrentPsp;
  if (psp == PSP) return FALSE;
  U16 parent = rw(psp, 0x16u);
  if (!parent) return FALSE;

  U16 terminate_offset = rw(psp, 0x0au);
  U16 terminate_segment = rw(psp, 0x0cu);
  for (U16 vector = 0x23u; vector <= 0x24u; vector++) {
    U16 field = (U16)(0x0eu + (vector - 0x23u) * 4u);
    ww(0, (U16)(vector * 4u), rw(psp, field));
    ww(0, (U16)(vector * 4u + 2u), rw(psp, (U16)(field + 2u)));
  }
  for (U32 i = 0; i < DOS_MAX_BLOCKS; i++) {
    DOS_BLOCK *block = &vm->Blocks[i];
    if (block->Used && block->OwnerPsp == psp)
      block->Used = FALSE;
  }
  DosUpdateMemoryControlBlocks(vm);

  vm->LastChildExitCode = code;
  vm->LastChildTerminationType = 0;
  vm->ChildStatusAvailable = TRUE;
  vm->CurrentPsp = parent;
  vm->DtaSegment = parent;
  vm->DtaOffset = 0x80u;
  seg[SS] = rw(parent, 0x30u);
  r[SP] = rw(parent, 0x2eu);
  seg[CS] = terminate_segment;
  ip = terminate_offset;
  dos_sync_system_data();
  return TRUE;
}

static void dos_int_dispatch(U8 n) {
  U8 ah = (U8)(r[AX] >> 8);
  U32 done = 0;
  U16 off = r[DX];
  /* The XMS far-call bridge is a resident driver entry, independent of
   * application hooks on the multiplex interrupt used to implement it. */
  if (n == 0x2f && seg[CS] == 0xf000u && ip == 0x0607u) {
    dos_xms_call(current());
    return;
  }
  /* A non-null IVT entry is a guest-installed real-mode handler. */
  U16 vector_off = rw(0, (U16)(n * 4u));
  U16 vector_seg = rw(0, (U16)(n * 4u + 2u));
  if ((vector_off || vector_seg) &&
      !(vector_seg == DOS_BUILTIN_SEGMENT && vector_off == DOS_BUILTIN_OFFSET(n))) {
    if (n == 6)
      KeLog("[dos] INT 6h target=%04x:%04x from=%04x:%04x SS:SP=%04x:%04x\n",
            vector_seg, vector_off, seg[CS], ip, seg[SS], r[SP]);
    r[SP] -= 2; ww(seg[SS], r[SP], flags | 2u);
    r[SP] -= 2; ww(seg[SS], r[SP], seg[CS]);
    r[SP] -= 2; ww(seg[SS], r[SP], ip);
    flags &= ~(0x0100u | 0x0200u);
    seg[CS] = vector_seg;
    ip = vector_off;
    return;
  }
  /* Timer/keyboard IRQ bookkeeping is maintained by the virtual PIT/8042.
   * Default BIOS chaining targets simply acknowledge/return. */
  if (n == 0x09) { dos_bios_keyboard(); return; }
  if (n == 0x08 || n == 0x1c || n == 0x28) return;
  /* The BIOS overflow vector is an IRET stub when no handler is installed.
   * INTO is common in DOS startup code, so a null IVT entry must not be
   * treated as an unsupported host interrupt. */
  if (n == 0x04) return;
  if (n == 0x28) return; /* DOS idle notification; guest hooks run above. */
  if (n == 0x15) {
    /* Report the DOS VM's 16 MiB XMS arena through the BIOS memory probes
     * used by DOSX and HIMEM-compatible managers.  The arena spans 1 MiB to
     * 17 MiB: 15 MiB below 16 MiB and sixteen 64 KiB blocks above it. */
    if (r[AX] == 0xe801u) {
      r[AX] = r[CX] = 15u * 1024u;
      r[BX] = r[DX] = 16u;
      flags &= ~CF;
      return;
    }
    if (ah == 0x88u) {
      r[AX] = 15u * 1024u;
      flags &= ~CF;
      return;
    }
  }
  if (n == 0x2f) {
    switch (r[AX]) {
    case 0x1600: /* Windows-version query: no Windows host is resident yet. */
      set8(0, 0x00);
      seg[ES] = 0;
      r[BX] = 0;
      return;
    case 0x1605: /* Windows task-start broadcast. */
    case 0x1606: /* Windows task-end broadcast. */
      return;
    case 0x160a: /* Windows/DOSX initialization broadcast. */
      return;
    case 0x1680: /* DOS idle/yield request. */
      r[AX] = 0;
      return;
    case 0x1686: /* DPMI protected-mode query. */
      r[AX] = current()->ProtectedMode ? 0 : 1;
      return;
    case 0x1687: /* DPMI host installation query is handled below. */
      break;
    case 0x4300: /* XMS installation query is handled below. */
    case 0x4310: /* XMS entry-point query is handled below. */
      break;
    default:
      break;
    }
    if (r[AX] == 0x4300u) { /* XMS installation check. */
      set8(0, 0x80u);
      flags &= ~CF;
      return;
    }
    if (r[AX] == 0x4310u) { /* Return XMS entry point in ES:BX. */
      seg[ES] = 0xf000u;
      r[BX] = 0x0600u;
      flags &= ~CF;
      return;
    }
    if (r[AX] == 0x1687) {
      KeLog("[dos] INT 2Fh AX=1687 DPMI host discovery\n");
      flags &= ~CF; /* successful multiplex discovery must not leak old CF */
      r[AX] = 0;
      r[BX] = 1;              /* 32-bit protected-mode clients supported */
      r[CX] = 3;               /* CL=80386, CH reserved and clear */
      r[DX] = 0x0100;          /* DPMI 1.0 */
      r[SI] = 1;               /* one paragraph of private host data */
      seg[ES] = 0xf000;
      r[DI] = 0x0100;          /* synthetic mode-switch entry */
      KeLog("[dos] DPMI host response AX=%04x BX=%04x CX=%04x DX=%04x SI=%04x ES:DI=%04x:%04x\n",
            r[AX], r[BX], r[CX], r[DX], r[SI], seg[ES], r[DI]);
    } else {
      r[AX] = 0;
    }
    return;
  }
  if (n == 0x20) {
    KeLog("[dos] INT20 terminate psp=%04x from=%04x:%04x ss:sp=%04x:%04x ax=%04x\n",current()->CurrentPsp,seg[CS],ip,seg[SS],r[SP],r[AX]);
    running = 0;
    exit_code = 0;
    return;
  }
  if (n == 0x29) {
    U8 ch = get8(0);
    output(1, &ch, 1);
    return;
  }
  if (n == 0x33) {
    dos_int33();
    return;
  }
  if (n == 0x10 && (ah == 0x01 || ah == 0x02 || ah == 0x03)) {
    U16 page = (U16)(r[BX] >> 8) & 7u;
    if (ah == 0x01) ww(0, 0x460, r[CX]);
    else if (ah == 0x02) ww(0, (U16)(0x450u + page * 2u), r[DX]);
    else {
      r[DX] = rw(0, (U16)(0x450u + page * 2u));
      r[CX] = rw(0, 0x460);
    }
    return;
  }
  if (n == 0x10 && (ah == 0x09 || ah == 0x0a)) {
    U16 page = (U16)(r[BX] >> 8) & 7u;
    U16 cursor = rw(0, (U16)(0x450u + page * 2u));
    U32 cell = (U32)(cursor >> 8) * 80u + (cursor & 0xffu);
    U32 base = 0xb8000u + (U32)page * 4096u;
    for (U32 i = 0; i < r[CX] && cell + i < 2000u; i++) {
      current()->Memory[base + (cell + i) * 2u] = (U8)r[AX];
      if (ah == 0x09) current()->Memory[base + (cell + i) * 2u + 1u] = (U8)r[BX];
    }
    return;
  }
  if (n == 0x10 && ah == 0x0e) {
    U8 ch = get8(0);
    output(1, &ch, 1);
    return;
  }
  if (n == 0x10 && ah == 0x00) {
    DosVideoSetMode(current(), get8(0));
    return;
  }
  if (n == 0x10 && ah == 0x0f) {
    r[AX] = (U16)(0x5000u | current()->VideoMode);
    r[BX] &= 0x00ffu; /* active display page is zero */
    return;
  }
  if (n == 0x10 && r[AX] == 0x6f00u) {
    /* Video7/VEGA extension probe. No V7 signature means generic VGA. */
    flags &= ~CF;
    return;
  }
  if (n == 0x10 && r[AX] == 0x1130u) {
    DosVideoInitializeFonts(current());
    U8 which = get8(7);
    U16 offset = which == 2 ? 0x1000 : (which == 3 || which == 4) ? 0x2000 : 0;
    if (which == 4) offset += 128u * 8u;
    seg[ES] = 0xc000; r[BP] = offset;
    r[CX] = which == 2 ? 14 : (which == 3 || which == 4) ? 8 : 16;
    set8(2, current()->Memory[0x484] ? current()->Memory[0x484] : 24);
    return;
  }
  if (n == 0x10 && ah == 0x10) {
    DOS_VM *vm = current();
    U8 subfunction = get8(0);
    U8 palette_index = get8(3); /* BL: palette register number */
    U8 value = get8(7);         /* BH: palette value */
    switch (subfunction) {
    case 0x00: /* Set one EGA attribute/palette register. */
      DosVideoSetPaletteRegister(vm, palette_index, value);
      return;
    case 0x01: /* Set overscan/border register. */
      DosVideoSetPaletteRegister(vm, 0x11u, value);
      return;
    case 0x02: /* Set 16 palette registers and overscan from ES:DX. */
      for (U8 i = 0; i < 16u; i++)
        DosVideoSetPaletteRegister(vm, i, rb(seg[ES], (U16)(r[DX] + i)));
      DosVideoSetPaletteRegister(vm, 0x11u,
                                 rb(seg[ES], (U16)(r[DX] + 16u)));
      return;
    case 0x03: /* BL=0 intensity, BL=1 blinking. */
      if (palette_index <= 1u) {
        if (palette_index) vm->VgaAttribute[0x10] |= 0x08u;
        else vm->VgaAttribute[0x10] &= (U8)~0x08u;
        vm->VideoDirty = TRUE;
      }
      return;
    case 0x07: /* Read one palette register into BH. */
      set8(7, DosVideoGetPaletteRegister(vm, palette_index));
      return;
    case 0x08: /* Read overscan register into BH. */
      set8(7, DosVideoGetPaletteRegister(vm, 0x11u));
      return;
    case 0x09: /* Copy 16 palette registers and overscan to ES:DX. */
      for (U8 i = 0; i < 16u; i++)
        wb(seg[ES], (U16)(r[DX] + i), DosVideoGetPaletteRegister(vm, i));
      wb(seg[ES], (U16)(r[DX] + 16u),
         DosVideoGetPaletteRegister(vm, 0x11u));
      return;
    case 0x10: { /* Set one VGA DAC register: DH=R, CH=G, CL=B. */
      U32 index = r[BX] & 0xffu;
      U32 at = index * 3u;
      vm->VgaDac[at] = get8(6) & 0x3fu;
      vm->VgaDac[at + 1u] = get8(1) & 0x3fu;
      vm->VgaDac[at + 2u] = get8(5) & 0x3fu;
      vm->VideoDirty = TRUE;
      return;
    }
    case 0x12: { /* Set CX consecutive DAC registers from RGB triples. */
      U32 index = r[BX] & 0xffu;
      U32 count = r[CX];
      if (count > 256u - index) count = 256u - index;
      for (U32 i = 0; i < count; i++) {
        U32 at = (index + i) * 3u;
        U16 offset = (U16)(r[DX] + i * 3u);
        vm->VgaDac[at] = rb(seg[ES], offset) & 0x3fu;
        vm->VgaDac[at + 1u] = rb(seg[ES], (U16)(offset + 1u)) & 0x3fu;
        vm->VgaDac[at + 2u] = rb(seg[ES], (U16)(offset + 2u)) & 0x3fu;
      }
      vm->VideoDirty = TRUE;
      return;
    }
    case 0x15: { /* Read one VGA DAC register: DH=R, CH=G, CL=B. */
      U32 at = (U32)palette_index * 3u;
      set8(6, vm->VgaDac[at]);
      set8(1, vm->VgaDac[at + 1u]);
      set8(5, vm->VgaDac[at + 2u]);
      return;
    }
    case 0x17: { /* Read a block of VGA DAC registers to ES:DX. */
      U32 index = r[BX] & 0xffu;
      U32 count = r[CX];
      if (count > 256u - index) count = 256u - index;
      for (U32 i = 0; i < count; i++) {
        U32 at = (index + i) * 3u;
        U16 offset = (U16)(r[DX] + i * 3u);
        wb(seg[ES], offset, vm->VgaDac[at]);
        wb(seg[ES], (U16)(offset + 1u), vm->VgaDac[at + 1u]);
        wb(seg[ES], (U16)(offset + 2u), vm->VgaDac[at + 2u]);
      }
      return;
    }
    default:
      /* Reserved palette calls are ignored like a tolerant VGA BIOS. */
      return;
    }
  }
  if (n == 0x10 && ah == 0x0b) {
    DOS_VM *vm = current();
    U8 operation = get8(7); /* BH selects the BIOS subfunction. */
    U8 color = get8(3);     /* BL carries the color/palette. */
    if (operation == 0x00u) {
      /* In mode 6 this is the foreground; in modes 4/5 it is background. */
      DosVideoSetPaletteRegister(vm, vm->VideoMode == 0x06u ? 1u : 0u,
                                 dos_cga_palette_to_ega(color));
    } else if (operation == 0x01u &&
               (vm->VideoMode == 0x04u || vm->VideoMode == 0x05u)) {
      U8 first = color ? 0x03u : 0x02u;
      U8 second = color ? 0x05u : 0x04u;
      U8 third = color ? 0x07u : 0x06u;
      DosVideoSetPaletteRegister(vm, 1, dos_cga_palette_to_ega(first));
      DosVideoSetPaletteRegister(vm, 2, dos_cga_palette_to_ega(second));
      DosVideoSetPaletteRegister(vm, 3, dos_cga_palette_to_ega(third));
    }
    return;
  }
  if (n == 0x10 && ah == 0x0c) {
    DosVideoWritePixel(current(), r[CX], r[DX], get8(0));
    return;
  }
  if (n == 0x10 && ah == 0x0d) {
    set8(0, DosVideoReadPixel(current(), r[CX], r[DX]));
    return;
  }
  if (n == 0x10 && ah == 0x1a && get8(0) == 0x00) {
    /* Report an EGA/VGA-compatible display combination. */
    set8(0, 0x1au);
    set8(3, 0x08u); /* EGA color display. */
    set8(7, 0x00u); /* no alternate display adapter */
    return;
  }
  if (n == 0x10 && ah == 0x12 && get8(3) == 0x10u) {
    /* EGA configuration query: color adapter with 256 KiB video RAM. */
    set8(7, 0x00u); /* BH: color mode / I/O base 3Dxh */
    set8(3, 0x03u); /* BL: 256 KiB installed */
    set8(1, 0x00u); /* CH: feature bits */
    set8(5, 0x00u); /* CL: switch settings */
    return;
  }
  if (n == 0x11) {
    /* BIOS equipment word: one floppy, EGA/VGA display, no extra cards. */
    r[AX] = 0x0001u;
    return;
  }
  if (n == 0x12) {
    /* 640 KiB conventional memory, matching the DOS VM's real-mode arena. */
    r[AX] = 640;
    return;
  }
  if (n == 0x1a) {
    if (ah == 0) {
      r[CX] = rw(0x40, 0x6e); r[DX] = rw(0x40, 0x6c);
      set8(0, rb(0x40, 0x70)); wb(0x40, 0x70, 0); flags &= ~CF;
    } else if (ah == 2 || ah == 4) {
      DOS_DATE_TIME now = dos_now();
      U8 a = ah == 2 ? now.Hour : now.Year / 100;
      U8 b = ah == 2 ? now.Minute : now.Year % 100;
      U8 c = ah == 2 ? now.Second : now.Month;
      U8 d = ah == 2 ? 0 : now.Day;
      r[CX] = (U16)(((a / 10u) * 16u + a % 10u) << 8) | ((b / 10u) * 16u + b % 10u);
      r[DX] = (U16)(((c / 10u) * 16u + c % 10u) << 8) | ((d / 10u) * 16u + d % 10u);
      flags &= ~CF;
    } else { set8(4, 0x86); flags |= CF; }
    return;
  }
  if (n == 0x16 && r[AX] == 0x6f00u) {
    /* HP Vectra EX-BIOS installation probe: no vendor BIOS installed. */
    return;
  }
  if (n == 0x16 && (ah == 0x01 || ah == 0x11)) {
    U16 head = rw(0x40u, 0x1au);
    if (head == rw(0x40u, 0x1cu)) flags |= ZF;
    else { r[AX] = rw(0x40u, head); flags &= ~ZF; }
    return;
  }
  if (n == 0x16 && (ah == 0 || ah == 0x10)) {
    U16 head = rw(0x40u, 0x1au);
    if (head != rw(0x40u, 0x1cu)) {
      r[AX] = rw(0x40u, head);
      ww(0x40u, 0x1au, head + 2u < 0x3eu ? head + 2u : 0x1eu);
      return;
    }
    U8 ch = 0;
    transfer(0, &ch, 1, &done, FALSE);
    r[AX] = ch;
    return;
  }
  if (n == 0x16 && ah == 0x02) {
    set8(0, rb(0x0040u, 0x0017u)); /* BIOS keyboard shift/lock flags */
    return;
  }
  if (n == 0x15) {
    /* Unknown BIOS INT 15h services report "function not supported" via
     * CF/AH=86h.  DOS extenders probe optional BIOS extensions and must be
     * allowed to fall back when one is absent. */
    r[AX] = (r[AX] & 0x00ffu) | 0x8600u;
    flags |= CF;
    return;
  }
  if (n == 0xe4) {
    /* Optional third-party monitor hook (e.g. Logitech Modula). Uninstalled
     * high vectors in a PC BIOS return with registers preserved. */
    return;
  }
  if (n >= 0x40u &&
      ((rw(0, (U16)(n * 4u)) == DOS_BUILTIN_OFFSET(n) &&
        rw(0, (U16)(n * 4u + 2u)) == DOS_BUILTIN_SEGMENT) ||
       (!rw(0, (U16)(n * 4u)) && !rw(0, (U16)(n * 4u + 2u))))) {
    /* BIOS/DOS leave unclaimed software vectors at/above 40h as IRET stubs
     * (or null). DOSX and Windows probe optional vectors; preserve registers. */
    return;
  }
  if (n != 0x21) {
    KeLog("[dos] unsupported INT %02x AX=%04x BX=%04x CX=%04x DX=%04x at %04x:%04x\n",
          n, r[AX], r[BX], r[CX], r[DX], seg[CS], ip);
    fail("DOSVM: unsupported interrupt\r\n");
    return;
  }
  dos_sync_system_data();
  flags &= ~CF;
  switch (ah) {
  case 0x11:
  case 0x12:
    set8(0, dos_find_fcb(seg[DS], off, ah == 0x11) ? 0 : 0xffu);
    break;
  case 0x00:
    KeLog("[dos] AH00 terminate psp=%04x from=%04x:%04x ss:sp=%04x:%04x ax=%04x\n",current()->CurrentPsp,seg[CS],ip,seg[SS],r[SP],r[AX]);
    running = 0;
    exit_code = 0;
    break;
  case 0x0c: {
    U8 input_function = get8(0);
    /* DOS flushes input before dispatching a supported buffered/character
     * input service; unsupported AL values still leave the input queue
     * flushed, as on DOS 3.x. */
    IoFlushInputHandleKernel(standard_handle(0));
    switch (input_function) {
    case 0x01: case 0x06: case 0x07: case 0x08: case 0x0a:
      set8(4, input_function);
      dos_int_dispatch(0x21);
      return;
    default:
      break;
    }
    break;
  }
  case 0x01:
  case 0x07:
  case 0x08: {
    U8 ch = 0;
    transfer(0, &ch, 1, &done, FALSE);
    set8(0, ch);
    if (ah == 1)
      output(1, &ch, 1);
    break;
  }
  case 0x02: {
    U8 ch = (U8)r[DX];
    output(1, &ch, 1);
    set8(0, ch);
    break;
  }
  case 0x06: {
    U8 ch = (U8)r[DX];
    if (ch == 0xff) {
      SIZE_T read = 0;
      if (NT_SUCCESS(IoTryReadHandleKernel(standard_handle(0), &ch, 1,
                                           &read)))
        done = (U32)read;
      if (!done) {
        flags |= ZF;
        set8(0, 0);
      } else {
        flags &= ~ZF;
        set8(0, ch);
      }
    } else
      output(1, &ch, 1);
    break;
  }
  case 0x09: {
    U16 p = off;
    while (rb(seg[DS], p) != '$' && (U16)(p - off) < 65535) {
      U8 ch = rb(seg[DS], p++);
      output(1, &ch, 1);
    }
    break;
  }
  case 0x0a: {
    U8 limit = rb(seg[DS], off), nread = 0, ch;
    if (!limit) {
      wb(seg[DS], (U16)(off + 1), 0);
      break;
    }
    /* Byte zero includes room for the trailing CR. */
    while (nread + 1 < limit) {
      if (!transfer(0, &ch, 1, &done, FALSE) || !done || ch == '\r' ||
          ch == '\n')
        break;
      wb(seg[DS], (U16)(off + 2 + nread++), ch);
    }
    wb(seg[DS], (U16)(off + 1), nread);
    wb(seg[DS], (U16)(off + 2 + nread), '\r');
    break;
  }
  case 0x0b: {
    BOOLEAN available = FALSE;
    if (!NT_SUCCESS(IoHandleInputAvailableKernel(standard_handle(0),
                                                  &available))) {
      r[AX] = 6;
      flags |= CF;
    } else
      set8(0, available ? 0xff : 0);
    break;
  }
  case 0x0e:
    /* The mounted FAT volume is DOS drive C:. AL reports LASTDRIVE. */
    set8(0, 3);
    break;
  case 0x19:
    set8(0, 2);
    break;
  case 0x1a:
    current()->DtaSegment = seg[DS];
    current()->DtaOffset = off;
    dos_sync_system_data();
    break;
  case 0x2f:
    seg[ES] = current()->DtaSegment;
    r[BX] = current()->DtaOffset;
    break;
  case 0x29:
    set8(0, dos_parse_fcb());
    break;
  case 0x4e: {
    char path[260];
    if (!guest_path(seg[DS], off, path)) {
      r[AX] = 3;
      flags |= CF;
      break;
    }
    unsigned last = 0, i = 0;
    while (path[i]) {
      if (path[i] == '\\')
        last = i + 1;
      i++;
    }
    DOS_VM *vm = current();
    if (!last || !path[last]) {
      r[AX] = 3;
      flags |= CF;
      break;
    }
    unsigned j = 0;
    while (j < last) {
      vm->SearchDirectory[j] = path[j];
      j++;
    }
    vm->SearchDirectory[j] = 0;
    FAT_FIND_DATA directory;
    if (!NT_SUCCESS(FatQueryPath(vm->SearchDirectory, &directory)) ||
        !(directory.Attributes & 0x10u)) {
      vm->SearchActive = FALSE;
      r[AX] = 3;
      flags |= CF;
      break;
    }
    dos_name_mask(path + last, vm->SearchPattern);
    vm->SearchIndex = 0;
    vm->SearchAttributes = r[CX];
    vm->SearchDtaSegment = vm->DtaSegment;
    vm->SearchDtaOffset = vm->DtaOffset;
    vm->SearchActive = TRUE;
    if (!dos_find_next()) {
      vm->SearchActive = FALSE;
      r[AX] = 18;
      flags |= CF;
    } else
      r[AX] = 0;
    break;
  }
  case 0x4f:
    if (!dos_find_next()) {
      r[AX] = 18;
      flags |= CF;
    } else
      r[AX] = 0;
    break;
  case 0x25:
    ww(0, (U16)((U8)r[AX] * 4u), r[DX]);
    ww(0, (U16)((U8)r[AX] * 4u + 2u), seg[DS]);
    break;
  case 0x34: /* Get address of DOS InDOS flag. */
    seg[ES] = DOS_SYSTEM_DATA_SEGMENT;
    r[BX] = (U16)(DOS_SDA_OFFSET + 1u);
    break;
  case 0x35: {
    U16 vector = (U8)r[AX];
    r[BX] = rw(0, (U16)(vector * 4u));
    seg[ES] = rw(0, (U16)(vector * 4u + 2u));
    break;
  }
  case 0x2a: {
    DOS_DATE_TIME now = dos_now();
    r[CX] = now.Year;
    r[DX] = ((U16)now.Month << 8) | now.Day;
    set8(0, now.Weekday);
    break;
  }
  case 0xdc: /* Windows' DOS environment probe; absent on ordinary DOS. */
    set8(0, 0);
    break;
  case 0x2b: /* Set date, including DESQview's invalid-date detection probe. */
    /* The VM uses the host clock; reject changes with DOS's documented AL
     * result instead of terminating DOSX during environment detection. */
    set8(0, 0xff);
    break;
  case 0x2c: {
    DOS_DATE_TIME now = dos_now();
    r[CX] = ((U16)now.Hour << 8) | now.Minute;
    r[DX] = ((U16)now.Second << 8) | now.Hundredths;
    break;
  }
  case 0x1b:
  case 0x1c: { /* Allocation information and pointer to the media byte. */
    U8 drive = ah == 0x1b ? 0 : (U8)r[DX];
    U32 sectors, free_count, bytes, total;
    if ((drive != 0 && drive != 3) ||
        !NT_SUCCESS(FatQueryDiskSpace(&sectors, &free_count, &bytes, &total))) {
      set8(0, 0xff); break;
    }
    set8(0, (U8)sectors); r[CX] = (U16)bytes;
    r[DX] = (U16)(total > 0xffffu ? 0xffffu : total);
    seg[DS] = DOS_SYSTEM_DATA_SEGMENT; r[BX] = DOS_DPB_OFFSET + 0x16u;
    break;
  }
  case 0x36: {
    U8 drive = (U8)r[DX];
    U32 sectors, free_count, bytes, total;
    if ((drive != 0 && drive != 3) ||
        !NT_SUCCESS(FatQueryDiskSpace(&sectors, &free_count, &bytes,
                                      &total))) {
      r[AX] = 0xffff;
      break;
    }
    r[AX] = (U16)sectors;
    r[BX] = (U16)(free_count > 0xffffu ? 0xffffu : free_count);
    r[CX] = (U16)bytes;
    r[DX] = (U16)(total > 0xffffu ? 0xffffu : total);
    break;
  }
  case 0x30:
    if (current()->MouseWindowsVersionProbe) {
      current()->MouseWindowsVersionProbe = FALSE;
      r[AX] = 0x000au;
    } else {
      r[AX] = current()->DosVersion;
    }
    r[BX] = 0;
    r[CX] = 0;
    break;
  case 0x33:
    switch (get8(0)) {
    case 0x00: /* get Ctrl-Break state */
      set8(2, current()->CtrlBreak);
      break;
    case 0x01: /* set Ctrl-Break state */
      current()->CtrlBreak = !!get8(2);
      break;
    case 0x06: /* true DOS version, unaffected by SETVER */
      r[BX] = 0x0005; /* BL=5.00 major, BH=0 minor */
      r[CX] = 0;
      r[DX] = 0;
      break;
    default:
      set8(0, 0xff);
      break;
    }
    break;
  case 0x38: {
    U8 requested = get8(0);
    if (off == 0xffffu) {
      U16 country = requested == 0xff ? r[BX] : requested;
      if (country != 1) {
        r[AX] = 2;
        flags |= CF;
      } else
        current()->CountryCode = country;
      break;
    }
    U16 country = requested == 0 ? current()->CountryCode
                                 : (requested == 0xff ? r[BX] : requested);
    if (country != 1) {
      r[AX] = 2;
      flags |= CF;
      break;
    }
    /* DOS 3.x country table: US date/currency/time formats. */
    U8 info[34] = {0};
    info[2] = '$';
    info[7] = ',';
    info[9] = '.';
    info[11] = '/';
    info[13] = ':';
    info[16] = ',';
    for (U16 i = 0; i < sizeof(info); i++)
      wb(seg[DS], (U16)(off + i), info[i]);
    r[BX] = country;
    break;
  }
  case 0x44: {
    /* IOCTL 4400h: identify a handle as a character device or a C: file. */
    U16 handle = r[BX];
    if (get8(0) == 1) {
      U16 error = dos_exec_shared(FALSE);
      KeLog("[exec] load-only error=%u\n",error);
      r[AX] = error;
      if (error) flags |= CF;
    } else if (get8(0) != 0) {
      r[AX] = 1;
      flags |= CF;
    } else if (handle < 3) {
      r[DX] = 0x80u | (handle == 0 ? 1u : 2u);
    } else if (handle >= 5 && handle < 20 && files[handle]) {
      r[DX] = 2; /* file on drive C: (zero-based drive number 2) */
    } else {
      r[AX] = 6;
      flags |= CF;
    }
    break;
  }
  case 0x39:
  case 0x3a:
  case 0x3b:
  case 0x41:
  case 0x43: {
    char path[260];
    if (!guest_path(seg[DS], off, path)) {
      r[AX] = 3;
      flags |= CF;
      break;
    }
    NTSTATUS st = STATUS_NOT_SUPPORTED;
    if (ah == 0x39)
      st = FatCreateDirectory(path);
    else if (ah == 0x3a)
      st = FatRemoveDirectory(path);
    else if (ah == 0x41)
      st = FatDeleteFile(path);
    else if (ah == 0x3b) {
      FAT_FIND_DATA info;
      st = FatQueryPath(path, &info);
      if (NT_SUCCESS(st) && !(info.Attributes & 0x10))
        st = STATUS_OBJECT_PATH_NOT_FOUND;
      if (NT_SUCCESS(st)) {
        char *cwd = KeGetCurrentThread()->Process->CurrentDirectory;
        unsigned j = 0;
        while (path[j] && j < 259) {
          cwd[j] = path[j];
          j++;
        }
        cwd[j] = 0;
        dos_sync_system_data();
      }
    } else if (get8(0) == 0) {
      FAT_FIND_DATA info;
      st = FatQueryPath(path, &info);
      if (NT_SUCCESS(st))
        r[CX] = (U16)info.Attributes;
    }
    if (!NT_SUCCESS(st)) {
      r[AX] = ah == 0x41 ? 2 : 3;
      flags |= CF;
    }
    break;
  }
  case 0x47: {
    U8 drive = (U8)r[DX];
    if (drive != 0 && drive != 3) {
      r[AX] = 15;
      flags |= CF;
      break;
    }
    const char *cwd = KeGetCurrentThread()->Process->CurrentDirectory;
    unsigned length = 0;
    while (cwd[3 + length] && length < 63) length++;
    if (cwd[3 + length]) {
      r[AX] = 3;
      flags |= CF;
      break;
    }
    for (unsigned j = 0; j < length; j++)
      wb(seg[DS], (U16)(r[SI] + j), (U8)cwd[3 + j]);
    wb(seg[DS], (U16)(r[SI] + length), 0);
    break;
  }
  case 0x3c:
  case 0x3d:
  case 0x5b: {
    BOOLEAN create_new = ah == 0x5b;
    U8 mode = get8(0) & 3;
    if (create_new && get8(0) != 0) {
      r[AX] = 1; /* invalid subfunction */
      flags |= CF;
      break;
    }
    if (ah == 0x3d && mode == 3) {
      r[AX] = 12; /* invalid access code */
      flags |= CF;
      break;
    }
    char path[260];
    if (!guest_path(seg[DS], off, path)) {
      r[AX] = 3;
      flags |= CF;
      break;
    }
    unsigned slot = 5;
    while (slot < 20 && files[slot])
      slot++;
    if (slot == 20) {
      r[AX] = 4;
      flags |= CF;
      break;
    }
    DOS_FILE *f = ExAllocatePoolWithTag(NonPagedPool, sizeof(*f), 'fsoD');
    if (!f) {
      r[AX] = 8;
      flags |= CF;
      break;
    }
    memset(f, 0, sizeof(*f));
    unsigned i = 0;
    while (path[i] && i < 259) {
      f->Path[i] = path[i];
      i++;
    }
    f->Path[i] = 0;
    void *data = NULL;
    SIZE_T size = 0;
    NTSTATUS st = STATUS_SUCCESS;
    if (create_new) {
      FAT_FIND_DATA existing;
      st = FatQueryPath(f->Path, &existing);
      if (NT_SUCCESS(st)) {
        ExFreePool(f);
        r[AX] = 0x50; /* exclusive create: the file already exists */
        flags |= CF;
        break;
      }
      st = FatStoreFile(f->Path, NULL, 0, TRUE);
    } else if (ah == 0x3c) {
      st = FatStoreFile(f->Path, NULL, 0, TRUE);
    }
    if (NT_SUCCESS(st))
      st = FatLoadFile(f->Path, &data, &size, NULL);
    if (!NT_SUCCESS(st)) {
      ExFreePool(f);
      r[AX] = create_new ? (st == STATUS_NO_MEMORY ? 8 : 5) :
             ah == 0x3c ? 5 : 2;
      flags |= CF;
      break;
    }
    f->Data = data;
    f->Size = size;
    f->Position = 0;
    f->Writable = ah == 0x3c || create_new || mode != 0;
    f->Readable = ah == 0x3c || create_new || mode != 1;
    f->References = 1;
    files[slot] = f;
    DosUpdateSftEntry(current(), (U16)slot);
    r[AX] = (U16)slot;
    break;
  }
  case 0x3e:
    if (r[BX] < 5 || r[BX] >= 20 || !files[r[BX]]) {
      r[AX] = 6;
      flags |= CF;
    } else {
      dos_file_release(files[r[BX]]);
      files[r[BX]] = NULL;
      DosUpdateSftEntry(current(), r[BX]);
    }
    break;
  case 0x3f:
  case 0x40: {
    U16 h = r[BX], count = r[CX];
    if (h >= 20 || (h >= 5 && !files[h]) || h == 3 || h == 4) {
      r[AX] = 6;
      flags |= CF;
      break;
    }
    /* The DOS buffer must stay within the 1 MiB arena. */
    U32 a = addr(seg[DS], off);
    if ((U32)off + count > 0x10000u || a + count > MEM_SIZE) {
      r[AX] = 5;
      flags |= CF;
      break;
    }
    BOOLEAN ok = transfer(h, mem + a, count, &done, ah == 0x40);
    if (!ok) {
      r[AX] = 5;
      flags |= CF;
    } else
      r[AX] = (U16)done;
    break;
  }
  case 0x42: {
    U16 h = r[BX];
    if (h < 5 || h >= 20 || !files[h] || get8(0) > 2) {
      r[AX] = 6;
      flags |= CF;
      break;
    }
    INT32 distance = (INT32)((U32)r[CX] << 16 | r[DX]);
    INT64 pos = (get8(0) == 0   ? 0
                 : get8(0) == 1 ? (INT64)files[h]->Position
                                : (INT64)files[h]->Size) +
                distance;
    if (pos < 0 || pos > 0xffffffffLL) {
      r[AX] = 1;
      flags |= CF;
    } else {
      files[h]->Position = (SIZE_T)pos;
      DosUpdateSftEntry(current(), h);
      r[AX] = (U16)pos;
      r[DX] = (U16)((UINT64)pos >> 16);
    }
    break;
  }
  case 0x57: {
    U16 h = r[BX];
    if (h < 5 || h >= 20 || !files[h]) {
      r[AX] = 6;
      flags |= CF;
      break;
    }
    if (get8(0) == 0) {
      FAT_FIND_DATA info;
      if (NT_SUCCESS(FatQueryPath(files[h]->Path, &info))) {
        r[CX] = info.WriteTime;
        r[DX] = info.WriteDate;
      } else {
        r[AX] = 5;
        flags |= CF;
      }
    } else if (get8(0) == 1) {
      if (!files[h]->Writable ||
          !NT_SUCCESS(FatSetFileDateTime(files[h]->Path, r[DX], r[CX]))) {
        r[AX] = 5;
        flags |= CF;
      }
    } else {
      r[AX] = 1;
      flags |= CF;
    }
    break;
  }
  case 0x48: {
    U16 wanted = r[BX], block = dos_alloc(wanted);
    KeLog("[alloc] owner=%04x size=%04x -> %04x\n",current()->CurrentPsp,wanted,block);
    if (!block) {
      U16 best = 0;
      U16 start = PSP + current()->InitialParagraphs;
      while (start < 0xa000) {
        U16 size = free_paragraphs_from(start);
        if (size > best)
          best = size;
        if (start + size >= 0xa000)
          break;
        start += size;
        for (unsigned i = 0; i < 128; i++)
          if (current()->Blocks[i].Used &&
              current()->Blocks[i].Segment == start) {
            start += current()->Blocks[i].Paragraphs;
            break;
          }
      }
      r[BX] = best;
      r[AX] = 8;
      flags |= CF;
    } else
      r[AX] = block;
    break;
  }
  case 0x49: {
    U16 target = seg[ES];
    BOOLEAN found = FALSE;
    for (unsigned i = 0; i < DOS_MAX_BLOCKS; i++)
      if (current()->Blocks[i].Used && current()->Blocks[i].Segment == target) {
        current()->Blocks[i].Used = FALSE;
        DosUpdateMemoryControlBlocks(current());
        found = TRUE;
        break;
      }
    if (!found) {
      r[AX] = 7;
      flags |= CF;
    }
    break;
  }
  case 0x4a: {
    U16 target = seg[ES], wanted = r[BX];
    KeLog("[resize] owner=%04x block=%04x size=%04x\n",current()->CurrentPsp,target,wanted);
    if (target == PSP) {
      if (wanted >= 0x10 &&
          wanted <=
              current()->InitialParagraphs +
                  free_paragraphs_from(PSP + current()->InitialParagraphs)) {
        current()->InitialParagraphs = wanted;
        ww(PSP, 2, PSP + wanted);
      } else {
        r[AX] = 8;
        flags |= CF;
      }
      break;
    }
    BOOLEAN found = FALSE;
    for (unsigned i = 0; i < DOS_MAX_BLOCKS; i++)
      if (current()->Blocks[i].Used && current()->Blocks[i].Segment == target) {
        found = TRUE;
        U16 available =
            current()->Blocks[i].Paragraphs +
            free_paragraphs_from(target + current()->Blocks[i].Paragraphs);
        if (wanted && wanted <= available) {
          current()->Blocks[i].Paragraphs = wanted;
          DosUpdateMemoryControlBlocks(current());
        } else {
          r[BX] = available;
          r[AX] = 8;
          flags |= CF;
        }
        break;
      }
    if (!found) {
      r[AX] = 7;
      flags |= CF;
    }
    break;
  }
  case 0x4c: {
    exit_code = get8(0);
    KeLog("[dos] INT 21h/4c exit=%02x at %04x:%04x SS:SP=%04x:%04x\n",
          (U8)exit_code, seg[CS], ip, seg[SS], r[SP]);
    if (!dos_terminate_nested_psp((U8)exit_code)) running = 0;
    break;
  }
  case 0x4b:
    KeLog("[exec] al=%u from=%04x:%04x initial=%04x\n", get8(0),seg[CS],ip,current()->InitialParagraphs);
    if (get8(0) == 1) {
      U16 error = dos_exec_shared(FALSE);
      KeLog("[exec] load-only error=%u\n",error);
      r[AX] = error;
      if (error) flags |= CF;
    } else if (get8(0) != 0) {
      r[AX] = 1;
      flags |= CF;
    } else {
      U16 error = current()->DosVersion == 0x1e03u ? dos_exec_shared(TRUE) : dos_exec();
      if (error) {
        r[AX] = error;
        flags |= CF;
      } else
        r[AX] = 0;
    }
    break;
  case 0x4d:
    if (current()->ChildStatusAvailable) {
      r[AX] = ((U16)current()->LastChildTerminationType << 8) |
              current()->LastChildExitCode;
      current()->ChildStatusAvailable = FALSE;
    } else
      r[AX] = 0;
    break;
  case 0x56: {
    char old_path[260], new_path[260];
    if (!guest_path(seg[DS], off, old_path) ||
        !guest_path(seg[ES], r[DI], new_path) ||
        !NT_SUCCESS(FatRenamePath(old_path, new_path))) {
      r[AX] = 2;
      flags |= CF;
    }
    break;
  }
  case 0x52: /* Get DOS List of Lists (AL is not a subfunction). */
    seg[ES] = DOS_SYSTEM_DATA_SEGMENT;
    r[BX] = DOS_LISTS_OFFSET;
    break;
  case 0x5d:
    if (get8(0) == 0x06u) {
      /* Get DOS 3.x Swappable Data Area. */
      seg[DS] = DOS_SYSTEM_DATA_SEGMENT;
      r[SI] = DOS_SDA_OFFSET;
      r[CX] = DOS_SDA_SIZE;
      r[DX] = 0x0018u; /* first 18h bytes are always swapped */
    } else {
      r[AX] = 1;
      flags |= CF;
    }
    break;
  case 0x50: /* Set current PSP (Windows switches DOS process context). */
    current()->CurrentPsp = r[BX];
    dos_sync_system_data();
    break;
  case 0x51:
  case 0x62:
    r[BX] = current()->CurrentPsp;
    break;
  case 0x26:
  case 0x55: { /* Create PSP; AH=55h also switches the current PSP. */
    U16 parent = current()->CurrentPsp, child = r[DX];
    U8 psp[256];
    for (unsigned i = 0; i < sizeof(psp); i++) psp[i] = rb(parent, (U16)i);
    for (unsigned i = 0; i < sizeof(psp); i++) wb(child, (U16)i, psp[i]);
    ww(child, 2, r[SI]);
    ww(child, 0x16u, parent);
    ww(child, 0x2eu, 0);
    ww(child, 0x30u, 0);
    ww(child, 0x32u, 20);
    ww(child, 0x34u, 0x18u);
    ww(child, 0x36u, child);
    for (unsigned vector = 0x22u; vector <= 0x24u; vector++) {
      U16 field = (U16)(0x0au + (vector - 0x22u) * 4u);
      ww(child, field, rw(0, (U16)(vector * 4u)));
      ww(child, (U16)(field + 2u), rw(0, (U16)(vector * 4u + 2u)));
    }
    if (ah == 0x55u) {
      ww(parent, 0x2eu, r[SP]);
      ww(parent, 0x30u, seg[SS]);
      for (U32 i = 0; i < DOS_MAX_BLOCKS; i++)
        if (current()->Blocks[i].Used &&
            current()->Blocks[i].Segment == child)
          current()->Blocks[i].OwnerPsp = child;
      current()->CurrentPsp = child;
      dos_sync_system_data();
    }
    break;
  }
  case 0x58: {
    switch (get8(0)) {
    case 0x00: /* Get allocation strategy. */
      r[BX] = current()->DosAllocationStrategy;
      break;
    case 0x01: /* Set allocation strategy: first, best or last fit. */
      if (r[BX] <= 2u)
        current()->DosAllocationStrategy = r[BX];
      else {
        r[AX] = 1;
        flags |= CF;
      }
      break;
    case 0x02: /* Get UMB link state; there is no UMB arena. */
      r[BX] = 0;
      set8(0, 0);
      break;
    case 0x03: /* UMB linking is unavailable without an upper-memory arena. */
      if (r[BX] != 0) {
        r[AX] = 1;
        flags |= CF;
      }
      break;
    default:
      r[AX] = 1;
      flags |= CF;
      break;
    }
    break;
  }
  default:
    KeLog("[dos] unsupported INT 21h AX=0x%04x BX=0x%04x at %04x:%04x\n",
          r[AX], r[BX], seg[CS], ip);
    fail("DOSVM: unsupported INT 21h function\r\n");
    break;
  }
}

void dos_int(U8 n) {
  DOS_VM *vm = current();
  if (n != 0x21 || !vm || !vm->Memory) {
    dos_int_dispatch(n);
    return;
  }

  U32 in_dos = ((U32)DOS_SYSTEM_DATA_SEGMENT << 4) + DOS_SDA_OFFSET + 1u;
  if (vm->InDosCount != 0xffu) vm->InDosCount++;
  vm->Memory[in_dos] = vm->InDosCount;
  dos_int_dispatch(n);
  if (vm->InDosCount) vm->InDosCount--;
  vm->Memory[in_dos] = vm->InDosCount;
}

