#include "Internal.h"

void dos_file_release(DOS_FILE *file) {
  if (file && !--file->References) {
    ExFreePool(file->Data);
    ExFreePool(file);
  }
}

static BOOLEAN path_from_dos(const char *input, char output[260]) {
  if (!input || !*input)
    return FALSE;
  /* DOS C:foo is relative to C:'s current directory. Only C: is mounted. */
  if (input[1] == ':') {
    if (input[0] != 'C' && input[0] != 'c')
      return FALSE;
    if (!input[2]) {
      input = ".";
    } else if (input[2] != '\\' && input[2] != '/') {
      input += 2;
    }
  }
  PKPROCESS process = KeGetCurrentThread()->Process;
  return IoResolveDosPathKernel(input,
                                process ? process->CurrentDirectory : NULL,
                                output);
}
BOOLEAN guest_path(U16 s, U16 o, char out[260]) {
  char raw[260];
  unsigned i = 0;
  while (i + 1 < sizeof(raw)) {
    U8 c = rb(s, (U16)(o + i));
    if (!c)
      break;
    raw[i++] = (char)c;
  }
  if (i + 1 == sizeof(raw))
    return FALSE;
  raw[i] = 0;
  return path_from_dos(raw, out);
}

HANDLE standard_handle(U16 slot) {
  PKPROCESS p = KeGetCurrentThread()->Process;
  return slot == 0   ? p->StandardInput
         : slot == 1 ? p->StandardOutput
                     : p->StandardError;
}
BOOLEAN transfer(U16 slot, void *buffer, U32 length, U32 *done,
                        BOOLEAN write) {
  *done = 0;
  if (slot < 3) {
    SIZE_T count = 0;
    NTSTATUS status =
        write
            ? IoWriteHandleKernel(standard_handle(slot), buffer, length, &count)
            : IoReadHandleKernel(standard_handle(slot), buffer, length, &count);
    *done = (U32)count;
    return NT_SUCCESS(status);
  }
  if (slot < 5 || slot >= 20 || !files[slot])
    return FALSE;
  DOS_FILE *f = files[slot];
  if (write) {
    if (!f->Writable || f->Position + length > 0x80000u)
      return FALSE;
    if (!length) {
      if (f->Position == f->Size)
        return TRUE;
      U8 *next = NULL;
      if (f->Position > f->Size) {
        next = ExAllocatePoolWithTag(NonPagedPool, f->Position, 'fsoD');
        if (!next)
          return FALSE;
        memset(next, 0, f->Position);
        if (f->Size)
          memcpy(next, f->Data, f->Size);
      }
      const void *data = next ? next : f->Data;
      if (!NT_SUCCESS(FatStoreFile(f->Path, data, f->Position, FALSE))) {
        if (next)
          ExFreePool(next);
        return FALSE;
      }
      if (next) {
        ExFreePool(f->Data);
        f->Data = next;
      }
      f->Size = f->Position;
      DosUpdateSftEntry(current(), slot);
      return TRUE;
    }
    SIZE_T end = f->Position + length;
    SIZE_T size = end > f->Size ? end : f->Size;
    U8 *next = ExAllocatePoolWithTag(NonPagedPool, size, 'fsoD');
    if (!next)
      return FALSE;
    memset(next, 0, size);
    if (f->Size)
      memcpy(next, f->Data, f->Size);
    memcpy(next + f->Position, buffer, length);
    if (!NT_SUCCESS(FatStoreFile(f->Path, next, size, FALSE))) {
      ExFreePool(next);
      return FALSE;
    }
    ExFreePool(f->Data);
    f->Data = next;
    f->Size = size;
    f->Position = end;
    *done = length;
  } else {
    if (!f->Readable)
      return FALSE;
    SIZE_T available = f->Position < f->Size ? f->Size - f->Position : 0;
    *done = (U32)(available < length ? available : length);
    if (*done)
      memcpy(buffer, f->Data + f->Position, *done);
    f->Position += *done;
  }
  DosUpdateSftEntry(current(), slot);
  return TRUE;
}
void output(U16 slot, const void *p, U32 n) {
  U32 done;
  transfer(slot, (void *)p, n, &done, TRUE);
}
void say(const char *s) {
  U32 n = 0;
  while (s[n])
    n++;
  output(2, s, n);
}

