/* DOS image loading and VM lifetime. */
#include "Internal.h"

U16 free_paragraphs_from(U16 start) {
  U16 end = 0xa000;
  for (unsigned i = 0; i < DOS_MAX_BLOCKS; i++)
    if (current()->Blocks[i].Used && current()->Blocks[i].Segment >= start &&
        current()->Blocks[i].Segment < end)
      end = current()->Blocks[i].Segment;
  return end > start ? end - start : 0;
}
U16 dos_alloc(U16 paragraphs) {
  if (!paragraphs)
    return 0;
  unsigned slot = 0;
  while (slot < DOS_MAX_BLOCKS && current()->Blocks[slot].Used)
    slot++;
  if (slot == DOS_MAX_BLOCKS)
    return 0;
  U16 candidate = PSP + current()->InitialParagraphs;
  while (candidate < 0xa000) {
    U16 free = free_paragraphs_from(candidate);
    if (free >= paragraphs) {
      current()->Blocks[slot] = (DOS_BLOCK){candidate, paragraphs, TRUE};
      return candidate;
    }
    if (candidate + free >= 0xa000)
      break;
    candidate += free;
    for (unsigned i = 0; i < DOS_MAX_BLOCKS; i++)
      if (current()->Blocks[i].Used &&
          current()->Blocks[i].Segment == candidate) {
        candidate += current()->Blocks[i].Paragraphs;
        break;
      }
  }
  return 0;
}
static int load(const U8 *image, U32 size, const char *tail) {
  if (size < 1)
    return 0;
  const U8 *hdr = image;
  int mz = size >= 28 && hdr[0] == 'M' && hdr[1] == 'Z';
  /* A DOS MZ executable may have a large overlay after the load image.
   * The header's page count, rather than the host file size, bounds memory. */
  if (!mz && size > 0xff00u)
    return 0;
  U32 file_len = size, header = 0, image_len = size;
  U16 load_seg = PSP + 0x10;
  if (mz) {
    U32 pages = word(hdr + 4), last = word(hdr + 2);
    if (!pages || last > 512)
      return 0;
    file_len = pages * 512u - (last ? 512u - last : 0u);
    header = (U32)word(hdr + 8) * 16u;
    if (file_len > size || header < 28 || header > file_len ||
        word(hdr + 0x1a) != 0 || word(hdr + 0xc) < word(hdr + 0xa))
      return 0;
    image_len = file_len - header;
    if (image_len + (U32)word(hdr + 0xa) * 16u > 0x8f000u)
      return 0;
    U32 base_paras = 0x10u + (image_len + 15u) / 16u;
    /* Keep the conventional memory beyond minalloc available to INT 21h/48h.
     * Programs that need a larger PSP block can grow it with AH=4Ah. */
    U32 extra = word(hdr + 0x0a);
    U32 total = base_paras + extra;
    if (total > 0x9000u)
      total = 0x9000u;
    if (total < base_paras + word(hdr + 0xa))
      return 0;
    current()->InitialParagraphs = (U16)total;
    seg[CS] = load_seg + word(hdr + 0x16);
    ip = word(hdr + 0x14);
    seg[SS] = load_seg + word(hdr + 0xe);
    r[SP] = word(hdr + 0x10);
    seg[DS] = seg[ES] = PSP;
  } else {
    current()->InitialParagraphs = 0x9000;
    seg[CS] = seg[DS] = seg[ES] = seg[SS] = PSP;
    ip = 0x100;
    r[SP] = 0xfffe;
    ww(PSP, 0xfffe, 0);
  }
  U32 target = addr(mz ? load_seg : PSP, mz ? 0 : 0x100);
  if (target + image_len > 0xa0000u)
    return 0;
  memcpy(mem + target, image + header, image_len);
  if (mz) {
    U16 count = word(hdr + 6), roff = word(hdr + 0x18);
    if ((U32)roff + (U32)count * 4 > header)
      return 0;
    for (U16 i = 0; i < count; i++) {
      const U8 *pair = image + roff + (U32)i * 4;
      U16 s = load_seg + word(pair + 2), o = word(pair);
      U32 physical = (U32)s * 16u + o;
      if (physical < target ||
          physical + 1 >= (U32)(PSP + current()->InitialParagraphs) * 16u)
        return 0;
      ww(s, o, rw(s, o) + load_seg);
    }
  }
  wb(PSP, 0, 0xcd);
  wb(PSP, 1, 0x20);
  ww(PSP, 2, PSP + current()->InitialParagraphs);
  ww(PSP, 0x16, PSP);
  wb(PSP, 0x50, 0xcd);
  wb(PSP, 0x51, 0x21);
  wb(PSP, 0x52, 0xcb);
  for (unsigned i = 0; i < 20; i++)
    wb(PSP, (U16)(0x18 + i), i < 3 ? (U8)i : 0xff);
  ww(PSP, 0x2c, 0);
  wb(PSP, 0x80, 0);
  unsigned n = 0;
  while (tail && tail[n] && n < 126) {
    wb(PSP, (U16)(0x81 + n), (U8)tail[n]);
    n++;
  }
  wb(PSP, 0x80, (U8)n);
  wb(PSP, (U16)(0x81 + n), '\r');
  current()->DtaSegment = PSP;
  current()->DtaOffset = 0x80;
  return 1;
}

