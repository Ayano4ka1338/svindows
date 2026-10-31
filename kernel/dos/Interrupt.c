#include "Internal.h"

typedef struct _DOS_DATE_TIME {
  U16 Year;
  U8 Month, Day, Weekday, Hour, Minute, Second, Hundredths;
} DOS_DATE_TIME;

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
    if (!NT_SUCCESS(status))
      return 2;
  }
  if (!DosIsImage(path, image, size)) {
    ExFreePool(image);
    return 11;
  }
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
  DosDestroyVm(child);
  parent->LastChildExitCode = child_status;
  parent->LastChildTerminationType = termination_type;
  parent->ChildStatusAvailable = TRUE;
  return 0;
}

void dos_int(U8 n) {
  U8 ah = (U8)(r[AX] >> 8);
  U32 done = 0;
  U16 off = r[DX];
  /* A non-null IVT entry is a guest-installed real-mode handler. */
  U16 vector_off = rw(0, (U16)(n * 4u));
  U16 vector_seg = rw(0, (U16)(n * 4u + 2u));
  if (vector_off || vector_seg) {
    r[SP] -= 2; ww(seg[SS], r[SP], flags | 2u);
    r[SP] -= 2; ww(seg[SS], r[SP], seg[CS]);
    r[SP] -= 2; ww(seg[SS], r[SP], ip);
    flags &= ~(0x0100u | 0x0200u);
    seg[CS] = vector_seg;
    ip = vector_off;
    return;
  }
  if (n == 0x2f) {
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
      r[AX] = 0x1680; /* unsupported multiplex function */
    }
    return;
  }
  if (n == 0x20) {
    running = 0;
    exit_code = 0;
    return;
  }
  if (n == 0x29) {
    U8 ch = get8(0);
    output(1, &ch, 1);
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
  if (n == 0x16 && ah == 0) {
    U8 ch = 0;
    transfer(0, &ch, 1, &done, FALSE);
    r[AX] = ch;
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
  if (n != 0x21) {
    KeLog("[dos] unsupported INT %02x AX=%04x BX=%04x CX=%04x DX=%04x at %04x:%04x\n",
          n, r[AX], r[BX], r[CX], r[DX], seg[CS], ip);
    fail("DOSVM: unsupported interrupt\r\n");
    return;
  }
  flags &= ~CF;
  switch (ah) {
  case 0x00:
    running = 0;
    exit_code = 0;
    break;
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
    break;
  case 0x2f:
    seg[ES] = current()->DtaSegment;
    r[BX] = current()->DtaOffset;
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
  case 0x2c: {
    DOS_DATE_TIME now = dos_now();
    r[CX] = ((U16)now.Hour << 8) | now.Minute;
    r[DX] = ((U16)now.Second << 8) | now.Hundredths;
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
    r[AX] = 0x0005;
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
    case 0x06: /* true DOS version */
      r[BX] = 0x0500; /* DOS 5.00 compatibility level */
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
    if (get8(0) != 0) {
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
  case 0x3d: {
    U8 mode = get8(0) & 3;
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
    NTSTATUS st =
        ah == 0x3c ? FatStoreFile(f->Path, NULL, 0, TRUE) : STATUS_SUCCESS;
    if (NT_SUCCESS(st))
      st = FatLoadFile(f->Path, &data, &size, NULL);
    if (!NT_SUCCESS(st)) {
      ExFreePool(f);
      r[AX] = ah == 0x3c ? 5 : 2;
      flags |= CF;
      break;
    }
    f->Data = data;
    f->Size = size;
    f->Position = 0;
    f->Writable = ah == 0x3c || (get8(0) & 3) != 0;
    f->Readable = ah == 0x3c || mode != 1;
    f->References = 1;
    files[slot] = f;
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
        if (wanted && wanted <= available)
          current()->Blocks[i].Paragraphs = wanted;
        else {
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
  case 0x4c:
    exit_code = get8(0);
    running = 0;
    break;
  case 0x4b:
    if (get8(0) != 0) {
      r[AX] = 1;
      flags |= CF;
    } else {
      U16 error = dos_exec();
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
  case 0x62:
    r[BX] = PSP;
    break;
  default:
    KeLog("[dos] unsupported INT 21h AX=0x%04x BX=0x%04x at %04x:%04x\n",
          r[AX], r[BX], seg[CS], ip);
    fail("DOSVM: unsupported INT 21h function\r\n");
    break;
  }
}