static U8 dos_upper(U8 ch) {
  return ch >= 'a' && ch <= 'z' ? ch - ('a' - 'A') : ch;
}
void dos_name_mask(const char *source, char mask[11]) {
  memset(mask, ' ', 11);
  unsigned i = 0;
  BOOLEAN star = FALSE;
  while (*source && *source != '.') {
    if (*source == '*') {
      while (i < 8)
        mask[i++] = '?';
      star = TRUE;
    } else if (i < 8)
      mask[i++] = (char)dos_upper((U8)*source);
    source++;
  }
  if (*source == '.') {
    source++;
    i = 8;
    while (*source) {
      if (*source == '*') {
        while (i < 11)
          mask[i++] = '?';
      } else if (i < 11)
        mask[i++] = (char)dos_upper((U8)*source);
      source++;
    }
  } else if (star)
    for (i = 8; i < 11; i++)
      mask[i] = '?';
}
static BOOLEAN dos_match(const char mask[11], const char *name) {
  char packed[11];
  memset(packed, ' ', sizeof(packed));
  unsigned i = 0;
  while (*name && *name != '.') {
    if (i < 8)
      packed[i++] = (char)dos_upper((U8)*name);
    name++;
  }
  if (*name == '.') {
    name++;
    i = 8;
    while (*name) {
      if (i < 11)
        packed[i++] = (char)dos_upper((U8)*name);
      name++;
    }
  }
  for (i = 0; i < 11; i++)
    if (mask[i] != '?' && mask[i] != packed[i])
      return FALSE;
  return TRUE;
}
BOOLEAN dos_find_next(void) {
  DOS_VM *vm = current();
  FAT_FIND_DATA entry;
  if (!vm->SearchActive || vm->DtaSegment != vm->SearchDtaSegment ||
      vm->DtaOffset != vm->SearchDtaOffset)
    return FALSE;
  while (NT_SUCCESS(FatEnumerateDirectory(vm->SearchDirectory,
                                          vm->SearchIndex++, &entry))) {
    U32 attr = entry.Attributes;
    if ((attr & 0x10u) && !(vm->SearchAttributes & 0x10u))
      continue;
    if ((attr & 0x02u) && !(vm->SearchAttributes & 0x02u))
      continue;
    if ((attr & 0x04u) && !(vm->SearchAttributes & 0x04u))
      continue;
    if (!dos_match(vm->SearchPattern, entry.ShortName))
      continue;
    U16 s = vm->DtaSegment, o = vm->DtaOffset;
    wb(s, (U16)(o + 0x15), (U8)attr);
    ww(s, (U16)(o + 0x16), entry.WriteTime);
    ww(s, (U16)(o + 0x18), entry.WriteDate);
    ww(s, (U16)(o + 0x1a), (U16)entry.Size);
    ww(s, (U16)(o + 0x1c), (U16)(entry.Size >> 16));
    unsigned i = 0;
    while (i < 12 && entry.ShortName[i]) {
      wb(s, (U16)(o + 0x1e + i), (U8)entry.ShortName[i]);
      i++;
    }
    wb(s, (U16)(o + 0x1e + i), 0);
    return TRUE;
  }
  return FALSE;
}

/* FCB searches return a drive byte followed by a raw 32-byte directory entry,
 * with a seven-byte prefix for extended FCBs (DOS 3.3 reference, AH=11h).
 * Store the enumeration cursor in the unopened FCB's DOS-reserved area so
 * callers can switch DTAs or interleave separate searches. */
BOOLEAN dos_find_fcb(U16 source_segment, U16 source_offset, BOOLEAN first) {
  DOS_VM *vm = current();
  BOOLEAN extended = rb(source_segment, source_offset) == 0xffu;
  U8 attributes = extended ? rb(source_segment, (U16)(source_offset + 6u)) : 0;
  U16 fcb = (U16)(source_offset + (extended ? 7u : 0u));
  U8 drive = rb(source_segment, fcb);
  if (drive && drive != 3u) return FALSE;
  char pattern[11];
  for (unsigned i = 0; i < 11; i++)
    pattern[i] = (char)dos_upper(rb(source_segment, (U16)(fcb + 1u + i)));
  U32 index = first ? 0 : (U32)rw(source_segment, (U16)(fcb + 0x10u)) |
      ((U32)rw(source_segment, (U16)(fcb + 0x12u)) << 16);
  const char *directory = KeGetCurrentThread()->Process->CurrentDirectory;
  FAT_FIND_DATA entry;
  while (NT_SUCCESS(FatEnumerateDirectory(directory, index++, &entry))) {
    U8 attr = (U8)entry.Attributes;
    if (attributes & 8u) {
      if (!(attr & 8u)) continue;
    } else {
      if (attr & 8u) continue;
      if ((attr & 0x16u) & ~attributes) continue;
    }
    if (!dos_match(pattern, entry.ShortName)) continue;
    ww(source_segment, (U16)(fcb + 0x10u), (U16)index);
    ww(source_segment, (U16)(fcb + 0x12u), (U16)(index >> 16));
    U16 s = vm->DtaSegment, o = vm->DtaOffset;
    for (unsigned i = 0; i < (extended ? 40u : 33u); i++)
      wb(s, (U16)(o + i), 0);
    if (extended) {
      wb(s, o, 0xffu);
      wb(s, (U16)(o + 6u), attributes);
      o = (U16)(o + 7u);
    }
    wb(s, o, 3u);
    char name[11];
    dos_name_mask(entry.ShortName, name);
    for (unsigned i = 0; i < 11; i++) wb(s, (U16)(o + 1u + i), (U8)name[i]);
    wb(s, (U16)(o + 12u), attr);
    ww(s, (U16)(o + 23u), entry.WriteTime);
    ww(s, (U16)(o + 25u), entry.WriteDate);
    ww(s, (U16)(o + 29u), (U16)entry.Size);
    ww(s, (U16)(o + 31u), (U16)(entry.Size >> 16));
    return TRUE;
  }
  return FALSE;
}