BOOLEAN DosIsImage(const char *filename, const UINT8 *image, SIZE_T size) {
  if (!image || size < 1)
    return FALSE;
  if (size >= 2 && image[0] == 'M' && image[1] == 'Z') {
    if (size >= 0x40) {
      U32 pe = (U32)image[0x3c] | ((U32)image[0x3d] << 8) |
               ((U32)image[0x3e] << 16) | ((U32)image[0x3f] << 24);
      if (pe <= size && size - pe >= 4 && image[pe] == 'P' &&
          image[pe + 1] == 'E' && image[pe + 2] == 0 && image[pe + 3] == 0)
        return FALSE;
      if (pe <= size && size - pe >= 2 &&
          ((image[pe] == 'N' && image[pe + 1] == 'E') ||
           (image[pe] == 'L' &&
            (image[pe + 1] == 'E' || image[pe + 1] == 'X'))))
        return FALSE;
    }
    return TRUE;
  }
  SIZE_T n = 0;
  while (filename[n])
    n++;
  if (n < 4)
    return FALSE;
  const char *ext = filename + n - 4;
  return ext[0] == '.' && (ext[1] == 'C' || ext[1] == 'c') &&
         (ext[2] == 'O' || ext[2] == 'o') && (ext[3] == 'M' || ext[3] == 'm');
}

