/* DOS image loading and VM lifetime. */
#include "Internal.h"
#include <svindows/Console.h>


static BOOLEAN dos_command_is_win100_bin(const char *command) {
  if (!command) return FALSE;
  const char *start = command;
  if (*start == '"') start++;
  const char *end = start;
  while (*end && *end != '"' && *end != ' ' && *end != '\t') end++;
  const char *base = start;
  for (const char *p = start; p < end; p++)
    if (*p == '\\' || *p == '/') base = p + 1;
  static const char expected[] = "WIN100.BIN";
  if ((SIZE_T)(end - base) != sizeof(expected) - 1) return FALSE;
  for (SIZE_T i = 0; i < sizeof(expected) - 1; i++) {
    char ch = base[i];
    if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 'a' + 'A');
    if (ch != expected[i]) return FALSE;
  }
  return TRUE;
}

static BOOLEAN dos_command_is_windows31(const char *command) {
  if (!command) return FALSE;
  const char *start = command;
  while (*start == ' ' || *start == '\t') start++;
  if (*start == '"') start++;
  const char *end = start;
  while (*end && *end != '"' && *end != ' ' && *end != '\t') end++;
  const char *base = start;
  BOOLEAN windows3 = FALSE;
  for (const char *p = start; p < end; p++) {
    if (*p == '\\' || *p == '/') base = p + 1;
    static const char directory[] = "WINDOWS3";
    unsigned i;
    for (i = 0; i < sizeof(directory) - 1u && p + i < end; i++) {
      char ch = p[i];
      if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 'a' + 'A');
      if (ch != directory[i]) break;
    }
    if (i == sizeof(directory) - 1u) windows3 = TRUE;
  }
  (void)base;
  return windows3;
}

U32 DosImageHash(const U8 *image, U32 size) {
  /* FNV-1a is used only as a cheap identity check for the DOS/4GW self-exec
   * bootstrap, not as a security primitive. */
  U32 hash = 2166136261u;
  for (U32 i = 0; i < size; i++) {
    hash ^= image[i];
    hash *= 16777619u;
  }
  return hash;
}

BOOLEAN DosImageIsDos4g(const U8 *image, U32 size) {
  static const U8 marker[] = {'D','O','S','/','4','G','W'};
  if (!image || size < sizeof(marker)) return FALSE;
  for (U32 i = 0; i <= size - sizeof(marker); i++)
    if (!memcmp(image + i, marker, sizeof(marker))) return TRUE;
  return FALSE;
}

U16 free_paragraphs_from(U16 start) {
  U16 end = 0xa000;
  for (unsigned i = 0; i < DOS_MAX_BLOCKS; i++)
    if (current()->Blocks[i].Used && current()->Blocks[i].Segment >= start &&
        current()->Blocks[i].Segment < end)
      end = current()->Blocks[i].Segment;
  return end > start ? end - start : 0;
}
U16 dos_largest_free(void) {
  U32 start = PSP + current()->InitialParagraphs + 1u, best = 0;
  while (start < 0x9fffu) {
    U32 next = 0x9fffu;
    DOS_BLOCK *occupied = 0;
    for (U32 i = 0; i < DOS_MAX_BLOCKS; i++) {
      DOS_BLOCK *b = &current()->Blocks[i];
      if (b->Used && b->Segment >= start && b->Segment < next) { next = b->Segment; occupied = b; }
    }
    if (next > start) {
      U32 gap = next - start - (occupied ? 1u : 0u);
      if (gap > best) best = gap;
    }
    if (!occupied) break;
    start = next + occupied->Paragraphs + 1u;
  }
  return (U16)best;
}