NTSTATUS DosCreateVm(const UINT8 *image, SIZE_T size, const char *command,
                     PVOID *out) {
  *out = NULL;
  if (size > 0xffffffffu)
    return STATUS_INVALID_IMAGE_FORMAT;
  DOS_VM *vm = ExAllocatePoolWithTag(NonPagedPool, sizeof(*vm), 'msoD');
  if (!vm)
    return STATUS_NO_MEMORY;
  memset(vm, 0, sizeof(*vm));
  vm->CountryCode = 1; /* DOS default country: United States */
  vm->FpuControl = 0x037f; /* reset x87 control word */
  vm->VideoMode = 3; /* 80x25 color text mode */
  vm->PicMasterMask = 0xfcu; /* BIOS leaves PIT and keyboard IRQs enabled */
  vm->PicSlaveMask = 0xffu;
  vm->PicMasterVectorBase = 0x08u;
  vm->PicSlaveVectorBase = 0x70u;
  vm->PitControl = 0x36u; /* channel 0, low/high byte, square-wave mode */
  vm->PitReload = 0;
  vm->VgaMiscOutput = 0x63u;
  vm->VgaDacMask = 0xffu;
  vm->Memory = ExAllocatePoolWithTag(NonPagedPool, MEM_SIZE, 'asoD');
  if (!vm->Memory) {
    ExFreePool(vm);
    return STATUS_NO_MEMORY;
  }
  memset(vm->Memory, 0, MEM_SIZE);
  /* A zero-length DPMI state buffer makes the save/restore entry a RETF. */
  vm->Memory[0xf0000u + 0x0108u] = 0xcbu;
  for (unsigned vector = 0; vector < 256; vector++)
    vm->Memory[0xf0000u + 0x0200u + vector * 2u] = 0xcfu;
  for (unsigned vector = 0; vector < 32; vector++)
    vm->Memory[0xf0000u + 0x0400u + vector * 2u] = 0xcfu;
  /* Each BIOS/DOS entry needs its own stable address: extenders save IVT
   * pointers and call them later with DPMI 0301/0302. */
  static const U8 builtin_vectors[] = {0x10, 0x15, 0x16, 0x20, 0x21, 0x29, 0x2f};
  for (unsigned i = 0; i < sizeof(builtin_vectors); i++) {
    unsigned vector = builtin_vectors[i];
    U16 offset = DOS_BUILTIN_OFFSET(vector);
    vm->Memory[vector * 4u] = (U8)offset;
    vm->Memory[vector * 4u + 1u] = (U8)(offset >> 8);
    vm->Memory[vector * 4u + 2u] = (U8)DOS_BUILTIN_SEGMENT;
    vm->Memory[vector * 4u + 3u] = (U8)(DOS_BUILTIN_SEGMENT >> 8);
    vm->Memory[((U32)DOS_BUILTIN_SEGMENT << 4) + DOS_BUILTIN_OFFSET(vector)] = 0xcfu;
  }
  /* Native command line begins with the image name, possibly quoted. */
  const char *p = command;
  while (*p == ' ' || *p == '\t')
    p++;
  if (*p == '"') {
    p++;
    while (*p && *p != '"')
      p++;
    if (*p)
      p++;
  } else
    while (*p && *p != ' ' && *p != '\t')
      p++;
  PVOID previous = KeGetCurrentThread()->DosContext;
  KeGetCurrentThread()->DosContext = vm;
  int ok = load(image, (U32)size, p);
  if (ok) {
    static const U8 prefix[] = {'P','A','T','H','=','C',':','\\',0,0,1,0};
    U32 base = (U32)DOS_ENV_SEGMENT << 4;
    memcpy(vm->Memory + base, prefix, sizeof(prefix));
    unsigned i = 0;
    const char *name = command;
    if (*name == '"') name++;
    while (name[i] && name[i] != '"' && name[i] != ' ' &&
           name[i] != '\t' && i < 259) {
      vm->Memory[base + sizeof(prefix) + i] = (U8)name[i];
      i++;
    }
    vm->Memory[base + sizeof(prefix) + i] = 0;
    ww(PSP, 0x2c, DOS_ENV_SEGMENT);
  }
  KeGetCurrentThread()->DosContext = previous;
  if (!ok) {
    ExFreePool(vm->Memory);
    ExFreePool(vm);
    return STATUS_INVALID_IMAGE_FORMAT;
  }
  vm->Running = 1;
  vm->Flags = 0x202;
  *out = vm;
  return STATUS_SUCCESS;
}

void DosDestroyVm(PVOID context) {
  DOS_VM *vm = (DOS_VM *)context;
  if (!vm)
    return;
  for (unsigned i = 5; i < 20; i++)
    dos_file_release(vm->Files[i]);
  DosVideoStop(vm);
  ExFreePool(vm->Memory);
  ExFreePool(vm);
}

U8 run_vm(DOS_VM *vm) {
  if (vm && !vm->KeyboardMirrorActive) {
    KbdInputMirrorAcquire();
    vm->KeyboardMirrorActive = TRUE;
  }
  /* Interactive DOS programs run until they exit. An instruction count is
   * a scheduling boundary, not a lifetime limit for games. */
  while (running) {
    steps++;
    step();
    if (!(steps & 0x3fffu)) KeYield();
  }
  return (U8)exit_code;
}

void DosRun(PVOID context) {
  DOS_VM *vm = (DOS_VM *)context;
  KeGetCurrentThread()->ExitStatus = (NTSTATUS)run_vm(vm);
  DosDestroyVm(vm);
  KeGetCurrentThread()->DosContext = NULL;
}