U16 dos_alloc(U16 paragraphs) {
  if (!paragraphs)
    return 0;
  DOS_VM *vm = current();
  unsigned slot = 0;
  while (slot < DOS_MAX_BLOCKS && current()->Blocks[slot].Used)
    slot++;
  if (slot == DOS_MAX_BLOCKS)
    return 0;
  /* Every allocated DOS block has an MCB paragraph immediately before the
   * returned data segment. Keep allocations beyond the initial PSP's MCB. */
  U32 start = PSP + current()->InitialParagraphs + 1u;
  U32 selected = 0, selected_gap = 0xffffffffu;
  U16 strategy = vm->DosAllocationStrategy;
  while (start < 0x9fffu) {
    U32 next = 0x9fffu;
    DOS_BLOCK *occupied = 0;
    for (U32 i = 0; i < DOS_MAX_BLOCKS; i++) {
      DOS_BLOCK *block = &current()->Blocks[i];
      if (block->Used && block->Segment >= start && block->Segment < next) {
        next = block->Segment;
        occupied = block;
      }
    }
    if (next == start && occupied) {
      start = next + occupied->Paragraphs + 1u;
      continue;
    }
    U32 gap = next == 0x9fffu ? next - start : next - start - 1u;
    if (gap >= paragraphs) {
      if (strategy == 0) {
        selected = start;
        break;
      }
      if (strategy == 1 && gap < selected_gap) {
        selected = start;
        selected_gap = gap;
      }
      if (strategy == 2)
        selected = start + gap - paragraphs;
    }
    if (!occupied) break;
    start = next + occupied->Paragraphs + 1u;
  }
  if (!selected) return 0;
  current()->Blocks[slot] = (DOS_BLOCK){(U16)selected, paragraphs, TRUE,
                                        current()->CurrentPsp};
  DosUpdateMemoryControlBlocks(current());
  return (U16)selected;
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
    /* EXEC grants up to maxalloc, bounded by conventional memory. With both
     * allocation fields zero, DOS grants the whole block and loads the image
     * at its upper end (LINK /HIGH). WIN100.BIN uses that layout to build its
     * own heap below the resident image. */
    BOOLEAN load_high = !word(hdr + 0x0a) && !word(hdr + 0x0c);
    U32 total = load_high ? 0x9000u : base_paras + word(hdr + 0x0c);
    if (total > 0x9000u)
      total = 0x9000u;
    if (total < base_paras + word(hdr + 0xa))
      return 0;
    current()->InitialParagraphs = (U16)total;
    if (load_high)
      load_seg = (U16)(PSP + total - (base_paras - 0x10u));
    seg[CS] = load_seg + word(hdr + 0x16);
    ip = word(hdr + 0x14);
    seg[SS] = load_seg + word(hdr + 0xe);
    r[SP] = word(hdr + 0x10);
    /* DOS assigns a usable top-of-block stack when an MZ image leaves SS:SP
     * at zero. Windows 3.1's WSWAP.EXE uses exactly that header convention. */
    if (seg[SS] == load_seg && r[SP] == 0) {
      seg[SS] = (U16)(PSP + current()->InitialParagraphs - 2u);
      r[SP] = 0xfffeu;
    }
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
  ww(PSP, 0x32, 20);
  ww(PSP, 0x34, 0x18);
  ww(PSP, 0x36, PSP);
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
      /* MZ/NE images (Windows 1.x/2.x) carry a real DOS-mode stub at
       * CS:IP.  DOS must execute that stub so it can enter Windows mode;
       * rejecting NE here makes WIN.COM report "Cannot find Windows startup
       * files" before the Windows kernel gets control.  Continue rejecting
       * PE and LE/LX images, which are not ordinary DOS MZ executables. */
      if (pe <= size && size - pe >= 2 && image[pe] == 'L' &&
          (image[pe + 1] == 'E' || image[pe + 1] == 'X'))
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
  vm->CurrentPsp = PSP;
  vm->ImageSize = (U32)size;
  vm->ImageHash = DosImageHash(image, (U32)size);
  /* Windows 1.x needs its SETVER 3.34 value; the Windows 3.1 DOS startup
   * path follows the DOS 3.30 compatibility version used by 16helloos.
   * Other DOS programs continue to see 5.00; AX=3306h reports true 5.00. */
  vm->DosVersion = dos_command_is_win100_bin(command) ? 0x2203u :
                   (dos_command_is_windows31(command) ||
                    (KeGetCurrentThread()->Process && dos_command_is_windows31(
                      KeGetCurrentThread()->Process->CurrentDirectory))) ? 0x1e03u : 0x0005u;
  vm->Dos4gImage = DosImageIsDos4g(image, (U32)size);
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
  vm->Memory[0x41au] = vm->Memory[0x41cu] = 0x1eu;
  vm->Memory[0x480u] = 0x1eu;
  vm->Memory[0x482u] = 0x3eu;
  vm->Memory[0x449u] = 3;
  vm->Memory[0x44au] = 80;
  /* A zero-length DPMI state buffer makes the save/restore entry a RETF. */
  vm->Memory[0xf0000u + 0x0108u] = 0xcbu;
  /* XMS clients far-call this bridge; INT 2Fh dispatches the function and
   * RETF returns to the caller with its register ABI intact. */
  /* XMS specifies a hookable five-byte entry. WSWAP patches this prologue
   * to virtualize extended memory while the parent is swapped out. */
  static const U8 xms_entry[] = {0xeb, 0x03, 0x90, 0x90, 0x90, 0xcd, 0x2f, 0xcb};
  memcpy(vm->Memory + 0xf0600u, xms_entry, sizeof(xms_entry));
  for (unsigned vector = 0; vector < 256; vector++)
    vm->Memory[0xf0000u + 0x0200u + vector * 2u] = 0xcfu;
  for (unsigned vector = 0; vector < 32; vector++)
    vm->Memory[0xf0000u + 0x0400u + vector * 2u] = 0xcfu;
  /* Unclaimed hardware IRQs have chainable BIOS IRET entries. */
  for (unsigned vector = 0x0a; vector <= 0x0f; vector++) {
    U16 offset = 0x0200u + vector * 2u;
    vm->Memory[vector * 4u] = (U8)offset;
    vm->Memory[vector * 4u + 1u] = (U8)(offset >> 8);
    vm->Memory[vector * 4u + 3u] = 0xf0u;
  }
  /* BIOS user timer hook is an IRET even before a TSR installs a handler.
   * WSWAP saves and chains this vector without checking for a null pointer. */
  vm->Memory[0x1cu * 4u] = 0x38u;
  vm->Memory[0x1cu * 4u + 1u] = 0x02u;
  vm->Memory[0x1cu * 4u + 3u] = 0xf0u;
  /* Default IRQ0 advances the BIOS tick counter and chains INT 1Ch. Keep
   * this as guest code so saved vectors and guest hooks use normal IRET
   * frames, including when Windows chains the original timer handler. */
  static const U8 bios_timer[] = {
    0x50, 0x51, 0x52, 0x53,       /* push ax, cx, dx, bx */
    0x55, 0x56, 0x57, 0x06,       /* push bp, si, di, es */
    0x1e,                         /* push ds */
    0x31, 0xc0, 0x8e, 0xd8,       /* xor ax,ax; mov ds,ax */
    0xff, 0x06, 0x6c, 0x04,       /* inc word [046Ch] */
    0x75, 0x04,                   /* jnz tick_done */
    0xff, 0x06, 0x6e, 0x04,       /* inc word [046Eh] */
    0xcd, 0x1c,                   /* tick_done: int 1Ch */
    0x1f, 0x07, 0x5f, 0x5e,       /* pop ds, es, di, si */
    0x5d, 0x5b, 0x5a, 0x59,       /* pop bp, bx, dx, cx */
    0x58, 0xcf                    /* pop ax; iret */
  };
  memcpy(vm->Memory + 0xf1800u, bios_timer, sizeof(bios_timer));
  vm->Memory[0x08u * 4u + 1u] = 0x18u;
  vm->Memory[0x08u * 4u + 3u] = 0xf0u;
  /* Each BIOS/DOS entry needs its own stable address: extenders save IVT
   * pointers and call them later with DPMI 0301/0302. */
  static const U8 builtin_vectors[] = {
    0x09, 0x10, 0x11, 0x12, 0x15, 0x16, 0x1a, 0x20, 0x21, 0x28, 0x29, 0x2f, 0x31, 0x33
  };
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
  /* Install and initialize the resident DOS mouse interface before the
   * program starts. Windows 1.x checks the INT 33h vector while loading. */
  DosMouseInitialize(vm);
  int ok = load(image, (U32)size, p);
  if (ok) {
    for (unsigned vector = 0x22u; vector <= 0x24u; vector++) {
      U16 field = (U16)(0x0au + (vector - 0x22u) * 4u);
      ww(PSP, field, rw(0, (U16)(vector * 4u)));
      ww(PSP, (U16)(field + 2u), rw(0, (U16)(vector * 4u + 2u)));
    }
    /* Windows 3.1 standard mode locates DOSX.EXE through PATH.  The launcher
     * runs from C:\\WINDOWS3, while DOSX.EXE is installed in SYSTEM; keep the
     * drive root searchable too for ordinary DOS programs. */
    static const U8 prefix[] =
      "PATH=C:\\;C:\\WINDOWS3\\SYSTEM\0"
      "TEMP=C:\\WINDOWS3\0"
      "TMP=C:\\WINDOWS3\0\0\1";
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
    const char *cwd = KeGetCurrentThread()->Process
                          ? KeGetCurrentThread()->Process->CurrentDirectory
                          : "C:\\";
    DosInitializeSystemData(vm, cwd);
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
  ConRedraw();
  ExFreePool(vm->Memory);
  ExFreePool(vm);
}

U8 run_vm(DOS_VM *vm) {
  if (!vm) return 0;
  if (!vm->KeyboardMirrorActive) {
    KbdInputMirrorAcquire();
    vm->KeyboardMirrorActive = TRUE;
  }
  /* Keep the DOS context in a local pointer throughout the hot instruction
   * loop instead of looking it up again for every guest instruction. */
  while (vm->Running) {
    /* Execute a short batch inside Cpu.c. This removes the cross-module
     * run-loop call from every guest instruction without changing instruction
     * boundaries, virtual IRQ polling, or the kernel timer's preemption. */
    if (!DosCpuRunBatch(vm, DOS_CPU_BATCH_SIZE)) break;
    /* The timer IRQ enforces the normal kernel-thread quantum.  A periodic
     * cooperative yield is only a long-run safety valve for unusual systems
     * where the timer is masked. The batch size divides this interval. */
    if (!(vm->Steps & 0xfffffu)) {
      U32 trace_linear = (((U32)vm->Segments[CS] << 4) + (vm->ProtectedMode ? vm->Eip : vm->Ip)) & 0xfffffu;
      KeLog("[dos] progress steps=%08x pm=%u cs=%04x ip=%08x ax=%08x\n",
            vm->Steps, vm->ProtectedMode, vm->Segments[CS],
            vm->ProtectedMode ? vm->Eip : vm->Ip,
            vm->ProtectedMode ? vm->Registers32[0] : vm->Registers[0]);
      if (!vm->ProtectedMode) {
        KeLog("[dos] trace ds=%04x es=%04x ss:sp=%04x:%04x si=%04x di=%04x bytes=%02x %02x %02x %02x %02x %02x %02x %02x\n",
              vm->Segments[DS], vm->Segments[ES], vm->Segments[SS], vm->Registers[SP],
              vm->Registers[SI], vm->Registers[DI], vm->Memory[trace_linear],
              vm->Memory[(trace_linear + 1) & 0xfffffu], vm->Memory[(trace_linear + 2) & 0xfffffu],
              vm->Memory[(trace_linear + 3) & 0xfffffu], vm->Memory[(trace_linear + 4) & 0xfffffu],
              vm->Memory[(trace_linear + 5) & 0xfffffu], vm->Memory[(trace_linear + 6) & 0xfffffu],
              vm->Memory[(trace_linear + 7) & 0xfffffu]);
      }
      KeYield();
    }
  }
  return (U8)vm->ExitCode;
}

void DosRun(PVOID context) {
  DOS_VM *vm = (DOS_VM *)context;
  KeGetCurrentThread()->ExitStatus = (NTSTATUS)run_vm(vm);
  DosDestroyVm(vm);
  KeGetCurrentThread()->DosContext = NULL;
}
